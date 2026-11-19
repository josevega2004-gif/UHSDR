# Host bench for the UHSDR digital modes. Builds nothing into the radio.
#
#   make test    self checks (used by CI)
#   make rtty    RTTY shift/SNR character error rate matrix (used by CI, fails above the per case
#                limit), firmware modem looped back on itself
#   make rtty-long  the same matrix with eight copies of the message: 0.26 % is one character
#   make wefax   synthetic HF weather fax broadcast through the reference demodulator
#   make rtty-shift  does the shift setting still matter: one signal, every setting
#   make rtty-tune  how many Hz of tuning error the demodulator survives, and what that is in ppm
#
# drivers/audio/rtty.c and drivers/audio/audio_agc.c are compiled from the real sources: the
# modem and the AGC the radio puts in front of it. The shim/ directory replaces only the headers
# they reach outside themselves, so they compile against their own rtty.h and a minimal
# audio_driver.h.

AUDIO    = ../../drivers/audio
LIBDIR   = $(AUDIO)
SHIM     = shim

BENCH_OBJS = digi_bench.o modem_host.o wefax.o softdds_host.o
FW_OBJS    = rtty.o audio_agc.o uhsdr_math.o

CC       ?= gcc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
DEFINES   = -DUSE_RTTY_PROCESSOR -DHOST_BENCH
INCLUDES  = -I. -I$(SHIM) -I$(LIBDIR) -I$(LIBDIR)/cw -I$(LIBDIR)/softdds -I../../hardware -I../../misc

# The firmware's own build passes -Wno-sign-compare (see the target Makefile): the shipping sources
# compare signed block sizes with unsigned sample indices in a few places, and the bench compiles
# them as they are rather than editing production code to suit the host. It applies to those
# sources only - the bench's own code is held to the full warning set, where a signed/unsigned
# comparison is a bug worth stopping for.
FWCFLAGS  = -Wno-sign-compare

digi_bench: $(BENCH_OBJS) $(FW_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# the bench's own sources
%.o: %.c bench.h
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

# firmware sources, as they are shipped
rtty.o: $(LIBDIR)/rtty.c bench.h
	$(CC) $(CFLAGS) $(FWCFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

audio_agc.o: $(LIBDIR)/audio_agc.c
	$(CC) $(CFLAGS) $(FWCFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

uhsdr_math.o: ../../misc/uhsdr_math.c
	$(CC) $(CFLAGS) $(FWCFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

test: digi_bench
	@./digi_bench test

rtty: digi_bench
	@./digi_bench rtty

rtty-long: digi_bench
	@./digi_bench rtty-long

wefax: digi_bench
	@./digi_bench wefax

rtty-shift: digi_bench
	@./digi_bench rtty-shift

rtty-tune: digi_bench
	@./digi_bench rtty-tune

clean:
	rm -f digi_bench *.o *.wav *.pgm
