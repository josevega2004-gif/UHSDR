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
#define SITOR_ORDER_LIMIT 40 // characters to wait for the phasing before falling back to the convention
#define SITOR_FIGS   0x36u
#define SITOR_LTRS   0x5Au
#define SITOR_SP     0x5Cu
#define SITOR_RPT    0x66u
#define SITOR_BLK    0x6Au

// Window and search sizes.  The rate search looks at 2048 samples - 17 bits at the slowest rate it
// will accept - and tries bit periods from 96 to 144 samples at 12 kHz, which is 83 to 125 Bd: wide
// enough for a capture whose sample rate is not exactly the transmitter's, narrow enough that the
// search costs 100k multiplies once rather than continuously.  A 5 % error is what the old reference
// decoder could not follow; this covers ±20 % and then tracks.
#define SITOR_EDGE_WINDOW   4096
#define SITOR_LAG_MIN         96
#define SITOR_LAG_MAX        144
#define SITOR_RING            16   // characters held for the time diversity: two blocks of five, spare
// Bits spent deciding the group boundary.  It has to be shorter than a transmission, not just
// statistically comfortable: the first version asked for 1400 bits and a NAVTEX message of 46
// characters is 861 bits long, so the decision was never reached and the receiver sat in alignment
// forever on every take this bench can generate.
#define SITOR_ALIGN_BITS     280
#define SITOR_WIN_CHARS      60    // characters the confidence number is a share of
#ifndef SITOR_LOCK_SHARE
#define SITOR_LOCK_SHARE    0.85f  // 4-of-7 share that settles the boundary and holds the lock
#endif
#define SITOR_DROP_SHARE    0.55f  // below this for SITOR_DROP_CHARS characters, search again
#define SITOR_DROP_CHARS      40

/* A streaming SITOR-B receiver: one sample in, and a character out when there is one.  It is written
 * this way because that is the shape the radio has - there is no whole transmission to look back over,
 * and the buffer between the audio interrupt and this is one block long.
 *
 * The reference implementation in support/digi/navtex.c cannot be used on the radio for the same
 * reason: it finds its bit clock by trying every phase and every group boundary in a window of the
 * file, which is a search over the whole take, and it only ever tried bit periods from 118 to 122
 * samples, which is ±1.7 % of 100 Bd and is a statement about what this bench can generate rather
 * than about what a recording can be.  Here the rate is *measured* from the transitions in the
 * signal, over a range, and then tracked.
 *
 * What the mode gives for free, and what this uses:
 *   - every valid character has exactly four marks and three spaces, so a word that is not 4-of-7 is
 *     a word the receiver knows it got wrong, and the share of words that are valid is a confidence
 *     number that does not need the text;
 *   - each character is sent twice, five character slots apart, so one damaged copy can be taken
 *     from the other;
 *   - the transmission opens with an alternation of SIA and SIB, which is how the bit order is told.
 */
typedef struct
{
    // ---- the two tones ---------------------------------------------------------------------
    // A rotating phasor per tone, a one pole lowpass on each of its two parts, and the difference of
    // the two energies is the discriminator.  Four state variables per tone and about ten multiplies
    // a sample for the whole front end: a radio pays per sample, and the shape a 100 Bd signal needs
    // is only "about fifty hertz wide", which one pole gives.
    float32_t ph_lo_c, ph_lo_s;      // e^{-j w n} at the low tone
    float32_t ph_hi_c, ph_hi_s;      // and at the high tone
    float32_t rot_lo_c, rot_lo_s;    // the angle one sample advances
    float32_t rot_hi_c, rot_hi_s;
    float32_t lp_lo_i, lp_lo_q;      // the lowpassed baseband, i.e. the tone's complex envelope
    float32_t lp_hi_i, lp_hi_q;
    float32_t alpha;                 // the pole, from the bandwidth
    uint32_t  sample_count;

    // ---- the bit clock ---------------------------------------------------------------------
    float32_t period;          // samples per bit, measured then tracked
    float32_t phase;           // how far into the current bit we are
    float32_t half;            // period / 2, where the decision is taken
    int32_t   since_edge;      // samples since the last bit boundary
    int       last_bit;        // the previous decision, for finding transitions
    float32_t disc;            // the discriminator, high tone energy minus low tone energy
    uint32_t  bit_count;       // bits decided, for the confidence window

    // ---- rate acquisition ------------------------------------------------------------------
    // The transition train, one byte per sample, and the autocorrelation of it: a FSK signal
    // changes tone at bit boundaries, so the train repeats at exactly one bit period whatever
    // that period is.  This is the part that replaced the 118..122 assumption.
    uint8_t   edges[SITOR_EDGE_WINDOW];
    int32_t   edge_fill;
    uint32_t  edge_start;      // the receiver's sample index where this window began
    float32_t rate_score;    // how sharply the transitions piled up: 1.0 is no structure at all

    // ---- word assembly and alignment -------------------------------------------------------
    // The last fourteen bits, newest in the low end.  Fourteen and not seven because the group
    // boundary is one of seven possibilities and each of them has to be scored from the same bits:
    // a character whose first bit is `a` samples back in time is the window hist[a..a+6], read with
    // the oldest bit as the low one, which is how SITOR sends - least significant bit first.  A
    // receiver that shifts seven bits into a register and calls the result a character has picked
    // one of the seven alignments silently, and gets the bit order wrong besides.
    uint32_t  hist;
    int32_t   hist_bits;
    int32_t   align;           // which of the seven boundaries the characters start on
    uint32_t  ring[SITOR_RING];  // whole characters, newest at ring[head]
    int32_t   head;
    int32_t   have;            // how many characters the ring holds so far

    // the seven possible group boundaries, counted while the clock is being settled
    uint32_t  trial_valid[7];
    uint32_t  trial_words[7];
    // Phasing alternation scored per candidate boundary while the boundary is still being found.
    // It has to be scored there and not after: the preamble is twenty characters and the alignment
    // needs forty to decide, so a receiver that starts looking for phasing once it is locked has
    // missed it on every take this bench can generate, and then prints text it cannot order.
    uint32_t  trial_run[7][2];     // current run length, forward and reversed
    uint32_t  trial_best[7][2];    // longest run seen, forward and reversed
    uint32_t  trial_prev[7];       // previous word at that boundary, all ones if none yet
    int32_t   trial_bits;
    int32_t   trial_bitpos;    // which of the seven offsets this bit belongs to

    // ---- lock and confidence ---------------------------------------------------------------
    int32_t   state;
    int32_t   letters;         // figures shift or letters shift
    int32_t   reversed;        // the take carries the bits the other way round
    int32_t   chars_seen;      // characters looked at while deciding the bit order
    int32_t   slot;            // character slots since the phasing ended, for the block phase
    uint32_t  prev_code;       // the character before this one in time, for the phasing test
    float32_t valid_share;     // share of recent words that are 4-of-7: the confidence number
    uint32_t  win_valid, win_words;
    uint32_t  phasing_run;
    int32_t   run_norm, run_rev, best_norm, best_rev;   // SIA/SIB alternation, read both ways
    int32_t   order_locked;      // the bit order is decided once, not re-decided by later text
    int32_t   bad_streak;      // characters in a row with neither copy surviving

    // ---- output ----------------------------------------------------------------------------
    char      out_char;        // the character the last SitorRx_Sample() produced, when it returned 1
    uint32_t  chars_out;       // characters printed since init
    uint32_t  repaired;        // characters whose two copies disagreed and were fixed
    uint32_t  lost;            // characters with no surviving copy
} SitorRx;

enum
{
    SITOR_SEARCH_RATE = 0,   // measuring the bit period from the transitions
    SITOR_SEARCH_ALIGN,      // period found; which of the seven boundaries the groups start on
    SITOR_LOCKED             // characters are being assembled
};

// Initialise for a given audio rate and tone pair.  NAVTEX is 100 Bd with a 170 Hz shift and the pair
// sits at 1615/1785 Hz of the receiver's audio when the operator has tuned it onto the lower tone;
// both are arguments because the radio's audio frequency is the operator's tuning, not the mode's.
void SitorRx_Init(SitorRx* s, float32_t sample_rate_hz, float32_t low_hz, float32_t high_hz);

// One audio sample.  Returns 1 when a character is ready, 0 otherwise.
int SitorRx_Sample(SitorRx* s, float32_t x);

// The receiver's own opinion of the take, 0..1: the share of the last characters that were valid
// 4-of-7 words.  It needs no expected text, which is the whole reason a capture hunt can use it.
float32_t SitorRx_Confidence(const SitorRx* s);

// 1 once the clock and the group boundary are settled.
static inline int SitorRx_Locked(const SitorRx* s) { return s->state == SITOR_LOCKED; }

#endif
