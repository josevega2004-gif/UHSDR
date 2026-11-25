// Scratch harness for the review graphs.  Drives the firmware's own designer, setup, queues and worker
// (audio_convolution.c in as text) and the radio's own filter path table, and prints tagged CSV rows.
// -DBEFORE builds against the committed tree and repeats the setup audio_driver.c had there.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "filters.h"
#include "audio_filter.h"
#include "audio_convolution.c"

static void setup(const FilterPathDescriptor* p, int lsb, float32_t rate)
{
#ifdef BEFORE
    cbs.size = FFT_CONVOLUTION_SIZE / 2;
    cbs.nc = CONVOLUTION_MAX_NO_OF_COEFFS;
    if (cbs.nc / cbs.size > CONVOLUTION_MAX_NO_OF_BLOCKS) cbs.nc = cbs.size * CONVOLUTION_MAX_NO_OF_BLOCKS;
    cbs.nfor = cbs.nc / cbs.size;
    cbs.buffidx = 0;
    const float32_t conv_bw = (float32_t) FilterInfo[p->id].width;
    const float32_t conv_lo = conv_bw / 10.0f;
    const float32_t conv_sign = lsb ? -1.0f : 1.0f;
    AudioDriver_SetConvolutionFilter(cbs.nc, conv_sign * conv_lo, conv_sign * conv_bw, rate, 0, rate / conv_bw);
    AudioDriver_ConvolutionReset();
#else
    AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[p->id].width, (float32_t) p->offset, lsb != 0, rate);
#endif
}

static double db(double x) { return 20.0 * log10(x + 1e-9); }      // floor at -180 dB

// audio amplitude for a unit complex tone at f through the taps the firmware just designed
static double conv_amp(double f, double rate)
{
    double re = 0, im = 0;
    for (int k = 0; k < cbs.nc; k++)
    {
        const double w = 2 * M_PI * f * k / rate, hr = cbs.impulse[2 * k], hi = cbs.impulse[2 * k + 1];
        re += hr * cos(w) + hi * sin(w);
        im += hi * cos(w) - hr * sin(w);
    }
    return sqrt(re * re + im * im);
}

// the same through the path's Hilbert pair and i+q / i-q sum, in arm_fir_f32's tap order
static double stock_amp(const FilterPathDescriptor* p, double f, double rate, int lsb)
{
    const int T = p->FIR_numTaps;
    const double s = lsb ? -1 : 1;
    double a = 0, b = 0;
    for (int k = 0; k < T; k++)
    {
        const double w = 2 * M_PI * f * k / rate;
        a += p->FIR_I_coeff_file[T - 1 - k] * cos(w) - s * p->FIR_Q_coeff_file[T - 1 - k] * sin(w);
        b += p->FIR_I_coeff_file[T - 1 - k] * sin(w) + s * p->FIR_Q_coeff_file[T - 1 - k] * cos(w);
    }
    return sqrt(a * a + b * b);
}

static const FilterPathDescriptor* find(int id, int offset)
{
    for (int i = 1; i < AUDIO_FILTER_PATH_NUM; i++)
        if (FilterPathInfo[i].id == id && FilterPathInfo[i].offset == offset
            && (FilterPathInfo[i].mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW)))) return &FilterPathInfo[i];
    return NULL;
}

static void sweep(const char* tag, const FilterPathDescriptor* p, double from, double to, double step)
{
    const double rate = 12000.0;
    setup(p, 0, (float32_t) rate);
    for (double f = from; f <= to + 1e-6; f += step)
        printf("%s,%.1f,%.2f,%.2f\n", tag, f, db(stock_amp(p, f, rate, 0)), db(conv_amp(f, rate)));
}

// A tone switched on at sample ON: what comes out of each path, as an envelope.  Through the real
// queues and worker, 8 samples a slice, which is what the decimated path hands over.
#define NB 6144
#define ON 2048
static void burst(const char* tag, const FilterPathDescriptor* p, double f)
{
    const double rate = 12000.0;
    static double cr[NB], ci[NB], s0[NB], s1[NB];
    setup(p, 0, (float32_t) rate);
    memset(cob.fftin, 0, sizeof cob.fftin);
    memset(cob.fftout, 0, sizeof cob.fftout);
    float32_t i[8], q[8];
    long w = 0;
    for (int n = 0; n < NB; n += 8)
    {
        for (int k = 0; k < 8; k++)
        {
            const double a = 2 * M_PI * f * (n + k - ON) / rate;
            i[k] = n + k >= ON ? (float32_t) cos(a) : 0.0f;
            q[k] = n + k >= ON ? (float32_t) sin(a) : 0.0f;
        }
        AudioDriver_ConvolutionInput(i, q, 8);
        convolution_handle();
        const uint16_t g = AudioDriver_ConvolutionOutput(i, q, 8);
        for (int k = 0; k < 8; k++, w++) { cr[w] = k < g ? i[k] : 0.0; ci[w] = k < g ? q[k] : 0.0; }
    }
    // the path in use, twice: the tone and the tone a quarter cycle on, so their two outputs give the envelope
    const int T = p->FIR_numTaps;
    for (int n = 0; n < NB; n++)
    {
        double a0 = 0, a1 = 0;
        for (int k = 0; k < T && n - k >= ON; k++)
        {
            const double a = 2 * M_PI * f * (n - k - ON) / rate;
            a0 += p->FIR_I_coeff_file[T - 1 - k] * cos(a) + p->FIR_Q_coeff_file[T - 1 - k] * sin(a);
            a1 += p->FIR_I_coeff_file[T - 1 - k] * sin(a) - p->FIR_Q_coeff_file[T - 1 - k] * cos(a);
        }
        s0[n] = a0; s1[n] = a1;
    }
    const double cfin = hypot(cr[NB - 200], ci[NB - 200]), sfin = hypot(s0[NB - 200], s1[NB - 200]);
    double c50 = -1, s50 = -1;
    for (int n = ON - 360; n < ON + 1800; n++)
    {
        const double ce = hypot(cr[n], ci[n]) / cfin, se = hypot(s0[n], s1[n]) / sfin;
        if (c50 < 0 && ce >= 0.5) c50 = (n - ON) * 1000.0 / rate;
        if (s50 < 0 && se >= 0.5) s50 = (n - ON) * 1000.0 / rate;
        printf("%s,%.4f,%.5f,%.5f\n", tag, (n - ON) * 1000.0 / rate, se, ce);
    }
    printf("meta,%s_delay_ms,%.2f,%.2f\n", tag, s50, c50);
}

int main(void)
{
    const FilterPathDescriptor* ssb = find(AUDIO_2P7KHZ, 1350);
    const FilterPathDescriptor* cw = find(AUDIO_500HZ, 750);
    if (!ssb || !cw) { fprintf(stderr, "path not found\n"); return 1; }
    setup(ssb, 0, 12000.0f);
    printf("meta,geometry,%d,%d\n", FFT_CONVOLUTION_SIZE, cbs.nc);
    printf("meta,ram_bytes,%zu,0\n", sizeof(cob) + sizeof(cbs) + sizeof(conv_blocks));

    sweep("ssb", ssb, -6000, 6000, 2);
    sweep("cw", cw, -2000, 2000, 1);

    // the centre of every SSB/CW path, on the sideband asked for and on the other one
    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if ((p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) == 0 || p->FIR_I_coeff_file == NULL) continue;
        const int decimated = p->FIR_I_coeff_file == i_rx_new_coeffs;
        const double rate = decimated ? 48000.0 / p->sample_rate_dec : 48000.0;
        const double width = FilterInfo[p->id].width, centre = p->offset ? p->offset : width / 2;
        setup(p, 0, (float32_t) rate);
        printf("level,%d,%s,%.0f,%.0f,%.0f,%.2f,%.2f,%.2f\n", id, p->name, width, centre, rate,
               db(stock_amp(p, centre, rate, 0)), db(conv_amp(centre, rate)), db(conv_amp(-centre, rate)));
    }

    burst("burst_ssb", ssb, 1000.0);
    burst("burst_cw", cw, 750.0);
    return 0;
}
