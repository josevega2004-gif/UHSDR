// Olivia reference modem for the digi bench.
//
// The mode: thirty-two tones 31.25 Hz apart, so the signal is 1000 Hz wide, keyed at 31.25 baud.
// What makes Olivia Olivia is that each symbol carries *two* of the thirty-two tones at once. A pair
// of tones is one of 496 combinations, and a character needs seven bits, 128 of them, so there is
// nearly four times the room needed: the mode spends that room on redundancy, the pair that is sent
// is a fixed long way from the pair for any other character, and an interleaver spreads a burst over
// a block of characters. That is why Olivia keeps working when a single-tone mode has already gone.
//
// Two things here are this bench's and not the mode's, and the README says so: the mapping from a
// seven bit character to a tone pair is derived arithmetically (pair index 3c+1 of the 496) rather
// than the mode's own table, and the interleaver is a block of eight characters rather than Olivia's
// much longer one. The tone geometry, the rate, the two tones per symbol and the length header are
// what the table measures.

#include "olivia.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OL_RATE     12000.0
#define OL_BAUD     31.25
#define OL_SPACING  31.25
#define OL_TONES    32
#define OL_BASE     935.0
#define OL_SYMBOL   384                 // samples: 32 ms at 12 kHz
#define OL_PAIRS    496                 // C(32,2)
#define OL_BLOCK    8                   // characters per interleaver block

static const char ol_alpha[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?-/";
#define OL_ALPHA_LEN ((int)(sizeof(ol_alpha) - 1))

static int ol_code(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char* p = strchr(ol_alpha, c);
    return p ? (int)(p - ol_alpha) : 0;
}

// the 496 tone pairs in a fixed order, and the index a character uses: 3c+1 leaves every character
// two pairs away from its neighbours, which is the redundancy the mode spends on being heard wrong
static void ol_pair(int idx, int* a, int* b)
{
    int n = 0;
    for (int i = 0; i < OL_TONES; i++)
        for (int j = i + 1; j < OL_TONES; j++, n++)
            if (n == idx) { *a = i; *b = j; return; }
    *a = 0;
    *b = 1;
}

static int ol_index(int a, int b)
{
    if (a > b) { const int t = a; a = b; b = t; }
    int n = 0;
    for (int i = 0; i < OL_TONES; i++)
        for (int j = i + 1; j < OL_TONES; j++, n++)
            if (i == a && j == b) return n;
    return 0;
}

// the nearest pair a character could have used: with the pairs three apart, a symbol that came back
// one pair out is still read as its own character, which is the redundancy the mode spends here
static int ol_code_of_index(int idx)
{
    if (idx < 1) return 0;
    int c = (idx - 1) / 3;
    const int lo = 3 * c + 1, hi = 3 * (c + 1) + 1;
    if (abs(hi - idx) < abs(lo - idx)) c++;
    if (c < 0) c = 0;
    if (c > 127) c = 127;
    return c;
}

// bit reversal of a three bit index: the interleaver block is eight characters, and the permutation
// is its own inverse, which is what lets the same code undo it
static int ol_perm(int i)
{
    return ((i & 1) ? 4 : 0) | ((i & 2) ? 2 : 0) | ((i & 4) ? 1 : 0);
}

size_t Bench_OliviaGenerate(float32_t* out, size_t cap, const char* text, float tune_hz)
{
    const size_t len = strlen(text);
    const int nchars = len > 4095 ? 4095 : (int)len;
    const int nsym = 2 + ((nchars + OL_BLOCK - 1) / OL_BLOCK) * OL_BLOCK;

    static int codes[8192];
    int n = 0;
    codes[n++] = (nchars >> 6) & 0x3F;          // the length, high six bits
    codes[n++] = nchars & 0x3F;                 // and low six
    for (int i = 0; i < nchars; i++) codes[n++] = ol_code(text[i]);
    while (n % OL_BLOCK) codes[n++] = 0;
    if ((size_t)nsym * OL_SYMBOL > cap) return 0;

    size_t k = 0;
    double phase[OL_TONES];
    for (int t = 0; t < OL_TONES; t++) phase[t] = 0.0;
    for (int blk = 0; blk * OL_BLOCK < n; blk++)
    {
        for (int i = 0; i < OL_BLOCK; i++)
        {
            const int c = codes[blk * OL_BLOCK + ol_perm(i)];
            int a, b;
            ol_pair(3 * c + 1, &a, &b);
            const double fa = OL_BASE + (double)tune_hz + OL_SPACING * (double)a;
            const double fb = OL_BASE + (double)tune_hz + OL_SPACING * (double)b;
            const double sa = 2.0 * M_PI * fa / OL_RATE, sb = 2.0 * M_PI * fb / OL_RATE;
            for (int s = 0; s < OL_SYMBOL; s++, k++)
            {
                // two tones at half amplitude each, so a symbol is one unit of signal either way
                out[k] = (float32_t)(0.2 * sin(phase[a]) + 0.2 * sin(phase[b]));
                phase[a] += sa;
                phase[b] += sb;
                if (phase[a] > 2.0 * M_PI) phase[a] -= 2.0 * M_PI;
                if (phase[b] > 2.0 * M_PI) phase[b] -= 2.0 * M_PI;
            }
        }
    }
    // one silent symbol of tail: the receiver's phase fit can land anywhere in a symbol, and without
    // the tail a phase near the end of a symbol would cost the last interleaver block
    for (int i = 0; i < OL_SYMBOL && k < cap; i++, k++) out[k] = 0.0f;
    return k;
}

static double ol_tab_cos[OL_TONES][OL_SYMBOL], ol_tab_sin[OL_TONES][OL_SYMBOL];
static int ol_tab_ready;

static void ol_tables(void)
{
    if (ol_tab_ready) return;
    for (int t = 0; t < OL_TONES; t++)
    {
        const double w = 2.0 * M_PI * (OL_BASE + OL_SPACING * (double)t) / OL_RATE;
        for (int i = 0; i < OL_SYMBOL; i++)
        {
            ol_tab_cos[t][i] = cos(w * (double)i);
            ol_tab_sin[t][i] = sin(w * (double)i);
        }
    }
    ol_tab_ready = 1;
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
    res->period_samples = OL_SYMBOL;
    res->header = -1;
    if (n < (size_t)(4 * OL_SYMBOL)) return 0;

    int best_phase = 0;
    double best_ratio = -1.0;
    // The phase is fitted on the first few symbols only. A sync search that averaged over the whole
    // take would be pulled by a burst anywhere in it - a silent symbol in the middle made the fit
    // prefer a phase that dropped the last symbol of the take, which is a self check's worth of bug.
    const size_t probe = n / OL_SYMBOL < 6 ? n / OL_SYMBOL : 6;
    double e[OL_TONES];
    for (int ph = 0; ph < OL_SYMBOL; ph += 4)
    {
        double ratio = 0.0;
        for (size_t s = 0; s < probe; s++)
        {
            const size_t at = (size_t)ph + s * OL_SYMBOL;
            if (at + OL_SYMBOL > n) break;
            ol_energies(x, at, e);
            double sum = 0.0, top = 0.0, second = 0.0;
            for (int t = 0; t < OL_TONES; t++)
            {
                sum += e[t];
                if (e[t] > top) { second = top; top = e[t]; }
                else if (e[t] > second) second = e[t];
            }
            // both tones of the pair have to be up: a symbol read at the wrong phase puts one tone's
            // energy into two neighbours and leaves the pair split
            ratio += (top + second) / (2.0 * (sum / OL_TONES) + 1e-12);
        }
        if (ratio > best_ratio) { best_ratio = ratio; best_phase = ph; }
    }
    res->phase = best_phase;

    const size_t nsym = (n - (size_t)best_phase) / OL_SYMBOL;
    const size_t nblk = nsym / OL_BLOCK;
    if (nblk < 1) return 0;
    static int codes[8192];
    if (nblk * OL_BLOCK > sizeof(codes) / sizeof(codes[0])) return 0;

    for (size_t s = 0; s < nblk * OL_BLOCK; s++)
    {
        const size_t at = (size_t)best_phase + s * OL_SYMBOL;
        ol_energies(x, at, e);
        int a = -1, b = -1;
        double top = -1.0, second = -1.0, sum = 0.0;
        for (int t = 0; t < OL_TONES; t++)
        {
            sum += e[t];
            if (e[t] > top) { second = top; b = a; top = e[t]; a = t; }
            else if (e[t] > second) { second = e[t]; b = t; }
        }
        res->symbols++;
        if (top + second > 0.5 * sum) res->pairs_ok++;
        const int c = ol_code_of_index(ol_index(a, b));
        // the symbol index carries the interleaver permutation, undoing it as the block fills
        codes[(s / OL_BLOCK) * OL_BLOCK + ol_perm((int)(s % OL_BLOCK))] = c;
    }

    int hdr = ((codes[0] & 0x3F) << 6) | (codes[1] & 0x3F);
    res->header = hdr;
    int nchars = hdr;
    if (nchars > (int)(nblk * OL_BLOCK) - 2) nchars = (int)(nblk * OL_BLOCK) - 2;

    size_t at = 0;
    for (int i = 0; i < nchars && at + 1 < out_sz; i++)
    {
        const int v = codes[2 + i];
        if (v == 0) out[at++] = ' ';
        else if (v <= OL_ALPHA_LEN) out[at++] = ol_alpha[v];
        else out[at++] = '?';
    }
    out[at] = 0;
    return 1;
}
