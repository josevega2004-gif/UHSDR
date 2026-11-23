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

/* One sample in, one character out, no whole-transmission buffer.
 *
 * This file was rewritten around the two properties the reference modem in support/digi/navtex.c
 * exploits and the first version of this file did not: the 4-of-7 alphabet as a synchronisation
 * objective, and the SIA/SIB preamble as an exact bit pattern. The first version measured the bit
 * period from the autocorrelation of the tone transitions, which is a clever-sounding way to find a
 * number the mode gives away far more strongly, and it is the reason that version would not lock at
 * 20 dB of noise where the reference decodes without a single character error.
 *
 * Costs for the radio, at the 12 kHz the narrow filters already produce:
 *   - front end: two sliding Goertzel filters, about twelve multiplies a sample;
 *   - acquisition: a grid of 1080 candidate clocks (nine integer periods, every phase in each),
 *     each fed once per its own period, so about sixty integer operations a sample for 1.7 s;
 *   - locked: the grid stops and only the winning candidate and its two neighbouring phases are
 *     fed, so about twenty operations a sample forever;
 *   - RAM: the grid's registers and aged scores, roughly 20 kB - see the size the bench prints.
 */

#include <math.h>
#include <string.h>

#include "sitor_rx.h"

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

/* The four 14 bit windows the preamble can produce, and how they were derived, because getting this
 * wrong is silent: a bit of the wrong parity decodes as a perfectly legal character.
 *
 * A word's bits arrive least significant bit first (CCIR 476 is written most significant bit first
 * and SITOR sends it the other way round, as ITA2 does), so after two words the receiver's window -
 * newest decision in bit 0 - holds the earlier word reversed in its high half and the later word
 * reversed in its low half. Written as received, the four possibilities are the two alternations of
 * SIA/SIB in the convention's bit order, and the same two with each code bit-reversed, which is what
 * a transmitter that keys most significant bit first produces. Because the two codes of a pair are
 * different, each of these four identifies both the group boundary and the bit order, and the
 * alphabet test cannot: SIA and SIB are both 4-of-7, so during the preamble all seven boundaries
 * score 100 % and none of them means anything. */
static uint16_t phase_pat[4];
static uint8_t rev7_tab[128];
static unsigned code_list[40];     // the constant-ratio words the two tables actually define
static int code_count;
static int tables_ready;

static unsigned rev7(unsigned v)
{
    unsigned r = 0;
    for (int b = 0; b < 7; b++) r |= (unsigned)((v >> b) & 1u) << (6 - b);
    return r;
}

static uint8_t pop4_tab[16];

static void build_pop4(void)
{
    for (int v = 0; v < 16; v++)
    {
        int n = 0;
        for (int b = 0; b < 4; b++) n += (v >> b) & 1;
        pop4_tab[v] = (uint8_t)n;
    }
}

// Cortex-M4 has no population count instruction, so this is four table lookups and three adds
// rather than a loop: it runs four times per candidate per bit, which is the hottest thing in the
// file after the tone filter.
static int popcount(unsigned v)
{
    int n = pop4_tab[v & 15u];
    v >>= 4;
    n += pop4_tab[v & 15u];
    v >>= 4;
    n += pop4_tab[v & 15u];
    v >>= 4;
    n += pop4_tab[v & 15u];
    return n;
}

static void build_tables(void)
{
    if (tables_ready) return;
    build_pop4();
    for (int v = 0; v < 128; v++) rev7_tab[v] = (uint8_t)rev7((unsigned)v);
    code_count = 0;
    for (int v = 0; v < 128; v++)
    {
        if (popcount((unsigned)v) != 4) continue;
        if (SitorLetters[v] || SitorFigures[v]) code_list[code_count++] = (unsigned)v;
    }
    phase_pat[0] = (uint16_t)((rev7_tab[SITOR_SIA] << 7) | rev7_tab[SITOR_SIB]);
    phase_pat[1] = (uint16_t)((rev7_tab[SITOR_SIB] << 7) | rev7_tab[SITOR_SIA]);
    phase_pat[2] = (uint16_t)((SITOR_SIA << 7) | SITOR_SIB);
    phase_pat[3] = (uint16_t)((SITOR_SIB << 7) | SITOR_SIA);
    tables_ready = 1;
}

// Which period a candidate index belongs to, and its phase within that period.  Derived rather than
// stored, so the grid costs its registers and scores and nothing else.
static int grid_period(const SitorRx* s, int c)
{
    for (int i = 0; i < s->nper; i++)
        if (c < s->base[i] + (s->per_lo + i)) return i;
    return s->nper - 1;
}

void SitorRx_Init(SitorRx* s, float32_t sample_rate_hz, float32_t low_hz, float32_t high_hz)
{
    build_tables();
    memset(s, 0, sizeof(*s));
    s->letters = 1;    // the alphabet starts in letters; zeroed state would read as a figures shift

    s->win = (int32_t)lrintf(sample_rate_hz / 100.0f);        // one bit at 100 Bd
    if (s->win < 2) s->win = 2;
    if (s->win > SITOR_MAX_WIN) s->win = SITOR_MAX_WIN;
    s->rot_c = cosf(2.0f * (float)M_PI * low_hz / sample_rate_hz);
    s->rot_s = sinf(2.0f * (float)M_PI * low_hz / sample_rate_hz);
    s->rot2_c = cosf(2.0f * (float)M_PI * high_hz / sample_rate_hz);
    s->rot2_s = sinf(2.0f * (float)M_PI * high_hz / sample_rate_hz);
    s->ph_c = 1.0f; s->ph_s = 0.0f;
    s->ph2_c = 1.0f; s->ph2_s = 0.0f;
    // at n = 0 the lagged instant is -W, so its rotation is e^{+j w W}
    const float32_t wc = cosf(2.0f * (float)M_PI * low_hz * (float)s->win / sample_rate_hz);
    const float32_t ws = sinf(2.0f * (float)M_PI * low_hz * (float)s->win / sample_rate_hz);
    const float32_t hc = cosf(2.0f * (float)M_PI * high_hz * (float)s->win / sample_rate_hz);
    const float32_t hs = sinf(2.0f * (float)M_PI * high_hz * (float)s->win / sample_rate_hz);
    s->lag_c = wc; s->lag_s = ws;
    s->lag2_c = hc; s->lag2_s = hs;

    /* The grid: every integer period within +-3.3 % of the nominal, and every phase within each.
     * At 12 kHz that is nine periods and 1080 candidates.  The span shrinks rather than the arrays
     * growing, because a wide nominal costs phases quadratically: at 48 kHz the same percentage
     * would need fifteen thousand candidates, which is not a thing a radio can be given, so at that
     * rate the receiver covers +-0.1 % and relies on running on the decimated audio path instead.
     * SITOR says so in the header rather than leaving it to be discovered by a silent failure. */
    int span = SITOR_GRID_SPAN;
    int nper, total;
    do
    {
        nper = 2 * span + 1;
        total = 0;
        for (int i = 0; i < nper; i++)
        {
            int p = s->win - span + i;
            if (p < 2) p = 2;
            total += p;
        }
        if (total <= SITOR_MAX_GRID) break;
        span--;
    } while (span > 0);
    if (span < 1) { span = 1; nper = 3; total = 3 * s->win; if (total > SITOR_MAX_GRID) total = SITOR_MAX_GRID; }
    s->per_lo = s->win - span;
    if (s->per_lo < 2) s->per_lo = 2;
    s->per_hi = s->win + span;
    s->nper = s->per_hi - s->per_lo + 1;
    s->base[0] = 0;
    for (int i = 1; i < s->nper; i++)
        s->base[i] = s->base[i - 1] + (s->per_lo + i - 1);
    s->total = s->base[s->nper - 1] + s->per_hi;
    if (s->total > SITOR_MAX_GRID) s->total = SITOR_MAX_GRID;
    s->next_decide = (uint32_t)(s->win * 40);
}

// Feed one decision bit to one candidate: its 14 bit window, its aged alphabet share per group
// boundary, and its phasing run.
static void grid_update(SitorRx* s, int c, int bit, float32_t mag)
{
    s->win14[c] = (uint16_t)(((s->win14[c] << 1) | (uint16_t)bit) & 0x3FFFu);
    if (s->seen[c] < 60000u) s->seen[c]++;
    const uint16_t w = s->win14[c];
    const uint32_t seen = s->seen[c];

    if (seen >= 7u)
    {
        const int o = (int)(seen % 7u);
        const int valid = (popcount((unsigned)(w & 0x7Fu)) == 4);
        const unsigned sc = s->score[c][o];
        // aged by a factor of 16 with a step of 8, so a candidate that is always valid settles at
        // 8 * 16 = 128 and the number reads directly as a percentage of 128
        s->score[c][o] = (uint8_t)(sc - (sc >> 4) + (valid ? 8u : 0u));
        const unsigned mg = s->margin[c];
        s->margin[c] = (uint8_t)(mg - (mg >> 4) + (mag > 240.0f ? 240u : (unsigned)mag));
    }

    if (seen >= 14u)
    {
        // two of the fourteen bits may be wrong and the pattern still counts: at 10 dB the bit error
        // rate is a few percent, so an exact match would be found once in three words and a run of
        // six would never happen, while a false match at distance two has to chain at a stride of
        // fourteen bits as well before it means anything
        int best = 99, which = -1;
        for (int k = 0; k < 4; k++)
        {
            const int d = popcount((unsigned)(w ^ phase_pat[k]));
            if (d < best) { best = d; which = k; }
        }
        if (best <= 2)
        {
            // The pair recurs every SEVEN bits, not fourteen: word boundaries are seven bits apart,
            // so the window over words 0-1 is SIA,SIB, over words 1-2 is SIB,SIA, over 2-3 is
            // SIA,SIB again.  Both patterns are in the table, so an alternating preamble matches at
            // every boundary and demanding a stride of fourteen threw away every second match and
            // broke the chain - which is why the phasing lock, the strongest thing this receiver
            // knows, never got past a run of one.
            if (s->ph_next[c] && seen == s->ph_next[c]) s->ph_run[c]++;
            else s->ph_run[c] = 1;
            s->ph_next[c] = seen + 7u;
            if (s->ph_run[c] > s->ph_best[c])
            {
                s->ph_best[c] = s->ph_run[c];
                s->ph_off[c] = (uint8_t)(seen % 7u);   // the boundary the pair ends on
                s->ph_ord[c] = (uint8_t)(which >= 2);  // the two high patterns are the other bit order
            }
        }
        // A bit that is not a match must NOT clear the run.  Pair matches are seven bits apart, so
        // between them there are six bits that are not matches: clearing on those held every run at
        // one forever, and that is why the phasing lock - the strongest thing this receiver knows -
        // never won a single case.  The chain is held by the stride: if the next match is more than
        // two pair-lengths overdue, the preamble has ended, and that moment is recorded, because the
        // end of the phasing is where the blocks of ten start.
        else if (s->ph_next[c] && seen > s->ph_next[c] + 14u && s->ph_run[c])
        {
            if (s->ph_run[c] >= SITOR_PHASING_RUN && !s->ph_have_end[c])
            {
                s->ph_end_word[c] = (uint16_t)((s->ph_next[c] - 7u) / 7u);
                s->ph_have_end[c] = 1;
            }
            s->ph_run[c] = 0;
        }
    }
}

// The winner, from the grid.  A phasing lock is taken in preference to an alphabet lock wherever it
// exists, because it knows three things the alphabet only guesses at: the boundary while the
// preamble is still running, and the bit order, which the alphabet can never know at all.
static void grid_decide(SitorRx* s)
{
    int best_c = -1, best_o = 0, best_v = 0, second = 0;

    for (int c = 0; c < s->total; c++)
    {
        if (s->seen[c] < (uint16_t)SITOR_BITS_TO_DECIDE) continue;
        // A candidate whose period is not the nominal one reads the right bits for a while and then
        // walks off them: an integer period 4 samples from the true one drifts a whole bit every 30
        // bits, so its early words are as valid as the truth and its aged score is still high when
        // the decision is made.  Costing it eight points per sample of period error is not a guess
        // about the mode, it is arithmetic about the grid, and it is what stops a drifter winning
        // the lock from a candidate that is right on the nominal clock and simply slower to saturate.
        const int pr = grid_period(s, c);
        const int bias = 8 * (s->per_lo + pr > s->win ? s->per_lo + pr - s->win : s->win - (s->per_lo + pr));
        for (int o = 0; o < 7; o++)
        {
            int v = s->score[c][o] - bias;
            if (v < 0) v = 0;
            if (v > best_v) { second = best_v; best_v = v; best_c = c; best_o = o; }
            else if (v > second) second = v;
        }
    }
    s->best_norm = 0; s->best_rev = 0;
    for (int c = 0; c < s->total; c++)
    {
        if (s->ph_ord[c]) { if (s->ph_best[c] > s->best_rev) s->best_rev = s->ph_best[c]; }
        else { if (s->ph_best[c] > s->best_norm) s->best_norm = s->ph_best[c]; }
    }

    // The clock and the boundary come from the alphabet, and the phasing is consulted afterwards on
    // the candidate the alphabet chose.  Letting the phasing pick the clock was tried and is wrong:
    // a 20-character preamble is a 14-bit pattern repeated 10 times, which is the one signal in this
    // mode where a candidate sampling in the middle of a bit can chain six near-matches at distance
    // two, so the strongest-looking evidence gave the wrong answer with total confidence and printed
    // garbage at 0.90.  As a confirmation step it does what it can actually do: tell the bit order
    // and the block phase of a clock that has already been established by evidence that discriminates.
    int chosen = -1, off = 0, rev = 0, known = 0;
    if (best_c >= 0 && best_v >= SITOR_ALPHA_LOCK && (second == 0 || best_v * SITOR_MARGIN_NUM > second * 2)
        && best_c == s->prev_best)                         // the same answer twice, not one good frame
    {
        chosen = best_c; off = best_o;
        if (s->ph_best[best_c] >= SITOR_PHASING_RUN && s->ph_off[best_c] == off)
        {
            rev = s->ph_ord[best_c];
            known = 1;
        }
    }
    s->prev_best = best_c;
    if (chosen < 0) return;

    const int i = grid_period(s, chosen);
    s->period = s->per_lo + i;
    s->phase = chosen - s->base[i];
    if (known)
    {
        s->reversed = rev;
        s->order_locked = 1;
        if (s->ph_have_end[chosen])
        {
            // the block of ten starts where the phasing stopped, expressed in the same word count the
            // pairing uses, so the very first repeated character after the lock is emitted and none
            // of the direct copies in front of it are cross-paired with a different character
            const int words_now = (int)(s->seen[chosen] / 7u);
            s->block_phase = (((words_now - (int)s->ph_end_word[chosen]) % 10) + 10) % 10;
            s->block_locked = 1;
            memset(s->agree, 0, sizeof(s->agree));
        }
    }
    else if (!s->order_locked) s->reversed = 0;   // an alphabet lock cannot know the order
    s->live[0] = chosen;
    s->live_n = 1;
    s->valid_share = (float32_t)s->score[chosen][off] / 128.0f;
    if (chosen != s->win_idx || off != s->align)
    {
        // a different candidate, or the same one at a different boundary: the characters assembled so
        // far belong to the old reading, so the pairing starts again rather than mixing the two
        s->char_slots = 0;
        s->block_locked = 0;
        memset(s->agree, 0, sizeof(s->agree));
        memset(s->ring_codes, 0, sizeof(s->ring_codes));
    }
    s->win_idx = chosen;
    s->align = off;
    if (s->state != SITOR_LOCKED)
    {
        s->state = SITOR_LOCKED;
        s->locks++;
        if (known) s->phasing_locks++;
        s->char_slots = 0;
        s->ring_head = 0;
        s->block_locked = 0;
        memset(s->agree, 0, sizeof(s->agree));
        memset(s->ring_codes, 0, sizeof(s->ring_codes));
    }
}

// Clear the grid so a broken lock cannot be re-won from the scores that were already there.
//
// ccur[] is deliberately NOT cleared.  It is the counter that says which phase of each period this
// sample is, so it is a function of the absolute sample index, not of the search: zeroing it leaves
// candidate base[i]+p meaning "the p-th sample after the reset" instead of "every sample congruent
// to p modulo the period", and from then on every candidate is fed at an arbitrary point inside the
// bit.  Its decisions then straddle boundaries, its words are mixtures of two characters, the
// alphabet still validates often enough to look confident, and the text is deterministic garbage -
// which is what this receiver produced, at 0.97 confidence, on a clean take.
static void grid_reset(SitorRx* s)
{
    memset(s->win14, 0, sizeof(s->win14));
    memset(s->seen, 0, sizeof(s->seen));
    memset(s->score, 0, sizeof(s->score));
    memset(s->margin, 0, sizeof(s->margin));
    memset(s->ph_run, 0, sizeof(s->ph_run));
    memset(s->ph_best, 0, sizeof(s->ph_best));
    memset(s->ph_ord, 0, sizeof(s->ph_ord));
    memset(s->ph_next, 0, sizeof(s->ph_next));
    memset(s->ph_have_end, 0, sizeof(s->ph_have_end));
    memset(s->ph_end_word, 0, sizeof(s->ph_end_word));
    s->state = SITOR_SEARCH_RATE;
    s->char_slots = 0;
    memset(s->ring_codes, 0, sizeof(s->ring_codes));
    s->next_decide = s->sample_count + (uint32_t)(s->win * 40);
}

static void emit_char(SitorRx* s, unsigned code, int* ready, char* ch)
{
    if (code == SITOR_LTRS) { s->letters = 1; return; }
    if (code == SITOR_FIGS) { s->letters = 0; return; }
    if (code == SITOR_SIA || code == SITOR_SIB || code == SITOR_RPT || code == SITOR_BLK) return;
    const char c = s->letters ? SitorLetters[code] : SitorFigures[code];
    if (!c) return;
    *ch = c;
    *ready = 1;
    s->chars_out++;
}

int SitorRx_Sample(SitorRx* s, float32_t x)
{
    int ready = 0;
    char ch = 0;
    s->out_char = 0;

    /* the discriminator: two sliding one-bit Goertzel filters, difference of the energies.
     *
     * X[n] = X[n-1] + x[n] e^{-j w n} - x[n-W] e^{-j w (n-W)}: the term leaving the window has to be
     * removed with the rotation it was added with, not with today's. Multiplying both by the current
     * phasor is the mistake that looks like a plausible sliding DFT and is not one - it leaves the
     * two tone energies nearly equal whatever is transmitted, so the discriminator has no sign to
     * speak with and no candidate can reach 4-of-7 more often than chance. Costs one more complex
     * multiply a sample per tone, and the ring of samples for the delayed value anyway. */
    const int32_t ri = s->wring;
    const float32_t old = s->ring[ri];
    s->ring[ri] = x;
    s->wring = (s->wring + 1 >= s->win) ? 0 : s->wring + 1;
    s->acc_i += x * s->ph_c - old * s->lag_c;
    s->acc_q += x * s->ph_s - old * s->lag_s;
    s->acc2_i += x * s->ph2_c - old * s->lag2_c;
    s->acc2_q += x * s->ph2_s - old * s->lag2_s;
    float32_t t = s->ph_c * s->rot_c + s->ph_s * s->rot_s;
    s->ph_s = s->ph_s * s->rot_c - s->ph_c * s->rot_s;
    s->ph_c = t;
    t = s->lag_c * s->rot_c + s->lag_s * s->rot_s;
    s->lag_s = s->lag_s * s->rot_c - s->lag_c * s->rot_s;
    s->lag_c = t;
    t = s->ph2_c * s->rot2_c + s->ph2_s * s->rot2_s;
    s->ph2_s = s->ph2_s * s->rot2_c - s->ph2_c * s->rot2_s;
    s->ph2_c = t;
    t = s->lag2_c * s->rot2_c + s->lag2_s * s->rot2_s;
    s->lag2_s = s->lag2_s * s->rot2_c - s->lag2_c * s->rot2_s;
    s->lag2_c = t;
    const float32_t e_lo = s->acc_i * s->acc_i + s->acc_q * s->acc_q;
    const float32_t e_hi = s->acc2_i * s->acc2_i + s->acc2_q * s->acc2_q;
    s->disc = e_hi - e_lo;
    const int bit = (s->disc > 0.0f);
    // scaled so a full scale tone in one of the two comes out near 60 and noise stays small: the
    // margin is only a tie-break, and an unnormalised one would saturate on every take
    float32_t mag = (s->disc < 0.0f ? -s->disc : s->disc) * (60.0f / ((float)s->win * (float)s->win));
    if (mag > 240.0f) mag = 240.0f;
    s->sample_count++;

    /* The whole grid is fed, always, including while locked.  The first version stopped feeding
     * everything but the winner and its two neighbouring phases, to save about 130 integer
     * operations a sample, and it cost more than it saved: the neighbour the lock migrated to has a
     * different bit count, so its residue for the group boundary is different, and the diversity
     * pairing that had been running against the old candidate's word stream went on pairing
     * characters that were one slot out of step - which shows up as letters swapping with their
     * neighbours on a take whose bit decisions are provably perfect.  Letting the grid run means the
     * winner can move only at a decision, and a decision that changes it resets the pairing. */
    int live_hit = 0;
    for (int i = 0; i < s->nper; i++)
    {
        const int p = s->per_lo + i;
        int c = s->base[i] + s->ccur[i];
        s->ccur[i] = (s->ccur[i] + 1 >= p) ? 0 : s->ccur[i] + 1;
        if (c >= s->total) continue;
        if (c == s->live[0]) live_hit = 1;
        grid_update(s, c, bit, mag);
    }

    if (s->state == SITOR_LOCKED)
    {
        const int c = s->live[0];
        s->valid_share = (float32_t)s->score[c][s->align] / 128.0f;

        // a character ends on this sample when the candidate's bit count lands on the boundary.
        // `live_hit` is not decoration: seen[] only moves once per bit period, so without the test
        // for a bit actually having arrived here the condition below stays true for every sample in
        // between and each character is printed period/7 times over - which is exactly what this
        // receiver did the first time it locked, and what made a correct decode look like garbage.
        if (live_hit && s->seen[c] >= 7u && (int)(s->seen[c] % 7u) == s->align)
        {
            // The window holds the word with its first received bit at bit 6, because the shift
            // puts each new bit at 0.  The alphabet is written with the first received bit at bit 0
            // - CCIR 476 is most significant bit first and SITOR sends it the other way round - so
            // the convention's reading of this word is the window reversed.  A take that keys
            // highest bit first has its intended code in the window as it stands, which is why the
            // bit order decided from the phasing changes which of the two is used.
            const unsigned raw = (unsigned)(s->win14[c] & 0x7Fu);
            const unsigned use_code = s->reversed ? raw : (unsigned)rev7_tab[raw];
            const int slot = (int)(s->char_slots % 10u);
            const int prev5 = (int)((s->char_slots + 5u) % 10u);
            const int have_hist = (s->char_slots >= 5u);
            s->char_slots++;
            unsigned a = 0, b = use_code;
            if (have_hist)
            {
                a = s->ring_codes[prev5];
                // Which residue class is the second half of a block?  Slot j pairs with j-5 and holds
                // the same character only for the five residues after the block start; for the five
                // before it, j-5 reaches into the previous block and is a different character
                // entirely.  A receiver that locked mid-message does not know where the block began,
                // so it asks the signal: the residue classes where the two copies agree are the
                // emitting ones.  Without this the pairing runs half on matches and half across a
                // block boundary, and the characters it gets from a boundary are its neighbours.
                if (a == b) { if (s->agree[slot] < 120u) s->agree[slot] = (uint8_t)(s->agree[slot] + 8u); }
                else if (s->agree[slot] > 4u) s->agree[slot] = (uint8_t)(s->agree[slot] - 4u);
                else s->agree[slot] = 0;
                if (!s->block_locked && s->char_slots >= 22u)
                {
                    // Five consecutive classes hot and the other five cold, which is what the mode's
                    // block of ten looks like from inside.  Stated as a shape rather than a count:
                    // the absolute number depends on how long the receiver has been watching, and a
                    // threshold in counts is what made this never fire.
                    int bq = 0, bs = -1, hot = 0;
                    for (int q = 0; q < 10; q++)
                    {
                        int sum = 0, hh = 0;
                        for (int i2 = 0; i2 < 5; i2++)
                        {
                            sum += s->agree[(q + i2) % 10];
                            if (s->agree[(q + i2) % 10] >= 4) hh++;
                        }
                        if (sum > bs) { bs = sum; bq = q; hot = hh; }
                    }
                    int cold = 0;
                    for (int i2 = 5; i2 < 10; i2++) cold += s->agree[(bq + i2) % 10];
                    if (hot >= 4 && bs >= 24 && cold * 3 <= bs)
                    {
                        s->block_phase = (bq + 5) % 10;
                        s->block_locked = 1;
                    }
                }
            }
            // Say when the take's clock is outside what an integer grid can follow, rather than
            // printing across it.  A period the receiver has to round by more than one sample drifts
            // a whole bit within about 140 bits, and what it prints while that happens is characters
            // assembled from two different words: confident garbage, with a 4-of-7 share high enough
            // to look like a copy.  The mode's own answer to a rate error is to resample the audio,
            // which this does not do, so it reports the limit instead of hiding it.
            const int off_nominal = s->period > s->win ? s->period - s->win : s->win - s->period;
            s->rate_out_of_range = (off_nominal > SITOR_RATE_TOLERANCE);
            if (s->block_locked && !s->rate_out_of_range
                && ((slot - s->block_phase + 10) % 10) >= 5 && have_hist)
            {
                const int pa = popcount(a), pb = popcount(b);
                unsigned best;
                int ok;
                if (pa == 4 || pb == 4)
                {
                    // time diversity, not guessing at bits: a copy that is not a word at all is a
                    // copy the receiver knows was destroyed, and the other copy is taken as it is.
                    // Scoring the pair together would reject a character that is sitting intact in
                    // one of the two, which is the burst case.
                    best = (pa == 4) ? a : b;
                    ok = 1;
                }
                else
                {
                    int bd = 99; best = 0;
                    for (int i = 0; i < code_count; i++)
                    {
                        const int d = popcount(code_list[i] ^ a) + popcount(code_list[i] ^ b);
                        if (d < bd) { bd = d; best = code_list[i]; }
                    }
                    ok = (bd <= 3);
                }
                if (!ok) s->lost++;
                else
                {
                    if (a != b) s->repaired++;
                    emit_char(s, best, &ready, &ch);
                }
            }
            s->ring_codes[slot] = use_code;
            s->ring_head = slot;
        }

        // hold the lock only while the winner still agrees with itself, and let it go rather than
        // print confident garbage.  The three live candidates are consulted together, because a
        // single candidate losing its score is usually the clock having drifted half a sample and
        // is fixed by moving, while all three failing is the take having ended or never been this
        // mode at all.  The scores are reset with it, so a re-acquisition is a real search.
        if ((s->sample_count & 63u) == 0u && s->seen[c] >= (uint16_t)SITOR_BITS_TO_DECIDE)
        {
            int best_live = 0;
            for (int j = 0; j < s->live_n; j++)
                if (s->score[s->live[j]][s->align] > best_live) best_live = s->score[s->live[j]][s->align];
            if (best_live < SITOR_ALPHA_DROP)
            {
                grid_reset(s);
                s->order_locked = 0;      // a new take may well be in the other order
                return ready ? (s->out_char = ch, 1) : 0;
            }
        }
        if (!s->order_locked && s->char_slots > SITOR_ORDER_LIMIT)
            s->order_locked = 1;      // assumed, and recorded as assumed: see the header
        if (ready) { s->out_char = ch; return 1; }
        return 0;
    }

    if (s->sample_count >= s->next_decide)
    {
        // Decide often and let the evidence gate the lock, rather than deciding rarely: the phasing
        // preamble is 140 bits long and an alphabet candidate needs 320 before it can be trusted, so
        // a receiver that only looks every 320 bits throws away the strongest clue the mode offers
        // and starts printing in the middle of the message.  Costs one pass over the grid per 0.4 s.
        s->next_decide = s->sample_count + (uint32_t)(s->win * 40);
        grid_decide(s);
    }
    return 0;
}

float32_t SitorRx_Confidence(const SitorRx* s)
{
    if (s->state == SITOR_LOCKED) return s->valid_share;
    int best = 0;
    for (int c = 0; c < s->total; c++)
        for (int o = 0; o < 7; o++)
            if (s->score[c][o] > best) best = s->score[c][o];
    return (float32_t)best / 128.0f;
}
