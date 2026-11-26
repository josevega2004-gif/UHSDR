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
// A row is printed n/c on the stock shape when the stock table's -6 dB point is inside the path's
// band: the top edge is then the table's cutoff, not the path, and the post-demod IIR cannot restore
// it.  At this geometry that is the two 3.6 kHz paths and six of the wide paths.  Cost is still printed.
//
// Build with build-sweep.sh.  Three metrics per path and side:
//
// `--chain` prints a second table: the same paths through the path's pre-AGC IIR, the firmware WDSP
// AGC (audio_agc.c) and the post-AGC anti-alias IIR.  The linear columns use the IIR magnitudes; the
// two-tone AGC columns run the real state recursion at the bench level and at a station 20 dB weaker,
// because the AGC is level-dependent.
//
// `--chain-levels` repeats that two-tone pass over station level instead of printing shape columns:
// 20 dB above the bench level down to 40 dB below it in 2 dB steps (A = 0.5 to 0.0005), one row per
// path and level.  It is a separate table so the chain CSV keeps exactly the columns it has, and it is
// where the 20 dB column is checked against the whole range rather than one other point.  The upward
// half is not decoration: the factory AGC's knee sits a few dB below the bench level, so a sweep that
// only went down would measure a fixed gain and say nothing about compression.
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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "filters.h"
#include "audio_filter.h"
#include "audio_agc.h"
#include "audio_convolution.c"

// counted by the host transform (convolution_fft.c), the same counter the probe uses
extern unsigned long conv_probe_fft_ops;

static void setup(const FilterPathDescriptor* p, int usb, float32_t rate)
{
    AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[p->id].width,
                                         (float32_t) p->offset, usb == 0, rate);
}

// The paths both tables cover: SSB or CW, with both coefficient tables present.
static int is_ssb_cw_path(const FilterPathDescriptor* p)
{
    return (p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) != 0
        && p->FIR_I_coeff_file != NULL && p->FIR_Q_coeff_file != NULL;
}

// What every table takes from a path: the rate its blocks carry, the band it asks for, and the
// 50 Hz clamp where the band reaches zero.
typedef struct { double rate, width, centre, lo, hi; } path_band_t;

static path_band_t path_band(const FilterPathDescriptor* p)
{
    path_band_t b;
    b.width = FilterInfo[p->id].width;
    b.centre = p->offset ? p->offset : b.width / 2.0;
    b.rate = p->FIR_I_coeff_file == i_rx_new_coeffs ? 48000.0 / p->sample_rate_dec : 48000.0;
    b.lo = b.centre - b.width / 2.0;
    b.hi = b.centre + b.width / 2.0;
    if (b.lo < 50.0) b.lo = 50.0;
    return b;
}

static double db(double x) { return 20.0 * log10(x + 1e-12); }

typedef struct { double re, im; } cd_t;

static cd_t cmul(cd_t a, cd_t b)
{
    return (cd_t) { a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re };
}

static cd_t cdiv(cd_t a, cd_t b)
{
    const double d = b.re * b.re + b.im * b.im;
    return (cd_t) { (a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d };
}

static double cabs_cd(cd_t a) { return hypot(a.re, a.im); }

// H(f) of the taps the firmware just designed, one complex multiply per tap rather than a cos and a
// sin: the response is the DFT of the impulse response, h[k] * e^{-j w k}.
static cd_t conv_cresp(double f, double rate)
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
    return (cd_t) { re, im };
}

static double conv_resp(double f, double rate)
{
    return cabs_cd(conv_cresp(f, rate));
}

// the same for the pair: |sum u[k] e^{+j w k}| with u = I' + j s Q', s = +1 for the i+q sum
static cd_t stock_cresp(const FilterPathDescriptor* p, double f, double rate, int usb)
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
    return (cd_t) { re, im };
}

static double stock_resp(const FilterPathDescriptor* p, double f, double rate, int usb)
{
    return cabs_cd(stock_cresp(p, f, rate, usb));
}

enum { MAXPTS = 32768 };
static double lv[MAXPTS];

typedef double (*resp_fn)(const FilterPathDescriptor*, double, double, int);

static double resp_conv_wrap(const FilterPathDescriptor* p, double f, double rate, int usb)
{
    (void) p; (void) usb;
    return conv_resp(f, rate);
}

// Response at the band's nominal top edge, relative to the loudest point inside the band.  A stock
// value at or below -6 dB means the table's own cutoff is inside the band the path asks for.
static double edge_db(resp_fn fn, const FilterPathDescriptor* p, double rate, double lo, double hi)
{
    const double step = rate >= 30000.0 ? 1.0 : 0.5;
    double ref = 0.0;
    for (double f = lo; f <= hi; f += step)
    {
        const double v = fn(p, f, rate, 1);
        if (v > ref) ref = v;
    }
    return db(fn(p, hi, rate, 1) / ref);
}

// The three stock shape columns, or n/c when the stock table's own cutoff is inside the path.
static void stock_shape(char* out, size_t n, int valid, double rip, double skirt, double img)
{
    if (valid) snprintf(out, n, "%.2f,%.1f,%.1f", rip, skirt, img);
    else snprintf(out, n, "n/c,n/c,n/c");
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
    // already 60 dB down at the nominal edge there is no skirt to measure; the caller prints n/c when
    // the stock table's -6 dB point is inside the path, rather than scoring that cutoff as the path's
    // skirt.
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

// |H(f)| of an ARM CMSIS lattice IIR from its reflection (pk) and ladder (pv) coefficients.  The
// coefficients are time-reversed ({kN..k1}, {vN..v0}); walking the lattice from stage 1 up avoids
// running a settling transient once per frequency.  Only the magnitude is returned: this direct
// walk has the right magnitude but not the CMSIS lattice's phase, so anything that needs phase (the
// AGC two-tone below) runs the state recursion on the samples instead.
static double iir_mag(const arm_iir_lattice_instance_f32* S, double f, double rate)
{
    if (S == NULL || S->numStages == 0) return 1.0;
    const int N = S->numStages;
    const double w = 2.0 * M_PI * f / rate;
    const cd_t q = { cos(w), -sin(w) };          // z^-1 at z = e^{jw}
    cd_t F = { 1.0, 0.0 }, G = { 1.0, 0.0 };     // G0 = F0
    cd_t Y = { S->pvCoeffs[N], 0.0 };            // v0 * g0

    for (int m = 1; m <= N; m++)
    {
        const double k = S->pkCoeffs[N - m];     // k1, k2, ... kN
        const cd_t qG = cmul(q, G);
        const cd_t Fn = { F.re + k * qG.re, F.im + k * qG.im };
        const cd_t Gn = { k * F.re + qG.re, k * F.im + qG.im };
        F = Fn;
        G = Gn;
        const double v = S->pvCoeffs[N - m];     // v1, v2, ... vN
        Y.re += v * G.re;
        Y.im += v * G.im;
    }
    return cabs_cd(cdiv(Y, F));
}

// The real CMSIS lattice recursion, sample by sample.  The AGC two-tone uses this so the prefilter's
// phase and state are the radio's, not the magnitude-only approximation above.  The widest pre-filter
// the path table ships is 10 stages; 32 leaves room without a per-call allocation.
static void iir_run(const arm_iir_lattice_instance_f32* S, float32_t* x, int n)
{
    if (S == NULL || S->numStages == 0) return;
    const int N = S->numStages;
    double g[32] = { 0.0 };
    for (int i = 0; i < n; i++)
    {
        double f = x[i];
        double acc = 0.0;
        double gnew[32];
        for (int m = 0; m < N; m++)
        {
            const double gp = g[m];
            const double k = S->pkCoeffs[m];
            const double fn = f - k * gp;
            const double gn = gp + k * fn;
            gnew[m] = gn;
            acc += gn * S->pvCoeffs[m];
            f = fn;
        }
        acc += f * S->pvCoeffs[N];
        for (int m = 0; m < N - 1; m++) g[m] = gnew[m + 1];
        g[N - 1] = f;
        x[i] = (float32_t) acc;
    }
}

// The stock or convolution response at f, on the USB (i+q) convention the chain is measured with.
static cd_t side_cresp(const FilterPathDescriptor* p, double f, double rate, int is_conv)
{
    return is_conv ? conv_cresp(f, rate) : stock_cresp(p, f, rate, 1);
}

// The path-specific post-demod chain magnitude: the stock or convolution response, then the pre-AGC
// IIR and the post-AGC anti-alias IIR.  iir_mag() is 1 for a path without one, so the 5.0-7.5 kHz
// paths fall through unchanged.  The common decimation and interpolation FIRs are left out; they
// multiply both sides equally.
static double chain_resp(const FilterPathDescriptor* p, double f, double base_rate, int is_conv)
{
    double h = cabs_cd(side_cresp(p, f, base_rate, is_conv));
    h *= iir_mag(p->pre_instance, f, 48000.0 / p->sample_rate_dec);
    h *= iir_mag(p->iir_instance, f, 48000.0);
    return h;
}

static int chain_conv;

static double chain_wrap(const FilterPathDescriptor* p, double f, double rate, int usb)
{
    (void) usb;
    return chain_resp(p, f, rate, chain_conv);
}

static void chain_metrics(const FilterPathDescriptor* p, double rate, double lo, double hi,
                          int is_conv, double* ripple, double* skirt, double* image, double* edge)
{
    chain_conv = is_conv;
    metrics(chain_wrap, p, rate, lo, hi, 1, ripple, skirt, image);
    *edge = edge_db(chain_wrap, p, rate, lo, hi);
}

static double chain_top_rel(const FilterPathDescriptor* p, double rate, double fref, double ftop,
                            int is_conv)
{
    const double ref = chain_resp(p, fref, rate, is_conv);
    const double top = chain_resp(p, ftop, rate, is_conv);
    return db(top / ref);
}

// Factory AGC defaults, as the digi bench states them before AudioAgc_AgcWdsp_Init() reads the
// configuration the radio normally loads from EEPROM.
static void agc_setup(void)
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
}

// The bench level used by the first chain table and a station 20 dB weaker.  The AGC is level
// dependent, so the weaker pass is a check that its gain does not change the relative shape.
static const double AGC_TONE_LEVEL_NOMINAL = 0.05;
static const double AGC_TONE_LEVEL_WEAK = 0.005;

// Two tones at the path's audio rate: an equal-amplitude reference in the passband and one at the
// top edge, each carrying the stock/convolution filter's own image as well as its wanted response.
// The AGC is the real firmware AGC; the anti-alias IIR after it is applied to each measured tone.
// The result is the top-edge level relative to the reference, i.e. the relative shape the operator
// hears after the AGC has settled.  `level` is the two-tone amplitude before the path filter.
// `ref_db`, when asked for, is the reference tone's absolute level after the AGC, so a sweep can show
// that the AGC really did hold its output rather than run out of gain at the weak end.
static double agc_top_rel(const FilterPathDescriptor* p, double base_rate, double fref, double ftop,
                          int is_conv, double level, double* ref_db)
{
    const double arate = 48000.0 / p->sample_rate_dec;
    const int n = (int) (arate * 2.5);
    const int start = (int) (arate * 1.0);
    const int meas = (int) arate;
    if (ref_db != NULL) *ref_db = 0.0;
    float32_t* buf = calloc((size_t) n, sizeof(float32_t));
    if (buf == NULL) return 0.0;

    const cd_t hr = side_cresp(p, fref, base_rate, is_conv);
    const cd_t hri = side_cresp(p, -fref, base_rate, is_conv);
    const cd_t ht = side_cresp(p, ftop, base_rate, is_conv);
    const cd_t hti = side_cresp(p, -ftop, base_rate, is_conv);

    for (int i = 0; i < n; i++)
    {
        const double wr = 2.0 * M_PI * fref * i / arate;
        const double wt = 2.0 * M_PI * ftop * i / arate;
        // each tone at half the level; the AGC's own target is 4096 ADC counts
        const double x = 0.5 * level *
            (hr.re * cos(wr) - hr.im * sin(wr) + hri.re * cos(wr) + hri.im * sin(wr) +
             ht.re * cos(wt) - ht.im * sin(wt) + hti.re * cos(wt) + hti.im * sin(wt));
        buf[i] = (float32_t) x;
    }
    iir_run(p->pre_instance, buf, n);

    AudioAgc_ResetAgcWdsp();
    AudioAgc_SetupAgcWdsp((float32_t) arate, false);
    for (int i = 0; i < n; i += AUDIO_BLOCK_SIZE)
    {
        const int blk = (n - i) < AUDIO_BLOCK_SIZE ? (n - i) : AUDIO_BLOCK_SIZE;
        for (int k = 0; k < blk; k++) buf[i + k] *= 4096.0f;
        AudioAgc_RunAgcWdsp((int16_t) blk,
            (float32_t (*)[AUDIO_BLOCK_SIZE]) (void*) (buf + i), false);
        for (int k = 0; k < blk; k++) buf[i + k] /= 4096.0f;
    }

    double sr = 0.0, si = 0.0, st = 0.0, ti = 0.0;
    for (int i = start; i < start + meas; i++)
    {
        const double wr = 2.0 * M_PI * fref * i / arate;
        const double wt = 2.0 * M_PI * ftop * i / arate;
        sr += buf[i] * cos(wr);
        si -= buf[i] * sin(wr);
        st += buf[i] * cos(wt);
        ti -= buf[i] * sin(wt);
    }
    const double ar = 2.0 * hypot(sr, si) / meas;
    const double at = 2.0 * hypot(st, ti) / meas;
    free(buf);
    if (ref_db != NULL) *ref_db = db(ar);

    return db((at * iir_mag(p->iir_instance, ftop, 48000.0))
              / (ar * iir_mag(p->iir_instance, fref, 48000.0)));
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

// The level sweep behind --chain-levels.  Two dB steps put 0.05, 0.005 and 0.0005 exactly on the grid
// (0, -20 and -40 dB), so the existing weak column is one of the rows rather than a nearby point, and
// +20 dB is a two-tone whose peak reaches the AGC's max_input (4096 counts).
static const double AGC_LEVEL_STEP_DB = 2.0;
static const double AGC_LEVEL_ABOVE_DB = 20.0;
static const double AGC_LEVEL_BELOW_DB = 40.0;

static int chain_levels_main(void)
{
    agc_setup();
    printf("id,name,level_db,level,stock_agc_top,conv_agc_top,stock_deficit,stock_agc_ref\n");

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if (!is_ssb_cw_path(p)) continue;

        const path_band_t b = path_band(p);
        setup(p, 1, (float32_t) b.rate);
        const double fref = floor((b.lo + b.hi) / 2.0);

        for (double rel = AGC_LEVEL_ABOVE_DB; rel >= -AGC_LEVEL_BELOW_DB; rel -= AGC_LEVEL_STEP_DB)
        {
            const double level = AGC_TONE_LEVEL_NOMINAL * pow(10.0, rel / 20.0);
            double ref;
            const double s = agc_top_rel(p, b.rate, fref, b.hi, 0, level, &ref);
            const double c = agc_top_rel(p, b.rate, fref, b.hi, 1, level, NULL);
            printf("%d,%s,%.0f,%.6g,%.3f,%.3f,%.3f,%.2f\n",
                   id, p->name, rel, level, s, c, c - s, ref);
        }
    }
    return 0;
}

static int chain_main(void)
{
    agc_setup();
    printf("id,name,width,rate,audio_rate,pre_stages,aa_stages,"
           "stock_chain_edge,stock_chain_ripple,stock_chain_skirt,stock_chain_img,"
           "conv_chain_edge,conv_chain_ripple,conv_chain_skirt,conv_chain_img,"
           "stock_top_rel,conv_top_rel,stock_agc_top,conv_agc_top,"
           "stock_agc_top_weak,conv_agc_top_weak\n");

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if (!is_ssb_cw_path(p)) continue;

        const double audio_rate = 48000.0 / p->sample_rate_dec;
        const path_band_t b = path_band(p);
        setup(p, 1, (float32_t) b.rate);

        double s_edge, s_rip, s_skirt, s_img;
        double c_edge, c_rip, c_skirt, c_img;
        chain_metrics(p, b.rate, b.lo, b.hi, 0, &s_rip, &s_skirt, &s_img, &s_edge);
        chain_metrics(p, b.rate, b.lo, b.hi, 1, &c_rip, &c_skirt, &c_img, &c_edge);

        // The chain cannot make the stock table's cutoff comparable: withhold the same three shape
        // columns the main table withholds.  The edge and the AGC top level stay, because they are
        // the diagnostic that says how far down the operator's top edge is.
        const int stock_shape_valid = edge_db(stock_resp, p, b.rate, b.lo, b.hi) > -6.0;
        char s_shape[48], c_skirt_s[16];
        stock_shape(s_shape, sizeof s_shape, stock_shape_valid, s_rip, s_skirt, s_img);
        if (c_skirt < 0.0) snprintf(c_skirt_s, sizeof c_skirt_s, "-");
        else snprintf(c_skirt_s, sizeof c_skirt_s, "%.1f", c_skirt);

        const double fref = floor((b.lo + b.hi) / 2.0);
        const double s_top = chain_top_rel(p, b.rate, fref, b.hi, 0);
        const double c_top = chain_top_rel(p, b.rate, fref, b.hi, 1);
        const double s_agc = agc_top_rel(p, b.rate, fref, b.hi, 0, AGC_TONE_LEVEL_NOMINAL, NULL);
        const double c_agc = agc_top_rel(p, b.rate, fref, b.hi, 1, AGC_TONE_LEVEL_NOMINAL, NULL);
        const double s_agc_weak = agc_top_rel(p, b.rate, fref, b.hi, 0, AGC_TONE_LEVEL_WEAK, NULL);
        const double c_agc_weak = agc_top_rel(p, b.rate, fref, b.hi, 1, AGC_TONE_LEVEL_WEAK, NULL);

        printf("%d,%s,%.0f,%.0f,%.0f,%d,%d,%.2f,%s,%.2f,%.2f,%s,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
               id, p->name, b.width, b.rate, audio_rate,
               p->pre_instance ? p->pre_instance->numStages : 0,
               p->iir_instance ? p->iir_instance->numStages : 0,
               s_edge, s_shape,
               c_edge, c_rip, c_skirt_s, c_img,
               s_top, c_top, s_agc, c_agc, s_agc_weak, c_agc_weak);
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--chain-levels") == 0) return chain_levels_main();
    if (argc > 1 && strcmp(argv[1], "--chain") == 0) return chain_main();

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
           "stock_ripple,stock_skirt,stock_img,stock_edge_db,conv_ripple,conv_skirt,conv_img\n");

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if (!is_ssb_cw_path(p)) continue;

        // the rate the blocks carry: the pair runs at 48 kHz unless this path is one of the decimated
        // ones, which is the radio's own test in AudioDriver_RxProcessor() and in the setup above it
        const path_band_t b = path_band(p);
        const double stock_edge = edge_db(stock_resp, p, b.rate, b.lo, b.hi);

        setup(p, 1, (float32_t) b.rate);

        // A stock table whose -6 dB point is inside the path's band is measuring its own cutoff at
        // the top edge, not the path; the post-demod IIR cannot restore what the table removed.  Mark
        // those shape columns n/c rather than scoring the cutoff.  Their cost stays comparable.
        const int stock_shape_valid = stock_edge > -6.0;

        double s_rip = 0.0, s_skirt = 0.0, s_img = 0.0, c_rip, c_skirt, c_img;
        if (stock_shape_valid)
        {
            metrics(stock_resp, p, b.rate, b.lo, b.hi, 1, &s_rip, &s_skirt, &s_img);
        }
        metrics(resp_conv_wrap, p, b.rate, b.lo, b.hi, 1, &c_rip, &c_skirt, &c_img);

        char s_shape[48];
        stock_shape(s_shape, sizeof s_shape, stock_shape_valid, s_rip, s_skirt, s_img);

        const double s_ops = 4.0 * p->FIR_numTaps;      // 4 per tap: two multiplies and two adds, both channels
        const double c_ops = b.rate <= 12000.0 ? conv_ops[0] : (b.rate <= 24000.0 ? conv_ops[1] : conv_ops[2]);
        const double s_ops_s = s_ops * b.rate, c_ops_s = c_ops * b.rate;
        // 1.5 cycles per counted operation at 168 MHz, the model the README's CPU column uses
        const double s_pct = s_ops_s * 1.5 / 168e6 * 100.0, c_pct = c_ops_s * 1.5 / 168e6 * 100.0;

        printf("%d,%s,%.0f,%.0f,%.0f,%d,%.0f,%.0f,%.2f,%.0f,%.0f,%.2f,"
               "%s,%.2f,%.2f,%.1f,%.1f\n",
               id, p->name, b.width, b.centre, b.rate, p->FIR_numTaps,
               s_ops, s_ops_s, s_pct, c_ops, c_ops_s, c_pct,
               s_shape, stock_edge, c_rip, c_skirt, c_img);
    }
    return 0;
}
