# Host bench for the UHSDR digital modes. Builds nothing into the radio.
#
#   make test    self checks (used by CI)
#   make rtty    RTTY shift/SNR character error rate matrix (used by CI, fails above the per case
#                limit), firmware modem looped back on itself
#   make rtty-long  the same matrix with eight copies of the message: 0.26 % is one character
#   make wefax   synthetic HF weather fax broadcast through the reference demodulator
#   make rtty-shift  does the shift setting still matter: one signal, every setting
#   make rtty-tune  how many Hz of tuning error the demodulator survives, and what that is in ppm
#   make rtty-ddk2  the one on air case: a DWD transmission recorded off the NA5B WebSDR, graded
#                against the text the station sends. Uses the committed fixture in fixtures/
#   make fixture-align  the two characters that take gets wrong, by name, with the audio under them
#   make rtty-trace  which bit of which character the demodulator read wrong, from its own decisions
#   make rtty-multipath  a second path: character error rate against delay spread
#   make tx-check  the modulator's own keying through the receiver's gate
#
# drivers/audio/rtty.c and drivers/audio/audio_agc.c are compiled from the real sources: the
# modem and the AGC the radio puts in front of it. The shim/ directory replaces only the headers
# they reach outside themselves, so they compile against their own rtty.h and a minimal
# audio_driver.h.

AUDIO    = ../../drivers/audio
LIBDIR   = $(AUDIO)
SHIM     = shim

BENCH_OBJS = digi_bench.o modem_host.o wefax.o navtex.o hellschreiber.o mfsk16.o olivia.o \
             softdds_host.o
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

# NAVTEX / SITOR-B: 100 Bd, 170 Hz shift, CCIR 476 constant ratio alphabet, DX/RX time diversity
navtex: digi_bench
	@./digi_bench navtex

# Hellschreiber: on/off keyed raster at 122.5 Bd, scored as a picture rather than as characters
hell: digi_bench
	@./digi_bench hell

# MFSK16: 16 tones at 15.625 Bd with the rate 1/2 code and the interleaver in the loop
mfsk16: digi_bench
	@./digi_bench mfsk16

# Olivia: 32 tones at 31.25 Bd, two tones a symbol, interleaved over 8 characters
olivia: digi_bench
	@./digi_bench olivia

rtty-shift: digi_bench
	@./digi_bench rtty-shift

rtty-tune: digi_bench
	@./digi_bench rtty-tune

# the on air case. fixtures/ddk2-4583.wav is a 13.7 s slice of a 4583 kHz DDK2 transmission recorded
# through the WebSDR client's own recorder (see README.md): real propagation, not a synthetic channel.
# 3 % is just above the 2.7 % this copy measured when it was pinned, so a modem change that alters
# the decode of real audio fails here even when every synthetic row still reads 0.0 %.
DDK2_TEXT := CQ CQ CQ DE DDK2 DDH7 DDK9 FREQUENCIES   4583 KHZ   7646 KHZ   10100.8 KHZ

rtty-ddk2: digi_bench
	@./digi_bench rtty-wav fixtures/ddk2-4583.wav 450 "$(DDK2_TEXT)" 50 3

# what that 2.7 % is made of: the two characters the take gets wrong, named, with the mark and space
# level of each one's own window against the take's median. It fails if they stop being the ones the
# record names, or if either window loses the keying that makes a fade the wrong explanation.
fixture-align: digi_bench
	@./digi_bench align-check fixtures/ddk2-4583.wav 450 "$(DDK2_TEXT)" 50

# the decoder's own bit decisions, which is the only way to say which data bit of a wrong character
# was read wrong: a letter is five bits and the report carries only the letter. Diagnostic, so it
# prints rather than asserting
rtty-trace: digi_bench
	@./digi_bench rtty-trace fixtures/ddk2-4583.wav 450 "$(DDK2_TEXT)" 50

# a second path, the channel shape nothing else here produces: the same bit at two places in the
# audio. Fails if a delay inside a tenth of a bit stops decoding or a quarter bit stops costing text
rtty-multipath: digi_bench
	@./digi_bench rtty-multipath

# the transmitter's own output through the gate the receiver's audio has to clear, plus the one
# check only a keyer can fail: every interval between tone changes on the half bit lattice
# (1 start, 5 data, 1.5 stop), with no short runs and no lost stop bits
tx-check: digi_bench
	@./digi_bench tx-check

# the gate a candidate take has to clear before it is pinned: the pair, the bit clock and the
# continuity, measured from the audio. Run it on any new recording first; the fixture below is the
# reference for what an accepted take reads like.
capture-check: digi_bench
	@./digi_bench capture-check fixtures/ddk2-4583.wav 450 50

# the manifest has to keep describing the file it keeps: the stored rate, channels, frame count and
# byte size are read back out of each WAV header and compared with fixtures/MANIFEST.md. A re-encode,
# a different trim or a decimation that forgot the manifest fails here rather than leaving the
# provenance describing a file that no longer has that shape.
fixture-check:
	@python3 fixture_check.py

clean:
	rm -f digi_bench *.o *.wav *.pgm
# fixtures/ is untouched on purpose: the on air recording is tracked, not a build product
