/* UHSDR digital mode host bench - shared declarations. See README.md. */
#ifndef DIGI_BENCH_H
#define DIGI_BENCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "arm_math.h"          // the host stand-in for the CMSIS types the firmware sources use

/* ---- host stand-ins for the firmware environment (modem_host.c) ---- */
void        Bench_TextReset(void);
const char* Bench_TextGet(void);                 // decoded characters handed to UiDriver_TextMsgPutChar
extern size_t Bench_CharClock;                   // the sample index the decoder is being fed
extern size_t Bench_CharAt[];                    // where in that stream each decoded character ended
extern int    Bench_CharCount;
void        Bench_TxFeed(const char* text);      // characters the modulator may pull from the digi buffer
bool        Bench_TxOffRequested(void);          // RadioManagement_Request_TxOff() was called
void        Bench_TxOffClear(void);
int           Bench_TxRemoveCalls(void);

/* ---- signal conditioning ---- */
void     Bench_RngSeed(uint32_t seed);
float    Bench_Gauss(void);                      // deterministic, seeded
void     Bench_AddNoise(float32_t* x, size_t n, float snr_db, float rms);
void     Bench_AddQsb(float32_t* x, size_t n, float rate_hz, float depth_db);

/* ---- scoring ---- */
size_t Bench_Levenshtein(const char* a, const char* b);
float  Bench_Cer(const char* expected, const char* got);   // edit distance / len(expected)

/* ---- wav ---- */
bool Bench_WavWrite(const char* path, const float32_t* x, size_t n, uint32_t rate);

/* ---- weather fax ---- */
#define WEFAX_SAMPLES_PER_LINE   6000      // 120 lines/min at 12 kHz
#define WEFAX_SYNC_SAMPLES       60        // 5 ms sync tip
#define WEFAX_IMG_COLS           300
#define WEFAX_TEST_ROWS          60

typedef struct
{
    const char* name;
    float    snr_db;          // added AWGN, 0 -> clean
    float    qsb_rate_hz;     // sine fading rate
    float    qsb_depth_db;    // fading depth
    float    skew_ppm;        // per-line random timing skew, parts per million of a line
    bool     invert;          // black and white tones swapped (some stations do)
    bool     steer_right;     // scan direction reversed
    uint32_t rows;
    float    tune_hz;         // every tone moved by this much: a tuning or reference error, which
                              // is ppm * rf_hz in Hz (14 Hz per ppm on 20 m, 50 on 6 m)
} wefax_cfg_t;

// synthesise one broadcast: samples (WEFAX_SAMPLES_PER_LINE * rows long) plus the ground truth
// grey levels (rows * WEFAX_IMG_COLS, 0..255). Deterministic for a given cfg.
void Bench_WefaxGenerate(float32_t* out, uint8_t* truth, const wefax_cfg_t* cfg);

typedef struct
{
    uint32_t lines_expected;
    uint32_t lines_found;
    float    row_rms_error;   // pixel columns
    float    grey_corr;       // Pearson correlation against the ground truth
    float    mean_level;      // decoded grey mean, 0..255
} wefax_result_t;

bool Bench_WefaxDecode(const float32_t* x, size_t n, uint8_t* grey, wefax_result_t* res);

#endif
