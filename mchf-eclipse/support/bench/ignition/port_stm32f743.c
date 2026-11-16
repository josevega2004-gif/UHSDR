/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**
 * @file port_stm32f743.c
 * @brief FK743M5 (STM32F743) platform layer for the ignition bench box
 *
 * Intended to be dropped into a CubeMX-generated project for the board with:
 *   - I2C1, 400 kHz (SDA/SCL at the FK743M5 user header)
 *   - one output GPIO named BENCH_MUTE (push-pull) wired to the SI5351 module MUTE pin
 *   - one UART with its RX interrupt enabled for the console (115200)
 *   - the DWT unit available (default; enabled at startup below)
 *
 * The spark window is 1 to 40 microseconds wide, so the main loop must never block:
 * console characters arrive through the UART interrupt into a ring buffer, everything
 * timing-critical happens in the tight polling loop.
 *
 * Set the macros below to the CubeMX-generated names where they differ. This file is
 * not built by the host test Makefile (no F7 CMSIS headers in this tree) - the host
 * test runs the identical core against instrumented stubs.
 */
#include <string.h>

#include "stm32f7xx_hal.h"

#include "ignition_core.h"
#include "si5351_port.h"

#ifndef BENCH_I2C_HANDLE
#define BENCH_I2C_HANDLE hi2c1
#endif
#ifndef BENCH_MUTE_PORT
#define BENCH_MUTE_PORT GPIOB
#endif
#ifndef BENCH_MUTE_PIN
#define BENCH_MUTE_PIN  GPIO_PIN_0
#endif
#ifndef BENCH_CONSOLE_UART
#define BENCH_CONSOLE_UART huart2
#endif

extern I2C_HandleTypeDef BENCH_I2C_HANDLE;
extern UART_HandleTypeDef BENCH_CONSOLE_UART;

void si5351_port_i2c_write(uint8_t address, uint8_t reg, uint8_t value)
{
    HAL_I2C_Mem_Write(&BENCH_I2C_HANDLE, (uint16_t)(address << 1), reg,
                      I2C_MEMADD_SIZE_8BIT, &value, 1, 100);
}

// microsecond clock from the DWT cycle counter, started once and never reset
static uint32_t bench_now_us(void)
{
    return (uint32_t)(DWT->CYCCNT / (SystemCoreClock / 1000000u));
}

static void bench_start_cycle_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void bench_mute(int muted)
{
    HAL_GPIO_WritePin(BENCH_MUTE_PORT, BENCH_MUTE_PIN, muted ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void bench_print(const char* line)
{
    HAL_UART_Transmit(&BENCH_CONSOLE_UART, (uint8_t*)line, (uint16_t)strlen(line), 100);
    HAL_UART_Transmit(&BENCH_CONSOLE_UART, (uint8_t*)"\r\n", 2, 10);
}

// console input ring, filled from the UART RX interrupt
#define CONSOLE_RING 256
static volatile uint8_t console_ring[CONSOLE_RING];
static volatile uint16_t console_head;
static volatile uint16_t console_tail;
static uint8_t console_rx_buffer;

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == BENCH_CONSOLE_UART.Instance)
    {
        console_ring[console_head] = console_rx_buffer;
        console_head = (uint16_t)((console_head + 1) % CONSOLE_RING);
        HAL_UART_Receive_IT(huart, &console_rx_buffer, 1);
    }
}

static int bench_getc(void)
{
    if (console_tail == console_head)
    {
        return -1;
    }
    const int c = console_ring[console_tail];
    console_tail = (uint16_t)((console_tail + 1) % CONSOLE_RING);
    return c;
}

static const IgnitionHal_t bench_hal = { bench_now_us, bench_mute, bench_print };

int main(void)
{
    static IgnitionState_t ign;
    char line[64];
    uint8_t len = 0;

    HAL_Init();
    SystemClock_Config();                 // from CubeMX
    MX_GPIO_Init();
    MX_I2C_Init();                        // from CubeMX
    MX_USART_Init();                      // from CubeMX, NVIC enabled for its RX interrupt
    bench_start_cycle_counter();

    Ignition_Init(&ign, &bench_hal, 7040000u);
    bench_print("ign box ready (e w j k seed f o x i)");

    HAL_UART_Receive_IT(&BENCH_CONSOLE_UART, &console_rx_buffer, 1);

    for (;;)
    {
        Ignition_Poll(&ign, &bench_hal);

        const int c = bench_getc();
        if (c < 0)
        {
            continue;
        }
        if (c == '\r' || c == '\n')
        {
            line[len] = '\0';
            if (len)
            {
                Ignition_ConsoleLine(&ign, &bench_hal, line);
            }
            len = 0;
        }
        else if (len < sizeof(line) - 1)
        {
            line[len++] = (char)c;
        }
    }
}
