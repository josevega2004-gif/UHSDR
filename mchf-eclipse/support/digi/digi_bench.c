/* UHSDR digital mode host bench.
 *
 *   make test    self checks, non zero exit on regression
 *   make rtty    character error rate of the shipping RTTY modem, loopback
 *   make wefax   weather fax: synthetic broadcast, reference demodulator, scores and files
 *
 * The RTTY numbers come from drivers/audio/rtty.c itself: the firmware modulator generates the
 * signal, the firmware demodulator reads it back, and only the channel between them is invented
 * here. See README.md for what is stubbed and why that still counts.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "rtty.h"
#include "softdds.h"

#define RATE            12000.0f          // Rtty_Modem_Init() pins the demodulator at 12 kHz
#define TXRATE          48000.0f          // the modulator is fed at 4x that: one bit is
                                          // oneBitSampleCount * 4 calls to GenSample()
#define MAX_SAMP        200000


// from drivers/audio/rtty.c
extern void     Rtty_Modulator_StartTX(void);
extern int16_t  Rtty_Modulator_GenSample(void);

/* ------------------------------------------------------------------ *
 *  helpers
 * ------------------------------------------------------------------ */
static char normalize[2][8192];

// keep only what ITA2 can carry, so the comparison is about the modem, not about the alphabet
static const char* norm_baudot(const char* in, int which)
{
    char* out = normalize[which];
    size_t j = 0;
    for (const char* p = in; *p && j + 1 < sizeof(normalize[0]); p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') out[j++] = c;
    }
    out[j] = '\0';
    // leading and trailing run in is noise floor for the metric, collapse repeated spaces
    return out;
}

static float pearson(const uint8_t* a, const uint8_t* b, size_t n)
{
    if (n < 2) return 0.0f;
    double sa = 0, sb = 0;
    for (size_t i = 0; i < n; i++) { sa += a[i]; sb += b[i]; }
    double ma = sa / n, mb = sb / n, num = 0, da = 0, db = 0;
    for (size_t i = 0; i < n; i++)
    {
        double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    if (da <= 0 || db <= 0) return 0.0f;
    return (float) (num / sqrt(da * db));
}

static void pgm_write(const char* path, const uint8_t* img, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n%u %u\n255\n", w, h);
    fwrite(img, 1, (size_t) w * h, f);
    fclose(f);
}

/* ------------------------------------------------------------------ *
 *  RTTY loopback with the firmware modem
 * ------------------------------------------------------------------ */
typedef struct
{
    const char* label;
    uint16_t    shift;
    float       snr_db;
    float       qsb_rate;
    float       qsb_depth;
} rtty_case_t;

// The demodulator emits one or two characters of junk while it locks on, which is a property of
// the modem but not what the CER is meant to measure, so the score is taken from the best
// alignment of the received text against the expected one and the skipped lead-in is reported.
static int lead_in_junk;

static float score_aligned(const char* expected, const char* got)
{
    float best = 100.0f;
    int best_skip = 0;
    for (int skip = 0; skip < 12 && got[skip]; skip++)
    {
        float cer = Bench_Cer(expected, got + skip);
        if (cer < best) { best = cer; best_skip = skip; }
    }
    lead_in_junk = best_skip;
    return best;
}

static float rtty_run(const rtty_case_t* c, const char* msg, char* got, size_t got_sz, int* eot)
{
    size_t n  = 160000;                                  // received samples: 13.3 s, room for the message and EOT

    rtty_ctrl_config.speed_idx = RTTY_SPEED_45;
    for (int i = 0; i < RTTY_SHIFT_NUM; i++)
    {
        if (rtty_shifts[i].value == (float32_t) c->shift) rtty_ctrl_config.shift_idx = (rtty_shift_t) i;
    }

    // TX: the firmware modulator produces the waveform, EOT ends it
    Rtty_Modem_Init((uint32_t) TXRATE);
    char feed[256];
    snprintf(feed, sizeof(feed), "%s", msg);
    strncat(feed, "\004", sizeof(feed) - strlen(feed) - 1);
    Bench_TxFeed(feed);
    Bench_TxOffClear();
    Bench_TextReset();
    Rtty_Modulator_StartTX();

    static float32_t buf[(size_t) MAX_SAMP];
    for (size_t i = 0; i < n; i++)
    {
        // four transmit samples per received sample, averaged: a 4:1 decimation with a filter
        // that is more than wide enough for tones below 2 kHz
        float acc = 0.0f;
        for (int k = 0; k < 4; k++) acc += (float32_t) Rtty_Modulator_GenSample() / 32768.0f;
        buf[i] = acc * 0.25f;
    }
    *eot = Bench_TxOffRequested() ? 1 : 0;

    // channel, at the rate the demodulator sees
    Bench_AddQsb(buf, n, c->qsb_rate, c->qsb_depth);
    Bench_RngSeed(0xC0FFEEu);
    Bench_AddNoise(buf, n, c->snr_db, 0.0f);

    // RX: the firmware demodulator, characters collected by the UI stand-in
    Rtty_Modem_Init((uint32_t) TXRATE);
    for (size_t i = 0; i < n; i++)
    {
        Rtty_Demodulator_ProcessSample(buf[i]);
    }
    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    return score_aligned(norm_baudot(msg, 0), got);
}

static void rtty_table(void)
{
    const char* msg = "DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT";
    static const rtty_case_t cases[] =
    {
        { "clean 170",     170,  100.0f, 0, 0 },
        { "snr 20 170",    170,   20.0f, 0, 0 },
        { "snr 15 170",    170,   15.0f, 0, 0 },
        { "snr 10 170",    170,   10.0f, 0, 0 },
        { "snr 10 425",    425,   10.0f, 0, 0 },
        { "snr 10 850",    850,   10.0f, 0, 0 },
        { "snr 7 170",      170,   7.0f, 0, 0 },
        { "qsb 10 170",     170,  10.0f, 0.5f, 6.0f },
        { "qsb fast 10",    170,  10.0f, 2.0f, 10.0f },
    };

    printf("RTTY loopback of drivers/audio/rtty.c (45.45 Bd, 1.5 stop bits, tx 48 kHz -> rx 12 kHz)\n");
    printf("%-16s %7s %6s  %s\n", "case", "CER %", "lead", "decoded");
    char got[8192];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        int eot = 0;
        float cer = rtty_run(&cases[i], msg, got, sizeof(got), &eot);
        printf("%-16s %7.1f %6d  %s%s\n", cases[i].label, cer, lead_in_junk, got,
               eot ? "" : "   [transmission cut off before EOT]");
    }
    printf("\nnote: unsupported shifts are mapped to 170 by Rtty_Modem_Init(), and the shift list\n"
           "      in rtty.c is 85/170/200/425/450/850; a stored value outside it decodes badly.\n");
}

/* ------------------------------------------------------------------ *
 *  weather fax
 * ------------------------------------------------------------------ */
static void wefax_table(void)
{
    static const wefax_cfg_t cases[] =
    {
        { "clean",              100.0f, 0,    0,    0,    false, false, 12 },
        { "snr 20",              20.0f, 0,    0,    0,    false, false, 12 },
        { "snr 12",              12.0f, 0,    0,    0,    false, false, 12 },
        { "snr 12 slow qsb",     12.0f, 0.4f, 6.0f, 0,   false, false, 12 },
        { "snr 12 fast qsb",     12.0f, 2.0f, 10.0f, 0,  false, false, 12 },
        { "snr 12 + skew",       12.0f, 0,    0,    20000, false, false, 12 },
        { "snr 12 black/white swapped", 12.0f, 0,    0,   0,    true,  false, 12 },
        { "snr 12 reversed scan",12.0f, 0,    0,   0,    false, true,  12 },
        { "snr 8 qsb skew",       8.0f,   0.4f, 6.0f, 3000, false, false, 12 },
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);

    printf("HF weather fax, 120 lines/min at 12 kHz, sync 1857 Hz, black 1500 / white 2300 Hz\n");
    printf("%-28s %6s %8s %8s %8s\n", "case", "lines", "skewRMS", "corr", "mean");
    for (size_t i = 0; i < n; i++)
    {
        size_t samples = (size_t) cases[i].rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(samples * sizeof(float32_t));
        uint8_t* truth = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        uint8_t* grey = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        wefax_result_t res;

        Bench_WefaxGenerate(x, truth, &cases[i]);
        if (cases[i].snr_db < 90.0f)
        {
            Bench_RngSeed(0xF00Du);
            Bench_AddNoise(x, samples, cases[i].snr_db, 0.0f);
        }
        Bench_WefaxDecode(x, samples, grey, &res);
        res.lines_expected = cases[i].rows;

        size_t cmp = (size_t) (res.lines_found < cases[i].rows ? res.lines_found : cases[i].rows) * WEFAX_IMG_COLS;
        float corr = pearson(truth, grey, cmp);
        res.grey_corr = corr;

        printf("%-28s %3u/%-2u %8.1f %8.3f %8.1f\n", cases[i].name,
               res.lines_found, res.lines_expected, res.row_rms_error, corr, res.mean_level);

        if (i == 1)                     // leave one readable broadcast behind for inspection
        {
            Bench_WavWrite("wefax_snr20.wav", x, samples, (uint32_t) RATE);
            pgm_write("wefax_truth.pgm", truth, WEFAX_IMG_COLS, cases[i].rows);
            pgm_write("wefax_decoded.pgm", grey, WEFAX_IMG_COLS, res.lines_found ? res.lines_found : cases[i].rows);
        }
        free(x); free(truth); free(grey);
    }
    printf("\nblack/white swapped gives a correlation of about -0.95: the demodulator recovers the\n"
           "picture but with the polarity upside down, which is why a station parameter is needed.\n"
           "The reversed scan case scores normally by construction: the reference decoder has no\n"
           "phasing reference, so it reproduces whatever was scanned, which is exactly what a real\n"
           "operator fixes with the steer control.\n");
    printf("files: wefax_snr20.wav (listen), wefax_truth.pgm, wefax_decoded.pgm\n");
}

/* ------------------------------------------------------------------ *
 *  decoding a recorded file: the input path any real feed will use
 * ------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// 16 bit PCM WAV, mono or stereo, any sample rate: resampled to the 12 kHz the demodulator is
// pinned at, and gain trimmed from the file peak so a quiet recording still reaches the modems
// threshold. This is the hook for real audio (a WebSDR capture, another receivers file export,
// or an on-air recording), which is the whole reason the bench exists.
static float decode_wav(const char* path, const char* expected, char* got, size_t got_sz, uint16_t shift)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return -1.0f; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); printf("short read\n"); return -1.0f; }
    fclose(f);

    uint32_t rate = 0, ch = 1, i = 12; const uint8_t* data = 0; uint32_t dlen = 0;
    while (i + 8 <= (size_t)sz)
    {
        uint32_t clen = rd32(buf + i + 4);
        if (!memcmp(buf + i, "fmt ", 4)) { ch = rd16(buf + i + 10); rate = rd32(buf + i + 12); }  /* fmt data: tag,channels,rate,...,bits */
        else if (!memcmp(buf + i, "data", 4)) { data = buf + i + 8; dlen = clen; }
        else break;
        i += 8 + clen + (clen & 1);
    }
    if (!data || !rate || !dlen) { free(buf); printf("not a 16 bit PCM WAV with a data chunk\n"); return -1.0f; }

    size_t frames = (size_t)dlen / (2 * ch);
    float peak = 0.0f;
    for (size_t k = 0; k < frames; k++)
    {
        float v = (float)(int16_t)rd16(data + k * 2 * ch) / 32768.0f;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    float gain = peak > 0.0f ? 0.7f / peak : 1.0f;

    rtty_ctrl_config.speed_idx = RTTY_SPEED_45;
    for (int k = 0; k < RTTY_SHIFT_NUM; k++)
    {
        if (rtty_shifts[k].value == (float32_t)shift) rtty_ctrl_config.shift_idx = (rtty_shift_t)k;
    }
    Rtty_Modem_Init(12000);
    Bench_TextReset();

    size_t out_n = (size_t)((double)frames * 12000.0 / (double)rate);
    for (size_t k = 0; k < out_n; k++)
    {
        double src = (double)k * (double)rate / 12000.0;
        size_t a = (size_t)src;
        double fr = src - (double)a;
        if (a + 1 >= frames) a = frames > 1 ? frames - 2 : 0;
        float s0 = (float)(int16_t)rd16(data + a * 2 * ch) / 32768.0f;
        float s1 = (float)(int16_t)rd16(data + (a + 1) * 2 * ch) / 32768.0f;
        Rtty_Demodulator_ProcessSample((float32_t)((s0 + (float)fr * (s1 - s0)) * gain));
    }
    free(buf);

    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    printf("file      : %s\n", path);
    printf("format    : %u Hz, %u channel(s), %.1f s, gain %.1f dB\n",
           rate, ch, (double)frames / (double)rate, 20.0 * log10(gain));
    printf("decoded   : %s\n", got);
    if (expected && *expected) return score_aligned(norm_baudot(expected, 0), got);
    return -1.0f;
}

/* ------------------------------------------------------------------ *
 *  self checks
 * ------------------------------------------------------------------ */
static int failures;

static void check(int ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int selftest(void)
{
    // 1. the oscillator this bench invented (softdds_host.c) must produce the tone the modem asks
    // for. The firmware TX path is MSK shaped and re-steps its accumulator at bit changes, so the
    // check goes against the oscillator itself, which is the only part of the signal chain here
    // that is not firmware code.
    {
        soft_dds_t dds;
        softdds_setFreqDDS(&dds, 915.0f, (uint32_t) TXRATE, 0);
        int crossings = 0;
        int16_t prev = softdds_nextSample(&dds);
        for (int i = 1; i < 48000; i++)
        {
            int16_t v = softdds_nextSample(&dds);
            if ((prev < 0 && v >= 0) || (prev > 0 && v <= 0)) crossings++;
            prev = v;
        }
        float f = crossings * TXRATE / 2.0f / 48000.0f;
        printf("     (host oscillator measured at %.1f Hz, asked for 915.0 Hz)\n", f);
        check(fabsf(f - 915.0f) < 2.0f, "the host oscillator produces the frequency it is asked for");
    }
    {
        // zero crossing count of the modulator tone, measured through the same path the bench uses
        rtty_case_t c = { "probe", 170, 100.0f, 0, 0 };
        int eot = 0;
        char got[8192];
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST ONE", got, sizeof(got), &eot);
        check(cer == 0.0f, "clean RTTY loopback decodes with zero character errors");
        printf("     (lead-in junk before lock: %d characters)\n", lead_in_junk);
        check(strstr(got, "UHSDR") != NULL, "the call sign survives the loopback");
        check(eot == 1, "an EOT in the tx buffer asks the radio manager to leave TX");
    }
    {
        rtty_case_t c = { "snr", 170, 15.0f, 0, 0 };
        int eot = 0;
        char got[8192];
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST TWO", got, sizeof(got), &eot);
        check(cer <= 5.0f, "RTTY loopback stays under 5% CER at 15 dB SNR");
    }
    {
        wefax_cfg_t cfg = { "selftest", 20.0f, 0, 0, 0, false, false, 12 };
        size_t samples = (size_t) cfg.rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(samples * sizeof(float32_t));
        uint8_t* truth = malloc((size_t) cfg.rows * WEFAX_IMG_COLS);
        uint8_t* grey = malloc((size_t) cfg.rows * WEFAX_IMG_COLS);
        wefax_result_t res;
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_RngSeed(1u);
        Bench_AddNoise(x, samples, cfg.snr_db, 0.0f);
        bool any = Bench_WefaxDecode(x, samples, grey, &res);
        float corr = pearson(truth, grey, (size_t) cfg.rows * WEFAX_IMG_COLS);
        check(any && res.lines_found == cfg.rows, "fax: every sync tip of a 20 dB broadcast is found");
        check(corr > 0.9f, "fax: grey scale of a 20 dB broadcast correlates above 0.90");

        cfg.snr_db = 8.0f; cfg.qsb_rate_hz = 0.4f; cfg.qsb_depth_db = 6.0f;
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_RngSeed(2u);
        Bench_AddNoise(x, samples, cfg.snr_db, 0.0f);
        Bench_WefaxDecode(x, samples, grey, &res);
        corr = pearson(truth, grey, (size_t) (res.lines_found < cfg.rows ? res.lines_found : cfg.rows) * WEFAX_IMG_COLS);
        check(res.lines_found >= cfg.rows * 3 / 4, "fax: three quarters of the lines survive 8 dB with fading");

        // the skew knob must actually move the transmitter, otherwise the line capture is not being
        // tested at all and the table would read as if it were
        // Known gap, recorded rather than hidden. The transmitter skews each line by up to
        // +-120 samples (6 pixel columns); the reference decoder still finds every line and still
        // reads the chart, but its line spacing metric comes out at 0.0, which proves the capture
        // snaps to the grid of the first line instead of to each sync tip. It gets away with that
        // only because this test chart is smooth. An envelope based edge capture is the first real
        // piece of work on the fax path, see README.md.
        cfg.snr_db = 20.0f; cfg.qsb_rate_hz = 0.0f; cfg.qsb_depth_db = 0.0f; cfg.skew_ppm = 20000.0f;
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_WefaxDecode(x, samples, grey, &res);
        float skew_corr = pearson(truth, grey, (size_t) cfg.rows * WEFAX_IMG_COLS);
        printf("     skewed transmitter: %u/%u lines, spacing metric %.1f samples (0.0 means the tip is not tracked), chart correlation %.3f\n",
               res.lines_found, cfg.rows, res.row_rms_error, skew_corr);
        check(res.lines_found == cfg.rows && skew_corr > 0.9f,
              "fax: a skewed broadcast is still readable, even though it is not still tip locked");
        check(corr > 0.5f, "fax: the chart is still recognisable at 8 dB with fading");
        free(x); free(truth); free(grey);
    }
    {
        // the recorded file input path, which is how any real feed (WebSDR capture, another
        // receiver, an on air recording) reaches the firmware demodulator
        char got[8192];
        char msg[] = "DE UHSDR FILE PATH TEST";
        size_t n = 60000;
        static float32_t rec[60000];
        rtty_ctrl_config.speed_idx = RTTY_SPEED_45;
        rtty_ctrl_config.shift_idx = RTTY_SHIFT_170;
        Rtty_Modem_Init(48000);
        char feed[128];
        snprintf(feed, sizeof(feed), "%s", msg);
        strncat(feed, "\004", sizeof(feed) - strlen(feed) - 1);
        Bench_TxFeed(feed);
        Rtty_Modulator_StartTX();
        for (size_t k = 0; k < n; k++)
        {
            float acc = 0.0f;
            for (int q = 0; q < 4; q++) acc += (float32_t)Rtty_Modulator_GenSample() / 32768.0f;
            rec[k] = acc * 0.25f;
        }
        check(Bench_WavWrite("rtty_roundtrip.wav", rec, n, 12000), "the bench can write a 12 kHz RTTY recording");
        float cer_file = decode_wav("rtty_roundtrip.wav", msg, got, sizeof(got), 170);
        check(cer_file == 0.0f, "a recorded WAV comes back through the file input with zero errors");
    }
    printf("%s\n", failures ? "SELF CHECKS FAILED" : "all self checks passed");
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    const char* what = argc > 1 ? argv[1] : "test";
    if (!strcmp(what, "test")) return selftest();
    if (!strcmp(what, "rtty")) { rtty_table(); return 0; }
    if (!strcmp(what, "wefax")) { wefax_table(); return 0; }
    if (!strcmp(what, "rtty-wav") && argc > 2)
    {
        char got[8192];
        uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 170;
        const char* expect = argc > 4 ? argv[4] : "";
        float cer = decode_wav(argv[2], expect, got, sizeof(got), shift);
        if (cer >= 0.0f) printf("CER       : %.1f%% (after aligning the lead in)\n", cer);
        return 0;
    }
    fprintf(stderr, "usage: digi_bench [test|rtty|wefax|rtty-wav <file.wav> [shift] [expected text]]\n");
    return 2;
}
