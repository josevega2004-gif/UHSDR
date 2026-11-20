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
    float       in_db;       // how loud the station arrives, relative to the modulator's own level;
                             // the AGC is what turns this knob into no change at all
    uint16_t    rx_shift;    // the shift setting, when it is not the signal's own (0 = same): this
                             // is how a case grades a wrong setting without moving the signal too
    float       tx_clock;    // the transmitter's symbol clock error in percent, positive = faster
                             // than the receiver expects, so the bits arrive short
    float       min_cer;     // a floor for the cases that have to keep failing: a wrong speed
                             // setting decodes badly, and a change that made it decode would be
                             // a different modem, not a better one
} rtty_case_t;

// The demodulator emits one or two characters of junk while it locks on, which is a property of
// the modem but not what the CER is meant to measure, so the score is taken from the best
// alignment of the received text against the expected one and the skipped lead-in is reported.
static int lead_in_junk;
// what the last score was measured on: the length of the expected text and how many of its
// characters the decoder got wrong. One character is 2.1 % of the 48 character message, so 0.0 %
// says "under one character wrong", not "clean".
static int cer_chars;
static int cer_wrong;

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

static float score_aligned(const char* expected, const char* got){
    float best = 100.0f;
    const size_t need = strlen(expected);
    size_t best_wrong = need;               // a total failure counts every character, not none
    int best_skip = 0;
    for (int skip = 0; skip < 12 && got[skip]; skip++)
    {
        size_t wrong = Bench_Levenshtein(expected, got + skip);
        float cer = need ? 100.0f * (float) wrong / (float) need : 0.0f;
        if (cer < best) { best = cer; best_skip = skip; best_wrong = wrong; }
    }
    lead_in_junk = best_skip;
    cer_chars = (int) need;
    cer_wrong = (int) best_wrong;
    return best;
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
        Rtty_BenchTxSpaceTone(915.0f + (float) c->shift + tune, (uint32_t) TXRATE);
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

    // how loudly the station arrives. The AGC is what should make this irrelevant, so this is the
    // knob the level rows turn; it is applied to the receiver's input, before the AGC.
    if (c->in_db != 0.0f)
    {
        const float g = powf(10.0f, c->in_db / 20.0f);
        for (size_t i = 0; i < n; i++) buf[i] *= g;
    }

    // the receiver: a cold AGC, the fixed post-AGC gain, then the demodulator. Cold, because the
    // AGC carries its gain, ring buffer and hang counter from one transmission into the next, so a
    // level row would otherwise measure whatever the case before it left in the gain and its
    // position in the table would be part of the result - the same order dependency the WAV check
    // had. The radio starts a new station from this state, and every case does too.
    AudioAgc_ResetAgcWdsp();
    AudioAgc_SetupAgcWdsp(RATE, false);
    radio_agc(buf, n);

    // RX: the firmware demodulator, characters collected by the UI stand-in
    for (size_t i = 0; i < n; i++)
    {
        Rtty_Demodulator_ProcessSample(buf[i]);
    }
    snprintf(got, got_sz, "%s", norm_baudot(Bench_TextGet(), 1));
    return score_aligned(norm_baudot(msg, 0), got);
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
        // 2 Hz, 10 dB deep, at 10 dB SNR. The AGC is what makes this row decodable at all: measured
        // 6.2 % (3/48) short and 2.3 % (9/392) long, with the receiver's AGC in the loop, against
        // 58.3 % when the bench fed the demodulator directly and left the level to chance.
        { .label = "qsb fast 10",  .shift = 170, .snr_db = 10.0f, .qsb_rate = 2.0f,
          .qsb_depth = 10.0f, .max_cer = 10.0f },
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
    printf("%-16s %7s %8s %6s  %s\n", "case", "CER %", "err/len", "lead", "decoded");
    char got[8192];
    int failures = 0;
    for (size_t i = 0; i < n_cases; i++)
    {
        int eot = 0;
        float cer = rtty_run(&cases[i], msg, long_mode, got, sizeof(got), &eot);
        char counted[32];
        snprintf(counted, sizeof(counted), "%d/%d", cer_wrong, cer_chars);
        printf("%-16s %7.1f %8s %6d  %.64s%s%s\n", cases[i].label, cer, counted, lead_in_junk,
               got, strlen(got) > 64 ? " ..." : "",
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
    // a file is a fresh receiver: reset before init, as rtty_run does, because the init is what
    // puts the filter state and its pointers back. Without this the decode starts from whatever
    // the last case left behind, so the check quietly depended on which case ran before it.
    Rtty_BenchResetDecoder();
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
 *  the capture gate: whether a recording is worth pinning at all
 * ------------------------------------------------------------------ */
// Pinning an on air take promises four things: the audio holds the signal, at the pair the modem
// filters expect, on a bit clock that repeats, with no gap in it. Every one of those has been
// assumed rather than measured here at least once - a pair 1.5 kHz off, two "tones" that were one
// carrier's skirt, a take that was mostly noise, and truncation. This verb measures them.
typedef struct { float* x; size_t n; uint32_t rate; } cap_t;

static int cap_load(const char* path, cap_t* c)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
        free(buf); fclose(f); printf("short read on %s\n", path); return 0;
    }
    fclose(f);

    uint32_t rate = 0, ch = 1, bits = 0, tag = 0, i = 12; const uint8_t* data = 0; uint32_t dlen = 0;
    while (i + 8 <= (size_t)sz)
    {
        uint32_t clen = rd32(buf + i + 4);
        if (clen > (size_t)sz - i - 8) break;
        if (!memcmp(buf + i, "fmt ", 4))
        {
            if (clen < 16) { free(buf); printf("the fmt chunk of %s is too short\n", path); return 0; }
            tag  = rd16(buf + i + 8);
            ch   = rd16(buf + i + 10);
            rate = rd32(buf + i + 12);
            bits = rd16(buf + i + 22);
        }
        else if (!memcmp(buf + i, "data", 4)) { data = buf + i + 8; dlen = clen; }
        else break;
        i += 8 + clen + (clen & 1);
    }
    if (!data || !rate || !dlen || tag != 1 || bits != 16 || !ch)
    {
        free(buf);
        printf("%s: needs a 16 bit PCM WAV with a data chunk (format %u, %u bit, %u channel(s), %u Hz)\n",
               path, tag, bits, ch, rate);
        return 0;
    }
    const size_t frames = (size_t)dlen / (2 * ch);
    c->x = malloc(frames * sizeof(float));
    if (!frames || !c->x) { free(c->x); free(buf); printf("%s holds no audio\n", path); return 0; }
    for (size_t k = 0; k < frames; k++)
    {
        c->x[k] = (float)(int16_t)rd16(data + k * 2 * ch) / 32768.0f;
    }
    c->n = frames;
    c->rate = rate;
    free(buf);
    return 1;
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

typedef struct { double period_ms, dispersion_pct; int transitions; } gate_clock_t;

// The tone the signal is on flips at every bit boundary, so the transitions sit on a grid whose step
// is the bit. Fitting that step over the whole take, then measuring how far the transitions scatter
// off it, is the difference between a clock and a stretch of audio that happens to have two tones.
static void gate_clock(const cap_t* c, double lo, double hi, double baud, gate_clock_t* k)
{
    const int w = (int)lround(0.005 * c->rate);
    const int hop = (int)lround(0.001 * c->rate);
    size_t cap = 4096, n = 0;
    const size_t nh = c->n > (size_t)w ? (c->n - (size_t)w) / (size_t)hop + 1 : 0;
    double* t = malloc(cap * sizeof(double));
    double* g = nh ? malloc(nh * sizeof(double)) : NULL;
    k->transitions = 0; k->period_ms = 0.0; k->dispersion_pct = 0.0;
    if (!t || !g) { free(t); free(g); return; }
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
            if (n == cap) { cap *= 2; t = realloc(t, cap * sizeof(double)); if (!t) { free(g); return; } }
            t[n++] = t0 + (t1 - t0) * (g0 / (g0 - g1));
            state = now;
            settled = opp_start;
            opp = 0;
        }
    }
    free(g);
    if (n < 16) { free(t); return; }
    // A Baudot character is 7.5 bits long - one start, five data, 1.5 stop - so the transitions at a
    // character boundary sit half a bit away from the ones inside a character. Fitting the plain
    // coherence has to reconcile both phases on one grid, and on the DDK2 fixture that pulls the
    // answer to 18.75 ms, 6 % short of the 20 ms the decoder itself uses, with 19 % of a bit left
    // over as scatter. Squaring the phase first cancels the half bit and leaves the bit period;
    // the two phases come back below as two centres, which is what the scatter is measured against.
    //
    // the search is deliberately wider than the accept window: a recording whose time base is wrong
    // by 20 % still has a grid, and finding it is what lets the gate say "40 baud, not 50" instead of
    // reporting the nominal period for a take that has no such period at all
    const double nominal = 1000.0 / baud;
    double best_t = nominal, best_r = -1.0;
    for (double T = nominal * 0.70; T <= nominal * 1.30; T += 0.005)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            const double ph = fmod(t[i] * 1000.0, T) / T;
            re += cos(4.0 * GATE_PI * ph);
            im += sin(4.0 * GATE_PI * ph);
        }
        const double r = sqrt(re * re + im * im) / (double)n;
        if (r > best_r) { best_r = r; best_t = T; }
    }
    // A grid that explains the transitions at the bit period explains them at half of it too, so a
    // take with long marks can fit a harmonic as well as the fundamental, and reporting the harmonic
    // turns a 40 baud take into "15 ms". Among the periods that explain the transitions equally
    // well, the longest is the bit: the loop runs upward, so a later near-equal fit replaces the
    // shorter one instead of being hidden behind it.
    for (double T = nominal * 0.70; T <= nominal * 1.30; T += 0.005)
    {
        if (T <= best_t) continue;
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            const double ph = fmod(t[i] * 1000.0, T) / T;
            re += cos(4.0 * GATE_PI * ph);
            im += sin(4.0 * GATE_PI * ph);
        }
        const double r = sqrt(re * re + im * im) / (double)n;
        if (r >= 0.995 * best_r) best_t = T;
    }
    double centre[2];
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            const double ph = fmod(t[i] * 1000.0, best_t) / best_t;
            re += cos(2.0 * GATE_PI * ph);
            im += sin(2.0 * GATE_PI * ph);
        }
        centre[0] = fmod(atan2(im, re) / (2.0 * GATE_PI) + 1.0, 1.0);
        centre[1] = fmod(centre[0] + 0.5, 1.0);
    }
    for (int round = 0; round < 3; round++)
    {
        double re[2] = {0.0, 0.0}, im[2] = {0.0, 0.0};
        for (size_t i = 0; i < n; i++)
        {
            const double ph = fmod(t[i] * 1000.0, best_t) / best_t;
            int k = 0;
            for (int c = 1; c < 2; c++)
            {
                double near = ph - centre[c], best = ph - centre[k];
                while (near > 0.5) near -= 1.0;
                while (near < -0.5) near += 1.0;
                while (best > 0.5) best -= 1.0;
                while (best < -0.5) best += 1.0;
                if (fabs(near) < fabs(best)) k = c;
            }
            re[k] += cos(2.0 * GATE_PI * ph);
            im[k] += sin(2.0 * GATE_PI * ph);
        }
        for (int c = 0; c < 2; c++)
            if (re[c] != 0.0 || im[c] != 0.0)
                centre[c] = fmod(atan2(im[c], re[c]) / (2.0 * GATE_PI) + 1.0, 1.0);
    }
    double sq = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        const double ph = fmod(t[i] * 1000.0, best_t) / best_t;
        double d0 = ph - centre[0], d1 = ph - centre[1];
        while (d0 > 0.5) d0 -= 1.0;
        while (d0 < -0.5) d0 += 1.0;
        while (d1 > 0.5) d1 -= 1.0;
        while (d1 < -0.5) d1 += 1.0;
        const double d = fabs(d0) <= fabs(d1) ? d0 : d1;
        sq += d * d;
    }
    k->period_ms = best_t;
    k->transitions = (int)n;
    k->dispersion_pct = 100.0 * sqrt(sq / (double)n);
    free(t);
}

typedef struct { double longest_ms, fraction; int gaps; } gate_drop_t;

static int cmp_double(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

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

// Thresholds, set from four measurements that bracket the useful range. Transitions are interpolated
// zero crossings against two phase centres, so these are the audio's numbers and not the estimator's:
// a clean 50 baud take generated for this purpose measures 20.000 ms with 0.1 % scatter, the DDK2
// fixture - real audio through the WebSDR client's recorder - 20.000 ms with 8.8 %, a 45.45 baud take
// 22.000 ms with 2.2 %, and a 40 baud take 25.000 ms with 0.9 %.
//
// The fixture is the realistic case, so the limits sit just outside it. The period window is 5 %,
// which passes both reference takes at their nominal period and refuses a 45.45 baud take at 10 %
// long even though its pair and its scatter look perfect; the scatter limit is 12 %, above the
// fixture's 8.8 % and well above the 2.2 % a mis-keyed rate costs. Scatter is what carries a take
// damaged in a way the pair cannot see - a keyed signal whose edges have been smeared or clipped -
// while the pair and the bimodality carry a stretch of noise or hash. A take that cannot clear these
// is not evidence about the modem either way.
#define GATE_SINGLE_MIN   0.55   // windows holding one tone
#define GATE_BOTH_MAX     0.35   // windows holding both: straddled bits plus noise
#define GATE_PAIR_SLACK   30.0   // Hz between the measured pair and the declared shift
#define GATE_PERIOD_TOL   0.05   // fraction of a bit
#define GATE_DISPERSION   12.0   // percent of a bit
#define GATE_GAP_MS        5.0   // longest hole a take may carry

static int capture_check(const char* path, uint16_t shift, uint16_t baud)
{
    cap_t c;
    if (!cap_load(path, &c)) return 1;
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
    printf("            spread %.1f / %.1f Hz; offset from the modem's 915/1365 %.0f / %.0f Hz\n",
           pair.lo_spread, pair.hi_spread, 915.0 - pair.lo_hz, 1365.0 - pair.hi_hz);
    printf("bimodality: %.2f low, %.2f high, %.2f both (5 ms windows, %d)\n",
           pair.f_low, pair.f_high, pair.f_both, pair.windows);
    if (clock.transitions)
    {
        printf("clock     : period %.3f ms (%d transitions), scatter %.1f %% of a bit\n",
               clock.period_ms, clock.transitions, clock.dispersion_pct);
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
    if (!clock.transitions || fabs(clock.period_ms - nominal) > nominal * GATE_PERIOD_TOL
        || clock.dispersion_pct > GATE_DISPERSION)
    {
        // the rate the take actually carries, so a wrong time base reads as the baud rate it is and
        // not only as a period in milliseconds
        printf("REJECT    : no %u baud clock (wanted %.2f ms, measured %.2f ms = %.1f baud, "
               "scatter %.1f %%)\n",
               (unsigned)baud, nominal, clock.period_ms,
               clock.period_ms > 0.0 ? 1000.0 / clock.period_ms : 0.0, clock.dispersion_pct);
        bad = 1;
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
        float cer = rtty_run(&c, "DE UHSDR BENCH TEST ONE", false, got, sizeof(got), &eot);
        check(cer == 0.0f, "clean RTTY loopback decodes with zero character errors");
        printf("     (lead-in junk before lock: %d characters)\n", lead_in_junk);
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
        Rtty_BenchTxBitScale(1.0f);   // this recording is the modem's own clock, not a case's
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
    // every mode that feeds the modem goes through the receiver's AGC, so it is set up once, before
    // any case. rtty-wav is left out on purpose: it decodes a recording that already went through a
    // receiver's AGC, and running it through a second one would not be the radio's path.
    if (strcmp(what, "rtty-wav") && strcmp(what, "wefax") && strcmp(what, "capture-check")) radio_agc_setup();
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
    if (!strcmp(what, "rtty-shift")) { return rtty_shift(); }
    if (!strcmp(what, "rtty-tune")) { return rtty_tune(); }
    if (!strcmp(what, "wefax")) return wefax_table();
    // the gate a take has to clear before it is worth decoding, let alone pinning: it measures the
    // file and never touches the modem, so it cannot be fooled by the modem agreeing with itself
    if (!strcmp(what, "capture-check") && argc > 2)
    {
        const uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 450;
        const uint16_t baud = argc > 4 ? (uint16_t)atoi(argv[4]) : 50;
        return capture_check(argv[2], shift, baud);
    }
    if (!strcmp(what, "rtty-wav") && argc > 2)
    {
        char got[8192];
        uint16_t shift = argc > 3 ? (uint16_t)atoi(argv[3]) : 170;
        const char* expect = argc > 4 ? argv[4] : "";
        uint16_t baud = argc > 5 ? (uint16_t)atoi(argv[5]) : 45;
        printf("decoding with %u Hz shift, %u baud\n", (unsigned)shift, (unsigned)baud);
        float cer = decode_wav(argv[2], expect, got, sizeof(got), shift, baud);
        if (cer < 0.0f) return 0;
        printf("CER       : %.1f%% (after aligning the lead in)\n", cer);
        // the optional limit turns a recording into a graded case: a fixture whose text is pinned has
        // to keep decoding to that text, otherwise the mode accepts any change to the modem's output
        if (argc > 6 && cer > (float) atof(argv[6]))
        {
            printf("CER is above the %s %% this case pins\n", argv[6]);
            return 1;
        }
        return 0;
    }
    fprintf(stderr, "usage: digi_bench [test|rtty|rtty-long|rtty-shift|rtty-tune|wefax"
                    "|capture-check <file.wav> [shift] [45|50 baud]"
                    "|rtty-wav <file.wav> [shift] [expected text] [45|50 baud] [max CER %%]]\n");
    return 2;
}
