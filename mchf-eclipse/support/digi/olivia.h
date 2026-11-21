// Olivia as a reference modem for the bench, in the same spirit as navtex.c and mfsk16.c: it exists to
// be measured. See the README section for what is the mode and what is this bench's stand-in.

#ifndef OLIVIA_H
#define OLIVIA_H

#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"

typedef struct
{
    double   period_samples;   // the symbol the receiver fitted, in samples at 12 kHz
    unsigned symbols;          // symbols read
    unsigned pairs_ok;         // symbols whose two tones held more than half the energy
    int      phase;            // the symbol phase the receiver locked to
    int      header;           // the length the stream declared, -1 when it ran out first
} olivia_result_t;

// Text to audio at 12 kHz: 32 tones 31.25 Hz apart from 935 Hz up, 31.25 Bd, two tones per symbol.
// `tune_hz` moves the tone grid, which is where a tuning error enters this mode.
size_t Bench_OliviaGenerate(float32_t* out, size_t cap, const char* text, float tune_hz);

// The receiver fits the symbol phase, takes the two strongest tones of each symbol, maps the pair
// back to its seven bits, undoes the interleaver and reads the length header.
int Bench_OliviaDecode(const float32_t* x, size_t n, char* out, size_t out_sz, olivia_result_t* res);

#endif
