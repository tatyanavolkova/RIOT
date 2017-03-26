/*
 * Copyright (C) 2014-2016 Freie Universität Berlin
 * Copyright (C) 2016 OTA keys
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_stm32f2
 * @{
 *
 * @file
 * @brief       Low-level UART driver implementation
 *
 * @author      Hauke Petersen <hauke.petersen@fu-berlin.de>
 * @author      Thomas Eichinger <thomas.eichinger@fu-berlin.de>
 * @author      Fabian Nack <nack@inf.fu-berlin.de>
 * @author      Hermann Lelong <hermann@otakeys.com>
 * @author      Toon Stegen <toon.stegen@altran.com>
 *
 * @}
 */

#include "cpu.h"
#include "sched.h"
#include "thread.h"
#include "assert.h"
#include "periph/uart.h"
#include "periph/gpio.h"

#ifdef UART_NUMOF

#define RXENABLE            (USART_CR1_RE | USART_CR1_RXNEIE)

/**
 * @brief   Allocate memory to store the callback functions
 */
static uart_isr_ctx_t isr_ctx[UART_NUMOF];

static inline USART_TypeDef *dev(uart_t uart)
{
    return uart_config[uart].dev;
}

int uart_set_baudrate(uart_t uart, uint32_t baudrate) {
    uint16_t mantissa;
    uint8_t fraction;
    uint32_t uart_clk;
    
    uart_clk = periph_apb_clk(uart_config[uart].bus)/baudrate;

    if (uart_clk < 8) {
        /* clock is too slow for using UART with specified baudrate */
        periph_clk_dis(uart_config[uart].bus, uart_config[uart].rcc_mask);
        return UART_NOBAUD;
    } else {       
        /* Disable UART. Setting BRR on enabled USART1 sometimes somehow results in Hard Fault */
        dev(uart)->CR1 &= ~USART_CR1_UE;
        
        /* choose between 8x and 16x oversampling */
        /* 16x is preferred, but is not possible on low clock frequency */
        if (uart_clk < 16) {
            dev(uart)->CR1 |= USART_CR1_OVER8;
            mantissa = (uint16_t)(uart_clk / 8);
            fraction = (uint8_t)(uart_clk - (mantissa * 8));
            dev(uart)->BRR = ((mantissa & 0x0fff) << 4) | (fraction & 0x07);
        } else {
            dev(uart)->CR1 &= ~USART_CR1_OVER8;
            mantissa = (uint16_t)(uart_clk / 16);
            fraction = (uint8_t)(uart_clk - (mantissa * 16));
            dev(uart)->BRR = ((mantissa & 0x0fff) << 4) | (fraction & 0x0f);
        }

        /* Enable UART */
        dev(uart)->CR1 |= USART_CR1_UE;
        
        return UART_OK;
    }
}

int uart_init_ext(uart_t uart, uart_params_t *params, uart_rx_cb_t rx_cb, void *arg)
{
    assert(uart < UART_NUMOF);

    /* save ISR context */
    isr_ctx[uart].rx_cb = rx_cb;
    isr_ctx[uart].arg   = arg;

    /* configure RX and TX pin */
    gpio_init(uart_config[uart].rx_pin, GPIO_IN);
    gpio_init(uart_config[uart].tx_pin, GPIO_OUT);
    /* set TX pin high to avoid garbage during further initialization */
    gpio_set(uart_config[uart].tx_pin);
#ifdef CPU_FAM_STM32F1
    gpio_init_af(uart_config[uart].tx_pin, GPIO_AF_OUT_PP);
#else
    gpio_init_af(uart_config[uart].tx_pin, uart_config[uart].tx_af);
    gpio_init_af(uart_config[uart].rx_pin, uart_config[uart].rx_af);
#endif

    /* enable the clock */
    periph_clk_en(uart_config[uart].bus, uart_config[uart].rcc_mask);

    /* reset UART configuration -> defaults to 8N1 mode */
    dev(uart)->CR1 = 0;
    dev(uart)->CR2 = 0;
    dev(uart)->CR3 = 0;
    
    if (params->databits == UART_DATABITS_9) {
        dev(uart)->CR1 |= USART_CR1_M;
    }
    
    switch (params->parity) {
        case UART_PARITY_ODD:
            dev(uart)->CR1 |= USART_CR1_PCE;
            dev(uart)->CR1 |= USART_CR1_PS;
            break;
        case UART_PARITY_EVEN:
            dev(uart)->CR1 |= USART_CR1_PCE;
            break;
        default:
            break;
    }
    
    switch (params->stopbits) {
        case UART_STOPBITS_05:
            dev(uart)->CR2 |= USART_CR2_STOP_0;
            break;
        case UART_STOPBITS_15:
            dev(uart)->CR2 |= (USART_CR2_STOP_0 | USART_CR2_STOP_1);
            break;
        case UART_STOPBITS_20:
            dev(uart)->CR2 |= USART_CR2_STOP_1;
            break;
        default:
            break;
    }
    
    /* calculate and apply baudrate */
    if (uart_set_baudrate(uart, params->baudrate) != UART_OK) {
        return UART_NOBAUD;
    }

    /* enable RX interrupt if applicable */
    if (rx_cb) {
        NVIC_EnableIRQ(uart_config[uart].irqn);
        dev(uart)->CR1 = (USART_CR1_UE | USART_CR1_TE | RXENABLE);
    }
    else {
        dev(uart)->CR1 = (USART_CR1_UE | USART_CR1_TE);
    }

    return UART_OK;
}

int uart_init(uart_t uart, uint32_t baudrate, uart_rx_cb_t rx_cb, void *arg) {
    uart_params_t params;
    params.baudrate = baudrate;
    params.databits = UART_DATABITS_8;
    params.parity = UART_PARITY_NOPARITY;
    params.stopbits = UART_STOPBITS_10;
    
    return uart_init_ext(uart, &params, rx_cb, arg);
}

void uart_write(uart_t uart, const uint8_t *data, size_t len)
{
    assert(uart < UART_NUMOF);

    for (size_t i = 0; i < len; i++) {
#if defined(CPU_FAM_STM32F0) || defined(CPU_FAM_STM32F3)
        while (!(dev(uart)->ISR & USART_ISR_TXE)) {}
        dev(uart)->TDR = data[i];
#else
        while (!(dev(uart)->SR & USART_SR_TXE)) {}
        dev(uart)->DR = data[i];
#endif
    }

    /* make sure the function is synchronous by waiting for the transfer to
     * finish */
#if defined(CPU_FAM_STM32F0) || defined(CPU_FAM_STM32F3)
    while (!(dev(uart)->ISR & USART_ISR_TC)) {}
#else
    while (!(dev(uart)->SR & USART_SR_TC)) {}
#endif
}

void uart_poweron(uart_t uart)
{
    assert(uart < UART_NUMOF);
    periph_clk_en(uart_config[uart].bus, uart_config[uart].rcc_mask);
}

void uart_poweroff(uart_t uart)
{
    assert(uart < UART_NUMOF);
    periph_clk_en(uart_config[uart].bus, uart_config[uart].rcc_mask);
}

static inline void irq_handler(uart_t uart)
{
#if defined(CPU_FAM_STM32F0) || defined(CPU_FAM_STM32F3)

    uint32_t status = dev(uart)->ISR;

    if (status & USART_ISR_RXNE) {
        isr_ctx[uart].rx_cb(isr_ctx[uart].arg, (uint8_t)dev(uart)->RDR);
    }
    if (status & USART_ISR_ORE) {
        dev(uart)->ICR |= USART_ICR_ORECF;    /* simply clear flag on overrun */
    }

#else

    uint32_t status = dev(uart)->SR;

    if (status & USART_SR_RXNE) {
        isr_ctx[uart].rx_cb(isr_ctx[uart].arg, (uint8_t)dev(uart)->DR);
    }
    if (status & USART_SR_ORE) {
        /* ORE is cleared by reading SR and DR sequentially */
        dev(uart)->DR;
    }

#endif

    cortexm_isr_end();
}

#ifdef UART_0_ISR
void UART_0_ISR(void)
{
    irq_handler(UART_DEV(0));
}
#endif

#ifdef UART_1_ISR
void UART_1_ISR(void)
{
    irq_handler(UART_DEV(1));
}
#endif

#ifdef UART_2_ISR
void UART_2_ISR(void)
{
    irq_handler(UART_DEV(2));
}
#endif

#ifdef UART_3_ISR
void UART_3_ISR(void)
{
    irq_handler(UART_DEV(3));
}
#endif

#ifdef UART_4_ISR
void UART_4_ISR(void)
{
    irq_handler(UART_DEV(4));
}
#endif

#ifdef UART_5_ISR
void UART_5_ISR(void)
{
    irq_handler(UART_DEV(5));
}
#endif

#endif /* UART_NUMOF */
