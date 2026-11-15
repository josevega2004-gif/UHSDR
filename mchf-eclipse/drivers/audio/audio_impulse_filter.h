/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**********************************************************************************
 **                                                                              **
 **                              UHSDR                                           **
 **                a powerful firmware for STM32 based SDR transceivers          **
 **                                                                              **
 **--------------------------------------------------------------------------------
 **  Description: time domain impulse (static, crackle) suppression for the      **
 **               demodulated audio                                              **
 **  Licence:     GNU GPLv3                                                      **
 **********************************************************************************/

/**
 * @file audio_impulse_filter.h
 * @brief impulse (static, crackle) suppressor for the demodulated RX audio
 *
 * Impulse noise is the dominant disturbance on AM. A plain median filter does remove it, but it
 * is not transparent at 12 kHz sample rate: measurements show that it already distorts tones
 * around 1 kHz and destroys everything above 2 kHz. Level and median-deviation based detectors
 * fail as well, either because high frequency audio looks as spiky as a weak impulse, or
 * because a repeated noise raises the threshold until nothing is detected any more.
 *
 * This filter uses a criterion that needs no level tracking at all: a sample is an impulse if
 * it is much larger than the largest of its older neighbours inside the window. Wanted audio is
 * smooth, so each of its samples is at most as large as the peak within a few samples of it;
 * an impulse, being a disturbance of a fraction of a millisecond, is not. Because the
 * comparison is purely local it cannot be "trained away" by the noise, and clean audio passes
 * bit exact.
 *
 * A detected impulse (and the samples directly following it) is replaced by the median of its
 * window, i.e. reconstructed from its neighbours, so no mute "holes" that would be audible as
 * clicks are left behind - unlike a blanker, which zeroes the disturbed samples. Because the
 * median needs clean neighbours, a window of 5 samples removes impulses of up to 2 samples and
 * a window of 7 samples impulses of up to 3 samples (125 us to 250 us at 12 kHz).
 *
 * The window is centred, delaying the audio by (taps - 1) / 2 samples: 1 or 2 samples at
 * 12 kHz.
 *
 * A second detector runs in parallel on the energy of the last four samples: an ignition burst
 * stores far more energy than its individual samples stand out against their direct neighbours
 * (behind the AM filter a burst is a ringing oscillation only some times larger than loud
 * modulation on the very samples the neighbour test sees), so whenever the window energy rises
 * clearly above the median of the recent window energies the centre is treated as an impulse as
 * well and is repaired by the same median reconstruction. A median is not trained upwards by
 * repeated noise the way a tracked maximum is, and at the firing rates of a car engine even a
 * dense burst sequence leaves most of the history untouched.
 *
 * The code is header only (static inline) so that the very same code can be compiled and tested
 * outside of the firmware.
 */
#ifndef AUDIO_IMPULSE_FILTER_H
#define AUDIO_IMPULSE_FILTER_H

#include <stdint.h>
#include <math.h>

#define AUDIO_IMPULSE_MAX_TAPS      7
#define AUDIO_IMPULSE_MIN_LEVEL     1e-6f   // below this there is nothing to detect
#define AUDIO_IMPULSE_ENERGY_TAPS   4       // samples in the energy window
#define AUDIO_IMPULSE_ENERGY_HIST   16      // recent energy windows kept for the median

typedef struct
{
    float   history[AUDIO_IMPULSE_MAX_TAPS];    // ring buffer with the newest samples
    uint8_t position;                           // next write position in history
    uint8_t fill;                               // valid samples in history, saturates at AUDIO_IMPULSE_MAX_TAPS
    uint8_t replacing;                          // samples still to be replaced by the median
    float   square[AUDIO_IMPULSE_ENERGY_TAPS];  // squares of the last ENERGY_TAPS input samples
    uint8_t square_pos;                         // next write position in square
    float   energy_hist[AUDIO_IMPULSE_ENERGY_HIST]; // recent window energies, updated every ENERGY_TAPS samples
    uint8_t energy_pos;                         // next write position in energy_hist
    uint8_t energy_fill;                        // valid entries in energy_hist, saturates
    float   energy_ref;                         // median of the valid energy_hist entries
} AudioImpulseState_t;

/**
 * @brief clear the filter history, e.g. when the impulse suppressor is switched off
 * @param state filter state
 */
static inline void AudioImpulse_Reset(AudioImpulseState_t *state)
{
    for (uint8_t i = 0; i < AUDIO_IMPULSE_MAX_TAPS; i++)
    {
        state->history[i] = 0.0f;
    }
    state->position = 0;
    state->fill = 0;
    state->replacing = 0;
    for (uint8_t i = 0; i < AUDIO_IMPULSE_ENERGY_TAPS; i++)
    {
        state->square[i] = 0.0f;
    }
    state->square_pos = 0;
    for (uint8_t i = 0; i < AUDIO_IMPULSE_ENERGY_HIST; i++)
    {
        state->energy_hist[i] = 0.0f;
    }
    state->energy_pos = 0;
    state->energy_fill = 0;
    state->energy_ref = 0.0f;
}

/**
 * @brief delay of the impulse suppressor in samples
 * @param taps window length
 * @return (taps - 1) / 2 samples
 */
static inline uint8_t AudioImpulse_Delay(uint8_t taps)
{
    return (uint8_t)((taps - 1) / 2);
}

/**
 * @brief remove impulses from one audio sample stream
 * @param state filter state
 * @param sample new input sample
 * @param taps window length, 5 or AUDIO_IMPULSE_MAX_TAPS (odd values in between work as well)
 * @param threshold_factor a sample is an impulse if it is this many times larger than the
 *        largest of its older neighbours, 3 (aggressive) to 4 (mild)
 * @return the sample AudioImpulse_Delay() samples ago, with impulses replaced by the median
 */
static inline float AudioImpulse_Process(AudioImpulseState_t *state, float sample, uint8_t taps, float threshold_factor)
{
    if (taps < 5)
    {
        taps = 5;                                       // three taps cannot tell audio from impulses
    }
    if (taps > AUDIO_IMPULSE_MAX_TAPS)
    {
        taps = AUDIO_IMPULSE_MAX_TAPS;
    }
    taps |= 1;                                          // a median needs an odd window length

    state->history[state->position] = sample;
    state->position = (uint8_t)((state->position + 1) % AUDIO_IMPULSE_MAX_TAPS);
    if (state->fill < AUDIO_IMPULSE_MAX_TAPS)
    {
        state->fill++;
    }

    if (state->fill < taps)
    {
        return 0.0f;                                    // filling the delay line, output starts later
    }

    const uint8_t delay = AudioImpulse_Delay(taps);
    const uint8_t centre = (uint8_t)((state->position + AUDIO_IMPULSE_MAX_TAPS - 1 - delay) % AUDIO_IMPULSE_MAX_TAPS);

    // the sample output by this call and the largest of the older neighbours in the window.
    // Only the past is used, so a run of impulse samples is peeled off one sample at a time:
    // once a sample has been repaired it is written back, and it then serves as the reference
    // that lets the detector find the next sample of the same impulse.
    const float current = state->history[centre];
    float neighbours = AUDIO_IMPULSE_MIN_LEVEL;
    float values[AUDIO_IMPULSE_MAX_TAPS];
    uint8_t idx = state->position;
    for (uint8_t i = 0; i < taps; i++)
    {
        idx = (uint8_t)((idx + AUDIO_IMPULSE_MAX_TAPS - 1) % AUDIO_IMPULSE_MAX_TAPS);
        values[i] = state->history[idx];
    }
    for (uint8_t i = 1; i <= delay; i++)
    {
        const float neighbour = fabsf(state->history[(uint8_t)((centre + AUDIO_IMPULSE_MAX_TAPS - i) % AUDIO_IMPULSE_MAX_TAPS)]);
        if (neighbour > neighbours)
        {
            neighbours = neighbour;
        }
    }

    // insertion sort, at most AUDIO_IMPULSE_MAX_TAPS entries
    for (uint8_t i = 1; i < taps; i++)
    {
        const float key = values[i];
        int8_t j = (int8_t)(i - 1);
        while ((j >= 0) && (values[j] > key))
        {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
    const float median = values[taps / 2];

    // the parallel energy detector: the square of the new input sample enters the window
    // energy; every fourth sample the recent energies are re-sorted and their median becomes the
    // reference. The current window energy is compared against that reference every sample, so a
    // ringing burst whose individual samples only loosely stand out still keeps the flag up for
    // all of its samples.
    state->square[state->square_pos] = sample * sample;
    state->square_pos = (uint8_t)((state->square_pos + 1) % AUDIO_IMPULSE_ENERGY_TAPS);
    float energy = 0.0f;
    for (uint8_t i = 0; i < AUDIO_IMPULSE_ENERGY_TAPS; i++)
    {
        energy += state->square[i];
    }
    if ((state->position % AUDIO_IMPULSE_ENERGY_TAPS) == 0)
    {
        state->energy_hist[state->energy_pos] = energy;
        state->energy_pos = (uint8_t)((state->energy_pos + 1) % AUDIO_IMPULSE_ENERGY_HIST);
        if (state->energy_fill < AUDIO_IMPULSE_ENERGY_HIST)
        {
            state->energy_fill++;
        }
        float med[AUDIO_IMPULSE_ENERGY_HIST];
        for (uint8_t i = 0; i < state->energy_fill; i++)
        {
            const float key = state->energy_hist[i];
            int8_t j = (int8_t)(i - 1);
            med[i] = key;
            while ((j >= 0) && (med[j] > key))
            {
                med[j + 1] = med[j];
                j--;
            }
        }
        state->energy_ref = med[state->energy_fill / 2];
    }
    const uint8_t energy_impulse = (state->energy_fill >= AUDIO_IMPULSE_ENERGY_HIST / 2)
                                && (energy > threshold_factor * state->energy_ref);

    const uint8_t impulse = (fabsf(current) > (threshold_factor * neighbours)) || energy_impulse;
    if (impulse)
    {
        state->replacing = (taps > 5) ? 3 : 1;          // impulses of more than one sample
    }

    float retval = current;
    if (impulse || (state->replacing > 0))
    {
        retval = median;
        state->history[centre] = median;                // repaired, so it can serve as reference
        if (!impulse)
        {
            state->replacing--;
        }
    }

    return retval;
}

#endif
