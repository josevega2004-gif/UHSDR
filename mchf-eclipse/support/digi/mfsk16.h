// MFSK16 as a reference modem for the bench, in the same spirit as wefax.c and navtex.c: it exists
// to be measured, not to run on the radio. See the README section for what is the mode and what is
// this bench's stand-in for it.

#ifndef MFSK16_H
#define MFSK16_H

#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"

typedef struct
{
    unsigned symbols;         // symbols read
    unsigned tones_ok;        // symbols whose winning tone held more than half the energy
    unsigned bits;            // coded bits read
    unsigned viterbi_bits;    // decoded information bits
    unsigned characters;      // Varicode characters framed out of them
} mfsk16_result_t;

// Text to audio at 12 kHz: 16 tones 15.625 Hz apart from 1 kHz up, 15.625 Bd, one tone per symbol.
// `tune_hz` moves the tone grid, which is where a tuning error enters this mode.
size_t Bench_Mfsk16Generate(float32_t* out, size_t cap, const char* text, float tune_hz);

// The receiver fits the symbol phase, decides each symbol by which tone holds the energy, undoes the
// interleaver, runs the Viterbi decoder and frames the result with the Varicode table.
int Bench_Mfsk16Decode(const float32_t* x, size_t n, char* out, size_t out_sz, mfsk16_result_t* res);

#endif
