/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**
 * @file ignition_core.h
 * @brief portable ignition-noise simulator used by the SI5351 bench box
 *
 * The box holds a 7 MHz carrier output muted (SI5351 MUTE pin / disable state). A spark is a
 * short window of un-muting: the carrier edges at the start and end of the window are what the
 * receiver hears as an impulse. The core schedules those windows with period, width, jitter and
 * per-cycle pulse count knobs, so the noise looks like a 4-stroke engine instead of a metronome
 * and, most importantly, every run can be made byte-identical to the previous one (seed).
 *
 * Everything here is plain C with three callbacks, which makes it unit-testable on the host
 * (main_host.c) and tiny enough to run on any of the dev boards.
 */
#ifndef IGNITION_CORE_H
#define IGNITION_CORE_H

#include <stdint.h>

typedef struct
{
    uint32_t (*now_us)(void);                      // monotonic microsecond clock
    void     (*mute)(int muted);                   // 1 = outputs muted, 0 = spark window open
    void     (*print)(const char* line);           // console output
} IgnitionHal_t;

typedef struct
{
    uint32_t events_per_sec;      // 25 / 50 / 100 ... (cylinders * rpm / 60 / 2)
    uint32_t width_us;            // spark window, 1..40 us
    uint32_t jitter_pct;          // period jitter 0..50 %
    uint32_t width_pct;           // width jitter 0..50 %
    uint8_t  running;
    uint32_t seed;                // deterministic pseudo noise source
    // schedule state
    uint32_t next_event_us;
    uint32_t close_window_us;
} IgnitionState_t;

void Ignition_Init(IgnitionState_t* st, const IgnitionHal_t* hal, uint32_t carrier_hz);
void Ignition_Poll(IgnitionState_t* st, const IgnitionHal_t* hal);       // call as often as you like
void Ignition_ConsoleLine(IgnitionState_t* st, const IgnitionHal_t* hal, const char* line);

#endif
