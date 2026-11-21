// MFSK16 reference modem for the digi bench.
//
// The mode: sixteen tones 15.625 Hz apart, keyed one tone per symbol at 15.625 baud, so a symbol is
// 64 ms and four bits, and the whole signal is 234 Hz wide plus the tone itself. What makes MFSK16
// work at all is the forward error correction and the interleaver: a symbol that survives is worth
// four bits, and a symbol lost to a burst is worth nothing unless the bits it carried were spread
// over its neighbours. The bench keeps that structure: a rate 1/2 constraint length 7 convolutional
// code, a block interleaver over eight symbols, and hard decisions from the tone energies.
//
// The alphabet is the mode's own: the IZ8BLY MFSK Varicode, as published by Murray Greenman ZL1BPU
// ("The IZ8BLY MFSK Varicode", 10 July 2000, cut from Nino IZ8BLY's source). Every character is a
// run of bits starting with 1 and terminated by "00", so the stream frames itself and needs no
// header: the decoder finds each character at the 1 that follows a run of at least two zeros.
// Varicode is not the PSK31 code - this one may contain runs of more than two zeros inside a code -
// which is why the decoder separates characters on a 1 after a zero run rather than on the first
// "00" it sees.
//
// Two things here are still the bench's and the README says so: the interleaver is a block of eight
// symbols rather than the mode's ten concatenated 4x4 diagonal interleavers, and the take ends with
// a NUL, the idle character fldigi sends when it has nothing to transmit, followed by one start bit.
// A NUL is needed because codes may end in zeros (every digit does): zero padding after the text is
// indistinguishable from the tail of the last code, and no amount of it can mark the end.

#include "mfsk16.h"

#include <math.h>
#include <string.h>

#define MF_RATE     12000.0
#define MF_SPACING  15.625
#define MF_TONES    16
#define MF_BASE     1000.0
#define MF_SYMBOL   768                 // samples: 64 ms at 12 kHz
#define MF_G1       0x79u               // octal 171
#define MF_G2       0x5Bu               // octal 133
#define MF_BLOCK    8                   // symbols per interleaver block, 32 coded bits
#define MF_MAX_STEPS 4096                // trellis steps: 300 maximum length Varicode characters

// The IZ8BLY MFSK Varicode, indexed by 7 bit ASCII, with the framing "00" stripped: the table on
// ZL1BPU's page lists each code with its terminator, so "100" there is the space and is "1" here.
// Codes 30, 31 and 127 end in a zero in the published table, which the framing cannot represent;
// they are control codes the bench never sends, and the encoder falls back to '?' for anything
// without an entry.
static const char* mf_varicode[128] =
{
    [0] = "111010111",      [1] = "1110110000",     [2] = "1110110100",
    [3] = "111011011",      [4] = "1110111000",     [5] = "111011101",
    [6] = "1110111100",     [7] = "111011111",      [8] = "101010",
    [9] = "1111000000",     [10] = "1111010000",    [11] = "1111010100",
    [12] = "111101011",     [13] = "101011",        [14] = "1111011000",
    [15] = "111101101",     [16] = "1111011100",    [17] = "111101111",
    [18] = "1111100000",    [19] = "1111101000",    [20] = "111110101",
    [21] = "1111101100",    [22] = "111110111",     [23] = "1111110000",
    [24] = "1111110100",    [25] = "111111011",     [26] = "1111111000",
    [27] = "111111101",     [28] = "1111111100",    [29] = "111111111",
    [30] = "1000000000",    [31] = "1010000000",
    [32] = "1",             [33] = "1110000",       [34] = "1111111",
    [35] = "101101100",     [36] = "101010100",     [37] = "101010000",
    [38] = "10000000",      [39] = "1101111",       [40] = "1111101",
    [41] = "1111100",       [42] = "10101101",      [43] = "1111000",
    [44] = "101000",        [45] = "1110110",       [46] = "1110101",
    [47] = "1111010",       [48] = "111000",        [49] = "111100",
    [50] = "1010000",       [51] = "1010101",       [52] = "1011101",
    [53] = "1011000",       [54] = "1011011",       [55] = "1101000",
    [56] = "1100000",       [57] = "1101011",       [58] = "1111011",
    [59] = "1111110",       [60] = "101100000",     [61] = "1110111",
    [62] = "10101111",      [63] = "1110100",       [64] = "101000000",
    [65] = "101111",        [66] = "1000000",       [67] = "110101",
    [68] = "110111",        [69] = "101110",        [70] = "111110",
    [71] = "1010100",       [72] = "1010110",       [73] = "110000",
    [74] = "1101101",       [75] = "1011111",       [76] = "111101",
    [77] = "111010",        [78] = "111111",        [79] = "110100",
    [80] = "111011",        [81] = "1101100",       [82] = "110110",
    [83] = "101101",        [84] = "101100",        [85] = "1010111",
    [86] = "1101010",       [87] = "1011010",       [88] = "1011100",
    [89] = "1011110",       [90] = "1101110",       [91] = "101110100",
    [92] = "101101000",     [93] = "10111011",      [94] = "10110101",
    [95] = "101011000",     [96] = "10101011",      [97] = "101",
    [98] = "11000",         [99] = "1110",          [100] = "1101",
    [101] = "10",           [102] = "10100",        [103] = "10110",
    [104] = "1100",         [105] = "110",          [106] = "100000",
    [107] = "11100",        [108] = "1011",         [109] = "10000",
    [110] = "111",          [111] = "100",          [112] = "10101",
    [113] = "11110",        [114] = "1000",         [115] = "1010",
    [116] = "11",           [117] = "1111",         [118] = "11011",
    [119] = "11010",        [120] = "11101",        [121] = "10111",
    [122] = "11111",        [123] = "10110111",     [124] = "101011100",
    [125] = "101110000",    [126] = "101111000",    [127] = "1010100000",
};

// the character a code stands for, -1 when the bits are not in the table (0 is the NUL, the idle)
static int mf_varicode_char(unsigned code, int len)
{
    for (int c = 0; c < 128; c++)
    {
        const char* p = mf_varicode[c];
        if (!p || (int)strlen(p) != len) continue;
        unsigned v = 0;
        for (; *p; p++) v = (v << 1) | (unsigned)(*p - '0');
        if (v == code) return c;
    }
    return -1;
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

// MF_MAX_STEPS trellis steps, two coded bits a step and four coded bits a symbol: the audio that
// fits is half as many symbols as there are steps.
size_t Bench_Mfsk16MaxSamples(void) { return (size_t)(MF_MAX_STEPS / 2) * (size_t)MF_SYMBOL; }

size_t Bench_Mfsk16Generate(float32_t* out, size_t cap, const char* text, float tune_hz)
{
    static uint8_t bits[8192], coded[8192], tx[8192];
    const size_t len = strlen(text);
    const int nchars = len > 300 ? 300 : (int)len;
    size_t nb = 0;

    // Varicode: the character's bits and then "00". Four bits to a symbol, so the padding the
    // interleaver needs is extra zeros, which decode as idle and print nothing.
    for (int i = 0; i < nchars; i++)
    {
        int c = (unsigned char)text[i];
        const char* p = c < 128 ? mf_varicode[c] : NULL;
        if (!p) p = mf_varicode['?'];
        for (; *p; p++) bits[nb++] = (uint8_t)(*p - '0');
        bits[nb++] = 0;
        bits[nb++] = 0;
    }
    // the idle NUL, then one start bit for the character after it: without a following 1 the NUL is
    // not framed, and without the NUL the padding zeros cannot say where the text ended
    for (const char* p = mf_varicode[0]; *p; p++) bits[nb++] = (uint8_t)(*p - '0');
    bits[nb++] = 0;
    bits[nb++] = 0;
    bits[nb++] = 1;
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
    static uint8_t pred[MF_MAX_STEPS * S];
    static uint8_t pick[MF_MAX_STEPS * S];
    if (steps > MF_MAX_STEPS) return 0;
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
    if (n < (size_t)(2 * MF_SYMBOL)) return 0;

    // The symbol phase: the alignment that puts the most energy in one tone per symbol. A phase half
    // a symbol out straddles two symbols and splits the energy between two tones, so the ratio of the
    // winning tone to the mean over the sixteen is the thing to maximise over the first symbols.
    int best_phase = 0;
    double best_ratio = -1.0;
    const size_t probe = n / MF_SYMBOL < 12 ? n / MF_SYMBOL : 12;
    // Only the first half symbol is searched: a phase past half a symbol puts the first window
    // mostly inside the next symbol, which reads the whole interleaver one symbol late. The
    // generators start the take on a symbol boundary, so there is no earlier phase to find.
    for (int ph = 0; ph < MF_SYMBOL / 2; ph += 4)
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
    const size_t nsym = (n - (size_t)best_phase) / MF_SYMBOL;
    if (nsym < MF_BLOCK) return 0;
    // A phase that is not zero leaves the last block short by up to one symbol. Rounding down would
    // throw the whole block away - with it the idle NUL that ends the message - so the block is
    // completed with zero bits, which is what an erasure looks like to the decoder.
    const size_t nblocks = (nsym + MF_BLOCK - 1) / MF_BLOCK;
    static uint8_t rx[8192], coded[8192], info[4096];
    if (nblocks * 32 > sizeof(rx)) return 0;
    memset(rx, 0, nblocks * 32);

    double e[MF_TONES];
    for (size_t s = 0; s < nsym; s++)
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
    if (nb < 4) return 0;

    // Varicode framing. A character starts at a 1 and ends two bits before the next 1 that follows a
    // run of at least two zeros; the zeros before that 1 are the delimiter, and a code that itself
    // ends in zeros keeps them, because only the last two are removed. The NUL closes the message.
    size_t at = 0;
    unsigned acc = 0;
    int len = 0;
    for (size_t i = 0; i < nb; i++)
    {
        const int bit = info[i];
        if (len == 0)
        {
            if (!bit) continue;
            acc = 1;
            len = 1;
            continue;
        }
        acc = (acc << 1) | (unsigned)bit;
        len++;
        if (bit == 1 && len >= 4 && (acc & 6u) == 0u)
        {
            const int c = mf_varicode_char(acc >> 3, len - 3);
            acc = 1;
            len = 1;
            res->characters++;
            if (c == 0) break;                  // the idle NUL: the message ended
            if (c > 0 && at + 1 < out_sz) out[at++] = (char)c;
            continue;
        }
        // the longest code is ten bits, plus the two of the delimiter and up to fifteen of the
        // padding the interleaver needs: anything longer is not a character, so resync on it
        if (len > 30) { acc = 0; len = 0; }
    }
    out[at] = 0;
    return 1;
}
