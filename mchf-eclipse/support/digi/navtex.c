/* NAVTEX / SITOR-B for the UHSDR digital mode host bench.
 *
 * NAVTEX is the maritime broadcast half of SITOR: 100 Bd FSK with a 170 Hz shift, carrying the
 * CCIR 476 seven bit alphabet. Two properties do all the work and both are tested here rather than
 * assumed:
 *
 *   - every valid character has exactly four marks and three spaces, so a single bit error shows up
 *     as a word that is not in the table at all. That is detection for free, and it is also what
 *     finds the character boundary: seven contiguous bits that are not 4-of-7 out of alignment are
 *     mostly not 4-of-7 in alignment either.
 *   - there is no return path, so the correction is time diversity: each character is sent twice,
 *     the direct (DX) copy and a repeat (RX) copy five character slots (350 ms) later. A burst that
 *     destroys one copy usually spares the other, and the two can be combined when both are damaged.
 *
 * The receiver here is a reference implementation for measurement, not firmware: it fits the bit
 * clock and the seven bit group boundary by maximising the number of 4-of-7 words, then combines each
 * pair of copies by maximum agreement over the 35 valid codes. Nothing is fed to it that the
 * transmitter did not send, and the same code is used to generate and to score, which is the same
 * arrangement the fax path has: it establishes the mode and its failure modes, and it is not evidence
 * that a real station was copied.
 *
 * Bit order: CCIR 476 is written most significant bit first, and SITOR sends each character least
 * significant bit first, as ITA2 does. A reversed bit order is still 4-of-7, so a receiver cannot
 * tell from the ratio alone - but the phasing pair it is about to look for is 0x0F/0x33, whose
 * reversals are 0x78/0x66, so the receiver detects the order from the phasing and says which it used.
 * The transmitter here can key either order, phasing included, and can also key a take whose phasing
 * is in one order and whose text is in the other: the first is a decode the receiver has to get from
 * the phasing alone, the second prices a detection that guessed wrong instead of assuming it is free.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "navtex.h"

#define NV_RATE      12000.0
#define NV_SPACE_HZ   1615.0      // '0', the low tone
#define NV_MARK_HZ    1785.0      // '1', the high tone: SITOR makes the high tone the mark
#define NV_BIT_SAMPLES 120        // exactly one bit at 12 kHz
#define NV_PHASING      20        // phasing characters before the text, alternating SIA and SIB

// the characters SITOR uses beyond the 32 ITA2 code points
#define NV_SIA   0x0F             // phasing signal 1
#define NV_SIB   0x33             // phasing signal 2
#define NV_FIGS  0x36
#define NV_LTRS  0x5A
#define NV_SP    0x5C
#define NV_RPT   0x66
#define NV_BLK   0x6A

/* The alphabet, indexed by the seven bit code. Zero is "not a character": only the 35 constant ratio
 * words are filled in, which is what makes the table its own validator.
 *
 * Two entries have no printable ASCII: BEL (figures 0x17) and ENQ (figures 0x53) are mapped to a
 * space, and the pound sign (figures 0x69) to '#', so a decode can be compared as text. NAVTEX
 * messages are upper case letters, digits and a few stops, so nothing in this bench's messages is
 * affected - the mapping is written down rather than left to be discovered. */
static const char nv_letters[128] =
{
    [0x17] = 'J', [0x1B] = 'F', [0x1D] = 'C', [0x1E] = 'K',
    [0x27] = 'W', [0x2B] = 'Y', [0x2D] = 'P', [0x2E] = 'Q',
    [0x35] = 'G', [0x39] = 'M', [0x3A] = 'X', [0x3C] = 'V',
    [0x47] = 'A', [0x4B] = 'S', [0x4D] = 'I', [0x4E] = 'U',
    [0x53] = 'D', [0x55] = 'R', [0x56] = 'E', [0x59] = 'N',
    [0x5A] = '\0', [0x5C] = ' ',
    [0x63] = 'Z', [0x65] = 'L', [0x69] = 'H',
    [0x6C] = ' ', [0x71] = 'O', [0x72] = 'B', [0x74] = 'T', [0x78] = ' ',
};

static const char nv_figures[128] =
{
    [0x17] = ' ', [0x1B] = '!', [0x1D] = ':', [0x1E] = '(',
    [0x27] = '2', [0x2B] = '6', [0x2D] = '0', [0x2E] = '1',
    [0x35] = '&', [0x39] = '.', [0x3A] = '/', [0x3C] = '=',
    [0x47] = '-', [0x4B] = '\'', [0x4D] = '8', [0x4E] = '7',
    [0x53] = ' ', [0x55] = '4', [0x56] = '3', [0x59] = ',',
    [0x5A] = '\0', [0x5C] = ' ',
    [0x63] = '+', [0x65] = ')', [0x69] = '#',
    [0x6C] = ' ', [0x71] = '9', [0x72] = '?', [0x74] = '5', [0x78] = ' ',
};

static int nv_popcount(unsigned v)
{
    int n = 0;
    for (; v; v >>= 1) n += (int)(v & 1u);
    return n;
}

// the same word with its seven bits reversed: what a take with the other bit order looks like
static unsigned nv_reverse(unsigned v)
{
    unsigned r = 0;
    for (int b = 0; b < 7; b++) r |= ((v >> b) & 1u) << (6 - b);
    return r;
}

// the 35 valid codes, in ascending order. All of them, including the four SITOR control words: a
// receiver that only offered the printable ones would have to turn every phasing or idle character
// into the nearest letter, and the end of every transmission would print as text.
static const unsigned* nv_codes(int* count)
{
    static unsigned codes[35];
    static int n = -1;
    if (n < 0)
    {
        n = 0;
        for (unsigned c = 0; c < 128; c++)
            if (nv_popcount(c) == 4) codes[n++] = c;
    }
    *count = n;
    return codes;
}

// the code the transmitter sends for a character in a given shift, 0 when the character is not in it
static unsigned nv_code_for(char c, int figures)
{
    const char* table = figures ? nv_figures : nv_letters;
    for (unsigned c2 = 0; c2 < 128; c2++)
        if (table[c2] == c) return c2;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  transmitter
 * ------------------------------------------------------------------ */
static size_t nv_encode(const char* text, uint8_t* out, size_t cap)
{
    size_t n = 0;
    int figures = 0;
    for (const char* p = text; *p && n + 3 < cap; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c == ' ') { out[n++] = NV_SP; continue; }
        unsigned code = nv_code_for(c, figures);
        if (!code)
        {
            // not in the shift in use: send the shift, then the character
            unsigned other = nv_code_for(c, !figures);
            if (!other) continue;                       // not in the alphabet at all
            out[n++] = figures ? NV_LTRS : NV_FIGS;
            figures = !figures;
            code = other;
        }
        out[n++] = (uint8_t)code;
    }
    return n;
}

// SITOR-B: each block of five characters is sent twice, the repeat five slots later
static size_t nv_frame(const uint8_t* codes, size_t n, uint8_t* out, size_t cap)
{
    size_t m = 0;
    for (size_t i = 0; i < n; i += 5)
    {
        uint8_t blk[5];
        for (int k = 0; k < 5; k++) blk[k] = (i + (size_t)k < n) ? codes[i + (size_t)k] : (uint8_t)NV_BLK;
        for (int copy = 0; copy < 2; copy++)
            for (int k = 0; k < 5 && m < cap; k++) out[m++] = blk[k];
    }
    return m;
}

size_t Bench_NavtexGenerate(float32_t* out, size_t cap, const char* text, float tune_hz, int order)
{
    // tune_hz moves both tones by the same number of Hz, which is what a reference or tuning error
    // does to a received pair: the shift is unchanged and the receiver's fixed bins no longer sit on
    // the tones. Shifting the generated pair is how a receiver sees it, rather than filtering the
    // audio afterwards.
    const double lo_hz = NV_SPACE_HZ + (double)tune_hz;
    const double hi_hz = NV_MARK_HZ + (double)tune_hz;
    uint8_t codes[1024], framed[4096], all[4200];
    const size_t nc = nv_encode(text, codes, sizeof(codes));
    const size_t nf = nv_frame(codes, nc, framed, sizeof(framed));
    size_t na = 0;
    // the phasing signal: at least 15 characters of alternating SIA and SIB, which is what tells a
    // receiver where the seven bit groups are before there is any text to find them in
    for (int i = 0; i < NV_PHASING; i++) all[na++] = (i & 1) ? NV_SIB : NV_SIA;
    memcpy(all + na, framed, nf);
    na += nf;
    // end of emission: phasing signal 1 three times
    for (int i = 0; i < 3; i++) all[na++] = NV_SIA;

    size_t n = 0;
    double phase = 0.0;
    for (size_t c = 0; c < na; c++)
    {
        // The bit order belongs to the transmitter, so it is applied to the word before it is keyed
        // rather than by walking the bits backwards here: the reversed code is what the other
        // convention puts on the air, and it is still 4-of-7. The phasing pair goes with it - that
        // is what lets the receiver detect the order - unless the take is the mixed one, which
        // deliberately leaves the phasing in the table's order so the receiver is told the wrong one.
        unsigned word = all[c];
        if (order == NAVTEX_ORDER_REVERSED || (order == NAVTEX_ORDER_MIXED && c >= NV_PHASING))
            word = nv_reverse(word);
        for (int b = 0; b < 7; b++)
        {
            const double f = ((word >> b) & 1u) ? hi_hz : lo_hz;
            for (int s = 0; s < NV_BIT_SAMPLES && n < cap; s++)
            {
                out[n++] = (float32_t)(0.4 * sin(phase));
                phase += 2.0 * M_PI * f / NV_RATE;
                if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ *
 *  receiver
 * ------------------------------------------------------------------ */
// energy at each tone over the one bit window ending at every sample. A sliding sum, so the whole
// take costs two multiplications and a subtraction per sample rather than a DFT per bit.
static double* nv_discriminator(const float32_t* x, size_t n)
{
    double* d = malloc((n ? n : 1) * sizeof(double));
    if (!d) return NULL;
    const double wl = 2.0 * M_PI * NV_SPACE_HZ / NV_RATE;
    const double wh = 2.0 * M_PI * NV_MARK_HZ / NV_RATE;
    double rl_i = 0.0, rl_q = 0.0, rh_i = 0.0, rh_q = 0.0;
    for (size_t k = 0; k < n; k++)
    {
        const double cl = cos(wl * (double)k), sl = sin(wl * (double)k);
        const double ch = cos(wh * (double)k), sh = sin(wh * (double)k);
        rl_i += (double)x[k] * cl; rl_q -= (double)x[k] * sl;
        rh_i += (double)x[k] * ch; rh_q -= (double)x[k] * sh;
        if (k >= NV_BIT_SAMPLES)
        {
            const double j = (double)(k - NV_BIT_SAMPLES);
            const double xj = (double)x[k - NV_BIT_SAMPLES];
            rl_i -= xj * cos(wl * j); rl_q += xj * sin(wl * j);
            rh_i -= xj * cos(wh * j); rh_q += xj * sin(wh * j);
        }
        d[k] = (rh_i * rh_i + rh_q * rh_q) - (rl_i * rl_i + rl_q * rl_q);
    }
    return d;
}

// read the seven bit word whose first bit starts at `first`, with `per` samples to a bit
static unsigned nv_read_word(const double* d, size_t n, double first, double per)
{
    unsigned code = 0;
    for (int b = 0; b < 7; b++)
    {
        const long end = lround(first + (double)(b + 1) * per) - 1;   // last sample of that bit
        if (end < 0 || (size_t)end >= n) return 0xFFFFu;
        code |= (unsigned)(d[end] > 0.0) << b;
    }
    return code;
}

// how many of the words at this alignment are members of the alphabet. The constant ratio is the
// whole synchroniser: seven bits taken across a group boundary land on 4-of-7 words by accident
// about a fifth of the time, seven bits taken inside one do it always.
static int nv_alignment_score(const double* d, size_t n, double start, double per, int offset,
                              int* words, double* margin)
{
    int valid = 0;
    *words = 0;
    *margin = 0.0;
    for (int j = 0;; j++)
    {
        const double first = start + (double)(offset + 7 * j) * per;
        if (first + 7.0 * per >= (double)n) break;
        const unsigned code = nv_read_word(d, n, first, per);
        if (code == 0xFFFFu) break;
        (*words)++;
        if (nv_popcount(code) == 4) valid++;
        for (int b = 0; b < 7; b++)
        {
            const long end = lround(first + (double)(b + 1) * per) - 1;
            *margin += fabs(d[end]);
        }
    }
    return valid;
}

// the longest stretch of alternating phasing signals: SIA/SIB in the table's bit order, or their
// reversals when the take carries the other one
static int nv_phasing_run(const unsigned* grp, int ng, int reversed, int* end)
{
    int best = 0, best_end = 0, run = 0;
    unsigned prev = 0;
    for (int j = 0; j < ng; j++)
    {
        const unsigned c = reversed ? nv_reverse(grp[j]) : grp[j];
        if (c == NV_SIA || c == NV_SIB)
        {
            if (run > 0 && c != prev) run++;
            else run = 1;
            prev = c;
            if (run > best) { best = run; best_end = j + 1; }
        }
        else run = 0;
    }
    *end = best_end;
    return best;
}

int Bench_NavtexDecode(const float32_t* x, size_t n, char* out, size_t out_sz, navtex_result_t* res)
{
    memset(res, 0, sizeof(*res));
    if (out_sz) out[0] = '\0';
    if (n < 4096) return 0;
    double* d = nv_discriminator(x, n);
    if (!d) return 0;

    // the bit clock and the group boundary, searched together on the one property the mode gives
    // away: a run of 4-of-7 words. The period is searched as well as the phase because a recording
    // through a receiver is not guaranteed to be at the transmitter's rate, and a fit that reports
    // the rate it found is more useful than one that assumes it.
    double best_start = 0.0, best_per = NV_BIT_SAMPLES, best_frac = -1.0, best_margin = -1.0;
    int best_offset = 0;
    for (double per = 118.0; per <= 122.0; per += 0.5)
    {
        for (int p = 0; p < NV_BIT_SAMPLES; p++)
        {
            for (int o = 0; o < 7; o++)
            {
                int words = 0;
                double margin = 0.0;
                const int valid = nv_alignment_score(d, n, (double)p, per, o, &words, &margin);
                if (words < 40) continue;
                const double frac = (double)valid / (double)words;
                if (frac > best_frac + 1e-9
                    || (fabs(frac - best_frac) <= 1e-9 && margin > best_margin))
                {
                    best_frac = frac;
                    best_margin = margin;
                    best_start = (double)p;
                    best_per = per;
                    best_offset = o;
                }
            }
        }
    }
    res->valid_fraction = best_frac;
    res->period_samples = best_per;

    // the words at that alignment, then the phasing run that says which bit order the take carries
    int ng = 0;
    for (int j = 0;; j++)
    {
        const double first = best_start + (double)(best_offset + 7 * j) * best_per;
        if (first + 7.0 * best_per >= (double)n) break;
        ng++;
    }
    if (ng < 20) { free(d); return 0; }
    unsigned* grp = malloc((size_t)ng * sizeof(unsigned));
    if (!grp) { free(d); return 0; }
    for (int j = 0; j < ng; j++)
        grp[j] = nv_read_word(d, n, best_start + (double)(best_offset + 7 * j) * best_per, best_per);
    free(d);

    int end_normal = 0, end_rev = 0;
    const int run_normal = nv_phasing_run(grp, ng, 0, &end_normal);
    const int run_rev = nv_phasing_run(grp, ng, 1, &end_rev);
    const int reversed = run_rev > run_normal;
    const int start = reversed ? end_rev : end_normal;
    res->reversed = reversed ? 1 : 0;
    res->phasing_run = reversed ? run_rev : run_normal;

    int count = 0;
    const unsigned* codes = nv_codes(&count);
    int letters = 1;
    size_t used = 0;
    // every block of five characters is sent twice, the repeat five slots later, so a character is
    // the best agreement between slot k and slot k+5 across all 35 words. The block phase does not
    // matter: pairing any slot with the one five later pairs a character with itself.
    for (int base = start; base + 9 < ng; base += 10)
    {
        for (int k = 0; k < 5; k++)
        {
            const unsigned a = reversed ? nv_reverse(grp[base + k]) : grp[base + k];
            const unsigned b = reversed ? nv_reverse(grp[base + k + 5]) : grp[base + k + 5];
            unsigned best = 0;
            int ok = 0;
            const int pa = nv_popcount(a), pb = nv_popcount(b);
            if (pa == 4 || pb == 4)
            {
                // A copy that is not a constant ratio word is a character the receiver has already
                // detected as broken, and the mode's correction is time diversity, not guessing at
                // bits inside a damaged character: take the copy that is still a word. This is the
                // burst case - one copy erased to nothing reads as 0x00, which is four bits from any
                // word, so scoring the pair together would reject a character that is sitting there
                // intact in the other copy.
                best = (pa == 4) ? a : b;
                ok = 1;
            }
            else
            {
                // neither copy is a word on its own: correct against the pair. One bit wrong in
                // each copy puts the true word two bits away from both.
                int best_dist = 99;
                for (int c = 0; c < count; c++)
                {
                    const int dist = nv_popcount(codes[c] ^ a) + nv_popcount(codes[c] ^ b);
                    if (dist < best_dist) { best_dist = dist; best = codes[c]; }
                }
                ok = best_dist <= 3;
            }
            if (!ok) { res->both_bad++; continue; }   // neither copy survived
            if (a != b) res->repaired++;
            if (best == NV_LTRS) { letters = 1; continue; }
            if (best == NV_FIGS) { letters = 0; continue; }
            if (best == NV_SIA || best == NV_SIB || best == NV_RPT || best == NV_BLK) continue;
            const char c = letters ? nv_letters[best] : nv_figures[best];
            if (!c) continue;
            if (used + 1 < out_sz) out[used++] = c;
        }
    }
    out[used] = '\0';
    free(grp);
    return 1;
}
