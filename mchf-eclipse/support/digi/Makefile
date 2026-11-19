# Host bench for the UHSDR digital modes. Builds nothing into the radio.
#
#   make test    self checks (used by CI)
#   make rtty    RTTY shift/SNR character error rate matrix (used by CI, fails above the per case
#                limit), firmware modem looped back on itself
#   make wefax   synthetic HF weather fax broadcast through the reference demodulator
#
# drivers/audio/rtty.c is compiled from the real source. The shim/ directory replaces only the
# headers that modem reaches outside itself, so it compiles against its own rtty.h.

AUDIO    = ../../drivers/audio
LIBDIR   = $(AUDIO)
SHIM     = shim

SRCS_BENCH = digi_bench.c modem_host.c wefax.c softdds_host.c
SRCS_FW    = $(LIBDIR)/rtty.c

CC       ?= gcc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
DEFINES   = -DUSE_RTTY_PROCESSOR -DHOST_BENCH
INCLUDES  = -I. -I$(SHIM) -I$(LIBDIR) -I$(LIBDIR)/cw -I$(LIBDIR)/softdds -I../../hardware

digi_bench: $(SRCS_BENCH) $(SRCS_FW) bench.h
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -o $@ $(SRCS_BENCH) $(SRCS_FW) -lm

test: digi_bench
	@./digi_bench test

rtty: digi_bench
	@./digi_bench rtty

wefax: digi_bench
	@./digi_bench wefax

clean:
	rm -f digi_bench *.wav *.pgm
