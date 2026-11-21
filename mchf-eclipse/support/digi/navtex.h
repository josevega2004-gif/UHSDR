/* NAVTEX / SITOR-B for the UHSDR digital mode host bench. See navtex.c. */
#ifndef DIGI_BENCH_NAVTEX_H
#define DIGI_BENCH_NAVTEX_H

#include <stddef.h>
#include "arm_math.h"

typedef struct
{
    double   period_samples;   // the bit length the receiver fitted, in samples: 120.0 is 100 Bd
    double   valid_fraction;   // share of the words at that alignment that are 4-of-7
    int      phasing_run;      // how many alternating phasing characters it found
    int      reversed;         // the take carries the bits in the order the table is not written in
    unsigned repaired;         // characters whose two copies did not agree
    unsigned both_bad;         // characters with neither copy inside the correction radius
} navtex_result_t;

/* The bit order a take is generated in. SITOR sends each seven bit character least significant bit
 * first, which is how the tables in navtex.c are read. A transmitter following the CCIR 476 wording
 * instead sends the same words most significant bit first, and the reversal of every 4-of-7 word is
 * still 4-of-7, so the ratio cannot tell the two apart - only the phasing pair can. NAVTEX_ORDER_MIXED
 * puts the phasing in the table's order and the text in the other one: the receiver is told the
 * wrong order, which is what a wrong detection costs. */
enum { NAVTEX_ORDER_TABLE = 0, NAVTEX_ORDER_REVERSED = 1, NAVTEX_ORDER_MIXED = 2 };

// text -> 12 kHz audio at 100 Bd, 170 Hz shift (space 1615 / mark 1785 Hz), phasing and FEC framing
// included. tune_hz moves both tones, which is a reference error and not a shift error; order is one
// of NAVTEX_ORDER_*. Returns the number of samples written.
size_t Bench_NavtexGenerate(float32_t* out, size_t cap, const char* text, float tune_hz, int order);

// audio -> text. Returns 0 when no alignment would hold (fewer than 20 characters read).
int Bench_NavtexDecode(const float32_t* x, size_t n, char* out, size_t out_sz, navtex_result_t* res);

#endif
