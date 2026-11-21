// Olivia as a reference modem for the bench, in the same spirit as navtex.c and mfsk16.c: it exists to
// be measured. See the README section for what is the mode and what is this bench's stand-in.

#ifndef OLIVIA_H
#define OLIVIA_H

#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"

typedef struct
{
    unsigned symbols;          // symbols read
    unsigned tones_ok;         // symbols whose winning tone held more than half the energy
    unsigned blocks;           // 64 symbol Walsh blocks decoded
    int      header;           // the length the stream declared, -1 when it ran out first
} olivia_result_t;

// Text to audio at 12 kHz: 32 tones 31.25 Hz apart from 935 Hz up, 31.25 Bd, one tone per symbol,
// five characters in a Walsh block of sixty four symbols. `tune_hz` moves the tone grid, which is
// where a tuning error enters this mode.
size_t Bench_OliviaGenerate(float32_t* out, size_t cap, const char* text, float tune_hz);

// The receiver fits the symbol phase, decides each symbol by its strongest tone, undoes the Gray
// code, the scrambler and the diagonal spreading against the Walsh table, and reads the length
// header the stream starts with.
int Bench_OliviaDecode(const float32_t* x, size_t n, char* out, size_t out_sz, olivia_result_t* res);

// How many samples that decode can take: five characters a block into a buffer of 512, after which
// the take is refused rather than read short. A longer recording has to be cropped and said so.
size_t Bench_OliviaMaxSamples(void);

#endif
