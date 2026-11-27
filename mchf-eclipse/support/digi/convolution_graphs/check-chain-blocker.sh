#!/bin/sh
# Run --chain-blocker and check the claims the convolution README makes: 62 paths at 31 station
# levels, one fixed mirror frequency per path, the 12 kHz paths losing at most 0.05 dB of wanted top
# to the blocker, and the wide paths losing the measured several dB while the convolution side loses
# none.  The check reads the table the binary prints, so a lost row or a moved column fails here too.
#
# usage: check-chain-blocker.sh <tree>/mchf-eclipse
set -e
M=$1
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
sh "$(dirname "$0")/build-sweep.sh" "$M" "$OUT" --chain-blocker

awk -F, '
BEGIN {
    # the mirror frequency measured for each path, negative: the blocker input is the complex
    # opposite-sideband tone, and its audio image lands at the absolute value
    split("4:-400 5:-458 6:-517 7:-576 8:-580 9:-636 10:-694 11:-754 12:-814 13:-873 " \
          "14:-350 15:-458 16:-576 17:-694 18:-754 19:-185 20:-230 21:-230 22:-235 23:-405 " \
          "24:-576 25:-754 26:-873 27:-1052 28:-230 29:-286 30:-286 31:-400 32:-517 " \
          "33:-642 34:-814 35:-286 36:-295 37:-342 38:-342 39:-345 40:-342 41:-400 " \
          "42:-400 43:-400 44:-400 45:-458 46:-405 47:-458 48:-425 49:-455 50:-445 " \
          "51:-465 52:-3965 53:-4145 54:-4227 55:-545 56:-595 57:-645 58:-695 59:-745 " \
          "60:-795 61:-845 62:-895 63:-945 64:-1425 65:-1425", a, " ")
    for (i = 1; i <= 62; i++) {
        split(a[i], kv, ":")
        want_hz[kv[1]] = kv[2] + 0
    }
    # the README table: 62 SSB/CW paths, ids 4-65, on levels from +20 to -40 dB in 2 dB steps
    first = 4; last = 65; n_want = last - first + 1
    top = 20; bottom = -40; step = 2; n_levels = (top - bottom) / step + 1
    maxconv = 0
}
FNR == 1 {
    if ($0 != "id,name,level_db,level,blocker_hz,blocker_level,stock_blocker_db,conv_blocker_db,stock_agc_top_abs,conv_agc_top_abs,stock_deficit,stock_blocker_loss,conv_blocker_loss") {
        printf "FAIL  unexpected header, the column order the check reads has moved: %s\n", $0
        badh = 1
    }
    next
}
{
    id = $1 + 0; L = $3 + 0
    # Paths added after the documented 62 (currently the VARA 2750 path, id 87) are outside
    # the mirror-frequency table this check pins, so they are not part of its claim.
    if (!(id in want_hz)) {
        if (id < first || id > last) next
        printf "FAIL  unknown path id %d\n", id
        fail++
        next
    }
    if (L < bottom || L > top || (top - L) % step != 0) {
        printf "FAIL  row %d %s has unexpected level %d dB\n", id, $2, L
        fail++
        next
    }
    if ($0 ~ /[Nn][Aa][Nn]|[Ii][Nn][Ff]/) {
        printf "FAIL  row %d %s at %d dB has a non-finite value: %s\n", id, $2, L, $0
        fail++
        next
    }

    if (!(id in name)) { name[id] = $2; npaths++ }
    rows[L]++
    seen[L, id]++
    if (!(id in hz0)) {
        hz0[id] = $5 + 0
        if (hz0[id] != want_hz[id]) {
            printf "FAIL  row %d %s: mirror %.0f Hz, expected %.0f Hz\n", id, $2, hz0[id], want_hz[id]
            fail++
        }
    } else if (($5 + 0) != hz0[id]) {
        printf "FAIL  row %d %s: mirror moved from %.0f to %.0f Hz at %d dB\n", \
               id, $2, hz0[id], $5, L
        fail++
    }
    if ($5 >= 0) {
        printf "FAIL  row %d %s: mirror frequency %.0f Hz is not negative\n", id, $2, $5
        fail++
    }
    if (($6 + 0) != 1.0) {
        printf "FAIL  row %d %s: blocker level %.6g, expected 1.0\n", id, $2, $6
        fail++
    }
    if (($7 + 0) <= ($8 + 0)) {
        printf "FAIL  row %d %s: stock blocker %.2f dB is not above conv %.2f dB\n", id, $2, $7, $8
        fail++
    }
    if (($8 + 0) > -100.0) {
        printf "FAIL  row %d %s: conv blocker only %.2f dB down, expected at least 100\n", id, $2, $8
        fail++
    }
    if (id >= 4 && id <= 45 && ($7 + 0) > -60.0) {
        printf "FAIL  row %d %s: 12 kHz stock mirror only %.2f dB down\n", id, $2, $7
        fail++
    }
    if (id >= 48 && id <= 60 && ($7 + 0) < -35.0) {
        printf "FAIL  row %d %s: wide stock mirror %.2f dB down, expected a leak above -35\n", \
               id, $2, $7
        fail++
    }

    d = $11 - ($10 - $9); if (d < 0) d = -d
    if (d > 0.011) {
        printf "FAIL  row %d %s at %d dB: deficit %.2f does not equal conv minus stock\n", \
               id, $2, L, $11
        fail++
    }
    if (($12 + 0) > maxloss[id]) maxloss[id] = $12 + 0
    if (($12 + 0) > maxwide) { maxwide = $12 + 0; maxwide_id = id }
    if (($13 + 0) > maxconv) maxconv = $13 + 0
    if ($13 > 0.011 || $13 < -0.011) {
        printf "FAIL  row %d %s at %d dB: conv blocker loss %.2f dB is not zero\n", \
               id, $2, L, $13
        fail++
    }
    if (id >= 4 && id <= 45 && ($12 + 0) > max12) max12 = $12 + 0

    if (L == 0 && ($4 < 0.049999 || $4 > 0.050001)) {
        printf "FAIL  row %d %s: 0 dB level %.6g, expected 0.05\n", id, $2, $4
        fail++
    }
    if (L == 20 && ($4 < 0.499999 || $4 > 0.500001)) {
        printf "FAIL  row %d %s: +20 dB level %.6g, expected 0.5\n", id, $2, $4
        fail++
    }
    if (L == -40 && ($4 < 0.000499999 || $4 > 0.000500001)) {
        printf "FAIL  row %d %s: -40 dB level %.6g, expected 0.0005\n", id, $2, $4
        fail++
    }
}
END {
    if (badh) fail++
    if (npaths != n_want) {
        printf "FAIL  %d paths, expected %d (ids 4-65)\n", npaths, n_want
        fail++
    }
    for (L = top; L >= bottom; L -= step) {
        if (rows[L] != n_want) {
            printf "FAIL  level %d dB has %d rows, expected %d\n", L, rows[L], n_want
            fail++
        }
        for (id = first; id <= last; id++) {
            if ((id in name) && seen[L, id] != 1) {
                printf "FAIL  level %d dB: row %d appears %d times\n", L, id, seen[L, id]
                fail++
            }
        }
    }
    for (id = first; id <= last; id++) {
        if (!(id in name)) {
            printf "FAIL  row %d is missing from the sweep\n", id
            fail++
            continue
        }
        if (!(id in hz0)) {
            printf "FAIL  row %d %s has no mirror frequency\n", id, name[id]
            fail++
        }
    }
    for (id = 4; id <= 45; id++) {
        if (maxloss[id] > 0.05) {
            printf "FAIL  row %d %s: 12 kHz blocker loss %.2f dB, ceiling 0.05\n", \
                   id, name[id], maxloss[id]
            fail++
        }
    }
    for (id = 48; id <= 60; id++) {
        if (maxloss[id] < 2.0) {
            printf "FAIL  row %d %s: wide blocker loss only %.2f dB, expected at least 2\n", \
                   id, name[id], maxloss[id]
            fail++
        }
    }
    if (maxloss[55] < 9.0 || maxloss[55] > 9.2) {
        printf "FAIL  row 55 %s: wide blocker loss peaks at %.2f dB, expected 9.0-9.2\n", \
               name[55], maxloss[55]
        fail++
    }
    if (max12 > 0.05) {
        printf "FAIL  12 kHz blocker loss ceiling exceeded: %.2f dB\n", max12
        fail++
    }
    if (maxconv > 0.011) {
        printf "FAIL  convolution blocker loss %.2f dB, expected zero\n", maxconv
        fail++
    }

    if (fail) {
        printf "%d failure(s)\n", fail
        exit 1
    }
    printf "ok    blocker sweep: %d paths x %d levels, fixed per-path mirror, " \
           "12 kHz loss <= %.2f dB, wide loss up to %.2f dB (row %d), conv loss <= %.2f dB\n", \
           n_want, n_levels, max12, maxwide, maxwide_id, maxconv
}' "$OUT"
