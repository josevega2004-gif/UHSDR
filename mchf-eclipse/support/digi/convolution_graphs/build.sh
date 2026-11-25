#!/bin/sh
# usage: build.sh <tree>/mchf-eclipse <out.csv> [extra -D flags]
set -e
M=$1; OUT=$2; shift 2
D=$M/support/digi; O=$(mktemp -d)
F="-O2 -std=gnu11 -w -DUSE_RTTY_PROCESSOR -DHOST_BENCH -DUSE_CONVOLUTION -D__IO=volatile -I$D/shim -I$M/drivers/audio -I$M/drivers/audio/filters -I$M/hardware -I$M/misc"
sed '/^}; \/\/ end FilterPath/q' $M/drivers/audio/audio_filter.c > $O/filter_paths.gen.c
cc $F "$@" -c -o $O/filter_paths.o $O/filter_paths.gen.c
for f in $M/drivers/audio/filters/*.c; do cc $F "$@" -c -o $O/$(basename $f .c).o $f; done
cc $F -c -o $O/convolution_fft.o $D/convolution_fft.c
cc $F "$@" -c -o $O/compare.o compare.c
cc -o $O/compare $O/*.o -lm
$O/compare > $OUT
rm -rf $O
