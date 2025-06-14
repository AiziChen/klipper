// SPI functions on STM32H7
//
// Copyright (C) 2019-2025  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/io.h" // readb, writeb
#include "command.h" // shutdown
#include "gpio.h" // spi_setup
#include "internal.h" // gpio_peripheral
#include "sched.h" // sched_shutdown
#include "board/misc.h" // timer_is_before

#define MAX_FIFO 8 // Limit TX FIFO usage so RX FIFO doesn't overrun

struct spi_info {
    SPI_TypeDef *spi;
    uint8_t miso_pin, mosi_pin, sck_pin, function;
};

DECL_ENUMERATION("spi_bus", "spi2", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi2", "PB14,PB15,PB13");

DECL_ENUMERATION("spi_bus", "spi1", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi1", "PA6,PA7,PA5");
DECL_ENUMERATION("spi_bus", "spi1a", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi1a", "PB4,PB5,PB3");

#if !CONFIG_MACH_STM32F1
DECL_ENUMERATION("spi_bus", "spi2a", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi2a", "PC2,PC3,PB10");
#endif

#ifdef SPI3
DECL_ENUMERATION("spi_bus", "spi3a", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi3a", "PC11,PC12,PC10");
#endif

#ifdef SPI4
DECL_ENUMERATION("spi_bus", "spi4", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi4", "PE13,PE14,PE12");
#endif

#ifdef GPIOI
DECL_ENUMERATION("spi_bus", "spi2b", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi2b", "PI2,PI3,PI1");
#endif

#ifdef SPI5
DECL_ENUMERATION("spi_bus", "spi5", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi5", "PF8,PF9,PF7");
DECL_ENUMERATION("spi_bus", "spi5a", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi5a", "PH7,PF11,PH6");
#endif

#ifdef SPI6
DECL_ENUMERATION("spi_bus", "spi6", __COUNTER__);
DECL_CONSTANT_STR("BUS_PINS_spi6", "PG12,PG14,PG13");
#endif


static const struct spi_info spi_bus[] = {
    { SPI2, GPIO('B', 14), GPIO('B', 15), GPIO('B', 13), GPIO_FUNCTION(5) },
    { SPI1, GPIO('A', 6), GPIO('A', 7), GPIO('A', 5), GPIO_FUNCTION(5) },
    { SPI1, GPIO('B', 4), GPIO('B', 5), GPIO('B', 3), GPIO_FUNCTION(5) },
#if !CONFIG_MACH_STM32F1
    { SPI2, GPIO('C', 2), GPIO('C', 3), GPIO('B', 10), GPIO_FUNCTION(5) },
#endif
#ifdef SPI3
    { SPI3, GPIO('C', 11), GPIO('C', 12), GPIO('C', 10), GPIO_FUNCTION(6) },
#endif
#ifdef SPI4
    { SPI4, GPIO('E', 13), GPIO('E', 14), GPIO('E', 12), GPIO_FUNCTION(5) },
#endif
    { SPI2, GPIO('I', 2), GPIO('I', 3), GPIO('I', 1), GPIO_FUNCTION(5) },
#ifdef SPI5
    { SPI5, GPIO('F', 8), GPIO('F', 9), GPIO('F', 7), GPIO_FUNCTION(5) },
    { SPI5, GPIO('H', 7), GPIO('F', 11), GPIO('H', 6), GPIO_FUNCTION(5) },
#endif
#ifdef SPI6
    { SPI6, GPIO('G', 12), GPIO('G', 14), GPIO('G', 13), GPIO_FUNCTION(5)},
#endif
};

struct spi_config
spi_setup(uint32_t bus, uint8_t mode, uint32_t rate)
{
    if (bus >= ARRAY_SIZE(spi_bus))
        shutdown("Invalid spi bus");

    // Enable SPI
    SPI_TypeDef *spi = spi_bus[bus].spi;
    if (!is_enabled_pclock((uint32_t)spi)) {
        enable_pclock((uint32_t)spi);
        gpio_peripheral(spi_bus[bus].miso_pin, spi_bus[bus].function, 1);
        gpio_peripheral(spi_bus[bus].mosi_pin, spi_bus[bus].function, 0);
        gpio_peripheral(spi_bus[bus].sck_pin, spi_bus[bus].function, 0);
    }

    // Calculate CR1 register
    uint32_t pclk = get_pclock_frequency((uint32_t)spi);
    uint32_t div = 0;
    while ((pclk >> (div + 1)) > rate && div < 7)
        div++;

    return (struct spi_config){ .spi = spi, .div = div, .mode = mode };
}

void
spi_prepare(struct spi_config config)
{
    uint32_t div = config.div;
    uint32_t mode = config.mode;
    SPI_TypeDef *spi = config.spi;

    // Load frequency
    spi->CFG1 = (div << SPI_CFG1_MBR_Pos) | (7 << SPI_CFG1_DSIZE_Pos);
    // Load mode
    uint32_t cfg2 = ((mode << SPI_CFG2_CPHA_Pos) | SPI_CFG2_MASTER
                     | SPI_CFG2_SSM | SPI_CFG2_AFCNTR | SPI_CFG2_SSOE);
    uint32_t diff = spi->CFG2 ^ cfg2;
    spi->CFG2 = cfg2;
    uint32_t end = timer_read_time() + timer_from_us(1);
    if (diff & SPI_CFG2_CPOL_Msk)
        while (timer_is_before(timer_read_time(), end))
            ;
}

void
spi_transfer(struct spi_config config, uint8_t receive_data,
             uint8_t len, uint8_t *data)
{
    uint8_t *wptr = data, *end = data + len;
    SPI_TypeDef *spi = config.spi;

    spi->CR2 = len << SPI_CR2_TSIZE_Pos;
    // Enable SPI and start transfer, these MUST be set in this sequence
    spi->CR1 = SPI_CR1_SSI | SPI_CR1_SPE;
    spi->CR1 = SPI_CR1_SSI | SPI_CR1_CSTART | SPI_CR1_SPE;

    while (data < end) {
        uint32_t sr = spi->SR & (SPI_SR_TXP | SPI_SR_RXP);
        if (sr == SPI_SR_TXP && wptr < end && wptr < data + MAX_FIFO)
            writeb((void*)&spi->TXDR, *wptr++);
        if (!(sr & SPI_SR_RXP))
            continue;
        uint8_t rdata = readb((void *)&spi->RXDR);
        if (receive_data)
            *data = rdata;
        data++;
    }

    while ((spi->SR & SPI_SR_EOT) == 0)
        ;

    // Clear flags and disable SPI
    spi->IFCR = 0xFFFFFFFF;
    spi->CR1 = SPI_CR1_SSI;
}
