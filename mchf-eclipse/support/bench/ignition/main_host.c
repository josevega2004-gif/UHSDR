/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**
 * @file main_host.c
 * @brief host test for the ignition bench box core (no hardware, no HAL)
 *
 * Runs the same ignition_core.c that goes onto the FK743 against a virtual microsecond
 * clock, a logging MUTE stub and a recording SI5351 I2C stub, then asserts:
 *   1. the SI5351 divider math reproduces the requested carrier within 6 Hz
 *   2. spark windows have the configured width, the configured rate within the configured
 *      jitter, never overlap, and are identical for the same seed
 *   3. the console parser moves the knobs and survives nonsense
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "ignition_core.h"
#include "si5351.h"
#include "si5351_port.h"

// ---- virtual platform ------------------------------------------------------

static uint64_t virt_now;
static IgnitionState_t st;

static uint32_t now_us(void) { return (uint32_t)virt_now; }

#define MAX_EDGES 200000
static uint64_t edge_time[MAX_EDGES];
static int edge_muted[MAX_EDGES];
static int edge_count;

static void mute_log(int muted)
{
    assert(edge_count < MAX_EDGES);
    edge_time[edge_count] = virt_now;
    edge_muted[edge_count] = muted;
    edge_count++;
}

#define MAX_I2C 4096
static struct { uint8_t reg, val; } i2c_log[MAX_I2C];
static int i2c_count;

void si5351_port_i2c_write(uint8_t address, uint8_t reg, uint8_t value)
{
    (void)address;
    assert(i2c_count < MAX_I2C);
    i2c_log[i2c_count].reg = reg;
    i2c_log[i2c_count].val = value;
    i2c_count++;
}

static void print_capture(const char* line) { printf("    box> %s\n", line); }

static const IgnitionHal_t hal = { now_us, mute_log, print_capture };

static void run_virtual_us(uint64_t us)
{
    for (uint64_t t = 0; t < us; t += 10)     // 10 us main loop, coarse but honest
    {
        virt_now += 10;
        Ignition_Poll(&st, &hal);
    }
}

// independent reconstruction of the driver's own contract (si5351.c header comments):
// f = fXtal * N / M / rdiv with N = mult + num/denom (24..36) and M = div + num/denom (8..1800)

static void test_carrier_math(void)
{
    si5351PLLConfig_t pll; si5351OutputConfig_t out;
    const int32_t freqs[] = { 7040000, 10000000, 14000000, 500000, 35000000 };
    printf("test 1: si5351_Calc round trip\n");
    for (unsigned i = 0; i < sizeof(freqs)/sizeof(freqs[0]); i++)
    {
        si5351_Calc(freqs[i], &pll, &out);
        const double n = pll.mult + pll.num / (double)pll.denom;
        const double m = out.div + out.num / (double)out.denom;
        const double f = 25000000.0 * n / m / (out.rdiv == SI5351_R_DIV_64 ? 64 : 1);
        const double err = fabs(f - (double)freqs[i]);
        printf("    %9d Hz -> reconstructed err %.3f Hz\n", freqs[i], err);
        assert(err <= 6.0);              // the driver promises <= 6 Hz, rdiv path included
    }
}

static void test_pattern(int events, int width, int jitter, uint32_t seed)
{
    char cmd[24];
    edge_count = 0;
    st.seed = seed; st.events_per_sec = events; st.width_us = width;
    st.jitter_pct = jitter; st.width_pct = 0; st.running = 0;
    st.next_event_us = 0; st.close_window_us = 0;

    snprintf(cmd, sizeof cmd, "seed%u", seed); Ignition_ConsoleLine(&st, &hal, cmd);
    Ignition_ConsoleLine(&st, &hal, "o");
    run_virtual_us(2000000);                    // 2 s of virtual time
    Ignition_ConsoleLine(&st, &hal, "x");

    // collect windows (unmute edges)
    int windows = 0; static uint64_t start[MAX_EDGES]; static uint32_t wid[MAX_EDGES];
    for (int i = 0; i < edge_count; i++)
    {
        if (edge_muted[i] == 0) { start[windows] = edge_time[i]; wid[windows] = 0; windows++; }
        else if (windows)       { wid[windows-1] = (uint32_t)(edge_time[i] - start[windows-1]); }
    }
    const int expect = events * 2;
    printf("test 2 (e%d w%d j%d): %d windows (expect ~%d)\n", events, width, jitter, windows, expect);
    assert(abs(windows - expect) <= expect * (jitter ? 0.30 : 0.10));
    for (int i = 1; i < windows; i++)
    {
        const uint64_t gap = start[i] - (start[i-1] + wid[i-1]);
        assert(wid[i-1] >= (uint32_t)width && wid[i-1] <= (uint32_t)width + 10u);   // loop step tolerance
        if (jitter == 0) { assert(gap >= (uint64_t)(1000000/events - width - 10) && gap <= (uint64_t)(1000000u/events - width + 10)); }
    }
}

static void test_determinism(void)
{
    static uint64_t a[1000], b[1000]; (void)b;
    edge_count = 0;
    st.seed = 777u; Ignition_ConsoleLine(&st, &hal, "seed777"); Ignition_ConsoleLine(&st, &hal, "o");
    run_virtual_us(1000000); Ignition_ConsoleLine(&st, &hal, "x");
    for (int i = 0; i < edge_count; i++) a[i] = edge_time[i] - edge_time[0];   // offsets from run start
    const int n = edge_count;
    edge_count = 0; st.seed = 777u; Ignition_ConsoleLine(&st, &hal, "seed777"); Ignition_ConsoleLine(&st, &hal, "o");
    run_virtual_us(1000000); Ignition_ConsoleLine(&st, &hal, "x");
    int same = (n == edge_count);
    for (int i = 0; same && i < n; i++) same = (a[i] == edge_time[i] - edge_time[0]);
    printf("test 3: seed reproducibility %s (%d edges)\n", same ? "OK" : "FAIL", n);
    assert(same);
}

static void test_console(void)
{
    Ignition_ConsoleLine(&st, &hal, "e100");  assert(st.events_per_sec == 100);
    Ignition_ConsoleLine(&st, &hal, "w8");    assert(st.width_us == 8);
    Ignition_ConsoleLine(&st, &hal, "zzz42"); // nonsense: state unchanged
    assert(st.events_per_sec == 100 && st.width_us == 8);
    Ignition_ConsoleLine(&st, &hal, "e5000"); // out of range: rejected
    assert(st.events_per_sec == 100);
    printf("test 4: console parser OK\n");
}

int main(void)
{
    Ignition_Init(&st, &hal, 7040000u);
    assert(i2c_count > 20);                       // carrier setup wrote real register traffic
    test_carrier_math();
    test_pattern(50, 5, 0, 1234u);
    test_pattern(25, 20, 20, 99u);
    test_determinism();
    Ignition_Init(&st, &hal, 7040000u);
    test_console();
    printf("all ignition bench tests passed\n");
    return 0;
}
