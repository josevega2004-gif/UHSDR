/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/**********************************************************************************
 **                                                                              **
 **                               UHSDR                                          **
 **                a powerful firmware for STM32 based SDR transceivers          **
 **                                                                              **
 **--------------------------------------------------------------------------------
 **  Description: Hellschreiber receiver, written to run one sample at a time    **
 **  Licence:     GNU GPLv3                                                      **
 **********************************************************************************/

/**
 * @file hell_rx.h
 * @brief streaming Feld-Hell receiver: one sample in, one keyed pixel out
 *
 * Why this file exists separately from support/digi/hellschreiber.c: that one is the reference modem.
 * It sees the whole take, sorts it to find a key threshold, tries every pixel phase against every
 * sample, and picks the grouping that leaves one column in six blank.  A radio cannot do any of that.
 * This file makes the same decisions with a fixed amount of state and one sample at a time, which is
 * the form the audio interrupt needs, and the bench grades it against the reference on identical
 * takes - so the question the table answers is not "does it decode" but "what does streaming cost".
 *
 * The mode has no alphabet and no error control, which removes the tool the SITOR-B receiver was
 * built around: 4-of-7 validity is a synchronisation objective that noise cannot fake, and Feld-Hell
 * has nothing like it.  What is left is the *shape* of the raster.  Each character is five glyph
 * columns plus one blank column, drawn as keyed pixels at a fixed rate, so at the true pixel period
 * the envelope is strongly two-valued (ink and space) and its variance sits almost entirely between
 * pixel slots; at a wrong period the sampling grid walks across pixel edges and the same variance
 * smears into the slots.  So the objective here is
 *
 *      score(period) = variance of the bucket means / variance of the samples
 *
 * where each candidate period holds one running sum per phase slot.  It is scale-free, which matters
 * because a QSB ride changes the signal level and must not look like a lost lock, and it is a real
 * discriminator rather than self-agreement: a receiver cannot raise this number by agreeing with its
 * own decisions.
 *
 * Two known weaknesses, both inherited from the mode rather than this implementation:
 * - Nothing in the objective says the carrier is a Hell carrier.  Any on/off keyed signal at the
 *   right rate scores well, which is why the mode gate (support/digi, the shape test) still has to be
 *   the thing that decides whether to believe a take at all.
 * - The pixel period has to be searched, and the search is what costs memory.  Each candidate period
 *   needs one bucket per phase slot, so a ±2 % range around 122.5 Bd at 12 kHz is nine periods of
 *   about 98 slots.  Widening the range to cover 100-300 Bd multiplies that by the range, which is
 *   why HELL_PERIOD_LO/HI are build knobs and the default is narrow and stated, not broad and hidden.
 */
#ifndef DIGI_HELL_RX_H
#define DIGI_HELL_RX_H

#include <stdint.h>
#include "arm_math.h"

/* The raster the mode transmits: five glyph columns, a sixth left blank, seven pixels per column. */
#define HELL_RX_COLS     5
#define HELL_RX_ROWS     7
#define HELL_RX_CELL     6

/* Receiver geometry, all three overridable from the build line so the cost can be measured:
 *  - the tone the envelope filter sits on and its width, which together set how far off-tune a
 *    station can be and still be seen (the reference table pins +700 Hz as out of reach, and this
 *    filter's first null is at 12000/HELL_RX_TONE_WIN, so the number is arithmetic, not preference);
 *  - the pixel-period range to search, in samples at the configured sample rate.  94..102 is 122.5 Bd
 *    plus or minus about two per cent, which is the span the bench table measures. */
#ifndef HELL_RX_TONE_HZ
#define HELL_RX_TONE_HZ   1000.0f
#endif
#ifndef HELL_RX_TONE_WIN
#define HELL_RX_TONE_WIN  40        /* boxcar width in samples: ~300 Hz wide, ~1/2 pixel */
#endif
#ifndef HELL_PERIOD_LO
#define HELL_PERIOD_LO    94
#endif
#ifndef HELL_PERIOD_HI
#define HELL_PERIOD_HI    102
#endif
/* The grid is searched in fractions of a sample, held as Q7 fixed point.  This is not refinement for
 * its own sake: the mode's true pixel period at 12 kHz and 122.5 Bd is 97.959 samples, and an integer
 * grid can only offer 98.  That 0.041-sample error is invisible in one pixel and fatal in a line - it
 * accumulates to a whole pixel of slip every 24 pixels, which shears the raster at a column boundary
 * about every twenty-fifth one, and the picture then reads as roughly 5 % wrong pixels with almost no
 * complete glyphs even though the clock was found, the level was found, and every pixel but the seams
 * is correct.  SITOR-B hit the same wall from the other side (a rate error of 1.5 % is two integer
 * steps, so it cannot be followed at all), so the fix is the same one: hold a non-integer period.
 *
 * 1/16 of a sample puts the worst residual at 0.031, which is 0.4 pixels of slip across a 1300-pixel
 * take - under one seam.  Halving the step halves the slip and doubles the grid, which is the whole
 * trade: HELL_PERIOD_STEP is Q7, so 16 is 1/16 and 8 is 1/8. */
#define HELL_PERIOD_Q7(p)  ((int)((p) * 128.0 + 0.5))
#ifndef HELL_PERIOD_STEP
#define HELL_PERIOD_STEP   16        // Q7: 16/128 = one eighth of a sample
#endif
#define HELL_MAX_PERIOD    HELL_PERIOD_HI
#define HELL_N_PERIODS     (((HELL_PERIOD_HI - HELL_PERIOD_LO) * 128) / HELL_PERIOD_STEP + 1)

typedef struct
{
    /* ---- the tone envelope, one sample at a time --------------------------------------------- */
    float32_t rot_c, rot_s;                 // e^{-j w} per sample at the keying tone
    float32_t ph_c, ph_s;                   // current e^{-j w n}
    float32_t ring[2 * HELL_RX_TONE_WIN];   // rotated samples, interleaved i,q: what leaves the boxcar
    float32_t acc_i, acc_q;                 // sliding sum
    float32_t env;                          // |z|: the keyed envelope of the last HELL_RX_TONE_WIN samples
    uint32_t  n;                            // samples since init
    float32_t total_sum, total_sq;          // the denominator of the variance share, kept running

    /* ---- the candidate pixel clocks ------------------------------------------------------------
     * One entry per candidate period, holding the statistics of the *pixel blocks* that period
     * produces: how many completed, and the running sum and sum of squares of their mean levels.
     *
     * This replaced a design that kept one accumulator per phase slot, reasoning that a correct period
     * would show its keying structure across the slots.  It does not, and the measurement said so
     * immediately: a pixel is one constant level, so every phase inside it holds the same value and the
     * variance across slots stayed near zero (0.001 of the total) however well the mode was being
     * copied.  What separates at the true period is the sequence of successive pixel levels, so that is
     * what is accumulated here.  It is also far cheaper: the phase-slot grid cost 7,764 bytes per
     * receiver, almost all of it buying a statistic that could not rise. */
    struct {
        float32_t blk_sum;          // envelope accumulated inside the block being filled
        uint16_t  blk_n;            // samples so far in this block
        uint16_t  phase_q7;         // position inside the period, in 1/128 samples
        float32_t blk_min, blk_max; // extremes inside the block being filled: does it straddle an edge?
        uint32_t  straddle_n;       // blocks that held both an inked and an uninked stretch
        float32_t mean_sum;         // sum of completed block means
        double    mean_sq;          // sum of their squares
        uint32_t  mean_n;           // how many blocks have completed
        float32_t lo_sum, hi_sum;   // the two clusters: a threshold that tracks the level
        uint32_t  lo_n, hi_n;
        float32_t mid;              // the midpoint between them, updated as blocks complete
        uint32_t  clear_n;          // blocks that landed clearly on one side: the pixel-count analogue
                                    // of "this clock is producing whole pixels, not mixtures"
    } cand[HELL_N_PERIODS];

    /* ---- decision state ------------------------------------------------------------------------ */
    uint32_t  next_decide;                  // sample index of the next grid evaluation
    int32_t   period_index;                 // which candidate won
    uint16_t  period_q7;                    // its period in 1/128 samples; the clock actually used
    int32_t   period;                       // the whole-sample part, for reporting only

    float32_t thr;                          // ink/space threshold, from the grid's own two clusters
    float32_t score_best;                   // the winner's variance share, 0..1
    float32_t score_second;                 // the runner-up's, so the margin is visible
    int       locked;                       // 1 once a period and threshold were accepted
    int       pixels_out;                   // pixels emitted since the lock, for the acquisition cost
    uint8_t   pixel;                        // the ink state the last HellRx_Sample() produced
    uint8_t   have_pixel;                   // and whether it is new on this call
} HellRx;

/* The receiver deliberately does not group pixels into columns or characters.  Feld-Hell has no
 * character sync to find, and the reference modem gets that grouping wrong for the same reason it
 * gets the clock wrong, so folding the guess in here would let a synchronisation error hide inside a
 * decoding score.  Assembling seven pixels into a column and six columns into a cell is the caller's
 * job, which is also what the bench scorer does - and it means the table can report the gap structure
 * as an independent observation rather than as something the receiver believed about itself. */

/**
 * Initialise.  `sample_rate_hz` is the rate the caller will feed samples at; the period range is
 * expressed in samples because that is what the grid needs, and the caller is responsible for giving
 * it a range that brackets its baud rate at that rate (see HELL_PERIOD_LO/HI).
 */
void HellRx_Init(HellRx* s, float32_t sample_rate_hz, float32_t tone_hz);

/**
 * One audio sample.  Sets s->pixel to the ink state (1 = keyed) of the pixel that ended here and
 * returns 1 when a pixel is ready, 0 otherwise.  Pixels are produced at the locked period, so the
 * caller assembles seven of them into a column and six columns into a character, exactly as the
 * raster was sent.
 */
int HellRx_Sample(HellRx* s, float32_t x);

/* 1 once a pixel period and threshold have been accepted. */
static inline int HellRx_Locked(const HellRx* s) { return s->locked; }

/* How decisively the grid chose: the winner's variance share, and the runner-up's.  A take that is not
 * Feld-Hell at all, or a period the grid cannot bracket, shows up here as a winner that is not ahead,
 * which is the only honest form of confidence available to a mode with no alphabet. */
static inline float HellRx_Confidence(const HellRx* s) { return s->score_best; }
static inline float HellRx_Margin(const HellRx* s) { return s->score_best - s->score_second; }
static inline int HellRx_Period(const HellRx* s) { return s->period; }

/* The period the receiver is actually running, in 1/128 samples.  Reported separately from
 * HellRx_Period() because the fraction is the thing worth seeing: 97.959 is the mode and 98 is what an
 * integer grid can offer, and the difference between those two is a whole pixel every twenty-fourth. */
static inline int HellRx_PeriodQ7(const HellRx* s) { return s->period_q7; }

#endif
