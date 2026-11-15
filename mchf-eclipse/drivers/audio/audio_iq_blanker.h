/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*- */
/**********************************************************************************
 **                                                                              **
 **                              UHSDR                                           **
 **                a powerful firmware for STM32 based SDR transceivers          **
 **                                                                              **
 **--------------------------------------------------------------------------------
 **  Description: RF side (I/Q) noise blanker for ignition and other impulse     **
 **               noise, running before the receive filters                      **
 **  Licence:     GNU GPLv3                                                      **
 **********************************************************************************/

/**
 * @file audio_iq_blanker.h
 * @brief blank ignition impulses on the I/Q stream before the digital receive filters
 *
 * The receiver is a direct conversion design: behind the mixer every ignition pulse enters the
 * DSP as a spike of one to three samples at the 48 kHz I/Q rate. Only the narrow digital
 * filters further down the chain smear it into the ringing burst of four to eight samples at
 * 12 kHz that the audio suppressor in audio_impulse_filter.h has to fight. Fixing the spike
 * where it still is a spike is far easier: at this point the wanted signal has the full I/Q
 * bandwidth, so its amplitude can only grow slowly from sample to sample, and the pulse stands
 * out clearly against its direct neighbours.
 *
 * A sample (I and Q together, they carry the same impulse) is replaced when either
 *  - its amplitude is spike_threshold times the largest amplitude of the samples in front of
 *    it, or
 *  - its amplitude rises above energy_threshold times the median of the amplitudes of the
 *    recent history. The median cannot be trained upwards by repeated engine noise the way a
 *    tracked maximum or average can, and it keeps working on a pulse that lands right on top
 *    of a modulation peak, where the neighbour test goes blind.
 *
 * The replacement is the mean of the two older and the two newer samples, so no mute hole is
 * left that the receive filters would turn into a click, and the repair needs only the small
 * delay of two samples (42 us at 48 kHz). Repaired samples are written back into the history,
 * so the second and third sample of a wider pulse are caught one at a time against the
 * already repaired ones.
 *
 * The code is header only (static inline) so that the very same code can be compiled and
 * tested outside of the firmware.
 */
#ifndef AUDIO_IQ_BLANKER_H
#define AUDIO_IQ_BLANKER_H

#include <stdint.h>
#include <math.h>

#define AUDIO_IQ_BLANKER_WINDOW       5       // centred repair window
#define AUDIO_IQ_BLANKER_DELAY        2       // (AUDIO_IQ_BLANKER_WINDOW - 1) / 2 samples of delay
#define AUDIO_IQ_BLANKER_AMP_HIST     32      // recent amplitudes for the median
#define AUDIO_IQ_BLANKER_AMP_DIV      8       // a new amplitude enters the history every ... samples
#define AUDIO_IQ_BLANKER_MIN_AMP      1e-5f   // below this there is nothing to detect

typedef struct
{
    float   i_history[AUDIO_IQ_BLANKER_WINDOW];     // ring buffers with the newest input samples
    float   q_history[AUDIO_IQ_BLANKER_WINDOW];
    uint8_t position;                               // next write position in the history rings
    uint8_t fill;                                   // valid samples in history, saturates at AUDIO_IQ_BLANKER_WINDOW
    float   amp_hist[AUDIO_IQ_BLANKER_AMP_HIST];    // ring with the sampled amplitudes
    uint8_t amp_pos;                                // next write position in amp_hist
    uint8_t amp_fill;                               // valid entries in amp_hist, saturates at AUDIO_IQ_BLANKER_AMP_HIST
    uint8_t amp_div;                                // samples since the last amp_hist update
    float   amp_ref;                                // median of the valid amp_hist entries
} AudioIQBlankerState_t;

/**
 * @brief clear the blanker history, e.g. when it is switched off
 * @param state blanker state
 */
static inline void AudioIQBlanker_Reset(AudioIQBlankerState_t *state)
{
    for (uint8_t i = 0; i < AUDIO_IQ_BLANKER_WINDOW; i++)
    {
        state->i_history[i] = 0.0f;
        state->q_history[i] = 0.0f;
    }
    state->position = 0;
    state->fill = 0;
    for (uint8_t i = 0; i < AUDIO_IQ_BLANKER_AMP_HIST; i++)
    {
        state->amp_hist[i] = 0.0f;
    }
    state->amp_pos = 0;
    state->amp_fill = 0;
    state->amp_div = 0;
    state->amp_ref = 0.0f;
}

/**
 * @brief remove ignition spikes from one block of I/Q samples, in place
 *
 * The output stream is the input stream delayed by AUDIO_IQ_BLANKER_DELAY samples; blocks are
 * processed back to back, the two samples of latency simply move across the block boundary.
 *
 * @param state blanker state
 * @param i in/out: I samples of the block, the oldest first
 * @param q in/out: Q samples of the block, the oldest first
 * @param count number of samples in the block
 * @param spike_threshold replace a sample whose amplitude is this times the largest amplitude
 *        in front of it, 4 (mild) down to 2.5 (aggressive)
 * @param energy_threshold replace a sample whose amplitude is this times the median of the
 *        recent amplitudes, 3 (mild) down to 2 (aggressive)
 */
static inline void AudioIQBlanker_Process(AudioIQBlankerState_t *state, float *i, float *q,
                                          uint16_t count, float spike_threshold, float energy_threshold)
{
    for (uint16_t k = 0; k < count; k++)
    {
        state->i_history[state->position] = i[k];
        state->q_history[state->position] = q[k];

        const float amp = fabsf(i[k]) + fabsf(q[k]);

        // amplitude history and its median, refreshed every AUDIO_IQ_BLANKER_AMP_DIV samples
        state->amp_div++;
        if (state->amp_div >= AUDIO_IQ_BLANKER_AMP_DIV)
        {
            state->amp_div = 0;
            state->amp_hist[state->amp_pos] = amp;
            state->amp_pos = (uint8_t)((state->amp_pos + 1) % AUDIO_IQ_BLANKER_AMP_HIST);
            if (state->amp_fill < AUDIO_IQ_BLANKER_AMP_HIST)
            {
                state->amp_fill++;
            }
            float med[AUDIO_IQ_BLANKER_AMP_HIST];
            for (uint8_t n = 0; n < state->amp_fill; n++)
            {
                const float key = state->amp_hist[n];
                int8_t m = (int8_t)(n - 1);
                med[n] = key;
                while ((m >= 0) && (med[m] > key))
                {
                    med[m + 1] = med[m];
                    m--;
                }
            }
            state->amp_ref = med[state->amp_fill / 2];
        }
        const uint8_t amp_ref_valid = (state->amp_fill >= AUDIO_IQ_BLANKER_AMP_HIST / 2);

        // the sample handed out by this call is the centre of the window, AUDIO_IQ_BLANKER_DELAY
        // positions in front of the newest one
        const uint8_t centre = (uint8_t)((state->position + AUDIO_IQ_BLANKER_WINDOW - AUDIO_IQ_BLANKER_DELAY) % AUDIO_IQ_BLANKER_WINDOW);

        if (state->fill >= AUDIO_IQ_BLANKER_WINDOW)
        {
            // the largest amplitude in front of the centre sample
            float neighbours = AUDIO_IQ_BLANKER_MIN_AMP;
            for (uint8_t n = 1; n <= AUDIO_IQ_BLANKER_DELAY; n++)
            {
                const uint8_t idx = (uint8_t)((centre + AUDIO_IQ_BLANKER_WINDOW - n) % AUDIO_IQ_BLANKER_WINDOW);
                const float neighbour_amp = fabsf(state->i_history[idx]) + fabsf(state->q_history[idx]);
                if (neighbour_amp > neighbours)
                {
                    neighbours = neighbour_amp;
                }
            }
            const float centre_amp = fabsf(state->i_history[centre]) + fabsf(state->q_history[centre]);

            const uint8_t spike = (centre_amp > spike_threshold * neighbours);
            const uint8_t pulse = amp_ref_valid && (centre_amp > energy_threshold * state->amp_ref);

            if (spike || pulse)
            {
                // replace I and Q by the mean of the two samples on each side, and write the
                // repaired pair back, so the next sample of a multi sample pulse is caught as
                // well
                const uint8_t im1 = (uint8_t)((centre + AUDIO_IQ_BLANKER_WINDOW - 1) % AUDIO_IQ_BLANKER_WINDOW);
                const uint8_t im2 = (uint8_t)((centre + AUDIO_IQ_BLANKER_WINDOW - 2) % AUDIO_IQ_BLANKER_WINDOW);
                const uint8_t ip1 = (uint8_t)((centre + 1) % AUDIO_IQ_BLANKER_WINDOW);
                const uint8_t ip2 = (uint8_t)((centre + 2) % AUDIO_IQ_BLANKER_WINDOW);
                state->i_history[centre] = 0.25f * (state->i_history[im2] + state->i_history[im1]
                                                  + state->i_history[ip1] + state->i_history[ip2]);
                state->q_history[centre] = 0.25f * (state->q_history[im2] + state->q_history[im1]
                                                  + state->q_history[ip1] + state->q_history[ip2]);
            }
        }
        else if (state->fill < AUDIO_IQ_BLANKER_WINDOW)
        {
            state->fill++;
        }

        i[k] = state->i_history[centre];
        q[k] = state->q_history[centre];
        state->position = (uint8_t)((state->position + 1) % AUDIO_IQ_BLANKER_WINDOW);
    }
}

#endif
