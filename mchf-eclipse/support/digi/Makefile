# Host bench for the UHSDR digital modes. Builds nothing into the radio.
#
#   make test    self checks (used by CI)
#   make break-rx  negative control: the real build has to pass the RTTY table and the build with
#                the receiver told the other symbol speed has to fail it on the clean 170 row, or
#                the table is not measuring the receiver
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

# A build with one receiver assumption deliberately broken: the receiver is told the other symbol
# speed, while the take it is handed is the one the case asked for. The RTTY table has to fail
# against it, so this target succeeds only when the broken build fails. A table that cannot see a
# broken receiver is not a table, and the noise rows are exactly where a receiver that is not being
# tested looks better.
digi_bench-break-rx: digi_bench.break-rx.o $(filter-out digi_bench.o,$(BENCH_OBJS)) $(FW_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

digi_bench.break-rx.o: digi_bench.c bench.h
	$(CC) $(CFLAGS) $(DEFINES) -DBENCH_BREAK_RX $(INCLUDES) -c -o $@ $<

# A nonzero exit on its own proves nothing: the bench could have crashed, or failed on a row that
# has nothing to do with the receiver's speed setting. So the real build first has to pass the same
# table, the broken build has to fail, the failure has to name the clean 170 row and its character
# error rate, and the run has to reach the end of the table - all of which a crash or an unrelated
# row cannot do.
break-rx: digi_bench digi_bench-break-rx
	@./digi_bench rtty >/tmp/digi-break-rx-normal.log 2>&1 || { \
		echo "FAIL  the real receiver does not pass the RTTY table, so a broken build failing it would prove nothing"; \
		grep -m3 FAIL /tmp/digi-break-rx-normal.log || tail -5 /tmp/digi-break-rx-normal.log; \
		exit 1; \
	}
	@grep -q "all .* cases within their character error rate limits" /tmp/digi-break-rx-normal.log || { \
		echo "FAIL  the real receiver's run printed no all cases summary, so it did not measure the whole table"; \
		tail -5 /tmp/digi-break-rx-normal.log; \
		exit 1; \
	}
	@if ./digi_bench-break-rx rtty >/tmp/digi-break-rx.log 2>&1; then \
		echo "FAIL  the broken receiver passed the RTTY table, so the table is not measuring it"; \
		tail -3 /tmp/digi-break-rx.log; \
		exit 1; \
	fi
	@grep -qE "^FAIL +clean 170 +character error rate .*is above" /tmp/digi-break-rx.log || { \
		echo "FAIL  the broken receiver failed, but not on the clean 170 row: a nonzero exit from anywhere else is not this control"; \
		grep -m3 "^FAIL" /tmp/digi-break-rx.log || tail -5 /tmp/digi-break-rx.log; \
		exit 1; \
	}
	@grep -q "note: the shift setting is graded" /tmp/digi-break-rx.log || { \
		echo "FAIL  the broken run stopped before the end of the RTTY table, so it did not measure the receiver"; \
		tail -5 /tmp/digi-break-rx.log; \
		exit 1; \
	}
	@echo "ok    the real build passes the table, and the broken receiver fails it on clean 170:"
	@grep -m1 "^FAIL  clean 170" /tmp/digi-break-rx.log

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

# the MFSK16 and Olivia floors over a 2.5 kHz channel, measured with noise shaped to that band: the
# 6.8 dB between a 12 kHz and a 2.5 kHz SNR is arithmetic, and this is the audio that checks it
floors-2k5: digi_bench
	@./digi_bench floors-2k5

# the same measurement against the readings the README documents: eleven points, twelve seeds each,
# and a failure if a median or a range has moved outside its window. This is the step CI runs; the
# target above is the full coast, for reading.
floors-2k5-check: digi_bench
	@./digi_bench floors-2k5-check

# the recording paths for the four non RTTY modes: the modems' own takes with a long lead, written
# through the WAV reader a real capture goes through
record-check: digi_bench
	@./digi_bench record-check

# the capture decoder's own self-test, in capture/decode_oracle.js: 169 frames of the size a real
# take arrives in, filled with random bytes, have to be refused with a nonzero exit and no WAV.
# That is the check on the piece that decides whether a .stream is worth decoding at all - the
# client it loads is not committed (see the licence note in README.md), so this grades the framing
# walk and the refusal, and needs no capture, no client and no network.
oracle-check:
	@command -v node >/dev/null 2>&1 || { echo "FAIL  node is needed for capture/decode_oracle.js"; exit 1; }
	@node capture/decode_oracle.js --self-test

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

# what that 2.7 % is made of: one inserted code 16 (read as '5') and the space the take's line break
# stands in for, named with the mark and space level of each window against the take's median. It
# fails if the insertion becomes a misread, if the nine stops decoding as sent, if the break moves,
# or if either window loses the keying that makes a fade the wrong explanation.
fixture-align: digi_bench
	@./digi_bench align-check fixtures/ddk2-4583.wav 450 "$(DDK2_TEXT)" 50

# the decoder's own bit decisions, which is the only way to say which data bit of a wrong character
# was read wrong: a letter is five bits and the report carries only the letter. Diagnostic, so it
# prints rather than asserting
rtty-trace: digi_bench
	@./digi_bench rtty-trace fixtures/ddk2-4583.wav 450 "$(DDK2_TEXT)" 50

# a second path, the channel shape nothing else here produces: the same bit at two places in the
# audio. The sweep must stay under 5 % per cell (7 ms at -3 dB is its only non-zero cell, 4.2 %),
# and the 18 ms case outside it must keep costing the message: that is the echo failure itself
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
	rm -f digi_bench digi_bench-break-rx *.o *.wav *.pgm
# fixtures/ is untouched on purpose: the on air recording is tracked, not a build product
