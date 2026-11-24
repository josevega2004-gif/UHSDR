/* UHSDR digital mode host bench.
 *
 *   make test    self checks, non zero exit on regression
 *   make rtty    character error rate of the shipping RTTY modem, loopback
 *   make wefax   weather fax: synthetic broadcast, reference demodulator, scores and files
 *   make rtty-tune  tuning error the RTTY demodulator survives, in Hz and in ppm per band
 *
 * The RTTY numbers come from drivers/audio/rtty.c itself: the firmware modulator generates the
 * signal, the firmware demodulator reads it back, and the firmware's audio AGC sits between the
 * channel and the demodulator, where the radio puts it. Only the channel is invented here. See
 * README.md for what is stubbed and why that still counts.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "hellschreiber.h"
#include "mfsk16.h"
#include "navtex.h"
#include "olivia.h"
#include "sitor_rx.h"
#include "rtty.h"
#include "softdds.h"
#include "audio_agc.h"
#include "audio_driver.h"   // POST_AGC_GAIN_SCALING_DECIMATE_4, the gain after the AGC

#define RATE            12000.0f          // Rtty_Modem_Init() pins the demodulator at 12 kHz
#define TXRATE          48000.0f          // the modulator is fed at 4x that: one bit is
                                          // oneBitSampleCount * 4 calls to GenSample()
#define MAX_SAMP        1200000           // room for the long message mode: 424 characters, 45 Bd
// the text every matrix row sends. rtty-long sends eight copies of it: see rtty_table()
#define RTTY_TABLE_MSG  "DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT"


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

// P5, one byte per pixel. Returns 0 when the file could not be opened or the close failed: a disk
// that filled up must not leave a truncated picture behind that the caller reports as written.
static int pgm_write(const char* path, const uint8_t* img, uint32_t w, uint32_t h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int wrote = fprintf(f, "P5\n%u %u\n255\n", w, h) > 0
                   && fwrite(img, 1, (size_t)w * h, f) == (size_t)w * h;
    return fclose(f) == 0 && wrote;
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
    float       in_db;       // how loud the station arrives, relative to the modulator's own level;
                             // the AGC is what turns this knob into no change at all
    uint16_t    rx_shift;    // the shift setting, when it is not the signal's own (0 = same): this
                             // is how a case grades a wrong setting without moving the signal too
    float       tx_clock;    // the transmitter's symbol clock error in percent, positive = faster
                             // than the receiver expects, so the bits arrive short
    float       min_cer;     // a floor for the cases that have to keep failing: a wrong speed
                             // setting decodes badly, and a change that made it decode would be
                             // a different modem, not a better one
    float       echo_ms;     // a second path arriving this much after the direct one. A bit is
                             // 22 ms at 45.45 Bd and 20 at 50, so 1..20 ms is a twentieth of a bit
                             // to a whole one - the range a one hop and a two hop path span
    float       echo_db;     // and at this level relative to the direct path
    uint32_t    noise_seed;  // 0 keeps the bench's default draw; a table row sets it per seed
} rtty_case_t;

// A take does not begin where its message begins, and the score is meant to measure the modem on
// the message, not on the bulletin around it: a receiver switched on part way through a DWD
// transmission opens inside a long RYRY run, and an operator stops recording some time after the
// message ends, so more of it follows. So the score
// searches the whole take for the message and reports where it found it as the lead.
static int lead_in_junk;
// what the last score was measured on: the length of the expected text and how many of its
// characters the decoder got wrong. One character is 2.1 % of the 48 character message, so 0.0 %
// says "under one character wrong", not "clean".
static int cer_chars;
static int cer_wrong;

// A copy of the take as the receiver receives it, before the AGC, for the self check that asks
// whether the noise on a take is real. The self check points this at a buffer, runs the same case
// with and without noise, and measures the difference.
static float32_t* take_copy;
static size_t     take_copy_cap;
static size_t     take_copy_n;

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

// The radio's audio path into the demodulator, as audio_driver.c builds it for a 12 ksps RX
// buffer: the impulse blanker, the WDSP AGC the operator has selected, then the fixed post-AGC
// gain every non-AM mode gets, then the notch/peak/lowshelf biquad and only then the modem. The
// bench models the two that set the level - the AGC and the fixed gain - because they are what the
// demodulator's thresholds see. The biquad is a passthrough until a notch, peak or shelf is picked
// (the firmware loads biquad_passthrough for "no filter"), and an operator's station filter is not
// what this bench measures; the impulse blanker needs an impulse to fire on and no case sends one.
//
// agc_wdsp_conf is loaded from EEPROM in the firmware, before AudioAgc_AgcWdsp_Init() reads it, so
// a host build has to state the factory values audio_agc.h documents as the defaults itself.
static void radio_agc_setup(void)
{
    agc_wdsp_conf.mode = AGC_WDSP_MODE_DEFAULT;
    agc_wdsp_conf.slope = AGC_WDSP_SLOPE_DEFAULT;
    agc_wdsp_conf.hang_enable = AGC_WDSP_HANG_ENABLE_DEFAULT;
    agc_wdsp_conf.thresh = AGC_WDSP_THRESH_DEFAULT;
    agc_wdsp_conf.hang_thresh = AGC_WDSP_HANG_THRESH_DEFAULT;
    agc_wdsp_conf.hang_time = AGC_WDSP_HANG_TIME_DEFAULT;
    agc_wdsp_conf.tau_hang_decay = AGC_WDSP_TAU_HANG_DECAY_DEFAULT;
    agc_wdsp_conf.tau_decay[0] = AGC_WDSP_TAU_DECAY_0_DEFAULT;
    agc_wdsp_conf.tau_decay[1] = AGC_WDSP_TAU_DECAY_1_DEFAULT;
    agc_wdsp_conf.tau_decay[2] = AGC_WDSP_TAU_DECAY_2_DEFAULT;
    agc_wdsp_conf.tau_decay[3] = AGC_WDSP_TAU_DECAY_3_DEFAULT;
    agc_wdsp_conf.tau_decay[4] = AGC_WDSP_TAU_DECAY_4_DEFAULT;
    agc_wdsp_conf.tau_decay[5] = AGC_WDSP_TAU_DECAY_5_DEFAULT;
    AudioAgc_AgcWdsp_Init();
    AudioAgc_SetupAgcWdsp(RATE, false);
}

// AudioAgc_RunAgcWdsp() works one sample at a time and takes a pointer to AUDIO_BLOCK_SIZE samples,
// so the buffer is walked in blocks of that size; the firmware calls it once per decimated
// interrupt, which is a smaller block, and the result does not depend on where the blocks fall.
//
// The AGC's own unit is the ADC count: max_input and out_target are both ADC_CLIP_WARN_THRESHOLD
// (4096), because the firmware's audio buffer carries raw converter counts. A +-1 float sample
// handed straight to it would sit 72 dB below that design level and its thresholds would mean
// something else entirely, so the chain is entered in the radio's units and the result divided back
// into the bench's. What the demodulator then sees is the AGC's relative gain, at the scale the
// rest of this bench is written in.
//
// The AGC delays its output by half its ring (48 samples at 12 ksps, 4 ms). That is inside the
// lead-in the score skips; its gain ramp is not necessarily, and the level rows are what say so.
static void radio_agc(float32_t* x, size_t n)
{
    const float post_agc_gain = POST_AGC_GAIN_SCALING_DECIMATE_4 * 0.333f;   // audio_driver.c
    const float adc_full_scale = (float) ADC_CLIP_WARN_THRESHOLD;            // audio_driver.h: 4096
    for (size_t i = 0; i < n; i += AUDIO_BLOCK_SIZE)
    {
        const int16_t block = (int16_t) ((n - i) < AUDIO_BLOCK_SIZE ? (n - i) : AUDIO_BLOCK_SIZE);
        for (int16_t k = 0; k < block; k++) x[i + k] *= adc_full_scale;
        AudioAgc_RunAgcWdsp(block, (float32_t (*)[AUDIO_BLOCK_SIZE]) (void*) (x + i), false);
        for (int16_t k = 0; k < block; k++) x[i + k] = x[i + k] * post_agc_gain / adc_full_scale;
    }
}

// The characters before the message are not the modem's errors, and neither are the ones after it,
// so the alignment that scores a take is free at both ends: every character of the expected text
// still has to be matched - that is what the CER counts - while anything before the message and
// anything after it costs nothing. Skipping a fixed number of leading characters cannot do this,
// whatever the number is picked as: a 60 character RYRY run is a normal opening for a live take,
// and charging it as insertions turns a copy that is a character or two wrong into 91-100 %.
//
// This is the Levenshtein recurrence with its first row zeroed, which makes the prefix free, and
// its answer taken from the cheapest cell of the last row, which makes the suffix free. It also
// returns where in the take the message started, which is what the lead column reports. Ties go
// the same way Bench_Levenshtein breaks them, so a take that starts on its message scores exactly
// what it scored before.
static size_t message_errors(const char* expected, const char* got, size_t* start)
{
    const size_t need = strlen(expected), have = strlen(got);
    size_t* prev    = malloc((have + 1) * sizeof *prev);
    size_t* cur     = malloc((have + 1) * sizeof *cur);
    size_t* prev_at = malloc((have + 1) * sizeof *prev_at);
    size_t* cur_at  = malloc((have + 1) * sizeof *cur_at);
    if (!prev || !cur || !prev_at || !cur_at)
    {
        free(prev); free(cur); free(prev_at); free(cur_at);
        *start = 0;
        return Bench_Levenshtein(expected, got);
    }

    // row 0: nothing of the expected text matched yet, at no cost wherever the message starts
    for (size_t j = 0; j <= have; j++) { prev[j] = 0; prev_at[j] = j; }

    for (size_t i = 1; i <= need; i++)
    {
        cur[0] = i;                     // the take ended first: the rest is missing, not free
        cur_at[0] = 0;
        for (size_t j = 1; j <= have; j++)
        {
            const size_t cost = (expected[i - 1] == got[j - 1]) ? 0 : 1;
            size_t best = cur[j - 1] + 1;               // a character of the take the text lacks
            size_t at   = cur_at[j - 1];
            if (prev[j - 1] + cost < best) { best = prev[j - 1] + cost; at = prev_at[j - 1]; }
            if (prev[j] + 1 < best)        { best = prev[j] + 1;        at = prev_at[j]; }
            cur[j] = best;
            cur_at[j] = at;
        }
        size_t* tr = prev;    prev = cur;    cur = tr;
        size_t* ta = prev_at; prev_at = cur_at; cur_at = ta;
    }

    // the message may end anywhere in the take: the cheapest cell of the last row is the answer
    size_t best = prev[0], at = prev_at[0];
    for (size_t j = 1; j <= have; j++)
    {
        if (prev[j] < best) { best = prev[j]; at = prev_at[j]; }
    }
    free(prev); free(cur); free(prev_at); free(cur_at);
    *start = at;
    return best;
}

static float score_aligned(const char* expected, const char* got){
    const size_t need = strlen(expected);
    size_t at = 0;
    const size_t wrong = message_errors(expected, got, &at);
    lead_in_junk = (int) at;
    cer_chars = (int) need;
    cer_wrong = (int) wrong;
    return need ? 100.0f * (float) wrong / (float) need : 0.0f;
}

// The modem's own two centres, asked of the demodulator instead of restated here.  The mark filter
// is label 0, and the space filter is the label the firmware picks for the shift it is set to, so a
// retuned rtty.c moves the synthesiser, the gate and the report with it rather than leaving this
// file grading tones against a pair the receiver no longer filters.  That is the same mistake as a
// README quoting the 915/1365 it remembers instead of the pair it measures, and every number here
// is unchanged today because label 0 is 915 and the space labels are 915 + shift.
static float bench_mark_hz(void)
{
    return (float) Rtty_BenchFilterLabel(0);
}

static float bench_space_hz(uint16_t shift)
{
    for (int i = 0; i < RTTY_SHIFT_NUM; i++)
    {
        if (rtty_shifts[i].value == (float32_t) shift) return (float) Rtty_BenchFilterLabel(1 + (unsigned) i);
    }
    // no filter in the bank carries that shift: this is what the firmware's own selection drives, so
    // ask for it and let the row that cannot be met fail on its pair, not on a missing constant
    return bench_mark_hz() + (float) shift;
}

// until_eot says how the transmission is bounded: false runs for 13.3 s, room for the 48 character
// matrix message, and true runs until the modulator has sent its EOT, which is what the long
// message needs because its length is a property of the caller and not of the text. It is a
// parameter rather than something measured off msg, so a mode that sends more characters gets the
// bound it asks for.
static float rtty_run(const rtty_case_t* c, const char* msg, bool until_eot, char* got,
                      size_t got_sz, int* eot)
{
    // 13.3 s of received samples is room for the short message and its EOT; the long one is bounded
    // by the EOT itself, with MAX_SAMP as the ceiling that keeps the buffer honest
    size_t n = until_eot ? (size_t) MAX_SAMP : 160000;
    // a reference error slides mark and space together: the shift a transmitter sends does not
    // change, the pair just lands somewhere else in the receiver's filter bank
    const float tune = c->ppm * 1e-6f * c->rf_hz;

    rtty_ctrl_config.speed_idx = c->baud == 50 ? RTTY_SPEED_50 : RTTY_SPEED_45;

    // the receiver's shift setting: its own, unless the case is grading someone else's signal
    const uint16_t sel_shift = c->rx_shift ? c->rx_shift : c->shift;

    for (int i = 0; i < RTTY_SHIFT_NUM; i++)
    {
        if (rtty_shifts[i].value == (float32_t) sel_shift) rtty_ctrl_config.shift_idx = (rtty_shift_t) i;
    }

    // TX: the firmware modulator produces the waveform, EOT ends it. The receiver starts from a
    // clean slate, so no case is scored on what the one before it left behind.
    Rtty_BenchResetDecoder();
    Rtty_Modem_Init((uint32_t) TXRATE);
    Rtty_BenchOffsetTx(tune, (uint32_t) TXRATE);
    // the signal keeps the shift it was sent with; without this the transmitter would follow the
    // receiver's setting, the loopback would decode its own mistake, and the case could not fail
    if (c->rx_shift && c->rx_shift != c->shift)
        Rtty_BenchTxSpaceTone(bench_space_hz(c->shift) + tune, (uint32_t) TXRATE);
    // the transmitter's symbol clock against the receiver's: a station 1 % fast sends 1 % shorter
    // bits, and the hook only touches how long the modulator holds a bit
    Rtty_BenchTxBitScale(1.0f / (1.0f + c->tx_clock / 100.0f));
    char feed[640];
    snprintf(feed, sizeof(feed), "%s", msg);
    strncat(feed, "\004", sizeof(feed) - strlen(feed) - 1);
    Bench_TxFeed(feed);
    Bench_TxOffClear();
    Bench_TextReset();
    Rtty_Modulator_StartTX();

    static float32_t buf[(size_t) MAX_SAMP];
    size_t used = 0;
    while (used < n)
    {
        // four transmit samples per received sample, averaged: a 4:1 decimation with a filter
        // that is more than wide enough for tones below 2 kHz
        float acc = 0.0f;
        for (int k = 0; k < 4; k++) acc += (float32_t) Rtty_Modulator_GenSample() / 32768.0f;
        buf[used++] = acc * 0.25f;
        if (until_eot && Bench_TxOffRequested()) break;
    }
    // 0.2 s of idle after the EOT: the demodulator needs the trailing tone to finish the last
    // character it has already got the bits for
    for (size_t tail = 0; until_eot && tail < 2400 && used < (size_t) MAX_SAMP; tail++)
    {
        float acc = 0.0f;
        for (int k = 0; k < 4; k++) acc += (float32_t) Rtty_Modulator_GenSample() / 32768.0f;
        buf[used++] = acc * 0.25f;
    }
    n = used;
    *eot = Bench_TxOffRequested() ? 1 : 0;

    // a station whose mark tone is the weak one: take the mark tone's own 20 Hz wide band out of
    // the transmit audio. Its peak gain is exactly 1, so the mark tone comes out c->mark_db weaker
    // and the space tone, one shift away, loses half a dB (94 % of its level at 170 Hz shift).
    if (c->mark_db > 0.0f) weaken_tone(buf, n, bench_mark_hz() + tune, 20.0f, c->mark_db);

    // an unmodulated carrier next to the space tone. The space filter has to reject it: if it does
    // not, the ATC sees that filter loud all the time and cannot tell a space bit from the carrier.
    if (c->adj_db != 0.0f)
    {
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) sum += (double) buf[i] * buf[i];
        float amp = (float) sqrt(sum / (double) n) * powf(10.0f, c->adj_db / 20.0f) * 1.41421356f;
        float w = (float) (2.0 * M_PI * (bench_mark_hz() + (float) c->shift + tune + c->adj_hz) / RATE);
        for (size_t i = 0; i < n; i++) buf[i] += amp * sinf(w * (float) i);
    }

    // a second path: the same transmission arriving echo_ms later and echo_db down, summed onto the
    // direct one. It is the one channel shape nothing else here produces - fading moves the level of
    // one path, noise adds a signal that does not carry the same bits, and a tuning error moves both
    // tones together - and it is the one that decides whether a bit boundary's keying is still a
    // boundary where the receiver samples it.
    if (c->echo_ms > 0.0f && c->echo_db != 0.0f)
    {
        const size_t d = (size_t)lround((double)c->echo_ms * 0.001 * (double)RATE);
        const float g = powf(10.0f, c->echo_db / 20.0f);
        if (d > 0 && d < n)
        {
            // backwards, so every term is the direct signal and not the sum of the two
            for (size_t i = n; i-- > d; ) buf[i] += g * buf[i - d];
        }
    }

    // channel, at the rate the demodulator sees
    Bench_AddQsb(buf, n, c->qsb_rate, c->qsb_depth);
    Bench_RngSeed(c->noise_seed ? c->noise_seed : 0xC0FFEEu);
    Bench_AddNoise(buf, n, c->snr_db, 0.0f);

    // how loudly the station arrives. The AGC is what should make this irrelevant, so this is the
    // knob the level rows turn; it is applied to the receiver's input, before the AGC.
    if (c->in_db != 0.0f)
    {
        const float g = powf(10.0f, c->in_db / 20.0f);
        for (size_t i = 0; i < n; i++) buf[i] *= g;
    }

    if (take_copy && n <= take_copy_cap)
    {
        memcpy(take_copy, buf, n * sizeof(float32_t));
        take_copy_n = n;
    }

    // the receiver: a cold AGC, the fixed post-AGC gain, then the demodulator. Cold, because the
    // AGC carries its gain, ring buffer and hang counter from one transmission into the next, so a
    // level row would otherwise measure whatever the case before it left in the gain and its
    // position in the table would be part of the result - the same order dependency the WAV check
    // had. The radio starts a new station from this state, and every case does too.
    AudioAgc_ResetAgcWdsp();
    AudioAgc_SetupAgcWdsp(RATE, false);
    radio_agc(buf, n);

#ifdef BENCH_BREAK_RX
    // Deliberately wrong: the receiver assumes the take's symbol rate matches its speed setting,
    // so a build that tells it the other speed must fail the RTTY table. break-rx makes that
    // failure the test, rather than a table that would pass whatever the receiver does.
    rtty_ctrl_config.speed_idx = (c->baud == 50) ? RTTY_SPEED_45 : RTTY_SPEED_50;
    Rtty_Modem_Init((uint32_t) TXRATE);
#endif

    // RX: the firmware demodulator, characters collected by the UI stand-in
    for (size_t i = 0; i < n; i++)
    {
        Rtty_Demodulator_ProcessSample(buf[i]);
    }
    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    return score_aligned(norm_baudot(msg, 0), got);
}

// Every noise row is a distribution of takes, not one draw: the same signal with a different noise
// sample, five times, and the row reports the mean with the seed range beside it. One 26 character
// take is 3.8 % per character, so a single seed cannot say where a floor is; the mean is what the
// limits pin. A row that is not noisy is one draw, and its range column says so with a dash.
#define TABLE_SEEDS 5

// The shaped 2.5 kHz measurement is not a pass/fail row, it is a description of a coast, and the
// coast is bimodal: at -13.5 dB one draw of the MFSK16 take decodes 7.7 % and the next 100 %, so
// five draws say more about which side of the cliff they landed on than about the cliff. Twelve
// draws give a median worth quoting, and the range is printed beside it.
#define FLOOR_SEEDS 12

typedef struct { double sum, lo, hi; int n; } spread_t;

static void spread_add(spread_t* s, double v)
{
    if (!s->n || v < s->lo) s->lo = v;
    if (!s->n || v > s->hi) s->hi = v;
    s->sum += v;
    s->n++;
}

static double spread_mean(const spread_t* s) { return s->n ? s->sum / (double) s->n : 0.0; }

// "low-high", or "-" for a row that was one draw
static const char* spread_range(const spread_t* s, char* buf, size_t sz)
{
    if (s->n > 1) snprintf(buf, sz, "%.1f..%.1f", s->lo, s->hi);
    else snprintf(buf, sz, "-");
    return buf;
}

// A row that has to keep failing needs a floor. It is derived from the five draws the row was
// measured with rather than picked: the mean minus the one sided 95 % lower confidence bound,
// t(0.05, 4) = 2.132 standard errors of a five sample mean. A build is only better on such a row
// when its mean falls below what the five draws support, so that is where the row fails. A row whose
// spread is wider than its mean gets 0.0 - it cannot be pinned from below, and the number says so.
static float floor_from_seeds(const float* v)
{
    double sum = 0.0;
    for (int i = 0; i < TABLE_SEEDS; i++) sum += (double) v[i];
    const double mean = sum / (double) TABLE_SEEDS;
    double var = 0.0;
    for (int i = 0; i < TABLE_SEEDS; i++) var += ((double) v[i] - mean) * ((double) v[i] - mean);
    const double se = sqrt(var / (double) (TABLE_SEEDS - 1) / (double) TABLE_SEEDS);
    const double floor = mean - 2.132 * se;
    return floor > 0.0 ? (float) floor : 0.0f;
}

// the same for a correlation, where the columns are three decimals apart
static const char* spread_range3(const spread_t* s, char* buf, size_t sz)
{
    if (s->n > 1) snprintf(buf, sz, "%.3f..%.3f", s->lo, s->hi);
    else snprintf(buf, sz, "-");
    return buf;
}

static int rtty_table(const char* msg, bool long_mode)
{
    // The .max_cer field is the character error rate each case has to stay within. Almost every case
    // decodes without a single wrong character, so a few characters of slack leave room for libm and
    // rounding differences between build hosts while still failing on a demodulator change that
    // costs text. The fast fading row and the wrong speed row are the exceptions, and their limits
    // are set from what they measure rather than from the clean 0 %.
    static const rtty_case_t cases[] =
    {
        // the space filter under a strong adjacent carrier: an unmodulated carrier 300 Hz above the
        // space tone at +20 dB over the wanted signal. The carrier alone grades nothing - with equal
        // tones this decodes at 0.0 % (0/48 and 0/392) at 170 Hz and at 850 Hz and at +30 dB too,
        // because the mark channel decides - so the row also sends the mark tone 30 dB down, the
        // condition that puts the bits on the space filter. Together they fail where the space
        // filter's rejection does: at the matching 170 Hz filter 0.0 % at +20 dB and 31.2 % (15/48,
        // 11.7 % long) with the carrier raised to +30 dB, and that level is the dynamic range the
        // README calls the weak point. At 850 Hz the setting has already cost the text - 97.9 %
        // (47/48) at either carrier level - so the carrier adds nothing there.
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
        // 2 Hz, 10 dB deep, at 10 dB SNR. Measured with real noise on the row: 47.9 % (23/48) short
        // and 93.6 % (367/392) long on the first seed - the modem loses lock partway through the
        // take and the rest of the message never comes out. That is pinned as a failure, not hidden:
        // the earlier 6.2 % / 2.3 % was measured while the bench's noise generator never advanced its
        // state, so the row had no noise on it at all. Over five seeds the fade catches the modem on
        // some draws and not on others (0.0-47.9 %, mean 21.2 %; the bottom was 4.2 % while the
        // scorer still charged the junk the decoder emits after it has lost lock), so the floor is
        // set well below the mean: losing lock is what the row is for, and a draw that survives it
        // is not a fix.
        { .label = "qsb fast 10",  .shift = 170, .snr_db = 10.0f, .qsb_rate = 2.0f,
          .qsb_depth = 10.0f, .max_cer = 100.0f, .min_cer = 10.0f },
        // a slow deep fade, so a character is read while its keying is down rather than after the
        // dip. The row above loses the text by losing lock: 2 Hz is one dip every 500 ms, and the
        // receiver comes out of it with the rest of the message missing. This one is one cycle
        // every 4 s and 26 dB deep with the signal at 6 dB SNR, so the fade holds still over
        // several characters and then lets go - the text either side of a dip is copied and the
        // characters inside it are not. Measured: 21.7 % over the five seeds (12.5..35.4). The
        // alignment of one seed's take charges seven characters, five of them 12 to 20 dB under
        // that take's own keying and two at its level, and the very same take with qsb_depth 0
        // charges none at 0.0 %, so the dip is what takes those characters down. That is the one
        // case in the table where a wrong character can be shown to sit on keying the channel
        // took down, which is what makes the at-level claim about the fixture testable the other
        // way. The floor keeps it that way: a clean run would mean the fade had stopped reaching
        // a character, and every charged character in the corpus would be an at-level one again.
        { .label = "qsb slow deep", .shift = 170, .snr_db = 6.0f, .qsb_rate = 0.25f,
          .qsb_depth = 26.0f, .max_cer = 100.0f, .min_cer = 10.0f },
        // the real services: DWD DDH/DDK are F1B 50 Baud with +/-225 Hz (450 Hz total) on
        // 4583/7646/10100.8/11039/14467.3 kHz and +/-42.5 Hz (85 Hz) on 147.3 kHz, per the DWD
        // table on http://weather.mailasail.com/franks-weather/radio-teletype-weather-broadcasts.
        // The space filter is only graded by a case where the mark channel cannot decide. Weakening
        // the space tone does not do it: the 915 Hz filter still reads mark at 0 dB and space 22.9 dB
        // down (the README figure for this shift), so the mark channel keeps the right polarity and
        // the space filter is not what decides. Weakening the MARK tone past that 22.9 dB inverts the
        // mark channel, which then reads the space tone 22.9 dB down as louder than the mark tone
        // 30 dB down, and the decision has to come from the space filter: this row decodes at 0.0 %
        // with the 170 Hz filter and at 97.9 % (47/48) with the 850 Hz one. It is the only row in the
        // table that grades the shift.
        { .label = "weak mark 170", .shift = 170, .snr_db = 20.0f, .mark_db = 30.0f,
          .max_cer = 5.0f },
        { .label = "dwd 450 50bd", .shift = 450, .snr_db = 20.0f, .baud = 50, .max_cer = 5.0f },
        { .label = "dwd 450 10bd", .shift = 450, .snr_db = 10.0f, .baud = 50, .max_cer = 5.0f },
        { .label = "ddh47 85 50bd", .shift = 85, .snr_db = 20.0f, .baud = 50, .max_cer = 5.0f },
        // the transmitter's symbol clock, which is not the receiver's. The demodulator's bit clock
        // is a DPLL that nudges itself by 1/32 of a bit on every transition, so it follows a clock
        // error until the phase runs away from it. Probed in 0.5 % steps, the edges are +8 % and
        // -7 %: clean up to there in both matrices, and gone at +8.5 % (6.2 % short, 8.9 % long)
        // and -7.5 % (20.8 % short, 15.8 % long). The asymmetry is measured, not explained. The two
        // inside rows pin the tolerance, the two edge rows pin the failure from above zero so a
        // demodulator change cannot quietly widen the window, and the last row is not drift but an
        // operator setting: 45.45 Bd decoded with the radio set to 50 Bd, the same hook at 45.45/50.
        { .label = "clk +8 %",     .shift = 170, .snr_db = 20.0f, .tx_clock = 8.0f,  .max_cer = 5.0f },
        { .label = "clk +8.5 %",   .shift = 170, .snr_db = 20.0f, .tx_clock = 8.5f,
          .max_cer = 100.0f, .min_cer = 3.0f },
        { .label = "clk -7 %",     .shift = 170, .snr_db = 20.0f, .tx_clock = -7.0f, .max_cer = 5.0f },
        { .label = "clk -7.5 %",   .shift = 170, .snr_db = 20.0f, .tx_clock = -7.5f,
          .max_cer = 100.0f, .min_cer = 10.0f },
        { .label = "45.45 as 50",  .shift = 170, .snr_db = 20.0f, .baud = 50, .tx_clock = -9.09f,
          .max_cer = 100.0f, .min_cer = 50.0f },
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
        // the same clean signal at four input levels, 40 dB apart, which is the level sensitivity the
        // AGC exists to remove. Measured through the radio's AGC, cold, at every one of them:
        // 0.0 % (0/48) and 0.0 % (0/392). The demodulator itself would not decode a station 30 dB
        // down - see the note in README.md on how the AGC has to be fed in ADC counts for this to
        // mean anything - so this is the AGC doing its job, not the modem being level proof.
        // a receiver set to the wrong shift while the signal stays where it belongs. This is the
        // row that grades the setting: measured 0.0 % with the right space filter and 97.9 %
        // (47/48) here. The mark tone 30 dB down is what makes it work - a clean two-tone signal
        // decodes at 0.0 % with every setting, because the mark channel carries the bits whatever
        // the space filter does, and that is measured rather than assumed.
        { .label = "shift 170/850", .shift = 170, .snr_db = 20.0f, .mark_db = 30.0f,
          .rx_shift = 850, .max_cer = 100.0f, .min_cer = 50.0f },
        { .label = "level -30",    .shift = 170, .snr_db = 20.0f, .in_db = -30.0f, .max_cer = 5.0f },
        { .label = "level -10",    .shift = 170, .snr_db = 20.0f, .in_db = -10.0f, .max_cer = 5.0f },
        { .label = "level +0",     .shift = 170, .snr_db = 20.0f, .max_cer = 5.0f },
        { .label = "level +10",    .shift = 170, .snr_db = 20.0f, .in_db = 10.0f, .max_cer = 5.0f },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);

    printf("RTTY loopback of drivers/audio/rtty.c through the radio's audio AGC\n");
    printf("(per case: 45.45 or 50 Bd, 1.5 stop bits, tx 48 kHz -> rx 12 kHz)\n");
    printf("%-16s %7s %11s %8s %6s  %s\n", "case", "CER %", "min-max", "err/len", "lead",
           "decoded");
    int failures = 0;
    for (size_t i = 0; i < n_cases; i++)
    {
        // the seed range is the same signal under different noise samples: what the row's limit
        // pins is the mean, not the luck of the first draw
        const int nseeds = cases[i].snr_db < 90.0f ? TABLE_SEEDS : 1;
        spread_t cs = { 0 };
        int eot = 0, lead = 0, wrong = 0, chars = (int) strlen(msg);
        char first[8192] = "";
        for (int k = 0; k < nseeds; k++)
        {
            rtty_case_t c = cases[i];
            if (nseeds > 1) c.noise_seed = 0xC0FFEEu + (uint32_t) k;
            int eot_k = 0;
            char text[8192];
            spread_add(&cs, rtty_run(&c, msg, long_mode, text, sizeof(text), &eot_k));
            wrong += cer_wrong;
            chars = cer_chars;
            if (!k) { snprintf(first, sizeof(first), "%s", text); eot = eot_k; lead = lead_in_junk; }
        }
        const float cer = (float) spread_mean(&cs);
        char counted[32], range[32];
        spread_range(&cs, range, sizeof(range));
        snprintf(counted, sizeof(counted), "%.1f/%d", (double) wrong / (double) nseeds, chars);
        printf("%-16s %7.1f %11s %8s %6d  %.64s%s%s\n", cases[i].label, (double) cer, range,
               counted, lead, first, strlen(first) > 64 ? " ..." : "",
               eot ? "" : "   [transmission cut off before EOT]");
        if (cer > cases[i].max_cer)
        {
            printf("FAIL  %-16s character error rate %.1f%% is above the %.1f%% this case allows\n",
                   cases[i].label, cer, cases[i].max_cer);
            failures++;
        }
        if (cer < cases[i].min_cer)
        {
            printf("FAIL  %-16s character error rate %.1f%% is below the %.1f%% this case has to stay\n"
                   "      above: a wrong speed setting is supposed to cost the text\n",
                   cases[i].label, cer, cases[i].min_cer);
            failures++;
        }
    }
    printf("\nnote: the shift setting is graded by the shift 170/850 row, not by the rest of the\n"
           "      table. A clean two-tone signal decodes at 0.0 %% whatever space filter is selected,\n"
           "      because the mark channel carries the bits; that row takes the mark tone 30 dB down\n"
           "      so the space filter has to decide, and there the wrong setting costs the text:\n"
           "      97.9 %% with the 850 Hz space filter against 0.0 %% with the 170 Hz one, the same\n"
           "      figure the weak mark row in this table measures.\n"
           "      \"digi_bench rtty-shift\" prints the whole setting curve. On live radio the shift\n"
           "      matters beyond this: a DDH 8 capture decodes only at 425/450 Hz shift. An\n"
           "      unsupported shift is still mapped to 170.\n");
    if (failures == 0)
    {
        printf("\nall %zu cases within their character error rate limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  how much the shift setting matters
 * ------------------------------------------------------------------ */
// The signal is always 170 Hz shift at 45.45 Bd and only the receiver's setting moves, which is what
// a radio tuned to a station and set wrong actually does. Two columns, because the clean signal
// does not grade the setting at all: the mark channel carries the bits whatever the space filter
// does, so every setting decodes it (measured). Taking the mark tone 30 dB down puts the decision on
// the space filter, and then only the setting that matches the signal works.
static int rtty_shift(void)
{
    static const uint16_t settings[] = { 85, 170, 200, 425, 450, 850 };
    const char* msg = "DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT";
    const size_t n_set = sizeof(settings) / sizeof(settings[0]);
    char got[8192];
    int failures = 0;

    printf("RTTY shift setting against a 170 Hz / 45.45 Bd signal: only the setting moves.\n"
           "\"clean\" is that signal at full level, \"weak mark\" has its mark tone 30 dB down.\n\n");
    printf("%8s %9s %11s\n", "setting", "clean %", "weak mark %");
    for (size_t k = 0; k < n_set; k++)
    {
        rtty_case_t c = { .shift = 170, .snr_db = 20.0f, .rx_shift = settings[k] };
        int eot = 0;
        const float clean = rtty_run(&c, msg, false, got, sizeof(got), &eot);
        c.mark_db = 30.0f;
        const float weak = rtty_run(&c, msg, false, got, sizeof(got), &eot);
        printf("%8u %9.1f %11.1f\n", (unsigned) settings[k], clean, weak);
        if (settings[k] == 170 && weak > 5.0f)
        {
            printf("FAIL  the setting that matches the signal has to decode it, %.1f %% is above 5 %%\n", weak);
            failures++;
        }
        if (settings[k] == 850 && weak < 50.0f)
        {
            printf("FAIL  a setting 680 Hz away has to cost the text, %.1f %% is below 50 %%\n", weak);
            failures++;
        }
    }
    printf("\nthe clean column is why this row needs the weak mark: with both tones at full level the\n"
           "mark channel decides and every setting decodes. The weak mark column is the setting\n"
           "working: 170 Hz and 200 Hz decode, every setting further away costs the text.\n");
    if (!failures) printf("\nthe shift setting is graded: %u settings, and only the 170/200 Hz pair\n"
                       "decodes the signal\n", (unsigned) n_set);
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
            cer[k] = rtty_run(&cases[k], msg, false, got, sizeof(got), &eot);
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
            cer_weak[k] = rtty_run(&weak[k], msg, false, got, sizeof(got), &eot);
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
    uint32_t lines_min;       // 0 = every line has to be found; otherwise the known gap's count
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
        // Re-measured end to end, because the numbers below 0.95 used to be taken against a test
        // signal that no longer exists.  The generator used to synthesize every picture column as its
        // own burst with a 4-sample fade at both ends; a real fax line is one oscillator whose frequency
        // steps once per pixel, and that difference dominated the measurement - a 14 Hz reference error
        // (1 ppm at 20 m) scored 0.549 while a 70 Hz error scored 0.976, which is not a property of any
        // decoder or channel.  With phase-continuous columns the tune rows sit together at 0.937..0.951
        // and the grey means came down to the truth's 199-201 instead of the 206-210 the faded signal
        // produced.  The correlation ceiling is now about 0.95 for a different reason: the grey scale is
        // estimated from a two-bin ratio at 1500 and 2300 Hz, which is coarse, and a real receiver uses
        // a discriminator and an integrator instead.
        { .name = "clean", .snr_db = 100.0f, .rows = 12, .corr_min = 0.92f, .corr_max = 1.00f },
        { .name = "snr 20", .snr_db = 20.0f, .rows = 12, .corr_min = 0.92f, .corr_max = 1.00f },
        { .name = "snr 12", .snr_db = 12.0f, .rows = 12, .corr_min = 0.88f, .corr_max = 1.00f },
        { .name = "snr 12 fast qsb", .snr_db = 12.0f, .qsb_rate_hz = 2.0f, .qsb_depth_db = 10.0f,
          .rows = 12, .corr_min = 0.88f, .corr_max = 1.00f },
        // Known gap, still recorded rather than hidden, but the gap is no longer what the number
        // measured.  The capture re-acquires each line from the previous tip and carries no model of
        // the line rate, so one miss offsets every later search window and the skew then costs whole
        // lines: 9 of 12 on the worst seed, and the spacing metric reports 2983 samples because the
        // tips it did find are progressively further from where it expected them.  A row it misses is
        // now a hole in the canvas instead of a slide of everything below it, which is what the earlier
        // min(lines_found, rows) comparison was hiding behind a correlation of 0.01.
        { .name = "snr 12 + skew (gap)", .snr_db = 12.0f, .skew_ppm = 20000, .rows = 12,
          .lines_min = 8, .corr_min = -1.00f, .corr_max = 0.50f },
        { .name = "snr 12 black/white swapped", .snr_db = 12.0f, .invert = true, .rows = 12,
          .corr_min = -1.00f, .corr_max = -0.90f },
        { .name = "snr 12 reversed scan", .snr_db = 12.0f, .steer_right = true, .rows = 12,
          .corr_min = 0.92f, .corr_max = 1.00f },
        // The mild-skew row that used to be a gap at 0.30..0.65 is not one any more: it finds all 12
        // lines and correlates at 0.794 (seeds 0.724..0.837), which is why its ceiling moved.  The
        // measured improvement is the honest one - tracking each tip and numbering rows by it.
        { .name = "snr 8 qsb skew", .snr_db = 8.0f, .qsb_rate_hz = 0.4f, .qsb_depth_db = 6.0f,
          .skew_ppm = 3000, .rows = 12, .lines_min = 10, .corr_min = 0.65f, .corr_max = 1.00f },
        // the same rigid reference error the RTTY rows carry: every tone of the emission moves by
        // ppm * rf_hz while the demodulator's bins stay at 1857/1500/2300 Hz. The sync tip is the
        // narrow one, so a tuning error attacks the line capture before it attacks the picture, and
        // the grey level it decodes to is what these rows pin.
        { .name = "20m 1ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.90f, .corr_max = 1.00f,
          .ppm = 1.0f, .rf_hz = 14e6f, .mean_min = 198.0f, .mean_max = 204.0f },
        { .name = "20m 5ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.90f, .corr_max = 1.00f,
          .ppm = 5.0f, .rf_hz = 14e6f, .mean_min = 196.0f, .mean_max = 202.0f },
        { .name = "10m 2ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.90f, .corr_max = 1.00f,
          .ppm = 2.0f, .rf_hz = 28e6f, .mean_min = 196.0f, .mean_max = 202.0f },
        { .name = "50m 1ppm", .snr_db = 20.0f, .rows = 12, .corr_min = 0.90f, .corr_max = 1.00f,
          .ppm = 1.0f, .rf_hz = 50e6f, .mean_min = 196.0f, .mean_max = 202.0f },
    };
    size_t n = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("HF weather fax, 120 lines/min at 12 kHz, sync 1857 Hz, black 1500 / white 2300 Hz\n");
    printf("%-28s %6s %8s %8s %14s %8s %7s\n", "case", "lines", "skewRMS", "corr", "min-max",
           "mean", "off Hz");
    for (size_t i = 0; i < n; i++)
    {
        size_t samples = (size_t) cases[i].rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(samples * sizeof(float32_t));
        float32_t* work = malloc(samples * sizeof(float32_t));
        uint8_t* truth = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        uint8_t* grey = malloc((size_t) cases[i].rows * WEFAX_IMG_COLS);
        wefax_result_t res;
        if (!x || !work || !truth || !grey)
        {
            printf("FAIL  %-28s out of memory\n", cases[i].name);
            failures++;
            free(x); free(work); free(truth); free(grey);
            continue;
        }
        const float tune_hz = cases[i].ppm * 1e-6f * cases[i].rf_hz;
        wefax_cfg_t cfg = { .name = cases[i].name, .snr_db = cases[i].snr_db,
                            .qsb_rate_hz = cases[i].qsb_rate_hz,
                            .qsb_depth_db = cases[i].qsb_depth_db,
                            .skew_ppm = cases[i].skew_ppm, .invert = cases[i].invert,
                            .steer_right = cases[i].steer_right, .rows = cases[i].rows,
                            .tune_hz = tune_hz };

        memset(grey, 0, (size_t) cases[i].rows * WEFAX_IMG_COLS);   // a lost line stays a hole, not a shift
        Bench_WefaxGenerate(x, truth, &cfg);
        // every noise row is a distribution of takes, not one draw: the same picture under five
        // noise samples, and the correlation printed is their mean with the seed range beside it.
        // The line count is the worst seed's, because a line the capture misses is lost whole.
        const int nseeds = cases[i].snr_db < 90.0f ? TABLE_SEEDS : 1;
        spread_t corr_s = { 0 }, mean_s = { 0 };
        unsigned lines_worst = cases[i].rows, grey_rows = cases[i].rows;
        float skew = 0.0f;
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, samples * sizeof(float32_t));
            if (nseeds > 1)
            {
                Bench_RngSeed(0xF00Du + (uint32_t) k);
                Bench_AddNoise(work, samples, cases[i].snr_db, 0.0f);
            }
            Bench_WefaxDecode(work, samples, grey, cases[i].rows, &res);
            res.lines_expected = cases[i].rows;

            // The whole canvas, not just the rows that were filled: a missed line leaves a black hole,
            // and that is a real loss which the score has to count instead of being smoothed away by
            // comparing only the rows that happen to line up.
            size_t cmp = (size_t) cases[i].rows * WEFAX_IMG_COLS;
            spread_add(&corr_s, pearson(truth, grey, cmp));
            spread_add(&mean_s, res.mean_level);
            if (res.lines_found < lines_worst) lines_worst = res.lines_found;
            if (!k)
            {
                skew = res.row_rms_error;
                grey_rows = res.lines_found ? res.lines_found : cases[i].rows;
                if (i == 1)             // leave one readable broadcast behind for inspection
                {
                    Bench_WavWrite("wefax_snr20.wav", work, samples, (uint32_t) RATE);
                    pgm_write("wefax_truth.pgm", truth, WEFAX_IMG_COLS, cases[i].rows);
                    pgm_write("wefax_decoded.pgm", grey, WEFAX_IMG_COLS, grey_rows);
                }
            }
        }
        const float corr = (float) spread_mean(&corr_s);
        const float mean_level = (float) spread_mean(&mean_s);
        char range[32];
        spread_range3(&corr_s, range, sizeof(range));
        printf("%-28s %3u/%-2u %8.1f %8.3f %14s %8.1f %7.1f\n", cases[i].name, lines_worst,
               cases[i].rows, (double) skew, (double) corr, range, (double) mean_level,
               (double) tune_hz);
        const uint32_t need = cases[i].lines_min ? cases[i].lines_min : cases[i].rows;
        if (lines_worst < need)
        {
            printf("FAIL  %-28s found %u of the %u lines it transmitted on its worst seed (at "
                   "least %u expected)\n", cases[i].name, lines_worst, cases[i].rows, need);
            failures++;
        }
        if (corr < cases[i].corr_min || corr > cases[i].corr_max)
        {
            printf("FAIL  %-28s mean correlation %.3f is outside %.2f..%.2f for this case\n",
                   cases[i].name, (double) corr, cases[i].corr_min, cases[i].corr_max);
            failures++;
        }
        if (cases[i].mean_max > cases[i].mean_min &&
            (mean_level < cases[i].mean_min || mean_level > cases[i].mean_max))
        {
            printf("FAIL  %-28s mean decoded grey %.1f is outside %.1f..%.1f for this case\n",
                   cases[i].name, (double) mean_level, cases[i].mean_min, cases[i].mean_max);
            failures++;
        }
        free(x); free(work); free(truth); free(grey);
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
 *  NAVTEX / SITOR-B
 * ------------------------------------------------------------------ */
// The first mode here that is not the radio's: navtex.c is a reference modulator and demodulator that
// exist only in this bench, the way wefax.c does. It is graded on the same terms as the rest - a
// synthesised transmission, a channel, and the text that comes back - and its two error control
// mechanisms are the ones that have to hold: the constant ratio alphabet, which makes a single wrong
// bit visible, and the repeat copy five character slots later, which is what a burst has to be
// survivable without. What it does not prove is that a real station was copied; no take of one has
// been made here, and the receiver fits the bit clock over a +-2 % window, which is enough for the
// rate a bench generates and not for an arbitrary recording.
static float navtex_score(const char* expected, const char* got, int* skip)
{
    const size_t need = strlen(expected);
    float best = 100.0f;
    *skip = 0;
    for (int s = 0; s < 40 && got[s]; s++)
    {
        const size_t wrong = Bench_Levenshtein(expected, got + s);
        const float cer = need ? 100.0f * (float)wrong / (float)need : 0.0f;
        if (cer < best) { best = cer; *skip = s; }
    }
    return best;
}

typedef struct
{
    const char* name;
    float snr_db, qsb_rate, qsb_depth;
    float tune_hz;          // both tones moved by this much: the receiver's bins do not follow them
    float erase_ms;         // a burst starting this far into the take, this many ms long, erased
    float erase_len_ms;     // (signal and noise together, which is what a fade looks like)
    int   order;            // NAVTEX_ORDER_*: which bit order the take is generated in
    float max_cer;
    float min_cer;          // a floor for the rows that have to keep failing
} navtex_case_t;

// Time-scale a take, which is how this makes a recording whose bit rate is not the transmitter's.
// A capture through a sound card whose clock is a fraction of a percent off is exactly this, and the
// reference decoder could only follow ±1.7 % because it searched 118 to 122 samples a bit; the
// streaming demodulator measures the rate instead, so the rows that break one are the point of the
// other.  Linear interpolation is enough: the effect being tested is the bit period, not the audio.
static size_t bench_timescale(const float32_t* x, size_t n, double factor, float32_t* out, size_t cap)
{
    if (factor <= 0.0) return 0;
    size_t m = (size_t)((double)n / factor);
    if (m > cap) m = cap;
    for (size_t i = 0; i < m; i++)
    {
        const double src = (double)i * factor;
        size_t k = (size_t)src;
        if (k + 1 >= n) k = n > 1 ? n - 2 : 0;
        const double f = src - (double)k;
        out[i] = (float32_t)((1.0 - f) * (double)x[k] + f * (double)x[k + 1]);
    }
    return m;
}

typedef struct
{
    const char* name;
    float snr_db, qsb_rate, qsb_depth;
    float tune_hz;
    float erase_ms, erase_len_ms;
    float rate_pct;         // the take's bit rate moved this far off 100 Bd
    int   order;
    float max_cer;
    float min_cer;
    int   expect_nothing;   // a row that must not decode at all
    int   expect_silent;    // a row the receiver is allowed to refuse, and must then print nothing on
    int   allow_unlocked;     // a row that may end without a lock, as long as what it printed stays inside max_sub
    float max_sub;          // wrong characters allowed, counted after the best alignment
} sitor_case_t;

// The fewest wrong characters any alignment of the output against the message has.  CER alone cannot
// tell the two failure modes apart: a streaming receiver that has not finished acquiring prints a
// suffix of the message and scores as if it had got those characters wrong, while one that assembled
// words across a boundary prints a permutation of the alphabet and scores the same.  Only the second
// is a decoder that is wrong, and only the first is the price of streaming, so they are counted
// separately.  A very short output can align somewhere by luck; that is what the confidence column
// beside it is for, not this one.
static int navtex_wrong_chars(const char* msg, const char* got)
{
    const size_t L = strlen(msg), G = strlen(got);
    if (!G) return 0;
    int best = (int)G;
    for (size_t o = 0; o <= L; o++)
    {
        int w = 0;
        for (size_t j = 0; j < G; j++)
            if (o + j >= L || got[j] != msg[o + j]) w++;
        if (w < best) best = w;
    }
    return best;
}

// The streaming SITOR-B demodulator, on the same takes the reference modem is graded with, and on the
// ones it cannot be: a bit rate that is not exactly 100 Bd.  The reference column is printed beside it
// because the difference between the two is the reason the streaming one exists, and a claim that this
// receiver follows a rate error is worth nothing unless the old one demonstrably did not.
static int sitor_stream(void)
{
    static const char* msg = "NAVTEX TEST MESSAGE DE UHSDR BENCH 12345 CQ CQ";
    static const sitor_case_t cases[] =
    {
        // The streaming receiver is graded on two numbers, not one: `CER` counts every character of
        // the message it did not print correctly, which includes the first dozen or so it cannot
        // print at all because it was still finding the clock, and `wrong` counts the characters it
        // printed that were not what was sent.  Only the second is a defect.  A radio joining a
        // transmission in progress loses its head and nobody calls that a broken decoder, so the
        // CER limits below are the acquisition cost written down, and max_sub = 0 is the claim.
        { .name = "clean", .snr_db = 100.0f, .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "snr 20", .snr_db = 20.0f, .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "snr 10", .snr_db = 10.0f, .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "snr 10 qsb", .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 6.0f,
          .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "burst on DX", .snr_db = 20.0f, .erase_ms = 1400.0f, .erase_len_ms = 280.0f,
          .max_cer = 60.0f, .max_sub = 0.0f },
        { .name = "burst both", .snr_db = 20.0f, .erase_ms = 1400.0f, .erase_len_ms = 700.0f,
          .max_cer = 75.0f, .max_sub = 3.0f },
        { .name = "tune +50 Hz", .snr_db = 20.0f, .tune_hz = 50.0f, .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "tune +100 Hz", .snr_db = 20.0f, .tune_hz = 100.0f, .max_cer = 35.0f,
          .max_sub = 0.0f, .expect_silent = 1 },
        { .name = "reversed bits", .snr_db = 20.0f, .order = NAVTEX_ORDER_REVERSED,
          .max_cer = 35.0f, .max_sub = 0.0f },
        { .name = "wrong order", .snr_db = 20.0f, .order = NAVTEX_ORDER_MIXED, .max_cer = 95.0f,
          .min_cer = 60.0f, .max_sub = 45.0f },
        // Outside what an integer bit grid can follow: a period error of more than one sample walks
        // off the clock within about 140 bits.  The mode's own answer is to resample the audio, which
        // this receiver does not do, so it is required here to print nothing and to have said why -
        // rate_out_of_range - rather than to assemble characters across a boundary and call the
        // result a copy.  The reference decoder could not follow these rows either; that is what the
        // column beside them is for.
        { .name = "rate -3 %", .snr_db = 20.0f, .rate_pct = -3.0f, .max_cer = 35.0f, .max_sub = 0.0f,
          .expect_silent = 1 },
        { .name = "rate +3 %", .snr_db = 20.0f, .rate_pct = 3.0f, .max_cer = 35.0f, .max_sub = 0.0f,
          .expect_silent = 1 },
        // A period error of 1.5 % is 1.8 samples, which the integer grid rounds and then walks off:
        // the receiver stays locked, keeps a 4-of-7 share near 1.00 and prints characters assembled
        // across a word boundary.  These two ceilings are that defect written down as a maximum, not
        // a result - the reference modem does no better (100 % and 21.7 % CER on the same takes), and
        // the fix is fractional clock recovery or resampling the take, which is said in the README
        // rather than quietly done here.  If either number goes up, the defect has got worse.
        { .name = "rate -1.5 %", .snr_db = 20.0f, .rate_pct = -1.5f, .max_cer = 100.0f, .max_sub = 10.0f,
          .allow_unlocked = 1 },
        { .name = "rate +1.5 %", .snr_db = 20.0f, .rate_pct = 1.5f, .max_cer = 100.0f, .max_sub = 6.0f,
          .allow_unlocked = 1 },
        // nothing transmitted: a receiver that prints text out of noise is worse than one that
        // prints nothing, because the second one can be noticed
        { .name = "noise only", .snr_db = 0.0f, .max_cer = 100.0f, .expect_nothing = 1 },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("SITOR-B / NAVTEX through the streaming demodulator (drivers/digi/sitor_rx.c), the file the\n"
           "radio would compile: one sample in, one character out, the bit period measured from the\n"
           "transitions rather than assumed, and the same 4-of-7 and time diversity rules the reference\n"
           "modem is graded with.  Text is %zu characters.\n\n", strlen(msg));
    printf("%-14s %6s %11s %8s %8s %7s %6s %6s %3s %5s %8s\n", "case", "CER %", "min-max", "err/len",
           "conf", "bit ms", "fix", "lost", "rev", "wrong", "reference CER");
    for (size_t i = 0; i < n_cases; i++)
    {
        const sitor_case_t* c = &cases[i];
        const size_t cap = 400000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        float32_t* work = malloc(cap * sizeof(float32_t));
        float32_t* scaled = malloc(cap * sizeof(float32_t));
        char* got = malloc(8192);
        if (!x || !work || !scaled || !got)
        {
            free(x); free(work); free(scaled); free(got);
            printf("out of memory\n");
            return 1;
        }
        size_t n = Bench_NavtexGenerate(x, cap, msg, c->tune_hz, c->order);
        if (c->expect_nothing)
        {
            // the same length of take with the signal gone: noise, and nothing else
            n = strlen(msg) * 120u + 4000u;
            if (n > cap) n = cap;
            memset(x, 0, n * sizeof(float32_t));
        }
        const int nseeds = c->snr_db < 90.0f ? TABLE_SEEDS : 1;
        spread_t cs = { 0 };
        float conf_sum = 0.0f, period_sum = 0.0f;
        unsigned fix = 0, lost = 0, rev = 0, chars = 0;
        int locked = 1, nothing_ok = 1;
        float ref_cer = -1.0f;
        size_t worst_used = 0;
        int worst_wrong = 0;
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, n * sizeof(float32_t));
            if (nseeds > 1)
            {
                Bench_RngSeed(0x510513u + (uint32_t) k);
                Bench_AddNoise(work, n, c->snr_db, 0.0f);
            }
            Bench_AddQsb(work, n, c->qsb_rate, c->qsb_depth);
            if (c->erase_len_ms > 0.0f)
            {
                const size_t a = (size_t)(c->erase_ms * 12.0);
                const size_t b = a + (size_t)(c->erase_len_ms * 12.0);
                for (size_t j = a; j < b && j < n; j++) work[j] = 0.0f;
            }
            size_t m = n;
            if (c->rate_pct != 0.0f)
            {
                m = bench_timescale(work, n, 1.0 + (double)c->rate_pct / 100.0, scaled, cap);
            }
            else
            {
                memcpy(scaled, work, n * sizeof(float32_t));
            }

            SitorRx rx;
            SitorRx_Init(&rx, 12000.0f, 1615.0f, 1785.0f);
            size_t used = 0;
            for (size_t j = 0; j < m; j++)
            {
                if (SitorRx_Sample(&rx, scaled[j]))
                {
                    if (used + 1 < 8192) got[used++] = rx.out_char;
                }
            }
            got[used] = '\0';
            if (used > worst_used) worst_used = used;
            {
                // the worst seed is what the row is graded on, as everywhere else in this bench: a
                // receiver that prints one bad character in five runs has printed a bad character
                const int w = navtex_wrong_chars(msg, got);
                if (w > worst_wrong) worst_wrong = w;
            }
            if (getenv("SITOR_DEBUG")) fprintf(stderr, "\n[%s] m=%zu used=%u state=%d align=%d rev=%d norm=%d r=%d\n expected [%s]\n got      [%s]\n per=%d ph=%d idx=%d scores=%d,%d,%d,%d,%d,%d,%d other=%d\n", c->name, m, (unsigned)used, rx.state, rx.align, rx.reversed, rx.best_norm, rx.best_rev, msg, got, rx.period, rx.phase, rx.win_idx, rx.score[rx.live[0]][0],rx.score[rx.live[0]][1],rx.score[rx.live[0]][2],rx.score[rx.live[0]][3],rx.score[rx.live[0]][4],rx.score[rx.live[0]][5],rx.score[rx.live[0]][6], (int)(128.0f*SitorRx_Confidence(&rx)));
            const int skip_dummy = 0;
            (void)skip_dummy;
            int sk = 0;
            const float cer = used ? navtex_score(msg, got, &sk) : 100.0f;
            spread_add(&cs, cer);
            conf_sum += SitorRx_Confidence(&rx);
            period_sum += rx.period;
            fix += rx.repaired;
            lost += rx.lost;
            rev = (unsigned)rx.reversed;
            chars += (unsigned)used;
            if (rx.state != SITOR_LOCKED) locked = 0;
            if (used > 40) nothing_ok = 0;

            if (!k && !c->expect_nothing)
            {
                // what the reference decoder makes of the very same take, for the column beside it
                char ref[8192];
                navtex_result_t rr;
                if (Bench_NavtexDecode(scaled, m, ref, sizeof(ref), &rr))
                {
                    int rsk = 0;
                    ref_cer = navtex_score(msg, ref, &rsk);
                }
                else
                {
                    ref_cer = 100.0f;
                }
            }
        }
        free(x); free(work); free(scaled); free(got);

        const float cer = (float)spread_mean(&cs);
        const int wrong = worst_wrong;
        const int silent = (worst_used <= 2);
        int ok;
        if (c->expect_nothing)        ok = nothing_ok && cer >= 99.0f;
        else if (silent)              ok = c->expect_silent;
        else ok = cer <= c->max_cer && (c->min_cer <= 0.0f || cer >= c->min_cer)
                  && wrong <= (int)(c->max_sub + 0.5f) && (locked || c->allow_unlocked);
        if (!ok) failures++;
        printf("%-14s %6.1f %4.1f-%5.1f %8.1f %7.2f %6.1f %6u %6u %3u %5d %8.1f  %s%s\n", c->name, cer,
               cs.lo, cs.hi, cer * (double)strlen(msg) / 100.0,
               conf_sum / (float)nseeds, period_sum / (12.0f * (float)nseeds), fix / (unsigned)nseeds,
               lost / (unsigned)nseeds, rev, wrong, ref_cer, ok ? "" : "FAIL",
               c->expect_nothing ? " (nothing decoded)" : "");
    }
    printf("\n`conf` is the share of recent words that are valid 4-of-7 characters, which the receiver\n"
           "knows without being told the text: it is 1.00 when the mode has been copied and near zero\n"
           "on a take that is not SITOR at all, which is the number a capture hunt needs.  `bit ms` is\n"
           "the period the receiver measured, 10.0 ms being exactly 100 Bd.  `reference CER` is the\n"
           "offline modem in navtex.c on the same take, and the rate rows are where the two differ.\n");
    return failures ? 1 : 0;
}


static int navtex_table(void)
{
    static const char* msg = "NAVTEX TEST MESSAGE DE UHSDR BENCH 12345 CQ CQ";
    // A character is 70 ms and a block of five is 350 ms, so the message starts at 1400 ms (after the
    // twenty phasing characters) and every block is sent twice with 350 ms between the copies. The
    // burst row erases 280 ms of the first block's direct copy, which is the whole point of the mode:
    // the repeat 350 ms later is outside it.
    static const navtex_case_t cases[] =
    {
        { .name = "clean", .snr_db = 100.0f, .max_cer = 0.0f },
        { .name = "snr 20", .snr_db = 20.0f, .max_cer = 5.0f },
        { .name = "snr 10", .snr_db = 10.0f, .max_cer = 10.0f },
        { .name = "snr 10 qsb", .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 6.0f,
          .max_cer = 10.0f },
        { .name = "burst on DX", .snr_db = 20.0f, .erase_ms = 1400.0f, .erase_len_ms = 280.0f,
          .max_cer = 5.0f },
        // the same burst over both copies: 700 ms covers a direct block and its repeat, so there is
        // nothing left to combine. This is the row that says the mode has a limit rather than a
        // habit of decoding, and it is pinned from below for the same reason the wrong speed rows
        // in the RTTY matrix are.
        { .name = "burst both", .snr_db = 20.0f, .erase_ms = 1400.0f, .erase_len_ms = 700.0f,
          .max_cer = 100.0f, .min_cer = 10.0f },
        // the pair survives a tuning error until the moved tones are closer to each other's bin than
        // to their own, which for a 170 Hz shift is 85 Hz. +50 Hz is inside that and +100 is past it,
        // where the space tone reads as mark and every word inverts.
        { .name = "tune +50 Hz", .snr_db = 20.0f, .tune_hz = 50.0f, .max_cer = 5.0f },
        { .name = "tune +100 Hz", .snr_db = 20.0f, .tune_hz = 100.0f, .max_cer = 100.0f,
          .min_cer = 50.0f },
        // The other bit order. Every word is keyed most significant bit first, phasing pair included,
        // which is a take this receiver's generator did not used to be able to make at all. Nothing
        // else about the take changes: a decode that comes back as sent can only have come from the
        // phasing pair, because every reversed 4-of-7 word is another valid character - the ratio
        // that finds the boundary cannot tell the two orders apart.
        { .name = "reversed bits", .snr_db = 20.0f, .order = NAVTEX_ORDER_REVERSED, .max_cer = 5.0f },
        // The same text with the phasing left in the table's order: the receiver is told the wrong
        // order and reads every character backwards. This is the cost, so it is pinned from below.
        { .name = "wrong order", .snr_db = 20.0f, .order = NAVTEX_ORDER_MIXED, .max_cer = 95.0f,
          .min_cer = 75.0f },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("NAVTEX / SITOR-B through the reference modem: 100 Bd, 170 Hz shift, CCIR 476 4-of-7,\n"
           "each character sent twice five slots (350 ms) apart. Text is %zu characters.\n\n",
           strlen(msg));
    printf("%-14s %6s %11s %8s %6s %7s %7s %6s %6s %3s  %s\n", "case", "CER %", "min-max",
           "err/len", "lead", "bit ms", "4of7", "fix", "lost", "rev", "decoded");
    for (size_t i = 0; i < n_cases; i++)
    {
        const navtex_case_t* c = &cases[i];
        const size_t cap = 400000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        char* got = malloc(8192);
        if (!x || !got) { free(x); free(got); printf("out of memory\n"); return 1; }
        // the tones are where the transmitter put them, or moved by a reference error that the
        // receiver's 1615/1785 Hz bins do not follow
        size_t n = Bench_NavtexGenerate(x, cap, msg, c->tune_hz, c->order);
        // every noise row is a distribution of takes, not one draw: the same transmission under five
        // noise samples, and the CER printed is their mean with the seed range beside it
        const size_t need = strlen(msg);
        const int nseeds = c->snr_db < 90.0f ? TABLE_SEEDS : 1;
        float32_t* work = malloc(cap * sizeof(float32_t));
        if (!work) { free(x); free(got); printf("out of memory\n"); return 1; }
        navtex_result_t res = { 0 };
        spread_t cs = { 0 };
        char first[8192] = "";
        int all_ok = 1, skip = 0, wrong = 0;
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, n * sizeof(float32_t));
            if (nseeds > 1)
            {
                Bench_RngSeed(0x4E4156u + (uint32_t) k);
                Bench_AddNoise(work, n, c->snr_db, 0.0f);
            }
            Bench_AddQsb(work, n, c->qsb_rate, c->qsb_depth);
            if (c->erase_len_ms > 0.0f)
            {
                const size_t a = (size_t)(c->erase_ms * 12.0);
                const size_t b = a + (size_t)(c->erase_len_ms * 12.0);
                for (size_t j = a; j < b && j < n; j++) work[j] = 0.0f;
            }

            char text[8192];
            navtex_result_t r_k;
            const int ok = Bench_NavtexDecode(work, n, text, sizeof(text), &r_k);
            int skip_k = 0;
            const float cer_k = ok ? navtex_score(msg, text, &skip_k) : 100.0f;
            spread_add(&cs, cer_k);
            wrong += (int)lround((double)cer_k * (double)need / 100.0);
            if (!ok) all_ok = 0;
            if (!k) { snprintf(first, sizeof(first), "%s", text); res = r_k; skip = skip_k; }
        }
        free(work);
        const int ok = all_ok;
        const float cer = (float) spread_mean(&cs);
        char counted[32], range[32];
        spread_range(&cs, range, sizeof(range));
        snprintf(counted, sizeof(counted), "%.1f/%zu", (double) wrong / (double) nseeds, need);
        printf("%-14s %6.1f %11s %8s %6d %7.3f %7.3f %6u %6u %3d  %.44s%s\n", c->name, (double)cer,
               range, counted, skip, res.period_samples / 12.0, res.valid_fraction, res.repaired,
               res.both_bad, res.reversed, first, strlen(first) > 44 ? " ..." : "");
        if (!ok)
        {
            printf("FAIL  %-14s the receiver found no alignment to read\n", c->name);
            failures++;
        }
        else if (cer > c->max_cer)
        {
            printf("FAIL  %-14s character error rate %.1f %% is above the %.1f %% this case allows\n",
                   c->name, (double)cer, (double)c->max_cer);
            failures++;
        }
        else if (cer < c->min_cer)
        {
            printf("FAIL  %-14s character error rate %.1f %% is below the %.1f %% this case has to "
                   "stay above\n", c->name, (double)cer, (double)c->min_cer);
            failures++;
        }
        free(x); free(got);
    }
    printf("\n'4of7' is the share of the seven bit words at the fitted alignment that are members of\n"
           "the constant ratio alphabet: a wrong alignment lands on one by accident about a fifth of\n"
           "the time, a right one always, so it is the synchroniser as well as the error detector.\n"
           "'fix' counts the characters whose two copies differed, which is one copy dropped as broken\n"
           "and the other taken, and 'lost' the ones where neither copy was a word and the pair was\n"
           "too far from every valid word to correct. 'rev' is the bit order the receiver took from the\n"
           "phasing pair: 0 for the table's order, 1 for its reversal.\n");
    if (!failures)
    {
        printf("\nall %zu cases inside their character error rate limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  Hellschreiber
 * ------------------------------------------------------------------ */
// The mode with no alphabet and no error control: the transmitter draws the text as a raster of
// keyed pixels and the receiver prints the raster. There is nothing to score by character error rate,
// so the score is the picture - the share of pixels the receiver got wrong, and how many of the
// glyph cells came out exactly as drawn (a cell being exact is what "the character is legible" means
// for a mode that has no characters). A receiver needs no glyph synchroniser: a human reads the
// picture wherever it lands, so the score allows a vertical roll and a horizontal offset and reports
// which it used.
typedef struct
{
    const char* name;
    float snr_db, qsb_rate, qsb_depth;
    float tune_hz;          // the keying tone moved by this much: the receiver's envelope stays at 1 kHz
    float clk_pct;          // the transmitter's pixel clock error against the receiver's, percent
    float max_pixel;        // percent of pixels the case allows to be wrong
    int   max_missing;      // glyph cells this case may fail to reproduce exactly
    int   min_missing;      // and the cells that have to stay missing, for the rows that must fail
} hell_case_t;

// The score is the picture: the paper position is free (a whole pixel of roll and a column of
// offset), so this walks them, takes the one that leaves the most glyph cells exactly as drawn, and
// returns the share of pixels that are wrong at it.
static double hell_score(const uint8_t* truth, int truth_cols, const uint8_t* gotp, int got_cols,
                         int stride, int* cells_ok, int* cell_total, int* out_roll, int* out_shift)
{
    int best_err = -1, best_seen = 0, best_cells = 0, best_total = 0;
    int best_roll = 0, best_shift = 0;
    for (int roll = 0; roll < HELL_ROWS; roll++)
    {
        for (int cs = 0; cs < 2 * HELL_CELL; cs++)
        {
            int err = 0, seen = 0, cells = 0, total = 0;
            for (int col = 0; col + cs < truth_cols && col < got_cols; col++)
                for (int r = 0; r < HELL_ROWS; r++)
                {
                    seen++;
                    if (truth[((r + roll) % HELL_ROWS) * stride + col + cs]
                        != gotp[r * stride + col]) err++;
                }
            // a cell is the glyph, five columns: the sixth is the gap the synchroniser uses and the
            // last one can be cut off by the end of the take without costing a glyph
            for (int col = 0; col + cs + HELL_COLS <= truth_cols && col + HELL_COLS <= got_cols;
                 col += HELL_CELL)
            {
                total++;
                int same = 1;
                for (int cc = 0; cc < HELL_COLS && same; cc++)
                    for (int r = 0; r < HELL_ROWS && same; r++)
                        if (truth[((r + roll) % HELL_ROWS) * stride + col + cs + cc]
                            != gotp[r * stride + col + cc]) same = 0;
                if (same) cells++;
            }
            if (cells > best_cells || (cells == best_cells && (best_err < 0 || err < best_err)))
            {
                best_cells = cells;
                best_total = total;
                best_err = err;
                best_seen = seen;
                best_roll = roll;
                best_shift = cs;
            }
        }
    }
    *cells_ok = best_cells;
    *cell_total = best_total;
    *out_roll = best_roll;
    *out_shift = best_shift;
    return (best_seen && best_err >= 0) ? 100.0 * (double) best_err / (double) best_seen : 100.0;
}

static int hell_table(void)
{
    static const char* msg = "HELLSCHREIBER TEST DE UHSDR 123";
    enum { MAXC = 640 };
    static uint8_t truth[HELL_ROWS * MAXC], gotp[HELL_ROWS * MAXC];
    static const hell_case_t cases[] =
    {
        { .name = "clean", .snr_db = 100.0f, .max_pixel = 0.0f, .max_missing = 0 },
        { .name = "snr 20", .snr_db = 20.0f, .max_pixel = 0.0f, .max_missing = 0 },
        { .name = "snr 10", .snr_db = 10.0f, .max_pixel = 0.0f, .max_missing = 0 },
        { .name = "snr 10 qsb", .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 6.0f,
          .max_pixel = 0.0f, .max_missing = 0 },
        { .name = "snr 0", .snr_db = 0.0f, .max_pixel = 100.0f, .max_missing = 31 },
        { .name = "tone +200 Hz", .snr_db = 20.0f, .tune_hz = 200.0f, .max_pixel = 0.0f,
          .max_missing = 0 },
        // the two that have to fail: a keying tone out of the envelope's reach, and a transmitter
        // whose pixel clock the receiver does not follow, which shears the picture. Both are pinned
        // from below, like the RTTY rows that have to keep costing characters.
        { .name = "tone +700 Hz", .snr_db = 20.0f, .tune_hz = 700.0f, .max_pixel = 100.0f,
          .max_missing = 31, .min_missing = 15 },
        { .name = "clk +2 %", .snr_db = 20.0f, .clk_pct = 2.0f, .max_pixel = 100.0f,
          .max_missing = 31, .min_missing = 15 },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("Hellschreiber through the reference modem: 122.5 Bd on/off keying, one pixel per 8.16 ms,\n"
           "5x7 glyph plus a blank column. Text is %zu characters, %d glyph cells.\n\n",
           strlen(msg), 31);
    printf("%-13s %7s %11s %9s %7s %6s %5s %5s  %s\n", "case", "pixels%", "min-max",
           "cells ok", "pix ms", "gaps", "roll", "shift", "sync");
    for (size_t i = 0; i < n_cases; i++)
    {
        const hell_case_t* c = &cases[i];
        const size_t cap = 300000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        float32_t* work = malloc(cap * sizeof(float32_t));
        int truth_cols = 0, got_cols = 0;
        hell_result_t res = { 0 };
        if (!x || !work) { free(x); free(work); printf("out of memory\n"); return 1; }
        const size_t n = Bench_HellGenerate(x, cap, msg, truth, MAXC, &truth_cols,
                                            1000.0f + c->tune_hz, c->clk_pct);
        if (!n)
        {
            free(x); free(work);
            printf("FAIL  %-13s nothing generated\n", c->name);
            failures++;
            continue;
        }

        // every noise row is a distribution of takes, not one draw: the same picture under five
        // noise samples, and the row prints their mean with the seed range beside it
        const int nseeds = c->snr_db < 90.0f ? TABLE_SEEDS : 1;
        spread_t pix_s = { 0 };
        int all_ok = 1, cell_total = 0, cells_ok = 0, missing = 0, roll = 0, shift = 0;
        double pix_ms = 0.0, gaps = 0.0;
        int phase = 0;
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, n * sizeof(float32_t));
            if (nseeds > 1)
            {
                Bench_RngSeed(0x48454C4Cu + (uint32_t) k);
                Bench_AddNoise(work, n, c->snr_db, 0.0f);
            }
            Bench_AddQsb(work, n, c->qsb_rate, c->qsb_depth);

            const int ok = Bench_HellDecode(work, n, gotp, MAXC, &got_cols, &res);
            int cells = 0, total = 0, r_roll = 0, r_shift = 0;
            const double pixel_pct = ok ? hell_score(truth, truth_cols, gotp, got_cols, MAXC,
                                                     &cells, &total, &r_roll, &r_shift) : 100.0;
            if (!ok) all_ok = 0;
            spread_add(&pix_s, pixel_pct);
            // the worst seed is what the cell limit pins. Each seed's own total is used, because a
            // paper shift of one column leaves the last glyph out of the count on that seed, and a
            // glyph that was counted nowhere is not one the receiver got wrong.
            if (total - cells > missing) missing = total - cells;
            if (!k)
            {
                pix_ms = res.pixel_samples / 12.0;
                gaps = res.gap_share;
                phase = res.pixel_phase;
                cells_ok = cells;
                cell_total = total;
                roll = r_roll;
                shift = r_shift;
            }
        }
        free(work);
        const double pixel_pct = spread_mean(&pix_s);
        char range[32];
        spread_range(&pix_s, range, sizeof(range));
        if (all_ok)
        {
            printf("%-13s %7.2f %11s %5d/%-3d %7.2f %6.2f %5d %5d  phase %d\n", c->name, pixel_pct,
                   range, cells_ok, cell_total, pix_ms, gaps, roll, shift, phase);
        }
        else
        {
            printf("%-13s %7.2f %11s %9s %7s %6s %5s %5s  no raster on one seed\n", c->name,
                   100.0, range, "-", "-", "-", "-", "-");
        }
        if (!all_ok)
        {
            printf("FAIL  %-13s the receiver found no raster on one of %d seeds\n", c->name, nseeds);
            failures++;
        }
        else if (pixel_pct > (double) c->max_pixel + 1e-9 || missing > c->max_missing)
        {
            printf("FAIL  %-13s mean %.2f %% of pixels wrong and %d of %d cells not exact; this "
                   "case allows %.2f %% and %d\n", c->name, pixel_pct, missing, cell_total,
                   (double)c->max_pixel, c->max_missing);
            failures++;
        }
        else if (missing < c->min_missing)
        {
            printf("FAIL  %-13s %d of %d cells not exact, and this case has to stay above %d: a "
                   "channel\n      that stopped costing the picture is a different receiver\n",
                   c->name, missing, cell_total, c->min_missing);
            failures++;
        }
        free(x);
    }
    printf("\n'cells ok' counts the 5x7 glyphs that came out exactly as drawn: a mode with no alphabet\n"
           "has no character error rate, and a cell that is not exact is a character a reader would\n"
           "have to guess at. 'gaps' is the share of the gap columns that came out blank, which is what\n"
           "the grouping search maximises; 'roll' and 'shift' are the paper position the score allowed.\n");
    if (!failures)
    {
        printf("\nall %zu cases inside their pixel and cell limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

// The two tone ladders score a take the same way: every character of the message that is missing or
// wrong, plus every character the receiver added. A failed decode is the whole message wrong.
static int exact_char_errors(const char* want, const char* got, int ok)
{
    if (!ok) return (int)strlen(want);
    const size_t len = strlen(want), glen = strlen(got);
    int err = 0;
    for (size_t k = 0; k < len; k++) if (k >= glen || got[k] != want[k]) err++;
    for (size_t k = len; k < glen; k++) err++;
    return err;
}

/* ------------------------------------------------------------------ *
 *  MFSK16
 * ------------------------------------------------------------------ */
// Sixteen tones at 15.625 baud is four bits a symbol and 64 ms of signal per symbol, which is a lot
// of energy per decision; what this table is really measuring is the coding. A symbol the receiver
// loses costs four bits, and the interleaver is what turns that into one bit in eight symbols, which
// the rate 1/2 code can carry. The rows are therefore ordered from the clean case to the ones where
// the correction is doing the work, and the last two are the ones that have to cost characters.
typedef struct
{
    const char* name;
    float snr_db, qsb_rate, qsb_depth, tune_hz;
    int   burst_symbols;    // symbols erased in the middle of the take
    int   no_signal;        // a take with nothing but noise in it: the receiver must not invent text
    float max_cer;
    float floor_seeds[TABLE_SEEDS]; // the five draws this row was measured with. The floor is the
                                    // lower 95 % bound of their mean (floor_from_seeds), not a number
                                    // picked by hand: all zero means the row has no floor
} mfsk16_case_t;

static int mfsk16_table(void)
{
    static const char* msg = "MFSK16 TEST DE UHSDR 12345";
    const size_t len = strlen(msg);
    static const mfsk16_case_t cases[] =
    {
        { .name = "clean", .snr_db = 100.0f, .max_cer = 0.0f },
        { .name = "snr 20", .snr_db = 20.0f, .max_cer = 0.0f },
        { .name = "snr 10", .snr_db = 10.0f, .max_cer = 0.0f },
        { .name = "snr 10 qsb", .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 3.0f,
          .max_cer = 100.0f },
        { .name = "snr 3", .snr_db = 3.0f, .max_cer = 0.0f },
        { .name = "snr -6", .snr_db = -6.0f, .max_cer = 0.0f },
        { .name = "snr -12", .snr_db = -12.0f, .max_cer = 0.0f },
        { .name = "snr -16", .snr_db = -16.0f, .max_cer = 0.0f },
        // the floor, measured over five seeds: 36.9 % mean (0.0-103.8), so the old 0.0 % was one
        // lucky draw. Two of the five draws decode exactly, so the lower bound of the mean is below
        // zero and the row cannot be pinned from below at all - it says 0.0 and the README does too.
        { .name = "snr -18", .snr_db = -18.0f, .max_cer = 70.0f,
          .floor_seeds = { 0.00f, 7.69f, 73.08f, 0.00f, 103.85f } },
        { .name = "snr -20", .snr_db = -20.0f, .max_cer = 100.0f,
          .floor_seeds = { 61.54f, 100.00f, 96.15f, 80.77f, 96.15f } },
        { .name = "snr -22", .snr_db = -22.0f, .max_cer = 110.0f,
          .floor_seeds = { 100.00f, 100.00f, 100.00f, 92.31f, 100.00f } },
        // a burst that takes two whole symbols: eight bits gone, spread over eight symbols by the
        // interleaver, which is what the code is there to repair
        { .name = "burst 2 symbols", .snr_db = 20.0f, .burst_symbols = 2, .max_cer = 0.0f },
        { .name = "burst 8 symbols", .snr_db = 20.0f, .burst_symbols = 8, .max_cer = 100.0f,
          .floor_seeds = { 42.31f, 11.54f, 11.54f, 15.38f, 53.85f } },
        { .name = "tone +8 Hz", .snr_db = 20.0f, .tune_hz = 8.0f, .max_cer = 100.0f,
          .floor_seeds = { 92.31f, 100.00f, 92.31f, 88.46f, 92.31f } },
        { .name = "tone +16 Hz", .snr_db = 20.0f, .tune_hz = 16.0f, .max_cer = 130.0f,
          .floor_seeds = { 119.23f, 119.23f, 119.23f, 100.00f, 100.00f } },
        // nothing transmitted at all: a receiver that reads a message out of noise would look like
        // an improvement on every other row here, so this row's floor is the whole message
        { .name = "noise only", .snr_db = 0.0f, .no_signal = 1, .max_cer = 100.0f,
          .floor_seeds = { 100.00f, 100.00f, 100.00f, 100.00f, 100.00f } },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("MFSK16 through the reference modem: 16 tones 15.625 Hz apart at 15.625 Bd, 4 bits a symbol,\n"
           "rate 1/2 K=7 code, interleaver over 8 symbols. Text is %zu characters.\n", len);
    printf("The alphabet is the mode's own IZ8BLY Varicode, framed by its 00 separators, so these\n"
           "rows measure the real alphabet and the coding; the eight symbol interleaver is the\n"
           "bench's, and the README says so.\n\n");
    printf("%-16s %6s %11s %8s %8s %8s %6s %s\n", "case", "CER %", "min-max", "err/len",
           "symbols", "heard", "bits", "decoded text");
    for (size_t i = 0; i < n_cases; i++)
    {
        const mfsk16_case_t* c = &cases[i];
        const size_t cap = 400000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        float32_t* work = malloc(cap * sizeof(float32_t));
        char got[512];
        if (!x || !work) { free(x); free(work); printf("out of memory\n"); return 1; }
        size_t n = Bench_Mfsk16Generate(x, cap, msg, c->tune_hz);
        if (c->no_signal)
        {
            memset(x, 0, n * sizeof(float32_t));      // the same length of silence, then noise
        }
        else if (!n) { free(x); free(work); printf("FAIL  %-16s nothing generated\n", c->name); failures++; continue; }
        if (c->burst_symbols)
        {
            // the middle of the take, which is inside the text for this message
            const size_t mid = (n / 2) & ~(size_t)63;
            const size_t wide = (size_t)c->burst_symbols * 768;
            for (size_t k = mid; k < mid + wide && k < n; k++) x[k] = 0.0f;
        }

        // every noise row is a distribution of takes, not one draw: the same signal, five seeds
        const int nseeds = c->snr_db < 90.0f ? TABLE_SEEDS : 1;
        double sum = 0.0, worst = 0.0, best = 100.0;
        int ok_all = 1;
        unsigned sym_total = 0, heard = 0, vbits = 0;
        char text[512] = "";
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, n * sizeof(float32_t));
            if (c->snr_db < 90.0f)
            {
                Bench_RngSeed(0x4D46534Bu + (uint32_t) k);
                Bench_AddNoise(work, n, c->snr_db, 0.0f);
            }
            Bench_AddQsb(work, n, c->qsb_rate, c->qsb_depth);

            mfsk16_result_t res;
            const int ok = Bench_Mfsk16Decode(work, n, got, sizeof(got), &res);
            const int err = exact_char_errors(msg, got, ok);
            const double cer = 100.0 * (double)err / (double)len;
            if (k == 0)
            {
                snprintf(text, sizeof(text), "%s", ok ? got : "(no decode)");
                vbits = res.viterbi_bits;
            }
            heard += res.tones_ok;
            sym_total += res.symbols;
            sum += cer;
            if (cer < best) best = cer;
            if (cer > worst) worst = cer;
            if (!ok) ok_all = 0;
        }
        const double mean = sum / (double)nseeds;
        const double mean_err = mean * (double)len / 100.0;
        const double floor_cer = floor_from_seeds(c->floor_seeds);
        printf("%-16s %6.1f %5.1f-%5.1f %4.1f/%-3zu %8.0f %5.2f%% %6u %s\n", c->name, mean, best,
               worst, mean_err, len, (double)sym_total / (double)nseeds,
               sym_total ? 100.0 * (double)heard / (double)sym_total : 0.0, vbits, text);
        if (!ok_all)
        {
            printf("FAIL  %-16s the receiver did not get through the stream on one of %d seeds\n",
                   c->name, nseeds);
            failures++;
        }
        else if (mean > (double)c->max_cer + 1e-9)
        {
            printf("FAIL  %-16s mean %.1f %% of characters wrong over %d seeds (range %.1f-%.1f), "
                   "this case allows %.1f %%\n", c->name, mean, nseeds, best, worst,
                   (double)c->max_cer);
            failures++;
        }
        else if (mean < floor_cer - 1e-9)
        {
            printf("FAIL  %-16s mean %.1f %% wrong over %d seeds (range %.1f-%.1f), and this case "
                   "has to stay above %.1f %% -\n      the lower 95 %% bound of the mean of the "
                   "five draws it was measured with. A channel that stopped\n      costing characters "
                   "is a different receiver.\n", c->name, mean, nseeds, best, worst, floor_cer);
            failures++;
        }
        free(x); free(work);
    }
    printf("\n'heard' is the share of symbols whose winning tone held more than half the energy: a\n"
           "symbol that is heard is not necessarily right, but a symbol that is not heard was lost to\n"
           "the channel. The burst row erases two whole symbols and is the interleaver at work; the\n"
           "tone rows are the 15.625 Hz grid, where a full tone spacing of error puts the signal on a\n"
           "neighbour's frequency.\n");
    if (!failures)
    {
        printf("\nall %zu cases inside their character error limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  Olivia
 * ------------------------------------------------------------------ */
// Thirty-two tones at 31.25 baud is 1000 Hz of signal, one tone a symbol, five bits a symbol. A
// character is a Walsh function of sixty four bits, so it is spread over a block of sixty four
// symbols - five characters a block - and one symbol lost to a burst is one bit in each of five
// codewords rather than five bits in one. The rows below are the same ladder as MFSK16's, so the
// two tables can be compared: the same channel, the same text, the same scoring.
typedef struct
{
    const char* name;
    float snr_db, qsb_rate, qsb_depth, tune_hz;
    int   burst_symbols;
    int   no_signal;
    float max_cer;
    float floor_seeds[TABLE_SEEDS]; // the five draws this row was measured with, as in MFSK16
} olivia_case_t;

static int olivia_table(void)
{
    static const char* msg = "OLIVIA TEST DE UHSDR 12345";
    const size_t len = strlen(msg);
    static const olivia_case_t cases[] =
    {
        { .name = "clean", .snr_db = 100.0f, .max_cer = 0.0f },
        { .name = "snr 20", .snr_db = 20.0f, .max_cer = 0.0f },
        { .name = "snr 10", .snr_db = 10.0f, .max_cer = 0.0f },
        { .name = "snr 10 qsb", .snr_db = 10.0f, .qsb_rate = 0.5f, .qsb_depth = 3.0f,
          .max_cer = 100.0f },
        { .name = "snr 0", .snr_db = 0.0f, .max_cer = 0.0f },
        { .name = "snr -6", .snr_db = -6.0f, .max_cer = 100.0f },
        { .name = "snr -12", .snr_db = -12.0f, .max_cer = 0.0f },
        { .name = "snr -14", .snr_db = -14.0f, .max_cer = 0.0f },
        { .name = "snr -16", .snr_db = -16.0f, .max_cer = 0.0f },
        // the floor: the first row where characters start to go, and the next one well past it
        // measured over five seeds: 20.8 % mean (11.5-34.6), and 73.1 % (50.0-96.2) below it
        { .name = "snr -18", .snr_db = -18.0f, .max_cer = 45.0f,
          .floor_seeds = { 34.62f, 19.23f, 23.08f, 11.54f, 15.38f } },
        { .name = "snr -20", .snr_db = -20.0f, .max_cer = 90.0f,
          .floor_seeds = { 80.77f, 96.15f, 69.23f, 50.00f, 69.23f } },
        { .name = "burst 2 symbols", .snr_db = 20.0f, .burst_symbols = 2, .max_cer = 0.0f },
        { .name = "burst 8 symbols", .snr_db = 20.0f, .burst_symbols = 8, .max_cer = 0.0f },
        { .name = "tone +15 Hz", .snr_db = 20.0f, .tune_hz = 15.0f, .max_cer = 0.0f },
        { .name = "noise only", .snr_db = 0.0f, .no_signal = 1, .max_cer = 100.0f,
          .floor_seeds = { 100.00f, 100.00f, 100.00f, 100.00f, 100.00f } },
    };
    const size_t n_cases = sizeof(cases) / sizeof(cases[0]);
    int failures = 0;

    printf("Olivia through the reference modem: 32 tones 31.25 Hz apart at 31.25 Bd, one tone a\n"
           "symbol, 64 symbol Walsh blocks of five characters, Gray coded. Text is %zu characters.\n",
           len);
    printf("The Walsh rows, the scrambler and the diagonal spread are the mode's own; the length in\n"
           "the first two characters is the bench's, because Olivia has no framing.\n\n");
    printf("%-16s %6s %11s %8s %8s %8s %s\n", "case", "CER %", "min-max", "err/len",
           "symbols", "heard", "decoded text");
    for (size_t i = 0; i < n_cases; i++)
    {
        const olivia_case_t* c = &cases[i];
        const size_t cap = 200000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        float32_t* work = malloc(cap * sizeof(float32_t));
        char got[512];
        if (!x || !work) { free(x); free(work); printf("out of memory\n"); return 1; }
        const size_t n = Bench_OliviaGenerate(x, cap, msg, c->tune_hz);
        if (c->no_signal)
        {
            memset(x, 0, n * sizeof(float32_t));
        }
        else if (!n) { free(x); free(work); printf("FAIL  %-16s nothing generated\n", c->name); failures++; continue; }
        if (c->burst_symbols)
        {
            const size_t mid = (n / 2) & ~(size_t)63;
            const size_t wide = (size_t)c->burst_symbols * 384;
            for (size_t k = mid; k < mid + wide && k < n; k++) x[k] = 0.0f;
        }

        const int nseeds = c->snr_db < 90.0f ? TABLE_SEEDS : 1;
        double sum = 0.0, worst = 0.0, best = 100.0;
        int ok_all = 1;
        unsigned sym_total = 0, heard = 0;
        char text[512] = "";
        for (int k = 0; k < nseeds; k++)
        {
            memcpy(work, x, n * sizeof(float32_t));
            if (c->snr_db < 90.0f)
            {
                Bench_RngSeed(0x4F4C4956u + (uint32_t) k);
                Bench_AddNoise(work, n, c->snr_db, 0.0f);
            }
            Bench_AddQsb(work, n, c->qsb_rate, c->qsb_depth);

            olivia_result_t res;
            const int ok = Bench_OliviaDecode(work, n, got, sizeof(got), &res);
            const int err = exact_char_errors(msg, got, ok);
            const double cer = 100.0 * (double)err / (double)len;
            if (k == 0)
            {
                snprintf(text, sizeof(text), "%s", ok ? got : "(no decode)");
            }
            heard += res.tones_ok;
            sym_total += res.symbols;
            sum += cer;
            if (cer < best) best = cer;
            if (cer > worst) worst = cer;
            if (!ok) ok_all = 0;
        }
        const double mean = sum / (double)nseeds;
        const double mean_err = mean * (double)len / 100.0;
        // the floor is a property of the distribution the row was measured with, not of this run
        const double floor_cer = floor_from_seeds(c->floor_seeds);
        printf("%-16s %6.1f %5.1f-%5.1f %4.1f/%-3zu %8.0f %5.2f%% %s\n", c->name, mean, best, worst,
               mean_err, len, (double)sym_total / (double)nseeds,
               sym_total ? 100.0 * (double)heard / (double)sym_total : 0.0, text);
        if (!ok_all)
        {
            printf("FAIL  %-16s the receiver did not get through the stream on one of %d seeds\n",
                   c->name, nseeds);
            failures++;
        }
        else if (mean > (double)c->max_cer + 1e-9)
        {
            printf("FAIL  %-16s mean %.1f %% of characters wrong over %d seeds (range %.1f-%.1f), "
                   "this case allows %.1f %%\n", c->name, mean, nseeds, best, worst,
                   (double)c->max_cer);
            failures++;
        }
        else if (mean < floor_cer - 1e-9)
        {
            printf("FAIL  %-16s mean %.1f %% wrong over %d seeds (range %.1f-%.1f), and this case "
                   "has to stay above %.1f %% -\n      the lower 95 %% bound of the mean of the "
                   "five draws it was measured with. A channel that stopped\n      costing characters "
                   "is a different receiver.\n", c->name, mean, nseeds, best, worst, floor_cer);
            failures++;
        }
        free(x); free(work);
    }
    printf("\n'heard' is the share of symbols whose winning tone held more than half the energy\n"
           "over the thirty-two: a symbol that is not heard was lost to the channel. The noise only\n"
           "row is the one that must never read as text, and the rows above it are the reason the\n"
           "mode is used where the channel is bad rather than where it is good.\n");
    if (!failures)
    {
        printf("\nall %zu cases inside their character error limits\n", n_cases);
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  the 2.5 kHz channel, measured rather than converted
 * ------------------------------------------------------------------ */
// The MFSK16 and Olivia sections quote a 2.5 kHz floor as the 12 kHz floor plus
// 10*log10(12000/2500) = 6.8 dB. That is arithmetic: it assumes the receiver rejects every hertz
// outside the channel, and no row measures it. This reruns the same takes, seeds and scoring with
// noise shaped to a 2.5 kHz band (500..3000 Hz, where both tone grids sit) at the channel SNR the
// arithmetic claims, so the README's number comes from audio rather than from a ratio.
// One point of the shaped channel: the same take under FLOOR_SEEDS noise seeds. `cer` comes back
// sorted from the least to the most wrong draw and `median_text` is the take decoded on the median
// draw, because a mean of a row this bimodal describes no take that was ever decoded. Returns 0 only
// when it could not measure at all.
static int floors_2k5_point(const char* mode, float snr_db, double* cer, char* median_text,
                            size_t text_sz)
{
    static const char* mmsg = "MFSK16 TEST DE UHSDR 12345";
    static const char* omsg = "OLIVIA TEST DE UHSDR 12345";
    const int mfsk = mode[0] == 'M';
    const char* msg = mfsk ? mmsg : omsg;
    const size_t len = strlen(msg);
    const size_t cap = 400000;
    float32_t* x = malloc(cap * sizeof(float32_t));
    float32_t* work = malloc(cap * sizeof(float32_t));
    static char seen[FLOOR_SEEDS][512];
    char got[512];
    if (!x || !work) { free(x); free(work); printf("%-8s out of memory\n", mode); return 0; }
    const size_t n = mfsk ? Bench_Mfsk16Generate(x, cap, msg, 0.0f)
                          : Bench_OliviaGenerate(x, cap, msg, 0.0f);
    for (int k = 0; k < FLOOR_SEEDS; k++)
    {
        memcpy(work, x, n * sizeof(float32_t));
        Bench_RngSeed((mfsk ? 0x4D46534Bu : 0x4F4C4956u) + (uint32_t) k);
        Bench_AddNoiseBand(work, n, snr_db, 0.0f, 500.0f, 3000.0f);
        int ok;
        if (mfsk)
        {
            mfsk16_result_t r;
            ok = Bench_Mfsk16Decode(work, n, got, sizeof(got), &r);
        }
        else
        {
            olivia_result_t r;
            ok = Bench_OliviaDecode(work, n, got, sizeof(got), &r);
        }
        const int err = exact_char_errors(msg, got, ok);
        cer[k] = 100.0 * (double) err / (double) len;
        snprintf(seen[k], sizeof(seen[k]), "%s", ok ? got : "(no decode)");
    }
    free(x);
    free(work);

    int idx[FLOOR_SEEDS];
    for (int i = 0; i < FLOOR_SEEDS; i++) idx[i] = i;
    for (int i = 1; i < FLOOR_SEEDS; i++)
        for (int j = i; j > 0 && cer[idx[j]] < cer[idx[j - 1]]; j--)
        {
            const int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    double sorted[FLOOR_SEEDS];
    for (int i = 0; i < FLOOR_SEEDS; i++) sorted[i] = cer[idx[i]];
    memcpy(cer, sorted, sizeof(sorted));
    snprintf(median_text, text_sz, "%s", seen[idx[FLOOR_SEEDS / 2]]);
    return 1;
}

static void floors_2k5_row(const char* mode, float snr_db)
{
    const size_t len = 26;              // both messages are 26 characters, which is the scoring step
    double cer_s[FLOOR_SEEDS];
    char text[512];
    if (!floors_2k5_point(mode, snr_db, cer_s, text, sizeof(text))) return;
    double sum = 0.0;
    for (int k = 0; k < FLOOR_SEEDS; k++) sum += cer_s[k];
    const double mean = sum / (double) FLOOR_SEEDS;
    char range[32];
    snprintf(range, sizeof(range), "%.1f..%.1f", cer_s[0], cer_s[FLOOR_SEEDS - 1]);
    printf("%-8s %9.1f %9.1f %8.1f %14s %6.1f %4.1f/%-3zu %s\n", mode, snr_db, snr_db - 6.8,
           cer_s[FLOOR_SEEDS / 2], range, mean, mean * (double) len / 100.0, len, text);
}

static int floors_2k5(void)
{
    printf("The floors over a 2.5 kHz channel, measured with noise shaped to 500..3000 Hz.\n");
    printf("The 12k dB column is the 12 kHz SNR the arithmetic maps each channel SNR onto; the CER\n"
           "is what that SNR actually does to the same take through the same receiver, over %d noise\n"
           "seeds: the median of them, the range they span and their mean, with the text of the draw\n"
           "on the median. The coast is bimodal, so the median and the range are the reading and the\n"
           "mean is printed only to show how little a mean of this shape means.\n\n", FLOOR_SEEDS);
    printf("%-8s %9s %9s %8s %14s %6s %8s %s\n", "mode", "2.5k dB", "12k dB", "median %",
           "min..max", "mean %", "err/len", "decoded (median draw)");
    static const struct { const char* mode; float snr; } mapped[] =
    {
        { "MFSK16", -11.2f }, { "MFSK16", -13.2f }, { "MFSK16", -15.2f },
        { "Olivia",  -9.2f }, { "Olivia", -11.2f }, { "Olivia", -13.2f },
    };
    for (size_t i = 0; i < sizeof(mapped) / sizeof(mapped[0]); i++)
    {
        floors_2k5_row(mapped[i].mode, mapped[i].snr);
    }
    printf("\nthe same two channels in 0.5 dB steps, to see where the floor actually is:\n\n");
    printf("%-8s %9s %9s %8s %14s %6s %8s %s\n", "mode", "2.5k dB", "12k dB", "median %",
           "min..max", "mean %", "err/len", "decoded (median draw)");
    for (float s = -10.0f; s >= -16.0f - 1e-6f; s -= 0.5f) floors_2k5_row("MFSK16", s);
    printf("\n");
    for (float s = -8.0f; s >= -14.0f - 1e-6f; s -= 0.5f) floors_2k5_row("Olivia", s);
    printf("\nThe 12 kHz rows in the tables are white noise over the whole band; these are the same\n"
           "channel power confined to 2.5 kHz. A receiver that rejected all out-of-band noise would\n"
           "read the same CER at the mapped pair; a difference is the receiver's real skirt.\n"
           "\nThe points this prints are pinned by \"digi_bench floors-2k5-check\", so they are a\n"
           "checked measurement rather than a number in the README; see that verb for the windows.\n");
    return 0;
}

// The 2.5 kHz floors the MFSK16 and Olivia sections quote, against the readings those sections
// document. Each point is measured again (twelve noise seeds) and has to land in a window around
// its documented median and its documented range, so a receiver change that moved the coast fails
// here instead of leaving the README quietly wrong. The window is one character of the 26 character
// take (3.85 %) for the median, which is the finest step this score has, and two characters for the
// two extreme draws, which one flipped symbol moves further than the median.
typedef struct
{
    const char* mode;
    float snr_db;
    float median, lo, hi;   // the documented reading: median draw, least wrong draw, worst draw
} floor_pin_t;

static int floors_2k5_check(void)
{
    static const floor_pin_t pins[] =
    {
        // the three 12 kHz equivalents the arithmetic maps onto, and where the median first moves
        { "MFSK16", -11.2f,   0.0f,   0.0f,   0.0f },
        { "MFSK16", -13.2f,   0.0f,   0.0f, 100.0f },
        { "MFSK16", -15.2f,  96.2f,  50.0f, 103.8f },
        { "MFSK16", -12.5f,   0.0f,   0.0f,  38.5f },
        { "MFSK16", -13.5f,   7.7f,   0.0f, 103.8f },
        { "MFSK16", -14.5f, 100.0f,  23.1f, 111.5f },
        { "Olivia",  -9.2f,   0.0f,   0.0f,   0.0f },
        { "Olivia", -11.2f,   0.0f,   0.0f,   0.0f },
        { "Olivia", -12.5f,   0.0f,   0.0f,  11.5f },
        { "Olivia", -13.0f,   3.8f,   0.0f,  88.5f },
        { "Olivia", -13.2f,   3.8f,   0.0f,  92.3f },
    };
    const double step = 100.0 / 26.0;          // one character of either take
    const double med_tol = step;
    const double edge_tol = 2.0 * step;
    int failures = 0;

    printf("The shaped 2.5 kHz floors against their documented readings, %d noise seeds a point.\n"
           "median +- %.1f %% (one character), extremes +- %.1f %% (two).\n\n", FLOOR_SEEDS,
           med_tol, edge_tol);
    printf("%-8s %8s %9s %15s %15s  %s\n", "mode", "2.5k dB", "median %", "range",
           "documented", "check");
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
    {
        const floor_pin_t* p = &pins[i];
        double cer_s[FLOOR_SEEDS];
        char text[512];
        if (!floors_2k5_point(p->mode, p->snr_db, cer_s, text, sizeof(text)))
        {
            printf("FAIL  %-8s %8.1f could not be measured\n", p->mode, (double) p->snr_db);
            failures++;
            continue;
        }
        const double median = cer_s[FLOOR_SEEDS / 2], lo = cer_s[0], hi = cer_s[FLOOR_SEEDS - 1];
        char range[32], documented[32];
        snprintf(range, sizeof(range), "%.1f..%.1f", lo, hi);
        snprintf(documented, sizeof(documented), "%.1f..%.1f", (double) p->lo, (double) p->hi);
        printf("%-8s %8.1f %9.1f %15s %8.1f..%-5.1f %s\n", p->mode, (double) p->snr_db, median,
               range, (double) p->lo, (double) p->hi,
               fabs(median - (double) p->median) <= med_tol
               && fabs(lo - (double) p->lo) <= edge_tol
               && fabs(hi - (double) p->hi) <= edge_tol ? "ok" : "FAIL");
        if (fabs(median - (double) p->median) > med_tol)
        {
            printf("FAIL  %-8s %8.1f dB median draw is %.1f %%, the section quotes %.1f %% (window "
                   "%.1f..%.1f)\n", p->mode, (double) p->snr_db, median, (double) p->median,
                   (double) p->median - med_tol, (double) p->median + med_tol);
            failures++;
        }
        if (fabs(lo - (double) p->lo) > edge_tol || fabs(hi - (double) p->hi) > edge_tol)
        {
            printf("FAIL  %-8s %8.1f dB range is %s, the section quotes %s (window %.1f..%.1f)\n",
                   p->mode, (double) p->snr_db, range, documented, (double) p->lo - edge_tol,
                   (double) p->hi + edge_tol);
            failures++;
        }
    }
    if (!failures)
    {
        printf("\nall %zu shaped floor points inside their documented windows\n",
               sizeof(pins) / sizeof(pins[0]));
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  decoding a recorded file: the input path any real feed will use
 * ------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// One WAV reader feeds every verb here: cap_load, in the capture gate below, walks the RIFF chunks
// and hands back 16 bit PCM as floats with its rate and channel count. This section used to carry a
// second, slightly different parser, so a file the gate could read was a file the decoder rejected.
typedef struct { float* x; size_t n; uint32_t rate; uint16_t ch; } cap_t;
static int cap_load(const char* path, cap_t* c);

// Linear resampling to the 12 kHz the demodulators are pinned at. Interpolation cannot overshoot the
// source, so a gain taken from the source peak still lands on the same level after the rate change.
static float* resample_12k(const float* x, size_t n, uint32_t rate, size_t* out_n)
{
    const size_t m = n ? (size_t)llround((double)n * 12000.0 / (double)rate) : 0;
    float* y = malloc((m ? m : 1) * sizeof(float));
    *out_n = 0;
    if (!y) return NULL;
    for (size_t k = 0; k < m; k++)
    {
        const double src = (double)k * (double)rate / 12000.0;
        size_t a = (size_t)src;
        if (a >= n) a = n - 1;
        const size_t b = a + 1 < n ? a + 1 : a;
        const double fr = b == a ? 0.0 : src - (double)a;
        y[k] = (float)(x[a] + fr * (x[b] - x[a]));
    }
    *out_n = m;
    return y;
}

static float peak_of(const float* x, size_t n)
{
    float peak = 0.0f;
    for (size_t i = 0; i < n; i++) if (fabsf(x[i]) > peak) peak = fabsf(x[i]);
    return peak;
}

// 16 bit PCM WAV, mono or stereo, any sample rate: resampled to the 12 kHz the demodulator is
// pinned at, and gain trimmed from the file peak so a quiet recording still reaches the modems
// threshold. This is the hook for real audio (a WebSDR capture, another receivers file export,
// or an on-air recording), which is the whole reason the bench exists.
// trace starts the decoder's bit trace before the first sample, so a mode can print which bit of
// which character was read wrong instead of inferring it from the letters that came out
static float decode_wav(const char* path, const char* expected, char* got, size_t got_sz, uint16_t shift, uint16_t baud, bool trace, char* raw, size_t raw_sz)
{
    cap_t c;
    if (!cap_load(path, &c)) return -1.0f;
    size_t n12 = 0;
    float* x = resample_12k(c.x, c.n, c.rate, &n12);
    const float peak = peak_of(c.x, c.n);
    const float gain = peak > 0.0f ? 0.7f / peak : 1.0f;
    const double secs = (double)c.n / (double)c.rate;
    const uint32_t rate = c.rate;
    const uint16_t ch = c.ch;
    free(c.x);
    if (!x || !n12) { free(x); printf("%s: no samples to decode\n", path); return -1.0f; }

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
        free(x);
        return -1.0f;
    }
    if (baud != 45 && baud != 50)
    {
        printf("%u baud is not supported, rtty_speeds[] only holds 45.45 and 50\n", (unsigned)baud);
        free(x);
        return -1.0f;
    }
    rtty_ctrl_config.speed_idx = baud == 50 ? RTTY_SPEED_50 : RTTY_SPEED_45;
    rtty_ctrl_config.shift_idx = RTTY_SHIFT_170;
    for (int k = 0; k < RTTY_SHIFT_NUM; k++)
    {
        if (rtty_shifts[k].value == (float32_t)shift) rtty_ctrl_config.shift_idx = (rtty_shift_t)k;
    }
    // a file is a fresh receiver: reset before init, as rtty_run does, because the init is what
    // puts the filter state and its pointers back. Without this the decode starts from whatever
    // the last case left behind, so the check quietly depended on which case ran before it.
    Rtty_BenchResetDecoder();
    Rtty_Modem_Init(12000);
    Bench_TextReset();
    if (trace) Rtty_BenchTraceStart();

    for (size_t k = 0; k < n12; k++)
    {
        Bench_CharClock = k;             // where a character reported now came from
        Rtty_Demodulator_ProcessSample((float32_t)(x[k] * gain));
    }
    free(x);

    // raw keeps every character the decoder printed, in the order it printed them: the trace has to
    // line its rows up with the text by position, and the normalised copy has already dropped the
    // punctuation that a station's frequency list is full of.
    if (raw && raw_sz) snprintf(raw, raw_sz, "%s", Bench_TextGet());
    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    printf("file      : %s\n", path);
    printf("format    : %u Hz, %u channel(s), %.1f s, gain %.1f dB\n",
           (unsigned)rate, (unsigned)ch, secs, 20.0 * log10(gain));
    printf("decoded   : %s\n", got);
    if (expected && *expected) return score_aligned(norm_baudot(expected, 0), got);
    return -1.0f;
}

/* ------------------------------------------------------------------ *
 *  the capture gate: whether a recording is worth pinning at all
 * ------------------------------------------------------------------ */
// Pinning an on air take promises four things: the audio holds the signal, at the pair the modem
// filters expect, on a bit clock that repeats, with no gap in it. Every one of those has been
// assumed rather than measured here at least once - a pair 1.5 kHz off, two "tones" that were one
// carrier's skirt, a take that was mostly noise, and truncation. This verb measures them.
static int cap_load(const char* path, cap_t* c)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (end <= 0) { fclose(f); printf("%s is empty, or not a file that can be sized\n", path); return 0; }
    const size_t sz = (size_t)end;
    uint8_t* buf = malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != sz)
    {
        free(buf); fclose(f); printf("short read on %s\n", path); return 0;
    }
    fclose(f);
    if (sz < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
    {
        free(buf); printf("%s is not a RIFF WAVE file\n", path); return 0;
    }

    uint32_t rate = 0, ch = 0, bits = 0, tag = 0;
    const uint8_t* data = NULL;
    size_t dlen = 0, i = 12;
    while (i + 8 <= sz)
    {
        const uint32_t clen = rd32(buf + i + 4);
        const size_t body = i + 8, avail = sz - body;
        // A chunk this reader does not want is stepped over: editors and recorders put LIST, bext,
        // fact and junk chunks ahead of the audio, and stopping at the first of them refuses a file
        // that is plain PCM all the way through.
        if (!memcmp(buf + i, "fmt ", 4))
        {
            if (clen < 16 || clen > avail)
            {
                free(buf);
                printf("%s: the fmt chunk is %s, so the stream is not described\n", path,
                       clen < 16 ? "too short" : "cut off");
                return 0;
            }
            tag  = rd16(buf + body);            // 1 PCM, 3 float, 0xfffe extensible
            ch   = rd16(buf + body + 2);
            rate = rd32(buf + body + 4);
            bits = rd16(buf + body + 14);
            // extensible carries the same samples behind a 40 byte header; its subformat GUID opens
            // with the plain PCM tag, and ffmpeg and several recorders write it for ordinary files
            if (tag == 0xfffe && clen >= 40 && rd16(buf + body + 24) == 1) tag = 1;
        }
        else if (!memcmp(buf + i, "data", 4))
        {
            // a recorder that stopped mid write leaves a data chunk longer than the file: decode
            // the audio that did arrive rather than call the whole take unreadable
            data = buf + body;
            dlen = clen <= avail ? clen : avail;
            if (clen > avail)
                printf("%s: the data chunk is cut off, reading the %zu bytes present\n", path, dlen);
        }
        if (clen > avail) break;                // nothing past a chunk that runs off the file
        i = body + (size_t)clen + (clen & 1);
    }
    if (!data || !dlen || !rate || !ch || tag != 1 || bits != 16)
    {
        free(buf);
        printf("%s: needs 16 bit PCM in a data chunk (format %u, %u bit, %u channel(s), %u Hz)\n",
               path, tag, bits, ch, rate);
        return 0;
    }
    if (rate < 1000 || rate > 384000)
    {
        free(buf);
        printf("%s: %u Hz is outside the 1 kHz to 384 kHz this resampler is for\n", path, rate);
        return 0;
    }
    const size_t frames = dlen / (2 * (size_t)ch);
    if (!frames)
    {
        free(buf); printf("%s: the data chunk holds no complete frame\n", path); return 0;
    }
    c->x = malloc(frames * sizeof(float));
    if (!c->x) { free(buf); printf("%s: %zu frames do not fit in memory\n", path, frames); return 0; }
    for (size_t k = 0; k < frames; k++)
        c->x[k] = (float)(int16_t)rd16(data + k * 2 * (size_t)ch) / 32768.0f;
    c->n = frames;
    c->rate = rate;
    c->ch = (uint16_t)ch;
    free(buf);
    return 1;
}

/* ------------------------------------------------------------------ *
 *  recorded input for the non RTTY modes
 * ------------------------------------------------------------------ */
// The tables above all hand a decoder a take that starts on a symbol boundary. A recording starts
// wherever the recorder was: leading silence, and the first symbol somewhere inside the buffer.
// These verbs trim the silence, then try starts across one symbol and keep the decode that scores
// best. The decoders themselves fit the phase inside a symbol; the search here is what gives them a
// buffer whose first symbol is close enough to find.

static float* rec_load_12k(const char* path, size_t* n_out, double* secs, uint32_t* rate_out,
                           uint32_t* ch_out)
{
    cap_t c;
    *n_out = 0;
    if (!cap_load(path, &c)) return NULL;
    size_t n = 0;
    float* x = resample_12k(c.x, c.n, c.rate, &n);
    if (x)
    {
        const float peak = peak_of(x, n);
        const float gain = peak > 0.0f ? 0.7f / peak : 1.0f;
        for (size_t k = 0; k < n; k++) x[k] *= gain;
    }
    *n_out = n;
    *secs = (double)c.n / (double)c.rate;
    *rate_out = 12000;
    *ch_out = c.ch;
    free(c.x);
    return x;
}

static int cmp_double(const void* a, const void* b);

// First 5 ms window whose level stands over the take's own. A recording that is mostly silence spends
// its symbol search on silence, and the block modes start their block count there. The reference is
// the 99th percentile of the window levels, not the loudest sample in the file: a recorder that opens
// with a click, or static anywhere in the take, raises a single sample's level but cannot move a
// percentile, so the trim still finds the signal it is looking for.
static size_t rec_onset(const float* x, size_t n)
{
    const size_t win = 60, stride = win / 2;
    const size_t nw = n >= win ? (n - win) / stride + 1 : 0;
    if (!nw) return 0;
    double* lvl = malloc(nw * sizeof(double));
    if (!lvl) return 0;
    for (size_t w = 0, i = 0; w < nw; w++, i += stride)
    {
        double acc = 0.0;
        for (size_t j = 0; j < win; j++) acc += (double)x[i + j] * x[i + j];
        lvl[w] = sqrt(acc / (double)win);
    }
    double* sorted = malloc(nw * sizeof(double));
    if (!sorted) { free(lvl); return 0; }
    memcpy(sorted, lvl, nw * sizeof(double));
    qsort(sorted, nw, sizeof(double), cmp_double);
    const double thr = 0.02 * sorted[(nw * 99) / 100];
    free(sorted);
    // one window over the threshold is not the signal: a click, a static crash or the settling
    // sample a recorder writes first clears the level and is gone 5 ms later. The take starts where
    // the level stays up, so a window counts only if at least half of the next ten (50 ms) clear the
    // threshold too. Hell is keyed, but its gaps are one 8 ms column in six, so a word still holds.
    size_t onset = 0;
    for (size_t w = 0; w < nw; w++)
    {
        if (lvl[w] <= thr) continue;
        size_t held = 0, seen = 0;
        for (size_t q = w + 1; q < nw && seen < 10; q++, seen++)
            if (lvl[q] > thr) held++;
        if (seen < 10 || held * 2 >= seen) { onset = w * stride; break; }
    }
    free(lvl);
    return onset;
}

// The start search only has to rank a start, not read the transmission. Twenty seconds is many
// symbols of every mode here and keeps nine, or seventeen, trial decodes from costing as many passes
// over a broadcast length take; the take itself is then decoded once, whole, from the start they
// chose. A take shorter than this is its own trial, which is why the graded cases are unaffected.
#define REC_TRIAL_SAMPLES 240000

// The block decoders hold a fixed number of symbols and refuse a take that does not fit; a broadcast
// length recording is longer than that, so it is cropped to what they hold and the crop is printed
// rather than leaving the caller with "nothing decoded" for a take that was fine.
static size_t rec_crop(const char* mode, size_t take, size_t holds)
{
    if (take <= holds) return take;
    printf("crop      : decoding the first %.1f s of %.1f s, where %s's buffers end\n",
           (double)holds / 12000.0, (double)take / 12000.0, mode);
    return holds;
}

// defined with the rest of the shape gate below, where cap_t and the bands live
static int gate_shape_gate(const float* x, size_t n, const char* mode);

static int navtex_wav(const char* path, const char* expected, float limit)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return 1;
    if (gate_shape_gate(x, n, "NAVTEX")) { free(x); return 1; }
    const size_t onset = rec_onset(x, n);
    int found = 0; double best_rank = 1e30;
    size_t best_st = onset;
    for (int k = 0; k <= 8; k++)
    {
        long st = (long)onset - 120 + (long)k * 15;
        if (st < 0) st = 0;
        if ((size_t)st >= n) break;
        const size_t left = n - (size_t)st;
        const size_t len = left <= REC_TRIAL_SAMPLES ? left : REC_TRIAL_SAMPLES;
        navtex_result_t r; char got[8192];
        const int ok = Bench_NavtexDecode(x + st, len, got, sizeof(got), &r);
        if (!ok) continue;
        // a short take is its own trial, and the text is the strongest score there is; a prefix of a
        // long one cannot be compared with the whole expected text, so its own word ratio ranks it
        const double rank = left <= REC_TRIAL_SAMPLES && expected && *expected
                          ? (double)exact_char_errors(expected, got, 1)
                          : -(double)r.valid_fraction;
        if (!found || rank < best_rank) { found = 1; best_rank = rank; best_st = (size_t)st; }
    }
    printf("file      : %s\nformat    : %u Hz, %u channel(s), %.2f s\n", path, rate, ch, secs);
    printf("onset     : %.1f ms (sample %zu); %s\n", 1000.0 * (double)onset / 12000.0, onset,
           found ? "decoded" : "nothing decoded");
    if (!found) { free(x); return 1; }
    {
        navtex_result_t r; char got[8192];
        if (!Bench_NavtexDecode(x + best_st, n - best_st, got, sizeof(got), &r))
        {
            printf("nothing decoded from the start the search chose\n");
            free(x);
            return 1;
        }
        printf("decoded   : %s\n", got);
        printf("clock     : period %.1f samples, phasing run %d, valid fraction %.3f\n",
               r.period_samples, r.phasing_run, r.valid_fraction);
        if (expected && *expected)
        {
            const double cer = 100.0 * (double)exact_char_errors(expected, got, 1) / (double)strlen(expected);
            printf("CER       : %.1f %%\n", cer);
            if (limit >= 0.0f && cer > (double)limit)
            {
                printf("CER is above the %g %% this case pins\n", (double)limit);
                free(x);
                return 1;
            }
        }
    }
    free(x);
    return 0;
}

static int mfsk16_wav(const char* path, const char* expected, float limit)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return 1;
    if (gate_shape_gate(x, n, "MFSK16")) { free(x); return 1; }
    const size_t onset = rec_onset(x, n);
    const size_t S = 768;                 // 12 kHz / 15.625 Bd
    int found = 0; double best_rank = 1e30;
    size_t best_start = 0; int best_k = -1;
    for (int k = 0; k <= 16; k++)
    {
        long st = (long)onset - (long)S + (long)k * (long)S / 16;
        if (st < 0) st = 0;
        if ((size_t)st >= n) break;
        const size_t left = n - (size_t)st;
        const size_t len = left <= REC_TRIAL_SAMPLES ? left : REC_TRIAL_SAMPLES;
        mfsk16_result_t r; char got[512];
        const int ok = Bench_Mfsk16Decode(x + st, len, got, sizeof(got), &r);
        if (!ok) continue;
        const double rank = left <= REC_TRIAL_SAMPLES && expected && *expected
                          ? (double)exact_char_errors(expected, got, 1)
                          : -(double)r.tones_ok / (r.symbols ? (double)r.symbols : 1.0);
        if (!found || rank < best_rank)
        {
            found = 1; best_rank = rank; best_start = (size_t)st; best_k = k;
        }
    }
    printf("file      : %s\nformat    : %u Hz, %u channel(s), %.2f s\n", path, rate, ch, secs);
    printf("onset     : %.1f ms (sample %zu)\n", 1000.0 * (double)onset / 12000.0, onset);
    if (!found) { printf("nothing decoded\n"); free(x); return 1; }
    printf("phase     : trial %d, start %zu samples (%.1f ms before onset)\n", best_k, best_start,
           1000.0 * ((double)onset - (double)best_start) / 12000.0);
    {
        mfsk16_result_t r; char got[512];
        const size_t take = rec_crop("MFSK16", n - best_start, Bench_Mfsk16MaxSamples());
        if (!Bench_Mfsk16Decode(x + best_start, take, got, sizeof(got), &r))
        {
            printf("nothing decoded from the start the search chose\n");
            free(x);
            return 1;
        }
        printf("decoded   : %s\n", got);
        printf("symbols   : %u, heard %.1f %%\n", r.symbols,
               r.symbols ? 100.0 * (double)r.tones_ok / (double)r.symbols : 0.0);
        if (expected && *expected)
        {
            const double cer = 100.0 * (double)exact_char_errors(expected, got, 1) / (double)strlen(expected);
            printf("CER       : %.1f %%\n", cer);
            if (limit >= 0.0f && cer > (double)limit)
            {
                printf("CER is above the %g %% this case pins\n", (double)limit);
                free(x);
                return 1;
            }
        }
    }
    free(x);
    return 0;
}

// WEFAX had no path from a recording at all: the table hands the decoder a buffer that begins on a
// line boundary and already knows the picture it is meant to find. A capture begins somewhere inside
// a line, its sync tips wander, and there is no ground truth - so this reports only what a receiver
// can know on its own: how many lines it captured, how the grey is spread, and the picture. The
// correlation the table scores with is simply not available here, and the run says so instead of
// printing something that looks like it.
// The acquisition numbers for a file, with no ground truth anywhere in the path.  A 120 lines/min
// emission puts its sync tips on a 6000-sample grid, so the spacing between the tips the capture
// accepted is a measurement the receiver can make without knowing what the picture shows - which is
// what the earlier claim in this file was wrong about: it said the harness cannot judge a fax capture
// at all, and what it cannot judge is the content.  Measured, this is the number: 2.6 samples on the
// generated chart, 335 on a real RTTY take, 317 on a real 25 s recording of 12262.5 kHz that turned
// out not to be fax.  Two orders of magnitude, from traffic, not from a model.
static float wefax_acquisition(const char* path, uint32_t* lines_out, float* ratio_out)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return -1.0f;
    const size_t onset = rec_onset(x, n);
    if (onset)
    {
        memmove(x, x + onset, (n - onset) * sizeof(float));
        n -= onset;
    }
    const size_t rows = n / WEFAX_SAMPLES_PER_LINE + 2;
    uint8_t* grey = calloc(rows, WEFAX_IMG_COLS);
    wefax_result_t res;
    memset(&res, 0, sizeof(res));
    if (grey)
    {
        Bench_WefaxDecode(x, n, grey, (uint32_t) rows, &res);
        free(grey);
    }
    free(x);
    if (lines_out) *lines_out = res.lines_found;
    if (ratio_out) *ratio_out = res.mean_tip_ratio;
    return res.row_rms_error;
}

static int wefax_wav(const char* path)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return 1;
    // Start where the signal starts, the way the other recording verbs do. Without this the line
    // spacing is set by the first two sync tips the decoder meets and one of them is a silence, so
    // every line after it is sampled from the wrong place: a take with a 0.75 s lead came back as 159
    // grey with a black tick at the start of each row, which is exactly what a fax line looks like
    // when the only thing you are reading is its sync tip.
    const size_t onset = rec_onset(x, n);
    printf("onset     : %zu samples (%.1f ms), %.3f line times\n", onset, 1000.0*(double)onset/12000.0, (double)onset/6000.0);
    if (onset)
    {
        memmove(x, x + onset, (n - onset) * sizeof(float));
        n -= onset;
    }
    const size_t possible = n / WEFAX_SAMPLES_PER_LINE;
    uint8_t* grey = malloc((possible + 2) * WEFAX_IMG_COLS);
    if (!grey) { free(x); return 1; }
    // Zeroed because a row the capture does not find has to stay a hole: the decoder now places each
    // line by where its tip fell, and anything it skipped is missing, not shifted up.
    memset(grey, 0, (possible + 2) * WEFAX_IMG_COLS);
    wefax_result_t res;
    memset(&res, 0, sizeof(res));
    const int ok = Bench_WefaxDecode(x, n, grey, (uint32_t) (possible + 2), &res);
    printf("file      : %s\n", path);
    printf("format    : %u Hz, %u channel(s), %.2f s\n", rate, ch, secs);
    printf("lines     : %u captured, %zu line times in the recording\n", res.lines_found, possible);
    // Acquisition, not content: how pure the accepted tips were, and how steady the spacing between
    // them came out.  Both are things the receiver knows without being told what the picture should
    // look like, which is what the grey statistics below are not.
    printf("tips      : mean ratio %.3f of 1857 Hz against the two picture tones, spacing rms %.1f samples\n",
           (double) res.mean_tip_ratio, (double) res.row_rms_error);
    printf("          : %u nominal line positions searched, %u with no tip found\n",
           res.lines_found + res.lines_missed, res.lines_missed);

    if (!ok || res.lines_found == 0)
    {
        printf("verdict   : nothing captured. A fax transmission is 4 kHz wide and this harness reads\n"
               "            it as everything else does; there is no WEFAX entry in the shape gate,\n"
               "            because its tone pair sits where RTTY's does. Aim by the waterfall.\n");
        free(grey); free(x);
        return 1;
    }
    size_t dark = 0, mid = 0, light = 0;
    const size_t px = (size_t)res.lines_found * WEFAX_IMG_COLS;
    for (size_t i = 0; i < px; i++)
    {
        if (grey[i] < 64) dark++;
        else if (grey[i] > 192) light++;
        else mid++;
    }
    printf("grey      : mean %.1f, %5.1f%% black, %5.1f%% mid, %5.1f%% white\n", res.mean_level,
           100.0 * (double)dark / (double)px, 100.0 * (double)mid / (double)px,
           100.0 * (double)light / (double)px);
    // Reported, not judged. The first version of this turned those two numbers into a verdict and
    // got it exactly backwards: it called the real capture "not a fax picture", because six of its
    // twelve rows are empty sky and none of them starts dark - the sync tip is outside the picture
    // area this generator draws - and it called the RTTY take a fax capture, because noise from a
    // demodulator that cannot track anything produces rows that vary wildly and begin black. So the
    // numbers stay, since they are what to look at, and the judgement does not: nothing in this
    // harness can tell a fax capture from a wrong-mode one without knowing the picture. WEFAX is the
    // one mode with no entry in the shape gate for the same reason, and the eye is the check.
    size_t contrast = 0, tip = 0;
    for (uint32_t r = 0; r < res.lines_found; r++)
    {
        const uint8_t* row = grey + (size_t)r * WEFAX_IMG_COLS;
        uint8_t lo = 255, hi = 0;
        for (int c = 0; c < WEFAX_IMG_COLS; c++)
        {
            if (row[c] < lo) lo = row[c];
            if (row[c] > hi) hi = row[c];
        }
        if (hi - lo > 60) contrast++;
        if (row[0] < 96) tip++;
    }
    printf("rows      : %u of %u span more than 60 levels, %u start dark\n",
           (unsigned)contrast, res.lines_found, (unsigned)tip);
    // Acquisition, which the receiver can judge, kept apart from content, which it cannot.
    printf("verdict   : acquisition. %u line times searched, %u tips accepted at mean purity %.3f,\n"
           "            spacing rms %.1f samples.  A 120 lines/min emission puts its tips on a\n"
           "            6000-sample grid, so that rms is the test: the generated chart gives 2.6, a\n"
           "            real RTTY take 335, a real 25 s take of 12262.5 kHz that was not fax 317.\n"
           "            Nothing here says what the picture shows, and the table's own invert row shows\n"
           "            the decoder does not choose polarity, so if the sky is black and the land\n"
           "            white, open the other file.\n",
           (unsigned) (res.lines_found + res.lines_missed), res.lines_found,
           (double) res.mean_tip_ratio, (double) res.row_rms_error);

    char base[512];
    const char* slash = strrchr(path, '/');
    snprintf(base, sizeof(base), "%s", slash ? slash + 1 : path);
    char* dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char out[600];
    snprintf(out, sizeof(out), "wefax-%s.pgm", base);
    if (pgm_write(out, grey, WEFAX_IMG_COLS, res.lines_found)) printf("picture   : %s\n", out);
    snprintf(out, sizeof(out), "wefax-%s-inverted.pgm", base);
    for (size_t i = 0; i < px; i++) grey[i] = (uint8_t)(255 - grey[i]);
    if (pgm_write(out, grey, WEFAX_IMG_COLS, res.lines_found)) printf("inverted  : %s\n", out);
    free(grey); free(x);
    return 0;
}

static int olivia_wav(const char* path, const char* expected, float limit)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return 1;
    if (gate_shape_gate(x, n, "Olivia")) { free(x); return 1; }
    const size_t onset = rec_onset(x, n);
    const size_t S = 384;                 // 12 kHz / 31.25 Bd
    int found = 0; double best_rank = 1e30;
    size_t best_start = 0; int best_k = -1;
    for (int k = 0; k <= 16; k++)
    {
        long st = (long)onset - (long)S + (long)k * (long)S / 16;
        if (st < 0) st = 0;
        if ((size_t)st >= n) break;
        const size_t left = n - (size_t)st;
        const size_t len = left <= REC_TRIAL_SAMPLES ? left : REC_TRIAL_SAMPLES;
        olivia_result_t r; char got[512];
        const int ok = Bench_OliviaDecode(x + st, len, got, sizeof(got), &r);
        if (!ok) continue;
        const double rank = left <= REC_TRIAL_SAMPLES && expected && *expected
                          ? (double)exact_char_errors(expected, got, 1)
                          : -(double)r.tones_ok / (r.symbols ? (double)r.symbols : 1.0);
        if (!found || rank < best_rank)
        {
            found = 1; best_rank = rank; best_start = (size_t)st; best_k = k;
        }
    }
    printf("file      : %s\nformat    : %u Hz, %u channel(s), %.2f s\n", path, rate, ch, secs);
    printf("onset     : %.1f ms (sample %zu)\n", 1000.0 * (double)onset / 12000.0, onset);
    if (!found) { printf("nothing decoded\n"); free(x); return 1; }
    printf("phase     : trial %d, start %zu samples (%.1f ms before onset)\n", best_k, best_start,
           1000.0 * ((double)onset - (double)best_start) / 12000.0);
    {
        olivia_result_t r; char got[512];
        const size_t take = rec_crop("Olivia", n - best_start, Bench_OliviaMaxSamples());
        if (!Bench_OliviaDecode(x + best_start, take, got, sizeof(got), &r))
        {
            printf("nothing decoded from the start the search chose\n");
            free(x);
            return 1;
        }
        printf("decoded   : %s\n", got);
        printf("symbols   : %u, heard %.1f %%, blocks %u\n", r.symbols,
               r.symbols ? 100.0 * (double)r.tones_ok / (double)r.symbols : 0.0, r.blocks);
        if (expected && *expected)
        {
            const double cer = 100.0 * (double)exact_char_errors(expected, got, 1) / (double)strlen(expected);
            printf("CER       : %.1f %%\n", cer);
            if (limit >= 0.0f && cer > (double)limit)
            {
                printf("CER is above the %g %% this case pins\n", (double)limit);
                free(x);
                return 1;
            }
        }
    }
    free(x);
    return 0;
}

static int hell_wav(const char* path, const char* out_pgm)
{
    size_t n; double secs; uint32_t rate, ch;
    float* x = rec_load_12k(path, &n, &secs, &rate, &ch);
    if (!x) return 1;
    if (gate_shape_gate(x, n, "Hell")) { free(x); return 1; }
    const size_t onset = rec_onset(x, n);
    const size_t S = 98;                  // 12 kHz / 122.5 Bd, rounded
    const int MAXC = 2000;
    uint8_t* pix = malloc((size_t)MAXC * HELL_ROWS);
    uint8_t* best = malloc((size_t)MAXC * HELL_ROWS);
    if (!pix || !best) { free(pix); free(best); free(x); return 1; }
    int found = 0, trial_cols = 0; double best_gap = -1.0;
    size_t best_st = onset;
    for (int k = 0; k <= 8; k++)
    {
        long st = (long)onset - (long)S + (long)k * (long)S / 8;
        if (st < 0) st = 0;
        if ((size_t)st >= n) break;
        const size_t left = n - (size_t)st;
        const size_t len = left <= REC_TRIAL_SAMPLES ? left : REC_TRIAL_SAMPLES;
        int cols = 0; hell_result_t r;
        const int ok = Bench_HellDecode(x + st, len, pix, MAXC, &cols, &r);
        if (!ok || cols <= 0) continue;
        if (!found || r.gap_share > best_gap || (r.gap_share == best_gap && cols > trial_cols))
        {
            found = 1; best_gap = r.gap_share; trial_cols = cols; best_st = (size_t)st;
        }
    }
    printf("file      : %s\nformat    : %u Hz, %u channel(s), %.2f s\n", path, rate, ch, secs);
    printf("onset     : %.1f ms (sample %zu)\n", 1000.0 * (double)onset / 12000.0, onset);
    if (!found) { printf("nothing decoded\n"); free(pix); free(best); free(x); return 1; }
    int best_gcols = 0; hell_result_t best_res;
    memset(&best_res, 0, sizeof(best_res));
    if (!Bench_HellDecode(x + best_st, n - best_st, best, MAXC, &best_gcols, &best_res))
    {
        printf("nothing decoded from the start the search chose\n");
        free(pix); free(best); free(x);
        return 1;
    }
    printf("picture   : %d columns, column phase %d, pixel phase %d, gap share %.3f\n",
           best_gcols, best_res.column_phase, best_res.pixel_phase, best_res.gap_share);
    if (best_gcols >= MAXC)
        printf("note      : the raster ends at the %d column buffer, about %.0f s of picture\n",
               MAXC, (double)MAXC * (12000.0 / 122.5) / 12000.0);
    // the decoder's raster is a full MAXC wide; give the columns it fitted a buffer of their own
    // width and write that through the one PGM writer, which answers for a file that could not be
    // written instead of leaving a truncated picture behind
    const char* out = out_pgm ? out_pgm : "hell-wav.pgm";
    uint8_t* packed = malloc((size_t)best_gcols * HELL_ROWS);
    int wrote = 0;
    if (packed)
    {
        for (int r = 0; r < HELL_ROWS; r++)
            memcpy(packed + (size_t)r * (size_t)best_gcols, best + (size_t)r * MAXC, (size_t)best_gcols);
        wrote = pgm_write(out, packed, (uint32_t)best_gcols, HELL_ROWS);
        free(packed);
    }
    free(pix); free(best); free(x);
    if (!wrote) { printf("cannot write %s\n", out); return 1; }
    printf("wrote     : %s\n", out);
    return 0;
}

// The P5 the verb wrote, read back, so the recording check grades the file rather than an array that
// never went through the writer.
static int pgm_read(const char* path, uint8_t** buf, int* w, int* h)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char magic[3] = {0};
    int mx = 0;
    int ok = fscanf(f, "%2s %d %d %d", magic, w, h, &mx) == 4 && !strcmp(magic, "P5")
          && *w > 0 && *h > 0 && mx == 255 && fgetc(f) != EOF;
    *buf = NULL;
    if (ok)
    {
        *buf = malloc((size_t)*w * (size_t)*h);
        ok = *buf && fread(*buf, 1, (size_t)*w * (size_t)*h, f) == (size_t)*w * (size_t)*h;
        if (!ok) { free(*buf); *buf = NULL; }
    }
    fclose(f);
    return ok;
}

// The raster the verb wrote against the picture the generator drew. Comparing kinds of pixels (how
// many were keyed) passes a picture shifted by a column, because the count is the same; comparing
// the pixels themselves does not. The decoder may drop a blank column at either end, so the columns
// are allowed to slide by up to eight and the overlap has to hold all but two of the drawn columns.
static int hell_raster_ok(const char* pgm, const uint8_t* truth, int tcols, int truth_stride,
                          int first, int* cols_out, int* shift_out, int* bad_out)
{
    uint8_t* pix = NULL;
    int w = 0, h = 0;
    int best = -1, best_s = 0;
    *cols_out = 0; *shift_out = 0; *bad_out = -1;
    if (!pgm_read(pgm, &pix, &w, &h) || h != HELL_ROWS) { free(pix); return 0; }
    for (int s = -8; s <= 8; s++)
    {
        int bad = 0, seen = 0;
        for (int d = 0; d < w; d++)
        {
            const int t = d - s;
            if (t < first || t >= tcols) continue;
            seen++;
            for (int r = 0; r < HELL_ROWS; r++)
                if ((pix[(size_t)r * (size_t)w + (size_t)d] != 0)
                    != (truth[(size_t)r * (size_t)truth_stride + (size_t)t] != 0)) bad++;
        }
        if (seen < tcols - 2) continue;          // too little of the picture to call it the truth
        if (best < 0 || bad < best) { best = bad; best_s = s; }
    }
    free(pix);
    *cols_out = w; *shift_out = best_s; *bad_out = best;
    return best == 0;
}

// Every verb that reports through check() shares this counter, so a failed check is a failed verb
// and not just a line that says FAIL while the verb exits 0.
static int failures;
static void check(int ok, const char* what);

// The recording paths are checked against the same modems' own takes: a test WAV with a long lead,
// written through the same WAV writer a fixture uses, has to decode as sent. This is not a real
// recording, but it is the first path that does not hand the decoder a buffer starting on a symbol.
static int record_check(void)
{
    const size_t lead = 9037;             // 0.75 s, not a multiple of any mode's symbol
    {
        static const char* msg = "NAVTEX TEST DE UHSDR";
        const size_t cap = 400000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        const size_t n = x ? Bench_NavtexGenerate(x, cap, msg, 0.0f, NAVTEX_ORDER_TABLE) : 0;
        float32_t* y = n ? calloc(n + lead, sizeof(float32_t)) : NULL;
        if (y) memcpy(y + lead, x, n * sizeof(float32_t));
        const bool wrote = y && Bench_WavWrite("/tmp/rec-navtex.wav", y, n + lead, 12000);
        check(wrote, "record: a NAVTEX take with a lead can be written as a WAV");
        if (wrote) failures += navtex_wav("/tmp/rec-navtex.wav", msg, 0.0f) != 0;
        if (wrote && n > 200)
        {
            const size_t d = 60;             // half a bit, cut from the start
            memmove(y + lead, y + lead + d, (n - d) * sizeof(float32_t));
            const bool w2 = Bench_WavWrite("/tmp/rec-navtex-phase.wav", y, n + lead - d, 12000);
            check(w2, "record: a NAVTEX take starting half a bit in can be written");
            if (w2) failures += navtex_wav("/tmp/rec-navtex-phase.wav", msg, 0.0f) != 0;
        }
        free(x); free(y);
    }
    {
        // WEFAX arrives from a file as a picture and nothing else, so this is the only check that
        // means anything for it: generate a chart, add the same 0.75 s lead the other modes get -
        // one and a half line times, which is not a line boundary and not a symbol boundary either -
        // run the recording verb, and read the PGM back off the disk it wrote. Truth-free judgement
        // of a fax capture is not possible in this harness (see wefax_wav), but here the truth is
        // known, which is the point of a recording check rather than a capture.
        static const wefax_cfg_t cfg = { .name = "record", .snr_db = 20.0f, .rows = 12 };
        const size_t cap = (size_t)cfg.rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(cap * sizeof(float32_t));
        uint8_t* truth = x ? malloc(cap / WEFAX_SAMPLES_PER_LINE * WEFAX_IMG_COLS) : NULL;
        if (x && truth)
        {
            Bench_WefaxGenerate(x, truth, &cfg);
            float32_t* y = calloc(cap + lead, sizeof(float32_t));
            if (y) memcpy(y + lead, x, cap * sizeof(float32_t));
            const bool wrote = y && Bench_WavWrite("/tmp/rec-wefax.wav", y, cap + lead, 12000);
            check(wrote, "record: a WEFAX chart with a lead can be written as a WAV");
            if (wrote)
            {
                failures += wefax_wav("/tmp/rec-wefax.wav") != 0;
                uint8_t* got = NULL;
                int gw = 0, gh = 0;
                const int read_back = pgm_read("wefax-rec-wefax.pgm", &got, &gw, &gh);
                check(read_back && gw == WEFAX_IMG_COLS && gh > 0,
                      "record: the verb left a picture on disk to read back");
                if (read_back && gw == WEFAX_IMG_COLS && gh > 0)
                {
                    // the lead is a line and a half, so the first line the capture holds is wherever
                    // the first sync tip after it is: compare at the offset that agrees best rather
                    // than assuming the file starts where the generator did
                    int best = 0;
                    for (int off = 0; off <= 4; off++)
                    {
                        int same = 0, cmp = 0;
                        for (int r = 0; r < gh; r++)
                        {
                            if (r + off >= (int)cfg.rows) break;
                            for (int c = 0; c < gw; c++)
                            {
                                cmp++;
                                if (abs((int)got[(size_t)r * gw + c] - (int)truth[(size_t)(r + off) * WEFAX_IMG_COLS + c]) <= 40) same++;
                            }
                        }
                        if (cmp && 100 * same / cmp > best) best = 100 * same / cmp;
                    }
                    // Was pinned as broken.  It no longer is, which is the one thing this harness
                    // says out loud when it happens: the ceiling was written to fail when someone made
                    // the capture hold, and the capture now holds.  Measured 88 % of pixels matching a
                    // take whose lead is one and a half line times, against the 32-45 % that every lead
                    // from 30 to 6000 samples used to produce identically.  Pinned at 70, which is a
                    // margin, not a measurement - the point is that falling back off a cliff is a
                    // failure, and the reason it used to fail (a 1857 Hz discriminator cannot tell a
                    // line's start from its end, because the phasing tone is 43 Hz away and in the same
                    // bin) is fixed by placing the tip at the leading edge of the plateau rather than at
                    // its peak, and by opening the search window on the known white reference.
                    printf("      %d%% of pixels match the chart sent\n", best);
                    check(best >= 70, "record: WEFAX survives a take that does not start on a line boundary");
                }
                free(got);
            }
        }
        free(x); free(truth);
    }
    {
        static const char* msg = "MFSK16 TEST DE UHSDR 12345";
        const size_t cap = 200000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        const size_t n = x ? Bench_Mfsk16Generate(x, cap, msg, 0.0f) : 0;
        float32_t* y = n ? calloc(n + lead, sizeof(float32_t)) : NULL;
        if (y) memcpy(y + lead, x, n * sizeof(float32_t));
        const bool wrote = y && Bench_WavWrite("/tmp/rec-mfsk16.wav", y, n + lead, 12000);
        check(wrote, "record: an MFSK16 take with a lead can be written as a WAV");
        if (wrote) failures += mfsk16_wav("/tmp/rec-mfsk16.wav", msg, 0.0f) != 0;
        if (wrote && n > 800)
        {
            const size_t d = 383;            // half a 768 sample symbol
            memmove(y + lead, y + lead + d, (n - d) * sizeof(float32_t));
            const bool w2 = Bench_WavWrite("/tmp/rec-mfsk16-phase.wav", y, n + lead - d, 12000);
            check(w2, "record: an MFSK16 take starting half a symbol in can be written");
            if (w2) failures += mfsk16_wav("/tmp/rec-mfsk16-phase.wav", msg, 0.0f) != 0;
        }
        free(x); free(y);
    }
    {
        static const char* msg = "OLIVIA TEST DE UHSDR 12345";
        const size_t cap = 200000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        const size_t n = x ? Bench_OliviaGenerate(x, cap, msg, 0.0f) : 0;
        float32_t* y = n ? calloc(n + lead, sizeof(float32_t)) : NULL;
        if (y) memcpy(y + lead, x, n * sizeof(float32_t));
        const bool wrote = y && Bench_WavWrite("/tmp/rec-olivia.wav", y, n + lead, 12000);
        check(wrote, "record: an Olivia take with a lead can be written as a WAV");
        if (wrote) failures += olivia_wav("/tmp/rec-olivia.wav", msg, 0.0f) != 0;
        if (wrote && n > 500)
        {
            const size_t d = 192;            // half a 384 sample symbol
            memmove(y + lead, y + lead + d, (n - d) * sizeof(float32_t));
            const bool w2 = Bench_WavWrite("/tmp/rec-olivia-phase.wav", y, n + lead - d, 12000);
            check(w2, "record: an Olivia take starting half a symbol in can be written");
            if (w2) failures += olivia_wav("/tmp/rec-olivia-phase.wav", msg, 0.0f) != 0;
        }
        free(x); free(y);
    }
    {
        static const char* msg = "HELL TEST";
        const size_t cap = 40000;
        static uint8_t truth[HELL_ROWS * 256];
        float32_t* x = malloc(cap * sizeof(float32_t));
        int tcols = 0;
        const size_t n = x ? Bench_HellGenerate(x, cap, msg, truth, 256, &tcols, 1000.0f, 0.0f) : 0;
        float32_t* y = n ? calloc(n + lead, sizeof(float32_t)) : NULL;
        if (y) memcpy(y + lead, x, n * sizeof(float32_t));
        const bool wrote = y && Bench_WavWrite("/tmp/rec-hell.wav", y, n + lead, 12000);
        check(wrote, "record: a Hell take with a lead can be written as a WAV");
        if (wrote)
        {
            failures += hell_wav("/tmp/rec-hell.wav", "/tmp/rec-hell.pgm") != 0;
            int cols = 0, shift = 0, bad = -1;
            const int same = hell_raster_ok("/tmp/rec-hell.pgm", truth, tcols, 256, 0, &cols, &shift, &bad);
            printf("     (Hell raster: %d of %d columns, shift %d, %d pixels differ)\n",
                   cols, tcols, shift, bad);
            check(same, "record: the Hell raster written is the picture that was drawn");
        }
        if (wrote && n > 400)
        {
            const size_t d = 49;             // half a 98 sample pixel
            memmove(y + lead, y + lead + d, (n - d) * sizeof(float32_t));
            const bool w2 = Bench_WavWrite("/tmp/rec-hell-phase.wav", y, n + lead - d, 12000);
            check(w2, "record: a Hell take starting half a pixel in can be written");
            if (w2)
            {
                failures += hell_wav("/tmp/rec-hell-phase.wav", "/tmp/rec-hell-phase.pgm") != 0;
                int cols = 0, shift = 0, bad = -1;
                // the take was cut half a pixel in, so its first column is half drawn and is not
                // compared; every column from the second on has all of its keying
                const int same = hell_raster_ok("/tmp/rec-hell-phase.pgm", truth, tcols, 256, 1,
                                                &cols, &shift, &bad);
                printf("     (half a pixel in: %d of %d columns, shift %d, %d pixels differ)\n",
                       cols, tcols, shift, bad);
                check(same, "record: the half pixel offset still comes back as the picture sent");
            }
        }
        free(x); free(y);
    }
    printf("%s\n", failures ? "RECORD CHECKS FAILED" : "record checks passed");
    return failures ? 1 : 0;
}

#define GATE_PI 3.14159265358979323846

// magnitude of one frequency inside a window, normalised so windows of different length compare
static double gate_tone(const float* x, int n, double hz, double sr)
{
    double re = 0.0, im = 0.0;
    const double w = 2.0 * GATE_PI * hz / sr;
    for (int k = 0; k < n; k++)
    {
        re += x[k] * cos(w * (double)k);
        im -= x[k] * sin(w * (double)k);
    }
    return n > 0 ? sqrt(re * re + im * im) / (double)n : 0.0;
}

// the busiest tone of the passband, skipping the neighbourhood of one already found. A 20 ms window
// is one bit of the slowest RTTY speed, so it resolves the pair without resolving the bits that
// make it up; scanning twice rather than keeping two maxima in one pass stops a pair of adjacent
// bins at the top of a single peak from being read as two tones.
static double gate_best_tone(const cap_t* c, double skip_hz)
{
    const int w = (int)lround(0.020 * c->rate);
    const int hop = w / 2;
    const double top = fmin(2200.0, 0.45 * (double)c->rate);
    double best = 0.0, bf = 0.0;
    for (double f = 300.0; f <= top; f += 5.0)
    {
        if (skip_hz > 0.0 && fabs(f - skip_hz) < 150.0) continue;
        double acc = 0.0;
        int frames = 0;
        for (size_t s = 0; s + (size_t)w <= c->n; s += (size_t)hop)
        {
            acc += gate_tone(c->x + s, w, f, c->rate);
            frames++;
        }
        if (!frames) return 0.0;
        acc /= frames;
        if (acc > best) { best = acc; bf = f; }
    }
    return bf;
}

static int gate_coarse_pair(const cap_t* c, double* lo, double* hi)
{
    const double f1 = gate_best_tone(c, 0.0);
    if (f1 <= 0.0) return 0;
    const double f2 = gate_best_tone(c, f1);
    if (f2 <= 0.0) return 0;
    *lo = f1 < f2 ? f1 : f2;
    *hi = f1 < f2 ? f2 : f1;
    return 1;
}

typedef struct
{
    double lo_hz, hi_hz, lo_spread, hi_spread;   // refined tone frequencies and their spread
    double f_low, f_high, f_both;                // fraction of 5 ms windows holding one tone or both
    int windows;
} gate_pair_t;

// 5 ms is the resolution that separates "this window is one tone" from "this window straddles a
// transition"; at 20 ms windows a straddling bit reads as both tones and a bad pair looks fine.
static void gate_pair_stats(const cap_t* c, double clo, double chi, gate_pair_t* g)
{
    const int w = (int)lround(0.005 * c->rate);
    double sl = 0.0, sl2 = 0.0, wl = 0.0, sh = 0.0, sh2 = 0.0, wh = 0.0;
    g->f_low = g->f_high = g->f_both = 0.0;
    g->windows = 0;
    for (size_t s = 0; s + (size_t)w <= c->n; s += (size_t)w)
    {
        const double a = gate_tone(c->x + s, w, clo, c->rate);
        const double b = gate_tone(c->x + s, w, chi, c->rate);
        const double db = 20.0 * log10((b + 1e-9) / (a + 1e-9));
        g->windows++;
        if (fabs(db) < 6.0) { g->f_both += 1.0; continue; }
        // refine the tone this window actually holds, so a dial offset reads as an offset and not
        // as a pair the modem cannot filter
        const double near = db < 0.0 ? clo : chi;
        double best = 0.0, bf = near;
        for (double f = near - 40.0; f <= near + 40.0; f += 0.5)
        {
            const double m = gate_tone(c->x + s, w, f, c->rate);
            if (m > best) { best = m; bf = f; }
        }
        if (db < 0.0) { g->f_low += 1.0; sl += bf * best; sl2 += bf * bf * best; wl += best; }
        else          { g->f_high += 1.0; sh += bf * best; sh2 += bf * bf * best; wh += best; }
    }
    if (g->windows)
    {
        const double n = (double)g->windows;
        g->f_low /= n; g->f_high /= n; g->f_both /= n;
    }
    g->lo_hz = wl > 0.0 ? sl / wl : clo;
    g->hi_hz = wh > 0.0 ? sh / wh : chi;
    g->lo_spread = wl > 0.0 ? sqrt(fabs(sl2 / wl - g->lo_hz * g->lo_hz)) : 0.0;
    g->hi_spread = wh > 0.0 ? sqrt(fabs(sh2 / wh - g->hi_hz * g->hi_hz)) : 0.0;
}

typedef struct
{
    double period_ms;
    double dispersion_pct;   // every transition's distance from its own phase centre, as an RMS
    double mad_pct;          // the same scatter as a median absolute deviation, which one bad
                             // crossing cannot move
    int    transitions;
} gate_clock_t;

// a phase difference folded into [-0.5, 0.5): the nearest way round the grid
static double wrap_half(double d) { return d - floor(d + 0.5); }

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// The tone the signal is on flips at every bit boundary, so the transitions sit on a grid whose step
// is the bit. Fitting that step over the whole take, then measuring how far the transitions scatter
// off it, is the difference between a clock and a stretch of audio that happens to have two tones.
// The transition times of a take: where the tone changed, as opposed to where a window first saw
// it. The clock fit and the alignment table both walk these, so they are measured once, here.
static size_t gate_crossings(const cap_t* c, double lo, double hi, double** out)
{
    const int w = (int)lround(0.005 * c->rate);
    const int hop = (int)lround(0.001 * c->rate);
    size_t cap = 4096, n = 0;
    const size_t nh = c->n > (size_t)w ? (c->n - (size_t)w) / (size_t)hop + 1 : 0;
    double* t = malloc(cap * sizeof(double));
    double* g = nh ? malloc(nh * sizeof(double)) : NULL;
    *out = NULL;
    if (!t || !g) { free(t); free(g); return 0; }
    for (size_t s = 0, i = 0; i < nh; s += (size_t)hop, i++)
    {
        const double a = gate_tone(c->x + s, w, lo, c->rate);
        const double b = gate_tone(c->x + s, w, hi, c->rate);
        g[i] = (b - a) / (b + a + 1e-9);
    }
    // A zero crossing of the raw discriminator is a fragile thing to count: noise crosses it
    // whenever neither tone is loud. Counting a transition only when the signal has settled on the
    // other tone (hysteresis) finds one per bit change instead of several per slew.
    // A single hop on the other tone is noise inside a long mark; a transition is the signal staying
    // there. The confirmation is shorter than a bit, so a real change is never missed.
    // Its time is not the centre of the window that first saw the new tone, which trails the change
    // by up to the window length and carries that smear into every period. The crossing is taken
    // instead between the last settled sample of the old tone and the first of the new one, on the
    // line through their two discriminator values: the window smooths the step symmetrically, so
    // that zero is where the change happened.
    const int confirm = (int)lround(0.003 * c->rate / (double)hop);
    int state = 0, opp = 0;
    size_t opp_start = 0, settled = 0;
    for (size_t i = 0; i < nh; i++)
    {
        if (fabs(g[i]) < 0.5) continue;              // mid slew, or neither tone: not a decision
        const int now = g[i] > 0.0 ? 1 : -1;
        if (!state) { state = now; settled = i; continue; }   // the first settled tone starts it
        if (now == state) { opp = 0; settled = i; continue; }
        if (opp == 0) opp_start = i;
        if (++opp >= confirm)
        {
            const double t0 = ((double)(settled * (size_t)hop) + 0.5 * (double)w) / (double)c->rate;
            const double t1 = ((double)(opp_start * (size_t)hop) + 0.5 * (double)w) / (double)c->rate;
            const double g0 = g[settled], g1 = g[opp_start];
            if (n == cap)
            {
                double* grow = realloc(t, cap * 2 * sizeof(double));
                if (!grow) { free(t); free(g); return 0; }
                t = grow;
                cap *= 2;
            }
            t[n++] = t0 + (t1 - t0) * (g0 / (g0 - g1));
            state = now;
            settled = opp_start;
            opp = 0;
        }
    }
    free(g);
    if (n < 16) { free(t); return 0; }
    *out = t;
    return n;
}

// Every phase measurement in the clock fit is the same sum: one unit phasor per transition on a grid
// of the given period, folded at `harmonic` times the phase. Harmonic 2 is the squared phase, which
// cancels the half bit a Baudot character's stop leaves and reads the bit; harmonic 1 is the
// fundamental the two centres are shifted and measured in. With more than one centre the transitions
// are split into a bucket each, nearest by folded distance, which is how a centre is moved onto the
// transitions that chose it; one bucket sums them all into a single phasor.
static void gate_phase_sums(const double* t, size_t n, double period_ms, int harmonic,
                            const double* centres, int buckets, double* re, double* im)
{
    for (int b = 0; b < buckets; b++) { re[b] = 0.0; im[b] = 0.0; }
    for (size_t i = 0; i < n; i++)
    {
        const double ph = fmod(t[i] * 1000.0, period_ms) / period_ms;
        int b = 0;
        if (buckets > 1)
            b = fabs(wrap_half(ph - centres[1])) < fabs(wrap_half(ph - centres[0])) ? 1 : 0;
        re[b] += cos((double)harmonic * 2.0 * GATE_PI * ph);
        im[b] += sin((double)harmonic * 2.0 * GATE_PI * ph);
    }
}

// The clock fit's search window, as a fraction of the declared bit.  It is wider than the 5 % the
// gate accepts, on purpose: a take whose time base is 20 % off still has a grid, and finding it is
// what lets the gate say "40 baud, not 50" instead of handing back the nominal period.  What the ends
// have to stay clear of is the lattice a two bit take sits on - half a bit either side, the whole
// point of the window - so both are asserted in the bench's clock section.
#define GATE_CLOCK_LO     0.70
#define GATE_CLOCK_HI     1.30

static void gate_clock(const cap_t* c, double lo, double hi, double baud, gate_clock_t* k)
{
    k->transitions = 0; k->period_ms = 0.0; k->dispersion_pct = 0.0; k->mad_pct = 0.0;
    double* t = NULL;
    const size_t n = gate_crossings(c, lo, hi, &t);
    if (!t) return;                       // fewer than 16 tone changes: no grid to fit
    // A Baudot character is 7.5 bits long - one start, five data, 1.5 stop - so the transitions at a
    // character boundary sit half a bit away from the ones inside a character. Fitting the plain
    // coherence has to reconcile both phases on one grid, and on the DDK2 fixture that pulls the
    // answer to 18.75 ms, 6 % short of the 20 ms the decoder itself uses, with 13.9 % of a bit left
    // over as scatter. Squaring the phase first cancels the half bit and leaves the bit period;
    // the two phases come back below as two centres, which is what the scatter is measured against.
    //
    // the search is deliberately wider than the accept window: a recording whose time base is wrong
    // by 20 % still has a grid, and finding it is what lets the gate say "40 baud, not 50" instead of
    // reporting the nominal period for a take that has no such period at all
    const double nominal = 1000.0 / baud;
    double best_t = nominal, best_r = -1.0;
    for (double T = nominal * GATE_CLOCK_LO; T <= nominal * GATE_CLOCK_HI; T += 0.005)
    {
        double re, im;
        gate_phase_sums(t, n, T, 2, NULL, 1, &re, &im);
        const double r = sqrt(re * re + im * im) / (double)n;
        if (r > best_r) { best_r = r; best_t = T; }
    }
    // The centres start half a phase apart and are then moved to sit on the transitions that chose
    // them, because the half bit a Baudot character boundary leaves is not exactly half and the
    // jitter is not symmetric. The round has a fixed point of its own, though: seed it so that the
    // halfway boundary falls inside one of the two clusters and each bucket collects half of both,
    // the means stay put and the centres never reach the transitions. A take whose transitions are
    // on the grid then reads 24.7 % of a bit of scatter while decoding at 4.1 % - the two-centre fit
    // is the spike, the crossings are fine. Which side of a cluster the boundary lands on follows
    // from where the take happens to start, so the split is run from a dozen seeds around the
    // circle and the tightest of the twelve is kept.
    double centre[2] = {0.0, 0.0};
    {
        double re, im, best_sq = -1.0;
        gate_phase_sums(t, n, best_t, 1, NULL, 1, &re, &im);
        const double base = fmod(atan2(im, re) / (2.0 * GATE_PI) + 1.0, 1.0);
        for (int seed = 0; seed < 12; seed++)
        {
            double c[2];
            c[0] = fmod(base + (double)seed / 12.0, 1.0);
            c[1] = fmod(c[0] + 0.5, 1.0);
            // the fixture's scatter is 9.4 % after one reassignment and settles by the second; four
            // leaves room for the seeds that start further from the clusters
            for (int round = 0; round < 4; round++)
            {
                double br[2], bi[2];
                gate_phase_sums(t, n, best_t, 1, c, 2, br, bi);
                for (int b = 0; b < 2; b++)
                    if (br[b] != 0.0 || bi[b] != 0.0)
                        c[b] = fmod(atan2(bi[b], br[b]) / (2.0 * GATE_PI) + 1.0, 1.0);
            }
            double sq = 0.0;
            for (size_t i = 0; i < n; i++)
            {
                const double ph = fmod(t[i] * 1000.0, best_t) / best_t;
                const double d = fmin(fabs(wrap_half(ph - c[0])), fabs(wrap_half(ph - c[1])));
                sq += d * d;
            }
            if (best_sq < 0.0 || sq < best_sq)
            {
                best_sq = sq;
                centre[0] = c[0];
                centre[1] = c[1];
            }
        }
    }
    double sq = 0.0;
    double* dev = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++)
    {
        const double ph = fmod(t[i] * 1000.0, best_t) / best_t;
        const double d = fmin(fabs(wrap_half(ph - centre[0])), fabs(wrap_half(ph - centre[1])));
        sq += d * d;
        if (dev) dev[i] = d;
    }
    k->period_ms = best_t;
    k->transitions = (int)n;
    k->dispersion_pct = 100.0 * sqrt(sq / (double)n);
    // An RMS over transitions is not a robust statistic: one crossing that landed 40 % of a bit out
    // of place adds 0.4/sqrt(n) to it, so the same audio reports 8.8 % or 19 % depending on which
    // single bit the noise caught (measured on repeated synthetic takes at a fixed SNR). Half the
    // transitions being closer than the median distance is a property of the keying; the worst
    // crossing in the take is not. 1.4826 turns the median absolute deviation into the sigma of a
    // normal distribution, so the number stays comparable with the RMS on a clean take.
    if (dev)
    {
        qsort(dev, n, sizeof(double), cmp_double);
        const double med = (n & 1) ? dev[n / 2] : 0.5 * (dev[n / 2 - 1] + dev[n / 2]);
        k->mad_pct = 100.0 * 1.4826 * med;
        free(dev);
    }
    free(t);
}

typedef struct { double longest_ms, fraction; int gaps; } gate_drop_t;

// a recorder that stalls leaves a stretch of silence in the middle of the take; trimming the ends
// does not remove it, and every bit clock measured across it is wrong
static void gate_dropouts(const cap_t* c, gate_drop_t* d)
{
    const int w = (int)lround(0.001 * c->rate);
    const size_t nw = c->n / (size_t)w;
    d->longest_ms = 0.0; d->fraction = 0.0; d->gaps = 0;
    if (!nw) return;
    double* rms = malloc(nw * sizeof(double));
    double* sorted = malloc(nw * sizeof(double));
    if (!rms || !sorted) { free(rms); free(sorted); return; }
    for (size_t i = 0; i < nw; i++)
    {
        double acc = 0.0;
        for (int k = 0; k < w; k++) acc += (double)c->x[i * (size_t)w + (size_t)k] * (double)c->x[i * (size_t)w + (size_t)k];
        rms[i] = sqrt(acc / (double)w);
    }
    memcpy(sorted, rms, nw * sizeof(double));
    qsort(sorted, nw, sizeof(double), cmp_double);
    const double median = sorted[nw / 2];
    const double thr = 0.15 * median;
    size_t run = 0, longest = 0, below = 0;
    for (size_t i = 0; i < nw; i++)
    {
        if (rms[i] < thr)
        {
            below++;
            if (run == 0) d->gaps++;
            run++;
            if (run > longest) longest = run;
        }
        else run = 0;
    }
    d->longest_ms = (double)longest;
    d->fraction = (double)below / (double)nw;
    free(rms); free(sorted);
}

// Thresholds, set from the takes that decode. Transitions are interpolated zero crossings against
// two phase centres, so these are the audio's numbers and not the estimator's: a clean 50 baud take
// generated for this purpose measures 20.000 ms with 0.6-0.9 % scatter, the DDK2 fixture - real
// audio through the WebSDR client's recorder - 20.000 ms with 7.0 % median (8.8 % rms), the fixture
// stretched by 1, 2 and 3 % either way 19.400 to 20.600 ms with 6.4 to 7.4 % while still decoding at
// 2.7 to 6.8 %, a 45.45 baud take 22.000 ms with 2.2 %, and a 40 baud take 25.000 ms with 0.9 %.
//
// The period window is 5 %, which passes both reference takes at their nominal period and refuses a
// 45.45 baud take at 10 % long even though its pair and its scatter look perfect. The pair and the
// bimodality are what carry a stretch of noise or hash.
//
// The scatter is printed and refuses nothing. It was a 12 % limit on the median absolute deviation,
// picked because that sits above the 7.4 % the worst stretched fixture read, but it refuses takes
// whose text is there: the fixture re-read with 4 ms of wow - the clock wandering by 0.2 of a bit,
// which a receiver's DPLL follows - decodes at 9.6 % CER while its transitions read 13.3 % MAD, and
// at 3 ms of wow 11.0 % at 12.1 %. A recording that decodes is evidence about the modem, so how
// scattered its clock is says how much the take has been through, not whether it may be graded: the
// gate reports the number and the character error rate is what the take is judged on.
#define GATE_SINGLE_MIN   0.55   // windows holding one tone
#define GATE_BOTH_MAX     0.35   // windows holding both: straddled bits plus noise
#define GATE_PAIR_SLACK   30.0   // Hz between the measured pair and the declared shift
#define GATE_PERIOD_TOL   0.05   // fraction of a bit
#define GATE_SCATTER_NOTE 12.0   // percent of a bit: the scatter the gate calls wide, as a note
#define GATE_GAP_MS        5.0   // longest hole a take may carry

// ---- the shape of a take, measured from the file and not from any modem -------------------------
//
// Every one of the four non-RTTY decoders prints text for a signal that is not its mode: the bench's
// own RTTY fixture, run through them, comes back as "ZAUU WBMNM Z ZAVI" from NAVTEX at 0.212 of its
// words being valid, and as "lrnrtl9t dediG f G]rxwatk nRip" from MFSK16 while reporting 0.0 % of its
// symbols heard.  A capture hunt cannot live with that, because a wall of confident garbage is
// indistinguishable from a schedule.  So the gate in front of them measures the spectrum of the file
// and never calls a modem, which is the same reason RTTY's own gate does not call rtty.c: a modem
// that agrees with itself is not evidence.
//
// Three numbers tell these five shapes apart, and they are all widths rather than levels, so a take
// that is quiet, loud, or trimmed to its peak still reads the same:
//
//   bw95   the narrowest band holding 95 % of the energy between 250 and 3000 Hz
//   bw50   the same at 50 %, which is how flat the top of that band is
//   centre where that band sits
//
// MFSK16 is 234 Hz of flat-topped comb, Olivia is 1000 Hz of it, NAVTEX is a 170 Hz pair, Hell is one
// keyed tone, and RTTY is two tones further apart than NAVTEX.  bw95 separates Olivia from everything
// else; fill = bw50/bw95 separates the comb modes from the two-tone and single-tone ones; centre
// separates NAVTEX from MFSK16, whose band is 500 Hz lower.

typedef struct
{
    double bw95, bw50, fill;         // Hz, Hz, dimensionless
    double lo_hz, hi_hz, centre_hz;  // the 95 % band and its middle
    double peak_hz[8];               // local maxima above 12 % of the loudest, 25 Hz apart
    double peak_db[8];               // and their level relative to the loudest
    int peaks;
    int windows;                     // one second windows averaged, so a short take says so
} gate_shape_t;

#define SHAPE_F_LO     250.0
#define SHAPE_F_HI    3000.0
#define SHAPE_STEP       1.0   // one second windows, so a bin is 1 Hz
#define SHAPE_MAX_WIN    24    // seconds averaged: enough for a comb, not enough to cost a minute
#define SHAPE_PEAK_DB   -18.0  // a maximum this far under the loudest is not a tone of the mode

// The median bin of the spectrum, which is the noise floor: these modes put their energy into a few
// bins, so most bins are whatever the receiver brought with them.  It sorts a copy - the caller's
// array is the spectrum itself, and a median that rearranged it would leave the band search walking a
// sorted list instead of a frequency axis.  The first version of this did exactly that, and every mode
// read a centre of 2900 Hz because the loudest bins had moved to the end of the array.
static double gate_median(const double* src, int n)
{
    double* v = calloc(n, sizeof(double));
    if (!v) return 0.0;
    memcpy(v, src, n * sizeof(double));
    // A median of a couple of thousand doubles does not need a library, and insertion sort over 2751
    // of them is 3.8 M swaps: nothing next to the Goertzel pass that produced them.
    for (int i = 1; i < n; i++)
    {
        const double x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    const double m = n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    free(v);
    return m;
}

static void gate_shape(const cap_t* c, gate_shape_t* s)
{
    memset(s, 0, sizeof(*s));
    const int win = (int)lround((double)c->rate);
    if (win < 32 || c->n < (size_t)win)
    {
        return;                      // shorter than one bin of resolution: nothing to say
    }
    const int nb = (int)lround((SHAPE_F_HI - SHAPE_F_LO) / SHAPE_STEP) + 1;
    double* acc = calloc(nb, sizeof(double));
    double* en = calloc(nb, sizeof(double));
    if (!acc || !en) { free(acc); free(en); return; }

    int used = 0;
    for (int b = 0; b < nb; b++)
    {
        const double f = SHAPE_F_LO + SHAPE_STEP * b;
        const double w = 2.0 * GATE_PI * f / (double)c->rate;
        const double coeff = 2.0 * cos(w);
        double sum = 0.0;
        int frames = 0;
        for (size_t st = 0; st + win <= c->n && frames < SHAPE_MAX_WIN; st += win)
        {
            // Goertzel: two multiplies a sample, and no table of sines the size of the file
            double s0 = 0.0, s1 = 0.0, s2 = 0.0;
            for (int k = 0; k < win; k++)
            {
                s0 = (double)c->x[st + k] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            const double re = s1 - s2 * cos(w);
            const double im = s2 * sin(w);
            sum += sqrt(re * re + im * im) / (double)win;
            frames++;
        }
        acc[b] = frames ? sum / frames : 0.0;
        if (frames > used) used = frames;
    }
    s->windows = used;
    if (!used) { free(acc); free(en); return; }

    // Subtract the noise floor before measuring any width.  A band holding 95 % of the *total* energy
    // is a statement about the hiss, not about the mode: at +3 dB over 12 kHz the noise spans the
    // passband on its own, and NAVTEX read 2001 Hz wide while its decoder was copying the text
    // exactly.  The median bin is the floor - these modes put their energy into a few bins, so most
    // bins are whatever the receiver brought - and what sits above it is the signal's width, which is
    // the thing the bands in kGateExpect are about.
    for (int b = 0; b < nb; b++) en[b] = acc[b] * acc[b];
    const double floor_e = gate_median(en, nb);
    double total = 0.0;
    for (int b = 0; b < nb; b++)
    {
        en[b] = en[b] > floor_e ? en[b] - floor_e : 0.0;
        total += en[b];
    }
    if (total <= 0.0)
    {
        // nothing above the floor anywhere: a take that is only noise, or only silence
        free(acc); free(en);
        return;
    }

    // Narrowest band holding `share` of the energy above the floor, for both shares at once: walk the
    // right edge out and pull the left edge in while what is left still holds the share.
    int b_lo[2] = { 0, 0 }, b_hi[2] = { 0, 0 };
    double width[2] = { 1e30, 1e30 };
    for (int which = 0; which < 2; which++)
    {
        const double want = (which == 0 ? 0.95 : 0.50) * total;
        int l = 0;
        double e = 0.0;
        for (int r = 0; r < nb; r++)
        {
            e += en[r];
            while (l < r && e - en[l] >= want) { e -= en[l]; l++; }
            if (e >= want && r - l < width[which]) { width[which] = r - l; b_lo[which] = l; b_hi[which] = r; }
        }
    }
    s->bw95 = width[0] < 1e29 ? width[0] * SHAPE_STEP : 0.0;
    s->bw50 = width[1] < 1e29 ? width[1] * SHAPE_STEP : 0.0;
    s->lo_hz = SHAPE_F_LO + SHAPE_STEP * b_lo[0];
    s->hi_hz = SHAPE_F_LO + SHAPE_STEP * b_hi[0];
    s->centre_hz = 0.5 * (s->lo_hz + s->hi_hz);
    s->fill = s->bw95 > 0.0 ? s->bw50 / s->bw95 : 0.0;

    double mx = 0.0;
    for (int b = 0; b < nb; b++) if (acc[b] > mx) mx = acc[b];
    if (mx > 0.0)
    {
        for (int b = 2; b + 2 < nb; b++)
        {
            if (acc[b] < 0.12 * mx) continue;
            if (!(acc[b] >= acc[b - 1] && acc[b] >= acc[b + 1] && acc[b] > acc[b - 2] && acc[b] > acc[b + 2])) continue;
            const double f = SHAPE_F_LO + SHAPE_STEP * b;
            const double db = 20.0 * log10(acc[b] / mx + 1e-30);
            if (db < SHAPE_PEAK_DB) continue;
            int at = -1;
            for (int k = 0; k < s->peaks; k++) if (fabs(s->peak_hz[k] - f) < 25.0) at = k;
            if (at < 0 && s->peaks < 8) { at = s->peaks++; s->peak_hz[at] = f; s->peak_db[at] = db; }
            else if (at >= 0 && db > s->peak_db[at]) s->peak_db[at] = db;
        }
        for (int i = 1; i < s->peaks; i++)      // loudest first, so the printed list is the short one
            for (int j = i; j > 0 && s->peak_db[j] > s->peak_db[j - 1]; j--)
            {
                double t = s->peak_db[j]; s->peak_db[j] = s->peak_db[j - 1]; s->peak_db[j - 1] = t;
                t = s->peak_hz[j]; s->peak_hz[j] = s->peak_hz[j - 1]; s->peak_hz[j - 1] = t;
            }
    }
    free(acc);
    free(en);
}

// What each of these modes occupies, as a band, a flatness and a place.  The ranges are the measured
// spread of the takes this bench can make - see gate-check, which prints the matrix these came from -
// widened to the nearest ten, and they are a gate rather than a description: a take outside them is
// not this mode, or is this mode under conditions the bench has not measured.
typedef struct
{
    const char* mode;
    double bw95_lo, bw95_hi;
    double fill_lo, fill_hi;
    double ctr_lo, ctr_hi;
} gate_expect_t;

// RTTY is deliberately not in this table.  Its shape is two tones at a shift nobody but the operator
// knows, at a baud rate that is a menu of five, so a band that contains real RTTY - the DWD take in
// fixtures/ is 1951 Hz of it, because a live recording carries its hiss and its neighbours - also
// contains NAVTEX, and the first version of this table had exactly that false positive.  RTTY already
// has a gate, and a better one: capture-check measures the pair, the bimodality of the windows and the
// transition grid, and it never calls rtty.c either.  What is left here is the four modes that had no
// gate at all.
static const gate_expect_t kGateExpect[] =
{
    { "NAVTEX",  120.0, 320.0, 0.08, 0.45, 1450.0, 1950.0 },
    { "MFSK16",  170.0, 330.0, 0.25, 0.65,  900.0, 1350.0 },
    { "Olivia",  700.0, 1300.0, 0.30, 0.70, 1100.0, 1750.0 },
    { "Hell",     60.0, 300.0, 0.01, 0.20,  650.0, 1650.0 },
};

static void gate_shape_report(const gate_shape_t* s)
{
    printf("shape     : bw95 %.0f Hz (%.0f..%.0f), bw50 %.0f Hz, fill %.2f, centre %.0f Hz, %d s averaged\n",
           s->bw95, s->lo_hz, s->hi_hz, s->bw50, s->fill, s->centre_hz, s->windows);
    if (s->peaks)
    {
        printf("tones     :");
        for (int k = 0; k < s->peaks; k++) printf(" %.0f (%+.0f dB)", s->peak_hz[k], s->peak_db[k]);
        printf("\n");
    }
}

// The gate's answer for one mode: 1 if the take's shape is inside that mode's band, and the margin of
// the tightest of the three tests, so a caller can rank modes instead of only accepting one.
static int gate_shape_is(const gate_shape_t* s, const gate_expect_t* e, double* margin)
{
    if (s->windows <= 0) return 0;
    const double m_bw = fmin(s->bw95 - e->bw95_lo, e->bw95_hi - s->bw95);
    const double m_fl = fmin(s->fill - e->fill_lo, e->fill_hi - s->fill);
    const double m_ct = fmin(s->centre_hz - e->ctr_lo, e->ctr_hi - s->centre_hz);
    *margin = fmin(fmin(m_bw, m_fl * 400.0), m_ct);
    return m_bw >= 0.0 && m_fl >= 0.0 && m_ct >= 0.0;
}

// The gate in front of a decoder.  Three answers, and only the middle one refuses anything:
//   this mode's shape   -> decode
//   another mode's      -> refuse, because the take is somebody else's signal
//   nobody's            -> decode anyway, and say that the shape test has no answer here
// The third case is not a hedge.  Below about +3 dB over the passband the median bin stops being the
// noise between the tones, every width runs to the top of the range, and a gate that refused then would
// be throwing away exactly the weak takes the modes are for.
static int gate_shape_gate(const float* x, size_t n, const char* mode)
{
    cap_t c;
    c.x = (float*)x;
    c.n = n;
    c.rate = 12000;
    c.ch = 1;
    gate_shape_t s;
    gate_shape(&c, &s);
    if (!s.windows)
    {
        printf("shape     : too short to measure (needs a second of audio)\n");
        return 0;
    }
    gate_shape_report(&s);
    const int n_exp = (int)(sizeof(kGateExpect) / sizeof(kGateExpect[0]));
    int mine = 0, other = -1;
    for (int i = 0; i < n_exp; i++)
    {
        double m = 0.0;
        if (!gate_shape_is(&s, &kGateExpect[i], &m)) continue;
        if (!strcmp(kGateExpect[i].mode, mode)) mine = 1;
        else if (other < 0) other = i;
    }
    if (mine)
    {
        printf("gate      : the shape is %s's, decoding\n", mode);
        return 0;
    }
    if (other >= 0)
    {
        printf("gate      : the shape is %s's, not %s's: %.0f Hz at %.0f Hz, fill %.2f.  Refusing, "
               "because %s will read this as text that was never sent\n",
               kGateExpect[other].mode, mode, s.bw95, s.centre_hz, s.fill, mode);
        return 1;
    }
    printf("gate      : the shape is none of the four (%.0f Hz at %.0f Hz, fill %.2f) - below about "
           "+3 dB this test has no answer, so decoding on the modem's own terms\n",
           s.bw95, s.centre_hz, s.fill);
    return 0;
}

static int mode_id(const char* path)
{
    cap_t c;
    if (!cap_load(path, &c))
    {
        printf("NO FILE   : mode-id never opened %s\n", path);
        return 2;
    }
    gate_shape_t s;
    gate_shape(&c, &s);
    printf("capture   : %s\naudio     : %u Hz, %.2f s\n", path, c.rate, (double)c.n / (double)c.rate);
    gate_shape_report(&s);
    const int n = (int)(sizeof(kGateExpect) / sizeof(kGateExpect[0]));
    int best = -1;
    double best_m = -1e30;
    printf("modes     :");
    for (int i = 0; i < n; i++)
    {
        double m = 0.0;
        const int in = gate_shape_is(&s, &kGateExpect[i], &m);
        if (in && m > best_m) { best_m = m; best = i; }
        printf(" %s%s", kGateExpect[i].mode, in ? "" : "-");
    }
    printf("\n");
    if (best >= 0)
    {
        printf("verdict   : %s, by %.0f of the margin on its tightest test\n", kGateExpect[best].mode, best_m);
    }
    else
    {
        printf("verdict   : none of the five: the shape is outside every band this bench has measured\n");
    }
    free(c.x);
    return best >= 0 ? 0 : 1;
}

// ---- calibrating the shape gate -----------------------------------------------------------------
//
// The bands in kGateExpect are a claim about real audio, and the only real audio in this tree is one
// DWD teletype take, which the first version of this gate refused outright: the bench's own RTTY
// signals are 250 to 900 Hz wide and the station is 1955 Hz, because a live recording carries its own
// hiss, its neighbours and the shape of whatever filter the receiver left in.  So the bands are
// measured here, over a ladder of noise, and printed before they are asserted: what it costs to widen
// one is visible as a control row starting to pass.

static void gate_row(const char* label, const float* x, size_t n, uint32_t rate, int* failures,
                     const char* must_pass, int expect_pass)
{
    cap_t c;
    c.x = (float*)x;
    c.n = n;
    c.rate = rate;
    c.ch = 1;
    gate_shape_t s;
    gate_shape(&c, &s);
    const int n_exp = (int)(sizeof(kGateExpect) / sizeof(kGateExpect[0]));
    char accepts[64] = "";
    int mine_ok = 1;
    for (int i = 0; i < n_exp; i++)
    {
        double m = 0.0;
        const int in = gate_shape_is(&s, &kGateExpect[i], &m);
        if (in)
        {
            strncat(accepts, accepts[0] ? " " : "", sizeof(accepts) - 1);
            strncat(accepts, kGateExpect[i].mode, sizeof(accepts) - 1);
        }
        if (must_pass && !strcmp(kGateExpect[i].mode, must_pass)) mine_ok = in;
    }
    printf("  %-22s bw95 %5.0f  fill %.2f  centre %5.0f  accepted by: %-24s", label, s.bw95, s.fill,
           s.centre_hz, accepts[0] ? accepts : "nothing");
    if (must_pass)
    {
        const int good = expect_pass ? mine_ok : !accepts[0];
        printf("  %s\n", good ? "ok" : "FAIL");
        if (!good) (*failures)++;
    }
    else
    {
        printf("\n");
    }
}

static int gate_check(void)
{
    int failures = 0;
    const size_t cap = 400000;
    float* x = malloc(cap * sizeof(float));
    float* work = malloc(cap * sizeof(float));
    if (!x || !work) { printf("out of memory\n"); free(x); free(work); return 1; }

    printf("Shape gate: what each mode's take looks like as noise is added, and what the bands in\n"
           "kGateExpect let through.  Nothing here calls a modem: the numbers are the file's spectrum\n"
           "over 250..3000 Hz, one second windows, and the bands are widths, so trimming the gain of a\n"
           "take cannot move it.  `accepted by` is every mode whose band the shape is inside.\n\n");

    printf("own takes, over the noise ladder\n");
    static const float snrs[] = { 100.0f, 10.0f, 3.0f, -6.0f, -12.0f, -18.0f };
    const size_t nsnr = sizeof(snrs) / sizeof(snrs[0]);

    struct { const char* name; const char* msg; int kind; } modes[] =
    {
        { "NAVTEX", "NAVTEX TEST MESSAGE DE UHSDR BENCH 12345 CQ CQ", 0 },
        { "MFSK16", "MFSK16 TEST DE UHSDR 12345", 1 },
        { "Olivia", "OLIVIA TEST DE UHSDR 12345", 2 },
        { "Hell",   "HELLSCHREIBER TEST DE UHSDR 123", 3 },
    };
    const size_t nmodes = sizeof(modes) / sizeof(modes[0]);

    for (size_t m = 0; m < nmodes; m++)
    {
        size_t n = 0;
        uint8_t truth[40000];
        int cols = 0;
        switch (modes[m].kind)
        {
            case 0: n = Bench_NavtexGenerate((float32_t*)x, cap, modes[m].msg, 0.0f, NAVTEX_ORDER_TABLE); break;
            case 1: n = Bench_Mfsk16Generate((float32_t*)x, cap, modes[m].msg, 0.0f); break;
            case 2: n = Bench_OliviaGenerate((float32_t*)x, cap, modes[m].msg, 0.0f); break;
            default: n = Bench_HellGenerate((float32_t*)x, cap, modes[m].msg, truth, 4000, &cols, 1000.0f, 0.0f); break;
        }
        if (!n) { printf("  %-6s nothing generated\n", modes[m].name); failures++; continue; }
        for (size_t k = 0; k < nsnr; k++)
        {
            memcpy(work, x, n * sizeof(float));
            char label[64];
            if (snrs[k] > 90.0f)
            {
                snprintf(label, sizeof(label), "%s clean", modes[m].name);
            }
            else
            {
                snprintf(label, sizeof(label), "%s snr %+.0f", modes[m].name, (double)snrs[k]);
                Bench_RngSeed(0x51A7E5u + (uint32_t)(k * 7 + m));
                Bench_AddNoise(work, n, snrs[k], 0.0f);
            }
            // the modes that decode cleanly at these points is the README's business; here every row
            // down to the one where the shape stops being recognisable has to be its own mode's
            gate_row(label, work, n, 12000, &failures, modes[m].name, snrs[k] >= 3.0f);
        }
        printf("\n");
    }

    printf("controls: shapes that are not any of the four\n");
    {
        const size_t n = 60000;
        Bench_RngSeed(0xC0FFEEu);
        for (size_t k = 0; k < n; k++) work[k] = 0.0f;
        gate_row("silence", work, n, 12000, &failures, NULL, 0);

        Bench_RngSeed(0xC0FFEEu);
        for (size_t k = 0; k < n; k++) work[k] = 0.0f;
        Bench_AddNoise(work, n, -20.0f, 0.2f);          // noise alone, no signal at all
        gate_row("noise only", work, n, 12000, &failures, NULL, 0);

        for (size_t k = 0; k < n; k++) work[k] = 0.4f * sinf(2.0f * GATE_PI * 1100.0f * k / 12000.0f);
        gate_row("single tone 1100", work, n, 12000, &failures, NULL, 0);

        // a wide band of noise 900..1300 Hz: MFSK16's own band, with none of its structure
        Bench_RngSeed(0xB0BA7Cu);
        for (size_t k = 0; k < n; k++) work[k] = 0.0f;
        Bench_AddNoise(work, n, -20.0f, 0.2f);
        Bench_AddNoiseBand(work, n, 0.0f, 0.25f, 900.0f, 1300.0f);
        gate_row("band noise 900..1300", work, n, 12000, &failures, NULL, 0);
    }

    printf("\nthe one real take in this tree\n");
    {
        cap_t c;
        if (cap_load("fixtures/ddk2-4583.wav", &c))
        {
            gate_row("ddk2 4583 kHz (RTTY)", c.x, c.n, c.rate, &failures, NULL, 0);
            free(c.x);
        }
        else
        {
            printf("  fixtures/ddk2-4583.wav would not open\n");
            failures++;
        }
    }

    free(x);
    free(work);
    if (failures)
    {
        printf("\ngate: %d row(s) outside what the bands claim - read them against the table above\n", failures);
    }
    return failures ? 1 : 0;
}


static int capture_check(const char* path, uint16_t shift, uint16_t baud)
{
    cap_t c;
    if (!cap_load(path, &c))
    {
        // 1 means the gate measured the take and refused it; this measured nothing, and the two
        // have to be told apart by anything that decides whether a take is worth pinning
        printf("NO FILE   : capture-check never opened %s, so it graded nothing\n", path);
        return 2;
    }
    printf("capture   : %s\n", path);
    float peak = 0.0f;
    for (size_t k = 0; k < c.n; k++) if (fabsf(c.x[k]) > peak) peak = fabsf(c.x[k]);
    printf("audio     : %u Hz, %.2f s, peak %.2f of full scale\n", c.rate, (double)c.n / (double)c.rate,
           (double)peak);

    double clo = 0.0, chi = 0.0;
    if (!gate_coarse_pair(&c, &clo, &chi))
    {
        printf("REJECT    : less than two tones in 300..2200 Hz, so there is no pair to measure\n");
        free(c.x);
        return 1;
    }
    gate_pair_t pair;
    gate_pair_stats(&c, clo, chi, &pair);
    const double sep = pair.hi_hz - pair.lo_hz;
    const double centre = 0.5 * (pair.hi_hz + pair.lo_hz);

    gate_clock_t clock;
    gate_clock(&c, pair.lo_hz, pair.hi_hz, (double)baud, &clock);
    gate_drop_t drop;
    gate_dropouts(&c, &drop);

    printf("tones     : %.0f / %.0f Hz, separation %.0f Hz (declared %u), centre %.0f Hz\n",
           pair.lo_hz, pair.hi_hz, sep, (unsigned)shift, centre);
    printf("            spread %.1f / %.1f Hz; offset from the modem's %.0f/%.0f %.0f / %.0f Hz\n",
           pair.lo_spread, pair.hi_spread, (double) bench_mark_hz(), (double) bench_space_hz(shift),
           (double) bench_mark_hz() - pair.lo_hz, (double) bench_space_hz(shift) - pair.hi_hz);
    printf("bimodality: %.2f low, %.2f high, %.2f both (5 ms windows, %d)\n",
           pair.f_low, pair.f_high, pair.f_both, pair.windows);
    if (clock.transitions)
    {
        printf("clock     : period %.3f ms (%d transitions), scatter %.1f %% rms / %.1f %% MAD of a bit\n",
               clock.period_ms, clock.transitions, clock.dispersion_pct, clock.mad_pct);
    }
    else
    {
        printf("clock     : no transition grid: fewer than 16 tone changes\n");
    }
    printf("dropouts  : %d gap(s) below 15 %% of the median, longest %.0f ms (%.1f %% of the take)\n",
           drop.gaps, drop.longest_ms, 100.0 * drop.fraction);

    int bad = 0;
    const double nominal = 1000.0 / (double)baud;
    if (fabs(sep - (double)shift) > GATE_PAIR_SLACK)
    {
        printf("REJECT    : the pair is %.0f Hz apart, not the %u Hz the modem will filter\n",
               sep, (unsigned)shift);
        bad = 1;
    }
    if (pair.f_low + pair.f_high < GATE_SINGLE_MIN || pair.f_both > GATE_BOTH_MAX)
    {
        printf("REJECT    : %.2f of the take holds one tone and %.2f holds both; that is not a\n"
               "            two tone signal being switched, whatever its spectrum looks like\n",
               pair.f_low + pair.f_high, pair.f_both);
        bad = 1;
    }
    if (!clock.transitions || fabs(clock.period_ms - nominal) > nominal * GATE_PERIOD_TOL)
    {
        // the rate the take actually carries, so a wrong time base reads as the baud rate it is and
        // not only as a period in milliseconds
        printf("REJECT    : no %u baud clock (wanted %.2f ms, measured %.2f ms = %.1f baud, "
               "scatter %.1f %% rms / %.1f %% MAD)\n",
               (unsigned)baud, nominal, clock.period_ms,
               clock.period_ms > 0.0 ? 1000.0 / clock.period_ms : 0.0, clock.dispersion_pct,
               clock.mad_pct);
        bad = 1;
    }
    else if (clock.mad_pct > GATE_SCATTER_NOTE)
    {
        // reported, not refused: a take whose clock wanders still carries its message, and the gate
        // is not the thing that decides whether text came back - rtty-wav is
        printf("note      : scatter %.1f %% MAD of a bit is wide; that is reported, not refused -\n"
               "            run rtty-wav for the take's CER and grade it on that\n", clock.mad_pct);
    }
    if (drop.longest_ms > GATE_GAP_MS)
    {
        printf("REJECT    : %.0f ms of silence in the middle of the take\n", drop.longest_ms);
        bad = 1;
    }
    if (!bad)
    {
        printf("ACCEPT    : the pair, the bit clock and the continuity are all inside the gate\n");
    }
    free(c.x);
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  where the errors are: the alignment, with the audio under each character
 * ------------------------------------------------------------------ */
// score_aligned() says how many characters are wrong and never which. That grades a modem against
// a message; it does not explain the result, and "the remaining CER is signal quality" was an
// assumption here until this table existed. One row per character the decoder reported, with the
// character it produced and what the audio was doing in that character's own 7.5 bit window:
//
//   - both tone levels, in dB of full scale, so a fade in one tone or in both reads as a dip
//     against the rows around it;
//   - the transitions in the window, against the 1 to 6 a character of start, data and stop bits
//     can carry, so a window the signal never reached reads as a near empty one;
//   - the RMS distance of those transitions from the bit grid, in ms: a clean take sits inside a
//     millisecond, and keying that has been smeared or delayed reads larger.
//
// A character that came out wrong while its window looks like its neighbours is the decoder's; one
// whose window is empty or half level is the signal's.
typedef struct { double start_s, end_s; } align_win_t;

// Levenshtein with the edit script kept: match[i] is the index in dec that expected character i was
// read as, or -1 where the modem inserted or dropped one. The alignment table and the bit trace
// both need it, so it lives on its own rather than twice.
static int align_chars(const char* exp, const char* dec, int* match)
{
    const size_t ne = strlen(exp), nd = strlen(dec), W = nd + 1;
    int* d = malloc((ne + 1) * W * sizeof(int));
    if (!d) return -1;
    for (size_t i = 0; i <= ne; i++) d[i * W] = (int)i;
    for (size_t j = 0; j <= nd; j++) d[j] = (int)j;
    for (size_t i = 1; i <= ne; i++)
        for (size_t j = 1; j <= nd; j++)
        {
            const int sub = d[(i - 1) * W + (j - 1)] + (exp[i - 1] == dec[j - 1] ? 0 : 1);
            const int del = d[(i - 1) * W + j] + 1, ins = d[i * W + (j - 1)] + 1;
            int best = sub < del ? sub : del;
            if (ins < best) best = ins;
            d[i * W + j] = best;
        }
    for (size_t i = 0; i <= ne; i++) match[i] = -1;
    // Gaps before substitutions when the cost is the same. One character arriving beside the text
    // and one character being read wrong as its neighbour are the same edit distance, but they are
    // not the same record: preferring the gap keeps every identical character matched, so an
    // insertion is reported as an insertion and a misread as a misread.
    for (size_t i = ne, j = nd; i > 0 || j > 0; )
    {
        if (i > 0 && d[i * W + j] == d[(i - 1) * W + j] + 1) i--;
        else if (j > 0 && d[i * W + j] == d[i * W + (j - 1)] + 1) j--;
        else { match[i - 1] = (int)(j - 1); i--; j--; }
    }
    free(d);
    return 0;
}

// One row per expected character, kept so a pinned take can be checked on the characters it got
// wrong and the audio under them rather than on the character error rate alone.
typedef struct
{
    char   exp, got;
    int    trans, ok;
    double mark_db, space_db;
} align_row_t;

// one row per character the expected text carries, so the array is the decode buffer's own length:
// the text is normalised into a buffer that size and normalising cannot lengthen it
static align_row_t align_row[8192];
static size_t align_rows;
// the edit script beside the per-character table, and the decoder's unnormalised text: an inserted
// character has no row of its own, and the row a space should have is taken by the take's line break
static int  align_ins, align_del, align_sub;
static char align_raw[8192];
// the level the take's own characters sit at, averaged over the ones it read as sent and keyed at
// all: the junction is checked against this, the same number the table prints, and not against a
// level carried over from another take
static double align_ref_mark, align_ref_space;

static int align_wav(const char* path, const char* expected, uint16_t shift, uint16_t baud)
{
    if (!expected || !*expected)
    {
        printf("an alignment needs the text the take carries\n");
        return 2;
    }
    align_rows = 0;
    char got[8192];
    Bench_CharCount = 0;
    if (decode_wav(path, expected, got, sizeof(got), shift, baud, false, align_raw,
                   sizeof(align_raw)) < 0.0f) return 1;

    const char* exp = norm_baudot(expected, 0);
    const char* dec = got + lead_in_junk;          // the score skips the junk before the lock
    const size_t ne = strlen(exp), nd = strlen(dec);
    const double bit_s = 1.0 / (double)baud;
    const double char_s = 7.5 * bit_s;

    cap_t c;
    if (!cap_load(path, &c)) return 1;
    double clo = 0.0, chi = 0.0;
    if (!gate_coarse_pair(&c, &clo, &chi)) { free(c.x); return 1; }
    gate_pair_t pair;
    gate_pair_stats(&c, clo, chi, &pair);
    double* t = NULL;
    const size_t nt = gate_crossings(&c, pair.lo_hz, pair.hi_hz, &t);

    int* match = malloc((ne + 1) * sizeof(int));
    if (!match || align_chars(exp, dec, match) != 0)
    {
        free(match); free(t); free(c.x);
        printf("out of memory aligning the decode\n");
        return 1;
    }

    // The edit script, read from the expected side: a row with no partner is a character the take
    // does not carry, a decoded character with no row is one the take added, and a paired row whose
    // characters differ is a character read wrong. Keeping them apart is the difference between
    // "one character arrived beside the text" and "two characters were misread".
    int ins = 0, del = 0, sub = 0;
    {
        static char paired[8192];
        memset(paired, 0, nd);
        for (size_t i = 0; i < ne; i++)
        {
            if (match[i] < 0) { del++; continue; }
            paired[match[i]] = 1;
            if (dec[match[i]] != exp[i]) sub++;
        }
        for (size_t j = 0; j < nd; j++)
            if (!paired[j]) ins++;
    }
    align_ins = ins; align_del = del; align_sub = sub;

    // No timing column: a character carries two or three transitions, and a phase fitted from those
    // same transitions leaves a residual that says how well the fit worked, not how well the clock
    // ran - a single transition always fits exactly. The clock is measured over the whole take by
    // capture-check and slot by slot by rtty-trace; this table reports what each character carried.
    printf("\n%3s %3s %3s %9s %9s %5s\n", "#", "exp", "got", "mark dB", "space dB", "trans");
    double sum_mark = 0.0, sum_space = 0.0, last_s0 = 0.0;
    int good = 0;
    for (size_t i = 0; i < ne; i++)
    {
        // The decoder reports a character when it has finished reading it, so its own report places
        // the window: one character back from there, less half a bit of slack for where in the stop
        // bits it reports. The start bit's leading edge is a transition, so that guess is then latched
        // onto the nearest one within half a bit, which is what makes the placement exact.
        // A character the modem dropped has no report and keeps the slot after the one before it.
        const int jj = match[i];
        double s0s = last_s0 + char_s;
        if (jj >= 0 && (size_t)lead_in_junk + (size_t)jj < (size_t)Bench_CharCount)
            s0s = (double)Bench_CharAt[lead_in_junk + jj] / 12000.0 - char_s - 0.5 * bit_s;
        // latch onto the start bit's leading edge, so the bit centres below are sampled where the
        // bits are; the latch never moves the window further than half a bit
        {
            double best_t = s0s, near = 1.0;
            for (size_t q = 0; q < nt; q++)
            {
                const double dd = fabs(t[q] - s0s);
                if (dd < 0.010 && dd < near) { near = dd; best_t = t[q]; }
            }
            s0s = best_t;
        }
        last_s0 = s0s;
        const double s1s = s0s + char_s;
        size_t s0 = (size_t)lround(s0s * (double)c.rate), s1 = (size_t)lround(s1s * (double)c.rate);
        if (s1 > c.n) s1 = c.n;
        if (s0 >= s1) s0 = s1 ? s1 - 1 : 0;
        const double lo_m = gate_tone(c.x + s0, (int)(s1 - s0), pair.lo_hz, c.rate);
        const double hi_m = gate_tone(c.x + s0, (int)(s1 - s0), pair.hi_hz, c.rate);
        const double mark_db = 20.0 * log10(lo_m + 1e-9);
        const double space_db = 20.0 * log10(hi_m + 1e-9);
        // how often the tone changed inside the character's own window: a character with its keying
        // under it changes at least once, a window that landed on silence does not change at all
        int trans = 0;
        for (size_t q = 0; q < nt; q++)
            if (t[q] >= s0s && t[q] < s1s) trans++;
        const char have_c = jj >= 0 ? dec[jj] : '-';
        const int ok = have_c == exp[i];          // '-' is never a character the text carries
        printf("%3zu %3c %3c %9.1f %9.1f %5d %s\n", i + 1, exp[i], have_c,
               mark_db, space_db, trans, ok ? "" : "*");
        align_row_t* r = &align_row[align_rows++];
        r->exp = exp[i]; r->got = have_c; r->trans = trans; r->ok = ok;
        r->mark_db = mark_db; r->space_db = space_db;
        if (ok && trans) { good++; sum_mark += mark_db; sum_space += space_db; }
    }
    const size_t distance = Bench_Levenshtein(exp, dec);
    printf("\n%zu expected, %zu decoded: %d inserted, %d not in the take, %d substituted; lead in "
           "%d characters\n", ne, nd, ins, del, sub, lead_in_junk);
    printf("edit distance %zu (%.1f %% over %zu characters), which is what the scorer charges\n",
           distance, ne ? 100.0 * (double)distance / (double)ne : 0.0, ne);
    if (good)
    {
        printf("the %d characters that decoded: mark %.1f dB, space %.1f dB on average\n",
               good, sum_mark / good, sum_space / good);
    }
    align_ref_mark = good ? sum_mark / good : NAN;
    align_ref_space = good ? sum_space / good : NAN;
    printf("%s\n", nt ? "* marks a character the modem did not reproduce as sent"
                      : "no transitions: nothing to place the windows against");

    free(match); free(t); free(c.x);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  the pinned junction: which characters, not only how many
 * ------------------------------------------------------------------ */
// What is left at the DDK9 junction is one inserted character, not two misreads. A per-character
// table cannot show an insertion - it has a row per character the pinned text carries - so the record
// is checked from both sides: the edit script names the insertion, the table says the nine came back
// as sent, and the take's own line break is what stands where the pinned text has a space. The
// inserted slot's own code is the decoder's reading (16, '5'); the audio there carries 24, '9', one
// data bit away, so no character the carried text has is read wrong. If the insertion stops being an
// insertion, or a carried character starts being read wrong, the record has to be rewritten rather
// than quietly left standing.
static int align_check(const char* path, const char* expected, uint16_t shift, uint16_t baud)
{
    const int rc = align_wav(path, expected, shift, baud);
    if (rc) return rc;
    if (align_rows < 27)
    {
        printf("FAIL  the alignment reported %zu characters, too few for the pinned junction\n",
               align_rows);
        return 1;
    }
    if (!isfinite(align_ref_mark))
    {
        printf("FAIL  nothing in the take decoded, so there is no level to compare the junction\n"
               "      with\n");
        return 1;
    }
    const double fade_db = 12.0;      // this far under the take's own level is a fade, not keying
    int bad = 0;
    printf("\npinned junction: one inserted character, and the line break where the pinned space is\n");
    printf("%3s %3s %3s %9s %9s %10s %5s  %s\n", "#", "exp", "got", "mark dB", "space dB",
           "vs take", "trans", "verdict");
    for (int i = 25; i < 27; i++)
    {
        const align_row_t* r = &align_row[i];
        // the worse of the two tones: a window that has lost half its keying is a fade whether the
        // louder tone is still where it was or not
        const double worst = fmin(r->mark_db - align_ref_mark, r->space_db - align_ref_space);
        const int keyed = r->trans >= 1 && worst > -fade_db;
        printf("%3d %3c %3c %9.1f %9.1f %+10.1f %5d  %s\n", i + 1, r->exp, r->got, r->mark_db,
               r->space_db, worst, r->trans,
               r->ok ? "decoded as sent" : r->got == '-' ? "the take's line break here" : "MISREAD");
        if (!keyed)
        {
            printf("FAIL  character %d carries %d transitions and sits %.1f dB under the take's own\n"
                   "      level: a fade, not keying\n", i + 1, r->trans, worst);
            bad = 1;
        }
    }
    if (!align_row[25].ok || align_row[25].exp != '9')
    {
        printf("FAIL  the character before the break is %c/%c, not the 9 read as sent: the\n"
               "      insertion is supposed to sit beside the text, not in place of it\n",
               align_row[25].exp, align_row[25].got);
        bad = 1;
    }
    if (align_row[26].got != '-')
    {
        printf("FAIL  the pinned space is not the take's line break: got '%c'\n", align_row[26].got);
        bad = 1;
    }
    printf("inserted: %d, substituted: %d, not in the take: %d\n", align_ins, align_sub, align_del);
    if (align_ins != 1 || align_sub != 0 || align_del != 1)
    {
        printf("FAIL  the junction is %d inserted / %d substituted / %d not in the take; the record\n"
               "      is one inserted character, no carried character read wrong, and the space the\n"
               "      take breaks the line at\n", align_ins, align_sub, align_del);
        bad = 1;
    }
    if (!strstr(align_raw, "DDK59\r\nFREQ"))
    {
        printf("FAIL  the decoded take no longer reads DDK59, a line break, then FREQUENCIES: the\n"
               "      insertion or the break has moved\n");
        bad = 1;
    }
    if (!bad)
    {
        printf("\nOne character inserted at the junction and no carried character read wrong: the\n"
               "take prints DDK59, breaks the line, then FREQUENCIES, and the nine is the character\n"
               "the pinned text carries. The inserted slot itself is the decoder's reading ('5');\n"
               "the audio there is a second nine, one data bit away. The scorer charges the insertion\n"
               "and the pinned space the break stands in for - %d edits over %zu characters, %.1f %% -\n"
               "which is what 2.7 %% is made of. Both windows carry their own keying at the take's own\n"
               "level, so a break that became a fade would fail here.\n",
               align_ins + align_sub + align_del, align_rows,
               100.0 * (double)(align_ins + align_sub + align_del) / (double)align_rows);
    }
    return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  the decoder's own bit decisions
 * ------------------------------------------------------------------ */
// A character is five data bits and a report only carries the letter they add up to, so a wrong
// letter never says which bit was wrong, and a character dropped at its stop bit never appears at
// all. These are the decisions the demodulator took, recorded inside its loop by rtty.c: the bit, the
// slot it was stored in, and the sample it was read on. The sent code comes from the firmware's own
// Ascii2Baudot, so the comparison is against what the transmitter keys and not against a copy.
static int rtty_trace(const char* path, const char* expected, uint16_t shift, uint16_t baud)
{
    if (!expected || !*expected)
    {
        printf("a bit trace needs the text the take carries\n");
        return 2;
    }
    char got[8192], raw[8192];
    Bench_CharCount = 0;
    if (decode_wav(path, expected, got, sizeof(got), shift, baud, true, raw, sizeof(raw)) < 0.0f)
        return 1;
    if (!raw[0])
    {
        printf("the decoder printed nothing to trace\n");
        return 1;
    }

    // Raw on both sides, so a punctuation mark keeps its place. The rows below are the characters the
    // decoder printed, in order; indexing the expected text through the normalised copy would compare
    // every row after the first stripped stop against the wrong character, which is exactly what this
    // column did before the trace was pointed at this take.
    const char* exp = expected;
    // Line breaks are not data. The take breaks a line where the pinned text has a space, and
    // aligning CR or LF against that space would report a misread of a character the receiver never
    // had to read: a break is formatting. The comparison therefore runs on the printable characters,
    // while the rows still show each break where it happened.
    static char printable[8192];
    size_t npr = 0;
    for (const char* p = raw; *p && npr + 1 < sizeof(printable); p++)
        if (*p != '\r' && *p != '\n') printable[npr++] = *p;
    printable[npr] = '\0';
    const char* dec = printable;
    const size_t ne = strlen(exp), nd = strlen(dec);
    const double bit_ms = 1000.0 / (double)baud;

    int* match = malloc((ne + 1) * sizeof(int));
    int* from = malloc((nd + 1) * sizeof(int));
    if (!match || !from || align_chars(exp, dec, match) != 0)
    {
        free(match); free(from);
        printf("out of memory aligning the decode\n");
        return 1;
    }
    for (size_t j = 0; j <= nd; j++) from[j] = -1;
    for (size_t i = 0; i < ne; i++) if (match[i] >= 0) from[match[i]] = (int)i;

    // The decisions group themselves: every character starts a run at slot 1 and ends it at slot 7,
    // so a run that stops early is a character the receiver abandoned at a stop bit. Each complete
    // run is one character the decoder printed, which is what ties a row to its text.
    printf("\n%-4s %-9s %-10s %-8s %-5s %8s %7s\n", "#", "at ms", "sent->read", "data bits",
           "stop", "spacing", "dev ms");
    // The transmitter opens every transmission with a letters shift, which the decoder consumes as a
    // control and does not print. The groups that produced a printed character are therefore not the
    // groups that were read, and the rows would all be shifted by one without this counter.
    int groups = 0, printed = 0, abandoned = 0, diff_chars = 0, off_clock = 0;
    int breaks = 0, inserted = 0;
    for (int k = 0; k < Rtty_BenchTraceCount; )
    {
        if (Rtty_BenchTraceBits[k].slot != 1) { k++; continue; }
        // A character is not always the same number of decisions. With the 1.5 stop bit setting the
        // receiver jumps its slot counter from the first stop bit straight to the last, so it takes
        // six decisions (five data and one stop) and never reads a seventh; with two stop bits it
        // takes seven. The run of increasing slot numbers is what the decoder itself did, so that is
        // what the group is.
        int len = 1;
        while (len < 8 && k + len < Rtty_BenchTraceCount
               && Rtty_BenchTraceBits[k + len].slot == (uint8_t)(len + 1)) len++;
        if (len < 6) { abandoned++; k++; continue; }
        const rtty_bench_bit_t* g = &Rtty_BenchTraceBits[k];
        unsigned code = 0;
        int diff = 0;
        char bits[8];
        for (int s = 0; s < 5; s++) code |= (unsigned)(g[s].bit & 1) << s;
        // The spacing of the decisions: one bit apart is a DPLL in step, and anything else is a
        // relock at a start bit, which is what a phase slip looks like from outside the decoder.
        double sp_mean = 0.0, dev_max = 0.0;
        for (int s = 1; s < len; s++)
        {
            const double d = (double)(g[s].at - g[s - 1].at) / 12.0;
            sp_mean += d / (double)(len - 1);
            if (fabs(d - bit_ms) > dev_max) dev_max = fabs(d - bit_ms);
        }
        if (dev_max > 0.5) off_clock++;
        if (code == 8u || code == 2u)
        {
            // CR and LF carry no character: a station that breaks a line where the pinned text has
            // a space has read nothing wrong. The slot is shown, and not compared with the text.
            for (int s = 0; s < 5; s++) bits[s] = (g[s].bit & 1) ? '1' : '0';
            bits[5] = 0;
            printf("%-4d %-9.1f %-10s %-8s %c%c    %8.3f %7.3f  line break\n", groups + 1,
                   (double)g[0].at / 12.0, code == 8u ? "break CR" : "break LF", bits,
                   (g[5].bit & 1) ? '1' : '0',
                   len > 6 ? ((g[6].bit & 1) ? '1' : ' ') : ' ', sp_mean, dev_max);
            breaks++;
            groups++;
            k += len;
            continue;
        }
        if (code == 27u || code == 31u)
        {
            // a shift is a character slot but not a character: the decoder consumes it and prints
            // nothing, so it gets no row and does not consume an expected one
            groups++;
            k += len;
            continue;
        }
        const int ci = printed;
        const int ei = (size_t)ci < nd ? from[ci] : -1;
        const unsigned sent = ei >= 0 ? Rtty_BenchTxCodeForAscii(exp[ei]) : 0;
        if (ei < 0) inserted++;
        if (ei >= 0)
            for (int s = 0; s < 5; s++) if (((sent >> s) & 1) != (g[s].bit & 1)) diff |= 1 << s;
        for (int s = 0; s < 5; s++)
            bits[s] = (diff >> s) & 1 ? '*' : ((g[s].bit & 1) ? '1' : '0');
        bits[5] = 0;
        if (diff) diff_chars++;
        char sent_read[24];
        if (ei >= 0) snprintf(sent_read, sizeof(sent_read), "%u->%u", sent, code);
        else snprintf(sent_read, sizeof(sent_read), "?->%u", code);
        printf("%-4d %-9.1f %-10s %-8s %c%c    %8.3f %7.3f%s\n", groups + 1,
               (double)g[0].at / 12.0, sent_read, bits, (g[5].bit & 1) ? '1' : '0',
               len > 6 ? ((g[6].bit & 1) ? '1' : ' ') : ' ', sp_mean, dev_max, diff ? "  *" : "");
        printed++;
        groups++;
        k += len;
    }
    printf("\n%d character slots read, %d shift codes, %d line breaks, %d abandoned at a stop bit,\n"
           "%d inserted with no counterpart in the text, %d compared characters with a data bit that\n"
           "differs from what was sent\n", groups, groups - printed - breaks, breaks, abandoned, inserted,
           diff_chars);
    if (off_clock)
        printf("%d of them had a decision spacing more than 0.5 ms off the bit: the DPLL relocked "
               "inside\nthe character, so its clock and the audio's disagreed there\n", off_clock);
    else
        printf("every character was read with its decisions one bit apart (within 0.5 ms), so the\n"
               "decoder's clock was in step: a * in the data bits is the slicer, not the clock\n");
    free(match); free(from);
    return 0;
}

/* ------------------------------------------------------------------ *
 *  a second path
 * ------------------------------------------------------------------ */
// The same transmission arriving twice, one path later than the other. Nothing else in this bench
// produces that channel: fading moves the level of one path, noise adds a signal that does not carry
// the same bits, and a tuning error moves both tones together. Two paths put the same bit at two
// places in the audio, and whether the shapes still add up to a keying edge where the receiver
// samples is exactly what a delay spread asks.
static int rtty_multipath(void)
{
    const char* msg = RTTY_TABLE_MSG;
    static const float delays[] = { 0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 7.0f, 10.0f, 13.0f, 16.0f, 20.0f };
    static const float levels[] = { -3.0f, -6.0f, -10.0f };
    const size_t n_delay = sizeof(delays) / sizeof(delays[0]);
    const size_t n_level = sizeof(levels) / sizeof(levels[0]);
    char got[8192];
    int failures = 0;

    // Measured with a live noise generator: no cell costs the text. The only non-zero cell is 7 ms
    // at -3 dB (4.2 %, 2/48); 7 ms at -6 dB reads 0.0 %, and with the noise removed entirely both
    // cells read 0.0 % too. The 100 % once recorded at 7 ms and -6 dB was not the echo: restoring the
    // old generator, where every sample received the same Gaussian value, reproduces 20.8 % at -3 dB
    // and 100 % at -6 dB. That is a constant offset, which biases the receiver's level rather than
    // dithering it, and it is why the cell disappeared when the generator started advancing.
    // The pin is therefore on the channel: a cell above 5 % is a notch somebody introduced, not the
    // echo this sweep is supposed to map.
    const float max_cer = 5.0f;

    printf("RTTY with a second path: the same signal echo_ms later and echo_db down, summed onto the\n"
           "direct one. 45.45 Bd, 170 Hz shift, 20 dB SNR; a bit is 22.0 ms.\n\n");
    printf("%9s", "delay ms");
    for (size_t l = 0; l < n_level; l++) printf(" %9.0f dB", (double)levels[l]);
    printf("\n");
    for (size_t d = 0; d < n_delay; d++)
    {
        printf("%9.1f", (double)delays[d]);
        for (size_t l = 0; l < n_level; l++)
        {
            rtty_case_t c = { .label = "echo", .shift = 170, .snr_db = 20.0f, .max_cer = 100.0f };
            c.echo_ms = delays[d];
            c.echo_db = levels[l];
            int eot = 0;
            const float cer = rtty_run(&c, msg, false, got, sizeof(got), &eot);
            printf(" %10.1f", (double)cer);
            if (cer > max_cer)
            {
                printf("\nFAIL  a path %.1f ms late at %.0f dB costs %.1f %% of the text; this "
                       "channel's\n      record is that the echo moves the error rate but no cell "
                       "costs the message\n",
                       (double)delays[d], (double)levels[l], (double)cer);
                failures++;
            }
            if (eot != 1)
            {
                printf("\nFAIL  the transmission was cut off before EOT\n");
                failures++;
            }
        }
        printf("\n");
    }
    printf("\nThe columns are the echo level, the rows its delay; 0.0 ms is the direct path alone.\n"
           "No cell of this sweep costs the text: the only one above zero is 7 ms at -3 dB (4.2 %%, two\n"
           "characters of forty eight), and 7 ms at -6 dB reads 0.0 %%. The 100 %% this table once\n"
           "carried there was the old generator's constant, not the echo: with no noise at all that\n"
           "cell also reads 0.0 %%, while restoring the stuck generator brings the 100 %% back.\n");

    // The sweep is where an echo moves characters without taking the message; it is not the whole
    // channel. A path 18 ms late - four fifths of a bit - arrives while the receiver is deciding the
    // next bit, and at -3 dB it costs the message. That is the echo failure this table exists to
    // exercise, pinned from below so a decoder that became immune to it would fail here.
    {
        rtty_case_t c = { .label = "echo 18 ms", .shift = 170, .snr_db = 20.0f, .max_cer = 100.0f };
        c.echo_ms = 18.0f;
        c.echo_db = -3.0f;
        int eot = 0;
        const float cer = rtty_run(&c, msg, false, got, sizeof(got), &eot);
        printf("\na path 18 ms late at -3 dB: %.1f %% of the text\n", (double)cer);
        if (cer < 40.0f)
        {
            printf("FAIL  the echo 18 ms late no longer costs the message (%.1f %%)\n", (double)cer);
            failures++;
        }
        if (eot != 1)
        {
            printf("FAIL  the transmission was cut off before EOT\n");
            failures++;
        }
    }
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  the transmitter's own output through the gate
 * ------------------------------------------------------------------ */
// Run the modulator over the message and hand back its 12 kHz output: the signal every case starts
// from, with no channel between it and the measurements.
static size_t tx_capture(const char* msg, uint16_t shift, uint16_t baud, float32_t* buf, size_t cap)
{
    rtty_ctrl_config.speed_idx = baud == 50 ? RTTY_SPEED_50 : RTTY_SPEED_45;
    for (int i = 0; i < RTTY_SHIFT_NUM; i++)
        if (rtty_shifts[i].value == (float32_t)shift) rtty_ctrl_config.shift_idx = (rtty_shift_t)i;

    Rtty_BenchResetDecoder();
    Rtty_Modem_Init((uint32_t)TXRATE);
    Rtty_BenchTxBitScale(1.0f);
    char feed[640];
    snprintf(feed, sizeof(feed), "%s", msg);
    strncat(feed, "\004", sizeof(feed) - strlen(feed) - 1);
    Bench_TxFeed(feed);
    Bench_TxOffClear();
    Rtty_Modulator_StartTX();

    size_t used = 0;
    while (used < cap)
    {
        float acc = 0.0f;
        for (int k = 0; k < 4; k++) acc += (float32_t)Rtty_Modulator_GenSample() / 32768.0f;
        buf[used++] = acc * 0.25f;
        if (Bench_TxOffRequested()) break;
    }
    // the trailing tone the last character needs to finish on the air
    for (size_t tail = 0; tail < 2400 && used < cap; tail++)
    {
        float acc = 0.0f;
        for (int k = 0; k < 4; k++) acc += (float32_t)Rtty_Modulator_GenSample() / 32768.0f;
        buf[used++] = acc * 0.25f;
    }
    return used;
}

// Every case here keys the modem and then grades the demodulator on the result, so a keyer that sent
// the wrong bit length, lost a stop bit or sent a tone the receiver does not filter would be scored
// as though the receiver had misread it. This runs the modulator's own output through the same gate
// the receiver's input has to clear - pair, bimodality, bit clock - and adds the one thing only a
// transmitter has: every interval between tone changes has to be a whole number of half bits,
// because a character is one start bit, five data and 1.5 stop. A keyer that lost or added a fraction
// of a bit moves those intervals off that lattice and nothing else in the bench would say so.
static int tx_check(void)
{
    static const struct { uint16_t shift, baud; } cfg[] = { { 170, 45 }, { 450, 50 } };
    const size_t n_cfg = sizeof(cfg) / sizeof(cfg[0]);
    const char* msg = RTTY_TABLE_MSG;
    static float32_t buf[(size_t)MAX_SAMP];
    int failures = 0;

    printf("The transmitter's own 12 kHz output through the receiver's gate. RTTY is the only mode\n"
           "in this bench with a firmware transmitter (the fax path has none, PSK is not benched), so\n"
           "these are the modes a TX gate can cover: 170 Hz / 45.45 Bd and 450 Hz / 50 Bd. The 85 Hz\n"
           "shift is not gated this way because the gate's pair search needs the tones 150 Hz apart.\n\n");
    printf("%4s %4s %6s %6s %7s %9s %7s %8s %8s\n", "shift", "baud", "mark", "space",
           "period", "mad % bit", "both", "worst ms", "2 bit");
    for (size_t i = 0; i < n_cfg; i++)
    {
        const uint16_t shift = cfg[i].shift, baud = cfg[i].baud;
        const double bit_ms = 1000.0 / (double)baud;
        const size_t n = tx_capture(msg, shift, baud, buf, (size_t)MAX_SAMP);
        cap_t c = { buf, n, 12000, 1 };
        double clo = 0.0, chi = 0.0;
        if (!gate_coarse_pair(&c, &clo, &chi))
        {
            printf("FAIL  %u Hz / %u Bd: the transmitter's output has no pair to measure\n",
                   (unsigned)shift, (unsigned)baud);
            failures++;
            continue;
        }
        gate_pair_t pair;
        gate_pair_stats(&c, clo, chi, &pair);
        gate_clock_t clock;
        gate_clock(&c, pair.lo_hz, pair.hi_hz, (double)baud, &clock);
        double* t = NULL;
        const size_t nt = gate_crossings(&c, pair.lo_hz, pair.hi_hz, &t);
        // The keyer's own rule, from Rtty_BaudotAdd(): one stop bit, or two for both 1.5 and 2, so
        // every interval between tone changes is a whole number of bits. Measuring against the whole
        // bit rather than the half is what makes the 1.5 stop bit setting visible: it is not 1.5 on
        // the air, it is 2, and the gate says so instead of passing either as close enough.
        int short_runs = 0, off_lattice = 0, stops = 0, half_stops = 0;
        double worst = 0.0;
        for (size_t q = 1; q < nt; q++)
        {
            const double ms = (t[q] - t[q - 1]) * 1000.0;
            const double bits = ms / bit_ms;
            const double err = fabs(bits - floor(bits + 0.5)) * bit_ms;
            if (ms < 0.6 * bit_ms) short_runs++;
            if (err > 0.12 * bit_ms) off_lattice++;
            if (err > worst) worst = err;
            if (fabs(bits - 2.0) < 0.12) stops++;
            if (fabs(bits - 1.5) < 0.12) half_stops++;
        }
        free(t);

        printf("%4u %4u %6.1f %6.1f %7.3f %9.2f %7.2f %8.2f %8d%s\n", (unsigned)shift,
               (unsigned)baud, pair.lo_hz, pair.hi_hz, clock.period_ms, clock.mad_pct,
               pair.f_both, worst, stops, "");

        const double pair_pct = fabs(pair.lo_hz - bench_mark_hz()) + fabs(pair.hi_hz - bench_space_hz(shift));
        if (pair_pct > 6.0)
        {
            printf("FAIL  %u/%u: the keyed pair is %.1f / %.1f Hz, not the %.0f / %.0f Hz it is sent\n",
                   (unsigned)shift, (unsigned)baud, pair.lo_hz, pair.hi_hz,
                   (double) bench_mark_hz(), (double) bench_space_hz(shift));
            failures++;
        }
        if (pair.f_both > 0.25 || pair.f_low + pair.f_high < 0.70)
        {
            printf("FAIL  %u/%u: %.2f of the keyed audio holds one tone and %.2f both; a keyed signal\n"
                   "      that is half tone in a quarter of its windows is not being keyed cleanly\n",
                   (unsigned)shift, (unsigned)baud, pair.f_low + pair.f_high, pair.f_both);
            failures++;
        }
        if (fabs(clock.period_ms - bit_ms) > 0.01 * bit_ms)
        {
            printf("FAIL  %u/%u: the keyed bit is %.3f ms, not the %.3f ms of %u Bd\n",
                   (unsigned)shift, (unsigned)baud, clock.period_ms, bit_ms, (unsigned)baud);
            failures++;
        }
        if (clock.mad_pct > 3.0)
        {
            printf("FAIL  %u/%u: the keyed transitions scatter %.2f %% of a bit (MAD); the transmitter\n"
                   "      is the one clock here that should be exact\n",
                   (unsigned)shift, (unsigned)baud, clock.mad_pct);
            failures++;
        }
        if (short_runs || off_lattice)
        {
            printf("FAIL  %u/%u: %d interval(s) shorter than a whole bit and %d off the whole bit "
                   "lattice\n", (unsigned)shift, (unsigned)baud, short_runs, off_lattice);
            failures++;
        }
        if (stops < 3)
        {
            printf("FAIL  %u/%u: only %d interval(s) measure 2 bits; the stop bits the modulator adds\n"
                   "      are not on the air as whole bit runs\n",
                   (unsigned)shift, (unsigned)baud, stops);
            failures++;
        }
        if (half_stops)
        {
            printf("FAIL  %u/%u: %d interval(s) measure 1.5 bits; the firmware sends two, so a\n"
                   "      keyer that started sending the half bit has to be a change someone made\n",
                   (unsigned)shift, (unsigned)baud, half_stops);
            failures++;
        }
    }
    printf("\n'2 bit' counts the intervals that are exactly two bits: the modulator's stop bits. The\n"
           "radio's stop bit setting is 1.5, and Rtty_BaudotAdd() sends two for both 1.5 and 2, so the\n"
           "air carries whole bit runs and this gate is the measurement that says so. 'worst ms' is the\n"
           "largest distance of any interval from the nearest whole bit; it is not zero because a\n"
           "transition time comes from the gate's 5 ms windows, which is about 2 ms at these bit\n"
           "lengths.\n");
    if (!failures) printf("\nthe transmitter's keying clears the same gate the receiver's audio has to\n");
    return failures ? 1 : 0;
}

/* ------------------------------------------------------------------ *
 *  self checks
 * ------------------------------------------------------------------ */
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

// A take built here rather than read off disk: these cases pin the estimator, and a recording on disk
// is a file that a later trim, re-encode or decimation can change under them. `slots` holds the tone
// of each slot (0 = low tone, 1 = high tone) and `slot_ms` its length, so a slot of half a bit gives
// the 1.5 stop bits of a Baudot character and a slot of a whole bit gives plain keying. The phase runs
// on across the steps, as a real FSK signal's does; restarting it at every step would put a click in
// each transition and widen the crossing the gate is asked to time.
static int synth_take(cap_t* c, const uint8_t* slots, size_t nslots, double slot_ms, double lo, double hi)
{
    c->rate = 12000;
    c->n = (size_t)llround((double)nslots * slot_ms * 0.001 * (double)c->rate);
    c->x = malloc((c->n ? c->n : 1) * sizeof(float));
    if (!c->x) return 0;
    double phase = 0.0;
    size_t s = 0;
    for (size_t k = 0; k < nslots && s < c->n; k++)
    {
        const double f = slots[k] ? hi : lo;
        const size_t end = (size_t)llround((double)(k + 1) * slot_ms * 0.001 * (double)c->rate);
        for (; s < end && s < c->n; s++)
        {
            c->x[s] = (float)(0.4 * sin(phase));
            phase += 2.0 * M_PI * f / (double)c->rate;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
    for (; s < c->n; s++) c->x[s] = 0.0f;
    return 1;
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
        // The noise and fading channels are the only part of a case the modem does not generate,
        // and a generator whose state stops advancing is invisible in a character error rate: every
        // sample receives the same value, so "noise" becomes a constant offset and the row reads
        // cleaner than it should. That is how three tables here were measured before the state was
        // written back, so check the channels directly rather than reading their effect in a CER.
        enum { CH_N = 12000 };
        static float32_t ch[CH_N];
        for (int i = 0; i < CH_N; i++) ch[i] = 0.0f;
        Bench_RngSeed(0x5EED1234u);
        Bench_AddNoise(ch, CH_N, 10.0f, 1.0f);
        double sum = 0.0, sq = 0.0, d2 = 0.0;
        int same = 0;
        for (int i = 0; i < CH_N; i++) { sum += ch[i]; sq += (double) ch[i] * ch[i]; }
        for (int i = 1; i < CH_N; i++)
        {
            const double d = (double) ch[i] - ch[i - 1];
            d2 += d * d;
            if (ch[i] == ch[i - 1]) same++;
        }
        const double n_mean = sum / CH_N;
        const double n_rms = sqrt(sq / CH_N);
        const double n_step = sqrt(d2 / (CH_N - 1));
        printf("     (noise: mean %+.4f, rms %.3f, %d/%d adjacent samples equal, step rms %.3f)\n",
               n_mean, n_rms, same, CH_N - 1, n_step);
        check(fabs(n_mean) < 0.01, "noise: the added signal has no DC");
        check(n_rms > 0.25 && n_rms < 0.40, "noise: the added signal has the RMS the SNR asks for");
        check(same < CH_N / 100 && n_step > 0.25,
              "noise: the generator advances sample to sample");

        const double depth = pow(10.0, -10.0 / 20.0);
        for (int i = 0; i < CH_N; i++) ch[i] = 1.0f;
        Bench_AddQsb(ch, CH_N, 2.0f, 10.0f);
        double qsum = 0.0, q2 = 0.0;
        float lo = ch[0], hi = ch[0];
        int qsame = 0;
        for (int i = 0; i < CH_N; i++)
        {
            qsum += ch[i];
            if (ch[i] < lo) lo = ch[i];
            if (ch[i] > hi) hi = ch[i];
            if (i)
            {
                const double d = (double) ch[i] - ch[i - 1];
                q2 += d * d;
                if (ch[i] == ch[i - 1]) qsame++;
            }
        }
        const double q_mean = qsum / CH_N;
        const double q_step = sqrt(q2 / (CH_N - 1));
        printf("     (qsb: %.3f..%.3f, mean %.3f, %d/%d adjacent samples equal, step rms %.2e)\n",
               lo, hi, q_mean, qsame, CH_N - 1, q_step);
        check(hi > 0.99f && lo > (float) depth - 0.01f && hi - lo > 0.6f,
              "qsb: the fade reaches its trough and peak");
        check(fabs(q_mean - 0.5 * (1.0 + depth)) < 0.01,
              "qsb: the envelope's mean is the depth it asks for, not a constant gain");
        check(qsame < CH_N / 100 && q_step > 1e-5,
              "qsb: the fading phase advances sample to sample");
        for (int i = 0; i < CH_N; i++) ch[i] = 0.0f;
        Bench_AddQsb(ch, CH_N, 2.0f, 10.0f);
        int nonzero = 0;
        for (int i = 0; i < CH_N; i++) if (ch[i] != 0.0f) nonzero++;
        check(nonzero == 0, "qsb: silence stays silent, so fading adds no DC");
    }
    {
        // The check above measures the generator. This one measures the take: run the same case
        // twice, once without noise and once at 10 dB, and treat the difference as the noise the
        // receiver is actually handed. A random number generator that stopped advancing leaves a
        // constant residue; a take that lost its noise leaves none at all.
        rtty_case_t clean = { .label = "take clean", .shift = 170, .snr_db = 100.0f, .max_cer = 5.0f };
        rtty_case_t noisy = { .label = "take noisy", .shift = 170, .snr_db = 10.0f, .max_cer = 5.0f };
        static float32_t take_clean[160000], take_noisy[160000], residual[160000];
        char got[8192];
        int eot = 0;
        take_copy = take_clean; take_copy_cap = 160000; take_copy_n = 0;
        rtty_run(&clean, "DE UHSDR NOISE TAKE", false, got, sizeof(got), &eot);
        take_copy = take_noisy;
        rtty_run(&noisy, "DE UHSDR NOISE TAKE", false, got, sizeof(got), &eot);
        take_copy = NULL; take_copy_cap = 0;
        const size_t n = take_copy_n;
        double sum = 0.0, sq = 0.0, d2 = 0.0, sig = 0.0;
        int same = 0;
        for (size_t i = 0; i < n; i++)
        {
            residual[i] = take_noisy[i] - take_clean[i];
            sum += residual[i];
            sq += (double) residual[i] * residual[i];
            sig += (double) take_clean[i] * take_clean[i];
            if (i)
            {
                const double step = (double) residual[i] - residual[i - 1];
                d2 += step * step;
                if (residual[i] == residual[i - 1]) same++;
            }
        }
        const double mean = n ? sum / (double) n : 0.0;
        const double rms = n ? sqrt(sq / (double) n) : 0.0;
        const double step = n > 1 ? sqrt(d2 / (double) (n - 1)) : 0.0;
        const double want = sqrt(sig / (double) (n ? n : 1)) * pow(10.0, -10.0 / 20.0);
        printf("     (noise take: mean %+.4f, rms %.4f (asked %.4f), %d/%zu adjacent equal, step rms %.4f)\n",
               mean, rms, want, same, n ? n - 1 : 0, step);
        check(fabs(mean) < 0.005, "noise take: the noise on the take has no DC");
        check(fabs(rms - want) < 0.2 * want,
              "noise take: the noise on the take has the level the SNR asks for");
        check(same < (int) (n / 100) && step > 0.001,
              "noise take: the noise changes sample to sample");
    }
    {
        // zero crossing count of the modulator tone, measured through the same path the bench uses
        rtty_case_t c = { .label = "probe", .shift = 170, .snr_db = 100.0f, .max_cer = 5.0f };
        int eot = 0;
        char got[8192];
        Rtty_BenchTraceStart();
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST ONE", false, got, sizeof(got), &eot);
        check(cer == 0.0f, "clean RTTY loopback decodes with zero character errors");
        printf("     (lead-in junk before lock: %d characters)\n", lead_in_junk);
        // the trace is what says which bit of a wrong character was wrong, so it has to see every
        // decision the decoder takes: seven per character, the five data slots and two stops
        int traced_groups = 0;
        for (int k = 0; k + 5 < Rtty_BenchTraceCount; k++)
        {
            if (Rtty_BenchTraceBits[k].slot != 1) continue;
            int len = 1;
            while (len < 8 && k + len < Rtty_BenchTraceCount
                   && Rtty_BenchTraceBits[k + len].slot == (uint8_t)(len + 1)) len++;
            if (len >= 6) traced_groups++;
        }
        printf("     (bit trace: %d decisions, %d complete character groups)\n",
               Rtty_BenchTraceCount, traced_groups);
        check(traced_groups > 5,
              "the bit trace records the decisions of every character the decoder assembles");
        check(strstr(got, "UHSDR") != NULL, "the call sign survives the loopback");
        check(eot == 1, "an EOT in the tx buffer asks the radio manager to leave TX");
    }
    {
        rtty_case_t c = { .label = "snr", .shift = 170, .snr_db = 15.0f, .max_cer = 5.0f };
        int eot = 0;
        char got[8192];
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST TWO", false, got, sizeof(got), &eot);
        check(cer <= 5.0f, "RTTY loopback stays under 5% CER at 15 dB SNR");
    }
    {
        wefax_cfg_t cfg = { .name = "selftest", .snr_db = 20.0f, .rows = 12 };
        size_t samples = (size_t) cfg.rows * WEFAX_SAMPLES_PER_LINE;
        float32_t* x = malloc(samples * sizeof(float32_t));
        uint8_t* truth = malloc((size_t) cfg.rows * WEFAX_IMG_COLS);
        uint8_t* grey = malloc((size_t) cfg.rows * WEFAX_IMG_COLS);
        wefax_result_t res;
        memset(grey, 0, (size_t) cfg.rows * WEFAX_IMG_COLS);   // a lost line stays a hole, not a shift
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_RngSeed(1u);
        Bench_AddNoise(x, samples, cfg.snr_db, 0.0f);
        bool any = Bench_WefaxDecode(x, samples, grey, cfg.rows, &res);
        float corr = pearson(truth, grey, (size_t) cfg.rows * WEFAX_IMG_COLS);
        check(any && res.lines_found == cfg.rows, "fax: every sync tip of a 20 dB broadcast is found");
        check(corr > 0.9f, "fax: grey scale of a 20 dB broadcast correlates above 0.90");

        cfg.snr_db = 8.0f; cfg.qsb_rate_hz = 0.4f; cfg.qsb_depth_db = 6.0f;
        memset(grey, 0, (size_t) cfg.rows * WEFAX_IMG_COLS);   // a lost line stays a hole, not a shift
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_RngSeed(2u);
        Bench_AddNoise(x, samples, cfg.snr_db, 0.0f);
        Bench_WefaxDecode(x, samples, grey, cfg.rows, &res);
        corr = pearson(truth, grey, (size_t) cfg.rows * WEFAX_IMG_COLS);
        check(res.lines_found >= cfg.rows * 3 / 4, "fax: three quarters of the lines survive 8 dB with fading");

        // the skew knob must actually move the transmitter, otherwise the line capture is not being
        // tested at all and the table would read as if it were
        // Known gap, recorded rather than hidden. The transmitter skews every line by up to
        // +-120 samples (6 pixel columns) and the capture does not follow each tip, so lines are
        // lost and the chart with them. These numbers are pinned; a capture that started tracking
        // would change them and this check would have to be updated, which is the point.
        cfg.snr_db = 20.0f; cfg.qsb_rate_hz = 0.0f; cfg.qsb_depth_db = 0.0f; cfg.skew_ppm = 20000.0f;
        memset(grey, 0, (size_t) cfg.rows * WEFAX_IMG_COLS);   // a lost line stays a hole, not a shift
        Bench_WefaxGenerate(x, truth, &cfg);
        Bench_WefaxDecode(x, samples, grey, cfg.rows, &res);
        float skew_corr = pearson(truth, grey, (size_t) cfg.rows * WEFAX_IMG_COLS);
        printf("     skewed transmitter: %u/%u lines, spacing metric %.1f samples, chart correlation %.3f\n",
               res.lines_found, cfg.rows, res.row_rms_error, skew_corr);
        // 9 of 12 and 0.272 measured: the capture re-acquires per line and has no rate model, so a
        // miss offsets every later window.  Rows it loses are holes now, not a slide of the picture.
        check(res.lines_found >= cfg.rows * 3 / 4 && skew_corr < 0.9f,
              "fax: per-line skew is not tracked, and the table pins how much it costs");
        {
            // The verdict against traffic rather than models: a real RTTY recording must not look like
            // fax to the acquisition metric, and the generated broadcast must.  Both numbers come from
            // files on disk, and the RTTY one is an on-air recording, so this is the first check in the
            // WEFAX section that touches propagation.
            uint32_t lines = 0; float ratio = 0.0f;
            const float rtty_rms = wefax_acquisition("fixtures/ddk2-4583.wav", &lines, &ratio);
            // Its own positive control, written here rather than borrowed from whichever target ran
            // last: wefax_snr20.wav only exists if make wefax has been run, and a check that reads a
            // stale file from a previous run is a check that can pass by accident.
            wefax_cfg_t plain = { .name = "acquisition", .snr_db = 20.0f, .rows = 12 };
            const size_t plain_n = (size_t) plain.rows * WEFAX_SAMPLES_PER_LINE;
            float32_t* px = malloc(plain_n * sizeof(float32_t));
            float fax_rms = -1.0f;
            if (px)
            {
                Bench_WefaxGenerate(px, NULL, &plain);
                if (Bench_WavWrite("/tmp/fax-acq.wav", px, plain_n, 12000))
                {
                    fax_rms = wefax_acquisition("/tmp/fax-acq.wav", &lines, &ratio);
                }
                free(px);
            }
            printf("     acquisition: real RTTY take spacing rms %.1f samples, generated chart %.1f\n",
                   (double) rtty_rms, (double) fax_rms);
            check(fax_rms >= 0.0f && fax_rms < 20.0f, "fax: a broadcast the harness made is accepted as fax by its tip spacing");
            check(rtty_rms > 100.0f, "fax: a real RTTY recording is rejected as fax by the same spacing metric that accepts it");
        }
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
        Rtty_BenchTxBitScale(1.0f);   // this recording is the modem's own clock, not a case's
        Rtty_Modulator_StartTX();
        for (size_t k = 0; k < n; k++)
        {
            float acc = 0.0f;
            for (int q = 0; q < 4; q++) acc += (float32_t)Rtty_Modulator_GenSample() / 32768.0f;
            rec[k] = acc * 0.25f;
        }
        check(Bench_WavWrite("rtty_roundtrip.wav", rec, n, 12000), "the bench can write a 12 kHz RTTY recording");
        float cer_file = decode_wav("rtty_roundtrip.wav", msg, got, sizeof(got), 170, 45, false, NULL, 0);
        check(cer_file == 0.0f, "a recorded WAV comes back through the file input with zero errors");
    }
    {
        // The score has to describe the message, not the bulletin around it. A live take opens inside
        // whatever the station was sending - the DWD stations open with long RYRY runs, and a receiver
        // switched on mid-bulletin lands in the middle of one - and the operator keeps recording for
        // a while after the message ends. A fixed skip cannot cover that, however many characters it
        // is widened to: the RYRY before the message is only free if it never has to be matched.
        const char* want = norm_baudot("DE UHSDR SCORE TEST", 0);
        char take[256];
        memset(take, 0, sizeof(take));
        for (int i = 0; i < 60; i++) take[i] = (i % 2) ? 'Y' : 'R';
        strcat(take, want);
        strcat(take, "RYRYRYRYRYRYRYRYRYRY");
        const float behind = score_aligned(want, take);
        check(behind == 0.0f && lead_in_junk == 60,
              "the message is found behind a long RYRY run and scored on its own characters");
        // and the freedom must not become a way to score noise: with nothing to find, a take of
        // junk still costs every character of the message
        const float junk = score_aligned(want, "QWEASDZXCVBNMQWEASDZXCVBNM");
        check(junk > 50.0f, "junk in the take is not scored as a message");
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
    // 5. the clock estimator, on takes made here rather than read from disk. The gate is what stands
    // between an on air recording and the fixture table, so its two sharpenings have to be held in
    // place by something that runs on every build: the squared phase fit that cancels the half bit a
    // Baudot character's stop leaves, and the rule that the reported period is the bit and not a
    // lattice the transitions alone would allow.
    {
        enum { NSLOT = 600 };
        static uint8_t slots[NSLOT];
        unsigned rng = 2463534242u;
        for (int i = 0; i < NSLOT; i++)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            slots[i] = (uint8_t)((rng >> 8) & 1);
        }
        cap_t c;
        gate_clock_t k;
        check(synth_take(&c, slots, NSLOT, 20.0, 775.0, 1225.0),
              "clock: a 50 baud take can be built in memory instead of read off disk");
        gate_clock(&c, 775.0, 1225.0, 50.0, &k);
        printf("     (clean 50 Bd take made here: %.3f ms, scatter %.1f %%, %d transitions)\n",
               k.period_ms, k.dispersion_pct, k.transitions);
        check(fabs(k.period_ms - 20.0) <= 20.0 * GATE_PERIOD_TOL,
              "clock: a clean 50 baud take fits 20 ms inside the period window");
        check(k.dispersion_pct < 1.0,
              "clock: and its transitions sit within 1 % of a bit of that grid");
        printf("     (robust scatter of that take: %.2f %% MAD against %.2f %% rms)\n",
               k.mad_pct, k.dispersion_pct);
        check(k.mad_pct < 1.0, "clock: the robust scatter of a clean take is under 1 % of a bit");
        free(c.x);

        // a Baudot character is 1 start bit, 5 data and 1.5 stop, so it is 15 half bits long and the
        // transitions at a character boundary sit half a bit off the ones inside it. Fitted on the
        // phase alone this take reads 23.440 ms, 42.7 baud and 6.7 % scatter: the two grids fight,
        // nothing wins cleanly, and the take looks like one recorded slightly fast. Fitted on the
        // squared phase it reads the 25 ms it is, with 0.3 %.
        enum { NCHAR = 70, NHALF = NCHAR * 15 };
        static uint8_t half[NHALF];
        for (int i = 0; i < NCHAR; i++)
        {
            uint8_t* p = half + i * 15;
            p[0] = p[1] = 0;                       // start: one space bit
            rng = 2463534242u ^ (unsigned)(i * 2654435761u);
            for (int b = 0; b < 5; b++)            // five data bits, two half slots each
            {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                p[2 + 2 * b] = p[3 + 2 * b] = (uint8_t)((rng >> 8) & 1);
            }
            p[12] = p[13] = p[14] = 1;             // stop: 1.5 mark bits
        }
        check(synth_take(&c, half, NHALF, 12.5, 775.0, 1225.0),
              "clock: a 40 baud take with 1.5 stop bits can be built in memory");
        gate_clock(&c, 775.0, 1225.0, 50.0, &k);
        printf("     (40 Bd take with 1.5 stop bits: %.3f ms = %.1f baud, scatter %.1f %%, "
               "%d transitions)\n", k.period_ms, 1000.0 / k.period_ms, k.dispersion_pct, k.transitions);
        check(fabs(1000.0 / k.period_ms - 40.0) <= 0.2,
              "clock: 1.5 stop bits do not pull the fitted period off the 25 ms bit (40 baud)");
        check(fabs(k.period_ms - 20.0) > 20.0 * GATE_PERIOD_TOL,
              "clock: and the take is refused against 50 baud on the period alone");
        free(c.x);

        // transitions every second bit: the bit tone still steps at 20 ms, but the tone only changes
        // every 40 ms, so 40 ms explains the transitions exactly as well as 20 does and 10 ms fits
        // their phases too. The reported period has to be the bit the modem will key at, which is
        // the fundamental of that grid, and not the coarser lattice the crossings alone allow.
        // The fit alone cannot choose between those three periods, so the search window is what
        // does, and it is asserted here as a rule before any take goes through it. For a declared
        // 50 Bd the window is 30 % either side of 20 ms, that is 14..26 ms, and it excludes both
        // 10 ms - the two bit harmonic, whose phases fit the same transitions - and 40 ms, the half
        // rate spacing that explains them. Both are outside the window, not merely less favoured
        // inside it, so there is no harmonic in the search for the fit to prefer. Nothing of that
        // kind should be added back: a preference inside the window would make the reported period
        // depend on which lattice scored highest on one take, not on the rate the modem keys. The
        // window is a fraction of the bit, so the same two bounds hold at every declared rate; the
        // case after them pins what the fit reports.
        const double bit_ms = 1000.0 / 50.0;
        check(bit_ms * GATE_CLOCK_LO > 0.5 * bit_ms,
              "clock: the search window starts above half a bit, where a two bit take's phases fit");
        check(bit_ms * GATE_CLOCK_HI < 2.0 * bit_ms,
              "clock: and ends below twice the bit, so a two bit take is never fitted to its 40 ms spacing");
        for (int i = 0; i < NSLOT; i++) slots[i] = (uint8_t)((i / 2) & 1);
        check(synth_take(&c, slots, NSLOT, 20.0, 775.0, 1225.0),
              "clock: a take that repeats on a two bit lattice can be built in memory");
        gate_clock(&c, 775.0, 1225.0, 50.0, &k);
        printf("     (two bit lattice at 20 ms a bit: %.3f ms = %.1f baud)\n",
               k.period_ms, 1000.0 / k.period_ms);
        check(fabs(k.period_ms - 20.0) <= 20.0 * 0.01,
              "clock: a two bit lattice is reported as the 20 ms bit, not its 40 ms spacing");
        free(c.x);
    }
    {
        // NAVTEX rests on two things and both are checked here: the constant ratio alphabet, which
        // is what lets the receiver find the character boundary at all, and the repeat copy, which
        // is the only correction a broadcast has. Erasing one copy of a block has to cost nothing.
        static const char* msg = "NAVTEX TEST DE UHSDR";
        const size_t cap = 200000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        char got[1024];
        navtex_result_t res;
        const size_t n = Bench_NavtexGenerate(x, cap, msg, 0.0f, NAVTEX_ORDER_TABLE);
        check(n > 0 && Bench_NavtexDecode(x, n, got, sizeof(got), &res),
              "navtex: a clean SITOR-B transmission decodes");
        printf("     (aligned on %.2f ms bits, %.3f of its words 4-of-7)\n",
               res.period_samples / 12.0, res.valid_fraction);
        check(strstr(got, "NAVTEX TEST DE UHSDR") != NULL,
              "navtex: and the text comes back as sent");
        // the message starts after twenty phasing characters, so the first block's direct copies are
        // the 350 ms from 1400 ms; the repeats are 350 ms later and are untouched
        for (size_t k = 1400 * 12; k < 1680 * 12 && k < n; k++) x[k] = 0.0f;
        navtex_result_t burst;
        const int ok = Bench_NavtexDecode(x, n, got, sizeof(got), &burst);
        printf("     (with one block's direct copy erased: ok %d, %u repaired, %u lost, %.3f 4-of-7, "
               "start %d) [%s]\n", ok, burst.repaired, burst.both_bad, burst.valid_fraction,
               burst.phasing_run, got);
        check(ok && strstr(got, "NAVTEX TEST DE UHSDR") != NULL && burst.repaired > 0,
              "navtex: the repeat copy carries a block whose direct copy was erased");

        // The other bit order, phasing pair included. Every reversed 4-of-7 word is a valid
        // character, so the ratio that finds the group boundary says nothing about the order: the
        // decode can only come right if the receiver read it off the phasing, which is what the
        // result flag has to show.
        const size_t nrev = Bench_NavtexGenerate(x, cap, msg, 0.0f, NAVTEX_ORDER_REVERSED);
        navtex_result_t rorder;
        const int rev_ok = nrev > 0 && Bench_NavtexDecode(x, nrev, got, sizeof(got), &rorder);
        printf("     (same text in the other bit order: ok %d, detected %d, phasing run %d, %.3f "
               "4-of-7) [%s]\n", rev_ok, rorder.reversed, rorder.phasing_run,
               rorder.valid_fraction, got);
        check(rev_ok && rorder.reversed == 1 && strstr(got, "NAVTEX TEST DE UHSDR") != NULL,
              "navtex: a take in the other bit order decodes, its order read from the phasing pair");

        // Phasing in the table's order and text in the other one: the receiver is told the wrong
        // order, so it has to read the text wrong rather than hedge. A receiver that quietly tried
        // both would decode this, and would then also be inventing characters out of noise.
        const size_t nmix = Bench_NavtexGenerate(x, cap, msg, 0.0f, NAVTEX_ORDER_MIXED);
        navtex_result_t rmix;
        const int mix_ok = nmix > 0 && Bench_NavtexDecode(x, nmix, got, sizeof(got), &rmix);
        printf("     (phasing in the table order, text reversed: ok %d, detected %d) [%s]\n",
               mix_ok, rmix.reversed, got);
        check(mix_ok && rmix.reversed == 0 && strstr(got, "NAVTEX TEST DE UHSDR") == NULL,
              "navtex: text in the other order than its phasing does not decode as the text sent");
        free(x);
    }
    {
        // Hellschreiber is scored as a picture, so the self check is about the picture: the raster has
        // to be found (every gap column blank) and carry as many pixels as were drawn, and a
        // transmitter whose pixel clock the receiver does not follow has to lose that raster rather
        // than drift quietly.
        static const char* msg = "HELL TEST";
        const size_t cap = 40000;
        static uint8_t truth[HELL_ROWS * 256], got[HELL_ROWS * 256];
        float32_t* x = malloc(cap * sizeof(float32_t));
        hell_result_t r;
        int tcols = 0, gcols = 0;
        const size_t n = Bench_HellGenerate(x, cap, msg, truth, 256, &tcols, 1000.0f, 0.0f);
        int t_on = 0;
        for (int row = 0; row < HELL_ROWS; row++)
            for (int col = 0; col < tcols; col++) t_on += truth[row * 256 + col] != 0;
        check(n > 0 && Bench_HellDecode(x, n, got, 256, &gcols, &r),
              "hell: a clean raster decodes");
        int g_on = 0;
        for (int row = 0; row < HELL_ROWS; row++)
            for (int col = 0; col < gcols; col++) g_on += got[row * 256 + col] != 0;
        printf("     (%d columns drawn, %d read, %d pixels drawn and %d read, gaps %.2f, pixel "
               "phase %d, column phase %d)\n", tcols, gcols, t_on, g_on, r.gap_share,
               r.pixel_phase, r.column_phase);
        check(t_on > 0 && g_on == t_on,
              "hell: every pixel that was keyed comes back, and no pixel that was not");
        check(r.gap_share == 1.0 && gcols >= tcols - 1,
              "hell: the raster is found with every gap column blank");
        free(x);

        x = malloc(cap * sizeof(float32_t));
        const size_t slow = Bench_HellGenerate(x, cap, msg, truth, 256, &tcols, 1000.0f, 2.0f);
        const int ok_slow = Bench_HellDecode(x, slow, got, 256, &gcols, &r);
        printf("     (same text with the pixel clock 2 %% off: ok %d, gaps %.2f)\n", ok_slow,
               r.gap_share);
        check(!ok_slow || r.gap_share < 0.5,
              "hell: a 2 % pixel clock error has to cost the raster, not just blur it");
        free(x);
    }
    {
        // MFSK16 rests on two things the self check can name: the tone decisions, which four bits a
        // symbol carries, and the code across symbols, which is what makes one lost symbol cheap.
        static const char* msg = "MFSK16 TEST";
        const size_t cap = 200000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        char got[256];
        mfsk16_result_t r;
        const size_t n = Bench_Mfsk16Generate(x, cap, msg, 0.0f);
        check(n > 0 && Bench_Mfsk16Decode(x, n, got, sizeof(got), &r),
              "mfsk16: a clean transmission decodes");
        printf("     (%u symbols, %u coded bits, %u characters, %.1f %% of symbols heard, text [%s])\n",
               r.symbols, r.bits, r.characters,
               r.symbols ? 100.0 * (double)r.tones_ok / r.symbols : 0.0, got);
        check(strcmp(got, msg) == 0, "mfsk16: and the text comes back as sent");
        memset(x + 30 * 768, 0, 768 * sizeof(float32_t));
        const int ok1 = Bench_Mfsk16Decode(x, n, got, sizeof(got), &r);
        printf("     (with symbol 30 erased: ok %d, %.1f %% heard, text [%s])\n", ok1,
               r.symbols ? 100.0 * (double)r.tones_ok / r.symbols : 0.0, got);
        check(ok1 && strcmp(got, msg) == 0,
              "mfsk16: one erased symbol - four bits, spread over eight - costs nothing");
        free(x);
    }
    {
        // Olivia: a character is a Walsh function spread over a block of sixty four symbols, so the
        // self check is that the text comes back and that one symbol lost inside a block does not
        // cost a character.
        static const char* msg = "OLIVIA TEST";
        const size_t cap = 100000;
        float32_t* x = malloc(cap * sizeof(float32_t));
        char got[256];
        olivia_result_t r;
        const size_t n = Bench_OliviaGenerate(x, cap, msg, 0.0f);
        check(n > 0 && Bench_OliviaDecode(x, n, got, sizeof(got), &r),
              "olivia: a clean transmission decodes");
        printf("     (%u symbols in %u blocks, header %d, %.1f %% of symbols heard, text [%s])\n",
               r.symbols, r.blocks, r.header, r.symbols ? 100.0 * (double)r.tones_ok / r.symbols : 0.0,
               got);
        check(strcmp(got, msg) == 0, "olivia: and the text comes back as sent");
        memset(x + 8 * 384, 0, 384 * sizeof(float32_t));
        const int ok1 = Bench_OliviaDecode(x, n, got, sizeof(got), &r);
        printf("     (with symbol 8 erased: ok %d, header %d, %u symbols, text [%s])\n", ok1,
               r.header, r.symbols, got);
        check(ok1 && strcmp(got, msg) == 0,
              "olivia: one symbol lost inside an interleaved block costs no character");
        free(x);
    }
    {
        // The generators cap a message at 300 characters, and a 300 character MFSK16 take is the
        // case that crosses the Viterbi step ceiling: a maximum length Varicode character is ten
        // bits plus its two bit delimiter, so 300 of them are 3600 information bits, not the 2048
        // the old path memory held. Only the control characters have a ten bit code - '~', which
        // this take used to send, is nine bits and 300 of them are 3312, short of the number the
        // check is about. Olivia's equivalent is 300 characters in 61 blocks, which is the other
        // end of the same buffer question. Both are checked by exact text, not by a count of
        // symbols, so a cap that silently truncates fails here.
        char msg[301];
        memset(msg, '~', 300);
        msg[300] = 0;
        char mfmsg[301];
        memset(mfmsg, '\x01', 300);              // ten bits per character: 3600 information bits
        mfmsg[300] = 0;
        const size_t cap = 2000000;         // 1.39 M MFSK16 samples, 1.50 M Olivia samples
        {
            float32_t* x = malloc(cap * sizeof(float32_t));
            char got[512];
            mfsk16_result_t r = {0};
            const size_t n = x ? Bench_Mfsk16Generate(x, cap, mfmsg, 0.0f) : 0;
            check(n > 1000000, "mfsk16: a 300 character take fits the generator cap and buffer");
            const int ok = n > 0 && Bench_Mfsk16Decode(x, n, got, sizeof(got), &r);
            printf("     (300 characters: %zu samples, %u symbols, %u coded bits, %u steps)\n",
                   n, r.symbols, r.bits, r.viterbi_bits);
            check(ok, "mfsk16: a 300 character take decodes");
            check(ok && r.viterbi_bits >= 3600,
                  "mfsk16: the long take needs the full 3600 step path memory");
            check(ok && strcmp(got, mfmsg) == 0, "mfsk16: the 300 characters come back as sent");
            free(x);
        }
        {
            float32_t* x = malloc(cap * sizeof(float32_t));
            char got[512];
            olivia_result_t r = {0};
            const size_t n = x ? Bench_OliviaGenerate(x, cap, msg, 0.0f) : 0;
            check(n > 1000000, "olivia: a 300 character take fits the generator cap and buffer");
            const int ok = n > 0 && Bench_OliviaDecode(x, n, got, sizeof(got), &r);
            printf("     (300 characters: %zu samples, %u symbols in %u blocks, header %d)\n",
                   n, r.symbols, r.blocks, r.header);
            check(ok, "olivia: a 300 character take decodes");
            check(ok && strcmp(got, msg) == 0, "olivia: the 300 characters come back as sent");
            free(x);
        }
    }
    printf("%s\n", failures ? "SELF CHECKS FAILED" : "all self checks passed");
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    const char* what = argc > 1 ? argv[1] : "test";
    // every mode that feeds the modem goes through the receiver's AGC, so it is set up once, before
    // any case. The *-wav verbs are left out on purpose: they decode a recording that already went
    // through a receiver's AGC, and running it through a second one would not be the radio's path.
    if (strcmp(what, "mode-id") && strcmp(what, "rtty-wav") && strcmp(what, "rtty-align") && strcmp(what, "align-check")
        && strcmp(what, "rtty-trace") && strcmp(what, "tx-check") && strcmp(what, "wefax")
        && strcmp(what, "navtex") && strcmp(what, "hell") && strcmp(what, "mfsk16")
        && strcmp(what, "olivia") && strcmp(what, "capture-check")
        && strcmp(what, "navtex-wav") && strcmp(what, "hell-wav")
        && strcmp(what, "mfsk16-wav") && strcmp(what, "olivia-wav")
        && strcmp(what, "record-check"))
        radio_agc_setup();
    if (!strcmp(what, "test")) return selftest();
    if (!strcmp(what, "rtty")) { return rtty_table(RTTY_TABLE_MSG, false); }
    if (!strcmp(what, "rtty-long"))
    {
        // One character is 2.1 % of the 48 character message, so a 0.0 % row only says "under
        // 2.1 %". Eight copies make 392 characters with the spaces between them: 0.0 % is then
        // under 0.26 %, and a row that is partly wrong reports a rate instead of a count of one or
        // two characters.
        char msg[512];
        msg[0] = 0;
        for (int i = 0; i < 8 && strlen(msg) + strlen(RTTY_TABLE_MSG) + 2 < sizeof(msg); i++)
        {
            strncat(msg, RTTY_TABLE_MSG " ", sizeof(msg) - strlen(msg) - 1);
        }
        return rtty_table(msg, true);
    }
    if (!strcmp(what, "record-check")) return record_check();
    if (!strcmp(what, "rtty-shift")) { return rtty_shift(); }
    if (!strcmp(what, "rtty-tune")) { return rtty_tune(); }
    if (!strcmp(what, "wefax")) return wefax_table();
    if (!strcmp(what, "navtex")) return navtex_table();
    if (!strcmp(what, "sitor-stream")) return sitor_stream();
    if (!strcmp(what, "hell")) return hell_table();
    if (!strcmp(what, "mfsk16")) return mfsk16_table();
    if (!strcmp(what, "olivia")) return olivia_table();
    if (!strcmp(what, "floors-2k5")) return floors_2k5();
    if (!strcmp(what, "floors-2k5-check")) return floors_2k5_check();
    // name the mode from the shape of the file, before any decoder is asked to like it
    if (!strcmp(what, "mode-id") && argc > 2) return mode_id(argv[2]);
    if (!strcmp(what, "gate-check")) return gate_check();
    // the gate a take has to clear before it is worth decoding, let alone pinning: it measures the
    // file and never touches the modem, so it cannot be fooled by the modem agreeing with itself
    if (!strcmp(what, "capture-check") && argc > 2)
    {
        const uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 450;
        const uint16_t baud = argc > 4 ? (uint16_t)atoi(argv[4]) : 50;
        return capture_check(argv[2], shift, baud);
    }
    // the non RTTY modes read a recording the same way: load it, trim the lead, and try the
    // starts one symbol apart that a recorder could have handed the decoder
    if (!strcmp(what, "wefax-wav") && argc > 2) { return wefax_wav(argv[2]); }
    if (!strcmp(what, "navtex-wav") && argc > 2)
        return navtex_wav(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? (float)atof(argv[4]) : -1.0f);
    if (!strcmp(what, "mfsk16-wav") && argc > 2)
        return mfsk16_wav(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? (float)atof(argv[4]) : -1.0f);
    if (!strcmp(what, "olivia-wav") && argc > 2)
        return olivia_wav(argv[2], argc > 3 ? argv[3] : "", argc > 4 ? (float)atof(argv[4]) : -1.0f);
    if (!strcmp(what, "hell-wav") && argc > 2)
        return hell_wav(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcmp(what, "rtty-wav") && argc > 2)
    {
        char got[8192];
        uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 170;
        const char* expect = argc > 4 ? argv[4] : "";
        uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 45;
        printf("decoding with %u Hz shift, %u baud\n", (unsigned)shift, (unsigned)baud);
        float cer = decode_wav(argv[2], expect, got, sizeof(got), shift, baud, false, NULL, 0);
        if (cer < 0.0f) return 0;
        printf("CER       : %.1f%% (after finding the message in the take)\n", cer);
        // the optional limit turns a recording into a graded case: a fixture whose text is pinned has
        // to keep decoding to that text, otherwise the mode accepts any change to the modem's output
        if (argc > 6 && cer > (float) atof(argv[6]))
        {
            printf("CER is above the %s %% this case pins\n", argv[6]);
            return 1;
        }
        return 0;
    }
    // the same recording and the same text as rtty-wav, one row per character: which ones the modem
    // missed, and whether the audio was there for them
    if (!strcmp(what, "rtty-align") && argc > 2 && argc > 4)
    {
        const uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 450;
        const uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 50;
        return align_wav(argv[2], argv[4], shift, baud);
    }
    // the two characters this take gets wrong, with the audio under them: the record the README
    // makes, kept by name so it cannot quietly change into a different pair
    if (!strcmp(what, "align-check") && argc > 4)
    {
        const uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 450;
        const uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 50;
        return align_check(argv[2], argv[4], shift, baud);
    }
    // the demodulator's own bit decisions, one row per character: which bit differed from what was
    // sent, and whether its clock was in step while it read it
    if (!strcmp(what, "rtty-trace") && argc > 4)
    {
        const uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 450;
        const uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 50;
        return rtty_trace(argv[2], argv[4], shift, baud);
    }
    if (!strcmp(what, "rtty-multipath")) return rtty_multipath();
    if (!strcmp(what, "tx-check")) return tx_check();
    fprintf(stderr, "usage: digi_bench [test|rtty|rtty-long|rtty-shift|rtty-tune|wefax|navtex"
                    "|rtty-multipath|tx-check|hell|mfsk16|olivia"
                    "|capture-check <file.wav> [shift] [45|50 baud]"
                    "|navtex-wav <file.wav> [expected text] [max CER %%]"
                    "|hell-wav <file.wav> [out.pgm]"
                    "|mfsk16-wav <file.wav> [expected text] [max CER %%]"
                    "|olivia-wav <file.wav> [expected text] [max CER %%]"
                    "|record-check"
                    "|rtty-wav <file.wav> [shift] [expected text] [45|50 baud] [max CER %%]"
                    "|rtty-align <file.wav> <shift> \"expected text\" [45|50 baud]"
                    "|align-check <file.wav> <shift> \"expected text\" [45|50 baud]"
                    "|rtty-trace <file.wav> <shift> \"expected text\" [45|50 baud]]\n");
    return 2;
}
