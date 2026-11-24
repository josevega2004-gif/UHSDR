/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/**********************************************************************************
 **                                                                              **
 **                               UHSDR                                          **
 **                a powerful firmware for STM32 based SDR transceivers          **
 **                                                                              **
 **--------------------------------------------------------------------------------
 **  Description: streaming Feld-Hell receiver                                   **
 **  Licence:     GNU GPLv3                                                      **
 **********************************************************************************/

/**
 * @file hell_rx.c
 * @brief Hellschreiber pixel synchroniser and key detector, one sample at a time
 *
 * See hell_rx.h for the objective and for what the mode gives and does not give a receiver.  The short
 * form: Feld-Hell has no alphabet to validate against, so the only thing to lock on is that at the true
 * pixel period the envelope is strongly two-valued and its variance sits between the pixel slots, while
 * at a wrong period the grid walks across pixel edges and that variance leaks into the slots.
 *
 * The one trap in a receiver shaped like this, worth stating before the code: the slot means are used to
 * *find* the clock and the level, never to decide what was sent.  Emitting "this slot is ink because its
 * average is high" would print the take's histogram over and over - it would reproduce the picture's
 * statistics instead of its text, and it would look like a decode.  So each pixel is decided against its
 * own envelope value, sampled at the middle of its own slot, and the grid supplies timing and level only.
 * The bench table is written so that this distinction is visible: it scores the raster, and a receiver
 * that printed the average picture would score badly on it because real text does not repeat.
 */
#include <string.h>
#include <math.h>
#ifdef HELL_DEBUG
#include <stdio.h>
#endif
#include "hell_rx.h"

/* How often the clocks are evaluated, in samples: about 170 ms at 122.5 Bd.  The first decision waits
 * longer on purpose - HELL_MIN_BLOCKS below is the real gate, and a candidate that has completed fewer
 * pixels than that has not shown enough of the keying to be believed. */
#define HELL_DECIDE_EVERY   2048
#define HELL_MIN_BLOCKS       24     // pixels a candidate must have completed before it can win
/* The lock is absolute, not a margin.  With a grid this fine the candidates either side of the truth
 * are indistinguishable - they differ by a sixteenth of a sample - so "beat the runner-up" asks a
 * question the grid cannot answer, and when it was tried the receiver locked late and then switched
 * clock mid-message, which re-phases the pixel blocks and costs a pixel at the seam.  SITOR-B met the
 * same thing: a ±1-sample neighbour of the right clock is the same clock, not a rival.
 *
 * What the absolute level has to clear is set by measurement rather than taste: white noise puts about
 * 0.72 of its blocks outside the midpoint by arithmetic (a normal spread has that much beyond a
 * quarter of its own range), so anything at or below that is not evidence of keying.  Real takes of the
 * generated picture sit at 0.89 to 1.00.  The gap between those is the whole of the discriminator, and
 * the noise-only row of the table is what keeps it honest. */
#define HELL_SCORE_LOCK     0.88f    // share of blocks that land clearly on one side of the middle
#define HELL_SCORE_DROP     0.70f    // below this, at the next evaluation, the lock is given back
#ifndef HELL_CLEAR_FRAC
#define HELL_CLEAR_FRAC     0.25f    // how far outside the midpoint a block must land to count as whole
#endif

/* Score every candidate clock on how much of the envelope's variance sits between its completed pixel
 * blocks, and return the best and second-best.  A steady carrier puts none there (its blocks are all
 * equal), and a period that is wrong puts little (its blocks straddle pixel edges and average two
 * levels together); only the true pixel rate swings from ink to space.  Nothing is stored on the
 * receiver here - the caller decides whether the numbers are enough to lock on. */
static void grid_evaluate(const HellRx* s, int* best_i, float32_t* best_score, float32_t* second_score)
{
    int bi = -1;
    float32_t bs = -1.0f, s2 = -1.0f;

    *best_i = -1; *best_score = -1.0f; *second_score = -1.0f;

    if (s->n < 512u) return;

    /* Total variance of the envelope, over the same span the blocks were collected in.  Using the
     * sample-level variance as the denominator is what makes this scale-free: a station fading by 20 dB
     * changes every level and changes this ratio not at all, so a fade cannot look like a lost lock. */
    const double gmean_all = (double)s->total_sum / (double)s->n;
    const double var_total = (double)s->total_sq / (double)s->n - gmean_all * gmean_all;
    if (var_total < 1e-12) return;

    for (int i = 0; i < HELL_N_PERIODS; i++)
    {
        if (s->cand[i].mean_n < HELL_MIN_BLOCKS) continue;

        /* Purity, not spread.  Spread is satisfied by any clock that produces different numbers, which
         * includes a clock a sample away from the truth: its blocks still alternate, and its variance
         * stays high, while the raster it prints is sheared.  A pixel is one constant level, so the
         * true clock's blocks land at exactly two values and a wrong one's land in between as well.
         * Counting the blocks that land clear of the middle measures that, and it is what separates
         * 98 samples from 99 when the transmitter is at 97.96. */
        const float32_t lo_m = s->cand[i].lo_n ? s->cand[i].lo_sum / (float32_t)s->cand[i].lo_n : 0.0f;
        const float32_t hi_m = s->cand[i].hi_n ? s->cand[i].hi_sum / (float32_t)s->cand[i].hi_n : 0.0f;
        (void) lo_m; (void) hi_m;
        const double bm = (double)s->cand[i].mean_sum / (double)s->cand[i].mean_n;
        const double var_blocks = (double)s->cand[i].mean_sq / (double)s->cand[i].mean_n - bm * bm;
        /* One minus the straddle rate.  The purity count stays as a floor: it says the block levels
         * really are two-valued, which a slow wobble on an unkeyed carrier would not produce. */
        const float32_t straddle_rate = (float32_t)s->cand[i].straddle_n / (float32_t)s->cand[i].mean_n;
        const float32_t purity = (float32_t)((double)s->cand[i].clear_n / (double)s->cand[i].mean_n);
        float32_t score = 1.0f - straddle_rate;
        if (purity < 0.70f) score = 0.0f;
        /* Both measures have to agree: purity alone rewards a clock that sees only one level (a steady
         * carrier: every block clear of a midpoint that never moves), so the spread is kept as a gate. */
        if (var_blocks < 0.05 * var_total) score = 0.0f;
        if (score > 1.0f) score = 1.0f;        // the two parts are not an exact partition
#ifdef HELL_DEBUG
        fprintf(stderr, "[hell] P=%.4f blocks=%u score=%.3f varb=%.5f vart=%.5f mid=%.4f\n",
                (HELL_PERIOD_LO * 128 + i * HELL_PERIOD_STEP) / 128.0,
                s->cand[i].mean_n, score, var_blocks, var_total, s->cand[i].mid);
#endif
        if (score > bs) { s2 = bs; bs = score; bi = i; }
        else if (score > s2) s2 = score;
    }

    *best_i = bi; *best_score = bs; *second_score = s2;
}

void HellRx_Init(HellRx* s, float32_t sample_rate_hz, float32_t tone_hz)
{
    memset(s, 0, sizeof(*s));

    const double w = 2.0 * 3.14159265358979 * (double)tone_hz / (double)sample_rate_hz;
    s->rot_c = (float32_t)cos(w);
    s->rot_s = (float32_t)sin(w);
    s->ph_c = 1.0f;
    s->ph_s = 0.0f;
    s->next_decide = HELL_DECIDE_EVERY;   // the first grid read is one window in, not immediately
}

int HellRx_Sample(HellRx* s, float32_t x)
{
    s->have_pixel = 0;

    /* ---- envelope -----------------------------------------------------------
     * Multiply by e^{-jwn} to bring the keying tone to DC, then boxcar it.  The window is
     * HELL_RX_TONE_WIN samples, so its first null is at sample_rate / HELL_RX_TONE_WIN, about 300 Hz
     * away: that is the reach of the filter, stated as arithmetic rather than as a tuning, and it is
     * what makes a station 200 Hz off still readable and one 700 Hz off invisible - which the reference
     * table already pins as a row that must fail. */
    const float32_t xi = x * s->ph_c;
    const float32_t xq = x * s->ph_s;
    const unsigned ri = (s->n % HELL_RX_TONE_WIN) * 2u;

    s->acc_i += xi - s->ring[ri];
    s->acc_q += xq - s->ring[ri + 1u];
    s->ring[ri] = xi;
    s->ring[ri + 1u] = xq;

    s->env = sqrtf(s->acc_i * s->acc_i + s->acc_q * s->acc_q);
    s->total_sum += s->env;
    s->total_sq += s->env * s->env;
    s->n++;

    /* advance the rotation; this is a unit circle, so it needs occasional renormalising if the caller
     * runs it for hours - the bench takes are minutes, and the note is here so the next reader does not
     * have to rediscover the drift. */
    const float32_t pc = s->ph_c * s->rot_c + s->ph_s * s->rot_s;
    s->ph_s = s->ph_s * s->rot_c - s->ph_c * s->rot_s;
    s->ph_c = pc;

    /* ---- fill the pixel blocks for every candidate clock, and emit on the locked one ----
     * Each candidate accumulates the envelope until its period is full, then records the block's own
     * mean.  That mean is what decides ink or space for the locked candidate: the pixel's own level,
     * never an average across pixels - a receiver that printed the average picture would reproduce the
     * take's statistics and look like text without carrying any. */
    for (int i = 0; i < HELL_N_PERIODS; i++)
    {
        HellRx* rr = s;               // (kept explicit so the block below reads as one unit)
        const int pq7 = HELL_PERIOD_LO * 128 + i * HELL_PERIOD_STEP;
        rr->cand[i].blk_sum += s->env;
        rr->cand[i].blk_n++;
        /* Only the middle of the block is watched.  The envelope filter is a 40-sample boxcar - about
         * two fifths of a pixel - so its output ramps for 20 samples either side of a keying edge, and
         * a block that includes those ramps shows a large internal swing whether the clock is right or
         * not.  Measured: with the whole block watched, 28 % of blocks "straddled" on a clean take,
         * which is indistinguishable from noise and the receiver never locked at all.  The flat interior
         * is the part that carries the pixel's level, so that is the part compared. */
        if (rr->cand[i].blk_n * 5u > rr->cand[i].blk_n && (unsigned)(pq7 >> 2) != 0u)
        {
            const unsigned quarter = (unsigned)(pq7 >> 2) / 128u;      // a quarter of the pixel, in samples
            const unsigned q = rr->cand[i].blk_n;
            if (q > quarter && q < 3u * quarter)
            {
                if (s->env < rr->cand[i].blk_min) rr->cand[i].blk_min = s->env;
                if (s->env > rr->cand[i].blk_max) rr->cand[i].blk_max = s->env;
            }
        }
        rr->cand[i].phase_q7 = (uint16_t)(rr->cand[i].phase_q7 + 128);

        /* The block closes when the fractional period is consumed and the remainder carries into the
         * next one, so block lengths alternate 97 and 98 samples in exactly the ratio the mode needs.
         * This is the whole point of the Q7 arithmetic: with an integer period the receiver can lock,
         * find the level, and get every pixel right except at the seams, and still read as 5 % wrong
         * with almost no intact glyphs, because 98 against a true 97.959 slips a pixel every 24th. */
        if (rr->cand[i].phase_q7 >= pq7) rr->cand[i].phase_q7 -= (uint16_t)pq7;
        else continue;

        const float32_t bm = rr->cand[i].blk_sum / (float32_t)rr->cand[i].blk_n;
        /* The clock test, in its sharpest available form.  A pixel holds one constant level, so a block
         * covering exactly one pixel has both extremes on the same side of the middle; a block that
         * straddles a keying edge holds both.  Only a wrong clock accumulates those progressively -
         * drift the length of one pixel and one block in twenty-four spans an edge - so counting
         * straddles is an early/late gate in all but name.  Unlike the purity ratio it separates
         * candidates a tenth of a sample apart, which is the difference between a picture that reads
         * and one sheared by a seam every fifth glyph. */
        if (rr->cand[i].blk_min < rr->cand[i].mid && rr->cand[i].blk_max > rr->cand[i].mid)
        {
            rr->cand[i].straddle_n++;
        }
        rr->cand[i].blk_sum = 0.0f;
        rr->cand[i].blk_n = 0;
        rr->cand[i].blk_min = 1e30f;
        rr->cand[i].blk_max = -1e30f;

        /* Statistics for the score.  Both are over completed blocks, so a candidate that is half a
         * sample off its neighbours is compared on the same quantity, not on a different window. */
        rr->cand[i].mean_sum += bm;
        rr->cand[i].mean_sq += (double)bm * (double)bm;
        rr->cand[i].mean_n++;

        /* Online two-cluster split: the midpoint assigns this block, and the midpoint then moves.
         * Seeded from the first block so it never sits at zero, where every later block would fall on
         * one side and the threshold would freeze. */
        if (rr->cand[i].mean_n == 1u) rr->cand[i].mid = bm;
        const float32_t lo_m = rr->cand[i].lo_n ? rr->cand[i].lo_sum / (float32_t)rr->cand[i].lo_n : 0.0f;
        const float32_t hi_m = rr->cand[i].hi_n ? rr->cand[i].hi_sum / (float32_t)rr->cand[i].hi_n : 0.0f;
        const float32_t sep = hi_m - lo_m;
        if (bm > rr->cand[i].mid) { rr->cand[i].hi_sum += bm; rr->cand[i].hi_n++; }
        else { rr->cand[i].lo_sum += bm; rr->cand[i].lo_n++; }
        if (rr->cand[i].lo_n && rr->cand[i].hi_n)
        {
            /* A block counts as clear when it sits well outside the middle of the two clusters.  The
             * factor is the whole of the tuning: too small and every block counts, which is the failure
             * mode that let a one-sample-wrong clock win, too large and a noisy take never locks. */
            if (sep > 1e-6f)
            {
                const float32_t d = bm - rr->cand[i].mid;
                if ((d > 0.0f ? d : -d) > HELL_CLEAR_FRAC * sep) rr->cand[i].clear_n++;
            }
            rr->cand[i].mid = 0.5f * (rr->cand[i].lo_sum / (float32_t)rr->cand[i].lo_n
                                      + rr->cand[i].hi_sum / (float32_t)rr->cand[i].hi_n);
        }

        if (s->locked && i == s->period_index)
        {
            s->pixel = (bm > rr->cand[i].mid) ? 1u : 0u;
            s->have_pixel = 1u;
            s->pixels_out++;
        }
    }

    /* ---- decide, or give the decision back ---- */
    if (s->n >= s->next_decide)
    {
        s->next_decide = s->n + HELL_DECIDE_EVERY;

        int bi;
        float32_t bs, s2;
        grid_evaluate(s, &bi, &bs, &s2);

        if (bi >= 0)
        {
            if (!s->locked)
            {
                if (bs >= HELL_SCORE_LOCK)
                {
                    s->locked = 1;
                    s->period_index = bi;
                    s->period_q7 = (uint16_t)(HELL_PERIOD_LO * 128 + bi * HELL_PERIOD_STEP);
                    s->period = s->period_q7 >> 7;
                    s->pixels_out = 0;
                }
            }
            else if (bs < HELL_SCORE_DROP)
            {
                /* Keying stopped, or the station drifted past the envelope's reach.  Stop emitting
                 * rather than keep producing pixels: Hell produced from a lost lock is ink in the wrong
                 * places, and ink in the wrong places still looks like a font.  The grid is cleared, so
                 * the next acquisition is a real search and not the residue of the last one. */
                s->locked = 0;
                memset(s->cand, 0, sizeof(s->cand));
                s->n = 0;
                s->total_sum = 0.0f;
                s->total_sq = 0.0f;
                s->next_decide = HELL_DECIDE_EVERY;
            }
            else
            {
                /* Still locked: follow the level, and move to a better period if one turns up.  A
                 * transmitter a little outside the bracketed range should be recovered rather than
                 * fought, and the grid is still feeding every candidate so it can. */
                /* The winning candidate's own midpoint is the level: there is no fixed threshold
                 * anywhere in this file, and none to tune.  The clock is deliberately NOT switched
                 * here even when another candidate scores higher - changing candidate re-phases the
                 * pixel blocks, and a mid-message phase change puts a seam through the raster.  A
                 * clock that turns out to be wrong is corrected by the drop rule above, which resets
                 * the grid and starts a real search, rather than by sliding sideways. */
                (void) bi;
            }
            s->score_best = bs;
            s->score_second = s2;
        }
    }

    return s->have_pixel ? 1 : 0;
}
