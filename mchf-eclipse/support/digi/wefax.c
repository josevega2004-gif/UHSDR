/* Weather fax (HF WEFAX) signal generator and reference demodulator, host only.
 *
 * The generator makes the broadcast the decoder has to survive: 120 lines/min at 12 kHz, a 5 ms
 * sync tip at 1857 Hz opening each line, picture tone swept between 1500 Hz (black) and 2300 Hz
 * (white), optional polarity swap, scan reversal, per line timing skew, QSB and AWGN.
 *
 * The demodulator is written the cheap way on purpose: after the sync tip fixes the line start,
 * each pixel gets its own exact DFT (Goertzel) at the black and the white frequency over the
 * pixel slot, so no filters have to be tuned and the per sample cost is a handful of multiply
 * accumulates. That is the number that decides whether this mode fits on an F4.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"

#define WEFAX_RATE          12000.0f
#define WEFAX_F_SYNC        1857.0f
#define WEFAX_F_BLACK       1500.0f
#define WEFAX_F_WHITE       2300.0f
#define WEFAX_GAP_SAMPLES   540                 // blanking after the picture
#define WEFAX_PIX_SAMPLES   18                  // 300 columns per line

static float grey_to_freq(float grey, bool invert)
{
    if (invert) grey = 255.0f - grey;
    return WEFAX_F_BLACK + (WEFAX_F_WHITE - WEFAX_F_BLACK) * (grey / 255.0f);
}

// deterministic test chart: gradient, bars, two "landmass" blobs, a dark frame
static uint8_t chart_grey(uint32_t row, uint32_t col, uint32_t rows)
{
    if (row < 3 || row >= rows - 3 || col < 3 || col >= WEFAX_IMG_COLS - 3) return 250;   // frame
    if (col < WEFAX_IMG_COLS / 3) return (uint8_t) (255.0f * (float) col / (WEFAX_IMG_COLS / 3.0f));
    if (col < 2 * WEFAX_IMG_COLS / 3)
    {
        int band = (int) ((col - WEFAX_IMG_COLS / 3) / 12);
        return (band & 1) ? 20 : 235;
    }
    float dx = (float) (col - 5 * WEFAX_IMG_COLS / 12) / 22.0f;
    float dy = (float) (row - rows / 3) / (rows / 6.0f);
    if (dx * dx + dy * dy < 1.0f) return 40;
    dx = (float) (col - 9 * WEFAX_IMG_COLS / 12) / 18.0f;
    dy = (float) (row - 2 * rows / 3) / (rows / 5.0f);
    if (dx * dx + dy * dy < 1.0f) return 70;
    return 215;
}

static void tone(float32_t* out, size_t from, size_t len, float freq, float amp, size_t total)
{
    size_t ramp = 4;                            // short fade keeps the synthetic signal click free
    for (size_t i = 0; i < len && from + i < total; i++)
    {
        float env = 1.0f;
        if (i < ramp) env = (float) i / (float) ramp;
        else if (i + ramp > len) env = (float) (len - i) / (float) ramp;
        out[from + i] += env * amp * sinf(2.0f * (float) M_PI * freq * (float) i / WEFAX_RATE);
    }
}

/* One segment of tone that continues the phase of the segment before it and applies no envelope.
 *
 * The picture of a fax line is not a string of separate bursts: it is one oscillator whose frequency
 * steps once per pixel, and that is what a receiver has to track.  The generator used to synthesize
 * each column with tone() - its own 4-sample fade in and out, its own phase - which made every column
 * boundary a click and made the whole format hypersensitive to a few samples of alignment error.
 * Measured consequence: with fades, a 14 Hz reference error (1 ppm on 20 m, far smaller than real
 * oscillator error) dropped the chart from 0.95 to 0.55 because the sampling grid then sat on the
 * fades, while a 70 Hz error scored 0.976.  A test signal that rewards one error and punishes a smaller
 * one is not measuring the decoder. */
static void tone_continuous(float* out, size_t from, size_t len, float freq, float amp,
                            float* phase, size_t total)
{
    const float step = 2.0f * (float) M_PI * freq / WEFAX_RATE;
    for (size_t i = 0; i < len && from + i < total; i++)
    {
        out[from + i] += amp * sinf(*phase);
        *phase += step;
        if (*phase > 2.0f * (float) M_PI) *phase -= 2.0f * (float) M_PI;
        else if (*phase < 0.0f) *phase += 2.0f * (float) M_PI;
    }
}

void Bench_WefaxGenerate(float32_t* out, uint8_t* truth, const wefax_cfg_t* cfg)
{
    size_t total = (size_t) cfg->rows * WEFAX_SAMPLES_PER_LINE;
    // a reference or tuning error moves every tone of the emission by the same number of Hz:
    // the sync tip and both picture tones. The demodulator below keeps its bins at the nominal
    // frequencies, which is the point - it has no more frequency tracking than the RTTY modem.
    const float tune = cfg->tune_hz;
    memset(out, 0, total * sizeof(float32_t));

    Bench_RngSeed(0x5eed1234u);
    for (uint32_t row = 0; row < cfg->rows; row++)
    {
        int skew = 0;
        if (cfg->skew_ppm > 0.0f)
        {
            skew = (int) ((Bench_Gauss() * cfg->skew_ppm / 1e6) * WEFAX_SAMPLES_PER_LINE);
        }
        size_t base = (size_t) row * WEFAX_SAMPLES_PER_LINE + (size_t) (skew > 0 ? skew : 0);
        // A line must stay inside its own slot.  Clamping an over-long line to the end of the
        // broadcast instead used to drop it on top of its neighbour, where the two tips overlap: the
        // spacing metric then reported 2810 samples and the absolute row placement wrote one row twice
        // - a defect of the test signal that looked exactly like a defect of the capture.  The blanking
        // gap (540 samples) is what the skew has to fit into, which is also what a real transmitter's
        // jitter does.
        if (base > (size_t) (row + 1) * WEFAX_SAMPLES_PER_LINE)
        {
            base = (size_t) (row + 1) * WEFAX_SAMPLES_PER_LINE;
        }
        if (base + WEFAX_SAMPLES_PER_LINE > total)
        {
            base = total > WEFAX_SAMPLES_PER_LINE ? total - WEFAX_SAMPLES_PER_LINE : 0;
        }

        tone(out, base, WEFAX_SYNC_SAMPLES, WEFAX_F_SYNC + tune, 0.9f, total);

        // One oscillator for the whole line: the picture columns step its frequency, the blanking gap
        // carries the white reference, and nothing between them is keyed silent.
        float phase = 0.0f;
        for (uint32_t col = 0; col < WEFAX_IMG_COLS; col++)
        {
            uint32_t src = cfg->steer_right ? (WEFAX_IMG_COLS - 1 - col) : col;
            uint8_t grey = chart_grey(row, src, cfg->rows);
            if (truth) truth[row * WEFAX_IMG_COLS + col] = grey;
            size_t at = base + WEFAX_SYNC_SAMPLES + (size_t) col * WEFAX_PIX_SAMPLES;
            tone_continuous(out, at, WEFAX_PIX_SAMPLES, grey_to_freq(grey, cfg->invert) + tune, 0.75f,
                            &phase, total);
        }
        tone_continuous(out, base + WEFAX_SYNC_SAMPLES + (size_t) WEFAX_IMG_COLS * WEFAX_PIX_SAMPLES,
                        WEFAX_GAP_SAMPLES, grey_to_freq(255.0f, cfg->invert) + tune, 0.75f, &phase, total);
    }
}

/* exact DFT power of one bin over a short window */
static float bin_power(const float32_t* x, size_t n, float freq)
{
    float w = 2.0f * (float) M_PI * freq / WEFAX_RATE;
    float cw = cosf(w), sw = sinf(w);
    float re = 0.0f, im = 0.0f, c = 1.0f, s = 0.0f;
    for (size_t i = 0; i < n; i++)
    {
        re += x[i] * c;
        im -= x[i] * s;
        float nc = c * cw - s * sw;
        s = s * cw + c * sw;
        c = nc;
    }
    return (re * re + im * im) / ((float) n * (float) n);
}

// How far past the nominal next tip the search reaches.  The backward limit is not a margin at all:
// the capture used to search a whole line length forward from the end of the previous picture, so a
// line that came in shorter than nominal put its tip behind the cursor and could not be found however
// the threshold was tuned.  The window now opens at the end of the picture - the blanking gap, which
// is 540 samples of nothing, so a tip as early as 120 samples before nominal is inside it.
//
// Opening the window any earlier is not a wider net, it is a false-alarm generator: the sync tip is at
// 1857 Hz, which is exactly mid-grey between the black 1500 and the white 2300 picture tones, so
// picture content at a steady middle grey looks like a sync tip to a ratio test.  Measured, not
// reasoned - a window that started 600 samples before nominal took the clean case from 0.95 to 0.57.
#define WEFAX_SEARCH_MARGIN 600

#define WEFAX_LINE_PICTURE_END  (WEFAX_SYNC_SAMPLES + (size_t)WEFAX_IMG_COLS * WEFAX_PIX_SAMPLES)

// How much weaker the window before a candidate has to be for that candidate to count as a tip
// arriving.  4 is chosen by measurement below, not by taste: the real tip's preceding window is the
// white reference and carries essentially no 1857 (0.0006 against the tip's 0.177), while a mid-grey
// picture stretch carries as much as the tip does (0.113 against 0.113), so the two are separated by
// more than an order of magnitude and any factor from 2 to 10 would do the same job here.
/* Where the picture starts, measured from the candidate the discriminator liked best.
 *
 * It is not WEFAX_SYNC_SAMPLES.  The tip is 60 samples of 1857 Hz whose leading edge fades in over the
 * generator's 4-sample ramp, and the power of a 60-sample DFT at 1857 keeps rising past the edge: for
 * the generated broadcast it peaks at +24 samples, and the picture tone that follows at mid-grey is
 * also 1857, which is why the peak is late rather than early.  Sampling at best+60 therefore begins
 * 24 samples into the picture, and each 18-sample column window lands across the fades that separate
 * the synthesized tones - measured, a column of true white 250 decoding to grey 110, uniformly, on
 * every row.  best+36 puts the window exactly on the first picture column.
 *
 * This is a calibration of this discriminator against this format, which is also what a real receiver
 * would be doing when it aligned to the standard's white reference after the tip rather than to the
 * tip's power peak.  It is not a property of the channel, and the number is only as good as the
 * profile in the README that shows it.
 */
/* How far below the profile's own peak a candidate may sit and still count as being on it.  The
 * discriminator's maximum is NOT at the pulse's leading edge - the 60-sample DFT keeps rising for about
 * 24 samples into the tip because the tone that follows a tip at mid-grey is 1857 Hz as well - so
 * taking the maximum parks the start inside the first picture column and every subsequent 18-sample
 * window lands across the generator's per-tone fades: measured, a column of true white 250 decoding to
 * grey 110 on every row.  Taking the earliest candidate on the plateau instead gives the edge, and it
 * is a relative rule, so it survives a frequency error: a fixed 24-sample correction did not, and the
 * 1 ppm row fell from 0.903 to 0.286 when it was tried. */
#define WEFAX_PLATEAU_TOL 0.03f

bool Bench_WefaxDecode(const float32_t* x, size_t n, uint8_t* grey, uint32_t grey_rows, wefax_result_t* res)
{
    memset(res, 0, sizeof(*res));

    size_t prev_tip = 0;
    int have_prev = 0;
    size_t first_tip = 0;
    int have_first = 0;
    size_t lines = 0;
    size_t starts[512];
    float mean_sum = 0.0f;

    while (have_prev ? (prev_tip + WEFAX_SAMPLES_PER_LINE < n) : (WEFAX_SAMPLES_PER_LINE < n))
    {
        size_t win_begin, win_end;

        if (have_prev)
        {
            // from the end of this line's picture (start of blanking) to past the nominal next tip
            win_begin = prev_tip + WEFAX_LINE_PICTURE_END;
            win_end = prev_tip + WEFAX_SAMPLES_PER_LINE + WEFAX_SEARCH_MARGIN;
        }
        else
        {
            // Nothing is locked yet: the first tip can be anywhere in the first two lines.
            win_begin = 0;
            win_end = 2 * WEFAX_SAMPLES_PER_LINE;
        }
        if (win_end > n)
        {
            win_end = n;
        }

        size_t best = win_begin;
        float best_ratio = 0.0f;
        for (size_t cand = win_begin; cand + WEFAX_SYNC_SAMPLES + 40 <= win_end; cand += 6)
        {
            float ps = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_SYNC);
            float pb = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_BLACK);
            float pw = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_WHITE);
            float ratio = ps / (ps + pb + pw + 1e-9f);
            if (ratio > best_ratio) { best_ratio = ratio; best = cand; }
        }

        // The plateau's leading edge, not its peak.  This is one more pass over cheap arithmetic, and
        // it is what turns the tip from "where 1857 looks strongest" into "where 1857 arrived".
        for (size_t cand = win_begin; cand <= best; cand += 6)
        {
            float ps = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_SYNC);
            float pb = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_BLACK);
            float pw = bin_power(&x[cand], WEFAX_SYNC_SAMPLES, WEFAX_F_WHITE);
            if (ps / (ps + pb + pw + 1e-9f) >= best_ratio - WEFAX_PLATEAU_TOL)
            {
                best = cand;
                break;
            }
        }

        if (best_ratio < 0.6f)
        {
            // No tip where one belongs.  Advance the nominal grid rather than jumping to the rejected
            // candidate, or one weak line drags the whole search off the picture.
            res->lines_missed++;
            if (!have_prev)
            {
                prev_tip = WEFAX_SAMPLES_PER_LINE;
                have_prev = 1;
            }
            else
            {
                prev_tip += WEFAX_SAMPLES_PER_LINE;
            }
            continue;
        }

        int row = 0;
        if (!have_first)
        {
            first_tip = best;
            have_first = 1;
        }
        else
        {
            row = (int) lroundf(((float) best - (float) first_tip) / (float) WEFAX_SAMPLES_PER_LINE);
        }

        if (row < 0)
        {
            row = 0;                       // a tip before the one we called first: reuse row 0
        }
        if ((uint32_t) row >= grey_rows)
        {
            break;                         // past the end of the canvas: the broadcast is over
        }

        size_t start = best + WEFAX_SYNC_SAMPLES;
        float row_sum = 0.0f;
        for (uint32_t col = 0; col < WEFAX_IMG_COLS; col++)
        {
            size_t at = start + (size_t) col * WEFAX_PIX_SAMPLES;
            if (at + WEFAX_PIX_SAMPLES > n) break;
            float pb = bin_power(&x[at], WEFAX_PIX_SAMPLES, WEFAX_F_BLACK);
            float pw = bin_power(&x[at], WEFAX_PIX_SAMPLES, WEFAX_F_WHITE);
            float r = pw / (pb + pw + 1e-12f);           // 1 = white, 0 = black
            int g = (int) lroundf((2.0f * r - 1.0f) * 255.0f);
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            grey[(size_t) row * WEFAX_IMG_COLS + col] = (uint8_t) g;
            row_sum += (float) g;
        }
        mean_sum += row_sum;

        if (lines < 512) starts[lines] = best;
        lines++;
        prev_tip = best;
        have_prev = 1;
    }

    res->lines_found = (uint32_t) lines;

    // Line-to-line spacing of the tips that were actually captured.  With a skewed transmitter this
    // number is the transmitter, not the decoder: what the capture is graded on is whether the picture
    // lands on the right rows, which is the correlation the caller computes.
    if (lines > 1)
    {
        float acc = 0.0f;
        for (size_t i = 1; i < lines; i++)
        {
            float d = (float) ((int) (starts[i] - starts[i - 1]) - (int) WEFAX_SAMPLES_PER_LINE);
            acc += d * d;
        }
        res->row_rms_error = sqrtf(acc / (float) (lines - 1));
    }
    res->mean_level = lines ? mean_sum / ((float) lines * WEFAX_IMG_COLS) : 0.0f;
    return lines > 0;
}
