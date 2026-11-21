/* Hellschreiber (Feld-Hell) for the UHSDR digital mode host bench.
 *
 * Hell is not a character mode: the transmitter draws the text as a bitmap. Each character is a
 * raster five columns wide and seven pixels tall, followed by one blank column, and the pixels are
 * sent in order - down each column, then on to the next - as on/off keying at 122.5 Bd. There is no
 * alphabet, no framing and no error control, which is exactly what makes it worth having in this
 * bench: it is the one mode here whose failure modes are purely the channel's, and the only way to
 * score it is against the picture that was sent.
 *
 * Receiver: the pixel clock is the one thing it must recover (the nominal is 12000/122.5 = 97.96
 * samples). It takes the envelope at the keying tone over one pixel, picks the sampling phase that
 * makes the pixels most two-valued, then groups them into columns of seven and characters of six by
 * the fraction of columns that come out blank - a Hell glyph is followed by a gap, so a correct
 * alignment has one blank column in six and a wrong one has almost none.
 *
 * What a receiver cannot recover is the paper position: a human reads a Hell picture wherever it
 * lands on the paper, so a whole pixel of vertical roll or a column of horizontal offset is not an
 * error. The bench's score therefore allows that shift and reports which one it used, rather than
 * pretending the printer has a glyph synchroniser it does not have.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "hellschreiber.h"

#define HS_RATE        12000.0
#define HS_BAUD          122.5
#define HS_TONE_HZ      1000.0
#define HS_COLS           5            // glyph columns
#define HS_ROWS           7
#define HS_GAP            1            // blank columns between characters
#define HS_CELL         (HS_COLS + HS_GAP)

/* The font, written as it is drawn. Rows top to bottom, five columns each; '#' is a keyed pixel.
 * Uppercase letters, digits and the few stops a test message needs - Hell's own alphabet is a drawn
 * one and there is nothing to standardise beyond legibility. */
typedef struct { char c; const char* row[HS_ROWS]; } hs_glyph_t;

static const hs_glyph_t hs_glyphs[] =
{
    { 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
    { 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
    { 'C', { ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###." } },
    { 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
    { 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
    { 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
    { 'G', { ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###." } },
    { 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
    { 'I', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####" } },
    { 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
    { 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
    { 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
    { 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#" } },
    { 'N', { "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#" } },
    { 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
    { 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
    { 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
    { 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
    { 'S', { ".###.", "#...#", "#....", ".###.", "....#", "#...#", ".###." } },
    { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
    { 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
    { 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
    { 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#" } },
    { 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
    { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
    { 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
    { '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
    { '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", "#####" } },
    { '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
    { '3', { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." } },
    { '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
    { '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
    { '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
    { '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
    { '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
    { '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
    { ' ', { ".....", ".....", ".....", ".....", ".....", ".....", "....." } },
    { '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
    { '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
    { '/', { "....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...." } },
    { ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
};

static const hs_glyph_t* hs_find(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(hs_glyphs) / sizeof(hs_glyphs[0]); i++)
        if (hs_glyphs[i].c == c) return &hs_glyphs[i];
    return NULL;
}

// The picture the text makes: HS_CELL columns per character, seven rows. One byte per pixel, so the
// truth and the received raster are the same shape and can be compared without a font at all.
static int hs_render(const char* text, uint8_t* pix, int max_cols)
{
    int col = 0;
    for (const char* p = text; *p && col + HS_CELL <= max_cols; p++)
    {
        const hs_glyph_t* g = hs_find(*p);
        if (!g) continue;                      // not in the drawn alphabet: nothing to send
        for (int c = 0; c < HS_COLS; c++)
        {
            for (int r = 0; r < HS_ROWS; r++)
                pix[r * max_cols + col] = (uint8_t)(g->row[r][c] == '#');
            col++;
        }
        for (int r = 0; r < HS_ROWS; r++) pix[r * max_cols + col] = 0;   // the gap column
        col++;
    }
    return col;
}

size_t Bench_HellGenerate(float32_t* out, size_t cap, const char* text, uint8_t* truth,
                          int max_cols, int* cols_out, float tone_hz, float clk_pct)
{
    const int cols = hs_render(text, truth, max_cols);
    *cols_out = cols;
    // 97.96 samples is one pixel. clk_pct is the transmitter's pixel clock against the receiver's: a
    // sender 2 % fast shears the picture by nearly two columns over a line of text, which is the
    // failure a raster mode has instead of a bit slip.
    const double period = HS_RATE / HS_BAUD / (1.0 + (double)clk_pct / 100.0);
    const size_t n_pix = (size_t)cols * HS_ROWS;
    const size_t total = (size_t)lround((double)n_pix * period);
    if (total > cap) return 0;

    double phase = 0.0;
    for (size_t k = 0; k < total; k++)
    {
        // The raster is sent down each column and then on to the next, while the truth is stored row
        // by row; the two orders are transposes of each other, and reading the truth linearly here
        // put a rotated picture on the air that no receiver could line up with the text.
        const size_t which = (size_t)((double)k / period);          // pixel index in transmission order
        const int col = (int)(which / HS_ROWS), r = (int)(which % HS_ROWS);
        const int on = which < n_pix ? truth[r * max_cols + col] : 0;
        out[k] = (float32_t)(on ? 0.4 * sin(phase) : 0.0);
        phase += 2.0 * M_PI * (double)tone_hz / HS_RATE;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    return total;
}

// amplitude of the keying tone over one pixel, sliding, one value per sample
static double* hs_envelope(const float32_t* x, size_t n)
{
    const int W = (int)lround(HS_RATE / HS_BAUD);
    const double w = 2.0 * M_PI * HS_TONE_HZ / HS_RATE;
    double* amp = malloc((n ? n : 1) * sizeof(double));
    if (!amp) return NULL;
    double ri = 0.0, rq = 0.0;
    for (size_t k = 0; k < n; k++)
    {
        const double c = cos(w * (double)k), s = sin(w * (double)k);
        ri += (double)x[k] * c;
        rq -= (double)x[k] * s;
        if (k >= (size_t)W)
        {
            const double j = (double)(k - W);
            const double xj = (double)x[k - W];
            ri -= xj * cos(w * j);
            rq += xj * sin(w * j);
        }
        amp[k] = sqrt(ri * ri + rq * rq) / (double)W;
    }
    return amp;
}

static int hs_cmp(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int Bench_HellDecode(const float32_t* x, size_t n, uint8_t* pix, int max_cols, int* cols_out,
                     hell_result_t* res)
{
    memset(res, 0, sizeof(*res));
    *cols_out = 0;
    if (n < 4096) return 0;
    double* amp = hs_envelope(x, n);
    if (!amp) return 0;

    const double period = HS_RATE / HS_BAUD;
    // the threshold between a keyed and an unkeyed pixel, from the take's own amplitudes rather than
    // from a fixed level: the first and last tenth are the quiet and the inked ones
    double* sorted = malloc(n * sizeof(double));
    if (!sorted) { free(amp); return 0; }
    memcpy(sorted, amp, n * sizeof(double));
    qsort(sorted, n, sizeof(double), hs_cmp);
    const double thr = 0.5 * (sorted[n / 10] + sorted[(n * 9) / 10]);
    free(sorted);

    // the pixel phase: the sampling offset that makes the pixels most two-valued
    double best_score = -1.0;
    int best_phase = 0;
    for (int ph = 0; ph < (int)lround(period); ph++)
    {
        double score = 0.0;
        for (size_t k = 0; ; k++)
        {
            const long at = lround((double)ph + ((double)k + 0.5) * period);
            if (at >= (long)n) break;
            const double e = amp[at];
            score += (e > thr) ? (e - thr) : (thr - e);
        }
        if (score > best_score) { best_score = score; best_phase = ph; }
    }
    res->pixel_phase = best_phase;
    res->pixel_samples = period;
    const int n_pix = (int)(((double)n - (double)best_phase) / period);
    if (n_pix < HS_ROWS * HS_CELL) { free(amp); return 0; }

    uint8_t* raw = malloc((size_t)n_pix);
    if (!raw) { free(amp); return 0; }
    for (int k = 0; k < n_pix; k++)
    {
        const long at = lround((double)best_phase + ((double)k + 0.5) * period);
        raw[k] = (uint8_t)(at < (long)n && amp[at] > thr);
    }
    free(amp);

    // The grouping and the character phase are found together, because they are the same question
    // asked twice: which run of seven pixels is a column, and which of every six columns is the gap.
    // A wrong grouping straddles two columns and shreds the gaps; a wrong character phase puts the
    // gap where a glyph's middle is. The correct answer leaves one column in six blank.
    int best_group = 0, best_col = 0, best_total = 0;
    double best_blank = -1.0;
    for (int go = 0; go < HS_ROWS; go++)
    {
        const int cols_avail = (n_pix - go) / HS_ROWS;
        for (int co = 0; co < HS_CELL; co++)
        {
            int blank = 0, total = 0;
            for (int m = 0; ; m++)
            {
                const int gap = co + HS_CELL * m + HS_COLS;
                if (gap >= cols_avail) break;
                const size_t first = (size_t)go + (size_t)gap * (size_t)HS_ROWS;
                int any = 0;
                for (int r = 0; r < HS_ROWS; r++) if (raw[first + (size_t)r]) any = 1;
                total++;
                if (!any) blank++;
            }
            if (total < 3) continue;            // too little of the picture to tell anything
            const double share = (double)blank / (double)total;
            if (share > best_blank + 1e-9 || (fabs(share - best_blank) <= 1e-9 && total > best_total))
            {
                best_blank = share;
                best_total = total;
                best_group = go;
                best_col = co;
            }
        }
    }
    if (best_blank < 0.0) { free(raw); return 0; }
    res->column_phase = best_col;
    // the share of gap columns that came out blank: 1.0 is a raster whose character boundaries were
    // all found, and it is the quantity the grouping search maximised
    res->gap_share = best_blank;

    const int cols_avail = (n_pix - best_group) / HS_ROWS;
    int out_cols = cols_avail - best_col;
    if (out_cols > max_cols) out_cols = max_cols;
    for (int c = 0; c < out_cols; c++)
    {
        const size_t first = (size_t)best_group + (size_t)(c + best_col) * (size_t)HS_ROWS;
        for (int r = 0; r < HS_ROWS; r++)
            pix[r * max_cols + c] = raw[first + (size_t)r];
    }
    free(raw);
    *cols_out = out_cols;
    return 1;
}
