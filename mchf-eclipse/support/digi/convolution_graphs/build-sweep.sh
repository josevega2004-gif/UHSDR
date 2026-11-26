#!/bin/sh
# usage: build-sweep.sh <tree>/mchf-eclipse <out.csv> [--chain|--chain-levels] [extra -D flags]
# Same build as build.sh, for path_sweep.c: the shipped convolution module in as text, the host
# transform, and FilterPathInfo[] cut out of audio_filter.c.  --chain also runs the paths through
# the pre-AGC IIR, the firmware WDSP AGC and the post-AGC anti-alias IIR; --chain-levels sweeps
# that two-tone pass over station level.
set -e
M=$1; OUT=$2; shift 2
MODE=
case "$1" in --chain|--chain-levels) MODE=$1; shift ;; esac
D=$M/support/digi; O=$(mktemp -d)
F="-O2 -std=gnu11 -w -DUSE_RTTY_PROCESSOR -DHOST_BENCH -DUSE_CONVOLUTION -D__IO=volatile -I$D/shim -I$M/drivers/audio -I$M/drivers/audio/filters -I$M/hardware -I$M/misc"
sed '/^}; \/\/ end FilterPath/q' $M/drivers/audio/audio_filter.c > $O/filter_paths.gen.c
cc $F "$@" -c -o $O/filter_paths.o $O/filter_paths.gen.c
for f in $M/drivers/audio/filters/*.c; do cc $F "$@" -c -o $O/$(basename $f .c).o $f; done
cc $F -c -o $O/convolution_fft.o $D/convolution_fft.c
cc $F "$@" -c -o $O/audio_agc.o $M/drivers/audio/audio_agc.c
cc $F "$@" -c -o $O/uhsdr_math.o $M/misc/uhsdr_math.c
cc $F "$@" -c -o $O/path_sweep.o $D/convolution_graphs/path_sweep.c
cc -o $O/path_sweep $O/*.o -lm
$O/path_sweep $MODE "$@" > $OUT
rm -rf $O
