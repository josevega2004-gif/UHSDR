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
#   make convolution-check  measure the RX filter USE_CONVOLUTION offers against the SSB stage the
#                radio runs today, from the shipped module: selectivity, sideband separation, cost
#                per sample and delay.  Needs no radio and no ARM toolchain.
#   make chain-levels-check  rerun the --chain-levels sweep and fail if any after-AGC top or deficit
#                moves more than 0.01 dB from nominal, or the positive-deficit set stops being the
#                eight n/c rows.  Needs no radio and no ARM toolchain.
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
DIRDIR   = ../../drivers/digi
FW_OBJS    = rtty.o audio_agc.o uhsdr_math.o sitor_rx.o hell_rx.o

CC       ?= gcc
# -MMD -MP: the bench allocates the firmware's structs on its own stack, so a stale object compiled
# against an old sitor_rx.h does not merely re-run differently, it writes past the frame it was given
# and dies in memset with nothing to say but a signal. That happened once while this file was being
# written, and an explicit header list is the sort of thing that is forgotten the second time someone
# adds an include, so dependency files are generated instead.
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -MMD -MP
# the link line takes the warning flags but not the dependency ones
LINKFLAGS = $(filter-out -MMD -MP,$(CFLAGS))
DEFINES   = -DUSE_RTTY_PROCESSOR -DHOST_BENCH
INCLUDES  = -I. -I$(SHIM) -I$(LIBDIR) -I$(LIBDIR)/cw -I$(LIBDIR)/softdds -I$(DIRDIR) -I../../hardware -I../../misc

# The firmware's own build passes -Wno-sign-compare (see the target Makefile): the shipping sources
# compare signed block sizes with unsigned sample indices in a few places, and the bench compiles
# them as they are rather than editing production code to suit the host. It applies to those
# sources only - the bench's own code is held to the full warning set, where a signed/unsigned
# comparison is a bug worth stopping for.
FWCFLAGS  = -Wno-sign-compare

digi_bench: $(BENCH_OBJS) $(FW_OBJS)
	$(CC) $(LINKFLAGS) -o $@ $^ -lm

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

# the streaming SITOR-B demodulator, compiled as the radio compiles it: this is the firmware file,
# and the reference modem in navtex.c reads the same alphabet tables it does
hell_rx.o: ../../drivers/digi/hell_rx.c ../../drivers/digi/hell_rx.h
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

sitor_rx.o: ../../drivers/digi/sitor_rx.c ../../drivers/digi/sitor_rx.h
	$(CC) $(CFLAGS) $(FWCFLAGS) $(DEFINES) $(INCLUDES) -c -o $@ $<

test: digi_bench
	@./digi_bench test

# A build with one receiver assumption deliberately broken: the receiver is told the other symbol
# speed, while the take it is handed is the one the case asked for. The RTTY table has to fail
# against it, so this target succeeds only when the broken build fails. A table that cannot see a
# broken receiver is not a table, and the noise rows are exactly where a receiver that is not being
# tested looks better.
digi_bench-break-rx: digi_bench.break-rx.o $(filter-out digi_bench.o,$(BENCH_OBJS)) $(FW_OBJS)
	$(CC) $(LINKFLAGS) -o $@ $^ -lm

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

# The same takes through the streaming demodulator in drivers/digi/sitor_rx.c, the one written to be
# compiled into the radio: one sample in, one character out, no whole-transmission buffer. Graded on
# two numbers rather than one, because CER cannot tell a character printed wrong from a character the
# receiver never got to - see the table's own text for what each row is pinning.
sitor-stream: digi_bench
	@./digi_bench sitor-stream

# Hellschreiber, but through the file the radio would compile: samples one at a time, the pixel clock
# found by a grid rather than assumed.  Graded against the same takes as `hell` above, so the column
# that matters is the difference between the two.
hell-stream: digi_bench
	@./digi_bench hell-stream

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
# The mode-identification gate, calibrated against modulated test audio: each mode's own takes have
# to pass with margins, controls (silence, noise, a single tone, band noise) have to be refused or
# pass through, and no mode may be able to claim another's signal.
gate-check: digi_bench
	./digi_bench gate-check

record-check: digi_bench
	@./digi_bench record-check

# the capture half's own self-tests, in capture/: decode_oracle.js and websdr_capture.py.  Neither
# needs a capture, a client or a network, and both are graded before anything is written.
#
# The decoder: a stream built element by element has to pass the framing walk with the counts it was
# built from, the same stream and a stream of blocks whose codes step between 0xd5 and 0xd6 both have
# to stay recordable to the rules in front of the decoder, and ten streams built to break one rule
# each - random bytes, stopped on an unknown message type, a silence run over 2 s, a rate that
# changes mid-stream, audio before the rate element, an 8-bit block one byte short, audio with no
# rate element, a take whose blocks are two copies of one code, a take that records nothing after its
# rate element, a take of that fill and a silence marker, bytes after the last message - have to be
# refused with a nonzero exit, no WAV and the whole complaint each was built to draw.  The two
# recordable cases are what catches a walk, or a rule, that has stopped accepting valid traffic; the
# ten refusals say it is still strict.  That is the check on the piece that decides whether a
# .stream is worth decoding at all - the client it loads is not committed (see the licence note in
# README.md), so this grades the framing walk and the refusals.
#
# The collector: 43 cases on fixtures/bandinfo-8band.js, the band table NA5B's front end serves.  The
# tune request it builds (the dial in kHz, which is the 20 November fault, and the band index with
# it), the eight spans, the band under each dial and the two gaps between them, the dials in those
# gaps being refused, --band outside the table being refused, a named band that does not cover the
# dial being warned about, the log line naming the band a part was tuned with, and the fetch that
# reads the table.  A table is taken whole or not at all, by two guards: an entry with a centre and
# no width is not skipped (it would move every index after it, and band= is an index), and a
# var nbands count the entries do not fill gives no table either.  A body that arrives encoded, or
# stops short of its own Content-Length, is an error rather than a short answer, for the same reason.
#
# What these two self-tests protect is the live retry, capture/ddk9_capture.sh, which is not a target
# here because it needs a receiver: it will not print a character error rate until the control on a
# dead channel has been refused as empty and the pinned DDK2 take has come back at its 2.7 %.
oracle-check:
	@command -v node >/dev/null 2>&1 || { echo "FAIL  node is needed for capture/decode_oracle.js"; exit 1; }
	@command -v python3 >/dev/null 2>&1 || { echo "FAIL  python3 is needed for capture/websdr_capture.py"; exit 1; }
	@node capture/decode_oracle.js --self-test
	@python3 capture/websdr_capture.py --self-test

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
	rm -f digi_bench digi_bench-break-rx convolution_probe *.o *.d *.wav *.pgm .convolution_defs filter_paths.gen.c
# fixtures/ is untouched on purpose: the on air recording is tracked, not a build product

# The convolution probe compiles drivers/audio/audio_convolution.c as *text*, because the arrays that
# hold the partition masks and the FFT history are file static and they are part of what is being
# measured.  So the probe is rebuilt when either file changes, and it is built against the bench's
# arm_math.h shim plus a host complex transform (convolution_fft.c) - which the probe then proves
# harmless by comparing the whole pipeline with a plain time-domain FIR that uses no FFT at all.
CONV_DEFS   = -DUSE_CONVOLUTION -D__IO=volatile
# The geometry of the filter is a build choice (see audio_convolution.h), so the harness that measures
# it takes the same choices: EXTRA_DEFS='-DFFT_CONVOLUTION_SIZE=64' measures the low latency variant,
# and the numbers it prints are that variant's, not the default's.  The tap count and the impulse array
# follow from the block size, so one flag moves the geometry rather than leaving one array sized for
# another build.
CONV_DEFS  += $(EXTRA_DEFS)

# ... but those choices are not visible to make's dependency rules, and a measurement taken from an
# object file that was compiled with yesterday's geometry is worse than no measurement.  So the flags
# themselves become a prerequisite: writing them to a stamp file makes the objects rebuild whenever the
# geometry changes, and stay put when it does not.
CONV_STAMP  = .convolution_defs
_conv_want := $(strip $(EXTRA_DEFS))
_conv_have := $(strip $(file < $(CONV_STAMP)))
ifneq ($(_conv_want),$(_conv_have))
$(file >$(CONV_STAMP),$(_conv_want))
endif

# and it must also exist as a target.  With no EXTRA_DEFS the two strings above are both empty, so
# nothing is written, and on a tree that has never been built - a fresh checkout, or any `make clean` -
# convolution_probe.o then depends on a file that does not exist and has no rule, which stops the step
# outright.  That is what `make clean` followed by `make convolution-check` did in CI and locally: the
# measurement was fine, the harness could not start.
$(CONV_STAMP):
	@printf '%s\n' '$(_conv_want)' > $@
CONV_INC    = -I$(SHIM) -I$(LIBDIR) -I$(LIBDIR)/filters -I../../hardware -I../../misc

# The probe checks every SSB and CW filter path the radio has, so it needs the radio's path table,
# FilterPathInfo[] in audio_filter.c.  The rest of that file is the filter setup of a running radio and
# does not build on a host, so the table is cut out of it as text: everything up to the line that
# closes the table.  If that line is ever reworded sed copies the whole file and the compile stops on
# the first function, which is the right way for this to fail.  The table points at the coefficient
# tables in filters/, all of which are plain data, so they are compiled as they are.
FILTER_TABLE_SRCS := $(wildcard $(LIBDIR)/filters/*.c)
FILTER_TABLE_OBJS := $(notdir $(FILTER_TABLE_SRCS:.c=.o))

filter_paths.gen.c: $(LIBDIR)/audio_filter.c
	sed '/^}; \/\/ end FilterPath/q' $< > $@

filter_paths.o: filter_paths.gen.c $(LIBDIR)/audio_filter.h $(LIBDIR)/filters/filters.h $(CONV_STAMP)
	$(CC) $(CFLAGS) $(CONV_DEFS) $(CONV_INC) -c -o $@ filter_paths.gen.c

$(FILTER_TABLE_OBJS): %.o: $(LIBDIR)/filters/%.c $(LIBDIR)/filters/filters.h $(CONV_STAMP)
	$(CC) $(CFLAGS) $(CONV_DEFS) $(CONV_INC) -c -o $@ $<

convolution_probe: convolution_probe.o convolution_fft.o filter_paths.o $(FILTER_TABLE_OBJS)
	$(CC) $(LINKFLAGS) -o $@ $^ -lm

# Everything the probe compiles in as text or includes, spelled out: it evaluates CONVOLUTION_USED_FOR,
# which lives in audio_driver.h, and the mode values behind it, so a header change that is not listed
# here leaves the probe checking a stale object.  A mutation of that macro sat unnoticed behind exactly
# this omission until the dependency was written down.
convolution_probe.o: convolution_probe.c $(LIBDIR)/audio_convolution.c $(LIBDIR)/audio_convolution.h \
        $(LIBDIR)/audio_driver.h $(LIBDIR)/audio_filter.h $(LIBDIR)/filters/filters.h \
        ../../hardware/uhsdr_demod_modes.h \
        $(SHIM)/uhsdr_board.h $(SHIM)/arm_math.h $(SHIM)/arm_const_structs.h $(CONV_STAMP)
	$(CC) $(CFLAGS) $(DEFINES) $(CONV_DEFS) $(CONV_INC) -c -o $@ convolution_probe.c

convolution_fft.o: convolution_fft.c $(SHIM)/arm_const_structs.h
	$(CC) $(CFLAGS) $(CONV_INC) -c -o $@ convolution_fft.c

# A nonzero exit on its own proves nothing, so the probe says which of its checks it failed on: the
# partitioned pipeline has to reproduce a plain complex FIR over the same taps, and the sideband
# separation it measures has to be at least the one the 199-tap Hilbert pair gives today.
# The README's AGC level-invariance claim, measured rather than remembered.  This runs the sweep
# itself and grades the table it prints, so a change that loses a row or a column fails here too.
chain-levels-check:
	@sh convolution_graphs/check-chain-levels.sh "$(abspath ../..)"

convolution-check: convolution_probe
	@./convolution_probe

# Generated dependency files.  Every rule above compiles with -MMD, so a header change rebuilds what
# includes it without anyone maintaining a list; the explicit lists that are still written down (the
# probe's, and sitor_rx.o's) stay, because they also say which files are compiled as text and which
# switches have to be re-run.
ALL_DEPS = $(BENCH_OBJS:.o=.d) $(FW_OBJS:.o=.d) digi_bench.break-rx.d \
           convolution_probe.d convolution_fft.d filter_paths.d $(FILTER_TABLE_OBJS:.o=.d)
-include $(ALL_DEPS)
