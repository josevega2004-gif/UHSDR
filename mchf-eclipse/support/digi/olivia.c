// Olivia reference modem for the digi bench.
//
// The mode: thirty-two tones 31.25 Hz apart, so the signal is 1000 Hz wide, keyed at 31.25 baud, one
// tone a symbol. A tone is five bits, sixty-four tones make a block, and a block carries five 7 bit
// characters: that is the mode's two-layer code, MFSK across the frequency axis and a Walsh function
// along the time axis. Each character is one of 128 Walsh sequences (sixty-four rows of the Hadamard
// matrix and their complements), scrambled with 0xE257E6D0291574EC rotated thirteen bits per
// character, and its sixty-four bits are spread diagonally over the block: character b puts its bit
// t into symbol t, bit position (b + t) mod 5. One lost symbol is then one bit in each of five
// codewords rather than five bits in one, which is the whole point of the mode.
//
// This is Olivia's own mapping, not an approximation: the Walsh row is not a letter table but the
// char's seven bits (six for the row, one for the sign), the scrambler and the diagonal spread are
// the ARRL draft specification's, and the five-bit symbols go to tones through the mode's Gray code.
// The structure follows fldigi's MFSK< double > codec, which is the on-air reference.
//
// Two things here are the bench's and the README says so: the stream starts with a length in its
// first two characters (Olivia itself is a continuous stream with no framing), and the receiver
// decides each symbol from the whole 32 ms symbol window rather than fldigi's overlapped FFT, so it
// has no separate symbol clock tracking.

#include "olivia.h"

#include <math.h>
#include <string.h>

#define OL_RATE     12000.0
#define OL_SPACING  31.25
#define OL_TONES    32
#define OL_BASE     935.0
#define OL_SYMBOL   384                 // samples: 32 ms at 12 kHz
#define OL_BITS     5                   // bits a symbol: log2 of the tone count
#define OL_SYMS     64                  // symbols in a Walsh block
#define OL_CHARS    5                   // characters in a block: 64 x 5 = 5 x 64 bits
#define OL_SCRAMBLE 0xE257E6D0291574ECull
#define OL_SHIFT    13                  // bits the scrambler rotates per character

static int ol_gray[32];                 // binary -> tone number
static int ol_binary[32];               // tone number -> binary
static int ol_tab_ready;

static void ol_tables_init(void)
{
    if (ol_tab_ready) return;
    for (int b = 0; b < OL_TONES; b++)
    {
        const int g = b ^ (b >> 1);
        ol_gray[b] = g;
        ol_binary[g] = b;
    }
    ol_tab_ready = 1;
}

// the inverse fast Hadamard transform, as fldigi's IFHT: one row of the matrix out of a single
// impulse, so a character's code is the row its seven bits name, and a burst costs one bit of it
static void ol_ifht(int* d, int n)
{
    for (int step = n / 2; step; step /= 2)
        for (int ptr = 0; ptr < n; ptr += 2 * step)
            for (int j = ptr; j < ptr + step; j++)
            {
                const int a = d[j], b = d[j + step];
                d[j] = a - b;
                d[j + step] = a + b;
            }
}

static void ol_fht(double* d, int n)
{
    for (int step = 1; step < n; step *= 2)
        for (int ptr = 0; ptr < n; ptr += 2 * step)
            for (int j = ptr; j < ptr + step; j++)
            {
                const double a = d[j], b = d[j + step];
                d[j] = b + a;
                d[j + step] = b - a;
            }
}

// five characters to sixty-four 5 bit symbols: each character's Walsh row, scrambled, its bits
// spread diagonally, then the Gray code onto the tone numbers
static void ol_encode_block(const int* chars, uint8_t* tone)
{
    int fht[OL_SYMS];
    uint8_t sym[OL_SYMS];
    memset(sym, 0, sizeof(sym));
    for (int b = 0; b < OL_CHARS; b++)
    {
        memset(fht, 0, sizeof(fht));
        const int ch = chars[b] & 0x7F;
        if (ch < OL_SYMS) fht[ch] = 1;
        else fht[ch - OL_SYMS] = -1;
        ol_ifht(fht, OL_SYMS);
        int code_bit = (b * OL_SHIFT) & (OL_SYMS - 1);
        for (int t = 0; t < OL_SYMS; t++)
        {
            if ((OL_SCRAMBLE >> code_bit) & 1ull) fht[t] = -fht[t];
            code_bit = (code_bit + 1) & (OL_SYMS - 1);
            if (fht[t] < 0) sym[t] |= (uint8_t)(1u << ((b + t) % OL_BITS));
        }
    }
    for (int t = 0; t < OL_SYMS; t++) tone[t] = (uint8_t)ol_gray[sym[t]];
}

// and back, with soft bits: the energies of the tones whose bit is 1 against the ones whose bit is 0
static void ol_decode_block(const double soft[OL_SYMS][OL_BITS], int* chars)
{
    double fht[OL_SYMS];
    for (int b = 0; b < OL_CHARS; b++)
    {
        int code_bit = (b * OL_SHIFT) & (OL_SYMS - 1);
        for (int t = 0; t < OL_SYMS; t++)
        {
            double v = soft[t][(b + t) % OL_BITS];
            if ((OL_SCRAMBLE >> code_bit) & 1ull) v = -v;
            fht[t] = v;
            code_bit = (code_bit + 1) & (OL_SYMS - 1);
        }
        ol_fht(fht, OL_SYMS);
        int pos = 0;
        for (int t = 1; t < OL_SYMS; t++) if (fabs(fht[t]) > fabs(fht[pos])) pos = t;
        chars[b] = pos + (fht[pos] < 0.0 ? OL_SYMS : 0);
    }
}

size_t Bench_OliviaGenerate(float32_t* out, size_t cap, const char* text, float tune_hz)
{
    ol_tables_init();
    const size_t len = strlen(text);
    const int nchars = len > 300 ? 300 : (int)len;

    static int codes[512];
    int n = 0;
    // the bench's length, seven bits a character: Olivia has no framing of its own
    codes[n++] = (nchars >> 7) & 0x7F;
    codes[n++] = nchars & 0x7F;
    for (int i = 0; i < nchars; i++)
    {
        const int c = (unsigned char)text[i];
        codes[n++] = c > 127 ? '.' : c;
    }
    while (n % OL_CHARS) codes[n++] = 0;
    const int nblk = n / OL_CHARS;
    const size_t total = (size_t)nblk * OL_SYMS * OL_SYMBOL;
    if (total > cap) return 0;

    size_t k = 0;
    double phase[OL_TONES];
    for (int t = 0; t < OL_TONES; t++) phase[t] = 0.0;
    for (int blk = 0; blk < nblk; blk++)
    {
        uint8_t tone[OL_SYMS];
        ol_encode_block(codes + blk * OL_CHARS, tone);
        for (int s = 0; s < OL_SYMS; s++)
        {
            const double f = OL_BASE + (double)tune_hz + OL_SPACING * (double)tone[s];
            const double step = 2.0 * M_PI * f / OL_RATE;
            for (int i = 0; i < OL_SYMBOL; i++, k++)
            {
                out[k] = (float32_t)(0.4 * sin(phase[tone[s]]));
                phase[tone[s]] += step;
                if (phase[tone[s]] > 2.0 * M_PI) phase[tone[s]] -= 2.0 * M_PI;
            }
        }
    }
    // one silent symbol of tail: the receiver's phase fit can land anywhere in a symbol, and without
    // the tail a phase near the end of a symbol would cost the last block
    for (int i = 0; i < OL_SYMBOL && k < cap; i++, k++) out[k] = 0.0f;
    return k;
}

static double ol_tab_cos[OL_TONES][OL_SYMBOL], ol_tab_sin[OL_TONES][OL_SYMBOL];
static int ol_wave_ready;

static void ol_tables(void)
{
    if (ol_wave_ready) return;
    for (int t = 0; t < OL_TONES; t++)
    {
        const double w = 2.0 * M_PI * (OL_BASE + OL_SPACING * (double)t) / OL_RATE;
        for (int i = 0; i < OL_SYMBOL; i++)
        {
            ol_tab_cos[t][i] = cos(w * (double)i);
            ol_tab_sin[t][i] = sin(w * (double)i);
        }
    }
    ol_wave_ready = 1;
}

static void ol_energies(const float32_t* x, size_t at, double* e)
{
    ol_tables();
    for (int t = 0; t < OL_TONES; t++)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < OL_SYMBOL; i++)
        {
            const double v = (double)x[at + (size_t)i];
            re += v * ol_tab_cos[t][i];
            im -= v * ol_tab_sin[t][i];
        }
        e[t] = re * re + im * im;
    }
}

int Bench_OliviaDecode(const float32_t* x, size_t n, char* out, size_t out_sz, olivia_result_t* res)
{
    memset(res, 0, sizeof(*res));
    res->header = -1;
    if (n < (size_t)(4 * OL_SYMBOL)) return 0;
    ol_tables_init();

    int best_phase = 0;
    double best_ratio = -1.0;
    // The phase is fitted on the first few symbols only. A sync search that averaged over the whole
    // take would be pulled by a burst anywhere in it - a silent symbol in the middle made the fit
    // prefer a phase that dropped the last symbol of the take, which is a self check's worth of bug.
    const size_t probe = n / OL_SYMBOL < 6 ? n / OL_SYMBOL : 6;
    double e[OL_TONES];
    // Only the first half symbol is searched. A phase past half a symbol puts the first window
    // mostly inside the *next* symbol, and while the tones still look fine the whole 64 symbol block
    // is then read one symbol late: the diagonal spread no longer lines up and every character
    // fails. The generators start the take on a block boundary, so that is the only phase to find.
    for (int ph = 0; ph < OL_SYMBOL / 2; ph += 4)
    {
        double ratio = 0.0;
        for (size_t s = 0; s < probe; s++)
        {
            const size_t at = (size_t)ph + s * OL_SYMBOL;
            if (at + OL_SYMBOL > n) break;
            ol_energies(x, at, e);
            double max = 0.0, sum = 0.0;
            for (int t = 0; t < OL_TONES; t++)
            {
                sum += e[t];
                if (e[t] > max) max = e[t];
            }
            // one tone a symbol, so the winning tone against the mean over the thirty-two is what
            // says the window is inside a symbol rather than across two
            ratio += max / (sum / OL_TONES + 1e-12);
        }
        if (ratio > best_ratio) { best_ratio = ratio; best_phase = ph; }
    }
    const size_t nsym = (n - (size_t)best_phase) / OL_SYMBOL;
    const size_t nblk = nsym / OL_SYMS;
    if (nblk < 1) return 0;
    static int codes[512];
    if (nblk * OL_CHARS > (size_t)(sizeof(codes) / sizeof(codes[0]))) return 0;

    double soft[OL_SYMS][OL_BITS];
    for (size_t blk = 0; blk < nblk; blk++)
    {
        for (int s = 0; s < OL_SYMS; s++)
        {
            const size_t at = (size_t)best_phase + (blk * OL_SYMS + (size_t)s) * OL_SYMBOL;
            ol_energies(x, at, e);
            double top = -1.0, sum = 0.0;
            double bit_on[OL_BITS] = { 0.0 }, bit_off[OL_BITS] = { 0.0 };
            for (int t = 0; t < OL_TONES; t++)
            {
                sum += e[t];
                if (e[t] > top) top = e[t];
                const int bin = ol_binary[t];
                for (int j = 0; j < OL_BITS; j++)
                {
                    if ((bin >> j) & 1) bit_on[j] += e[t];
                    else bit_off[j] += e[t];
                }
            }
            res->symbols++;
            if (top > 0.5 * sum) res->tones_ok++;
            // a symbol bit is 1 where the Walsh chip was -1, so the soft value is the negative of
            // the difference: the transform below then peaks positive on the character's row
            for (int j = 0; j < OL_BITS; j++)
                soft[s][j] = (bit_off[j] - bit_on[j]) / (sum + 1e-12);
        }
        ol_decode_block(soft, codes + blk * OL_CHARS);
        res->blocks++;
    }

    res->header = ((codes[0] & 0x7F) << 7) | (codes[1] & 0x7F);
    int nchars = res->header;
    if (nchars > (int)(nblk * OL_CHARS) - 2) nchars = (int)(nblk * OL_CHARS) - 2;

    size_t at = 0;
    for (int i = 0; i < nchars && at + 1 < out_sz; i++)
    {
        const int v = codes[2 + i] & 0x7F;
        if (v) out[at++] = (char)v;
    }
    out[at] = 0;
    return 1;
}
