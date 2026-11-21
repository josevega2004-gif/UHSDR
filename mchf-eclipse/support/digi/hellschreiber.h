/* Hellschreiber for the UHSDR digital mode host bench. See hellschreiber.c. */
#ifndef DIGI_BENCH_HELLSCHREIBER_H
#define DIGI_BENCH_HELLSCHREIBER_H

#include <stddef.h>
#include "arm_math.h"

#define HELL_COLS   5       // glyph columns
#define HELL_ROWS   7       // pixels per column
#define HELL_CELL   6       // glyph plus its blank column

typedef struct
{
    int    pixel_phase;      // sample offset of the first pixel the receiver fitted
    double pixel_samples;    // its pixel length, in samples: 97.96 is 122.5 Bd at 12 kHz
    int    column_phase;     // which column of the six the raster starts on
    double gap_share;        // share of the gap columns that came out blank: 1.0 is a found raster
} hell_result_t;

// text -> 12 kHz on/off keying at tone_hz, 122.5 Bd, one pixel per 1/122.5 s; truth gets the picture
// that was sent (rows * max_cols, one byte per pixel) and cols_out how many columns it holds
size_t Bench_HellGenerate(float32_t* out, size_t cap, const char* text, uint8_t* truth,
                          int max_cols, int* cols_out, float tone_hz, float clk_pct);

// audio -> picture. Returns 0 when there is nothing to read.
int Bench_HellDecode(const float32_t* x, size_t n, uint8_t* pix, int max_cols, int* cols_out,
                     hell_result_t* res);

#endif
