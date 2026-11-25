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
 *   4. that the design selects the sideband the sum in use today selects, both ways round, because an
 *      inverted sideband is not a subtle defect on the air;
 *   5. the arithmetic each one bills per audio sample, counted as it runs rather than quoted from a
 *      formula, and the delay each one adds;
 *   6. every SSB and CW filter path the radio has, taken from the radio's own table: a tone at the
 *      centre of each path has to come out of the convolution filter at the level the path in use
 *      delivers, on the same sideband.  Sections 2 to 5 measure one path, the 2.7 kHz one; this is
 *      what covers the other sixty;
 *   7. how late the audio is: the time from a tone being switched on to its audio reaching half
 *      level, which is the block's buffering and the delay of the taps together.  The taps are
 *      minimum phase, and this is where that is held to costing less than linear phase ones would.
 *   8. that a filter change leaves nothing in the pipeline behind - and that a worker pass landing in
 *      the middle of a design cannot corrupt the masks, which is the condition that lets the design
 *      borrow the worker's buffers as its working store in the first place;
 *   9. what the low edge of the band is for: DC has to be well down on every path whose band reaches
 *      0 Hz, both absolutely and against the Hilbert pair the radio runs today.
 *
 * The geometry is a build choice (see audio_convolution.h), so this file prints nothing that is not
 * derived from the -D flags it was compiled with: pass them to make as
 * EXTRA_DEFS='-DFFT_CONVOLUTION_SIZE=64 -DCONVOLUTION_MAX_NO_OF_COEFFS=512' and the tables, the delay
 * and the RAM below are that variant's.  The Makefile keeps a stamp of the flags for exactly that
 * reason: a measurement taken from an object built with yesterday's geometry is worse than none.
 *
 * The module is compiled in as text rather than linked, because `cob` is file-static and the geometry
 * it holds (the per-partition masks and the FFT history) is part of what is being measured.
 *
 * Everything here is a *prediction* about a receiver that has never been listened to: no amount of
 * tone testing covers the AGC and the post-demod audio filters that sit downstream in the real chain,
 * the latency one block costs at the rate the filter path carries, or the RAM the arrays occupy.  Those
 * are reported at the end rather than asserted, because they are the reasons the option is still off.
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
#define BW_OFFSET     1350.0f           // .offset of the 2.7 kHz "LPF" path: the middle of 0..2700
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
//
// The setup is the radio's too: AudioDriver_SetProcessingChain() calls this same function with the
// width and offset of the path in use.  This used to be a typed-out copy of that setup, and the copy
// had its own band edges and its own gain.
static void conv_path_reset(float32_t rate, int lsb)
{
    AudioDriver_ConvolutionSetFilterPath(BW, BW_OFFSET, lsb != 0, rate);
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
// AudioDriver_RxProcessor() does this, in two lines, on the path's two tables: a Hilbert lowpass on
// each of I and Q, then their sum for USB or difference for LSB.  Reproduced here from the same tables
// so the two paths are measured on the same signals; `ops` counts the arithmetic it bills.
//
// The tap order is the radio's, and it is not the textbook one: audio_filter.c copies the tables into
// arm_fir_f32() unreversed, and arm_fir_f32() pairs pCoeffs[0] with the OLDEST sample of its window.
// Written as table[k] * x[n-k] this model selects the opposite sideband from the radio, and a
// convolution filter with its sidebands swapped then passes every comparison against it.
static void hilbert_run(const float* ci, const float* cq, int taps,
                        const double* in_re, const double* in_im, int n, double* out,
                        int usb, unsigned long* ops)
{
    unsigned long counted = 0;
    const double sideband = usb ? 1.0 : -1.0;   // i+q for USB, i-q for LSB

    for (int at = 0; at < n; at++)
    {
        double acc = 0.0;
        for (int k = 0; k < taps; k++)
        {
            int src = at - k;
            if (src < 0)
            {
                continue;
            }
            acc += ci[taps - 1 - k] * in_re[src] + sideband * cq[taps - 1 - k] * in_im[src];
            counted += 4;                       // two multiplies and two adds per tap
        }
        out[at] = acc;
    }
    *ops = counted;
}

static void hilbert_path_run(const double* in_re, const double* in_im, int n, double* out,
                             int usb, unsigned long* ops)
{
    hilbert_run(i_rx_new_coeffs, q_rx_new_coeffs, IQ_RX_NUM_TAPS_HI, in_re, in_im, n, out, usb, ops);
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
static double delay_decimated_ms;     // measured, section 2, decimated path
static double delay_fullrate_ms;      // measured, section 2, path that does not decimate

static int check_vs_fir(const char* what, float32_t rate, uint16_t slice, double* delay_ms_out)
{
    double in_re[STREAM], in_im[STREAM], out[STREAM], ref[STREAM];

    // the same filter shape the 2.7 kHz SSB path gets, only designed for the rate this arrangement
    // carries, which is the part the port never did: it designed everything against 48 kHz
    conv_path_reset(rate, 0);
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
    printf("     relative difference %.3g, %ld samples of buffering = %.2f ms, on top of whatever the taps\n"
           "     themselves delay the signal by (section 9)\n",
           best_err, best_shift, best_shift * 1000.0 / rate);
    *delay_ms_out = best_shift * 1000.0 / (double) rate;
    return !(best_err < 2e-3 && best_shift >= 0);
}

static void implemented_filter_is_the_designed_one(void)
{
    printf("\n2. the partitioned pipeline against a plain complex FIR over the same taps\n");
    // The two filter paths the receiver actually has: the narrow ones that decimate the IQ by 4 and so
    // hand the queue 8 samples at a time at 12 kHz, and the rest, which hand it 32 samples at 48 kHz.
    failures += check_vs_fir("decimated path: 8 samples a slice at 12 kHz", RATE, SLICE_DECIM, &delay_decimated_ms);
    failures += check_vs_fir("full rate path: 32 samples a slice at 48 kHz", IQ_SAMPLE_RATE_F, SLICE_FULL, &delay_fullrate_ms);

    // The tone measurements below are the decimated path, which is where SSB actually sits.
    conv_path_reset(RATE, 0);

    // and the response, measured from the same run rather than from the design formula
    printf("\n3. what each RX path does to a single tone, measured through the code\n");
    printf("     offset   convolution   Hilbert+i+q today     (dB relative to the passband)\n");

    const double offsets[] = { 300, 1350, 2500, 2750, 3000, 3500, 4500, 6000, -1350, -3000, -4500 };
    const unsigned NOFF = sizeof offsets / sizeof offsets[0];
    double mag_c[NOFF];
    double mag_h[NOFF];
    // and the same pair turned round: the filter designed on the other side of zero, against the
    // other of the two sums the receiver uses today.  See the check after the table.
    double mag_c_lsb[NOFF];
    double mag_h_lsb[NOFF];

    for (unsigned o = 0; o < NOFF; o++)
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

        conv_path_reset(RATE, 0);
        conv_path_run(tone_re, tone_im, STREAM, y_conv, &ops_c, SLICE_DECIM);
        hilbert_path_run(tone_re, tone_im, STREAM, y_hilb, 1, &ops_h);
        mag_c[o] = tone_amplitude(y_conv + SETTLE, ANALYSIS, fabs(f), RATE);

        // the mirror: the same design below zero, and the i-q sum the receiver uses for LSB
        conv_path_reset(RATE, 1);
        conv_path_run(tone_re, tone_im, STREAM, y_conv, &ops_c, SLICE_DECIM);
        mag_c_lsb[o] = tone_amplitude(y_conv + SETTLE, ANALYSIS, fabs(f), RATE);

        hilbert_path_run(tone_re, tone_im, STREAM, y_hilb, 1, &ops_h);   // i+q, the USB sum
        mag_h[o] = tone_amplitude(y_hilb + SETTLE, ANALYSIS, fabs(f), RATE);
        hilbert_path_run(tone_re, tone_im, STREAM, y_hilb, 0, &ops_h);   // i-q, the LSB sum
        mag_h_lsb[o] = tone_amplitude(y_hilb + SETTLE, ANALYSIS, fabs(f), RATE);
    }

    // Which side of zero each arrangement puts its audio on.  The pairs are the two the seam in
    // AudioDriver_RxProcessor() builds: USB is a positive-frequency bandpass and no sum at all (the
    // real part is the audio), and the maintained path for the same sideband is i+q; LSB is the
    // mirror image and i-q.  If the two columns of a pair disagree about where the passband is, the
    // new path inverts the sideband, which on the air is not a subtle defect.
    {
        // Sum of the (linear) amplitudes on each side of zero, which is what "which sideband does
        // this arrangement listen to" means when the passband covers more than one row of the table:
        // several offsets are in band for the maintained path, so picking the single loudest row only
        // finds whichever of them the arithmetic rounded up.
        double c_pos = 0, c_neg = 0, h_pos = 0, h_neg = 0;
        double cl_pos = 0, cl_neg = 0, hl_pos = 0, hl_neg = 0;
        for (unsigned o = 0; o < NOFF; o++)
        {
            double *pup, *pdn, *pupl, *pdnl;
            if (offsets[o] > 0) { pup = &c_pos; pdn = &h_pos; pupl = &cl_pos; pdnl = &hl_pos; }
            else                { pup = &c_neg; pdn = &h_neg; pupl = &cl_neg; pdnl = &hl_neg; }
            *pup  += mag_c[o];
            *pdn  += mag_h[o];
            *pupl += mag_c_lsb[o];
            *pdnl += mag_h_lsb[o];
        }
        const int c_side  = c_pos > c_neg ? 1 : -1;         // the positive design, convolution
        const int h_side  = h_pos > h_neg ? 1 : -1;         // the same sideband, i+q today
        const int cl_side = cl_pos > cl_neg ? 1 : -1;       // the negative design, convolution
        const int hl_side = hl_pos > hl_neg ? 1 : -1;       // the other sideband, i-q today

        char which[140];
        snprintf(which, sizeof(which), "the new path puts its band %s zero, the sum in use today puts it %s zero",
                 c_side > 0 ? "above" : "below", h_side > 0 ? "above" : "below");
        check("a positive design selects the sideband i+q selects", c_side == h_side, which);
        snprintf(which, sizeof(which), "the new path puts its band %s zero, the sum in use today puts it %s zero",
                 cl_side > 0 ? "above" : "below", hl_side > 0 ? "above" : "below");
        check("a negative design selects the sideband i-q selects", cl_side == hl_side, which);
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

    // Sideband separation: how much louder a signal in the middle of the passband comes out on one
    // side of the tuning point than on the other.  Taken as a magnitude, so it is the same number
    // whichever side a path passes; that the two paths pass the SAME side is the check above.
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
           "      not a filter: what to take away is the passband shape, and the gap between the two numbers)\n");
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
        conv_path_reset(RATE, 0);
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
    // Counted the same way as the Hilbert line above it: a complex tap is two multiplies and two adds.
    printf("     the same %d-tap shape as an ordinary time-domain FIR: %.0f per sample, which is what\n"
           "     the partitioning exists to avoid paying, on the shape the tone table above measures\n",
           cbs.nc, 4.0 * (double) cbs.nc);
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
           "     which at the time of writing, with the geometry printed above, takes the ELF's bss from\n"
           "     93,860 to 157,276 bytes: the end of .bss moves from 0x20008760 to 0x20017fd8, leaving\n"
           "     32,800 bytes below the stack top at 0x2001fff8 where the shipping build leaves 96,408.\n"
           "     That space is shared, and nothing in the linker script bounds it: _sbrk() comes from\n"
           "     libnosys, bumps a pointer from _end upwards and has no ceiling to compare against, and\n"
           "     the ASSERT(__StackLimit >= __HeapLimit) in arm-gcc-link_f4_main.ld compares the heap's\n"
           "     *start* against the top of RAM, so it cannot fire however full the radio is.  What the\n"
           "     heap is asked for in this configuration is small - the 12 byte canary word in\n"
           "     uhsdr_canary.c and the USB class handles - so 32,800 bytes is stack headroom in practice,\n"
           "     down from 96,408.  Whether that is enough on a transmitter that has never had the option\n"
           "     on is a question for the radio, not for this harness.\n");
    printf("     buffering the block queues add: %.1f ms on the decimated path, where one block is\n"
           "     %.1f ms of a %.0f Hz stream, and %.1f ms where the filter path does not decimate and\n"
           "     the same block is %.1f ms of %.0f kHz.  That is not the whole delay: the taps have one\n"
           "     of their own, which section 9 measures.  The Hilbert pair costs %.2f ms and %.2f ms.\n",
           delay_decimated_ms, 1000.0 * cbs.size / RATE, (double) RATE,
           delay_fullrate_ms, 1000.0 * cbs.size / IQ_SAMPLE_RATE_F, IQ_SAMPLE_RATE_F / 1000.0,
           (IQ_RX_NUM_TAPS_HI - 1) / 2.0 * 1000.0 / RATE,
           (IQ_RX_NUM_TAPS_HI - 1) / 2.0 * 1000.0 / IQ_SAMPLE_RATE_F);
    printf("     the AGC, the notch, the post-demod filters and every digimode still run after this, on\n"
           "     samples that arrive that much later, on a radio that has never had the option switched\n"
           "     on.  The selectivity above is a measurement; whether it is worth this much latency and RAM\n"
           "     is listening, and nobody has listened yet.\n");
}

// The seam in AudioDriver_RxProcessor() replaces the maintained SSB stage for whatever
// CONVOLUTION_USED_FOR() names, and the only measurement of what that covers is this: a mode that is
// on the list without being measured here is a mode whose receive path has been changed on the air by
// accident.  DEMOD_DIGI is the one that was, until it was not - it is neither AM, SAM nor FM, and it is
// not one of the two-channel modes, so an exclusion list let it through and its RTTY and BPSK
// demodulators were being fed from behind the convolution filter.
static void seam_mode_table(void)
{
    printf("\n6. which modes this build hands to the convolution filter\n");

    const uint16_t wanted[]   = { DEMOD_USB, DEMOD_LSB, DEMOD_CW };
    const uint16_t unwanted[] = { DEMOD_AM, DEMOD_SAM, DEMOD_FM, DEMOD_DIGI,
#ifdef USE_TWO_CHANNEL_AUDIO
                                  DEMOD_SSBSTEREO, DEMOD_IQ,
#endif
                                  0xFFFF };
    const char* const wanted_names[]   = { "USB", "LSB", "CW" };
    const char* const unwanted_names[] = { "AM", "SAM", "FM", "DIGI (RTTY/BPSK)", "SSBSTEREO", "IQ", 0 };

    char detail[200];
    const char* bad = 0;

#ifdef CONVOLUTION_OFF_FOR_TEST
    // The control build: the feature is compiled in - geometry, queues, counters, debug line - and the
    // three modes are routed back to the Hilbert pair, which is the point of the image.  So the claim
    // this check makes here is the opposite one, and it is the claim that makes the control worth
    // flashing: if the control replaced a mode, "audible on this image" would say nothing about where
    // the fault is.  Everything above still measured the filter, which is what the image would run if
    // the switch were not there; that is worth saying out loud rather than leaving the operator to
    // wonder why a control build prints selectivity figures.
    for (unsigned k = 0; k < sizeof wanted / sizeof wanted[0] && !bad; k++)
    {
        if (CONVOLUTION_USED_FOR(wanted[k]))
        {
            snprintf(detail, sizeof(detail), "CONVOLUTION_OFF_FOR_TEST is defined but %s is still being "
                     "replaced, so this image is not the control it claims to be", wanted_names[k]);
            bad = detail;
        }
    }
    if (bad)
    {
        check("the control build routes all three modes back to the pair", !bad, detail);
        return;
    }
    snprintf(detail, sizeof detail, "CONVOLUTION_OFF_FOR_TEST is defined and no mode is handed to the "
             "filter, so the figures above describe what this image would run without the switch, not "
             "what it runs");
    check("the control build routes all three modes back to the pair", !bad, detail);
    return;
#endif
    for (unsigned k = 0; k < sizeof wanted / sizeof wanted[0] && !bad; k++)
    {
        if (!CONVOLUTION_USED_FOR(wanted[k]))
        {
            snprintf(detail, sizeof(detail), "%s is not being replaced, but everything above measured it as if it were", wanted_names[k]);
            bad = detail;
        }
    }
    for (unsigned k = 0; unwanted[k] != 0xFFFF && !bad; k++)
    {
        if (CONVOLUTION_USED_FOR(unwanted[k]))
        {
            snprintf(detail, sizeof(detail), "%s is being replaced, and nothing here has measured that path", unwanted_names[k]);
            bad = detail;
        }
    }
    check("only the modes measured here are the modes replaced", !bad,
          bad ? bad : "in: USB, LSB, CW; out: AM, SAM, FM and DIGI, which is where RTTY and BPSK live");
}

// ---------------------------------------------------------------- the response of each geometry, as numbers
//
// Section 3 samples eight spot offsets, which is enough to compare two paths but not enough to see a
// filter's shape.  `--csv` prints the whole thing - one row per frequency on the side of zero each path
// actually passes, at the finest spacing the analysis window resolves exactly - so the geometries can be
// plotted against each other and against the Hilbert pair.  Nothing else changes: it is the same
// conv_path_run() through the same queues and the same worker.
// Sweep the passed side of zero, once per path, in whole cycles of the analysis window.  Fills the
// three arrays (offset in Hz, convolution dB, Hilbert dB, each path normalised to its own loudest row)
// and returns the number of rows.  tone_amplitude() answers in linear amplitude, so the division by the
// row maximum has to happen after the sweep: a single pass printing straight through would label
// linear numbers as decibels, which is not distinguishable by eye in a table and was the first
// version's mistake.
#define SWEEP_MAX 512

// Both sides of zero, on the same design: the side it is meant to pass and the side it is meant to
// throw away.  The second one is the number an operator feels as the station next door, and it is where
// the two RX paths differ by fifty decibels rather than by a decibel of ripple.
struct Sweep
{
    double hz[SWEEP_MAX];
    double conv_pass[SWEEP_MAX];
    double hilb_pass[SWEEP_MAX];
    double conv_image[SWEEP_MAX];
    double hilb_image[SWEEP_MAX];
    int n;
};

static double one_tone_conv(double f)
{
    double re[STREAM], im[STREAM], y[STREAM];
    unsigned long ops;
    for (int n = 0; n < STREAM; n++)
    {
        const double a = 2.0 * M_PI * f * n / RATE;
        re[n] = cos(a);
        im[n] = sin(a);
    }
    conv_path_run(re, im, STREAM, y, &ops, SLICE_DECIM);
    return tone_amplitude(y + SETTLE, ANALYSIS, fabs(f), RATE);
}

static double one_tone_hilb(double f, int usb)
{
    double re[STREAM], im[STREAM], y[STREAM];
    unsigned long ops;
    for (int n = 0; n < STREAM; n++)
    {
        const double a = 2.0 * M_PI * f * n / RATE;
        re[n] = cos(a);
        im[n] = sin(a);
    }
    hilbert_path_run(re, im, STREAM, y, usb, &ops);
    return tone_amplitude(y + SETTLE, ANALYSIS, fabs(f), RATE);
}

static void measure_sweep(struct Sweep* w)
{
    const double step_hz = (double) RATE / ANALYSIS * 10.0;      // ten cycles a window: exact bins
    int n = (int) (2.0 * BW / step_hz);
    if (n > SWEEP_MAX)
    {
        n = SWEEP_MAX;
    }

    // tone_amplitude() answers in linear amplitude, and what an operator compares is shape, so every
    // row is divided by the loudest row of its own path afterwards.  Doing that in one pass would
    // label linear numbers as decibels, which is not distinguishable by eye in a table and was the
    // first version's mistake.
    conv_path_reset(RATE, 0);
    double mc = 0.0, mh = 0.0;
    for (int k = 0; k < n; k++)
    {
        w->hz[k] = step_hz * (k + 1);
        w->conv_pass[k] = one_tone_conv(w->hz[k]);                  // the side this design passes
        w->hilb_pass[k] = one_tone_hilb(w->hz[k], 1);
    }
    for (int k = 0; k < n; k++)
    {
        // the image side, at the same distance from zero: for a filter whose job is sideband selection
        // this is the same question asked of the other half of the spectrum
        w->conv_image[k] = one_tone_conv(-w->hz[k]);
        w->hilb_image[k] = one_tone_hilb(-w->hz[k], 1);
    }
    for (int k = 0; k < n; k++)
    {
        if (w->conv_pass[k] > mc) mc = w->conv_pass[k];
        if (w->hilb_pass[k] > mh) mh = w->hilb_pass[k];
    }
    for (int k = 0; k < n; k++)
    {
        w->conv_pass[k]  = 20.0 * log10(w->conv_pass[k] / mc + 1e-300);
        w->hilb_pass[k]  = 20.0 * log10(w->hilb_pass[k] / mh + 1e-300);
        w->conv_image[k] = 20.0 * log10(w->conv_image[k] / mc + 1e-300);
        w->hilb_image[k] = 20.0 * log10(w->hilb_image[k] / mh + 1e-300);
    }
    w->n = n;
}

// Where a sweep crosses a level, interpolating between the two rows either side of it, looking only at
// or above `from` because below that the passband's own edges are the thing being measured.
static double crossing(const double* fo, const double* yv, int n, double level, int from)
{
    for (int k = 1; k < n; k++)
    {
        if (fo[k] < from)
        {
            continue;
        }
        if (yv[k - 1] > level && yv[k] <= level)
        {
            const double t = (yv[k - 1] - level) / (yv[k - 1] - yv[k]);
            return fo[k - 1] + t * (fo[k] - fo[k - 1]);
        }
    }
    return 0.0;
}

static void response_shape(void)
{
    static struct Sweep w;
    measure_sweep(&w);
    const int n = w.n;
    const double* fo = w.hz;
    const double* yc = w.conv_pass;
    const double* yh = w.hilb_pass;

    double hi = -1e30, lo = 1e30;
    for (int k = 0; k < n; k++)
    {
        if (fo[k] >= 300.0 && fo[k] <= 2400.0)
        {
            if (yc[k] > hi) hi = yc[k];
            if (yc[k] < lo) lo = yc[k];
        }
    }

    // The corner that matters is the one the operator tunes towards: the passband is designed to stop
    // at the filter width, and how fast it gets to -60 dB past that is what an adjacent signal has to
    // come through.  The Hilbert column is measured on the same signal and the same window, and its
    // corner sits higher because the IQ filter is deliberately wider than the audio band and the
    // post-demod filters finish the shape afterwards - which is a real difference between the two
    // arrangements, not an artefact, and the reason the comparison is printed rather than scored.
    const double c6 = crossing(fo, yc, n, -6.0, 1500);
    const double c60 = crossing(fo, yc, n, -60.0, 1500);
    const double h6 = crossing(fo, yh, n, -6.0, 1500);
    const double h60 = crossing(fo, yh, n, -60.0, 1500);

    printf("\n7. the shape of the passband, from %d offsets on the side it passes\n", n);
    printf("     in band (300 to 2400 Hz): ripple %.2f dB\n", hi - lo);
    printf("     past the edge:   -6 dB at %6.0f Hz    -60 dB at %6.0f Hz    that is %.0f Hz of skirt\n",
           c6, c60, c60 - c6);
    printf("     the path in use: -6 dB at %6.0f Hz    -60 dB at %6.0f Hz    that is %.0f Hz of skirt\n",
           h6, h60, h60 > 0 ? h60 - h6 : 0.0);
    if (h60 > 0.0 && c60 > 0.0)
    {
        printf("     (the maintained path's corner is above the filter width on purpose: its IQ filter is\n"
               "      wider than the audio band and the post-demod filters finish the shape.  Fewer taps\n"
               "      means both a wider skirt and more ripple here, which is the trade the geometry makes)\n");
    }
    check("the passband stays within 3 dB across the audio band", hi - lo < 3.0, "see the ripple above");

    // What the same sweep says about the other side of zero, which is the reason the option exists.
    double wi = 1e30, hi_img = -1e30, whi = 1e30, hhi = -1e30;
    for (int k = 0; k < n; k++)
    {
        if (fo[k] >= 300.0 && fo[k] <= 2400.0)
        {
            if (w.conv_image[k] < wi) wi = w.conv_image[k];
            if (w.conv_image[k] > hi_img) hi_img = w.conv_image[k];
            if (w.hilb_image[k] < whi) whi = w.hilb_image[k];
            if (w.hilb_image[k] > hhi) hhi = w.hilb_image[k];
        }
    }
    printf("     the other side of zero, 300 to 2400 Hz (the sideband an SSB receiver must throw away):\n"
           "       this filter: worst case %6.1f dB    the path in use today: worst case %6.1f dB\n",
           hi_img, hhi);
}

static void response_csv(void)
{
    static struct Sweep w;
    measure_sweep(&w);
    printf("# offset_hz,conv_pass_db,conv_image_db,hilbert_pass_db,hilbert_image_db"
           "  (geometry: block %d samples, %d taps, %.0f Hz; each path normalised to its own loudest passband row)\n",
           FFT_CONVOLUTION_SIZE / 2, CONVOLUTION_MAX_NO_OF_BLOCKS * (FFT_CONVOLUTION_SIZE / 2), (double) RATE);
    for (int k = 0; k < w.n; k++)
    {
        printf("%.1f,%.2f,%.2f,%.2f,%.2f\n", w.hz[k], w.conv_pass[k], w.conv_image[k],
               w.hilb_pass[k], w.hilb_image[k]);
    }
}

// ---------------------------------------------------------------- every filter path the radio has
//
// Everything above measures one filter path.  The radio has about sixty that SSB and CW can select,
// and the convolution filter is set up differently for each: its band comes from the path's width
// and centre, and its rate from whether the path decimates.  A setup that is right for 2.7 kHz and
// wrong elsewhere passes every check above - the first one written was silent on all ten 300 Hz CW
// paths, because it filtered 30..300 Hz around a sidetone at 500..950 Hz.
//
// So each path is asked the one question that has a plain answer: a tone in the middle of the path
// has to come out of the convolution filter as loud as it comes out of the Hilbert pair and sum the
// path uses today, and the same tone on the other side of zero has to be gone.  The table is the
// radio's - the Makefile cuts FilterPathInfo[] out of audio_filter.c - and the rate, the slice and
// the setup call are written the way AudioDriver_SetProcessingChain() and the RX processor write them.
static double path_tone_conv(double f, float32_t rate, uint16_t slice)
{
    double re[STREAM], im[STREAM], y[STREAM];
    unsigned long ops;
    for (int n = 0; n < STREAM; n++)
    {
        const double a = 2.0 * M_PI * f * n / rate;
        re[n] = cos(a);
        im[n] = sin(a);
    }
    conv_path_run(re, im, STREAM, y, &ops, slice);
    return tone_amplitude(y + SETTLE, ANALYSIS, fabs(f), rate);
}

static double path_tone_hilbert(const FilterPathDescriptor* p, double f, float32_t rate, int usb)
{
    double re[STREAM], im[STREAM], y[STREAM];
    unsigned long ops;
    for (int n = 0; n < STREAM; n++)
    {
        const double a = 2.0 * M_PI * f * n / rate;
        re[n] = cos(a);
        im[n] = sin(a);
    }
    hilbert_run(p->FIR_I_coeff_file, p->FIR_Q_coeff_file, p->FIR_numTaps, re, im, STREAM, y, usb, &ops);
    return tone_amplitude(y + SETTLE, ANALYSIS, fabs(f), rate);
}

#define PATH_LEVEL_TOLERANCE_DB   1.0       // against the path in use, at the centre of the path
#define PATH_IMAGE_LIMIT_DB     -60.0       // the same tone on the other side of zero

static void every_filter_path(void)
{
    printf("\n8. every SSB and CW filter path the radio has, a tone at the centre of each\n");

    int paths = 0, failed = 0;
    double worst_level = 0.0, worst_image = -1e30;

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if ((p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) == 0 || p->FIR_I_coeff_file == NULL)
        {
            continue;
        }
        paths++;

        // the rate and the slice the RX processor hands this path's blocks over at
        const int decimated = (p->FIR_I_coeff_file == i_rx_new_coeffs);
        const float32_t rate = decimated ? IQ_SAMPLE_RATE_F / p->sample_rate_dec : IQ_SAMPLE_RATE_F;
        const uint16_t slice = decimated ? SLICE_FULL / p->sample_rate_dec : SLICE_FULL;

        const double width = FilterInfo[p->id].width;
        const double centre = p->offset ? p->offset : width / 2.0;
        // on the analysis grid, so the tone is a whole number of cycles in the window measured
        const double grid = (double) rate / ANALYSIS;
        const double f = floor(centre / grid + 0.5) * grid;

        for (int lsb = 0; lsb < 2; lsb++)
        {
            const double side = lsb ? -1.0 : 1.0;
            AudioDriver_ConvolutionSetFilterPath((float32_t) width, (float32_t) p->offset, lsb != 0, rate);
            const double wanted = path_tone_conv(side * f, rate, slice);
            const double image = path_tone_conv(-side * f, rate, slice);
            const double today = path_tone_hilbert(p, side * f, rate, !lsb);

            const double level_db = 20.0 * log10(wanted / today + 1e-300);
            const double image_db = 20.0 * log10(image / wanted + 1e-300);
            if (fabs(level_db) > fabs(worst_level)) worst_level = level_db;
            if (image_db > worst_image) worst_image = image_db;

            if (fabs(level_db) > PATH_LEVEL_TOLERANCE_DB || image_db > PATH_IMAGE_LIMIT_DB)
            {
                failed++;
                printf("     FAIL path %2d \"%s\", %5.0f Hz wide around %5.0f Hz at %.0f Hz, %s: %+.1f dB against "
                       "the path in use, other side of zero %.1f dB\n",
                       id, p->name, width, centre, (double) rate, lsb ? "LSB" : "USB", level_db, image_db);
            }
        }
    }

    printf("     %d paths, both sidebands: level against the path in use within %+.2f dB, the other side of\n"
           "     zero at %.1f dB or better\n", paths, worst_level, worst_image);

    char detail[200];
    snprintf(detail, sizeof detail, "%d of %d path and sideband pairs are listed above", failed, 2 * paths);
    check("each path passes its own centre at the level in use, on the right sideband",
          paths > 0 && failed == 0, detail);

    // leave the module set up the way the rest of the file found it
    conv_path_reset(RATE, 0);
}

// ---------------------------------------------------------------- how late the audio is
//
// Section 2 reports the buffering, which is the block.  It says nothing about the taps, and this file
// used to read it as "the block is what you wait for": a linear phase response delays everything by
// half its length, 42.6 ms for 1024 taps at 12 kHz, and comparing the pipeline with a plain FIR over
// the same taps cancels exactly that half out of the comparison.
//
// So this measures what a listener gets: a tone at the centre of the path is switched on, and the
// time until the audio reaches half its level is the delay, block and taps together.  The taps are
// minimum phase unless the build says CONVOLUTION_LINEAR_PHASE, and the check is the claim that makes
// that worth having: they must cost less than half of what the same response costs in linear phase.
// With CONVOLUTION_LINEAR_PHASE the check is the other half of the same statement, that the delay is
// then the half response this section was written to find.
static const FilterPathDescriptor* find_path(int filter_id, int offset)
{
    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if (p->id == filter_id && p->offset == offset && p->FIR_I_coeff_file == i_rx_new_coeffs
            && (p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) != 0)
        {
            return p;
        }
    }
    return NULL;
}

#define ONSET_AT    2048        // the sample the tone is switched on at

static double tone_onset_ms(const FilterPathDescriptor* p, double f)
{
    static double er[STREAM], ei[STREAM];
    float32_t i[SLICE_DECIM], q[SLICE_DECIM];

    AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[p->id].width, (float32_t) p->offset, false, RATE);

    long widx = 0;
    for (int at = 0; at < STREAM; at += SLICE_DECIM)
    {
        for (uint16_t k = 0; k < SLICE_DECIM; k++)
        {
            const int n = at + k;
            const double a = 2.0 * M_PI * f * (n - ONSET_AT) / RATE;
            i[k] = n >= ONSET_AT ? (float32_t) cos(a) : 0.0f;
            q[k] = n >= ONSET_AT ? (float32_t) sin(a) : 0.0f;
        }
        AudioDriver_ConvolutionInput(i, q, SLICE_DECIM);
        convolution_handle();
        const uint16_t given = AudioDriver_ConvolutionOutput(i, q, SLICE_DECIM);
        for (uint16_t k = 0; k < SLICE_DECIM; k++, widx++)
        {
            er[widx] = k < given ? i[k] : 0.0;
            ei[widx] = k < given ? q[k] : 0.0;
        }
    }

    // the filter hands back both halves of the complex signal, so their magnitude is the envelope
    const double settled = hypot(er[STREAM - 256], ei[STREAM - 256]);
    for (int n = 0; n < STREAM; n++)
    {
        if (hypot(er[n], ei[n]) >= 0.5 * settled)
        {
            return (n - ONSET_AT) * 1000.0 / RATE;
        }
    }
    return -1.0;
}

static void how_late_the_audio_is(void)
{
    printf("\n9. how late the audio is: a tone switched on, and the time until it is at half level\n");

    const struct { int filter_id; int offset; const char* name; } paths[] = {
        { AUDIO_2P7KHZ, 1350, "2.7 kHz SSB path, 1350 Hz tone" },
        { AUDIO_500HZ,   750, "500 Hz CW path,   750 Hz tone" },
    };
    const double hilbert_ms = (IQ_RX_NUM_TAPS_HI - 1) / 2.0 * 1000.0 / RATE;
    int ok = 1;
    char detail[300] = "";

    for (unsigned k = 0; k < sizeof paths / sizeof paths[0]; k++)
    {
        const FilterPathDescriptor* p = find_path(paths[k].filter_id, paths[k].offset);
        if (p == NULL)
        {
            snprintf(detail, sizeof detail, "the radio's table no longer has the %s", paths[k].name);
            ok = 0;
            continue;
        }
        const double total_ms = tone_onset_ms(p, paths[k].offset);
        const double taps_ms = total_ms - delay_decimated_ms;
        const double linear_ms = (cbs.nc - 1) / 2.0 * 1000.0 / RATE;

        printf("     %s: %5.1f ms = %.1f of buffering + %.1f in the taps, which are %d long and would\n"
               "        cost %.1f in linear phase.  The Hilbert pair in use: %.2f ms\n",
               paths[k].name, total_ms, delay_decimated_ms, taps_ms, cbs.nc, linear_ms, hilbert_ms);

#ifndef CONVOLUTION_LINEAR_PHASE
        if (!(total_ms >= 0.0 && taps_ms < 0.5 * linear_ms))
        {
            snprintf(detail, sizeof detail, "%s: the taps delay the signal by %.1f ms, and half of the "
                     "linear phase %.1f ms is the most a minimum phase response may cost", paths[k].name, taps_ms, linear_ms);
            ok = 0;
        }
#else
        if (!(total_ms >= 0.0 && fabs(taps_ms - linear_ms) < 1.0))
        {
            snprintf(detail, sizeof detail, "%s: the taps delay the signal by %.1f ms, where a linear phase "
                     "response of this length delays it by %.1f ms", paths[k].name, taps_ms, linear_ms);
            ok = 0;
        }
#endif
    }

#ifndef CONVOLUTION_LINEAR_PHASE
    check("the minimum phase taps cost less than half the delay of linear phase ones", ok, detail);
#else
    check("built with CONVOLUTION_LINEAR_PHASE: the taps cost half their length", ok, detail);
#endif

    conv_path_reset(RATE, 0);
}

// ---------------------------------------------------------------- a filter change leaves nothing behind
//
// The worker keeps the spectra of the last blocks it filtered, and the filter design borrows the same
// memory to work in.  Either one left where it was comes out of the loudspeaker when the next filter
// starts: the last syllable of the old filter, or the design's arithmetic, through the new masks.  No
// measurement above can see that, because every one of them lets the filter settle first.
static void a_filter_change_is_silent(void)
{
    printf("\n10. a filter change leaves nothing behind in the pipeline\n");

    const FilterPathDescriptor* ssb = find_path(AUDIO_2P7KHZ, 1350);
    const FilterPathDescriptor* cw = find_path(AUDIO_500HZ, 750);
    if (ssb == NULL || cw == NULL)
    {
        check("silence in is silence out straight after a filter change", 0, "the two paths used here are not in the radio's table");
        return;
    }

    float32_t i[SLICE_DECIM], q[SLICE_DECIM];
    double loudest = 0.0;
    for (int round = 0; round < 2; round++)
    {
        const FilterPathDescriptor* from = round ? cw : ssb;
        const FilterPathDescriptor* to = round ? ssb : cw;

        // sound through one filter, so there is a history to leave behind
        AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[from->id].width, (float32_t) from->offset, false, RATE);
        for (int at = 0; at < 4 * cbs.nc; at += SLICE_DECIM)
        {
            for (uint16_t k = 0; k < SLICE_DECIM; k++)
            {
                const double a = 2.0 * M_PI * from->offset * (at + k) / RATE;
                i[k] = (float32_t) cos(a);
                q[k] = (float32_t) sin(a);
            }
            AudioDriver_ConvolutionInput(i, q, SLICE_DECIM);
            convolution_handle();
            AudioDriver_ConvolutionOutput(i, q, SLICE_DECIM);
        }

        // the other filter, and nothing but silence into it for longer than its history reaches
        AudioDriver_ConvolutionSetFilterPath((float32_t) FilterInfo[to->id].width, (float32_t) to->offset, false, RATE);
        for (int at = 0; at < (cbs.nfor + 4) * cbs.size; at += SLICE_DECIM)
        {
            memset(i, 0, sizeof i);
            memset(q, 0, sizeof q);
            AudioDriver_ConvolutionInput(i, q, SLICE_DECIM);
            convolution_handle();
            const uint16_t given = AudioDriver_ConvolutionOutput(i, q, SLICE_DECIM);
            for (uint16_t k = 0; k < given; k++)
            {
                if (fabs(i[k]) > loudest) loudest = fabs(i[k]);
                if (fabs(q[k]) > loudest) loudest = fabs(q[k]);
            }
        }
    }

    char detail[200];
    snprintf(detail, sizeof detail, "with nothing going in, the first blocks after the change came out as loud as %g, "
             "where the passband gain is %g", loudest, (double) CONVOLUTION_PASSBAND_GAIN);
    check("silence in is silence out straight after a filter change", loudest == 0.0, detail);

    conv_path_reset(RATE, 0);
}

// ---------------------------------------------------------------- what the low edge of the band is for
//
// The band the masks are built for is the filter path's own width around its own centre, except that a
// path whose band reaches 0 Hz is given a low edge (CONVOLUTION_LOW_CUT_HZ) rather than the full reach
// down to DC.  Nothing in this file used to ask whether that edge does anything: set the constant to 0
// and every other check here still passes.
//
// What it does is hold DC.  A real I/Q pair always has some, and there is nothing downstream that
// takes it out on these modes: the AGC's own DC removal is switched on for AM and SAM only -
// audio_driver.c:559 passes `dmod_mode == DEMOD_AM || dmod_mode == DEMOD_SAM` - and USB, LSB and CW,
// the three modes this filter takes over, are exactly the three it is off for.
//
// With the edge removed the 2.7 kHz path passes DC at 6 dB down, within a factor of two of the audio
// itself.  With the edge at 50 Hz the same tone is 124 dB down at geometry 256, 35 at 128 and 19 at 64.
// The wide paths are the hard case: they run at the full 48 kHz, where 50 Hz is a thousandth of the
// rate, and a response of a few hundred taps cannot make a wall that narrow - so there the edge buys
// single-digit dB rather than 100, and at geometry 64 it buys too little.  That is printed as a cost of
// the geometry rather than hidden in a tolerance.
//
// So two things are required of every path whose band reaches zero, both measured against the Hilbert
// pair the radio runs today on the same path with the same signal:
//
//   LOW_EDGE_DC_MARGIN_DB   on the decimated paths, where the pair this replaces is 9.7 dB down at DC
//                           and this filter has enough taps to beat it, the filter must hold DC at
//                           least this much better than the pair it replaces (3 dB).  The unedgeled
//                           filter fails this everywhere.
//   LOW_EDGE_DC_FLOOR_DB    and on every path, DC must not come out at anything near the level of the
//                           audio: 3 dB down is a factor of two in amplitude, which is what an S-meter
//                           and a loudspeaker would notice.  This is the half that applies to the wide
//                           paths, where the pair being replaced is itself only 0.2 dB down at DC and
//                           there is therefore no margin to be had against it.
#define LOW_EDGE_DC_MARGIN_DB  (3.0)
#define LOW_EDGE_DC_FLOOR_DB   (-3.0)

static void the_low_edge_of_the_band(void)
{
    printf("\n11. the low edge: what a path whose band reaches 0 Hz does with DC\n");

    int paths = 0, failed = 0;
    double worst = -1e30, worst_today = 0.0;

    for (int id = 1; id < AUDIO_FILTER_PATH_NUM; id++)
    {
        const FilterPathDescriptor* p = &FilterPathInfo[id];
        if ((p->mode & ((1 << FILTER_MODE_SSB) | (1 << FILTER_MODE_CW))) == 0 || p->FIR_I_coeff_file == NULL)
        {
            continue;
        }
        const double width = FilterInfo[p->id].width;
        const double centre = p->offset ? p->offset : width / 2.0;
        if (centre - width / 2.0 >= CONVOLUTION_LOW_CUT_HZ)
        {
            continue;               // this path's band does not reach zero; the edge is not in its way
        }

        const int decimated = (p->FIR_I_coeff_file == i_rx_new_coeffs);
        const float32_t rate = decimated ? IQ_SAMPLE_RATE_F / p->sample_rate_dec : IQ_SAMPLE_RATE_F;
        const uint16_t slice = decimated ? SLICE_FULL / p->sample_rate_dec : SLICE_FULL;

        const double grid = (double) rate / ANALYSIS;
        const double f = floor(centre / grid + 0.5) * grid;

        AudioDriver_ConvolutionSetFilterPath((float32_t) width, (float32_t) p->offset, false, rate);
        const double ref = path_tone_conv(f, rate, slice);
        const double dc = path_tone_conv(0.0, rate, slice);
        const double today = path_tone_hilbert(p, f, rate, 1);
        const double today_dc = path_tone_hilbert(p, 0.0, rate, 1);

        const double dc_db = 20.0 * log10(dc / (ref + 1e-300) + 1e-300);
        const double today_db = 20.0 * log10(today_dc / (today + 1e-300) + 1e-300);
        paths++;
        if (dc_db > worst) { worst = dc_db; worst_today = today_db; }
        const int over_floor = dc_db > LOW_EDGE_DC_FLOOR_DB;
        const int no_margin = decimated && dc_db > today_db - LOW_EDGE_DC_MARGIN_DB;
        if (over_floor || no_margin)
        {
            failed++;
            char why[48] = "";
            if (over_floor) strcat(why, "above the floor");
            if (no_margin) strcat(why, over_floor ? ", not better than today" : "not better than today");
            printf("     FAIL path %2d \"%s\", %5.0f Hz around %5.0f Hz at %.0f Hz: DC %+.1f dB, "
                   "the path in use %+.1f dB  (%s)\n", id, p->name, width, centre, (double) rate,
                   dc_db, today_db, why);
        }
    }

    printf("     %d of the SSB/CW paths reach 0 Hz; the loudest of them passes DC at %+.1f dB, where the\n"
           "     path in use at the same point passes it at %+.1f dB (edge %.0f Hz, block %d samples,\n"
           "     response %d taps)\n",
           paths, worst, worst_today, (double) CONVOLUTION_LOW_CUT_HZ,
           FFT_CONVOLUTION_SIZE / 2, CONVOLUTION_MAX_NO_OF_BLOCKS * (FFT_CONVOLUTION_SIZE / 2));

    char detail[200];
    snprintf(detail, sizeof detail,
             "%d of %d paths fail: the loudest DC is %+.1f dB, which must be %.0f dB or better and at "
             "least %.0f dB better than the path in use there (%+.1f dB)", failed, paths, worst,
             LOW_EDGE_DC_FLOOR_DB, LOW_EDGE_DC_MARGIN_DB, worst_today);
    check("DC is well down on every path whose band reaches zero", paths > 0 && failed == 0, detail);

    conv_path_reset(RATE, 0);
}

// ---------------------------------------------------------------- a design a worker pass lands inside
//
// The design works in the worker's own buffers: fmask, fftin, fftout and accum are one union, and
// design_scratch is the same memory seen as one flat array.  That is only allowed because the module
// closes the seam for the duration - conv_design_busy, set before the queues are emptied and cleared
// after the history is written - and because thread mode cannot preempt a worker pass that has already
// begun, closing the seam is enough on its own.
//
// Nothing else here can see that condition: every other section designs first and measures afterwards.
// A single-threaded host cannot interrupt itself, so the module offers conv_test_preempt and this
// section uses it - one block's worth of slices, one worker pass, and the read back, from inside the
// design, at the two points where such a pass does the most damage.
//
// What it catches, measured by deleting the guard: all 4096 mask floats come out NaN, the band cache in
// AudioDriver_ConvolutionSetFilterPath() then holds them until the operator changes filter width, and no
// counter on the debug line notices, because a queue of NaN moves exactly as fast as a queue of audio.
static int preempt_calls;
static uint32_t preempt_blocks_queued;

static void preempt_with_a_worker_pass(const char* where)
{
    (void) where;
    preempt_calls++;

    float32_t i[SLICE_DECIM], q[SLICE_DECIM];
    for (uint16_t k = 0; k < SLICE_DECIM; k++)
    {
        i[k] = 0.25f;               // a level, not silence: a seam that refuses silence looks the same
        q[k] = -0.125f;
    }
    const uint32_t before = conv_stat_blocks_in;
    for (uint16_t r = 0; r < cbs.size / SLICE_DECIM; r++)
    {
        AudioDriver_ConvolutionInput(i, q, SLICE_DECIM);
        convolution_handle();
        AudioDriver_ConvolutionOutput(i, q, SLICE_DECIM);
    }
    preempt_blocks_queued += conv_stat_blocks_in - before;
}

static void a_design_is_not_interrupted(void)
{
    printf("\n12. a design that a worker pass lands inside\n");

    static float32_t clean[sizeof(cob.fmask) / sizeof(float32_t)];
    const size_t mask_floats = sizeof(cob.fmask) / sizeof(float32_t);
    float32_t* const masks = &cob.fmask[0][0];
    const float32_t other_offset = BW_OFFSET + 1000.0f;      // a different band, to defeat the cache

    conv_test_preempt = NULL;
    // A, forced rather than cached, so `clean` is a design and not whatever the section above left
    AudioDriver_ConvolutionSetFilterPath(BW, other_offset, false, RATE);
    AudioDriver_ConvolutionSetFilterPath(BW, BW_OFFSET, false, RATE);
    memcpy(clean, masks, mask_floats * sizeof(float32_t));

    // and the same design again with nobody interrupting: if the reference is not reproducible, the
    // comparison below says nothing about interruption
    AudioDriver_ConvolutionSetFilterPath(BW, other_offset, false, RATE);
    AudioDriver_ConvolutionSetFilterPath(BW, BW_OFFSET, false, RATE);
    const int reproducible = memcmp(clean, masks, mask_floats * sizeof(float32_t)) == 0;

    AudioDriver_ConvolutionSetFilterPath(BW, other_offset, false, RATE);
    preempt_calls = 0;
    preempt_blocks_queued = 0;
    conv_test_preempt = preempt_with_a_worker_pass;
    AudioDriver_ConvolutionSetFilterPath(BW, BW_OFFSET, false, RATE);        // A, under interruption
    conv_test_preempt = NULL;

    int identical = memcmp(clean, masks, mask_floats * sizeof(float32_t)) == 0;
    int finite = 1;
    for (unsigned k = 0; k < mask_floats; k++)
    {
        if (!isfinite(masks[k]))
        {
            finite = 0;
            break;
        }
    }

    printf("     %d worker passes ran inside that design, queueing %u block(s): the masks are%s the\n"
           "     undisturbed design's, and%s finite\n", preempt_calls, preempt_blocks_queued,
           identical ? "" : " NOT", finite ? "" : " are NOT");

    char detail[240];
    snprintf(detail, sizeof detail, "%d passes ran inside the design and %u block(s) got through the "
             "seam; masks identical to the clean design: %s; all masks finite: %s; the design is "
             "reproducible without interrupting it: %s", preempt_calls, preempt_blocks_queued,
             identical ? "yes" : "no", finite ? "yes" : "no", reproducible ? "yes" : "no");

    check("the seam refuses samples while a filter is being designed",
          preempt_calls > 0 && preempt_blocks_queued == 0, detail);
    check("a design interrupted by the worker still produces the masks it designed",
          identical && finite && reproducible, detail);

    conv_path_reset(RATE, 0);
}

// 13. the counters a measurement build reads, and whether they read what they claim
//
// CONVOLUTION_MEASURE is the option that answers "can this be switched on": how much audio a filter
// change eats, what a block costs, how close the stack comes to the .bss.  Those numbers are only
// worth the flash they sit in if the fields hold what their names say, and on a radio the only way to
// find out is to break the build and look.  So the counting half is checked on a host, where it can be
// arranged: the cache counter against a cache that is observable another way (memcmp of the masks), the
// refused-frame counter against refusals counted by the test itself, and the worker's pass count against
// calls that had nothing to do.
//
// The timing half cannot be checked here: conv_meas_clock() is the DWT counter, which does not exist on
// a host, so every *_cyc_* field reads 0 and this section says so instead of pretending otherwise.  A
// host cannot verify that 1.3 ms on x86 is 8 ms on an F407; that is what the radio session is for.
static uint32_t seam_refused;

static void seam_feeds_slices_the_interrupt_would(const char* where)
{
    (void) where;
    float32_t i[SLICE_DECIM], q[SLICE_DECIM];
    for (uint16_t k = 0; k < SLICE_DECIM; k++)
    {
        i[k] = 0.25f;
        q[k] = -0.125f;
    }
    if (AudioDriver_ConvolutionInput(i, q, SLICE_DECIM) == 0)
    {
        seam_refused++;           // refused, which is the design's seam doing its job
    }
}

static void what_the_measurement_build_counts(void)
{
    printf("\n13. the counters a measurement build reads\n");

#ifdef CONVOLUTION_MEASURE
    // Prime with a band nothing else in this file uses, and snapshot after it, so the counts below are
    // five calls and not five-plus-whatever-the-previous-section-left-cached.  The cache is keyed on the
    // last band designed, which is a fact about the module worth stating: a repeat of the *previous*
    // band is a hit even when the one before that was different.
    const float32_t off_c = BW_OFFSET + 1500.0f;
    AudioDriver_ConvolutionSetFilterPath(BW, off_c, false, RATE);
    conv_meas_reset();            // zeroes the conv_stat_* totals as well, so the deltas below are deltas
    const uint32_t setups0 = conv_meas_setup_count;
    const uint32_t designs0 = conv_meas_design_count;
#endif

    static float32_t mask_a[sizeof(cob.fmask) / sizeof(float32_t)];
    static float32_t mask_b[sizeof(cob.fmask) / sizeof(float32_t)];
    float32_t* const masks = &cob.fmask[0][0];
    const size_t mask_floats = sizeof(cob.fmask) / sizeof(float32_t);
    const float32_t off_a = BW_OFFSET;
    const float32_t off_b = BW_OFFSET + 900.0f;

    // A, A, B, B, A: five setups, three different bands.  The behaviour is what the cache is for and is
    // observable without the counters; the counters are only a report of it.
    AudioDriver_ConvolutionSetFilterPath(BW, off_a, false, RATE);
    memcpy(mask_a, masks, mask_floats * sizeof(float32_t));
    AudioDriver_ConvolutionSetFilterPath(BW, off_a, false, RATE);
    const int same_band_same_masks = memcmp(mask_a, masks, mask_floats * sizeof(float32_t)) == 0;
    AudioDriver_ConvolutionSetFilterPath(BW, off_b, false, RATE);
    memcpy(mask_b, masks, mask_floats * sizeof(float32_t));
    const int other_band_other_masks = memcmp(mask_a, mask_b, mask_floats * sizeof(float32_t)) != 0;
    AudioDriver_ConvolutionSetFilterPath(BW, off_b, false, RATE);
    AudioDriver_ConvolutionSetFilterPath(BW, off_a, false, RATE);
    const int back_to_a_is_a = memcmp(mask_a, masks, mask_floats * sizeof(float32_t)) == 0;

    char detail[240];
#ifdef CONVOLUTION_MEASURE
    const uint32_t setups = conv_meas_setup_count - setups0;
    const uint32_t designs = conv_meas_design_count - designs0;
    snprintf(detail, sizeof detail, "%u setups of which %u designed; masks for a band unchanged on a "
             "repeat: %s; a different band gives different masks: %s; back to the first band gives the "
             "first masks: %s",
             setups, designs, same_band_same_masks ? "yes" : "no",
             other_band_other_masks ? "yes" : "no", back_to_a_is_a ? "yes" : "no");
    printf("     5 setups, %u designs run; 3 of them should have been cache misses\n", designs);
    check("the design counter counts designs and not setups",
          setups == 5 && designs == 3 && same_band_same_masks && other_band_other_masks && back_to_a_is_a,
          detail);

    // The audio-off hole, counted in frames by the thing that refused them.  The seam feeds slices from
    // inside a design, which is what the audio interrupt does to one on a radio; whatever it refuses has
    // to come out as the same number in conv_meas_busyin_last, because that field is the difference
    // between "the design costs 2 ms" and "the design costs 2 ms and eight frames of audio".
    seam_refused = 0;
    conv_test_preempt = seam_feeds_slices_the_interrupt_would;
    AudioDriver_ConvolutionSetFilterPath(BW, off_b, false, RATE);
    conv_test_preempt = NULL;
    const uint32_t busy_b = conv_meas_busyin_last;

    AudioDriver_ConvolutionSetFilterPath(BW, off_a, false, RATE);   // same band: cached, but a setup
    const uint32_t busy_cached = conv_meas_busyin_last;

    snprintf(detail, sizeof detail, "the interrupted design refused %u frame(s) and the counter said %u; "
             "the same band again, un-interrupted, said %u", seam_refused, busy_b, busy_cached);
    check("frames refused during a design are counted for that design",
          busy_b == seam_refused && busy_b > 0 && busy_cached == 0, detail);

    // The worker's average is taken over passes that filtered something; if it counted the empty calls
    // too, W on the debug line would be a fraction of the real cost and the number would flatter the
    // build it is supposed to judge.
    AudioDriver_ConvolutionSetFilterPath(BW, BW_OFFSET, false, RATE);
    const uint32_t passes0 = conv_meas_block_count;
    float32_t i[SLICE_DECIM], q[SLICE_DECIM];
    for (uint16_t k = 0; k < SLICE_DECIM; k++)
    {
        i[k] = 0.5f;
        q[k] = 0.25f;
    }
    for (uint16_t r = 0; r < 2 * cbs.size / SLICE_DECIM; r++)
    {
        AudioDriver_ConvolutionInput(i, q, SLICE_DECIM);
        convolution_handle();
    }
    for (int idle = 0; idle < 5; idle++)
    {
        convolution_handle();               // nothing queued: five passes that do not filter
    }
    for (uint16_t r = 0; r < 2 * cbs.size / SLICE_DECIM; r++)
    {
        AudioDriver_ConvolutionOutput(i, q, SLICE_DECIM);
    }
    snprintf(detail, sizeof detail, "two blocks and five idle calls moved the working-pass count by %u "
             "(should be 2); the timing fields read 0 here because DWT does not exist on a host",
             conv_meas_block_count - passes0);
    check("the worker counts passes that filtered a block, not calls",
          conv_meas_block_count - passes0 == 2, detail);
#else
    snprintf(detail, sizeof detail, "CONVOLUTION_MEASURE is not compiled in, so only the behaviour the "
             "counters report is checked here: a band's masks held across a repeat (%s), a different band "
             "designed (%s), the first band's masks found again on the way back (%s)",
             same_band_same_masks ? "yes" : "no", other_band_other_masks ? "yes" : "no",
             back_to_a_is_a ? "yes" : "no");
    printf("     CONVOLUTION_MEASURE off: the counting fields do not exist in this build, so this checks\n"
           "     the caching they report and nothing else.  Run with EXTRA_DEFS=-DCONVOLUTION_MEASURE for\n"
           "     the counters themselves.\n");
    check("the band cache holds masks for the band they were designed for",
          same_band_same_masks && other_band_other_masks && back_to_a_is_a, detail);
#endif

    conv_path_reset(RATE, 0);
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--csv") == 0)
    {
        response_csv();
        return 0;
    }

    printf("convolution: measuring the RX filter USE_CONVOLUTION offers\n");
    // The geometry this build was given, and the response length that follows from it: the code clamps
    // the tap count to the partitions its arrays hold, so the array size is not what gets filtered.
    printf("  (block %d samples, %d partitions: a response of up to %d taps at %.0f Hz, arrays sized %d)\n",
           FFT_CONVOLUTION_SIZE / 2, CONVOLUTION_MAX_NO_OF_BLOCKS,
           CONVOLUTION_MAX_NO_OF_BLOCKS * (FFT_CONVOLUTION_SIZE / 2), RATE,
           CONVOLUTION_MAX_NO_OF_COEFFS);

    fft_round_trip();
    implemented_filter_is_the_designed_one();

    seam_mode_table();
    response_shape();
    every_filter_path();
    how_late_the_audio_is();
    a_filter_change_is_silent();
    the_low_edge_of_the_band();
    a_design_is_not_interrupted();
    what_the_measurement_build_counts();

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
