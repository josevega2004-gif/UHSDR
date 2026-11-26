#!/bin/sh
# Run --chain and --chain-levels and check the claims the convolution README makes: the level table's
# 0 dB row has to reproduce --chain's after-AGC tops, no top or deficit may move more than 0.01 dB
# from that row across the 20 dB above / 40 dB below range, the reference tone has to rise 4-6 dB
# from 0 to +20 dB so a dead AGC cannot pass, and the rows with a positive deficit are exactly the
# eight n/c rows at every level.  The check reads the tables the binary prints, so a formatting change
# that loses a row or a column fails here too, and it pins the 62 documented paths (ids 4-65) so a
# path that disappears from every level cannot shrink the sweep and pass.
#
# usage: check-chain-levels.sh <tree>/mchf-eclipse
set -e
M=$1
OUT=$(mktemp)
CHAIN=$(mktemp)
trap 'rm -f "$OUT" "$CHAIN"' EXIT
sh "$(dirname "$0")/build-sweep.sh" "$M" "$CHAIN" --chain
sh "$(dirname "$0")/build-sweep.sh" "$M" "$OUT" --chain-levels

awk -F, -v limit=0.01 -v abs_limit=0.006 -v ref_min=4.0 -v ref_max=6.0 -v chainfile="$CHAIN" '
BEGIN {
    n_nc = split("46 47 53 54 55 56 58 59", nc, " ")
    # the README table: 62 SSB/CW paths, ids 4-65, on levels from +20 to -40 dB in 2 dB steps
    first = 4; last = 65; n_want = last - first + 1
    top = 20; bottom = -40; step = 2; n_levels = (top - bottom) / step + 1
    gs = gc = gd = -1
    abs_worst = 0
    refrise_min = 1e9; refrise_max = -1e9
}
FNR == 1 {
    if (FILENAME == chainfile) {
        if ($18 != "stock_agc_top" || $19 != "conv_agc_top") {
            printf "FAIL  unexpected --chain header, columns 18/19 are not the after-AGC tops: %s\n", $0
            badh = 1
        }
    } else {
        if (levpass == 0 && $0 != "id,name,level_db,level,stock_agc_top,conv_agc_top,stock_deficit,stock_agc_ref") {
            printf "FAIL  unexpected header, the column order the check reads has moved: %s\n", $0
            badh = 1
        }
        levpass++
    }
    next
}
FILENAME == chainfile {
    chain_s[$1] = $18 + 0
    chain_c[$1] = $19 + 0
    next
}
levpass == 1 {
    id = $1
    if (!(id in name)) { name[id] = $2; npaths++ }
    if ($3 + 0 == 0) {
        s0[id] = $5 + 0; c0[id] = $6 + 0; d0[id] = $7 + 0
        ref0[id] = $8 + 0
    }
    if ($3 + 0 == 20) ref20[id] = $8 + 0
    next
}
{
    id = $1; L = $3 + 0
    if (!(L in rows)) nlev++
    rows[L]++
    seen[L, id]++
    p = ($7 + 0 > 0)
    pos[L, id] = p
    if (p) poscount[L]++
    if (!(id in s0)) next

    ds = $5 - s0[id]; if (ds < 0) ds = -ds
    dc = $6 - c0[id]; if (dc < 0) dc = -dc
    dd = $7 - d0[id]; if (dd < 0) dd = -dd
    if (ds > maxs[id]) { maxs[id] = ds; maxs_at[id] = L }
    if (dc > maxc[id]) { maxc[id] = dc; maxc_at[id] = L }
    if (dd > maxd[id]) { maxd[id] = dd; maxd_at[id] = L }
    if (ds > gs) { gs = ds; gs_id = id; gs_at = L }
    if (dc > gc) { gc = dc; gc_id = id; gc_at = L }
    if (dd > gd) { gd = dd; gd_id = id; gd_at = L }
}
END {
    if (badh) { printf "1 failure(s)\n"; exit 1 }
    fail = 0
    if (npaths != n_want) {
        printf "FAIL  the sweep has %d paths, the README documents %d (ids %d-%d)\n", \
               npaths, n_want, first, last
        fail++
    }
    if (nlev != n_levels) {
        printf "FAIL  the sweep has %d levels, the README documents %d (%d to %d dB)\n", \
               nlev, n_levels, top, bottom
        fail++
    }

    # Walking the pinned range covers the missing-path case.  If the count is n_want and every id in
    # the range is present, an id outside it cannot exist, so that needs no loop of its own.
    for (id = first; id <= last; id++) {
        if (!(id in name)) {
            printf "FAIL  row %d is missing from the sweep, the README documents %d paths\n", id, n_want
            fail++
            continue
        }
        if (!(id in s0)) {
            printf "FAIL  row %d %s has no 0 dB row\n", id, name[id]
            fail++
            continue
        }
        if (!(id in chain_s)) {
            printf "FAIL  row %d %s is missing from --chain\n", id, name[id]
            fail++
        } else {
            ds = s0[id] - chain_s[id]; if (ds < 0) ds = -ds
            dc = c0[id] - chain_c[id]; if (dc < 0) dc = -dc
            if (ds > abs_worst) abs_worst = ds
            if (dc > abs_worst) abs_worst = dc
            if (ds > abs_limit) {
                printf "FAIL  row %d %s: 0 dB stock top %.3f dB disagrees with the --chain value %.2f dB\n", \
                       id, name[id], s0[id], chain_s[id]
                fail++
            }
            if (dc > abs_limit) {
                printf "FAIL  row %d %s: 0 dB conv top %.3f dB disagrees with the --chain value %.2f dB\n", \
                       id, name[id], c0[id], chain_c[id]
                fail++
            }
        }
        if (!(id in ref20)) {
            printf "FAIL  row %d %s has no +20 dB reference\n", id, name[id]
            fail++
        } else {
            rise = ref20[id] - ref0[id]
            if (rise < refrise_min) refrise_min = rise
            if (rise > refrise_max) refrise_max = rise
            if (rise < ref_min || rise > ref_max) {
                printf "FAIL  row %d %s: stock_agc_ref rises %.2f dB from 0 to +20 dB, expected %.1f-%.1f\n", \
                       id, name[id], rise, ref_min, ref_max
                fail++
            }
        }
        if (maxs[id] > limit) {
            printf "FAIL  row %d %s: stock top moved %.3f dB at %d dB (limit %.2f)\n", \
                   id, name[id], maxs[id], maxs_at[id], limit
            fail++
        }
        if (maxc[id] > limit) {
            printf "FAIL  row %d %s: conv top moved %.3f dB at %d dB (limit %.2f)\n", \
                   id, name[id], maxc[id], maxc_at[id], limit
            fail++
        }
        if (maxd[id] > limit) {
            printf "FAIL  row %d %s: deficit moved %.3f dB at %d dB (limit %.2f)\n", \
                   id, name[id], maxd[id], maxd_at[id], limit
            fail++
        }
    }

    for (L = top; L >= bottom; L -= step) {
        if (!(L in rows)) {
            printf "FAIL  level %d dB is missing\n", L
            fail++
            continue
        }
        if (rows[L] != n_want) {
            printf "FAIL  level %d dB has %d rows, expected %d\n", L, rows[L], n_want
            fail++
        }
        # rows catches a duplicate; this catches a path missing at one level even when a duplicate
        # row of another path keeps the count at n_want
        for (id = first; id <= last; id++) {
            if ((id in name) && !seen[L, id]) {
                printf "FAIL  level %d dB: row %d is missing\n", L, id
                fail++
            }
        }
        if (poscount[L] != n_nc) {
            printf "FAIL  level %d dB: %d rows have a positive deficit, the n/c list has %d\n", \
                   L, poscount[L], n_nc
            fail++
        }
        # the count plus all eight present means no other row can be positive
        for (i = 1; i <= n_nc; i++) {
            if (!pos[L, nc[i]]) {
                printf "FAIL  level %d dB: row %d is on the n/c list but its deficit is not positive\n", \
                       L, nc[i]
                fail++
            }
        }
    }

    if (fail) {
        printf "%d failure(s)\n", fail
        exit 1
    }
    printf "ok    chain level sweep: %d paths x %d levels, 0 dB row matches --chain within %.3f dB, " \
           "stock_agc_ref rises %.2f-%.2f dB, worst stock top %.3f dB (row %d at %d dB), " \
           "conv top %.3f dB (row %d at %d dB), deficit %.3f dB (row %d at %d dB); " \
           "positive-deficit set = the eight n/c rows at every level\n", \
           n_want, n_levels, abs_worst, refrise_min, refrise_max, \
           gs, gs_id, gs_at, gc, gc_id, gc_at, gd, gd_id, gd_at
}' "$CHAIN" "$OUT" "$OUT"
