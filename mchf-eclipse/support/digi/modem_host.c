/* Host environment for the firmware sources: the few functions the RTTY modem reaches out for,
 * plus noise/fading, scoring and WAV output used by the bench.
 * Nothing here is compiled into the radio. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "bench.h"
#include "ui_driver.h"
#include "radio_management.h"
#include "uhsdr_digi_buffer.h"

/* ------------------------------------------------------------------ *
 *  the two globals the modem expects from the UI / radio manager
 * ------------------------------------------------------------------ */
bench_ts_t ts;

static char    rx_text[16384];
static size_t  rx_len;
static char    tx_script[4096];
static size_t  tx_pos;
static bool    tx_off_requested;
static int     tx_remove_calls;

/* Where in the sample stream each decoded character arrived. The alignment table needs to know
 * which stretch of audio a character came from, and the decoder is the only thing that knows: it
 * reports a character when it has finished reading its bits, not on a grid the caller can assume. */
size_t Bench_CharClock;
size_t Bench_CharAt[8192];
int    Bench_CharCount;

void UiDriver_TextMsgPutChar(char ch)
{
    if (Bench_CharCount < (int) (sizeof(Bench_CharAt) / sizeof(Bench_CharAt[0])))
        Bench_CharAt[Bench_CharCount++] = Bench_CharClock;
    if (rx_len + 1 < sizeof(rx_text))
    {
        rx_text[rx_len++] = ch;
        rx_text[rx_len] = '\0';
    }
}

void RadioManagement_Request_TxOff(void)
{
    tx_off_requested = true;
}

void Bench_TextReset(void)
{
    rx_len = 0;
    rx_text[0] = '\0';
}

const char* Bench_TextGet(void)
{
    return rx_text;
}

void Bench_TxFeed(const char* text)
{
    snprintf(tx_script, sizeof(tx_script), "%s", text);
    tx_pos = 0;
    tx_off_requested = false;
    tx_remove_calls = 0;
}

bool Bench_TxOffRequested(void)     { return tx_off_requested; }
void Bench_TxOffClear(void)         { tx_off_requested = false; }
int  Bench_TxRemoveCalls(void)      { return tx_remove_calls; }

/* The digi TX buffer, as the modulator uses it: one scripted string, EOT terminated. */
uint8_t DigiModes_TxBufferHasDataFor(digi_buff_consumer_t consumer)
{
    (void) consumer;
    return tx_script[tx_pos] != '\0' ? 1 : 0;
}

bool DigiModes_TxBufferRemove(uint8_t* c_ptr, digi_buff_consumer_t consumer)
{
    (void) consumer;
    tx_remove_calls++;
    if (tx_script[tx_pos] == '\0')
    {
        return false;
    }
    *c_ptr = (uint8_t) tx_script[tx_pos++];
    return true;
}

int32_t DigiModes_TxBufferPutChar(uint8_t c, digi_buff_consumer_t source)
{
    (void) c; (void) source;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  signal conditioning
 * ------------------------------------------------------------------ */
static uint32_t rng_state = 1u;

void Bench_RngSeed(uint32_t seed)
{
    rng_state = seed ? seed : 1u;
}

// xorshift32, so a run is reproducible across machines and compilers
static uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;                  // the state has to advance, or every sample is the same value
    return x;
}

float Bench_Gauss(void)
{
    float u1 = (float) rng_next() / 4294967296.0f;
    float u2 = (float) rng_next() / 4294967296.0f;
    if (u1 < 1e-9f) u1 = 1e-9f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float) M_PI * u2);
}

static float rms_of(const float32_t* x, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) acc += (double) x[i] * x[i];
    return (float) sqrt(acc / (n ? n : 1));
}

void Bench_AddNoise(float32_t* x, size_t n, float snr_db, float rms)
{
    float signal_rms = rms > 0.0f ? rms : rms_of(x, n);
    float noise_rms = signal_rms / powf(10.0f, snr_db / 20.0f);
    for (size_t i = 0; i < n; i++)
    {
        x[i] += noise_rms * Bench_Gauss();
    }
}

// Gaussian noise shaped to a band, as a 2.5 kHz channel hears it. A 12 kHz white floor and a
// 2.5 kHz floor are not the same channel: the arithmetic 10*log10(12000/2500) = 6.8 dB assumes the
// receiver rejects everything outside the channel, and that assumption is what this measures. The
// filtered noise is normalised to the requested total RMS, so snr_db is the SNR over the shaped
// band; at the same PSD inside the band, a 2.5 kHz channel is the 12 kHz SNR plus 6.8 dB.
static void design_bandpass(float* h, int taps, float lo_hz, float hi_hz, float fs)
{
    const int M = taps - 1;
    for (int k = 0; k <= M; k++)
    {
        const int m = k - M / 2;
        const double s_lo = m ? sin(2.0 * M_PI * lo_hz / fs * m) / (M_PI * m) : 2.0 * lo_hz / fs;
        const double s_hi = m ? sin(2.0 * M_PI * hi_hz / fs * m) / (M_PI * m) : 2.0 * hi_hz / fs;
        const double w = 0.54 - 0.46 * cos(2.0 * M_PI * k / M);   // Hamming
        h[k] = (float) ((s_hi - s_lo) * w);
    }
    // unity gain at the centre, so the filter is a channel shape and not an extra level change
    const double fc = 0.5 * (lo_hz + hi_hz);
    double gain = 0.0;
    for (int k = 0; k <= M; k++) gain += h[k] * cos(2.0 * M_PI * fc / fs * (k - M / 2));
    if (fabs(gain) > 1e-12)
        for (int k = 0; k <= M; k++) h[k] = (float) (h[k] / gain);
}

void Bench_AddNoiseBand(float32_t* x, size_t n, float snr_db, float rms, float lo_hz, float hi_hz)
{
    enum { TAPS = 257 };
    float signal_rms = rms > 0.0f ? rms : rms_of(x, n);
    float noise_rms = signal_rms / powf(10.0f, snr_db / 20.0f);
    float* w = malloc(n * sizeof(float));
    float* y = malloc(n * sizeof(float));
    if (!w || !y) { free(w); free(y); return; }
    for (size_t i = 0; i < n; i++) w[i] = Bench_Gauss();

    float h[TAPS];
    design_bandpass(h, TAPS, lo_hz, hi_hz, 12000.0f);
    for (size_t i = 0; i < n; i++)
    {
        double acc = 0.0;
        const size_t k0 = i >= TAPS - 1 ? i - (TAPS - 1) : 0;
        for (size_t k = k0; k <= i; k++) acc += (double) h[i - k] * w[k];
        y[i] = (float) acc;
    }

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += (double) y[i] * y[i];
    const double have = n ? sqrt(sum / (double) n) : 0.0;
    const double scale = have > 1e-12 ? (double) noise_rms / have : 0.0;
    for (size_t i = 0; i < n; i++) x[i] += (float) (y[i] * scale);
    free(w);
    free(y);
}

void Bench_AddQsb(float32_t* x, size_t n, float rate_hz, float depth_db)
{
    if (rate_hz <= 0.0f || depth_db <= 0.0f) return;
    float depth = powf(10.0f, -depth_db / 20.0f);      // linear trough
    for (size_t i = 0; i < n; i++)
    {
        float phase = 2.0f * (float) M_PI * rate_hz * (float) i / 12000.0f;
        float fade = depth + (1.0f - depth) * 0.5f * (1.0f + sinf(phase));
        x[i] *= fade;
    }
}

/* ------------------------------------------------------------------ *
 *  scoring
 * ------------------------------------------------------------------ */
size_t Bench_Levenshtein(const char* a, const char* b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la == 0) return lb;
    if (lb == 0) return la;
    size_t* row = malloc((lb + 1) * sizeof(size_t));
    if (!row) return 0;
    for (size_t j = 0; j <= lb; j++) row[j] = j;
    for (size_t i = 1; i <= la; i++)
    {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= lb; j++)
        {
            size_t up = row[j];
            size_t best = row[j - 1] + 1;
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            if (diag + cost < best) best = diag + cost;
            if (up + 1 < best) best = up + 1;
            row[j] = best;
            diag = up;
        }
    }
    size_t result = row[lb];
    free(row);
    return result;
}

float Bench_Cer(const char* expected, const char* got)
{
    size_t need = strlen(expected);
    if (need == 0) return 0.0f;
    return 100.0f * (float) Bench_Levenshtein(expected, got) / (float) need;
}

/* ------------------------------------------------------------------ *
 *  WAV output, 16 bit PCM mono
 * ------------------------------------------------------------------ */
static void put32(FILE* f, uint32_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f); }
static void put16(FILE* f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }

bool Bench_WavWrite(const char* path, const float32_t* x, size_t n, uint32_t rate)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t data = (uint32_t) n * 2;
    put32(f, 0x46464952u); put32(f, 36 + data); put32(f, 0x45564157u);   /* "WAVE" */
    put32(f, 0x20746D66u);   /* "fmt " */ put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    put32(f, 0x61746164u); put32(f, data);
    for (size_t i = 0; i < n; i++)
    {
        float v = x[i];
        if (v > 0.999f) v = 0.999f;
        if (v < -0.999f) v = -0.999f;
        put16(f, (uint16_t) (int16_t) lroundf(v * 32767.0f));
    }
    fclose(f);
    return true;
}
