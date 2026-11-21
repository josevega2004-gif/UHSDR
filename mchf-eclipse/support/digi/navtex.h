/* NAVTEX / SITOR-B for the UHSDR digital mode host bench. See navtex.c. */
#ifndef DIGI_BENCH_NAVTEX_H
#define DIGI_BENCH_NAVTEX_H

#include <stddef.h>
#include "arm_math.h"

typedef struct
{
    int      bit_phase;        // sample offset of the first bit the receiver fitted
    double   period_samples;   // its bit length, in samples: 120.0 is 100 Bd at 12 kHz
    int      group_offset;     // which of the seven bits starts a character
    double   valid_fraction;   // share of the words at that alignment that are 4-of-7
    int      phasing_run;      // how many alternating phasing characters it found
    int      reversed;         // the take carries the bits in the order the table is not written in
    unsigned characters;       // characters the receiver tried to read
    unsigned accepted;         // pairs that combined to a word within 3 bits of a valid one
    unsigned repaired;         // characters whose two copies did not agree
    unsigned both_bad;         // characters with neither copy inside the correction radius
    unsigned bits;             // seven bit words the alignment produced
} navtex_result_t;

// text -> 12 kHz audio at 100 Bd, 170 Hz shift (space 1615 / mark 1785 Hz), phasing and FEC framing
// included. tune_hz moves both tones, which is a reference error and not a shift error. Returns the
// number of samples written.
size_t Bench_NavtexGenerate(float32_t* out, size_t cap, const char* text, float tune_hz);

// audio -> text. Returns 0 when no alignment would hold (fewer than 20 characters read).
int Bench_NavtexDecode(const float32_t* x, size_t n, char* out, size_t out_sz, navtex_result_t* res);

#endif
