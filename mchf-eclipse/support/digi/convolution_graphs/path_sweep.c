// Every SSB/CW filter path of the radio, the Hilbert pair it runs today against the convolution
// filter that would replace it, measured on the same grid and printed as CSV.
//
// compare.c draws the two paths it was written for - 2.7 kHz SSB and 500 Hz CW - and prints only the
// centre level for the rest.  The two gaps that leaves are closed here: a shape for every path, and a
// cost for every path, because neither is the same across the table.  The pair is 199 taps at 12 kHz
// below 3.8 kHz and 89 taps at 48 kHz from there up (the wide paths carry no decimation at all: see
// use_decimatedIQ in audio_driver.c), while the convolution filter is one geometry, 128-sample blocks
// of a 1024-tap response, at whatever rate the path hands blocks over.
//
// Build with build-sweep.sh.  Three metrics per path and side:
//
//   ripple  peak-to-peak level over the band's interior, clamped at 50 Hz where the band reaches zero
//           (the convolution filter's own low cut, and about where the Hilbert pair is 4 dB down), and
//           with the outer tenth of the width at each edge left out: a windowed sinc is 6 dB down at
//           its nominal edges by design, so scoring those is scoring the edge, not the flat part
//   skirt   the width from -6 dB to -60 dB past the top of the band, the same definition as the
//           geometry table in ../README.md
//   image   the worst level anywhere in the mirror of that same interior, so the low-cut corner is
//           not scored as sideband leakage either
//
// The stock taps are swept in the radio's own order: audio_filter.c hands the tables to arm_fir_f32()
// unreversed, so pCoeffs[0] meets the oldest sample, which is the convention the published -76 dB
// image figure was measured with.
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "filters.h"
#include "audio_filter.h"
#include "audio_convolution.c"

// counted by the host transform (convolution_fft.c), the same counter the probe uses
extern unsigned long conv_probe_fft_ops;

static void setup(const FilterPathDescriptor* p, int usb, float32_t rate)
{
    AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[p->id].width,
                                         (float32_t) p->offset, usb == 0, rate);
}

static double db(double x) { return 20.0 * log10(x + 1e-12); }

// H(f) of the taps the firmware just designed, one complex multiply per tap rather than a cos and a
// sin: the response is the DFT of the impulse response, h[k] * e^{-j w k}.
static double conv_resp(double f, double rate)
{
    const double w = 2.0 * M_PI * f / rate;
    const double cr = cos(w), sr = -sin(w);         // e^{-j w} per tap
    double zr = 1.0, zi = 0.0, re = 0.0, im = 0.0;
    for (int k = 0; k < cbs.nc; k++)
    {
        const double hr = cbs.impulse[2 * k], hi = cbs.impulse[2 * k + 1];
        re += hr * zr - hi * zi;
        im += hr * zi + hi * zr;
        const double nzr = zr * cr - zi * sr, nzi = zr * sr + zi * cr;
        zr = nzr; zi = nzi;
    }
    return hypot(re, im);
}

// the same for the pair: |sum u[k] e^{+j w k}| with u = I' + j s Q', s = +1 for the i+q sum
static double stock_resp(const FilterPathDescriptor* p, double f, double rate, int usb)
{
    const int T = p->FIR_numTaps;
    const double s = usb ? 1.0 : -1.0;
    const double w = 2.0 * M_PI * f / rate;
    const double cr = cos(w), sr = sin(w);          // e^{+j w} per tap
    double zr = 1.0, zi = 0.0, re = 0.0, im = 0.0;
    for (int k = 0; k < T; k++)
    {
        const int idx = T - 1 - k;
        const double ur = p->FIR_I_coeff_file[idx], ui = s * p->FIR_Q_coeff_file[idx];
        re += ur * zr - ui * zi;
        im += ur * zi + ui * zr;
        const double nzr = zr * cr - zi * sr, nzi = zr * sr + zi * cr;
        zr = nzr; zi = nzi;
    }
    return hypot(re, im);
}

enum { MAXPTS = 32768 };
static double lv[MAXPTS];

typedef double (*resp_fn)(const FilterPathDescriptor*, double, double, int);

static double resp_conv_wrap(const FilterPathDescriptor* p, double f, double rate, int usb)
{
    (void) p; (void) usb;
    return conv_resp(f, rate);
}

// one grid, one pass, three numbers
static void metrics(resp_fn fn, const FilterPathDescriptor* p, double rate, double lo, double hi,
                    int usb, double* ripple, double* skirt, double* image)
{
    const double step = rate >= 30000.0 ? 1.0 : 0.5;
    const int n = (int) (rate * 0.5 / step) + 1;
    const double guard = 0.1 * (hi - lo);
    const double ilo = lo + guard, ihi = hi - guard;
    double ref = 0.0, worst = 0.0;

    for (int i = 0; i < n; i++)
    {
        const double f = i * step;
        lv[i] = fn(p, f, rate, usb);
        if (f >= ilo && f <= ihi)
        {
            if (lv[i] > ref) ref = lv[i];
            // the mirror of the same offset, on the other side of zero
            const double im = fn(p, -f, rate, usb);
            if (im > worst) worst = im;
        }
    }
    if (ref <= 0.0)
    {
        *ripple = *skirt = *image = 0.0;
        return;
    }

    double mn = ref, mx = 0.0;
    for (int i = 0; i < n; i++)
    {
        const double f = i * step;
        if (f >= ilo && f <= ihi)
        {
            if (lv[i] < mn) mn = lv[i];
            if (lv[i] > mx) mx = lv[i];
        }
    }
    *ripple = db(mx / mn);
    *image = db(worst / ref);

    // out through the top edge: the first -6 and -60 dB crossings above the band.  When the filter is
    // already 60 dB down at the nominal edge there is no skirt to measure - the pair's 3.6 kHz paths
    // are like that, because their IQ lowpass closes at 3.5 kHz and the post-demod IIR finishes the
    // shape - and the column says so rather than reporting the zero a search from that edge returns.
    double f6 = -1.0, f60 = -1.0;
    const int start = (int) ceil(hi / step);
    if (start < n && lv[start] > ref * 0.001)
    {
        for (int i = start; i < n; i++)
        {
            if (f6 < 0.0 && lv[i] <= ref * 0.501187) f6 = i * step;
            if (lv[i] <= ref * 0.001) { f60 = i * step; break; }
        }
    }
    *skirt = (f6 >= 0.0 && f60 >= 0.0) ? f60 - f6 : -1.0;
}

// what the convolution filter bills per sample, counted as the probe counts it: the transform's own
// arithmetic plus nfor partitions x (size * 2) bins x (4 multiplies + 2 adds) per block
static double conv_ops_per_sample(double rate)
{
    enum { N = 2048 };
    static float32_t i[32], q[32];
    const int slice = 8;
    const double width = 2700.0, offset = 1350.0;

    AudioDriver_ConvolutionSetFilterPath((float32_t) width, (float32_t) offset, false, (float32_t) rate);
    AudioDriver_ConvolutionReset();
    const unsigned long before = conv_probe_fft_ops;
    long out = 0;
    for (int at = 0; at < N; at += slice)
    {
        for (int k = 0; k < slice; k++)
        {
            const double a = 2.0 * M_PI * 700.0 * (at + k) / rate;
            i[k] = (float32_t) cos(a);
            q[k] = (float32_t) sin(a);
        }
        AudioDriver_ConvolutionInput(i, q, slice);
        convolution_handle();
        out += AudioDriver_ConvolutionOutput(i, q, slice);
    }
    const double blocks = (double) N / cbs.size;
    const double partition = (double) cbs.nfor * (cbs.size * 2) * 6.0;
    const double fft = (double) (conv_probe_fft_ops - before);
    (void) out;
    return (fft + partition * blocks) / N;
}

int main(void)
{
    // one geometry, so one bill per rate rather than per path
    const double rates[] = { 12000.0, 24000.0, 48000.0 };
    double conv_ops[3];
    for (int r = 0; r < 3; r++)
    {
        conv_ops[r] = conv_ops_per_sample(rates[r]);
        fprintf(stderr, "geometry %d/%d, rate %.0f: %.0f ops/sample\n",
                FFT_CONVOLUTION_SIZE, cbs.nc, rates[r], conv_ops[r]);
    }
    fprintf(stderr, "ram: masks+history %.1f KiB, taps+scratch %.1f KiB, blocks %.1f KiB, total %.1f KiB\n",
            sizeof(cob) / 1024.0, sizeof(cbs) / 1024.0, sizeof(conv_blocks) / 1024.0,
            (sizeof(cob) + sizeof(cbs) + sizeof(conv_blocks)) / 1024.0);

    printf("id,name,width,centre,rate,taps,stock_ops,stock_ops_s,stock_pct,"
           "conv_ops,conv_ops_s,conv_pct,"
           "stock_ripple,stock_skirt,stock_img,conv_ripple,conv_skirt,conv_img\n");

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if ((p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) == 0
            || p->FIR_I_coeff_file == NULL || p->FIR_Q_coeff_file == NULL)
        {
            continue;
        }

        // the rate the blocks carry: the pair runs at 48 kHz unless this path is one of the decimated
        // ones, which is the radio's own test in AudioDriver_RxProcessor() and in the setup above it
        const double rate = p->FIR_I_coeff_file == i_rx_new_coeffs ? 48000.0 / p->sample_rate_dec : 48000.0;
        const double width = FilterInfo[p->id].width;
        const double centre = p->offset ? p->offset : width / 2.0;
        double lo = centre - width / 2.0, hi = centre + width / 2.0;
        if (lo < 50.0) lo = 50.0;

        setup(p, 1, (float32_t) rate);

        double s_rip, s_skirt, s_img, c_rip, c_skirt, c_img;
        metrics(stock_resp, p, rate, lo, hi, 1, &s_rip, &s_skirt, &s_img);
        metrics(resp_conv_wrap, p, rate, lo, hi, 1, &c_rip, &c_skirt, &c_img);

        const double s_ops = 4.0 * p->FIR_numTaps;      // 4 per tap: two multiplies and two adds, both channels
        const double c_ops = rate <= 12000.0 ? conv_ops[0] : (rate <= 24000.0 ? conv_ops[1] : conv_ops[2]);
        const double s_ops_s = s_ops * rate, c_ops_s = c_ops * rate;
        // 1.5 cycles per counted operation at 168 MHz, the model the README's CPU column uses
        const double s_pct = s_ops_s * 1.5 / 168e6 * 100.0, c_pct = c_ops_s * 1.5 / 168e6 * 100.0;

        printf("%d,%s,%.0f,%.0f,%.0f,%d,%.0f,%.0f,%.2f,%.0f,%.0f,%.2f,"
               "%.2f,%.1f,%.1f,%.2f,%.1f,%.1f\n",
               id, p->name, width, centre, rate, p->FIR_numTaps,
               s_ops, s_ops_s, s_pct, c_ops, c_ops_s, c_pct,
               s_rip, s_skirt, s_img, c_rip, c_skirt, c_img);
    }
    return 0;
}
