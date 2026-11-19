/* UHSDR digital mode host bench.
 *
 *   make test    self checks, non zero exit on regression
 *   make rtty    character error rate of the shipping RTTY modem, loopback
 *   make wefax   weather fax: synthetic broadcast, reference demodulator, scores and files
 *   make rtty-tune  tuning error the RTTY demodulator survives, in Hz and in ppm per band
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
    float       mark_db;     // how much weaker the mark tone arrives than the space tone, 0 = equal
    float       adj_hz;      // an unmodulated carrier this far above the space tone, 0 = none
    float       adj_db;      // its level relative to the wanted signal, 0 = none
    float       qsb_rate;
    float       qsb_depth;
    uint16_t    baud;        // 0 = 45.45, the rate the modem was designed around; 50 = the DWD
                             // weather teleprinter stations, which are all F1B 50 Baud
    float       max_cer;     // the character error rate this case has to stay within, in percent
    float       ppm;         // receiver and transmitter reference error, as a tuning error at rf_hz
    float       rf_hz;       // the RF frequency ppm is measured at; a ppm is not a fixed number of
                             // audio Hz, it is ppm * rf_hz of them: 1 ppm is 3.5 Hz on 80 m,
                             // 14 Hz on 20 m, 28 Hz on 10 m and 50 Hz on 6 m
} rtty_case_t;

// The demodulator emits one or two characters of junk while it locks on, which is a property of
// the modem but not what the CER is meant to measure, so the score is taken from the best
// alignment of the received text against the expected one and the skipped lead-in is reported.
static int lead_in_junk;

// A band pass at freq with the constant peak gain form of the RBJ cookbook, so |H(freq)| is 1 and
// subtracting g of its output attenuates a tone sitting exactly there by exactly g, in the audio
// the modulator produced rather than in a re-implementation of it.
static void weaken_tone(float32_t* x, size_t n, float freq, float width, float db)
{
    float w0 = (float) (2.0 * M_PI * freq / RATE);
    float alpha = sinf(w0) / (2.0f * (freq / width));
    float a0 = 1.0f + alpha;
    float b0 = alpha / a0, b2 = -alpha / a0;
    float a1 = -2.0f * cosf(w0) / a0, a2 = (1.0f - alpha) / a0;
    float g = 1.0f - powf(10.0f, -db / 20.0f);
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
    for (size_t i = 0; i < n; i++)
    {
        float bp = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x[i];
        y2 = y1; y1 = bp;
        x[i] -= g * bp;
    }
}

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
    // a reference error slides mark and space together: the shift a transmitter sends does not
    // change, the pair just lands somewhere else in the receiver's filter bank
    const float tune = c->ppm * 1e-6f * c->rf_hz;

    rtty_ctrl_config.speed_idx = c->baud == 50 ? RTTY_SPEED_50 : RTTY_SPEED_45;

    for (int i = 0; i < RTTY_SHIFT_NUM; i++)
    {
        if (rtty_shifts[i].value == (float32_t) c->shift) rtty_ctrl_config.shift_idx = (rtty_shift_t) i;
    }

    // TX: the firmware modulator produces the waveform, EOT ends it. The receiver starts from a
    // clean slate, so no case is scored on what the one before it left behind.
    Rtty_BenchResetDecoder();
    Rtty_Modem_Init((uint32_t) TXRATE);
    Rtty_BenchOffsetTx(tune, (uint32_t) TXRATE);
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

    // a station whose mark tone is the weak one: take the 915 Hz tone's own 20 Hz wide band out of
    // the transmit audio. Its peak gain is exactly 1, so the mark tone comes out c->mark_db weaker
    // and the space tone, one shift away, loses half a dB (94 % of its level at 170 Hz shift).
    if (c->mark_db > 0.0f) weaken_tone(buf, n, 915.0f + tune, 20.0f, c->mark_db);

    // an unmodulated carrier next to the space tone. The space filter has to reject it: if it does
    // not, the ATC sees that filter loud all the time and cannot tell a space bit from the carrier.
    if (c->adj_db != 0.0f)
    {
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) sum += (double) buf[i] * buf[i];
        float amp = (float) sqrt(sum / (double) n) * powf(10.0f, c->adj_db / 20.0f) * 1.41421356f;
        float w = (float) (2.0 * M_PI * (915.0 + tune + c->shift + c->adj_hz) / RATE);
        for (size_t i = 0; i < n; i++) buf[i] += amp * sinf(w * (float) i);
    }

    // channel, at the rate the demodulator sees
    Bench_AddQsb(buf, n, c->qsb_rate, c->qsb_depth);
    Bench_RngSeed(0xC0FFEEu);
    Bench_AddNoise(buf, n, c->snr_db, 0.0f);

    // RX: the firmware demodulator, characters collected by the UI stand-in
    for (size_t i = 0; i < n; i++)
    {
        Rtty_Demodulator_ProcessSample(buf[i]);
    }
    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    return score_aligned(norm_baudot(msg, 0), got);
}

static int rtty_table(void)
{
    const char* msg = "DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT";
    // The .max_cer field is the character error rate each case has to stay within. All but the fast
    // fading case decode without a single wrong character, so a few characters of slack leave room
    // for libm and rounding differences between build hosts while still failing on a demodulator
    // change that costs text. The fast fading case loses most of the message, so its limit only
    // catches it getting worse.
    static const rtty_case_t cases[] =
    {
        // the space filter under a strong adjacent carrier: an unmodulated carrier 300 Hz above the
        // space tone at +20 dB over the wanted signal. The carrier alone grades nothing - with equal
        // tones this decodes at 0.0 % even with the 850 Hz space filter, because the mark channel
        // decides - so the row also sends the mark tone 30 dB down, the condition that puts the bits
        // on the space filter. Together they fail where the space filter's rejection does: 0.0 % with
        // the 170 Hz filter, 100 % with the 850 Hz one, and 18.8 % with the carrier raised to +30 dB.
        // That level is the dynamic range the README calls the weak point, and it moves with how well
        // the space filter rejects a carrier 300 Hz outside its passband.
        { .label = "wmark car300", .shift = 170, .snr_db = 20.0f, .mark_db = 30.0f,
          .adj_hz = 300.0f, .adj_db = 20.0f, .max_cer = 5.0f },
        { .label = "clean 170",    .shift = 170, .snr_db = 100.0f, .max_cer = 5.0f },
        { .label = "snr 20 170",   .shift = 170, .snr_db = 20.0f,  .max_cer = 5.0f },
        { .label = "snr 10 170",   .shift = 170, .snr_db = 10.0f,  .max_cer = 5.0f },
        { .label = "snr 10 425",   .shift = 425, .snr_db = 10.0f,  .max_cer = 5.0f },
        { .label = "snr 10 850",   .shift = 850, .snr_db = 10.0f,  .max_cer = 5.0f },
        { .label = "snr 7 170",    .shift = 170, .snr_db = 7.0f,   .max_cer = 5.0f },
        { .label = "qsb 10 170",   .shift = 170, .snr_db = 10.0f, .qsb_rate = 0.5f,
          .qsb_depth = 6.0f, .max_cer = 5.0f },
        { .label = "qsb fast 10",  .shift = 170, .snr_db = 10.0f, .qsb_rate = 2.0f,
          .qsb_depth = 10.0f, .max_cer = 70.0f },
        // the real services: DWD DDH/DDK are F1B 50 Baud with +/-225 Hz (450 Hz total) on
        // 4583/7646/10100.8/11039/14467.3 kHz and +/-42.5 Hz (85 Hz) on 147.3 kHz, per the DWD
        // table on http://weather.mailasail.com/franks-weather/radio-teletype-weather-broadcasts.
        // The space filter is only graded by a case where the mark channel cannot decide. Weakening
        // the space tone does not do it: the 915 Hz filter still reads mark at 0 dB and space 22.9 dB
        // down (the README figure for this shift), so the mark channel keeps the right polarity and
        // the row decodes with any space filter at all - measured, 0.0 % with the 850 Hz filter too.
        // Weakening the MARK tone past that 22.9 dB inverts the mark channel, which then reads the
        // space tone 22.9 dB down as louder than the mark tone 30 dB down, and the decision has to
        // come from the space filter: this row decodes at 0.0 % with the 170 Hz filter and at 100 %
        // with the 850 Hz one. It is the only row in the table that grades the shift.
        { .label = "weak mark 170", .shift = 170, .snr_db = 20.0f, .mark_db = 30.0f,
          .max_cer = 5.0f },
        { .label = "dwd 450 50bd", .shift = 450, .snr_db = 20.0f, .baud = 50, .max_cer = 5.0f },
        { .label = "dwd 450 10bd", .shift = 450, .snr_db = 10.0f, .baud = 50, .max_cer = 5.0f },
        { .label = "ddh47 85 50bd", .shift = 85, .snr_db = 20.0f, .baud = 50, .max_cer = 5.0f },
        // a reference error, not a shift error: both tones slide by the same number of Hz, which
        // is 14 Hz per ppm on 20 m, 28 on 10 m and 50 on 6 m. The modem has no AFC, so these rows
        // say how much of it the fixed filters survive - measured, 170 Hz shift with equal tones
        // breaks between 70 and 85 Hz off, so 1..3 ppm on 20 m and 1 ppm on 6 m are inside, while
        // 2.5 ppm on 6 m (125 Hz) is not. `digi_bench rtty-tune` prints the whole curve.
        { .label = "20m 1ppm",     .shift = 170, .snr_db = 20.0f, .max_cer = 5.0f,
          .ppm = 1.0f, .rf_hz = 14e6f },
        { .label = "20m 3ppm",     .shift = 170, .snr_db = 20.0f, .max_cer = 5.0f,
          .ppm = 3.0f, .rf_hz = 14e6f },
        { .label = "10m 2ppm",     .shift = 170, .snr_db = 20.0f, .max_cer = 5.0f,
          .ppm = 2.0f, .rf_hz = 28e6f },
        { .label = "50m 1ppm",     .shift = 170, .snr_db = 20.0f, .max_cer = 5.0f,
          .ppm = 1.0f, .rf_hz = 50e6f },
        { .label = "dwd 450 1ppm", .shift = 450, .snr_db = 20.0f, .baud = 50, .max_cer = 5.0f,
          .ppm = 1.0f, .rf_hz = 14e6f },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);

    printf("RTTY loopback of drivers/audio/rtty.c (per case: 45.45 or 50 Bd, 1.5 stop bits, tx 48 kHz -> rx 12 kHz)\n");
    printf("%-16s %7s %6s  %s\n", "case", "CER %", "lead", "decoded");
    char got[8192];
    int failures = 0;
    for (size_t i = 0; i < n_cases; i++)
    {
        int eot = 0;
        float cer = rtty_run(&cases[i], msg, got, sizeof(got), &eot);
        printf("%-16s %7.1f %6d  %s%s\n", cases[i].label, cer, lead_in_junk, got,
               eot ? "" : "   [transmission cut off before EOT]");
        if (cer > cases[i].max_cer)
        {
            printf("FAIL  %-16s character error rate %.1f%% is above the %.1f%% this case allows\n",
                   cases[i].label, cer, cases[i].max_cer);
            failures++;
        }
    }
    printf("\nnote: these loopback CER figures cannot grade the selected shift. The mark filter\n"
           "      (915 Hz, 3 dB band 865-965 Hz) rejects the space tone by 23 dB at 170 Hz shift and\n"
           "      by more at every wider shift, but by only 10 dB at 85 Hz, so on a clean two-tone\n"
           "      synthetic recording the ATC decides from the mark channel alone whatever shift is\n"
           "      selected - except in the weak mark row, where the mark channel is inverted and the\n"
           "      space filter has to carry the bits (0.0 %% with the right one, 100 %% with 850 Hz).\n"
           "      On live radio that is not true: a DDH 8 capture decodes only at 425/450 Hz\n"
           "      shift and fails at 85/170/200/850. See support/digi/README.md. An unsupported shift\n"
           "      is still mapped to 170.\n");
    if (failures == 0)
    {
        printf("\nall %zu cases within their character error rate limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  how much tuning error the modem survives
 * ------------------------------------------------------------------ */
// A ppm figure says nothing in the audio domain until it is multiplied by the RF frequency, and
// the receiver has no automatic frequency control: the mark filter is nailed to 915 Hz and the
// space filter to 915 + shift. So the pair has to be steered onto those filters by hand, and the
// useful question is how many Hz of error it takes before the text goes. This walks the pair off
// the filters and prints the ppm that produces each offset on the bands the radio covers.
static int rtty_tune(void)
{
    const char* msg = "DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT";
    static const float offsets[] = { -250.0f, -200.0f, -140.0f, -100.0f, -85.0f, -70.0f, -50.0f,
                                     -28.0f, -14.0f, 0.0f, 14.0f, 28.0f, 50.0f, 70.0f, 85.0f,
                                     100.0f, 140.0f, 200.0f, 250.0f };
    static const float bands[]   = { 3.5e6f, 14e6f, 28e6f, 50e6f };
    const size_t n_off = sizeof(offsets) / sizeof(offsets[0]);
    const size_t n_band = sizeof(bands) / sizeof(bands[0]);
    char got[8192];
    int failures = 0;

    printf("RTTY tuning error tolerance: the modem has no AFC, so an error is not pulled in,\n"
           "it just walks both tones away from the fixed filters. Offsets marked * are asserted.\n\n");
    printf("%7s", "off Hz");
    for (size_t b = 0; b < n_band; b++)
    {
        printf(" %8s", b == 0 ? "ppm 3.5" : b == 1 ? "ppm 14" : b == 2 ? "ppm 28" : "ppm 50");
    }
    printf(" %11s %11s %11s\n", "CER 170", "170 wmark", "CER 450");

    // the clean window this bench promises, chosen well inside the measured edges: 170 Hz shift
    // with equal tones breaks between 70 and 85 Hz off, the 30 dB weak mark row between 28 and 50,
    // and 450 Hz shift at 50 Bd between 100 and 140. Pinning an edge itself would let a libm
    // difference between build hosts look like a demodulator change.
    static const float floor_hz[3] = { 50.0f, 28.0f, 100.0f };
    static const char* const floor_what[3] =
    {
        "170 Hz shift, equal tones",
        "170 Hz shift with the mark tone 30 dB down",
        "450 Hz shift at 50 Bd",
    };

    // the three curves, written once: they differ in shift and in whether the mark channel can
    // decide, and every offset runs all three with only the reference error changing
    static const rtty_case_t curve[3] =
    {
        { .label = "t", .shift = 170, .snr_db = 20.0f, .max_cer = 100.0f, .rf_hz = 14e6f },
        { .label = "t", .shift = 170, .snr_db = 20.0f, .mark_db = 30.0f, .max_cer = 100.0f,
          .rf_hz = 14e6f },
        { .label = "t", .shift = 450, .snr_db = 20.0f, .baud = 50, .max_cer = 100.0f,
          .rf_hz = 14e6f },
    };

    for (size_t i = 0; i < n_off; i++)
    {
        // the sign matters: the mark tone sits at the bottom of the pair, so moving up consumes
        // the space filter's passband and moving down the mark filter's
        printf("%+7.0f", (double) offsets[i]);
        for (size_t b = 0; b < n_band; b++)
        {
            printf(" %8.1f", (double) (offsets[i] / bands[b] * 1e6f));
        }
        rtty_case_t cases[3];
        for (int k = 0; k < 3; k++)
        {
            cases[k] = curve[k];
            cases[k].ppm = offsets[i] / 14e6f * 1e6f;      // the walk is at 20 m throughout
        }
        float cer[3];
        bool pinned[3];
        for (int k = 0; k < 3; k++)
        {
            int eot = 0;
            cer[k] = rtty_run(&cases[k], msg, got, sizeof(got), &eot);
            pinned[k] = fabsf(fabsf(offsets[i]) - floor_hz[k]) < 0.01f;
            printf(" %10.1f%s", (double) cer[k], pinned[k] ? "*" : " ");
        }
        printf("\n");
        for (int k = 0; k < 3; k++)
        {
            if (pinned[k] && cer[k] > 0.0f)
            {
                printf("FAIL  %+.0f Hz, %s: %.1f %% character errors where this bench requires 0.0\n",
                       (double) offsets[i], floor_what[k], (double) cer[k]);
                failures++;
            }
        }
    }
    // the first table is a clean bench signal. A station that is a ppm off is also usually a weak
    // one, and the filter skirts that decide the tolerance are exactly where the noise is, so the
    // same walk is repeated at 10 and 7 dB and on top of 0.5 Hz / 6 dB QSB.
    printf("\nthe same error on a weak signal (170 Hz shift, equal tones)\n\n");
    printf("%7s %11s %11s %11s\n", "off Hz", "10 dB", "7 dB", "10+QSB");
    static const float weak_offsets[] = { 0.0f, 14.0f, 28.0f, 42.0f, 56.0f, 70.0f };
    static const rtty_case_t weak_curve[3] =
    {
        { .label = "t", .shift = 170, .snr_db = 10.0f, .max_cer = 100.0f, .rf_hz = 14e6f },
        { .label = "t", .shift = 170, .snr_db = 7.0f, .max_cer = 100.0f, .rf_hz = 14e6f },
        { .label = "t", .shift = 170, .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 6.0f,
          .max_cer = 100.0f, .rf_hz = 14e6f },
    };
    for (size_t i = 0; i < sizeof(weak_offsets) / sizeof(weak_offsets[0]); i++)
    {
        const float ppm14 = weak_offsets[i] / 14e6f * 1e6f;
        rtty_case_t weak[3];
        for (int k = 0; k < 3; k++)
        {
            weak[k] = weak_curve[k];
            weak[k].ppm = ppm14;
        }
        printf("%+7.0f", (double) weak_offsets[i]);
        float cer_weak[3];
        for (int k = 0; k < 3; k++)
        {
            int eot = 0;
            cer_weak[k] = rtty_run(&weak[k], msg, got, sizeof(got), &eot);
            printf(" %10.1f ", (double) cer_weak[k]);
        }
        printf("\n");
        // 56 Hz (4 ppm at 14 MHz) is inside the window at 7 dB as well: the tolerance is set by the
        // filters, not by the noise, so this fails only if a change makes the modem noise limited
        if (fabsf(weak_offsets[i] - 56.0f) < 0.01f
            && (cer_weak[0] > 0.0f || cer_weak[1] > 0.0f || cer_weak[2] > 0.0f))
        {
            printf("FAIL  +56 Hz: %.1f/%.1f/%.1f %% at 10 dB, 7 dB and with QSB, the bench requires 0.0\n",
                   (double) cer_weak[0], (double) cer_weak[1], (double) cer_weak[2]);
            failures++;
        }
    }

    printf("\noffsets are Hz of audio error; the ppm columns are that same error on other bands.\n"
           "1 ppm is 3.5 Hz on 80 m, 14 Hz on 20 m, 28 Hz on 10 m and 50 Hz on 6 m, and the receiver\n"
           "cannot pull any of it in - only the operator's tuning can. What our own reference is\n"
           "worth behind that:\n"
           "  Si570   warm, with soft_tcxo on and the radio nulled against WWV, the residual is\n"
           "          about +-10 Hz at 14 MHz (+-0.7 ppm, so about 20 Hz on 10 m and 36 Hz on 6 m).\n"
           "          From cold it is not: the table is referenced to 43 C and only holds once the\n"
           "          sensor and the Si570 are thermally bonded, so during warm up the correction\n"
           "          lags the crystal. Near room temperature the curve runs about 5 Hz per C at\n"
           "          14 MHz, i.e. a third of the 170 Hz window for every 6 C of lag.\n"
           "  Si5351A no temperature correction at all: Si5351a_PrepareNextFrequency discards the\n"
           "          temperature factor, so the 27 MHz crystal's drift lands on the audio pair.\n"
           "  freq_cal one calibration number in 0.1 ppm steps; it cannot follow drift.\n");
    if (failures == 0)
    {
        printf("the pinned offsets decode clean\n");
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  weather fax
 * ------------------------------------------------------------------ */
// one synthesised broadcast plus what it has to score. The correlation window is a range rather
// than a floor because the bands differ: eight of the nine recover the picture upright at
// 0.972..0.975, while the swapped polarity case is inverted by design and lands at -0.945.
typedef struct
{
    const char* name;
    float    snr_db;
    float    qsb_rate_hz;
    float    qsb_depth_db;
    float    skew_ppm;
    bool     invert;
    bool     steer_right;
    uint32_t rows;
    float    corr_min;
    float    corr_max;
    float    ppm;             // every tone moved by ppm * rf_hz, the same rigid reference error the
    float    rf_hz;           // RTTY rows carry: 14 Hz per ppm on 20 m, 50 Hz on 6 m
    float    mean_min;        // the decoded grey mean the case has to land in; equal bounds = not
    float    mean_max;        // pinned, which is every row whose tones are where the bins are
} wefax_case_t;

static int wefax_table(void)
{
    // .corr_min and .corr_max are the correlation window each case has to land in. 0.95 leaves
    // room for libm and rounding differences between build hosts but not for a demodulator change
    // that costs picture; the swapped case says the polarity is inverted, which is the behaviour the
    // printout below documents, so a fix that silently flips it is a change and has to say so here.
    //
    // .mean_min and .mean_max are the decoded grey mean, and only the offset rows pin it: Pearson
    // divides the constant grey bias an offset causes out of the correlation, so those rows would
    // pass at any offset at all if the correlation were all they had to score. Their windows sit
    // around the level each one really decodes (199.5 with the tones where the bins are, and the ppm
    // numbers in the printout for the rest), which is twice as far from the clean level as the
    // measured spread between hosts, and far inside what a 250 Hz error decodes to (183.4).
    static const wefax_case_t cases[] =
    {
        { .name = "clean", .snr_db = 100.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 20", .snr_db = 20.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 12", .snr_db = 12.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 12 fast qsb", .snr_db = 12.0f, .qsb_rate_hz = 2.0f, .qsb_depth_db = 10.0f,
          .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 12 + skew", .snr_db = 12.0f, .skew_ppm = 20000, .rows = 12,
          .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 12 black/white swapped", .snr_db = 12.0f, .invert = true, .rows = 12,
          .corr_min = -1.00f, .corr_max = -0.90f },
        { .name = "snr 12 reversed scan", .snr_db = 12.0f, .steer_right = true, .rows = 12,
          .corr_min = 0.95f, .corr_max = 1.00f },
        { .name = "snr 8 qsb skew", .snr_db = 8.0f, .qsb_rate_hz = 0.4f, .qsb_depth_db = 6.0f,
          .skew_ppm = 3000, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f },
        // the same rigid reference error the RTTY rows carry: every tone of the emission moves by
        // ppm * rf_hz while the demodulator's bins stay at 1857/1500/2300 Hz. The sync tip is the
        // narrow one, so a tuning error attacks the line capture before it attacks the picture, and
        // the grey level it decodes to is what these rows pin.
        { .name = "20m 1ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f,
          .ppm = 1.0f, .rf_hz = 14e6f, .mean_min = 200.5f, .mean_max = 203.5f },
        { .name = "20m 5ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f,
          .ppm = 5.0f, .rf_hz = 14e6f, .mean_min = 206.0f, .mean_max = 210.0f },
        { .name = "10m 2ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f,
          .ppm = 2.0f, .rf_hz = 28e6f, .mean_min = 205.0f, .mean_max = 209.0f },
        { .name = "50m 1ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.95f, .corr_max = 1.00f,
          .ppm = 1.0f, .rf_hz = 50e6f, .mean_min = 204.5f, .mean_max = 208.0f },
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("HF weather fax, 120 lines/min at 12 kHz, sync 1857 Hz, black 1500 / white 2300 Hz\n");
    printf("%-28s %6s %8s %8s %8s %7s\n", "case", "lines", "skewRMS", "corr", "mean", "off Hz");
    for (size_t i = 0; i < n; i++)
    {
        size_t samples = (size_t) cases[i].rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(samples * sizeof(float32_t));
        uint8_t* truth = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        uint8_t* grey = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        wefax_result_t res;
        const float tune_hz = cases[i].ppm * 1e-6f * cases[i].rf_hz;
        wefax_cfg_t cfg = { .name = cases[i].name, .snr_db = cases[i].snr_db,
                            .qsb_rate_hz = cases[i].qsb_rate_hz,
                            .qsb_depth_db = cases[i].qsb_depth_db,
                            .skew_ppm = cases[i].skew_ppm, .invert = cases[i].invert,
                            .steer_right = cases[i].steer_right, .rows = cases[i].rows,
                            .tune_hz = tune_hz };

        Bench_WefaxGenerate(x, truth, &cfg);
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

        printf("%-28s %3u/%-2u %8.1f %8.3f %8.1f %7.1f\n", cases[i].name,
               res.lines_found, res.lines_expected, res.row_rms_error, corr, res.mean_level,
               (double) tune_hz);
        if (res.lines_found < cases[i].rows)
        {
            printf("FAIL  %-28s found %u of the %u lines it transmitted\n",
                   cases[i].name, res.lines_found, cases[i].rows);
            failures++;
        }
        if (corr < cases[i].corr_min || corr > cases[i].corr_max)
        {
            printf("FAIL  %-28s correlation %.3f is outside %.2f..%.2f for this case\n",
                   cases[i].name, corr, cases[i].corr_min, cases[i].corr_max);
            failures++;
        }
        if (cases[i].mean_max > cases[i].mean_min &&
            (res.mean_level < cases[i].mean_min || res.mean_level > cases[i].mean_max))
        {
            printf("FAIL  %-28s decoded grey mean %.1f is outside %.1f..%.1f for this case\n",
                   cases[i].name, res.mean_level, cases[i].mean_min, cases[i].mean_max);
            failures++;
        }

        if (i == 1)                     // leave one readable broadcast behind for inspection
        {
            Bench_WavWrite("wefax_snr20.wav", x, samples, (uint32_t) RATE);
            pgm_write("wefax_truth.pgm", truth, WEFAX_IMG_COLS, cases[i].rows);
            pgm_write("wefax_decoded.pgm", grey, WEFAX_IMG_COLS, res.lines_found ? res.lines_found : cases[i].rows);
        }
        free(x); free(truth); free(grey);
    }
    printf("\nThe offset rows move every tone of the emission while the bins stay where they are. Their\n"
           "correlation barely changes because Pearson removes the constant grey bias an offset\n"
           "causes, so those rows pin the mean and not only the correlation: 199.5 clean against\n"
           "208.2 at 70 Hz, about 3 %% of the 800 Hz tone spacing, and 183.4 at 250 Hz off (5 ppm on\n"
           "6 m), where two of twelve sync tips are no longer found and the picture is gone at\n"
           "correlation 0.339. That case is not a row here because it would have to pass before it\n"
           "could be pinned, but every pinned window is far inside it.\n"
           "\nBlack/white swapped gives a correlation of about -0.95: the demodulator recovers the\n"
           "picture but with the polarity upside down, which is why a station parameter is needed.\n"
           "The reversed scan case scores normally by construction: the reference decoder has no\n"
           "phasing reference, so it reproduces whatever was scanned, which is exactly what a real\n"
           "operator fixes with the steer control.\n");
    printf("files: wefax_snr20.wav (listen), wefax_truth.pgm, wefax_decoded.pgm\n");
    if (failures == 0)
    {
        printf("\nall %zu cases recovered every line inside their correlation window\n", n);
    }
    return failures ? 1 : 0;
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
static float decode_wav(const char* path, const char* expected, char* got, size_t got_sz, uint16_t shift, uint16_t baud)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return -1.0f; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); printf("short read\n"); return -1.0f; }
    fclose(f);

    uint32_t rate = 0, ch = 1, bits = 0, tag = 0, i = 12; const uint8_t* data = 0; uint32_t dlen = 0;
    while (i + 8 <= (size_t)sz)
    {
        uint32_t clen = rd32(buf + i + 4);
        if (clen > (size_t)sz - i - 8) break;       /* a chunk cannot claim more than the file holds */
        if (!memcmp(buf + i, "fmt ", 4))
        {
            if (clen < 16) { free(buf); printf("the fmt chunk of %s is too short to describe a PCM stream\n", path); return -1.0f; }
            tag  = rd16(buf + i + 8);               /* 1 = PCM, 3 = float, 0xfffe = extensible */
            ch   = rd16(buf + i + 10);
            rate = rd32(buf + i + 12);
            bits = rd16(buf + i + 22);
        }
        else if (!memcmp(buf + i, "data", 4)) { data = buf + i + 8; dlen = clen; }
        else break;
        i += 8 + clen + (clen & 1);
    }
    if (!data || !rate || !dlen) { free(buf); printf("not a PCM WAV with a data chunk\n"); return -1.0f; }
    if (tag != 1 || bits != 16 || !ch)
    {
        /* an 8/24/32 bit or float export would be decoded as noise here, and the CER would blame
           the modem for a file this reader cannot read */
        free(buf);
        printf("%s: format %u, %u bit, %u channel(s); this reader decodes 16 bit PCM only\n",
               path, tag, bits, ch);
        return -1.0f;
    }

    size_t frames = (size_t)dlen / (2 * ch);
    if (!frames) { free(buf); printf("%s: the data chunk holds no complete frame\n", path); return -1.0f; }
    float peak = 0.0f;
    for (size_t k = 0; k < frames; k++)
    {
        float v = (float)(int16_t)rd16(data + k * 2 * ch) / 32768.0f;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    float gain = peak > 0.0f ? 0.7f / peak : 1.0f;

    /* the firmware silently maps an unsupported shift to 170 and only knows 45.45 and 50 baud,
       which against a real station looks exactly like a modem that cannot decode: refuse the
       combination here instead of reporting the modem for a harness mistake */
    int shift_ok = 0;
    for (int k = 0; k < RTTY_SHIFT_NUM; k++)
    {
        if (rtty_shifts[k].value == (float32_t)shift) shift_ok = 1;
    }
    if (!shift_ok)
    {
        printf("%u Hz shift is not in the modem's filter bank (85/170/200/425/450/850), Rtty_Modem_Init() "
               "would silently decode it as 170\n", (unsigned)shift);
        return -1.0f;
    }
    if (baud != 45 && baud != 50)
    {
        printf("%u baud is not supported, rtty_speeds[] only holds 45.45 and 50\n", (unsigned)baud);
        return -1.0f;
    }
    rtty_ctrl_config.speed_idx = baud == 50 ? RTTY_SPEED_50 : RTTY_SPEED_45;
    rtty_ctrl_config.shift_idx = RTTY_SHIFT_170;
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
        if (a >= frames) a = frames - 1;
        size_t b = (a + 1 < frames) ? a + 1 : a;    /* the last frame reads itself, never past the data */
        double fr = (b == a) ? 0.0 : src - (double)a;
        float s0 = (float)(int16_t)rd16(data + a * 2 * ch) / 32768.0f;
        float s1 = (float)(int16_t)rd16(data + b * 2 * ch) / 32768.0f;
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

// one -3 dB crossing of a receive filter, walking outwards from a point inside its passband. The
// top of these filters is flat to about 1e-5, so float32 arithmetic dithers the peak itself by a
// few Hz; the half power edges are steep and locate the centre to a fraction of a Hz.
static float filter_half_power_edge(unsigned idx, float from, float step, float half)
{
    float f = from;
    for (int k = 0; k < 400 && Rtty_BenchFilterMagnitude(idx, f + step) > half; k++)
    {
        f += step;
    }
    const float a = Rtty_BenchFilterMagnitude(idx, f);          // above half power
    const float b = Rtty_BenchFilterMagnitude(idx, f + step);   // at or below it
    return (a > b) ? f + step * (a - half) / (a - b) : f;
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
        rtty_case_t c = { .label = "probe", .shift = 170, .snr_db = 100.0f, .max_cer = 5.0f };
        int eot = 0;
        char got[8192];
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST ONE", got, sizeof(got), &eot);
        check(cer == 0.0f, "clean RTTY loopback decodes with zero character errors");
        printf("     (lead-in junk before lock: %d characters)\n", lead_in_junk);
        check(strstr(got, "UHSDR") != NULL, "the call sign survives the loopback");
        check(eot == 1, "an EOT in the tx buffer asks the radio manager to leave TX");
    }
    {
        rtty_case_t c = { .label = "snr", .shift = 170, .snr_db = 15.0f, .max_cer = 5.0f };
        int eot = 0;
        char got[8192];
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST TWO", got, sizeof(got), &eot);
        check(cer <= 5.0f, "RTTY loopback stays under 5% CER at 15 dB SNR");
    }
    {
        wefax_cfg_t cfg = { .name = "selftest", .snr_db = 20.0f, .rows = 12 };
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
        float cer_file = decode_wav("rtty_roundtrip.wav", msg, got, sizeof(got), 170, 45);
        check(cer_file == 0.0f, "a recorded WAV comes back through the file input with zero errors");
    }
    {
        // the receive filter bank has to be tuned to the frequency its own label claims and the
        // modulator sends. rtty_bp_12khz_1765 was an 1865 Hz design and the loopback never noticed,
        // because the ATC decides from the mark channel on a clean two-tone signal; this measures
        // each filter through its own recursion instead of decoding with it.
        const unsigned n = Rtty_BenchFilterCount();
        const float mark_hz = (float)Rtty_BenchFilterLabel(0);
        check(n == 1 + RTTY_SHIFT_NUM, "the filter bank holds a mark filter plus one space filter per shift");
        for (unsigned i = 0; i < n; i++)
        {
            const float label = (float)Rtty_BenchFilterLabel(i);
            float peak = 0.0f, peak_hz = label;
            for (float hz = label - 150.0f; hz <= label + 150.0f; hz += 1.0f)
            {
                const float m = Rtty_BenchFilterMagnitude(i, hz);
                if (m > peak) { peak = m; peak_hz = hz; }
            }
            const float half = peak * 0.70710678f;
            const float lo = filter_half_power_edge(i, peak_hz, -1.0f, half);
            const float hi = filter_half_power_edge(i, peak_hz, +1.0f, half);
            const float centre = (lo + hi) * 0.5f;
            char what[176];
            if (i == 0)
            {
                snprintf(what, sizeof(what),
                         "mark filter: -3 dB %.0f..%.0f Hz, centre %.0f Hz, label %.0f Hz",
                         lo, hi, centre, label);
            }
            else
            {
                snprintf(what, sizeof(what),
                         "%u Hz shift space filter: -3 dB %.0f..%.0f Hz, centre %.0f Hz, label %.0f Hz",
                         (unsigned)rtty_shifts[i - 1].value, lo, hi, centre, label);
            }
            // these biquads are designed so the middle of the half power band is the labelled
            // frequency to within 0.1 Hz; the filter this check exists for was 100 Hz away
            check(fabsf(centre - label) <= 2.0f, what);

            // the label is not independent of the filter: it is the .freq the modulator tunes its
            // DDS to, so comparing the two only ties the receiver to the transmitter. A space
            // filter has to be the mark plus the shift, which is what catches a label and a design
            // that were moved together.
            if (i != 0)
            {
                const float shift = (float)rtty_shifts[i - 1].value;
                snprintf(what, sizeof(what),
                         "%u Hz shift space filter: label %.0f Hz is the %.0f Hz mark plus the shift",
                         (unsigned)shift, label, mark_hz);
                check(fabsf(label - (mark_hz + shift)) <= 0.5f, what);
            }
        }
    }
    printf("%s\n", failures ? "SELF CHECKS FAILED" : "all self checks passed");
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    const char* what = argc > 1 ? argv[1] : "test";
    if (!strcmp(what, "test")) return selftest();
    if (!strcmp(what, "rtty")) { return rtty_table(); }
    if (!strcmp(what, "rtty-tune")) { return rtty_tune(); }
    if (!strcmp(what, "wefax")) return wefax_table();
    if (!strcmp(what, "rtty-wav") && argc > 2)
    {
        char got[8192];
        uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 170;
        const char* expect = argc > 4 ? argv[4] : "";
        uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 45;
        printf("decoding with %u Hz shift, %u baud\n", (unsigned)shift, (unsigned)baud);
        float cer = decode_wav(argv[2], expect, got, sizeof(got), shift, baud);
        if (cer >= 0.0f) printf("CER       : %.1f%% (after aligning the lead in)\n", cer);
        return 0;
    }
    fprintf(stderr, "usage: digi_bench [test|rtty|rtty-tune|wefax|rtty-wav <file.wav> [shift] [expected text] [45|50 baud]]\n");
    return 2;
}
