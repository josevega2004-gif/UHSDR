// MFSK16 reference modem for the digi bench.
//
// The mode: sixteen tones 15.625 Hz apart, keyed one tone per symbol at 15.625 baud, so a symbol is
// 64 ms and four bits, and the whole signal is 234 Hz wide plus the tone itself. What makes MFSK16
// work at all is the forward error correction and the interleaver: a symbol that survives is worth
// four bits, and a symbol lost to a burst is worth nothing unless the bits it carried were spread
// over its neighbours. The bench keeps that structure: a rate 1/2 constraint length 7 convolutional
// code, a block interleaver over eight symbols, and hard decisions from the tone energies.
//
// Two things here are this bench's and not the mode's, and the README says so at length: the
// alphabet is a 6 bit table (space, A-Z, digits, a few marks) rather than Varicode, and the stream
// starts with an eight bit character count so that a receiver can tell where the text stops. Varicode
// would give the framing for free; a fixed length code needs a header, and a header is the honest way
// to bench everything under it.

#include "mfsk16.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MF_RATE     12000.0
#define MF_BAUD     15.625
#define MF_SPACING  15.625
#define MF_TONES    16
#define MF_BASE     1000.0
#define MF_SYMBOL   768                 // samples: 64 ms at 12 kHz
#define MF_K        7
#define MF_G1       0x79u               // octal 171
#define MF_G2       0x5Bu               // octal 133
#define MF_BLOCK    8                   // symbols per interleaver block, 32 coded bits

// space, A-Z, 0-9 and a few marks: 6 bits, 41 of the 64 codes used
static const char mf_alpha[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,?-";
#define MF_ALPHA_LEN ((int)(sizeof(mf_alpha) - 1))

static int mf_code(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char* p = strchr(mf_alpha, c);
    return p ? (int)(p - mf_alpha) : 0;
}

static int mf_parity(unsigned v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (int)(v & 1u);
}

// rate 1/2, K = 7: state is the last six bits, outputs the two generator parities of the seven
static void mf_conv(const uint8_t* in, size_t nbits, uint8_t* out)
{
    unsigned state = 0;
    for (size_t i = 0; i < nbits; i++)
    {
        const unsigned full = (state << 1) | (unsigned)in[i];
        out[2 * i]     = (uint8_t)mf_parity(full & MF_G1);
        out[2 * i + 1] = (uint8_t)mf_parity(full & MF_G2);
        state = full & 0x3Fu;
    }
}

// A block of 32 coded bits is written as four rows of eight and read down the columns, so the four
// bits one symbol carries land in four different symbols after the channel. A burst that takes a
// symbol out costs the Viterbi decoder one bit in each of eight symbols instead of four in one.
static void mf_interleave(const uint8_t* in, uint8_t* out, size_t blocks)
{
    for (size_t b = 0; b < blocks; b++)
    {
        const uint8_t* src = in + b * 32;
        uint8_t* dst = out + b * 32;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 8; c++)
                dst[c * 4 + r] = src[r * 8 + c];
    }
}

static void mf_deinterleave(const uint8_t* in, uint8_t* out, size_t blocks)
{
    for (size_t b = 0; b < blocks; b++)
    {
        const uint8_t* src = in + b * 32;
        uint8_t* dst = out + b * 32;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 8; c++)
                dst[r * 8 + c] = src[c * 4 + r];
    }
}

size_t Bench_Mfsk16Generate(float32_t* out, size_t cap, const char* text, float tune_hz)
{
    static uint8_t bits[8192], coded[8192], tx[8192];
    const size_t len = strlen(text);
    const int nchars = len > 255 ? 255 : (int)len;
    size_t nb = 0;

    for (int k = 0; k < 8; k++) bits[nb++] = (uint8_t)((nchars >> k) & 1);
    for (int i = 0; i < nchars; i++)
    {
        const int v = mf_code(text[i]);
        for (int k = 0; k < 6; k++) bits[nb++] = (uint8_t)((v >> k) & 1);
    }
    // the interleaver works on whole 16 bit groups; the padding is part of the stream
    while (nb % 16) bits[nb++] = 0;
    const size_t nblocks = nb / 16;
    mf_conv(bits, nb, coded);
    mf_interleave(coded, tx, nblocks);

    const size_t nsym = nblocks * MF_BLOCK;
    const size_t total = nsym * MF_SYMBOL;
    if (total > cap) return 0;

    size_t k = 0;
    double phase = 0.0;
    for (size_t s = 0; s < nsym; s++)
    {
        const int tone = (int)(tx[s * 4] | (tx[s * 4 + 1] << 1) | (tx[s * 4 + 2] << 2)
                               | (tx[s * 4 + 3] << 3));
        const double f = MF_BASE + (double)tune_hz + MF_SPACING * (double)tone;
        const double step = 2.0 * M_PI * f / MF_RATE;
        for (int i = 0; i < MF_SYMBOL; i++, k++)
        {
            out[k] = (float32_t)(0.4 * sin(phase));
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
    }
    return total;
}

// Energy of one symbol window at one tone: a complex correlation over the whole 64 ms, which is what
// makes sixteen tones orthogonal - the spacing is exactly one cycle per symbol, so the sum of a tone
// against a neighbour's frequency over a symbol is zero.
static double mf_tab_cos[MF_TONES][MF_SYMBOL], mf_tab_sin[MF_TONES][MF_SYMBOL];
static int mf_tab_ready;

static void mf_tables(void)
{
    if (mf_tab_ready) return;
    for (int t = 0; t < MF_TONES; t++)
    {
        const double w = 2.0 * M_PI * (MF_BASE + MF_SPACING * (double)t) / MF_RATE;
        for (int i = 0; i < MF_SYMBOL; i++)
        {
            mf_tab_cos[t][i] = cos(w * (double)i);
            mf_tab_sin[t][i] = sin(w * (double)i);
        }
    }
    mf_tab_ready = 1;
}

static void mf_energies(const float32_t* x, size_t at, double* e)
{
    mf_tables();
    for (int t = 0; t < MF_TONES; t++)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < MF_SYMBOL; i++)
        {
            const double v = (double)x[at + (size_t)i];
            re += v * mf_tab_cos[t][i];
            im -= v * mf_tab_sin[t][i];
        }
        e[t] = re * re + im * im;
    }
}

// The trellis decoder: hard decisions in, one bit out per two coded bits. Both predecessors of a
// state are kept in the path memory, so the traceback is a walk backwards from the best final state.
static size_t mf_viterbi(const uint8_t* coded, size_t nbits, uint8_t* out)
{
    enum { S = 64 };
    const size_t steps = nbits / 2;
    static unsigned metric[2][S];
    static uint8_t pred[2048 * S];
    static uint8_t pick[2048 * S];
    if (steps > 2048) return 0;
    for (int s = 0; s < S; s++) metric[0][s] = 0xFFFFu;
    metric[0][0] = 0u;              // the encoder starts in the all zero state, and nothing else does

    for (size_t t = 0; t < steps; t++)
    {
        unsigned* cur = metric[(t + 1) & 1];
        const unsigned* prev = metric[t & 1];
        const int r1 = coded[2 * t], r2 = coded[2 * t + 1];
        for (int s = 0; s < S; s++) cur[s] = 0xFFFFu;
        for (int st = 0; st < S; st++)
        {
            if (prev[st] >= 0xFFFFu) continue;
            for (int b = 0; b < 2; b++)
            {
                const unsigned full = ((unsigned)st << 1) | (unsigned)b;
                const int o1 = mf_parity(full & MF_G1), o2 = mf_parity(full & MF_G2);
                const unsigned m = prev[st] + (unsigned)(o1 != r1) + (unsigned)(o2 != r2);
                const int ns = (int)(full & 0x3Fu);
                if (m < cur[ns])
                {
                    cur[ns] = m;
                    pred[t * S + ns] = (uint8_t)st;
                    pick[t * S + ns] = (uint8_t)b;
                }
            }
        }
    }

    const unsigned* last = metric[steps & 1];
    int best = 0;
    for (int s = 1; s < S; s++) if (last[s] < last[best]) best = s;
    for (size_t t = steps; t > 0; t--)
    {
        out[t - 1] = pick[(t - 1) * S + best];
        best = pred[(t - 1) * S + best];
    }
    return steps;
}

int Bench_Mfsk16Decode(const float32_t* x, size_t n, char* out, size_t out_sz, mfsk16_result_t* res)
{
    memset(res, 0, sizeof(*res));
    res->period_samples = MF_SYMBOL;
    res->header = -1;
    if (n < (size_t)(2 * MF_SYMBOL)) return 0;

    // The symbol phase: the alignment that puts the most energy in one tone per symbol. A phase half
    // a symbol out straddles two symbols and splits the energy between two tones, so the ratio of the
    // winning tone to the mean over the sixteen is the thing to maximise over the first symbols.
    int best_phase = 0;
    double best_ratio = -1.0;
    const size_t probe = n / MF_SYMBOL < 12 ? n / MF_SYMBOL : 12;
    for (int ph = 0; ph < MF_SYMBOL; ph += 4)
    {
        double ratio = 0.0;
        double e[MF_TONES];
        for (size_t s = 0; s < probe; s++)
        {
            const size_t at = (size_t)ph + s * MF_SYMBOL;
            if (at + MF_SYMBOL > n) break;
            mf_energies(x, at, e);
            double max = 0.0, sum = 0.0;
            for (int t = 0; t < MF_TONES; t++)
            {
                sum += e[t];
                if (e[t] > max) max = e[t];
            }
            ratio += max / (sum / MF_TONES + 1e-12);
        }
        if (ratio > best_ratio) { best_ratio = ratio; best_phase = ph; }
    }
    res->phase = best_phase;

    const size_t nsym = (n - (size_t)best_phase) / MF_SYMBOL;
    if (nsym < MF_BLOCK) return 0;
    const size_t nblocks = nsym / MF_BLOCK;
    static uint8_t rx[8192], coded[8192], info[4096];
    if (nblocks * 32 > sizeof(rx)) return 0;

    double e[MF_TONES];
    for (size_t s = 0; s < nblocks * MF_BLOCK; s++)
    {
        const size_t at = (size_t)best_phase + s * MF_SYMBOL;
        mf_energies(x, at, e);
        int winner = 0;
        double max = -1.0, sum = 0.0;
        for (int t = 0; t < MF_TONES; t++)
        {
            sum += e[t];
            if (e[t] > max) { max = e[t]; winner = t; }
        }
        // the winning tone holding more than half of the energy is a symbol that was heard, and it is
        // the number the table reports as a rate: sixteen equal tones is a symbol with nothing in it
        res->symbols++;
        if (max > 0.5 * sum) res->tones_ok++;
        for (int b = 0; b < 4; b++) rx[s * 4 + b] = (uint8_t)((winner >> b) & 1);
    }
    res->bits = nblocks * 32;
    mf_deinterleave(rx, coded, nblocks);
    const size_t nb = mf_viterbi(coded, res->bits, info);
    res->viterbi_bits = (unsigned)nb;
    if (nb < 8) return 0;

    int hdr = 0;
    for (int k = 0; k < 8; k++) hdr |= (int)info[k] << k;
    res->header = hdr;

    size_t at = 0;
    // the stream says how long the text is: without that the padding the interleaver needs would
    // come out as trailing spaces, and a reader cannot tell them from text
    int nchars = hdr;
    if (nchars > (int)((nb - 8) / 6)) nchars = (int)((nb - 8) / 6);
    for (int i = 0; i < nchars && at + 1 < out_sz; i++)
    {
        int v = 0;
        for (int k = 0; k < 6; k++) v |= (int)info[8 + 6 * (size_t)i + (size_t)k] << k;
        if (v == 0) out[at++] = ' ';
        else if (v <= MF_ALPHA_LEN) out[at++] = mf_alpha[v];
        else out[at++] = '?';
    }
    out[at] = 0;
    return 1;
}
