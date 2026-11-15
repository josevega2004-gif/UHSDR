# Host bench for the vendored ft8_lib decoder (drivers/digi/ft8_lib).
# Builds outside of the firmware: decodes WAV fixtures with the exact sources
# that are compiled into the radio, so measurements here carry over.

LIBDIR   = ../../drivers/digi/ft8_lib
SRCS_LIB = $(wildcard $(LIBDIR)/ft8/*.c) $(LIBDIR)/fft/kiss_fft.c $(LIBDIR)/fft/kiss_fftr.c
SRCS_BEN = demo/decode_ft8.c common/wave.c common/monitor.c common/audio.c

CC       = gcc
CFLAGS   = -O2 -Wall -I$(LIBDIR) -I. -DHAVE_STPCPY

ft8d_host: $(SRCS_LIB) $(SRCS_BEN)
	$(CC) $(CFLAGS) -o $@ $(SRCS_BEN) $(SRCS_LIB) -lm

# decode every bundled fixture
test: ft8d_host
	@for w in test/wav/*.wav; do echo "== $$w"; ./ft8d_host $$w | sed -n '/^0000/p' | head -6; done

clean:
	rm -f ft8d_host gen_ft8_host

.PHONY: test clean
