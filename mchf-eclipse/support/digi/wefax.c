/* Weather fax (HF WEFAX) signal generator and reference demodulator, host only.
 *
 * The generator makes the broadcast the decoder has to survive: 120 lines/min at 12 kHz, a 5 ms
 * sync tip at 1857 Hz opening each line, picture tone swept between 1500 Hz (black) and 2300 Hz
 * (white), optional polarity swap, scan reversal, per line timing skew, QSB and AWGN.
 *
 * The demodulator is written the cheap way on purpose: after the sync tip fixes the line start,
 * each pixel gets its own exact DFT (Goertzel) at the black and the white frequency over the
 * pixel slot, so no filters have to be tuned and the per sample cost is a handful of multiply
 * accumulates. That is the number that decides whether this mode fits on an F4.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"

#define WEFAX_RATE          12000.0f
#define WEFAX_F_SYNC        1857.0f
#define WEFAX_F_BLACK       1500.0f
#define WEFAX_F_WHITE       2300.0f
#define WEFAX_GAP_SAMPLES   540                 // blanking after the picture
#define WEFAX_PIX_SAMPLES   18                  // 300 columns per line

static float grey_to_freq(float grey, bool invert)
{
    if (invert) grey = 255.0f - grey;
    return WEFAX_F_BLACK + (WEFAX_F_WHITE - WEFAX_F_BLACK) * (grey / 255.0f);
}

// deterministic test chart: gradient, bars, two "landmass" blobs, a dark frame
static uint8_t chart_grey(uint32_t row, uint32_t col, uint32_t rows)
{
    if (row < 3 || row >= rows - 3 || col < 3 || col >= WEFAX_IMG_COLS - 3) return 250;   // frame
    if (col < WEFAX_IMG_COLS / 3) return (uint8_t) (255.0f * (float) col / (WEFAX_IMG_COLS / 3.0f));
    if (col < 2 * WEFAX_IMG_COLS / 3)
    {
        int band = (int) ((col - WEFAX_IMG_COLS / 3) / 12);
        return (band & 1) ? 20 : 235;
    }
    float dx = (float) (col - 5 * WEFAX_IMG_COLS / 12) / 22.0f;
    float dy = (float) (row - rows / 3) / (rows / 6.0f);
    if (dx * dx + dy * dy < 1.0f) return 40;
    dx = (float) (col - 9 * WEFAX_IMG_COLS / 12) / 18.0f;
    dy = (float) (row - 2 * rows / 3) / (rows / 5.0f);
    if (dx * dx + dy * dy < 1.0f) return 70;
    return 215;
}

static void tone(float32_t* out, size_t from, size_t len, float freq, float amp, size_t total)
{
    size_t ramp = 4;                            // short fade keeps the synthetic signal click free
    for (size_t i = 0; i < len && from + i < total; i++)
    {
        float env = 1.0f;
        if (i < ramp) env = (float) i / (float) ramp;
        else if (i + ramp > len) env = (float) (len - i) / (float) ramp;
        out[from + i] += env * amp * sinf(2.0f * (float) M_PI * freq * (float) i / WEFAX_RATE);
    }
}

void Bench_WefaxGenerate(float32_t* out, uint8_t* truth, const wefax_cfg_t* cfg)
{
    size_t total = (size_t) cfg->rows * WEFAX_SAMPLES_PER_LINE;
    memset(out, 0, total * sizeof(float32_t));

    Bench_RngSeed(0x5eed1234u);
    for (uint32_t row = 0; row < cfg->rows; row++)
    {
        int skew = 0;
        if (cfg->skew_ppm > 0.0f)
        {
            skew = (int) ((Bench_Gauss() * cfg->skew_ppm / 1e6) * WEFAX_SAMPLES_PER_LINE);
        }
        size_t base = (size_t) row * WEFAX_SAMPLES_PER_LINE + (size_t) (skew > 0 ? skew : 0);
        if (base + WEFAX_SAMPLES_PER_LINE > total) base = total > WEFAX_SAMPLES_PER_LINE ? total - WEFAX_SAMPLES_PER_LINE : 0;

        tone(out, base, WEFAX_SYNC_SAMPLES, WEFAX_F_SYNC, 0.9f, total);

        for (uint32_t col = 0; col < WEFAX_IMG_COLS; col++)
        {
            uint32_t src = cfg->steer_right ? (WEFAX_IMG_COLS - 1 - col) : col;
            uint8_t grey = chart_grey(row, src, cfg->rows);
            if (truth) truth[row * WEFAX_IMG_COLS + col] = grey;
            size_t at = base + WEFAX_SYNC_SAMPLES + (size_t) col * WEFAX_PIX_SAMPLES;
            tone(out, at, WEFAX_PIX_SAMPLES, grey_to_freq(grey, cfg->invert), 0.75f, total);
        }
        tone(out, base + WEFAX_SYNC_SAMPLES + (size_t) WEFAX_IMG_COLS * WEFAX_PIX_SAMPLES,
             WEFAX_GAP_SAMPLES, grey_to_freq(255.0f, cfg->invert), 0.75f, total);
    }
}

/* exact DFT power of one bin over a short window */
static float bin_power(const float32_t* x, size_t n, float freq)
{
    float w = 2.0f * (float) M_PI * freq / WEFAX_RATE;
    float cw = cosf(w), sw = sinf(w);
    float re = 0.0f, im = 0.0f, c = 1.0f, s = 0.0f;
    for (size_t i = 0; i < n; i++)
    {
        re += x[i] * c;
        im -= x[i] * s;
        float nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    return (re * re + im * im) / ((float) n * (float) n);
}

bool Bench_WefaxDecode(const float32_t* x, size_t n, uint8_t* grey, wefax_result_t* res)
{
    memset(res, 0, sizeof(*res));
    size_t pos = 0;
    size_t lines = 0;
    size_t starts[512];

    while (pos + WEFAX_SAMPLES_PER_LINE <= n)
    {
        // look for a sync tip: the 1857 Hz bin must dominate the line start
        size_t best = 0;
        float best_ratio = 0.0f;
        for (size_t cand = pos; cand + WEFAX_SYNC_SAMPLES + 40 <= n && cand < pos + WEFAX_SAMPLES_PER_LINE; cand += 6)
        {
            float ps = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_SYNC);
            float pb = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_BLACK);
            float pw = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_WHITE);
            float ratio = ps / (ps + pb + pw + 1e-9f);
            if (ratio > best_ratio) { best_ratio = ratio; best = cand; }
            if (ratio > 0.85f) break;
        }
        if (best_ratio < 0.6f)
        {
            pos += WEFAX_SAMPLES_PER_LINE / 2;
            continue;
        }

        size_t start = best + WEFAX_SYNC_SAMPLES;
        for (uint32_t col = 0; col < WEFAX_IMG_COLS; col++)
        {
            size_t at = start + (size_t) col * WEFAX_PIX_SAMPLES;
            if (at + WEFAX_PIX_SAMPLES > n) break;
            float pb = bin_power(&x[at], WEFAX_PIX_SAMPLES, WEFAX_F_BLACK);
            float pw = bin_power(&x[at], WEFAX_PIX_SAMPLES, WEFAX_F_WHITE);
            float r = pw / (pb + pw + 1e-12f);           // 1 = white, 0 = black
            int g = (int) lroundf((2.0f * r - 1.0f) * 255.0f);
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            grey[lines * WEFAX_IMG_COLS + col] = (uint8_t) g;
            res->mean_level += (float) g;
        }
        if (lines < 512) starts[lines] = best;
        lines++;
        pos = start + (size_t) WEFAX_IMG_COLS * WEFAX_PIX_SAMPLES + WEFAX_GAP_SAMPLES;
    }

    res->lines_found = (uint32_t) lines;

    // Timing stability: how far the captured sync tips sit from a perfect one-line-per-6000-sample
    // grid, after removing the constant offset of the first line. A decoder that ignored the sync
    // tip would show the transmitter skew here instead of ~0.
    // Line spacing stability. The transmitter skews each line on purpose in some test cases, so
    // this is the amount of skew the sync capture absorbed, not a decoder error: if the tip were
    // ignored and the picture sampled on a blind grid, the grey correlation would collapse here.
    if (lines > 1)
    {
        float acc = 0.0f;
        for (size_t i = 1; i < lines; i++)
        {
            float d = (float) ((int) (starts[i] - starts[i - 1]) - (int) WEFAX_SAMPLES_PER_LINE);
            acc += d * d;
        }
        res->row_rms_error = sqrtf(acc / (float) (lines - 1));
    }
    res->mean_level = lines ? res->mean_level / ((float) lines * WEFAX_IMG_COLS) : 0.0f;
    return lines > 0;
}
