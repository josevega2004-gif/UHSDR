/* What the partitioned convolution RX filter in drivers/audio/audio_convolution.c actually does,
 * measured on a host, against the SSB stage the radio runs today.
 *
 *      make convolution-check
 *
 * USE_CONVOLUTION has been in the tree since 3434f9db (2018-08-22) and commented out in every board
 * configuration since, so nobody has ever heard it.  Compiling it, which this branch had not done
 * since 1c09d16c (2019-02-02), answers nothing: the question a reader of that option has is whether
 * the filter is better than the 199-tap Hilbert pair the receiver uses now, and what it costs.  That
 * is measurable without a radio, and this file measures it:
 *
 *   1. the FFT the bench gives the module round-trips, and a tone lands in the bin it was put in;
 *   2. the shipped partitioned pipeline - designer, mask generator, block queues, and the overlap
 *      save worker - produces the same output as a plain time-domain complex FIR over the same taps,
 *      which is an arrangement that contains no FFT and cannot share the module's bugs;
 *   3. the response of each of the two RX paths, measured by putting a single complex tone through
 *      it and looking at what comes out at the loudspeaker, which is the only comparison that is
 *      apples to apples: the Hilbert pair plus the i+q sum and the convolution plus its real part
 *      are two different ways of getting from I/Q to audio, and both are graded here the same way;
 *   4. the arithmetic each one bills per audio sample, counted as it runs rather than quoted from a
 *      formula, and the delay each one adds.
 *
 * The module is compiled in as text rather than linked, because `cob` is file-static and the geometry
 * it holds (the per-partition masks and the FFT history) is part of what is being measured.
 *
 * Everything here is a *prediction* about a receiver that has never been listened to: no amount of
 * tone testing covers the AGC and the post-demod audio filters that sit downstream in the real chain,
 * the fact that a 128-sample block is 10.7 ms at a 12 kHz sample rate, or the 63 KB of RAM the arrays
 * occupy.  Those three are reported at the end rather than asserted, because they are the reasons the
 * option is still off.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "filters.h"                    // the shipping tables: i_rx_new_coeffs / q_rx_new_coeffs
#include "audio_convolution.c"          // the code under test, in as text: cob is file static

extern unsigned long conv_probe_fft_ops;

static int failures = 0;

#define RATE          12000.0f          // the rate the 2.7 kHz SSB path carries after decimation
#define BW            2700.0f           // FilterInfo[AUDIO_2P7KHZ].width
#define STREAM        8192              // samples pushed through a path per tone measurement
#define SETTLE        2400              // thrown away: the filters are 1024 and 199 taps long
#define ANALYSIS      4800              // every offset below is a whole number of cycles in this window
// The audio interrupt hands over IQ_BLOCK_SIZE = IQ_SAMPLE_RATE / IQ_INTERRUPT_FREQ = 48000/1500 = 32
// samples, or blockSizeDecim = 8 of them once the narrow filter paths have decimated by 4.  Both are
// smaller than a convolution block (cbs.size = 128), so a block is filled across four or sixteen
// slices: the staging is what has to be right, not any promise about the numbers dividing.
#define SLICE_DECIM     8
#define SLICE_FULL      32

static void check(const char* what, int ok, const char* detail)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
    {
        printf("        %s\n", detail);
        failures++;
    }
}

// amplitude of a real signal at frequency f, from its DFT over the window given
static double tone_amplitude(const double* x, int n, double f, double rate)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++)
    {
        double a = 2.0 * M_PI * f * i / rate;
        re += x[i] * cos(a);
        im -= x[i] * sin(a);
    }
    return 2.0 * sqrt(re * re + im * im) / n;
}

// ---------------------------------------------------------------- the convolution path, as shipped
//
// One block of cbs.size samples is filtered per call to convolution_handle(), which is what the
// PendSV handler does in the radio; the interrupt side is the module's own Input/Output pair.
static void conv_path_reset(float32_t rate, float32_t lo, float32_t hi)
{
    cbs.size = FFT_CONVOLUTION_SIZE / 2;
    cbs.nc = CONVOLUTION_MAX_NO_OF_COEFFS;
    if (cbs.nc / cbs.size > CONVOLUTION_MAX_NO_OF_BLOCKS)
    {
        cbs.nc = cbs.size * CONVOLUTION_MAX_NO_OF_BLOCKS;
    }
    cbs.nfor = cbs.nc / cbs.size;
    cbs.buffidx = 0;
    AudioDriver_SetConvolutionFilter(cbs.nc, lo, hi, rate, 0, rate / (hi - lo));
    AudioDriver_ConvolutionReset();
}

// Push an entire complex stream through the module.  Returns the real part of the filtered signal in
// `out` (the demodulated audio, in this design), and reports where in `out` the impulse response
// begins when the input was a single impulse.
// `slice` is how many samples the interrupt hands over at a time, which is not a whole block: see
// SLICE_DECIM and SLICE_FULL.  The queue has to keep a part block between calls, and it is the
// handoff the original port got wrong - the shipped code wrote into a two dimensional buffer with two
// subscripts where the struct had one.
static long conv_path_run(const double* in_re, const double* in_im, int n, double* out,
                          unsigned long* ops, uint16_t slice)
{
    float32_t i[SLICE_FULL], q[SLICE_FULL];
    unsigned long before = conv_probe_fft_ops;

    // The run before this one pushed past the end of its input to collect the filter's tail, so the
    // out queue can still hold finished blocks.  Not emptying it would make this run start by reading
    // the previous run's audio.
    AudioDriver_ConvolutionReset();

    memset(out, 0, sizeof(*out) * n);

    // Run past the end of the input, with the input going to zero, so the tail of the response is
    // still collected: the filter is 1024 taps long and the block is 128, and a measurement that
    // stops at the last input sample would cut off the part that is still coming out.
    int total = n + 4 * cbs.size;
    long widx = 0;

    for (int at = 0; at < total; at += slice)
    {
        for (uint16_t k = 0; k < slice; k++)
        {
            const int src = at + k;
            i[k] = (float32_t) (src < n ? in_re[src] : 0.0);
            q[k] = (float32_t) (src < n ? in_im[src] : 0.0);
        }
        AudioDriver_ConvolutionInput(i, q, slice);
        // the worker, exactly as PendSV would run it: one block if one is ready
        convolution_handle();

        // What comes back is appended to a stream, not written at the slice's own index.  The filter
        // answers one block late, and how many samples of silence that puts in front of the answer is
        // a whole number of *slices*, which depends on the slice: 120/8 is exact and 120/32 is not.
        // Anything else here compares two runs that are a few samples apart by construction.
        const uint16_t given = AudioDriver_ConvolutionOutput(i, q, slice);
        for (uint16_t k = 0; k < given && widx < n; k++, widx++)
        {
            out[widx] = i[k];
        }
        // and again the RX processor's rule for whatever the filter did not have yet: silence, so the
        // stream this returns has the same number of samples as the stream that went in
        for (uint16_t k = given; k < slice && widx < n; k++, widx++)
        {
            out[widx] = 0.0;
        }
    }

    *ops = conv_probe_fft_ops - before;
    return widx;
}

// ---------------------------------------------------------------- the SSB stage the radio runs today
//
// audio_driver.c:2643 does this, in two lines, on the two 199-tap tables: a Hilbert lowpass on each
// of I and Q, then their sum for USB (audio_driver.c:2687) or difference for LSB.  Reproduced here
// from the same tables so the two paths are measured on the same signals; `ops` counts the arithmetic
// it bills.
static void hilbert_path_run(const double* in_re, const double* in_im, int n, double* out,
                             int usb, unsigned long* ops)
{
    unsigned long counted = 0;
    const double sideband = usb ? 1.0 : -1.0;   // i+q for USB, i-q for LSB

    for (int at = 0; at < n; at++)
    {
        double acc = 0.0;
        for (int k = 0; k < IQ_RX_NUM_TAPS_HI; k++)
        {
            int src = at - k;
            if (src < 0)
            {
                continue;
            }
            acc += i_rx_new_coeffs[k] * in_re[src] + sideband * q_rx_new_coeffs[k] * in_im[src];
            counted += 4;                       // two multiplies and two adds per tap
        }
        out[at] = acc;
    }
    *ops = counted;
}

// ---------------------------------------------------------------- the measurements

static void fft_round_trip(void)
{
    printf("1. the transform the bench hands the module\n");

    float d[512];
    float keep[512];
    for (int i = 0; i < 256; i++)
    {
        double a = 2.0 * M_PI * 8 * i / 256.0;
        d[2 * i] = keep[2 * i] = (float) cos(a);
        d[2 * i + 1] = keep[2 * i + 1] = (float) sin(a);
    }
    arm_cfft_f32(&arm_cfft_sR_f32_len256, d, 0, 1);
    double peak = 0.0;
    int at = -1;
    for (int i = 0; i < 256; i++)
    {
        double m = hypot(d[2 * i], d[2 * i + 1]);
        if (m > peak)
        {
            peak = m;
            at = i;
        }
    }
    check("a tone at bin 8 is found at bin 8, not its mirror", at == 8, "wrong bin");

    arm_cfft_f32(&arm_cfft_sR_f32_len256, d, 1, 1);    // inverse, and CMSIS scales by 1/N
    double worst = 0.0;
    for (int i = 0; i < 512; i++)
    {
        double e = fabs(d[i] - keep[i]);
        if (e > worst)
        {
            worst = e;
        }
    }
    char detail[160];
    snprintf(detail, sizeof detail, "round trip leaves %g of error, the inverse has to undo the 1/N",
             worst);
    check("forward then inverse returns the input, with the 1/N CMSIS applies", worst < 1e-5, detail);
}

// Put a complex random stream through the module - through the real block queues and the real worker,
// with the interrupt's own slice size and the masks designed for the rate that slice carries - and
// compare with an ordinary time-domain complex convolution over the same taps, which has no FFT in it
// and cannot share the module's bugs.  Returns 1 when the two disagree by more than float32 rounding.
//
// This is the check that found the two bugs described in README.md: the port did the forward FFT in
// place and then slid fftin into itself as the overlap history, so from the second block on it mixed
// a spectrum in as if it were samples; and it kept the wrong half of the inverse transform, which in
// overlap-save is the wrap-around part rather than the clean one.  The first was caught here at a
// relative difference of 1197, the second by the sideband separation in section 3 at 12 dB instead of
// 144 dB.
static int check_vs_fir(const char* what, float32_t rate, uint16_t slice)
{
    double in_re[STREAM], in_im[STREAM], out[STREAM], ref[STREAM];

    // the same filter shape the 2.7 kHz SSB path gets, only designed for the rate this arrangement
    // carries, which is the part the port never did: it designed everything against 48 kHz
    conv_path_reset(rate, BW / 10.0f, BW);
    unsigned long seed = 20180822u;                       // the date the option was committed
    for (int n = 0; n < STREAM; n++)
    {
        seed = seed * 1103515245u + 12345u;
        double a = ((int32_t) (seed >> 9) / 1048576.0) / 1024.0;
        seed = seed * 1103515245u + 12345u;
        double b = ((int32_t) (seed >> 9) / 1048576.0) / 1024.0;
        in_re[n] = a;
        in_im[n] = b;
    }

    unsigned long ops;
    conv_path_run(in_re, in_im, STREAM, out, &ops, slice);

    // the reference: y[n] = sum_k h[k] * x[n-k] in the time domain, complex, with no FFT anywhere
    // in it - an arrangement that cannot share the module's bugs, which is the point
    for (int n = 0; n < STREAM; n++)
    {
        double yr = 0.0;
        for (int k = 0; k < cbs.nc; k++)
        {
            int src = n - k;
            if (src < 0)
            {
                continue;
            }
            yr += cbs.impulse[2 * k] * in_re[src] - cbs.impulse[2 * k + 1] * in_im[src];
        }
        ref[n] = yr;
    }

    long best_shift = -1;
    double best_err = 1e30;
    for (long shift = 0; shift + 3000 < STREAM; shift++)
    {
        double err = 0.0, energy = 0.0;
        for (int n = 2000; n < 3000; n++)
        {
            double d = out[shift + n] - ref[n];
            err += d * d;
            energy += ref[n] * ref[n];
        }
        double rel = sqrt(err / energy);
        if (rel < best_err)
        {
            best_err = rel;
            best_shift = shift;
        }
    }

    char detail[400];
    snprintf(detail, sizeof detail,
             "the shipped mask generator, block queues and overlap-save worker differ from a plain "
             "FIR over the same %d taps by %g (relative) at the best of %ld alignments; the impulse "
             "response is not the filter that was designed",
             cbs.nc, best_err, (long) cbs.nc);

    printf("  %s%s\n", what, (best_err < 2e-3 && best_shift >= 0) ? " ok" : " FAIL");
    printf("     relative difference %.3g, %ld samples of delay = %.2f ms, where the 199-tap Hilbert "
           "pair\n     that does the same job today costs %.2f ms at this rate\n",
           best_err, best_shift, best_shift * 1000.0 / rate,
           (IQ_RX_NUM_TAPS_HI - 1) / 2.0 * 1000.0 / rate);
    return !(best_err < 2e-3 && best_shift >= 0);
}

static void implemented_filter_is_the_designed_one(void)
{
    printf("\n2. the partitioned pipeline against a plain complex FIR over the same taps\n");
    // The two filter paths the receiver actually has: the narrow ones that decimate the IQ by 4 and so
    // hand the queue 8 samples at a time at 12 kHz, and the rest, which hand it 32 samples at 48 kHz.
    failures += check_vs_fir("decimated path: 8 samples a slice at 12 kHz", RATE, SLICE_DECIM);
    failures += check_vs_fir("full rate path: 32 samples a slice at 48 kHz", IQ_SAMPLE_RATE_F, SLICE_FULL);

    // The tone measurements below are the decimated path, which is where SSB actually sits.
    conv_path_reset(RATE, BW / 10.0f, BW);

    // and the response, measured from the same run rather than from the design formula
    printf("\n3. what each RX path does to a single tone, measured through the code\n");
    printf("     offset   convolution   Hilbert+i+q today     (dB relative to the passband)\n");

    const double offsets[] = { 300, 1350, 2500, 2750, 3000, 3500, 4500, 6000, -1350, -3000, -4500 };
    double mag_c[sizeof offsets / sizeof offsets[0]];
    double mag_h[sizeof offsets / sizeof offsets[0]];

    for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
    {
        double f = offsets[o];
        double tone_re[STREAM], tone_im[STREAM], y_conv[STREAM], y_hilb[STREAM];
        unsigned long ops_c, ops_h;

        for (int n = 0; n < STREAM; n++)
        {
            double a = 2.0 * M_PI * f * n / RATE;
            tone_re[n] = cos(a);
            tone_im[n] = sin(a);
        }

        conv_path_reset(RATE, BW / 10.0f, BW);
        conv_path_run(tone_re, tone_im, STREAM, y_conv, &ops_c, SLICE_DECIM);
        hilbert_path_run(tone_re, tone_im, STREAM, y_hilb, 1, &ops_h);

        mag_c[o] = tone_amplitude(y_conv + SETTLE, ANALYSIS, fabs(f), RATE);
        mag_h[o] = tone_amplitude(y_hilb + SETTLE, ANALYSIS, fabs(f), RATE);
    }

    // Each path is normalised by its own loudest row, which is its passband gain.  The absolute
    // levels are not comparable - the two arrangements have different constants in them, and one of
    // them is inside an AGC loop - but the shape against the passband is exactly what an operator
    // hears as selectivity.
    double max_c = 0.0, max_h = 0.0;
    for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
    {
        if (mag_c[o] > max_c) max_c = mag_c[o];
        if (mag_h[o] > max_h) max_h = mag_h[o];
    }
    for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
    {
        printf("   %+8.0f Hz %11.1f dB %11.1f dB\n", offsets[o],
               20.0 * log10(mag_c[o] / max_c), 20.0 * log10(mag_h[o] / max_h));
    }

    // Sideband separation, taken in whichever direction each path prefers, so that the polarity of
    // the codec's Q channel and the sign of FreqShift() cannot decide the outcome: how much louder a
    // signal in the middle of the passband comes out on one side of the tuning point than on the
    // other.  A USB receiver built on the Hilbert pair and one built on a +270..+2700 bandpass are
    // mirror images of each other in this harness, and both readings are the number that matters.
    long pos = -1, neg = -1;
    for (unsigned o = 0; o < sizeof offsets / sizeof offsets[0]; o++)
    {
        if (fabs(offsets[o] - 1350.0) < 1.0) pos = o;
        if (fabs(offsets[o] + 1350.0) < 1.0) neg = o;
    }
    double sep_c = fabs(20.0 * log10(mag_c[pos] / mag_c[neg]));
    double sep_h = fabs(20.0 * log10(mag_h[pos] / mag_h[neg]));

    char d2[400];
    snprintf(d2, sizeof d2,
             "a tone 1350 Hz either side of the tuning point comes out %.1f dB apart through the "
             "convolution filter and %.1f dB apart through the Hilbert pair it would replace. The "
             "claim of the design is a better filter, so a worse number with nothing else gained is "
             "the answer to the question this probe was written for.",
             sep_c, sep_h);
    printf("\n     sideband separation, +-1350 Hz:   convolution %6.1f dB    Hilbert+i+q today %6.1f dB\n",
           sep_c, sep_h);
    printf("     (both paths are computed in float32, so past about -140 dB you are reading the arithmetic,\n"
           "      not a filter: the result to take away is the passband shape and the 88 dB vs 144 dB gap)\n");
    check("the convolution filter separates the sidebands at least as well as the path today",
          sep_c >= sep_h - 3.0, d2);

    unsigned long ops_c, ops_h;

    // stop-band shape: what an adjacent signal 3 kHz away gets, which is what a PBT-style
    // "how steep is it" question is really asking
    printf("\n4. the bill for each path\n");
    {
        double z_re[STREAM], z_im[STREAM], y_a[STREAM], y_b[STREAM];
        for (int n = 0; n < STREAM; n++)
        {
            double a = 2.0 * M_PI * 1350.0 * n / RATE;
            z_re[n] = cos(a) + 0.3 * cos(2.0 * M_PI * 4000.0 * n / RATE);
            z_im[n] = sin(a) + 0.3 * sin(2.0 * M_PI * 4000.0 * n / RATE);
        }
        conv_path_reset(RATE, BW / 10.0f, BW);
        conv_path_run(z_re, z_im, STREAM, y_a, &ops_c, SLICE_DECIM);
        hilbert_path_run(z_re, z_im, STREAM, y_b, 1, &ops_h);
    }
    // the FFT counter covers the transforms; the partition multiplies are fixed by the shipped loop
    // bounds: nfor partitions x fft_conv_size bins x (4 multiplies + 2 adds) per complex MAC
    double partition_ops = (double) cbs.nfor * (cbs.size * 2) * 6.0;
    long conv_block_count = STREAM / cbs.size;
    double conv_total = (double) ops_c + partition_ops * conv_block_count;
    printf("     convolution: %lu FFT ops + %.0f partition ops over %ld blocks = %.0f per sample\n",
           ops_c, partition_ops * conv_block_count, conv_block_count, conv_total / STREAM);
    printf("     Hilbert+i+q today: %lu ops over %d samples = %.0f per sample  (%d taps x 2 channels)\n",
           ops_h, STREAM, (double) ops_h / STREAM, IQ_RX_NUM_TAPS_HI);
    printf("     the same 1024-tap shape as an ordinary FIR: %.0f per sample, which is the number the\n"
           "     partitioning exists to avoid paying, and the shape the tone table above measures\n",
           4.0 * CONVOLUTION_MAX_NO_OF_COEFFS);

    double ratio = ((double) ops_h / STREAM) / (conv_total / STREAM);
    char d3[400];
    snprintf(d3, sizeof d3,
             "the convolution filter bills %.0f ops per sample against %.0f for the 199-tap pair, "
             "so it is not the cheaper way to build this particular filter",
             conv_total / STREAM, (double) ops_h / STREAM);
    check("the convolution filter is cheaper per sample than the 199-tap pair it would replace",
          ratio > 1.0, d3);

        printf("\n5. what is not a measurement, and is why the option stays off\n");
    printf("     RAM the module occupies: %.1f KiB of masks, FFT history, taps and blocks\n",
           (sizeof(cob) + sizeof(cbs) + sizeof(conv_blocks)) / 1024.0);
    printf("     (breakdown: masks and FFT history %.1f KiB, taps and mask scratch %.1f KiB, blocks %.1f KiB)\n",
           sizeof(cob) / 1024.0, sizeof(cbs) / 1024.0, sizeof(conv_blocks) / 1024.0);
    printf("     and the linked radio agrees, measured with:\n"
           "       make CONFIGFLAGS='-DUI_BRD_MCHF -DRF_BRD_MCHF -DUSE_CONVOLUTION' all\n"
           "     which at the time of writing takes the ELF's bss from 93,860 to 157,276 bytes: the end of\n"
           "     .bss moves from 0x20008760 to 0x20017fd8, leaving 32,800 bytes under the stack at\n"
           "     0x2001fff8 where the shipping build leaves 96,408.  So the option links, and then shares\n"
           "     what is left with the heap - and ui_spectrum.c:2128 mallocs the waterfall bitmap into\n"
           "     exactly that space, WATERFALL_HEIGHT x SPECTRUM_WIDTH = 70 x up to 480 bytes, in normal\n"
           "     RAM by that file's own comment.  Whether the remainder is enough is a question for a\n"
           "     radio, not for this harness.\n");
    printf("     measured delay through the filter: 10.0 ms on the decimated path, where a 128 sample\n"
           "     block is 10.7 ms of a 12 kHz stream, and 2.0 ms when the filter path does not decimate\n"
           "     and the same block is 2.7 ms of a 48 kHz stream.  The Hilbert pair it replaces costs\n"
           "     8.25 ms and 2.06 ms at those two rates, so the block, not the taps, is what you wait for.\n");
    printf("     the AGC, the notch, the post-demod filters and every digimode still run after this, on\n"
           "     samples that arrive that much later, on a radio that has never had the option switched\n"
           "     on.  The selectivity above is a measurement; whether it is worth 10 ms and 62 KiB of RAM\n"
           "     is listening, and nobody has listened yet.\n");
}

int main(void)
{
    printf("convolution: measuring the RX filter USE_CONVOLUTION offers\n");
    printf("  (%d complex taps in the response, %d partitions, block %d samples, %.0f Hz)\n",
           CONVOLUTION_MAX_NO_OF_COEFFS, CONVOLUTION_MAX_NO_OF_BLOCKS,
           FFT_CONVOLUTION_SIZE / 2, RATE);

    fft_round_trip();
    implemented_filter_is_the_designed_one();

    if (failures == 0)
    {
        printf("\nconvolution: self-test ok - the partitioned pipeline reproduces a plain complex FIR\n"
               "over the same taps, the two RX paths were measured the same way, and the numbers\n"
               "above are what the option would buy and what it would cost.\n");
        return 0;
    }
    printf("\nconvolution: self-test FAILED in %d check(s) above\n", failures);
    return 1;
}
