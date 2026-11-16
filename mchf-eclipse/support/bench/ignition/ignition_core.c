/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**
 * @file ignition_core.c
 * @brief portable part of the SI5351 ignition simulator bench box
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ignition_core.h"
#include "si5351.h"

// small deterministic PRNG (xorshift32), seeded by the console so test runs are repeatable
static uint32_t ign_rand(uint32_t* seed)
{
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

static uint32_t ign_period_us(const IgnitionState_t* st)
{
    return 1000000u / st->events_per_sec;
}

// interval to the next spark: nominal period, scaled by +/- jitter_pct
static uint32_t ign_next_interval_us(IgnitionState_t* st)
{
    const uint32_t period = ign_period_us(st);
    if (st->jitter_pct == 0)
    {
        return period;
    }
    const int32_t jitter = (int32_t)(period * st->jitter_pct / 100u);
    const int32_t span = 2 * jitter + 1;
    const int32_t off = (int32_t)(ign_rand(&st->seed) % (uint32_t)span) - jitter;
    return (uint32_t)((int32_t)period + off);
}

static uint32_t ign_next_width_us(IgnitionState_t* st)
{
    if (st->width_pct == 0)
    {
        return st->width_us;
    }
    const int32_t jitter = (int32_t)(st->width_us * st->width_pct / 100u);
    const int32_t span = 2 * jitter + 1;
    const int32_t off = (int32_t)(ign_rand(&st->seed) % (uint32_t)span) - jitter;
    uint32_t w = (uint32_t)((int32_t)st->width_us + off);
    if (w < 1u)
    {
        w = 1u;
    }
    return w;
}

void Ignition_Init(IgnitionState_t* st, const IgnitionHal_t* hal, uint32_t carrier_hz)
{
    memset(st, 0, sizeof(*st));
    st->events_per_sec = 50;
    st->width_us = 5;
    st->jitter_pct = 20;
    st->width_pct = 0;
    st->seed = 1234u;

    // default module crystal is 25 MHz; correction 0 until the bench measures it against the rig
    si5351_Init(0);
    si5351_SetupCLK0((int32_t)carrier_hz, SI5351_DRIVE_STRENGTH_8MA);
    si5351_EnableOutputs(1u << 0);

    hal->mute(1);                                  // stay quiet until "o"
}

void Ignition_Poll(IgnitionState_t* st, const IgnitionHal_t* hal)
{
    const uint32_t now = hal->now_us();

    if (st->running && now >= st->close_window_us && st->close_window_us != 0)
    {
        hal->mute(1);
        st->close_window_us = 0;
    }
    if (st->running && now >= st->next_event_us)
    {
        hal->mute(0);                              // spark: open the window, the edges do the rest
        st->close_window_us = now + ign_next_width_us(st);
        st->next_event_us = now + ign_next_interval_us(st);
    }
}

static void ign_status(IgnitionState_t* st, const IgnitionHal_t* hal)
{
    char line[96];
    snprintf(line, sizeof(line), "ign: %s events=%u/s width=%uus jitter=%u%% wjit=%u%% seed=%u",
             st->running ? "RUN" : "idle",
             (unsigned)st->events_per_sec, (unsigned)st->width_us,
             (unsigned)st->jitter_pct, (unsigned)st->width_pct, (unsigned)st->seed);
    hal->print(line);
}

void Ignition_ConsoleLine(IgnitionState_t* st, const IgnitionHal_t* hal, const char* line)
{
    char cmd[16];
    unsigned value;
    char out[64];

    if (sscanf(line, "%15[a-zA-Z]%u", cmd, &value) == 2)
    {
        const char* c = cmd;
        if (strcmp(c, "e") == 0 && value >= 1 && value <= 1000) { st->events_per_sec = value; }
        else if (strcmp(c, "w") == 0 && value >= 1 && value <= 40) { st->width_us = value; }
        else if (strcmp(c, "j") == 0 && value <= 50) { st->jitter_pct = value; }
        else if (strcmp(c, "k") == 0 && value <= 50) { st->width_pct = value; }
        else if (strcmp(c, "seed") == 0) { st->seed = value ? value : 1u; }
        else if (strcmp(c, "f") == 0 && value >= 100000 && value <= 81000000u)
        {
            si5351_SetupCLK0((int32_t)value, SI5351_DRIVE_STRENGTH_8MA);
            si5351_EnableOutputs(1u << 0);
            hal->mute(!st->running);
            snprintf(out, sizeof(out), "carrier set to %u Hz", value);
            hal->print(out);
        }
        else
        {
            hal->print("ign: unknown command (e w j k seed f o x i)");
            return;
        }
        hal->print("ign: ok");
        return;
    }

    if (line[0] == 'o')
    {
        if (!st->running)
        {
            st->next_event_us = hal->now_us() + 100u;
            st->close_window_us = 0;
        }
        st->running = 1;
        hal->print("ign: running");
    }
    else if (line[0] == 'x')
    {
        st->running = 0;
        hal->mute(1);
        hal->print("ign: stopped");
    }
    else if (line[0] == 'i')
    {
        ign_status(st, hal);
    }
    else if (line[0] != '\r' && line[0] != '\n' && line[0] != '\0')
    {
        hal->print("ign: unknown command (e w j k seed f o x i)");
    }
}
