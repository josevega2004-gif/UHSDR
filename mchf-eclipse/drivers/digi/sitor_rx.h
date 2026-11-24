/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                **
 **                                        UHSDR                                   **
 **               a powerful firmware for STM32 based SDR transceivers             **
 **                                                                                **
 **--------------------------------------------------------------------------------**
 **                                                                                **
 **  Description:   SITOR-B / NAVTEX receive demodulator, streaming                **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

#ifndef SITOR_RX_H
#define SITOR_RX_H

#include <stdint.h>
#include "arm_math.h"

/* The alphabet, shared by the demodulator here and by the reference modem in support/digi/navtex.c,
 * so the radio and the measurement cannot disagree about what a code point means.  Indexed by the
 * seven bit CCIR 476 code; zero is "not a character", and only the 35 constant ratio words are filled
 * in, which is what makes the table its own validator. */
extern const char SitorLetters[128];
extern const char SitorFigures[128];

#define SITOR_SIA    0x0Fu   // phasing signal 1
#define SITOR_SIB    0x33u   // phasing signal 2
#define SITOR_REV_SIA 0x78u  // the same two as a transmitter that keys the other way round sends them
#define SITOR_REV_SIB 0x66u
#define SITOR_FIGS   0x36u
#define SITOR_LTRS   0x5Au
#define SITOR_SP     0x5Cu
#define SITOR_RPT    0x66u
#define SITOR_BLK    0x6Au

/* What the mode gives for free, and what this receiver uses.  It is written down here because the
 * first version of this file used none of it, and the difference is the whole difference between a
 * receiver that works at 10 dB and one that will not lock at 20 dB:
 *
 *   1. every valid character is exactly four marks and three spaces, so the SHARE of words that are
 *      4-of-7 is a synchronisation objective and a confidence number at the same time.  It ranks
 *      candidate bit clocks.  A random 7 bit word is 4-of-7 35/128 = 27 % of the time, so noise
 *      cannot score well, which is also the end of the need for a separate mode test;
 *   2. the bit period is what is being ranked, so it is measured rather than assumed - but measured
 *      by trying the few integer periods around the nominal and letting the objective function pick,
 *      not by autocorrelating transitions, which is the dead end the first version went down;
 *   3. the transmission opens with an alternation of SIA and SIB.  Read as a fifteen-bit window that
 *      is four specific bit patterns and nothing else, so an exact match gives the clock, the phase,
 *      the seven-bit group boundary AND the bit order in one test, and it does it while the preamble
 *      is still running.  The alphabet test cannot do that: SIA and SIB are both 4-of-7, so during
 *      the preamble every one of the seven boundaries scores perfectly and none can be told from
 *      another.  That degeneracy is why this file now has two lock paths instead of one;
 *   4. each character is sent twice, five character slots (350 ms) later.  Pair slot k with slot k+5
 *      and every character is emitted exactly once, on its repeat copy.  Emitting on both is how the
 *      first version printed its text twice.
 *
 * Costs, for a radio rather than for a bench: the grid is 1200 candidates of a handful of bytes, the
 * history is a 14 bit window per candidate, and once locked the receiver stops searching and only
 * feeds its own candidate, so the search is paid during acquisition instead of forever. */

/* How far the grid of candidate clocks reaches either side of the nominal bit period, in whole samples
 * of that period.  The table holds (2*SPAN+1) periods, each with one candidate per sample of its
 * period, so SPAN=4 at 12 kHz is 9 * about 120 = 1080 candidates covering +-3.3 % of rate error, and
 * SPAN=1 is 360 candidates covering +-0.8 %.  A radio locked to its own crystal is inside tens of ppm,
 * so the narrow grid is the one that belongs in a build which also wants the convolution filter: the two
 * features live in the same 128 KB.  The CI table is measured at the default wide span, so a narrowed
 * build runs a different receiver from the one that was graded - narrower is cheaper, not proven better.
 *
 * This comment used to claim SPAN=1 cost "about a tenth" of the wide grid while SITOR_MAX_GRID was a
 * hard 1300 that no span changed: every array but base[] and ccur[] was sized by that constant, so a
 * span-1 build saved 48 bytes of the 21 KB being described, and the radio measured 19,056 bytes of
 * stack headroom with span 4 against 19,104 with span 1.  The bound below is derived from the knob now,
 * so the knob is worth the thing it is documented to be worth. */
#ifndef SITOR_GRID_SPAN
#define SITOR_GRID_SPAN        4      // integer samples of period tried: nominal +- this
#endif
#ifndef SITOR_GRID_NOMINAL
#define SITOR_GRID_NOMINAL     120    // bit period in samples the arrays are sized for: 100 Bd at 12 kHz,
                                      // which is the only rate the radio feeds this receiver.  A call with
                                      // a wider nominal (48 kHz would be 480) clips the grid at line
                                      // SITOR_MAX_GRID below, and SitorRx_GridClipped() says so rather
                                      // than letting the truncation pass as a narrow search.
#endif
#define SITOR_MAX_GRID         ((2 * SITOR_GRID_SPAN + 1) * (SITOR_GRID_NOMINAL + SITOR_GRID_SPAN + 1))
#define SITOR_BITS_TO_DECIDE   150    // bits a candidate must have seen before it may win on alphabet: 26 words,
#define SITOR_DECIDE_EVERY     120     // bits between decisions while acquiring
#define SITOR_ALPHA_LOCK       109     // aged 4-of-7 share that locks: 128 is 100 %, so this is 85 %
#define SITOR_ALPHA_DROP        70     // below this the lock is given up and the grid is searched again
#define SITOR_MARGIN_NUM         3     // the winner must beat the runner up by 3x, integer math for 1.5x
#define SITOR_RATE_TOLERANCE     1     // samples of period error the integer grid can follow
#define SITOR_PHASING_RUN        6     // alternating SIA/SIB words that make a phasing lock
#define SITOR_ORDER_LIMIT       40     // characters before the bit order is assumed rather than known
#define SITOR_MAX_WIN          512     // the boxcar's ring: one bit at up to 51.2 kHz
#define SITOR_ALIGN_RING         8     // characters kept for the time diversity pairing

typedef struct
{
    // ---- the discriminator -------------------------------------------------------------------
    // Difference of the two tone energies, each measured over exactly one nominal bit by a sliding
    // Goertzel: X[n] = X[n-1] + (x[n] - x[n-W]) * e^{-j w n}, which is a boxcar of W samples and so
    // the matched filter for the symbol, at four multiplies and two adds a sample for both tones.
    // The window stays at the nominal bit length while the grid tries other periods: a few percent
    // of window error costs a little depth, whereas a window that tracked every candidate would cost
    // a ring buffer per candidate.
    float32_t rot_c, rot_s;        // e^{-j w} per sample, low tone
    float32_t rot2_c, rot2_s;      // and high tone
    float32_t ph_c, ph_s;          // current e^{-j w n}, low tone
    float32_t ph2_c, ph2_s;        // and high tone
    float32_t acc_i, acc_q;        // sliding sum, low tone
    float32_t acc2_i, acc2_q;      // and high tone
    float32_t lag_c, lag_s;        // e^{-j w (n-W)}: the rotation the leaving sample came in with
    float32_t lag2_c, lag2_s;      // and at the high tone
    float32_t ring[SITOR_MAX_WIN]; // the sample W ago, for the boxcar's subtraction
    int32_t   win;                 // W, nominal samples per bit
    int32_t   wring;               // write index in ring
    float32_t disc;                // high tone energy minus low tone energy
    uint32_t  sample_count;

    // ---- the candidate grid ------------------------------------------------------------------
    int32_t   per_lo, per_hi;      // integer periods covered, inclusive
    int32_t   grid_clipped;
    int32_t   base[SITOR_GRID_SPAN * 2 + 1];   // where each period's candidates start in the arrays
    int32_t   nper;                // how many periods
    int32_t   ccur[SITOR_GRID_SPAN * 2 + 1];   // running phase index within each period
    int32_t   total;               // candidates in use
    uint16_t  win14[SITOR_MAX_GRID];   // last 14 decisions, newest in bit 0
    uint16_t  seen[SITOR_MAX_GRID];    // bits fed to this candidate, saturating
    uint8_t   score[SITOR_MAX_GRID][7];// aged 4-of-7 share per group boundary, 128 = always valid
    uint8_t   margin[SITOR_MAX_GRID];  // aged |discriminator|: tie-break between equal shares
    uint8_t   ph_run[SITOR_MAX_GRID];  // current run of alternating SIA/SIB words
    uint8_t   ph_best[SITOR_MAX_GRID]; // longest such run seen
    uint8_t   ph_off[SITOR_MAX_GRID];  // group boundary the phasing implies
    uint8_t   ph_ord[SITOR_MAX_GRID];  // bit order the phasing implies, 1 = reversed
    uint32_t  ph_next[SITOR_MAX_GRID]; // bit count at which the next alternation is due
    uint16_t  ph_end_word[SITOR_MAX_GRID];   // word index at which a long phasing run stopped
    uint8_t   ph_have_end[SITOR_MAX_GRID];
    // ---- the live receiver --------------------------------------------------------------------
    int32_t   state;
    int32_t   period;              // the winning integer period, samples per bit
    int32_t   phase;               // its phase within the period
    int32_t   align;               // which of the seven boundaries the characters start on
    int32_t   reversed;            // the take carries the bits the other way round
    int32_t   order_locked;        // the order came from phasing, not from the convention
    int32_t   letters;             // figures shift or letters shift
    int32_t   live[3];             // the winner and its two neighbouring phases: all of the grid a
                                   // locked receiver keeps feeding
    int32_t   live_n;
    int32_t   win_idx;             // the candidate the receiver is reading characters from
    int32_t   prev_best;           // the alphabet leader at the last decision: the same answer twice
    uint32_t  next_decide;         // sample index of the next acquisition decision
    uint32_t  char_slots;          // character slots since the lock, for the diversity pairing
    uint32_t  ring_codes[SITOR_ALIGN_RING * 2 + 2];  // characters, for pairing k with k+5
    int32_t   ring_head;
    uint8_t   agree[10];           // per residue class, how often the two copies have matched
    int32_t   block_phase;         // where the block of ten starts, inferred from `agree`
    int32_t   block_locked;
    int32_t   rate_out_of_range;
   // the measured clock needs resampling, which this receiver does not do

    // ---- confidence and the counters the bench grades ------------------------------------------
    float32_t valid_share;         // the winner's aged 4-of-7 share, 0..1
    uint32_t  chars_seen;          // characters looked at while deciding the bit order
    uint32_t  chars_out;           // characters printed since init
    uint32_t  repaired;            // characters whose two copies disagreed and were fixed
    uint32_t  lost;                // characters with no surviving copy
    int32_t   best_norm, best_rev; // longest phasing alternation read each way, for the debug line
    uint32_t  locks;               // times the receiver has settled, and re-settled after a burst
    uint32_t  phasing_locks;       // ... of which came from the preamble rather than from the alphabet

    char      out_char;            // the character the last SitorRx_Sample() produced
} SitorRx;

enum
{
    SITOR_SEARCH_RATE = 0,   // the grid is being fed, no candidate has won yet
    SITOR_SEARCH_ALIGN,      // kept for the bench's debug line: a winner is provisional
    SITOR_LOCKED             // characters are being assembled from one candidate
};

// Initialise for a given audio rate and tone pair.  NAVTEX is 100 Bd with a 170 Hz shift and the pair
// sits at 1615/1785 Hz of the receiver's audio when the operator has tuned it onto the lower tone;
// both are arguments because the radio's audio frequency is the operator's tuning, not the mode's.
void SitorRx_Init(SitorRx* s, float32_t sample_rate_hz, float32_t low_hz, float32_t high_hz);

// One audio sample.  Returns 1 when a character is ready, 0 otherwise.
int SitorRx_Sample(SitorRx* s, float32_t x);

// The receiver's own opinion of the take, 0..1: the share of recent words that were valid 4-of-7
// characters, at the clock and boundary it is using.  It needs no expected text, which is the whole
// reason a capture hunt can use it - and noise alone caps it near 0.27, because a random 7 bit word
// is 4-of-7 35 times in 128.
float32_t SitorRx_Confidence(const SitorRx* s);

// 1 once the clock and the group boundary are settled.
static inline int SitorRx_Locked(const SitorRx* s) { return s->state == SITOR_LOCKED; }

/* How many characters the receiver has printed since init.  SitorRx_Confidence above is the other
 * number the status line needs, and was already here; this one was not, and the counter it reports is
 * the same one the bench table grades, so the radio and the table cannot disagree about what was
 * copied. */
static inline uint32_t SitorRx_CharsReceived(const SitorRx* s) { return s->chars_out; }

// 1 when the grid the caller asked for did not fit SITOR_MAX_GRID and was cut short.  It is reported
// because a silent truncation is how a build ends up with a receiver nobody measured.
static inline int SitorRx_GridClipped(const SitorRx* s) { return s->grid_clipped; }

#endif