#!/usr/bin/env bash
# The DDK9 attempt on 10100.8 kHz, in the only order that makes its number mean anything.
#
# The 19 November attempt is the reason for this script: a take that the gate should have refused was
# read for its character error rate, and an empty 10 MHz band was reported as a noise floor. So a run
# is graded here only after two controls have passed, and any failure exits nonzero with an ABORT line
# and no CER at all:
#
#   1. the dead-band control - a capture on a channel that carries nothing on this receiver, inside
#      one of its bands: the collector refuses a dial no band covers, because such a take would be
#      the band the connection opened in rather than the control it is labelled.  Either the oracle
#      has to refuse that take *as an empty take* - a refusal for bad framing says the stream was
#      broken, not that the pipeline can tell nothing from a station - or it has to decode and the
#      gate has to reject it.  Anything else here stops the run, including a gate or an oracle that
#      never ran: a pipeline that accepts a dead channel cannot tell an empty band from a station,
#      and nothing captured later in the run is evidence about the modem, the receiver, or 10 MHz.
#   2. DDK2 still copies - the pinned 4583 kHz fixture has to come back at its pinned 2.7 % CER, to
#      within --ddk2-tol.  DDK9 is the same transmitter family, sent through the same receiver, so a
#      modem or a gate that has stopped copying the one take that is pinned condemns a 10100.8 kHz
#      take before a capture slot is spent on it.
#   3. only then is 10100.8 kHz captured, gated, and - only when the gate accepts it - graded, and
#      only if it arrived as different bytes from the control: two dials that come back as the same
#      stream are one dial, which is how the 20 November parts came to look like five stations.
#
# A capture that cannot get a receiver is given up on after --deadline seconds rather than retried
# into the night: the collector reconnects forever until a part is complete, and a control step that
# hangs is a run that never says it was not graded.
#
# Nothing here invents a number: what is printed is the gate's verdict, and a CER only from a take the
# gate accepted and only when --expect gives the text to grade against.
#
# usage:
#   capture/ddk9_capture.sh --dead-khz <kHz> [options]
#
#   --dead-khz <kHz>      a channel that carries nothing on this receiver, e.g. one between DWD
#                         broadcasts and inside one of its bands; required, because "dead" depends
#                         on the site and the hour and a control that is guessed is not a control
#   --host <host:port>    receiver (default websdr.ewi.utwente.nl:8901, the collector's default)
#   --audio <lo-hi>       passband in Hz about the dial (default 600-1700, the DWD pair)
#   --shift <Hz>          RTTY shift for the gate and the modulator's table (default 450)
#   --baud <45|50>        (default 50)
#   --part-seconds <s>    length of each capture (default 30)
#   --deadline <s>        give up on one capture after this many seconds (default 4 x part-seconds
#                         + 30, which is the collector's own reconnect backoff and then some)
#   --out <dir>           where the takes, WAVs and logs go (default ./ddk9-run, where you run it)
#   --client <file>       the WebSDR client script (default $WEBSDR_CLIENT or capture/websdr-sound.js)
#   --ddk2-tol <points>   how far DDK2's CER may sit from 2.7 % (default 0.3)
#   --expect <text>       the text the DDK9 take should read, to print a CER; without it the gate's
#                         verdict is the whole result
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DIGI=$(cd "$HERE/.." && pwd)

HOST=websdr.ewi.utwente.nl:8901
AUDIO=600-1700
SHIFT=450
BAUD=50
SECS=30
DEADLINE=
OUT=ddk9-run
DEAD_KHZ=
CLIENT=${WEBSDR_CLIENT:-$HERE/websdr-sound.js}
TOL=0.3
EXPECT=
PINNED=2.7          # the CER fixtures/ddk2-4583.wav measured when it was pinned (see the Makefile)

die()  { printf 'ABORT   : %s\n' "$*" >&2; exit 1; }
say()  { printf '%s\n' "$*"; }

# Options that end up on another command line are checked before they get there: an empty --shift or
# a --part-seconds of "soon" would otherwise be the bench's or the collector's error, late, out of
# context, and about the wrong thing.
need_number() { # value flag-name
    printf '%s\n' "$1" | grep -Eq '^-?[0-9]+(\.[0-9]+)?$' || die "--$2 wants a number, got '$1'"
}

need_positive() { # value flag-name
    need_number "$1" "$2"
    awk -v v="$1" 'BEGIN { exit (v > 0 ? 0 : 1) }' || die "--$2 wants more than 0, got '$1'"
}

while [ $# -gt 0 ]; do
    case $1 in
        --dead-khz)      DEAD_KHZ=${2:-} ;;
        --host)          HOST=${2:-} ;;
        --audio)         AUDIO=${2:-} ;;
        --shift)         SHIFT=${2:-} ;;
        --baud)          BAUD=${2:-} ;;
        --part-seconds)  SECS=${2:-} ;;
        --deadline)      DEADLINE=${2:-} ;;
        --out)           OUT=${2:-} ;;
        --client)        CLIENT=${2:-} ;;
        --ddk2-tol)      TOL=${2:-} ;;
        --expect)        EXPECT=${2:-} ;;
        -h|--help)       awk '/^# usage:/{on=1} /^set -uo/{on=0} on{sub(/^# ?/,""); print}' "$0"; exit 0 ;;
        *)               die "unknown option $1 (try --help)" ;;
    esac
    shift 2 || shift
done

[ -n "$DEAD_KHZ" ] || die "--dead-khz is required: a channel that carries nothing on this receiver, so the gate has something it has to refuse"
need_positive "$DEAD_KHZ" dead-khz
need_positive "$SECS" part-seconds
need_positive "$SHIFT" shift
need_number "$TOL" ddk2-tol
case $BAUD in 45|50) ;; *) die "--baud is 45 or 50, not '$BAUD'" ;; esac
[ -n "$HOST" ]  || die "--host wants host:port"
[ -n "$AUDIO" ] || die "--audio wants two edges in Hz, e.g. 600-1700"
DEADLINE=${DEADLINE:-$(awk -v s="$SECS" 'BEGIN { printf "%.0f", 4 * s + 30 }')}
need_positive "$DEADLINE" deadline
[ -f "$CLIENT" ]   || die "no WebSDR client at $CLIENT: it is not committed here (see the licence note in README.md), so save the script your receiver's page loads and pass --client"
command -v node >/dev/null 2>&1    || die "node is needed for capture/decode_oracle.js"
command -v python3 >/dev/null 2>&1 || die "python3 is needed for capture/websdr_capture.py"

make -s -C "$DIGI" digi_bench || die "the bench does not build, so nothing could be graded"
mkdir -p "$OUT" || die "cannot make $OUT"

# one capture, one part; prints the .stream it wrote.  Called as $(capture_one ...), so a die here
# only ends the subshell: every caller has to check the assignment's status or the run carries on
# with an empty path, which is how a failed control capture used to read as a control that passed.
capture_one() { # dir kHz
    local dir=$1 khz=$2
    mkdir -p "$dir" || die "cannot make $dir"
    python3 "$HERE/websdr_capture.py" --host "$HOST" --freq-khz "$khz" --audio "$AUDIO" \
        --mode usb --part-seconds "$SECS" --deadline "$DEADLINE" --parts 1 --out "$dir" \
        >"$dir/capture.log" 2>&1 \
        || die "the collector gave up on ${khz} kHz after ${DEADLINE} s; see $dir/capture.log"
    newest_stream "$dir" || die "the collector wrote no part for ${khz} kHz; see $dir/capture.log"
}

# The part this run wrote, from the line the collector printed for it.  Listing the directory would
# pick up an older fragment in a --out dir a previous attempt left behind, and grading that against a
# control is not grading the control.
newest_stream() { # dir
    local name
    name=$(sed -n 's/^\(part-[0-9][0-9]*\.stream\)[[:space:]].*/\1/p' "$1/capture.log" 2>/dev/null |
              tail -1)
    [ -n "$name" ] || return 1
    printf '%s\n' "$1/$name"
}

# What an oracle run did, from its exit and its own first complaint.  A crash exits 1 as well, and the
# oracle refuses a missing client with an 'oracle: FAIL' line of its own, so neither the status nor
# the prefix is enough on its own: only a complaint that is not about the client is a rule having
# read the take and turned it down.  Confusing the two is what lets a broken pipeline pass step 1.
oracle_verdict() { # rc log
    case $1 in
        0) printf 'decoded\n'; return ;;
        1) ;;
        *) printf 'unrunnable\n'; return ;;
    esac
    grep -q 'oracle: FAIL' "$2"          || { printf 'unrunnable\n'; return; }
    grep -q 'cannot find the WebSDR client' "$2" && { printf 'unrunnable\n'; return; }
    printf 'refused\n'
}

# 0 the take decoded, 1 a rule refused it, 2 the oracle never got that far (no client, a crash,
# an unreadable file): reported separately because step 1 has to be refused *by a rule*
decode_one() { # stream wav
    local rc verdict
    node "$HERE/decode_oracle.js" "$1" "$2" --client "$CLIENT" >"$2.log" 2>&1
    rc=$?
    verdict=$(oracle_verdict "$rc" "$2.log")
    case $verdict in
        decoded) return 0 ;;
        refused) return 1 ;;
        *)       return 2 ;;
    esac
}

# ACCEPT, REJECT, or unrunnable.  The verdict is the gate's own line rather than its status, because
# the bench exits 1 both for a REJECT it measured and for an ACCEPT whose limits it broke, and the
# second of those is a take that passed.  A status of 2 is the other half: capture-check returns it
# when it never opened the WAV, which is not a verdict about radio and must not be read as a REJECT
# that step 1 could bank as a pass.
gate_one() { # wav
    local out rc verdict
    [ -x "$DIGI/digi_bench" ] || { printf 'unrunnable\n'; return; }
    out=$("$DIGI/digi_bench" capture-check "$1" "$SHIFT" "$BAUD" 2>&1)
    rc=$?
    printf '%s\n' "$out" >"$1.gate"
    [ "$rc" -eq 2 ] && { printf 'unrunnable\n'; return; }
    verdict=$(printf '%s\n' "$out" | sed -n 's/^ACCEPT.*/ACCEPT/p; s/^REJECT.*/REJECT/p' | head -1)
    case $verdict in
        ACCEPT) printf 'ACCEPT\n' ;;
        REJECT) printf 'REJECT\n' ;;
        *)      printf 'unrunnable\n' ;;
    esac
}

# the CER rtty-wav printed, or nothing when the take did not decode at all
cer_of() { # wav text
    "$DIGI/digi_bench" rtty-wav "$1" "$SHIFT" "$2" "$BAUD" 100 2>&1 |
        sed -n 's/^CER *: *\([0-9][0-9.]*\)%.*/\1/p' | tail -1
}

# ---- 1. the dead-band control: the pipeline has to refuse it for being empty ------------------
say "== 1. dead-band control on ${DEAD_KHZ} kHz"
control=$(capture_one "$OUT/control" "$DEAD_KHZ") || exit 1
[ -n "$control" ] || die "the control capture reported success and handed back no take"
cspan=${control%-*.stream}.wav
decode_one "$control" "$cspan"
case $? in
    2) die "the oracle could not run on the control take: $(head -1 "$cspan.log")" ;;
    1) reason=$(grep -m1 'oracle: FAIL' "$cspan.log" | sed 's/^oracle: FAIL //')
       case $reason in
         "no audio in the take"*)
             say "   the oracle refused it as an empty take: $reason" ;;
         *)
             die "the control take was refused for something other than being empty ($reason), so it does not show that this pipeline can tell an empty band from a station; pick another --dead-khz" ;;
       esac ;;
    0) verdict=$(gate_one "$cspan")
       case $verdict in
           unrunnable) die "the gate printed neither ACCEPT nor REJECT on the control take, so it did not measure it; see $cspan.gate" ;;
           ACCEPT) die "the gate ACCEPTED a take from ${DEAD_KHZ} kHz, which is supposed to carry nothing: it cannot tell an empty band from a station, so no later verdict in this run means anything; see $cspan.gate" ;;
           REJECT) say "   the gate refused it: $(grep -m1 '^REJECT' "$cspan.gate")" ;;
       esac ;;
esac

# ---- 2. DDK2 still copies at its pinned rate -----------------------------------------------
say "== 2. DDK2 on the pinned 4583 kHz take"
ddk2_text=$(sed -n 's/^DDK2_TEXT := //p' "$DIGI/Makefile")
[ -n "$ddk2_text" ] || die "no DDK2_TEXT in $DIGI/Makefile, so the pinned take cannot be graded"
cer=$(cer_of "$DIGI/fixtures/ddk2-4583.wav" "$ddk2_text")
[ -n "$cer" ] || die "the pinned DDK2 take did not decode at all, so the modem cannot be trusted with a 10100.8 kHz take"
if ! awk -v c="$cer" -v p="$PINNED" -v t="$TOL" 'BEGIN { d = c - p; if (d < 0) d = -d; exit (d <= t ? 0 : 1) }'; then
    die "DDK2 comes back at ${cer} %, not the pinned ${PINNED} % within ${TOL}: the modem or the gate has moved, so a DDK9 take is not worth a capture slot"
fi
say "   DDK2 ${cer} % against the pinned ${PINNED} %"

# ---- 3. only now the attempt ---------------------------------------------------------------
say "== 3. DDK9 on 10100.8 kHz"
ddk9=$(capture_one "$OUT/ddk9" 10100.8) || exit 1
[ -n "$ddk9" ] || die "the DDK9 capture reported success and handed back no take"

# The two dials have to have produced two streams.  On 20 November five parts came off five
# frequencies and were one channel: the receiver served the same buffer, each take was gated as if it
# had been tuned, and nothing recorded at the time could show the dial had not moved - parts.json
# only kept bytes, messages and a band index.  The request each part was asked with now goes in
# beside them, and here the two takes are compared outright before either is graded.
if [ -s "$control" ] && cmp -s "$control" "$ddk9"; then
    die "the ${DEAD_KHZ} kHz control and the 10100.8 kHz take arrived as the same $(stat -c %s "$ddk9") bytes: at most one dial reached the receiver, so there is no pair to score and no verdict in this run means anything"
fi
wav=${ddk9%-*.stream}.wav
decode_one "$ddk9" "$wav"
case $? in
    2) die "the oracle could not run on the take: $(head -1 "$wav.log")" ;;
    1) die "the oracle refused the take ($(grep -m1 'oracle: FAIL' "$wav.log" | sed 's/^oracle: FAIL //')), so there is no WAV behind it and a CER from it would be a number with no signal" ;;
esac
verdict=$(gate_one "$wav")
case $verdict in
    unrunnable) die "the gate printed neither ACCEPT nor REJECT on the take, so there is nothing to grade and no verdict to name; see $wav.gate" ;;
    REJECT) die "the gate refused the take, so there is nothing to grade; its refusal is the result, and the CER it would have produced is exactly what this script exists to stop printing; see $wav.gate" ;;
esac
say "   the gate accepted it: $(grep -m1 '^ACCEPT' "$wav.gate")"

if [ -n "$EXPECT" ]; then
    cer=$(cer_of "$wav" "$EXPECT")
    [ -n "$cer" ] || die "the take did not decode at all against the given text, so it has no CER"
    say "   CER ${cer} % against the text given in --expect"
    say "   takes and logs in $OUT"
else
    say "   no --expect text, so no CER: the gate's verdict is this run's result"
    say "   takes and logs in $OUT"
fi
