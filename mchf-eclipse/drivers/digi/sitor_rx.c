/*  -*-  mode: c; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4; coding: utf-8  -*-  */
/************************************************************************************
 **                                                                                **
 **                                        UHSDR                                   **
 **               a powerful firmware for STM32 based SDR transceivers             **
 **                                                                                **
 **--------------------------------------------------------------------------------**
 **                                                                                **
 **  Description:   SITOR-B / NAVTEX receive demodulator, streaming                **
 **  Licence:		GNU GPLv3                                                      **
 ************************************************************************************/

#include "sitor_rx.h"

#include <math.h>
#include <string.h>

/* The CCIR 476 alphabet, as the two shift tables.  Zero means "not a character", and only the 35
 * constant ratio words are filled in, so the table is its own validator: a word with the wrong number
 * of marks simply has no entry.  BEL and ENQ have no printable ASCII and are mapped to a space, and
 * the pound sign to '#', which is written down rather than left to be discovered.
 *
 * support/digi/navtex.c used to keep its own copies of these two tables.  It now uses these, because
 * a reference modem and a radio that disagree about what 0x4B means would make every measurement here
 * a measurement of something the radio does not do. */
const char SitorLetters[128] =
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

const char SitorFigures[128] =
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

// the character whose first bit arrived `a` bits before the newest one in the history
static unsigned sitor_reverse(unsigned v)
{
    unsigned r = 0;
    for (int b = 0; b < 7; b++) r |= ((v >> b) & 1u) << (6 - b);
    return r;
}

static unsigned sitor_word_at(const SitorRx* s, int a)
{
    // The history holds the newest bit low, so the window's oldest bit - the one SITOR sends first
    // and weights lowest - is at the top of it and the seven have to be turned round.  Skipping this
    // is invisible to everything downstream that only asks whether a word is 4-of-7, since a reversed
    // constant-ratio code is another constant-ratio code: the confidence reads 0.98 while every
    // character is wrong, and the only reason the bench notices is that it knows the text.
    const unsigned w = (s->hist >> a) & 0x7Fu;
    return sitor_reverse(w);
}

// how many of the seven bits are set, and whether the word is one of the 35 the mode can send
static int sitor_popcount(unsigned v)
{
    int n = 0;
    for (; v; v >>= 1) n += (int)(v & 1u);
    return n;
}

static int sitor_is_word(unsigned v)
{
    return sitor_popcount(v & 0x7Fu) == 4;
}


// The 35 valid codes, built once from the table rather than written out: the constant ratio property
// is the definition, and a list copied from somewhere else is a list that can be wrong.
static const unsigned* sitor_codes(int* count)
{
    static unsigned codes[35];
    static int n = -1;
    if (n < 0)
    {
        n = 0;
        for (unsigned c = 0; c < 128; c++)
            if (sitor_popcount(c) == 4) codes[n++] = c;
    }
    *count = n;
    return codes;
}

// The tone front end.  The bandwidth is what the pole is for: 50 Hz around each tone is about the
// widest filter that still separates a 170 Hz shift at 100 Bd, and narrower would start to close the
// eye.  The phasors are renormalised every 256 samples because a rotation by a fixed angle is only
// orthogonal to first order in the multiply, and the discriminator is a *difference of energies*, so
// a magnitude that walks walks the decision threshold with it.
#define SITOR_FILTER_HZ   50.0f
#define SITOR_TRIM_EVERY  256
#define SITOR_TRACK_PHASE 0.05f   // how hard a transition pulls the sampling point
#define SITOR_TRACK_RATE  0.0006f // and how hard it pulls the bit period, which is the slower fix

void SitorRx_Init(SitorRx* s, float32_t sample_rate_hz, float32_t low_hz, float32_t high_hz)
{
    memset(s, 0, sizeof(*s));

    const float32_t w_lo = 2.0f * (float32_t)M_PI * low_hz / sample_rate_hz;
    const float32_t w_hi = 2.0f * (float32_t)M_PI * high_hz / sample_rate_hz;
    s->rot_lo_c = cosf(w_lo);
    s->rot_lo_s = sinf(w_lo);
    s->rot_hi_c = cosf(w_hi);
    s->rot_hi_s = sinf(w_hi);
    s->ph_lo_c = 1.0f;
    s->ph_hi_c = 1.0f;

    s->alpha = 2.0f * (float32_t)M_PI * SITOR_FILTER_HZ / sample_rate_hz;

    s->period = sample_rate_hz / 100.0f;     // the nominal 100 Bd, until the measurement replaces it
    s->half = s->period * 0.5f;
    s->state = SITOR_SEARCH_RATE;
    s->letters = 1;
}

int SitorRx_Sample(SitorRx* s, float32_t x)
{
    s->out_char = 0;

    // ---- the two tones, and which one is winning -------------------------------------------
    // baseband = x shifted to the tone, then one pole.  x is real, so the complex multiply is two
    // multiplies and the lowpass two more.
    const float32_t bi = x * s->ph_lo_c, bq = -x * s->ph_lo_s;
    const float32_t di = x * s->ph_hi_c, dq = -x * s->ph_hi_s;
    s->lp_lo_i += s->alpha * (bi - s->lp_lo_i);
    s->lp_lo_q += s->alpha * (bq - s->lp_lo_q);
    s->lp_hi_i += s->alpha * (di - s->lp_hi_i);
    s->lp_hi_q += s->alpha * (dq - s->lp_hi_q);

    const float32_t pa = s->ph_lo_c, pb = s->ph_lo_s;
    s->ph_lo_c = pa * s->rot_lo_c - pb * s->rot_lo_s;
    s->ph_lo_s = pa * s->rot_lo_s + pb * s->rot_lo_c;
    const float32_t qa = s->ph_hi_c, qb = s->ph_hi_s;
    s->ph_hi_c = qa * s->rot_hi_c - qb * s->rot_hi_s;
    s->ph_hi_s = qa * s->rot_hi_s + qb * s->rot_hi_c;
    if ((s->sample_count++ & (SITOR_TRIM_EVERY - 1)) == SITOR_TRIM_EVERY - 1)
    {
        const float32_t rl = sqrtf(s->ph_lo_c * s->ph_lo_c + s->ph_lo_s * s->ph_lo_s);
        const float32_t rh = sqrtf(s->ph_hi_c * s->ph_hi_c + s->ph_hi_s * s->ph_hi_s);
        if (rl > 1e-6f) { s->ph_lo_c /= rl; s->ph_lo_s /= rl; }
        if (rh > 1e-6f) { s->ph_hi_c /= rh; s->ph_hi_s /= rh; }
    }

    const float32_t e_lo = s->lp_lo_i * s->lp_lo_i + s->lp_lo_q * s->lp_lo_q;
    const float32_t e_hi = s->lp_hi_i * s->lp_hi_i + s->lp_hi_q * s->lp_hi_q;
    s->disc = e_hi - e_lo;

    // ---- transitions, which are the only clock information an FSK signal gives ------------
    const int bit_now = s->disc > 0.0f ? 1 : 0;
    const int edge = (bit_now != s->last_bit);
    s->last_bit = bit_now;

    s->phase += 1.0f;
    s->since_edge++;

    if (s->state == SITOR_SEARCH_RATE)
    {
        s->edges[s->edge_fill++] = (uint8_t)edge;
        if (s->edge_fill >= SITOR_EDGE_WINDOW)
        {
            // A transition belongs on a bit boundary, so folding every transition in the window onto
            // a candidate period piles them into one bin when the period is right and spreads them
            // over P bins when it is not.  The score is how much of the pile the fullest bin holds,
            // relative to an even spread: 1.0 is no structure at all, and the true period reads
            // several times that.  Folding is used rather than a plain autocorrelation because a
            // correlation at one exact lag is destroyed by the sample-to-sample jitter a 50 Hz filter
            // puts on the zero crossing - the first version of this asked for eight transitions to
            // land on the same lag twice and found nothing on a clean take.
            int32_t total = 0;
            for (int i = 0; i < SITOR_EDGE_WINDOW; i++) total += s->edges[i];
            int best_p = 0, best_bin = 0, best_n = 0;
            float best_score = 0.0f;
            // A transition every other bit at 100 Bd is about 17 in this window, and a window that
            // short is why the first version never accepted a clean take at all.
            if (total >= 8)
            {
                for (int p = SITOR_LAG_MIN; p <= SITOR_LAG_MAX; p++)
                {
                    int32_t hist[SITOR_LAG_MAX + 1];
                    for (int q = 0; q < p; q++) hist[q] = 0;
                    for (int i = 0; i < SITOR_EDGE_WINDOW; i++)
                    {
                        if (s->edges[i]) hist[i % p]++;
                    }
                    int32_t mx = 0, at = 0;
                    for (int q = 0; q < p; q++)
                    {
                        // three bins wide: a transition is smeared by the filter's group delay, and
                        // a pile that straddles two bins is one pile
                        const int32_t v = hist[q] + hist[(q + 1) % p] + hist[(q + p - 1) % p];
                        if (v > mx) { mx = v; at = q; }
                    }
                    // An even spread over p bins puts three of them at total/p each, so a score of
                    // about 3 *is* "no structure" - the number to clear is well above it, not below.
                    const float score = (float)mx * (float)p / (float)total;
                    if (score > best_score) { best_score = score; best_p = p; best_n = mx; best_bin = at; }
                }
            }
            if (best_p && best_n >= 6 && best_score > 8.0f)
            {
                s->period = (float32_t)best_p;
                s->half = s->period * 0.5f;
                // where the boundary is *now*: the fold says a boundary falls on every sample i with
                // i % p == bin, and the phase counter is relative to this receiver's own sample count
                // best_bin is an index into this window, and the phase counter is relative to the
                // receiver's own sample count, so the window's own start has to be added back in -
                // without it the boundary lands somewhere arbitrary inside a bit and nothing ever
                // assembles, which is what made the first version reach SEARCH_ALIGN and then sit
                // there decoding nothing.
                long ph = ((long)s->sample_count - (long)(s->edge_start + best_bin)) % (long)best_p;
                if (ph < 0) ph += best_p;
                s->phase = (float32_t)ph;
                s->rate_score = best_score;
                s->state = SITOR_SEARCH_ALIGN;
            }
            s->edge_fill = 0;
            s->edge_start = s->sample_count;
        }
        return 0;
    }

    // ---- a bit boundary: take the decision the filter was too slow to take mid bit ---------
    if (s->phase < s->period)
    {
        return 0;
    }
    s->phase -= s->period;
    const int bit = bit_now;

    if (edge && s->state == SITOR_LOCKED)
    {
        // The transition happened `since_edge` samples after the boundary we just left.  Fold that
        // onto the nearest boundary and use it: the phase follows quickly, the period follows
        // slowly, and a take that is genuinely at the right rate simply stops moving either.
        float32_t err = (float32_t)s->since_edge - (float32_t)s->period * (float32_t)((s->since_edge + (int32_t)s->period / 2) / (int32_t)s->period);
        s->phase -= SITOR_TRACK_PHASE * err;
        s->period += SITOR_TRACK_RATE * err;
        if (s->period < 80.0f) s->period = 80.0f;
        if (s->period > 170.0f) s->period = 170.0f;
        s->half = s->period * 0.5f;
    }
    s->since_edge = 0;

    // ---- seven bits make a character ------------------------------------------------------
    s->hist = ((s->hist << 1) | (uint32_t)bit) & 0x3FFFu;
    s->hist_bits++;
    if (s->hist_bits < 7)
    {
        return 0;
    }
    s->hist_bits = 0;

    if (s->state == SITOR_SEARCH_ALIGN)
    {
        // every seven bits, all seven alignments are scored on the same history
        for (int o = 0; o < 7; o++)
        {
            const unsigned w7 = sitor_word_at(s, o);
            s->trial_words[o]++;
            if (sitor_is_word(w7)) s->trial_valid[o]++;
            // the same alternation test the locked path uses, per boundary, so whichever one wins the
            // boundary contest inherits the phasing evidence that boundary has been collecting
            const int fwd = (w7 == SITOR_SIA && s->trial_prev[o] == SITOR_SIB)
                         || (w7 == SITOR_SIB && s->trial_prev[o] == SITOR_SIA);
            const int rev = (w7 == SITOR_REV_SIA && s->trial_prev[o] == SITOR_REV_SIB)
                         || (w7 == SITOR_REV_SIB && s->trial_prev[o] == SITOR_REV_SIA);
            s->trial_run[o][0] = fwd ? s->trial_run[o][0] + 1 : 0;
            s->trial_run[o][1] = rev ? s->trial_run[o][1] + 1 : 0;
            if ((uint32_t)s->trial_run[o][0] > s->trial_best[o][0]) s->trial_best[o][0] = (uint32_t)s->trial_run[o][0];
            if ((uint32_t)s->trial_run[o][1] > s->trial_best[o][1]) s->trial_best[o][1] = (uint32_t)s->trial_run[o][1];
            s->trial_prev[o] = w7;
        }
        s->trial_bits++;
        if (s->trial_bits >= SITOR_ALIGN_BITS / 7)
        {
            // which of the seven boundaries the groups start on, decided by the share of words that
            // are words: seven bits taken across a boundary land on a constant ratio word about a
            // fifth of the time by accident, and seven bits taken inside one always do
            int best_off = -1;
            float32_t best_share = 0.0f;
            for (int o = 0; o < 7; o++)
            {
                if (s->trial_words[o] < 8) continue;
                const float32_t sh = (float32_t)s->trial_valid[o] / (float32_t)s->trial_words[o];
                if (sh > best_share) { best_share = sh; best_off = o; }
            }
            if (best_off >= 0 && best_share >= SITOR_LOCK_SHARE)
            {
                s->state = SITOR_LOCKED;
                s->align = best_off;
                s->valid_share = best_share;
                s->best_norm = (int32_t)s->trial_best[best_off][0];
                s->best_rev = (int32_t)s->trial_best[best_off][1];
                if (s->best_norm >= 2 || s->best_rev >= 2)
                {
                    s->reversed = s->best_rev > s->best_norm;
                    s->order_locked = 1;
                    // The block phase comes from here, and nowhere else.  SITOR-B sends a block of
                    // five characters and repeats it five slots later, so every character arrives
                    // twice and a receiver that prints both halves prints the message twice - which
                    // is what this did before, and it looks like text rather than like a bug, which
                    // is the dangerous part.  The only place the phase is knowable is the end of the
                    // phasing sequence, so the slot count starts there, and the second half of each
                    // block is printed with the first half's copy behind it.
                    s->slot = 0;
                }
                memset(s->trial_prev, 0xFF, sizeof(s->trial_prev));
            }
            else
            {
                // nothing that looks like characters: the rate was wrong, or the signal went away
                memset(s->trial_valid, 0, sizeof(s->trial_valid));
                memset(s->trial_words, 0, sizeof(s->trial_words));
                memset(s->trial_run, 0, sizeof(s->trial_run));
                memset(s->trial_best, 0, sizeof(s->trial_best));
                memset(s->trial_prev, 0xFF, sizeof(s->trial_prev));
                s->trial_bits = 0;
                s->state = SITOR_SEARCH_RATE;
                s->edge_fill = 0;
                s->edge_start = s->sample_count;
                s->valid_share = 0.0f;
            }
        }
        return 0;
    }

    const unsigned w = sitor_word_at(s, s->align);

    // the ring, and the character five slots back that is this one's repeat
    s->head = (s->head + 1) % SITOR_RING;
    s->ring[s->head] = w;
    if (s->have < SITOR_RING) s->have++;
    const int valid = sitor_is_word(w);

    s->win_words++;
    if (valid) s->win_valid++;
    if (s->win_words >= SITOR_WIN_CHARS)
    {
        s->valid_share = (float32_t)s->win_valid / (float32_t)s->win_words;
        s->win_words = 0;
        s->win_valid = 0;
        if (s->valid_share < SITOR_DROP_SHARE)
        {
            s->state = SITOR_SEARCH_RATE;
            s->edge_fill = 0;
            s->have = 0;
            s->order_locked = 0;
            s->run_norm = s->run_rev = s->best_norm = s->best_rev = 0;
            return 0;
        }
    }

    if (s->have < 11 && !s->order_locked)
    {
        return 0;
    }
    const int second_half = ((s->slot % 10) >= 5);
    s->slot++;
    if (s->order_locked && !second_half)
    {
        // the direct copy of this block: held, not printed, because its repeat arrives five slots
        // from now and the pair of them is what makes the character reliable
        return 0;
    }
    const unsigned prev = s->ring[(s->head - 5 + SITOR_RING) % SITOR_RING];

    // The bit order, from the phasing pair and not from the ratio: a reversed 4-of-7 word is still
    // 4-of-7, so the ratio that found the boundary is blind to it.  What is not blind is an
    // alternation of SIA and SIB, whose reversals are 0x78 and 0x66 - so the run is counted in both
    // readings and the longer one wins, which is the same detection the reference decoder makes.
    if (!s->order_locked)
    {
        // The bit order is decided from the phasing preamble, which compares each character with the
        // one *before it in time* - not with its diversity partner five slots back, which is what the
        // first attempt here did and why it reported every clean take as reversed: a block of five
        // repeated means the partner is the same character, so the alternation it was scoring was
        // never the SIA-SIB alternation at all.  A take keyed the other way round presents 0x78 and
        // 0x66 alternating instead of 0x0F and 0x33, and nothing else about the text distinguishes
        // the two, because a reversed constant-ratio code is another constant-ratio code.
        const int alt = (w == SITOR_SIA && s->prev_code == SITOR_SIB)
                     || (w == SITOR_SIB && s->prev_code == SITOR_SIA);
        const int alt_rev = (w == SITOR_REV_SIA && s->prev_code == SITOR_REV_SIB)
                         || (w == SITOR_REV_SIB && s->prev_code == SITOR_REV_SIA);
        s->run_norm = alt ? s->run_norm + 1 : 0;
        s->run_rev = alt_rev ? s->run_rev + 1 : 0;
        if (s->run_norm > s->best_norm) s->best_norm = s->run_norm;
        if (s->run_rev > s->best_rev) s->best_rev = s->run_rev;
        s->chars_seen++;
        if (s->best_norm >= 3 || s->best_rev >= 3)
        {
            s->reversed = s->best_rev > s->best_norm;
            s->phasing_run = s->reversed ? s->best_rev : s->best_norm;
            s->order_locked = 1;
        }
        else if (s->chars_seen >= SITOR_ORDER_LIMIT)
        {
            // no preamble in sight: keep the convention the mode puts on the air, and let the table
            // say so, rather than waiting forever for evidence that a station may never send
            s->reversed = 0;
            s->order_locked = 1;
        }
        if (w != SITOR_LTRS && w != SITOR_FIGS) s->prev_code = w;
        if (!s->order_locked) return 0;   // held back: the phasing precedes the text
    }

    // time diversity: a copy that is not a word has already been detected as broken, so take the
    // copy that is still a word rather than guessing at bits inside a damaged character.  Only when
    // neither is a word is the pair worth correcting against, and that is where a character can be
    // lost - which is counted, never invented.
    const int pv = sitor_is_word(prev);
    unsigned best = 0;
    int ok = 0;
    if (valid || pv)
    {
        best = valid ? w : prev;
        ok = 1;
        if (w != prev) s->repaired++;
    }
    else
    {
        int count = 0;
        const unsigned* codes = sitor_codes(&count);
        int best_dist = 99;
        for (int c = 0; c < count; c++)
        {
            const int dist = sitor_popcount(codes[c] ^ w) + sitor_popcount(codes[c] ^ prev);
            if (dist < best_dist) { best_dist = dist; best = codes[c]; }
        }
        ok = best_dist <= 3;
        if (!ok) s->lost++;
    }
    if (!ok)
    {
        return 0;
    }
    if (s->reversed) best = sitor_reverse(best);

    if (best == SITOR_LTRS) { s->letters = 1; return 0; }
    if (best == SITOR_FIGS) { s->letters = 0; return 0; }
    if (best == SITOR_SIA || best == SITOR_SIB || best == SITOR_RPT || best == SITOR_BLK) return 0;

    const char ch = s->letters ? SitorLetters[best] : SitorFigures[best];
    if (!ch) return 0;
    s->out_char = ch;
    s->chars_out++;
    return 1;
}

float32_t SitorRx_Confidence(const SitorRx* s)
{
    return s->valid_share;
}
