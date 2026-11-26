# Digital mode host bench

Grades the radio's own modems on a PC. Nothing in this directory is compiled into the firmware.

```
make -C support/digi test     # self checks, non zero exit on regression   (also: make digi-bench)
make -C support/digi break-rx # negative control: the real build passes, the broken one fails clean 170
make -C support/digi rtty     # RTTY character error rate, modem looped back on itself
make -C support/digi rtty-long  # the same matrix, eight copies of the message (one character 0.26 %)
make -C support/digi wefax    # synthetic HF weather fax through the reference demodulator
make -C support/digi navtex   # NAVTEX / SITOR-B through a reference modem: alphabet and repeat copy
make -C support/digi hell     # Hellschreiber: on/off keyed raster, scored as a picture
make -C support/digi mfsk16   # MFSK16: 16 tones, Varicode, rate 1/2 code, interleaver over 8 symbols
make -C support/digi olivia   # Olivia: 32 tones, 64 symbol Walsh blocks, five characters a block
make -C support/digi floors-2k5 # the MFSK16 and Olivia floors over a shaped 2.5 kHz channel, median
                          # and range of twelve noise seeds a point (about 40 s)
make -C support/digi floors-2k5-check # the points this file quotes, measured again and checked
                          # (CI, about 15 s)
make -C support/digi record-check # the four non RTTY modes reading a WAV with a long lead
make -C support/digi oracle-check # the capture half, self-tested: the stream rules and the band maths, no network
make -C support/digi rtty-ddk2  # the one on air case: a real DDK2 recording off the NA5B WebSDR
make -C support/digi fixture-align  # per character: what the take sent, what the modem read, and the audio
make -C support/digi rtty-trace    # the demodulator's own bit decisions, for the same take
make -C support/digi rtty-multipath  # the same modem against a second path: delay spread, 30 cells
make -C support/digi tx-check      # the modulator's own keying through the receiver's gate
make -C support/digi capture-check  # is a recording fit to be pinned: pair, bit clock, continuity
make -C support/digi fixture-check  # do fixtures/MANIFEST.md's numbers still describe the wav
./digi_bench rtty-tune     # how much tuning error the demodulator survives, in Hz and ppm
./digi_bench capture-check <file.wav> [shift] [45|50 baud]
./digi_bench rtty-wav <file.wav> [shift] ["expected text"] [45|50 baud] [max CER %]
                            # decode a recorded file with the firmware demodulator; the optional
                            # CER limit turns a pinned recording into a graded case
./digi_bench rtty-align <file.wav> <shift> "expected text" [45|50 baud]
                            # the same per character table for any take: what the modem read of
                            # each character and the audio under it, without the DDK2 junction
                            # check align-check pins
./digi_bench navtex-wav <file.wav> ["expected text"] [max CER %]
./digi_bench mfsk16-wav <file.wav> ["expected text"] [max CER %]
./digi_bench olivia-wav <file.wav> ["expected text"] [max CER %]
./digi_bench hell-wav <file.wav> [out.pgm]
                            # the same recorded input for the other modes; leading silence is
                            # trimmed and the start is searched across one symbol
```

## The capture gate, `capture-check`

A take is worth pinning only if the audio really holds the signal: two tones the modem's filters can
use, a bit clock that repeats, and no hole in the middle. Each of those has been assumed rather than
measured at least once here - a pair 1.5 kHz off the filters, "two tones" that were one carrier's
skirt, a take that was mostly noise, two transfers that arrived truncated. `capture-check` measures
them from the file itself, before any decoding, so it cannot be talked into agreeing with the modem.

```
make -C support/digi capture-check
  capture   : fixtures/ddk2-4583.wav
  audio     : 4000 Hz, 13.70 s, peak 0.22 of full scale
  tones     : 932 / 1374 Hz, separation 442 Hz (declared 450), centre 1153 Hz
              spread 15.8 / 14.9 Hz; offset from the modem's 915/1365 -17 / -9 Hz
  bimodality: 0.47 low, 0.39 high, 0.14 both (5 ms windows, 2740)
  clock     : period 20.000 ms (357 transitions), scatter 8.8 % rms / 7.0 % MAD of a bit
  dropouts  : 21 gap(s) below 15 % of the median, longest 2 ms (0.2 % of the take)
  ACCEPT    : the pair, the bit clock and the continuity are all inside the gate
```

Two of the numbers above are the demodulator's own rather than this file's: the mark centre is
label 0 of `Rtty_BenchFilterLabel()` and the space centre is the label the firmware picks for the
shift it is set to, so retuning `rtty.c` moves the gate, the synthesiser and the report with the
radio. Restating 915 and 1365 here is the same fault as a README quoting figures it remembers.

Three exit statuses, and none of them is a verdict on its own: 0 for an ACCEPT inside every limit, 1
for a REJECT *or* for an ACCEPT whose limits it broke, and 2 for a WAV it never opened, which is not
a verdict about radio at all.  So the verdict is read off the printed line, and the status is used
only to keep a missing file from being banked as a refusal - `capture/ddk9_capture.sh` reads 2 as
"the gate did not run", which is the one reading a control step is not allowed to make of a pass.

The four numbers, and why each one is measured the way it is:

- **tones** are refined on 5 ms windows that hold one tone only, so a dial offset reports as an
offset instead of a pair the modem cannot filter. 5 ms matters: at 20 ms a straddling bit
reads as both tones at once and any pair looks well behaved. - **bimodality** is the fraction
of those 5 ms windows holding one tone or both. A two tone signal being switched reads about
0.86 single tone and 0.14 both, the both being transitions; a single carrier or a stretch of
noise reads near 0.0 single and 1.0 both, whatever its spectrum looks like. - **clock** fits
the bit period to the times the signal changes tone, then measures how far those transitions
scatter off that grid, as a percentage of a bit. Each transition is the zero crossing of the
tone discriminator, interpolated between the last 5 ms window that settled on the old tone and
the first that settled on the new one: the window centre trails the change and folds that smear
into every reading. A Baudot character is 7.5 bits long - one start, five data, 1.5 stop - so
the transitions at a character boundary sit half a bit away from the ones inside it. The period
is fitted on the squared phase, which cancels that half bit and leaves the bit itself, and the
two phases come back as two centres that the scatter is measured against. Without the split
the fixture fitted to 18.75 ms - 6 % short of the rate the modem decodes it at - with 13.9 %
scatter, and that shape looked like a recorder rate error; with it the same audio reads 20.000
ms and 7.0 % median scatter (8.8 % rms), which leaves the take's 2.7 % to the text rather than
to the clock. The reassignment is seeded from twelve phases around the circle, because seeded
once from the take's mean phase a split whose halfway boundary lands inside a cluster leaves
each bucket holding half of both and the centres stranded between them: a fixture stretched 3 %
then reads 24.7 % of a bit of scatter while decoding at 4.1 %. `fixture-align` places it there:
the two characters the scorer charges carried their own keying at the take's own mean level,
their worse tone 7.1 and 5.5 dB under it, so 2.7 % is the station's extra nine and the line
break it sends where the pinned text has a space - not a copy that faded. A clean synthetic take
measures 20.000 ms and 0.1 %, and a 45.45 baud take measures 22.000 ms with 2.2 % and is refused
on the period alone, though its pair and its scatter look perfect. The fit searches 30 % either
side of the nominal period rather than at it, so a take at the wrong rate reports the rate it
carries: a 40 baud take reads 25.00 ms and 40.0 baud, and the rejection line names that beside
the period. The window is a fraction of the bit and the bench asserts that both its ends sit
clear of half a bit and of twice a bit, so a take repeating on a two bit lattice can only be read
as its bit. - **dropouts** are 1 ms slots below 15 % of the median level, reported as a count
and the longest run. Trimming the ends of a take does not remove a stall in the middle of one.

The limits are set just outside the fixture's own numbers (single tone above 0.55, both below
0.35, period within 5 %, no gap over 5 ms). The period window passes both reference takes at their
nominal period and refuses a 45.45 baud take at 10 % long. Scatter is printed and refuses nothing:
it is read as a median absolute deviation, so one stray crossing cannot move it, and it says how
much the recording has been through rather than whether it may be graded. The fixture reads 7.0 %
(8.8 % rms), stretched by 1, 2 and 3 % either way 6.4 to 7.4 % while still decoding at 2.7 to
6.8 %, and a clean synthetic take 0.6 %. What showed it cannot be a limit is the same fixture
re-read with 4 ms of wow: its clock wanders by 0.2 of a bit, which the receiver follows to 9.6 %
CER, but its transitions read 13.3 % MAD and the 12 % limit refused it while the text was there -
and at 3 ms of wow, 11.0 % CER at 12.1 % MAD, the same. The rate decides the gate and the
character error rate decides the take; one that fails the pair, the bimodality or the period is
not evidence about the modem either way. The gate has already earned its place: a 24 s take of the
10.1 MHz band whose free decode was `VXTYGJJUKD` reads 234 Hz apart with 0.66 of its windows
holding both tones and is refused, while a synthetic 50 baud take - the same program, the same
filters, where the pair really is switched - reads 0.00 both tones and 20.000 ms and is accepted.

## Recorded input for NAVTEX, Hell, MFSK16 and Olivia

`navtex-wav`, `mfsk16-wav`, `olivia-wav` and `hell-wav` are the recording paths for the modes
whose tables otherwise hand the decoder a buffer starting on a symbol boundary. Each takes the
same 16 bit PCM WAV as `rtty-wav`, resamples it to 12 kHz and trims gain from the file peak. It
then finds the first 5 ms window above 2 % of the take's loud windows - the 99th percentile,
so a click or a burst of static cannot lift the threshold over the signal - that the next 50
ms stay above, and starts the search there. A recording may therefore open with seconds of
silence, with the settling click a recorder writes first, or with a crash of static before the
transmission. The symbol clock has no phase known in advance: the block modes try starts over
one symbol in sixteenths and keep the decode with the fewest characters wrong (or, when no text
is given, the most symbols heard); NAVTEX and Hell are already fitting the bit/pixel phase
and column grouping themselves, and are given the same short set of starts around the onset.
`hell-wav` writes the decoded raster as a PGM, since Hell has no character alphabet to score.

The trials decode only the first 20 s of the take, which is enough to rank a start,
and the take is then decoded once and whole from the start they chose: nine whole
file decodes of a five minute NAVTEX broadcast took 163 s and now take 24 s, and the
longer block modes gained the same way. What the decoders cannot hold is cropped
rather than silently refused - 131 s of MFSK16, 209 s of Olivia, and the 2000 column
Hell raster, about 16 s of picture - and the verb prints the crop when it takes one.

```
./digi_bench mfsk16-wav recording.wav "MFSK16 TEST DE UHSDR 12345" 0
  file      : recording.wav
  format    : 12000 Hz, 1 channel(s), 7.41 s
  onset     : 750.0 ms (sample 9000)
  phase     : trial 5, start 8472 samples (44.0 ms before onset)
  decoded   : MFSK16 TEST DE UHSDR 12345
  symbols   : 104, heard 100.0 %
  CER       : 0.0 %
```

The optional expected text and CER limit make a recording a graded case, as with
`rtty-wav`; without them the verb prints the decode and the phase it chose.
`make -C support/digi record-check` writes each mode's own take with a 0.75 s lead into
`/tmp` and decodes it through these paths, which is the first check here that does not hand
a decoder the transmission start. Hell has no text to score, so the PGM the verb wrote is
read back and compared pixel by pixel with the picture the generator drew: the raster may
slide by a column at the ends, nothing else may move, and a one-column shift fails the check.

`rtty-wav` takes any 16 bit PCM WAV, any sample rate, mono or stereo: it resamples to the
12 kHz the demodulator is pinned at and trims gain from the file peak, so a quiet recording
still reaches its thresholds. The reader steps over chunks it does not need (`LIST`, `bext`,
`fact`), accepts the extensible header ffmpeg and several recorders put around plain PCM,
and decodes the audio that did arrive from a data chunk a recorder left cut off. **Note for
real signals**: `rtty.c` filters are built around a mark of 915 Hz, while amateur RTTY puts
mark and space at 2125/2295 Hz, so a live ham RTTY recording has to be tuned (or spectrally
translated) so the pair lands at 915/1085 Hz audio. NAVTEX style pairs at 1615/1785 Hz are
outside the filter bank as well, which is a real limit on reusing this modem for that service.

### WEFAX, `./digi_bench wefax-wav take.wav`

WEFAX had no recording path at all until now: its table hands the decoder a buffer that begins on a
line boundary and already knows the picture. The verb loads a take, trims to its onset the same way
the other four do, decodes, and writes two PGMs - one each way round, because the table's own
`invert` row shows the decoder does not choose polarity, so the eye decides.

It reports lines captured, the grey mean, the black/mid/white shares, the mean purity of the windows
it accepted as sync tips, the spacing between those tips, and the row statistics.

**There is now a verdict, and it is about acquisition rather than content.** A 120 lines/min emission
puts its sync tips on a 6000-sample grid, so the spacing between the tips the capture accepted is a
number the receiver can compute without knowing what the picture shows. Measured, it separates by two
orders of magnitude: **2.6 samples** on a generated broadcast, **335.4 samples** on `ddk2-4583.wav` -
a real RTTY recording, on air, in the repository as a fixture - and **317.1** on a real 25 s take of
12262.5 kHz that turned out not to be carrying fax at that hour. Both ends of that are pinned in
`make test`: the metric must accept the chart it made and reject the RTTY recording. The positive
control generates and writes its own take inside the check, because reading `wefax_snr20.wav` would
have made the check pass or fail on whichever target happened to run last.

What the earlier refusal said is still true of *content*, and the reason it was written down is worth
keeping: the first version of this did judge, from "rows that vary" and "rows that start dark", and got
it exactly backwards - the real chart has empty sky rows and a sync tip outside the picture area, while
an RTTY take decoded as fax produces rows that swing from end to end and begin black. Grey statistics
are not a mode signature. Tip spacing is, and the decoder does not yet choose polarity, which is why
two pictures are written and the eye still decides whether the sky is up.

**What joining a transmission mid-line costs, measured in `record-check`:** a take with a 0.75 s lead,
which is one and a half line times and therefore neither a line boundary nor a symbol boundary, returns
88 % of the chart it was sent.  It used to collapse, and not gradually: any lead from 30 samples to 6000
gave the same 32-45 % of matching pixels, the same grey mean of 93 and the same 82 % of the picture at
one flat grey, to four significant figures.  A number that does not move when the cut moves is the
signature of not being cut-relative at all - the receiver was starting wherever the waveform told it to.
What told it was the phasing tone that closes every HF facsimile line: it sits 43 Hz from the 1857 Hz
sync tip, inside the same bin, so a discriminator asking "is this window 1857 Hz" could not tell a line's
start from a line's end and took whichever scored higher.  Reading a line from its end is exactly what
flat grey with a dark tick at each row looks like.

Three changes closed it, each described with the rest of the capture work in the section below: placing
the tip at the leading edge of the ratio's plateau instead of at its peak, opening the search window at
the end of the previous picture - the blanking gap, whose content the format fixes - instead of one line
after the previous tip, and numbering each row by the tip that was found rather than by how many lines
had been found.  The earlier attempt at the same problem, requiring picture tones *after* the candidate,
broke the clean decode instead (0 of 12 lines) and was reverted; what precedes a tip is known in advance,
what follows it is the picture.

The residual 12 % is not attributed yet: the harness reports the aggregate, and pinning a cause to it
would need the per-row breakdown.  What is pinned as still open is the line-rate gap in the known gaps
below, which is a different symptom of the same acquisition.

So the WEFAX recording path can now be aimed at a capture off a waterfall, which is the situation it
exists for.

## What is real and what is scaffolding

`drivers/audio/rtty.c` is compiled from this Makefile, together with its own `rtty.h`
(the bench hooks it exposes live in the file behind `HOST_BENCH`, which the radio
build never defines, and nothing in the modem's own path is changed), and so is
`drivers/audio/audio_agc.c`, whose cold-start reset hook is behind the same guard:
every case runs through the radio's audio AGC before the demodulator, so the modem sees
the level the receiver would hand it rather than the modulator's own output. Both the
transmitter (`Rtty_Modulator_GenSample`), the AGC (`AudioAgc_RunAgcWdsp`) and the receiver
(`Rtty_Demodulator_ProcessSample`) in the numbers below are firmware code. The only substitutes:

| file | replaces | why it is safe |
|---|---|---|
| `shim/ui_driver.h` | `UiDriver_TextMsgPutChar`, `ts.rtty_cwi` | the modem asks the UI layer for exactly these two things |
| `shim/radio_management.h` | `RadioManagement_Request_TxOff` | one call, its firing is itself a checked item |
| `shim/uhsdr_digi_buffer.h` via the real header | the TX character buffer | real enum and prototypes, scripted feed in `modem_host.c` |
| `shim/softdds.h`, `softdds_host.c` | the sine table DDS | computed instead of looked up; `test` measures the produced tone (915.0 Hz asked, 915.0 Hz measured) before grading anything with it |
| `shim/arm_math.h` | CMSIS types | `hardware/uhsdr_types.h` includes that header only for `float32_t` and friends; the AGC additionally needs the filter instance types `audio_driver.h` names, `pow10f` (glibc has no such function) and `arm_max_f32`/`arm_min_f32`, whose four argument form `misc/uhsdr_math.c` calls |
| `shim/uhsdr_board_config.h` | `hardware/uhsdr_board_config.h` | the real one selects a board and includes STM32 headers; the AGC takes two numbers from it, the 48 kHz rate and the 32 sample block, and they are mirrored at the mcHF values |
| `shim/uhsdr_board.h`, `shim/profiling.h` | board switches, counters | empty; the switches the modem needs come from `-D` |

The chain around the modem is `audio_driver.c`'s 12 ksps one: the AGC, then the fixed post-AGC
gain (`POST_AGC_GAIN_SCALING_DECIMATE_4 * 0.333`, read from the real header), then the
demodulator. Two stages of that pipeline are left out by name rather than by accident: the
operator's notch/peak/lowshelf biquad, which stays a passthrough until a shape is picked and is
a station setting this bench does not measure, and the impulse blanker, which needs an impulse
and no case sends one. The AGC runs on the factory configuration from `audio_agc.h` (MED, 70 dB
slope, 20 dB threshold), because the firmware loads those from EEPROM; it is fed in ADC counts
and its output scaled back into the bench's +-1 samples, because a +-1 input is 72 dB below the
level its `max_input`/`out_target` are written for and its thresholds would then mean something
else. Every case starts it cold: the AGC carries gain, ring buffer and hang counter from one
transmission to the next, which would make a level row depend on the case before it, the same
order dependency the WAV check had. `-Wno-sign-compare` is the target build's own flag and is
passed to the firmware sources only, so those compile as they ship while the bench's own code
stays under the full warning set. Moving the chain in found the WAV bug - the round trip had never
reset the decoder, so it started from the previous case's filter state and only passed because
that state happened to be harmless - and it withdrew the loopback's ability to grade the shift
until the transmitter's tones could be held where they belong while only the receiver's setting
moved, which is what `Rtty_BenchTxSpaceTone` is for and what the shift bullet above records.

The shims are deliberately thin. If a firmware source ever reaches for more of the radio than
this, the bench stops compiling instead of quietly inventing behaviour, which is the point:
`drivers/audio/cw/cw_decoder.c` does not compile yet, because it needs `AudioFilter_Goertzel*`
from `audio_filter.c`, `CwGen_CharacterIdFunc`, six `ts.` members and `Board_RedLed`. That list is
the size of the CW lift, measured rather than guessed.

## Facts about the RTTY modem this bench established

* The transmitter runs at **4x the receive rate**: `Rtty_Modulator_GenSample()` holds each bit for
  `oneBitSampleCount * 4` calls while `Rtty_Modem_Init()` pins the demodulator at 12 kHz. A host
  loopback therefore has to generate at 48 kHz and decimate, or the bits arrive at 11.4 Bd and
  nothing locks. (This is how the first version of this bench lost every character.)
* The TX path is **MSK shaped**: bit changes are deferred to a zero crossing and the accumulator is
  stepped twice on those samples, so a naive zero crossing count of the idle tone reads high.
* `Rtty_Modem_Init()` accepts shifts 85/170/200/425/450/850 and **silently maps anything else to
  170**; mark is 915 Hz, space above it. A stored value outside that list decodes as 170.
* **The shift setting matters much more on live radio than on a synthetic loopback.** Mark is
  hard-wired to the 915 Hz bandpass (`rtty_bp_12khz_915`) and the space filter is picked
  from the shift. Evaluated on the real coefficient tables as `|H(e^jw)|` of the `xv`/`yv`
  recursion, that 915 Hz filter peaks at 913.5 Hz with its 3 dB band at 865-965 Hz (about
  100 Hz wide), and rejects the space tone by 22.9 dB at 170 Hz shift, 26.1 dB at 200,
  43.1 dB at 425, 44.7 dB at 450 and 83.9 dB at 850, but by only 10.2 dB at 85. On a clean
  generated two-tone recording the ATC comparison can therefore decide from the mark channel
  alone - which is why decoding a 170 Hz-shift *synthetic* recording returns 0.0% CER
  whichever shift is selected, as the code's own comment predicts (*"works quite well with
  mark-only/space-only"*). **Do not generalise that.** The same test on a live DDH 8 capture gives:

  | selected shift | 85 | 170 | 200 | 425 | **450** | 850 |
  |---|---|---|---|---|---|---|
  | live decode | fails | fails | fails | full text | **full text** | fails |

  So on air the space filter's selectivity is doing real work that an equal two-tone synthetic
  signal never asks of it: with both tones at full level the 915 Hz filter keeps the right polarity
  whatever the space filter does, so those rows decode at 0.0 % even when the receiver is given the
  850 Hz space filter for a 170 Hz signal. One row was built to break that: `weak mark 170` sends
  the mark tone 30 dB below the space tone, past the 22.9 dB by which the mark filter rejects the
  space tone, so the mark channel inverts and the space filter has to carry the bits. That row
  stopped grading anything the moment the shift setting started to move both ends of the loop: the
  transmitter took its shift from the receiver's filter, so "the 850 Hz setting" was really an
  850 Hz signal with an 850 Hz filter and decoded perfectly. `Rtty_BenchTxSpaceTone()` separates
  them - the signal stays at 170 Hz shift while only the setting moves - and with that, and the
  mark tone 30 dB down, the setting is graded again: 0.0 % at 170 and at 200 Hz (adjacent bands),
  **97.9 %** at 85, 425, 450 and 850. The clean two-tone signal still decodes at 0.0 % with every
  setting, because the mark channel carries the bits whatever the space filter does; that is
  measured, printed next to the other column by `rtty-shift`, and it is why the guard row needs
  the weak mark. A live capture, of course, still needs the tones on the modem's own frequencies.
* Inside the ATC branch the variable names are the opposite of what they hold. `mark_mag`/`mark_env`
  carry the **space** filter output and `space_mag`/`space_env` the **915 Hz mark** output, because
  the block starts by exchanging them to cancel the naming convention of the FlDigi formula it was
  taken from. The polarity is nevertheless correct and was checked against the modulator, which
  indexes `tx_dds[]` by the bit value with `tx_dds[1]` = mark = 915 Hz: a dominant mark tone drives
  `v1` negative and `return (v1 > 0) ? 0 : 1` yields 1. The non-ATC branch reaches the same result
  by inverting the mark line with no exchange at all, which is the confirmation that the exchange
  is a compensation and not the "dirty fix" its old comment suggested. **Do not remove it.**
* The demodulator emits **2 characters of junk** before it locks onto a transmission.

## Baseline, `make -C support/digi rtty`

Message `DE UHSDR BENCH RTTY 12345 TEST 45 BAUD 170 SHIFT`, 45.45 Bd, 1.5 stop bits.
CER is measured after finding the message anywhere in the take: the characters before
it and after it are free, so a take that opens inside a bulletin's RYRY run is scored
on what its message says and not on the bulletin around it. Every noise row is the
mean of five noise seeds and the table prints the seed range beside it; a row with
no channel in it is one draw and shows a dash, and the limit is applied to the mean.

| case | CER % |
|---|---|
| clean, 170 / 425 / 850 Hz shift | 0.0 |
| AWGN 20, 10, 7 dB | 0.0 |
| QSB 0.4 Hz, 6 dB deep | 0.0 |
| QSB 2 Hz, 10 dB deep | **21.2** mean over five seeds (0.0..47.9, 23/48 on the first seed); **69.5** mean (0.8..93.6) in `rtty-long` |
| weak mark, 30 dB below the space tone | 0.0 with the 170 Hz space filter (0.0 on every seed), **97.9** with the 850 Hz one (`rtty-shift`, one draw) |
| 170 Hz signal decoded with the 850 Hz setting, mark 30 dB down | **95.8** mean (87.5..97.9; 47/48 on the first seed), against 0.0 % at 170 and 200 Hz |
| weak mark + carrier 300 Hz above the space tone at +20 dB | 0.0 at 170 Hz on every seed, **31.2** with the carrier at +30 dB (11.7 in `rtty-long`); **97.9** at either carrier level once the setting is 850 Hz |
| reference error: 1 and 3 ppm on 20 m (14 and 42 Hz), 2 ppm on 10 m (56 Hz), 1 ppm on 6 m (50 Hz) | 0.0 |
| reference error: 1 ppm on 20 m on the 450 Hz / 50 Bd signal (14 Hz) | 0.0 |
| reference error: 56 Hz (4 ppm at 14 MHz) at 10 dB, at 7 dB and under 0.5 Hz / 6 dB QSB | 0.0 |
| input level -10, 0 and +10 dB | 0.0 at each: the AGC holds the modem where it should be |
| input level -30 dB | 0.0 (0/48, and 0/392 in `rtty-long`) |

The message is 48 characters, so one wrong character is 2.1 % and a "0.0" row only says *under
2.1 %*. Every row prints `wrong/total` for that reason, and `make -C support/digi rtty-long` sends
eight copies of the text - 392 characters with the spaces between them - through the same cases and
the same limits: a clean row is then under 0.26 %, which separates a decoder that is exactly right
from one that is quietly worse. Measured, every row that is clean at 48 characters is clean at 392,
so the loopback has no residual errors left to find. The two rows that are not clean are clean in
neither length and carry a limit in both: fast fading at 21.2 % mean over five seeds short
(0.0..47.9) and 69.5 % long (0.8..93.6), and a station sent at 45.45 Bd but decoded as 50 Bd at
69.6 % (66.7..70.8) short and 75.2 % (68.4..79.8) long.

The AGC changes what this table says, and the honest version is that the earlier one was partly a
harness artefact. The demodulator has no input level control of its own; it inherits whatever the
audio AGC does. The bench used to feed the modem directly and assume the rest, then ran the AGC on
a +-1 float signal - which is 72 dB below the ADC-count level the AGC is written for (`max_input`
and `out_target` are both `ADC_CLIP_WARN_THRESHOLD`, 4096, and the firmware's audio buffer carries
converter counts). Fed in its own units, from a cold start for every case, the AGC does its job: a
station 30 dB down decodes as well as one at nominal level, 0.0 % (0/48 and 0/392) at -30, -10, 0
and +10 dB. The level rows therefore record the AGC working, not the modem being level proof.

Two things still break it, and they are measured with the same chain. Fast fading, 2 Hz at 10 dB
deep, costs **21.2 %** mean over five seeds (0.0..47.9, 23/48 on the first) at 45.45 Bd and **69.5
%** mean (0.8..93.6) over the long message: on the draws it catches, the modem loses lock partway
and the rest never comes out. That spread is the point of running five seeds - a fade either reaches
the lock or it does not, so one draw could not say where the row is. The 6.2 % this row used to
report was measured against a noise generator whose state never advanced, so the row carried no
noise at all. That class of bug is now checked before any case runs: the self test requires the
added noise to have zero mean and to change on every sample, and the fading envelope to reach its
trough and peak and to change on every sample, so a frozen generator fails the bench instead of
making its rows look better. It also runs one case twice, with and without noise, and requires the
difference - the noise the receiver is actually handed - to have zero mean, the level the SNR asks
for, and a new value on every sample. In the other direction, `make -C support/digi break-rx` builds
the bench with the receiver told the other symbol speed, and the target only succeeds when the real
build passes that same table, the broken build fails it, the failure names the `clean 170` row and
its character error rate, and the run reaches the end of the table. A table that can no longer see
a broken receiver fails there, and so does a bench that crashed or failed on an unrelated row -
a nonzero exit on its own is not the control this is. A strong signal next door is the remaining
cliff: an unmodulated carrier 300 Hz above the space tone is harmless at +20 dB over the wanted
signal and costs **31.2 %** at +30 dB, with the weak mark tone in the same row holding the bits on
the space filter. That is a filter rejection limit, not a level one - which is what makes it the
interesting failure, since the AGC cannot help with it. It reproduces without a radio or an antenna.

## Shift setting, `make -C support/digi rtty-shift`

The signal is 170 Hz / 45.45 Bd and only the receiver's setting moves, which is what a radio set
wrong does. Two columns, because they say different things: the clean signal decodes whatever the
setting is, while the signal with its mark tone 30 dB down puts the decision on the space filter and
then the setting matters. Measured at 20 dB SNR:

| setting | clean % | weak mark % |
|---|---|---|
| 85 Hz | 0.0 | **97.9** |
| 170 Hz | 0.0 | 0.0 |
| 200 Hz | 0.0 | 0.0 |
| 425 Hz | 0.0 | **97.9** |
| 450 Hz | 0.0 | **97.9** |
| 850 Hz | 0.0 | **97.9** |

170 and 200 both decode because they are one filter band apart; everything else costs the text. The
mode asserts the matching setting and the 850 Hz one, and the matrix carries the same guard as the
`shift 170/850` row so the ordinary `rtty` run fails too if the setting stops mattering. What it
does not do, and cannot, is reproduce the on-air result that a real DDH 8 capture decodes only at
425/450 Hz: that comes from the signal, not from a synthetic two-tone.

## Symbol clock, `clk` rows

The receiver's bit clock is not free running: it is a DPLL that nudges itself by **1/32 of a bit**
on every transition (`DPLLBitPhase +/- oneBitSampleCount/32`), so a transmitter whose clock is off
is followed rather than fought - up to the point where the phase runs away from it.
`Rtty_BenchTxBitScale()` is the hook that asks how far: it scales the modulator's bit length, which
is `oneBitSampleCount * 4` samples of its 48 kHz output, and leaves the receiver's own bit clock
where it was. Measured at 20 dB SNR, 170 Hz shift, in 0.5 % steps around the edges:

| TX symbol clock | CER %, short (48 char) | CER %, `rtty-long` (392 char) |
|---|---|---|
| -7, -6, -5, -4, -3, +3, +4, +5, +6, +7, +7.5, **+8** % | 0.0 (0/48) at every one | 0.0 (0/392) at every one |
| **+8.5 %** | **6.2** (3/48) | **8.9** (35/392) |
| +8.75 %, +9 % | 18.8, 14.6 | 17.3, 23.5 |
| **-7.5 %** | **20.8** (10/48) | **15.8** (62/392) |
| -8, -8.5, -9 % | 64.6 to 68.8 | 65.1 to 69.4 |
| 45.45 Bd decoded as 50 Bd (-9.1 %) | **70.8** (34/48) | **69.4** (272/392) |

The window is about **+8 % / -7 %**, and it is not symmetric: the
decoder follows a transmitter that is fast up to +8 % and a slow one
only to -7 %. The failure either way is a cliff rather than a slope - +8
% is exact and +8.5 % already costs characters - and past it the text
collapses to a handful of surviving characters. Why the DPLL tolerates
one direction more than the other is measured but not explained here.

Four rows pin that edge instead of sampling the tolerance evenly: `clk +8 %` and `clk -7 %` inside,
exact in both matrices, and `clk +8.5 %` and `clk -7.5 %` at it, where the bench asserts the failure
with a floor rather than a ceiling (minimum 3 % and 10 %) so a demodulator change cannot quietly
widen the window. The wrong-speed row is the far side of the same curve - a 45.45 Bd station handed
to the radio as 50 Bd sends bits 10 % longer than the clock expects - and it is pinned the same way,
with a minimum of 50 %. A change that made either of those decode would be a different demodulator,
not a better one.

## Tuning error, and what our own reference is worth

The demodulator has **no automatic frequency control**. `bpfMarkConfig` is always the 915 Hz
filter and the space filter is chosen from the shift setting alone; nothing looks at where the
tones actually are. A frequency error is therefore not pulled in - it walks both tones away from
the filters and only the operator's tuning brings them back. Both tones move by the same number of
Hz, so the shift is unchanged: this is a tuning error, not a shift error.

A ppm only becomes an audio figure once it is multiplied by the RF frequency:

| ppm | 3.5 MHz | 14 MHz | 28 MHz | 50 MHz |
|---|---|---|---|---|
| 1 | 3.5 Hz | 14 Hz | 28 Hz | 50 Hz |
| 5 | 17.5 Hz | 70 Hz | 140 Hz | 250 Hz |

`make -C support/digi rtty-tune` walks the pair off the filters and prints the curve. Three offsets
are asserted - 50 Hz with the 170 Hz shift and equal tones, 28 Hz with the mark tone 30 dB down,
and 100 Hz with the 450 Hz shift at 50 Bd - because those sit far enough inside the measured edges
that a libm difference between build hosts cannot move them. Measured at 20 dB SNR, CER % for a
negative / positive error:

| error | 170 Hz shift | 170 Hz, mark 30 dB down | 450 Hz / 50 Bd |
|---|---|---|---|
| 14 Hz | 0.0 / 0.0 | 0.0 / **97.9** | 0.0 / 0.0 |
| 50 Hz | 0.0 / 0.0 | **8.3** / 0.0 | 0.0 / 0.0 |
| 70 Hz | 0.0 / 0.0 | **100** / 0.0 | 0.0 / 0.0 |
| 85 Hz | **85.4 / 89.6** | **97.9 / 95.8** | 0.0 / 0.0 |
| 100 Hz | **87.5 / 89.6** | **93.8 / 93.8** | 0.0 / 0.0 |
| 140 Hz | 87.5 / 87.5 | 87.5 / **93.8** | **100 / 95.8** |

The window is between 70 and 85 Hz with equal tones (41..50 % of the shift) and closes at 28 Hz
(16 %) once the mark channel is out of the picture, and it ends in a cliff, not a slope. The weak
mark column is not symmetric either: the text holds at +70 Hz and is gone at -70 Hz. The bench
records that asymmetry; it does not yet explain it.

Noise does not move that window. The same walk at 10 dB, at 7 dB and on top of 0.5 Hz / 6 dB QSB
decodes at 0.0 % for every offset up to 70 Hz, so the tolerance is set by the filters and not by
the SNR; `rtty-tune` prints that table and fails if the 56 Hz point stops being clean.

In practice: 1 ppm on 20 m (14 Hz) is comfortable; 5 ppm on 20 m (70 Hz) is at the edge; 2.5 ppm
on 10 m (70 Hz) is at the edge and 3 ppm is gone; on 6 m 1 ppm is 50 Hz, which still decodes with
equal tones while the weak mark row is already at 10 % on the low side, and **2 ppm on 6 m
(100 Hz) is past the end of the window entirely**. A calibrated radio contributes about +-10 Hz at
14 MHz, so the budget is mostly the other station's error - but two radios each 0.7 ppm off put
70 Hz on the pair at 6 m, which is the whole window. Tuning RTTY on 6 m therefore needs both ends
within about 1.4 ppm of the filters, which is the same order as the references themselves.

Our own side, measured rather than assumed: **warm**, with the software TCXO on and the radio
nulled against WWV, the residual is about **+-10 Hz at 14 MHz** (+-0.7 ppm), so roughly 20 Hz
on 10 m and 36 Hz on 6 m. What it takes to get there is the part that has to be maintained,
not just set: the Si570's uncompensated swing is -203..+24 Hz at 14 MHz (16 ppm) over 0..99
C, `soft_tcxo.c` removes it with a per-unit table (on by default, and its own header says
the curve varies from unit to unit), and `ts.freq_cal` nulls what is left in 0.1 ppm steps
(range +-149.9 ppm). One number cannot follow drift, so the calibration has to be redone.

Warm-up is part of that, not a detail. The table is referenced to 43 C and its header only claims
the curve for an Si570 and an MCP9801 that are thermally bonded, so before the two settle the
correction is chasing a temperature the sensor has not seen yet. Near room temperature the curve
runs about 5 Hz per C at 14 MHz (-99 Hz at 20 C against -75 Hz at 25 C), so every 6 C of sensor lag
is a third of the 170 Hz window, and a cold radio is the opposite of a calibrated one. The +-10 Hz
above is the warm figure; nothing here measures the first minutes, and the bench has no thermal
model to do it.

Si5351A boards get no temperature correction at all: `Si5351a_PrepareNextFrequency()`
takes the temperature factor and discards it, so there the +-10 Hz holds only
at the temperature it was calibrated at. The audio and symbol clocks do not
enter into it - a ppm of the 12 kHz side clock moves a 915 Hz tone by 0.001 Hz
and a 22 ms bit by 22 ns - the whole error is the RF reference times the band.

## Weather fax, `make -C support/digi wefax`

Synthetic 120 lines/min broadcast at 12 kHz: 5 ms sync tip at 1857 Hz, picture
tone 1500 Hz (black) to 2300 Hz (white), 300 columns per line, deterministic test
chart (gradient, bars, two blobs, frame). The reference demodulator finds the
tip, then gives every pixel its own exact two-bin DFT over the pixel slot, so no
filter bank has to be tuned; per sample that is a handful of multiply accumulates.

| case | lines | chart correlation |
|---|---|---|
| clean / 20 dB / 12 dB | 12/12 | 0.975 / 0.974 / 0.969 |
| 12 dB + 2 Hz QSB | 12/12 | 0.969 |
| 12 dB with per-line skew, known gap | **10/12** | 0.012 mean (-0.112..0.144) |
| 8 dB with QSB and per-line skew, known gap | **11/12** | 0.499 mean (0.441..0.535) |
| black and white swapped | 12/12 | **-0.945** (-0.946..-0.945) |
| 1 ppm on 20 m (14 Hz off), 5 ppm on 20 m (70 Hz) | 12/12 | 0.975 / 0.976 |
| 2 ppm on 10 m (56 Hz), 1 ppm on 6 m (50 Hz) | 12/12 | 0.976 |

Every noise row is the mean of five noise seeds, with the seed range in the table: the line count is
the worst seed's, because a line the capture misses is lost whole, and the correlation is the mean.

The rows with no channel in them are one draw and show a dash, and the top noise rows' ranges are a
thousandth wide because the noise there changes nothing the score can see. The two skew rows are
where five seeds matter: the correlation there is a catastrophe that either happens to the capture
or does not, and one draw would have called it either permanent or absent.

The negative correlation is the useful number: the picture is recovered perfectly with the polarity
upside down, which is why a station parameter (or an automatic decision) is required before this is
usable on air.

A frequency error is applied to the fax generator the same way it is to the RTTY pair: every
tone of the emission moves by ppm x band while the demodulator's bins stay at 1857/1500/2300 Hz.
Unlike RTTY, the correlation barely reacts - Pearson removes the constant grey bias the offset
causes, so a band that is 5 ppm off still correlates at 0.976. The mean column is where it shows:
199.5 clean against 207.6 mean at 70 Hz, about 3 % of the 800 Hz tone spacing. Correlation
alone would therefore pass those rows at every offset, and the wrong reason is worse than no
assertion: the four ppm rows pin the decoded grey mean as well (200.5..203.5 at 14 Hz, 205..209
at 56 Hz, 204.5..208 at 50 Hz, 206..210 at 70 Hz), so an offset that stopped moving the tone,
or that moved it much further, fails here. At 250 Hz off (5 ppm on 6 m) two of twelve sync tips
are no longer found and the picture is gone at mean 183.4, correlation 0.339, which is not a
row because it would have to pass before it could be pinned. The fax tones are 800 Hz apart and
the sync window is 5 ms, so it weathers a few ppm on HF and shares the 6 m limit with RTTY.

### Known gaps, all of them measured

* **The sync capture re-acquires each line and has no model of the line rate.** Under a transmitter
  that skews every line by 2 % it finds 9 of 12 lines on the worst seed and correlates at 0.363
  (seeds 0.306..0.416), and the spacing metric reports 2983 samples - not because the tips move that
  far, but because one miss shifts every later search window and the skew it then has to cover is
  the accumulated error, not the line's own.  At the milder 0.3 % skew it finds all twelve lines and
  correlates at 0.794 (0.724..0.837), which is why that row is no longer called a gap.  What closes
  this is a line-rate integrator - what a real facsimile decoder runs - not a wider window.

  Four things had to be fixed before that remaining gap could be stated honestly, and three of them
  were errors in this harness rather than in the decoder:

  **Rows were numbered by how many lines had been found.** One missed line slid everything after it
  up a row, and the score compared `min(lines_found, rows)` contiguous rows, so a capture that lost
  one line out of twelve was reported as a chart correlating at 0.012 when eleven twelfths of it were
  in fact correct.  Lines are now written at the row their own tip falls on, counted from the first
  tip acquired, and a canvas row the capture misses is a hole the caller zeroes and the score counts.
  That is a lower number in the skewed cases and a truer one everywhere.

  **The window searched for the next tip started after the previous tip.** It began at the end of the
  previous picture, so a line that arrived even slightly early had its sync behind the cursor and could
  not be found however the threshold was tuned.  It now opens at the picture's end - which is the
  blanking gap, 540 samples of known white reference, exactly the room a tip needs to arrive early.

  **The tip was placed at the maximum of a single-bin power ratio, and that maximum is not the tip.**
  The sync is 1857 Hz, which is mid-grey between the picture's 1500 and 2300, so a stretch of steady
  middle grey scores a ratio of 1.000 and beats the real tip.  Printed from the generated broadcast,
  60-sample windows, offset from the true tip position:

  | offset | -12 | -6 | **0** | +6 | +12 | +18 | **+24** | +30 |
  |---|---|---|---|---|---|---|---|---|
  | ratio | 0.867 | 0.931 | 0.974 | 0.954 | 0.950 | 0.959 | **0.994** | 0.987 |

  So "take the best candidate" parked the start 24 samples late, which put every 18-sample column
  window across the boundaries: a column whose true grey was 250 decoded to 110, uniformly, on every
  row.  The capture now takes the *earliest* candidate within 0.03 of the profile's own peak, which is
  the leading edge of the plateau and moves with the signal.  A fixed 24-sample correction was tried
  first and is wrong: it worked at zero tuning error and put the 1 ppm row on its head, whereas the
  relative rule holds across all the tune rows.

  **The generator's picture was not a fax signal.** Each column was synthesized as its own burst with
  a 4-sample fade at both ends, so every pixel boundary was a click, and the measurement was dominated
  by that: a 14 Hz reference error - 1 ppm at 20 m, far under real oscillator tolerance - scored 0.549
  while a 70 Hz error scored 0.976.  Nothing about a decoder or a channel has that shape.  Columns are
  now one phase-continuous oscillator that steps frequency once per pixel, which is what the format
  is, and the tune rows came together at 0.937..0.951 instead of swinging between 0.55 and 0.98.  With
  that gone the ceiling on this table is about 0.95 for an honest reason: grey is estimated from a
  two-bin ratio at 1500 and 2300 Hz, which is coarse, where a real receiver uses a discriminator and an
  integrator.

* **Left/right steering is invisible to this metric by construction**: the ground truth follows the
  transmitted scan order, and the reference decoder has no phasing reference. A real receiver needs
  the steer control, not a smarter score.
* Real station parameters (576 lines/min family, pilot levels, actual phasing signals, the
  frequency plans used by specific shore stations) are not modelled yet.
* **No recorded on-air sample of fax itself.** The path from a receiver to a graded picture is now
  proven end to end against live traffic - `capture/websdr_capture.py` at a 300-4300 Hz passband,
  through `decode_oracle.js`, into `wefax-wav` - and the acquisition metric rejects the real takes that
  were not fax (see above). What has not happened yet is a take that *is* fax, because the frequencies
  tried were not transmitting it at that hour: 12262.5 kHz was busy with something else, and 7880 and
  16943.5 kHz are not in NA5B's band table (it covers the amateur bands and MW-160, and the capture
  tool refuses a frequency outside a band rather than returning another band's audio). Everything in
  this file's WEFAX section is still a synthetic channel until one real chart comes back.
  Two ways to close it: catch the NOAA circuit on 12262.5 kHz at a time it is actually scheduled, which
  a receiver in Texas can reach, or record the radio's own USB audio output during a transmission.

## Future source of real test samples: public WebSDR receivers

For scoring decoders against live propagation instead of only synthetic channels, two shared
public receivers cover MF and shortwave and can be driven programmatically:

```
http://na5b.com:8901/                    NA5B, Texas       (used for the capture below; US NAVTEX is 512 kHz, not 518!)
http://websdr.ewi.utwente.nl:8901/       Twente, NL        (in range of European 518 kHz stations)
```

Both are HTML5 clients (`websdr-base.js` + `websdr-sound.js`) whose audio arrives over a WebSocket
on the same port, so the interface is readable straight out of the client script:

```
socket   ws://<host>:<port>/~~stream?v=11        binary frames, server -> client
tuning   text frame:  GET /~~param?f=<kHz>&band=<i>&lo=<kHz>&hi=<kHz>&mode=<n>&name=<who>
```

`f`, `lo` and `hi` are in kHz (the page's client keeps its frequency in kHz and sends the fractions
unrounded). `band` is the index of the front-end band that covers the dial, and it is read off the
receiver's own band table, served as `/tmp/bandinfo.js` (and tried as `/bandinfo.js`): NA5B's eight
bands run 0-2048, 3200-5248, 5324-7372,
8825-10873, 11599-13647, 13655-15703, 20888-21912 and 26961-29009 kHz, so 24 MHz is in a gap
between two of them and cannot be reached at all. `lo`/`hi` are the audio passband edges
(1400..2000 Hz is already the right shape for SITOR/NAVTEX or weather-fax tones). `name` is shown to
the receiver's operator, so fill something honest in. These are shared receivers with a limited
number of user slots: short sessions only.

### The collector, `capture/websdr_capture.py`

It does the transport half and nothing else: connect, tune, write the raw stream to
`part-NNN.stream` with one big-endian 32-bit length before each WebSocket message, resume at the
next index, and reconnect with backoff after a drop. A part is `--part-seconds` of wall clock, since
with no decode there is no sample count to reach. A fragment left by an attempt that was cut short
keeps its own index and is recorded `"complete": false`, so the next attempt cannot truncate it:

```
./websdr_capture.py --freq-khz 4581.41 --part-seconds 5 --parts 2 --out /tmp/proof
  part-000.stream     5.1 s      36780 bytes     41 messages  2026-11-20T05:37:48Z
  part-001.stream     5.0 s      36783 bytes     41 messages  2026-11-20T05:37:53Z
./websdr_capture.py --freq-khz 4581.41 --part-seconds 4 --parts 1 --out /tmp/proof   # resumes
  part-002.stream     4.0 s      29529 bytes     33 messages  2026-11-20T05:37:59Z
```

The first two files were still byte identical (md5 unchanged) after that second run, and every file
consumed exactly under the framing - 41, 41, 33, 46 and 25 messages of 518 to 909 bytes, no leftover
bytes. The collector grades nothing, and `--assemble` is rejected as an unknown argument: a part is
what the receiver sent and no more. Turning one into audio is `capture/decode_oracle.js` below, and
then `digi_bench capture-check` on the WAV it writes.

The collector reads the receiver's band table before it captures and sends the covering band with
the tune, because on a multi-band receiver the band is half of what "this frequency" means. Left
out, the server applies the passband and the mode to the band the connection opened in and leaves
the front end there, so two dials in different bands come back as the same audio: on 20 November the
12:00 UTC takes at 24000 and 10100.8 kHz both returned NA5B's default band - one hum, the same two
frequencies found in it, 169 messages each - and `decode_oracle.js` decoded it perfectly,
because it was a perfectly decodable stream of nothing. Sending `band=3` for the same 10.1 MHz dial
gave a live S-meter and DDK9's pair; `band=0` for it gave the hum back. A dial no band of the
receiver covers is now refused before a byte is captured, with the receiver's own band spans in the
message, rather than recorded as a take of whatever band it was already on. The refusal lists the
receiver's own band spans one per line and ends by asking for a frequency inside one, or `--band`
to name one.

The table is read whether or not `--band` names a band, because the name and the span an index
covers are the only thing that index can be checked against. Naming one is still the way to reach a
receiver whose table cannot be read, so `--band` overrides the dial rather than being refused by it:
an index past the end of a table that *was* read is refused, since it names no band and the server's
answer would be a fallback nobody could name, and a named band that does not cover the dial is
warned about on stderr and then used as asked. With no table at all the collector says so, tunes
without `band=`, and records `"band": null` in `parts.json` so the takes can be told apart later;
the log line says `band none`, and a band with no table behind it says `unnamed` rather than being
shown with a blank where its name should be.

What `parts.json` keeps for a part is the ask as well as the arrival: the host, the dial in kHz, the
mode, the passband, the exact tune request that went out, the band index with the span it names, the
`sha256` of the table that index came from, and the deadline it was given, with the reason there is
no table in place of the last three when there is none. Before this the file held bytes, messages,
a band index and nothing about what had been requested, which is how five parts of one channel could
sit in one directory and be five parts of five stations as far as anybody reading them could tell:
the tune string alone turns that claim into one field to diff, and the digest says whether two parts
were even tuned against the same bands.

Both halves of this are graded with no receiver on the end of it:

```
./capture/websdr_capture.py --self-test
```

It reads `fixtures/bandinfo-8band.js` - the table NA5B's front end serves, with only its scale-image
lists trimmed and its keys, order and number formatting left as received - and grades 53 cases: the
tune request built from it, the eight spans, the band under each of eight dials and the two gaps,
the dial in a gap being refused, `--band` outside the table being refused, a named band that does
not cover the dial being warned about, the log line naming the band a part was tuned with, what a
part records about the ask, and the fetch that reads the table: asked for with
`Accept-Encoding: identity`, and refused outright if the body arrives encoded, arrives
transfer-encoded, claims more than the 8 MB ceiling, or stops short of its own `Content-Length`. A
chunked body is refused rather than unwrapped because the sizes would reach the table parser glued
between the entries, which is the one shape the whole-or-nothing guard below cannot promise to see.
The table is also taken whole or not at all: an entry that carries a centre without a width, or a
`var nbands` count the entries do not fill, gives no table rather than a table whose indices have
all moved one place up - because `band=` is a *position* in this file, and a shifted index is the
same fault as a missing band, one dial quieter and one that no log line would show. Why there is no
table comes back with the answer and goes into the single line the collector prints before tuning
blind. `make -C support/digi oracle-check` runs this and the decoder's self test.

### The decode oracle, `capture/decode_oracle.js`

It turns a `part-NNN.stream` into a WAV with the receiver's own client code - the script the WebSDR
page runs - rather than with a port, because the port is the part nobody trusted. Before the decoder
is even loaded the whole file has to satisfy five rules: every message is a sequence of known
elements and ends exactly on its last byte, the rate comes from the stream's rate element and never
changes, no run of silence markers lasts more than 2 s (the client's recorder stops appending there
and truncates a take without saying so), no audio arrives in a message before the rate element, and
the take has audio in it at all - a block of 128 copies of one code is the fill an empty band gets
and a silence marker is 128 zeros, each one level held for 128 samples, so a take made only of them
changes nothing and decodes to levels, not to a noise floor.  A take with no recorded sample after
its rate element is the same fault with nothing to decode at all.
After decoding, the recorder has to have produced exactly the samples the framing walk counted. The
WAV is written last, so a refusal leaves no file:

```
node capture/decode_oracle.js part-000.stream take.wav --client ~/websdr-sound.js
  oracle: 353 messages, 7119 Hz, 315392 samples, 44.30 s, rms 0.0002
          blocks: 2464 8-bit, 7 packed, 0 silence
```

That take is a noise floor, and that is the reading that matters: the oracle
says the bytes are a valid stream and that the receiver's own decoder
read them exactly as sent. It does not say the audio holds a station -
`capture-check` answers that question, and only then is a take worth pinning.

`websdr-sound.js` is deliberately not in this tree. It is Copyright 2013-2025 pa3fwm@websdr.org, all
rights reserved, and the permission it carries covers serving it unmodified from his servers to his
clients, not redistributing it, so the copy stays local and is ignored by git. Save the script your
receiver's page loads, unmodified, and pass `--client`.

The self test is seventeen rows - thirteen streams and four recorder's answers - and it takes all of
them: a stream built in the file element by
element - 7119 Hz, one 8-bit block, one packed block from a `144..223` header, one from a sub-`0x80`
byte, one silence marker, 512 samples in 3 messages - has to pass the framing walk with exactly
those counts *and* still be recordable to the rules in front of the decoder, a stream of two blocks
whose codes step between `0xd5` and `0xd6` has to be recordable too, and ten streams, each built to
break one rule and be right everywhere else, have to be refused with a nonzero exit, no WAV and
the whole complaint they were built to draw: 169 frames of the size a real take arrives in, filled
from a fixed sequence so the refusal is the same byte for the same reason on every run (it stops on
an `0x8c` message type, one the client would have skipped silently), 112 silence markers (2.01 s,
the first whole count over the 2 s rule at 7119 Hz), that same rate repeated as 8000 in a later
message, three audio blocks in a message before any rate element, an 8-bit block one byte short of
the 129 it needs, audio with no rate element at all, a take whose blocks are two copies of one code
(what a dead band arrives as), a take that records nothing after its rate element, a take of that
fill and a silence marker, and three bytes after a stream that otherwise ends cleanly. The built
streams alone would pass a walk that accepted anything; the refusals alone would pass a walk - or a
rule in front of the decoder - that rejected every stream, which is why the packed blocks are built
to the format's own arithmetic (128 samples of width 4, one run bit and four mantissa bits each,
640 bits in all) so the walk has to land on the byte the builder wrote, not merely accept it, and
why the two recordable streams are graded by calling those rules directly. None of the thirteen
streams needs a client or a network, because all are decided before the decoder is loaded - which is
why they are what CI runs. The four recorder's answers are there because the two checks that sit
*after* the decoder loads were the only rules in this file with no test behind them: the rate the
recorder says it took against the rate the stream declares, and the sample count it says it filled
against the count the walk counted. They are graded by running the decode - `decodeWith()`, the half
of `decodeStream()` after the load - against a client written by the self test that keeps nothing but
the contract: it opens the socket, answers `rec_finish()` with a rate and a sample count the row
chooses, and hands back silence. One row answers correctly and has to produce a WAV of exactly the
samples it agreed to, at the stream's own rate; the other three record at the client's 8000 Hz
default, come back 128 samples short, and invent one, and each has to be refused with no WAV left
behind. That fake grades no decoding - a recorder's two answers are the whole input those rules look
at - but it reaches the call in front of the WAV writer, which a row calling `decode_mismatch()`
directly would have left ungraded: dropping either check, or the call, or writing the file before the
check runs, now turns a row red instead of turning a live capture into a WAV holding samples the
stream never carried. These four need no receiver either, so CI covers them too. The missing-rate stream is refused by the decode rather than by the walk, since the
rate is what the decode needs before it can index the messages at all, and that case goes red if the
check is dropped instead of passing on the walk's.

The DDH 8 capture quoted below was taken from NA5B. The station list,
baud rates and shifts came from the DWD table on the Franks-Weather page,
http://weather.mailasail.com/franks-weather/radio-teletype-weather-broadcasts - 50 Bd
with 225 Hz either side of the carrier on 4583/7646/10100.8/11039/14467.3 kHz, and 50 Bd
with 42.5 Hz on 147.3 kHz. The receiver's own tuning readout is what turns those numbers
into the audio pair the modem sees; do not take the dial frequency for the tone pair.

What is proven here and what is not:

* the WebSocket upgrade works: a hand-rolled HTTP/1.1 upgrade on `/~~stream?v=11` returned
  `101 Switching Protocols` and streamed ~97 kB of payload in eight seconds, including sample-rate,
  gain, **S-meter (`0xF0..0xFF`: `10*(256*(b&15)+next)` dBm)** and **true receive frequency
  (`0x85`)** announcements, the last two useful on their own for measurement work;
* the Python `websockets` library fails on that URL with "did not receive a valid HTTP response",
  so a library error is not evidence the server has no audio API;
* the sample payload itself is WebSDR's lossy DPCM (bit-sliced deltas through a 20-tap lossy
  integrator; `0x80` = 128 table-coded absolute samples, `0x84` = 128 zeros, `0x90..0xDF` and
  `<0x80` = delta blocks with `Ut = 14 - (b>>4)`). A faithful port has to reproduce JS ToInt32
  semantics at every shift and mask (`p` is an `Int16Array`, `qt`/`Kt` are float arrays, and
  `S*Ot + Ot/2` stays float until `>> 4`). **That port is unfinished and unvalidated**, so
  `capture/decode_oracle.js` does not attempt it: it loads this exact client, unmodified, and calls
  its `rec_start()` / `rec_finish()`. What this repository checks is the framing walk in front of
  the decoder and the decoder's own sample accounting, never a reimplementation of that loop;
* **the capture recipe this section used to give is wrong for the build that serves NA5B today**.
  `soundapplet.rec_start()` / `rec_finish()` **do exist** in this client, and they are the
  route that works: `rec_start()` allocates a 64 kB chunk and appends one more per audio
  block the socket delivers (about 4.1 s of audio at 8 kHz), and `rec_finish()` returns
  `{wavdata: [ArrayBuffer, ...], len, sr}` at the stream rate (`soundapplet.g`, 8000 Hz
  here). It needs no user gesture, no DPCM port and no server cooperation, and it fills from
  the WebSocket, so it works whether or not the page is rendering audio. What does **not**
  work here is the `soundapplet.p` ScriptProcessor tap: this client builds its node with
  `createScriptProcessor(4096, 0, 1)` - zero input channels - so `onaudioprocess` hands out a
  **zero length** `inputBuffer` (and a zero length `outputBuffer` in a tab that is not playing),
  and a tap on it records nothing at all. The "peak 8 of 32767" capture came from that tap,
  not from the recorder; the recorder was never the problem. Three things do bite in practice.
  A bare `setfreq()` only moves the displayed frequency, so use `setfreqif_fut('4581.84')` or
  the receiver stays where it was and the take is hum and rumble rather than FSK. The passband
  matters as much as the dial: `setmf('usb', 0.85, 1.45)` around the pair drops out-of-band
  noise by five orders of magnitude in the recording, where the default 50-2660 Hz passband
  leaves the modem fighting it. And getting the bytes out of the browser is the remaining
  wrinkle: a `fetch` POST from the page to loopback is refused by the private network policy,
  but a top-level navigation is not, so the page can hand a base64 blob to a local collector
  as `?d=...` and lose nothing but its own tab. Keep any in-page `await` under about 10 s; the
  recorder keeps growing between calls, so a longer take is a longer wait, not a longer call.

## First live decode through this harness

DDH 8, 14467.3 kHz, F1B, 50 Bd, +/-225 Hz (captured from the NA5B WebSDR at 11:41 local time on the
recording workstation - that clock reading was never logged against UTC, so it is not a UTC time -
30 s at 12 kHz; schedule and parameters from the Franks-Weather DWD table cited above):

```
decoded: MSOHWWM PPUWWNW  6  8  3  MSO  12 WNW  6  8  4  MMO 23 Z A  34  3  MO 23 12Z SSW  5  2
```

Tuned so the received pair (14467.075 / 14467.525 kHz) lands on the modem's own 915 / 1365 Hz
filters. The same audio decoded at 45.45 Bd instead of 50 gives `6 78 MOTLYMV20UWNW`, so the baud
setting is what carries it, and the mark/space polarity in `RttyDecoder_demodulator()` is correct -
an inversion there would have produced letters that are not in the DWD message set at all.

## The on air case, `make rtty-ddk2`

The only case in this bench that is not synthetic. `fixtures/ddk2-4583.wav` is 13.7 s of a real
**DDK2** transmission (4583 kHz, 00:00-24:00 UTC, F1B, 50 Bd, +/-225 Hz), recorded through the
NA5B WebSDR client's own recorder on 19 November 2026 at about 20:45 local time on the recording
workstation - that clock reading was never logged against UTC, so it is not a UTC time - on a dial
of 4581.84 kHz in USB with an 850-1450 Hz passband. The client's recorder ran for **64.1 s**,
and only the **last 38 s** of it were transferred out of the browser, since the hand-off is one
top-level navigation with one blob in it; every offset below is an offset into that 38 s window,
not into the whole take. That 38 s was decimated 2:1 to 4 kHz from the client's 8 kHz stream rate
(the pair sits below 1.4 kHz, more than 600 Hz clear of the 2 kHz Nyquist limit) and trimmed to the
one clean repetition, 19.1-32.8 s of the transferred 38 s. `fixtures/MANIFEST.md` carries the same
provenance as fields, so a re-capture is checked against numbers rather than against this prose.

The pinned text is the CQ line these stations send; the DDH 8 copy in the
section above shows it with that transmitter's own call sign and frequency list:

```
expected: CQ CQ CQ DE DDK2 DDH7 DDK9 FREQUENCIES   4583 KHZ   7646 KHZ   10100.8 KHZ
decoded : CQ CQ CQ DE DDK2 DDH7 DDK59FREQUENCIES   4583 KHZ   7646 KHZ   101008 KHZ
CER     : 2.7 %
```

`101008` is not a decode error: `norm_baudot()` keeps only A-Z, 0-9 and space, so the decimal
point never reaches the report. What the take carries at the `DDK9` junction is **one extra
character**, not two misreads: the line reads `DDK99` and then breaks, where the pinned text
carries one `9` and a space. The decoder prints `DDK59` - it reads the extra slot as code 16
(`'5'` in FIGS), one data bit away from the code 24 (`'9'` in FIGS) the audio carries - then a
**line break**, then `FREQUENCIES`. The `9` before the extra character and every character the
pinned text carries are read correctly. `fixture-align` names the same insertion from the other
side: one inserted, no substitution, the pinned space absent because the take breaks the line
there - and both windows carry the take's own keying at level, not a faded copy of it: the nine's
mark and space read -53.6 and -32.8 dB, the break -52.0 and -31.1 dB, against the take's own
mean of -46.5 / -38.4 dB, and the worse tone of those two windows is 7.1 and 5.5 dB under it.

The junction was read back from the audio without the decoder's decisions: 12 ms Goertzel windows
at the two keying tones (929 Hz mark, 1357 Hz space), one window per data bit, aligned on each
character. The first slot reads code 8 (CR) at its own alignment: its four space bits are 15-18 dB
down on the mark tone, its data bit 3 is a 10 dB mark there (at a fixed half-bit centre it sits at
0 dB), and no alignment of the slot reads it as a space. The second reads code 2 (LF): its mark
bit is 15 dB clear and its space bits 11-29 dB down. So the break was transmitted; it is not a
space misread as CR - that would need a mark at data bit 2, and the slot carries space there at
every alignment tested. The extra slot is real too: a full seven-and-a-half bit character whose
audio reads code 24 (`'9'`) at its best alignment, one bit from the code 16 the decoder printed.

The extra character is **not** a shift-handling defect. `rtty.c` has no unshift-on-space at
all - only codes 27 and 31 change `charSetMode`, and a space is `' '` in both tables. The
audio carries code 24 in FIGS, which is `RTTYSymbols[24]` = `'9'`, so the extra character is a
second nine; the `'5'` is `RTTYSymbols[16]`, one data bit away, and that is the bit the decoder
got wrong on a slot with no counterpart to compare it against. A synthesised stream of this
text decodes at **0.0 %**, so the alphabet and the decoder reproduce the line exactly and the
extra character is in the take. Three takes of the same transmission read `DDK9`, `DDK95` and
`DDK59` there, so the extra slot's identity came with the signal rather than with the logic.

The scorer charges the insertion and the pinned space the line break stands in for: 2 edits over the
**73** characters is 2.74 %, which is the 2.7 % this case pins, and the target fails it above 3 %.
No character the pinned text carries is read wrong; the one symbol the decoder misreads is the extra
nine, which has no counterpart in the text.

### The pair, measured

The average spectrum is the tempting measurement and the wrong one. Its humps are combs of 3.33 Hz
teeth from the idle `RYRY...` pattern, with keying sidebands at +-50 and +-100 Hz, so "the peak" is
whichever tooth is tallest and the answer comes out pulled inward: **441 +- 3 Hz** (932.0 / 1370.6).

Single tone runs only - 55 ms or longer, transitions trimmed, so neither the comb nor the
other tone's sidebands can bias the peak - give space **927.9 Hz** (n = 24, sd 0.44) and mark
**1375.4 Hz** (n = 15, sd 0.87), separation **447.5 +- 0.2 Hz**. Three estimators agree: per
run maximum likelihood median 447.54, a length extrapolation that removes the fixed window bias
447.51, and the phase slope across runs 447.62. **447.5 Hz** is the number to use. On this
take that is 0.55 % below the nominal 450 Hz, and the WWV check below holds the audio axis to
0.03 %, eighteen times smaller, so the offset belongs to the emission and not to the reading.

The receiver then decides the shift setting, and it is not close: **450 Hz decodes at 2.7 % CER**,
425 Hz at 49.3 %, 170 Hz at 30.1 %, 850 Hz at 32.9 %. The measured 447.5 Hz sits 2.5 Hz from the
450 Hz setting and 22.5 Hz from the 425 Hz one, which is why that is the setting to receive it
with: the pair is a 450 Hz class emission that this take measures 0.55 % low, not a measurement
that cannot tell the two apart. 50 Bd is confirmed the same way: 20.0 ms bit period from the
transition spacing, and 2.7 % at 50 Bd against 35.6 % at 45.45 Bd. Both tones sit about 11 Hz
above the modem's 915/1365 Hz filters, which moves the pair without touching the separation.

### Is the stream rate what it says it is?

A 441 Hz reading invites the obvious explanation that the client's "8 kHz" stream is not 8 kHz. It
is, checked against a known signal: WWV on 10 MHz, captured the same way through the same receiver,
with the dial at 9999.0 kHz in USB so the carrier belongs at 1000 Hz of audio and its 100 Hz
subaudible tone and 500/600 Hz tone at 1100/1500/1600 Hz:

```
carrier measured       980.82 Hz    (-19.2 Hz: the dial is off, not the rate)
100 Hz subaudible      spacing   99.99 Hz    -> scale 0.9999
500 Hz tone            spacing  500.04 Hz    -> scale 1.00008
600 Hz (500+100)       spacing  599.75 Hz    -> scale 0.99958
```

Scale **1.0000 +- 0.0003**, so the 8 kHz stream is right to about +-2.5 Hz, and across a 450 Hz
separation that is 0.15 Hz. A wrong nominal rate cannot explain a 6 Hz gap; the gap was the
estimator. The same capture pins the receiver's audio axis at -19.2 Hz from its dial on that band,
which is worth knowing before quoting an absolute audio frequency - do not carry that correction to
4583 kHz without measuring it there.

**What one 13.7 s take cannot show.** This is a single night path, one receiver, no fading and no
QRM, so it says nothing about weak or fading signals, and its 2.7 % is not a quality reading of any
kind: both characters it charges sit in the audio at full keying, within 8 dB of the take's own
mean level, so the number counts what the station sent - a second nine, which the decoder prints as
`'5'`, and a line break where the pinned text has a space - and not what the take lost. Which two of
those it charges depends on the rate the file is played at: resampled to 8 or 12 kHz the count is
still 2.7 %, but the inserted `5` goes away and a different edit replaces it - the `4` of `4583`
missing (see the fixture manifest). What it does pin is that the decoder handles the DWD shifting
correctly on real audio, and that any modem change which moves this output has to be explained. The
other direction needs a take where the channel itself took a character's keying down, and the `qsb
slow deep` row of the RTTY table is it: one 4 s cycle, 26 dB deep, at 6 dB SNR, so a dip holds still
over several characters. The alignment of one take of that row charges seven of its 48 characters
and puts five of them 12-20 dB under its own keying, while the text either side of each dip copies,
and the same take with the fade switched off charges none at 0.0 %. So the 2.7 % above is an
at-level reading because the fixture is one, not because the statistic cannot see a faded character.

### The second on-air case, DDK9 on 10100.8 kHz, is not pinned

DDK9 was attempted on 19 November 2026 and **no fixture was committed**, because
no take could be verified. NA5B, the receiver behind the DDK2 case, had nothing
there that evening: RMS 2300-2440 over two passbands, with no FSK pair anywhere
from 400 to 2500 Hz, which is a noise floor - 10 MHz was shut from Texas to Europe.

Twente (`websdr.ewi.utwente.nl`) did show a 450 Hz / **50.0 Bd** shape at **924.0 / 1371.0
Hz**, but nothing decodes through it: not this take, not DDK2, and not a synthesised 450 Hz /
50 Bd stream at Twente's 7119 Hz rate, which reads 4.1 %. That puts the fault in the capture
path rather than the decoder, so a 90 %-CER line from it must not be pinned as ground truth.
Reading that Twente take back through `capture/decode_oracle.js` says where in the path: its
`.stream` is a conformant 7119 Hz stream - 353 messages, 2464 8-bit blocks, 7 delta blocks, no
silence run, every message ending exactly on its last byte - and every one of those 2464 blocks
is 128 copies of the code `0xD5`, which the client's table maps to one level, 8 of 32768. That
is not a noise floor at -68 dBFS: the take carries no audio at all, and an rms of 8 counts is
the level itself. Nothing was lost in the framing or in the decode - the receiver sent nothing
on the channel it was left on - and a take like it is now refused by name rather than reported
as a signal. An rms on its own cannot tell a noise floor from a constant level; that is the
reading this rule takes away. Two takes that are both noise floors also score high against each
other on a spectral similarity check, so a 0.97 is neither confirmation nor refutation on its
own. The rest of the sweep is no better: DF0HTE (Friedrichshafen) and ES4O (Estonia) hold only
a weak carrier in noise there (5 ms windows, 60 % of them with both tones within 6 dB), and
Hack Green has no 10 MHz band; Twente's dial sits 1.025 kHz off its marker and ES4O's 105 Hz.

20 November's captures - taken at several widely spaced frequencies, 10100.8 kHz among them -
are the same hash at every one of them: a **0.974 spectral correlation** against the dead-band
take above, **75 to 85 %** of the gate's 5 ms windows holding **both tones at once** where
the gate allows 0.35, and a **100 % character error rate** against the DWD text on every
take. Those numbers are still what those takes measured, and those captures **must never be
pinned as a fixture**. What they are not is evidence about the band they were named after,
and the 21 November fix to `capture/websdr_capture.py` found why: the collector built `f=`
from `--freq-khz` times 1000, so a request for 10100.8 kHz went out as `f=10100800.000`, which
no receiver can tune - the server leaves the dial where it already was. Every take in an
attempt therefore came off one channel whatever the flag said, which is exactly what "content
that does not change with the dial" looked like: bytes that never left the collector's own
arithmetic, rather than anything the front end was hearing.

The dial is checked now rather than assumed. WWV's 10 MHz carrier, asked for at **9999.0 kHz** in
USB, sits at **1008.1 Hz** of audio (10,000.008 kHz) and, asked for at **9998.5 kHz**, at **1506.4
Hz** (10,000.006 kHz): the carrier stays on 10 MHz and only the audio offset follows the flag. The
old format still reproduces the fault, which is what makes it the explanation - `--freq-khz 5000000`
and `--freq-khz 10000000` both land on the same dead fallback channel, the constant fill the rule
above refuses, so an untunable frequency is visibly dead rather than quietly somewhere else.
Nothing in this tree is derived from those 20 November takes, and `capture/ddk9_capture.sh`
below is what a retry has to clear before a take from that band is graded at all.

The comparison this case lacks is a propagation problem, not a bench one:
retry on NA5B when 10 MHz is open to Europe, and prove any new receiver by
copying DDK2 through it to a few percent before trusting a 10100.8 kHz take.
A receiver's S-meter is not evidence; the tone pair plus a clean decode is.

That retry is `capture/ddk9_capture.sh`, and it will not grade a run until two things have
cleared. First a dead-band control on a channel the receiver hears nothing on (`--dead-khz`,
required): the oracle has to refuse that take *as an empty take* - a refusal for bad framing says
the stream was broken, not that this pipeline can tell nothing from a station - or it has to decode
and the gate have to reject it. A gate that printed neither ACCEPT nor REJECT, and an oracle that
crashed or never found its client, are not refusals either and are reported separately, because a
run that reads them as one is a run that passed its control without measuring anything. A pipeline
that accepts the control stops the run there, and nothing later in it is read. Then the pinned DDK2
take, which has to come back at its 2.7 % to
within `--ddk2-tol` (0.3 by default) - DDK9 is the same transmitter family through the same
receiver, so a modem that has stopped copying the one take that is pinned condemns the 10.1
MHz attempt before a capture slot is spent on it. Only then is 10100.8 kHz captured, and the two
stream files are compared outright first: if the dead channel and the 10.1 MHz dial came back as the
same bytes, at most one of them reached the receiver and the run stops there. That is the 20 November
fault named directly instead of inferred - five parts, one channel - and nothing recorded at the time
could show it, which is why `parts.json` now keeps the request each part was asked with beside the
bytes that came back. A CER is printed only when the gate accepts the take and `--expect` gives the text to grade
against; a take the gate or the oracle refuses is reported as a refusal, never as a number.

## Reading a take: `fixture-align`, `rtty-trace` and the other two verbs

Four bench verbs answer questions that a character error rate cannot.
`rtty-ddk2` says the take scores 2.7 %; these say what that number is made
of, and one of them measures the transmitter instead of the receiver.

### `fixture-align` (the `align-check` verb)

```
make -C support/digi fixture-align
./digi_bench align-check <file.wav> <shift> "expected text" [45|50]
```

One row per character the expected text carries: what was sent, what the modem read, the mark and
space level of that character's own 7.5 bit window, and the transitions inside it;
the junction table beside it is what compares its two windows with the take's own mean level. A
character the take does not carry is printed `-` in the `got` column, and the edit script below the
table separates **inserted**, **not in the take** and **substituted** - one character arriving
beside the text and two characters being read wrong have the same edit distance but are not the
same fault, and the old table could not tell them apart.

`rtty-align` is that table for any take: the same rows and the same edit script, with the DDK2
junction left out, so a new recording can be read character by character before anything about it is
pinned. `align-check` is `rtty-align` plus the junction pinned by name.

The DDK2 junction is then pinned by name rather than by count: one inserted `5`, the `9` read as
sent, the pinned space replaced by the station's line break, and the keying under both windows. It
fails if the insertion becomes a misread, if the break moves, if the `9` stops decoding as sent, or
if a window loses its tones - a fade is the one explanation that would make the record wrong rather
than the decoder.

### `rtty-trace`

```
make -C support/digi rtty-trace
./digi_bench rtty-trace <file.wav> <shift> "expected text" [45|50]
```

The demodulator's own decisions, recorded inside its loop: for every character slot, the time, the
code sent and the code read, the five data bits with `*` on each one that differs, the stop bit, the
spacing between decisions, and the largest deviation from one bit. Shift codes get no row (they are
slots, not characters), and CR/LF are shown as **line breaks** and not compared with the text: a
station that breaks a line where the pinned text has a space has read nothing wrong. The summary
counts shift codes, breaks, slots abandoned at a stop bit, inserted characters and differing bits,
and says whether the DPLL ever relocked inside a character - which is the difference between a
slicer error and a clock slip, and no letter can show it.

On the DDK2 take it reads: one inserted code 16, four line breaks, one abandoned slot, **zero**
compared characters with a differing data bit, every decision one bit apart.

### `rtty-multipath`

```
make -C support/digi rtty-multipath
```

The same 45.45 Bd transmission with a second path: ten delays from 0 to 20 ms and three echo levels
at -3, -6 and -10 dB below the direct signal, thirty cells of character error rate. It is the only
channel here that puts the same bit in two places in the audio.

With a live noise generator **no cell of that sweep costs the text**: the only one
above zero is 7 ms at -3 dB (4.2 %, two characters of forty eight), and 7 ms at -6 dB
reads 0.0 %. The 100 % notch this table once recorded at 7 ms and -6 dB was not the
echo. Restoring the old noise generator, where every sample received the same Gaussian
value, reproduces 20.8 % at -3 dB and 100 % at -6 dB; with the noise removed entirely
both cells read 0.0 %. A constant offset is not a noise floor - it is a bias on the
receiver's input - and that cell's failure belongs to the bias, not to the second path.

The echo failure is real, just later. At 18 ms - four fifths of a bit - the delayed copy's tone is
still arriving while the receiver decides the next bit, and a -3 dB path costs **64.6 %** (31/48) at
20 dB SNR. The grid around it is sharp: 16 ms and 20 ms read 0.0 % at -3 dB, and 18 ms itself reads
0.0 % at -6 and -10 dB. That case runs outside the sweep and is pinned from below, so the channel
still exercises the failure it exists for; the sweep is pinned from above at 5 %, where a non-zero
cell is a notch somebody introduced rather than the echo this map records.

### `tx-check`

```
make -C support/digi tx-check
```

The transmitter's own 12 kHz output through the receiver's gate, for the two settings RTTY sends:
170 Hz / 45.45 Bd and 450 Hz / 50 Bd. It measures the keyed pair against the demodulator's own mark
filter and that filter plus the shift, and then the bit
period and its scatter, and then the property only a keyer can fail - every interval between tone
changes is a whole number of bits, with two bit stop runs and no 1.5 bit ones. A receiver loopback
cannot see a keyer that sends a half stop bit, because the firmware's setting is 1.5 and
`Rtty_BaudotAdd()` sends two; this gate can.

## NAVTEX / SITOR-B, `make -C support/digi navtex`

The first mode in this bench that is not the radio's. `navtex.c` is a reference modulator and
demodulator that exist only here, the way `wefax.c` does, and they are graded the same way: a
synthesised transmission, a channel, and the text that comes back. What that establishes is the mode
and its failure modes. What it does not establish is that a real station was copied - no NAVTEX
take has been made here, and the receiver fits its bit clock over a +-2 % window (118..122 samples
a bit), which is enough for the rate this bench generates and not for an arbitrary recording.

The mode is 100 Bd FSK with a 170 Hz shift, carrying the CCIR 476 seven bit alphabet. Two properties
do all the work:

* **every valid character has exactly four marks and three spaces.** That is detection for free - a
  single wrong bit leaves a word that is not in the alphabet - and it is also the synchroniser,
  since seven bits taken across a character boundary land on a constant ratio word by accident
  about a fifth of the time and seven bits taken inside one always do. The receiver searches
  the bit phase, the bit length and the group boundary together and keeps the alignment
  that maximises the share of words in the alphabet: 1.000 on a clean take, 0.967 with one
  block's direct copy erased, 0.492 once the tuning error walks the tones off their bins.
* **there is no return path, so the correction is time diversity.** Each character is sent twice, the
  direct (DX) copy and a repeat (RX) copy five character slots - 350 ms - later, in blocks of five.
  A copy that is not a constant ratio word has already been detected as broken and is dropped, and
  the other copy is taken; only when neither copy is a word does the receiver correct against the
  pair. That order matters and the self check is what found it: a copy erased to silence reads as
  `0x00`, which is four bits from every word, so scoring the pair together rejected characters
  whose repeat was sitting there intact. A character with no surviving copy is lost and counted.

The bit order is the one thing a loopback cannot catch on its own: CCIR 476 is written most
significant bit first and SITOR sends each character least significant bit first, but a reversed
4-of-7 word is still 4-of-7. The receiver therefore decides the order from the phasing signal it is
about to look for: that is SIA `0x0F` / SIB `0x33` in the table's order and `0x78` / `0x66` in the
reverse one, so an alternation of CR and RPT is the tell. The generator can now key either order,
phasing pair included, so the reversed take is audio the receiver did not frame in its own order and
the decode can only come from that detection - it does, 0.0 % with the `rev` column at 1. What a
wrong detection would cost is measured rather than assumed: a take whose phasing is in the table's
order and whose text is reversed is read as the table's order, and 84.8 % of the message is wrong.

Measured on a 46 character message at 12 kHz; each row's own SNR and channel are in its name. A
noise row is the mean of five noise seeds and the table prints the seed range beside it; every row
has to align on all five seeds or it fails.

| case | CER % | err/len | repaired | lost | words 4-of-7 |
|---|---|---|---|---|---|
| clean | 0.0 | 0/46 | 0 | 0 | 1.000 |
| 20 dB | 0.0 | 0/46 | 0 | 0 | 1.000 |
| 10 dB | 0.0 | 0/46 | 0 | 0 | 1.000 |
| 10 dB + 0.5 Hz / 6 dB QSB | 0.0 | 0/46 | 0 | 0 | 1.000 |
| burst over one block's direct copy (280 ms) | 0.0 | 0/46 | **4** | 0 | 0.967 |
| burst over both copies (700 ms) | **10.9** | 5/46 | 0 | 5 | 0.918 |
| tuning error +50 Hz | 0.0 | 0/46 | 0 | 0 | 1.000 |
| tuning error +100 Hz | **87.0** (82.6..95.7) | 40.0/46 | 49 | 3 | **0.492** |
| reversed bit order, 20 dB | 0.0 | 0/46 | 0 | 0 | 1.000 |
| phasing in the table order, text reversed | **84.8** | 39/46 | 0 | 0 | 1.000 |

The first four rows are what the alphabet and the correction buy. The burst row is the mode working:
four characters of a block lose their direct copy and all four come back from the repeat, with the
character error rate still at zero. Erasing both copies instead loses exactly that block - five
characters of forty six - and is pinned from below, so a receiver that started inventing text where
there was none would fail here rather than read as an improvement.

The tuning rows are the two-bin geometry, not a filter skirt: the demodulator decides each
bit by comparing the energy at 1615 and 1785 Hz, so a tone that has moved stays on its
own side of the midpoint until the error passes half the shift. +50 Hz is inside that and
decodes clean; +100 Hz moves the space tone closer to the mark bin than to its own, so the
words stop being words rather than inverting cleanly into other valid ones - the constant
ratio share falls to 0.492 and 87.0 % of the message is wrong on average over five seeds
(82.6..95.7; 39 of 46 on the first seed). There is no AFC here either, so this is the same
cliff the RTTY section describes, at the one place it can be computed: half the shift, 85 Hz.

The last two rows are the bit order. Reversal is not damage - every reversed 4-of-7 word
is another member of the alphabet - so the reversed take keeps its 1.000 share and decodes
exactly, which is the detection and nothing else: the ratio that finds the group boundary
is blind to the order. The wrong-order row is what happens when that detection is told the
wrong thing: the phasing says the table's order and the text is reversed, so every character
the receiver reports is the reversal of the one sent. Both copies are reversed the same way,
so the repeat has nothing to disagree about - `fix` is 0 and `lost` is 0 - and the 4-of-7
share is still 1.000, because every word really is a word. Time diversity cannot repair an
error both copies share, which is why the row is pinned from below: a receiver that hedged
by trying both orders would decode this, and would then also be inventing text out of noise.

## Hellschreiber, `make -C support/digi hell`

The mode with no alphabet, no framing and no error control. A Hell transmitter keys a single
tone on and off and the receiver prints what it hears as a raster: 122.5 baud, one pixel every
8.16 ms, a 5x7 glyph followed by a blank column. Everything a synchronous mode gets from
its framing, Hell gets from the operator's eye, which has two consequences for the bench.

The first is that there is no character error rate to report. A cell that comes out
exactly as drawn is a character a reader would have read; a cell with one wrong pixel
is a character a reader would have guessed at. The table therefore counts exact cells
and the share of pixels wrong over the whole picture, and it allows the paper position
to be chosen freely - a whole pixel of roll and up to two characters of horizontal
offset - because where the picture lands on the page is not a decoding result.

The second is that the receiver has to find the raster in the keying alone: the pixel phase
by the bimodality of the envelope (there is nothing but ink and paper, so the right sampling
phase makes the histogram two spikes), and then the column grouping and the character phase
together, by requiring one column in six to be blank. Those two are the same question asked
twice, and they have to be searched together: a grouping that straddles two columns shreds
the gaps, and a character phase that is one column out puts the gap in the middle of a glyph.

Measured on a 31 character line at 12 kHz, scored over the 30 or 31 cells the take contains. A noise
row is the mean of five noise seeds, with the seed range beside the pixel share, and the cell limit
is applied to the worst seed - the paper shift that moves the last glyph out of the count on one
seed is not damage.

| case | pixels wrong % | cells exact | gaps blank |
|---|---|---|---|
| clean | 0.00 | 31/31 | 1.00 |
| 20 dB | 0.00 | 31/31 | 1.00 |
| 10 dB | 0.00 | 31/31 | 1.00 |
| 10 dB + 0.5 Hz / 6 dB QSB | 0.00 | 31/31 | 1.00 |
| **0 dB** | 0.00 | 31/31 | 1.00 |
| keying tone +200 Hz | 0.00 | 31/31 | 1.00 |
| keying tone +700 Hz | **14.82** (14.7..14.9) | 4/30 | 0.50 |
| pixel clock +2 % | **36.84** (36.7..37.0) | 4/30 | 0.28 |

The noise rows are not a surprise once the geometry is written down: a pixel is 8.16 ms of a 1
kHz tone, so the receiver's envelope integrates about fifty cycles per decision and the noise is
averaged away before the threshold. 0 dB in the audio band is still a clean picture. The last two
rows are the real limits. The envelope reaches about +-60 Hz (a 98 sample sliding correlation),
so a 200 Hz tuning error has lost most of the amplitude but not the on/off contrast - the
threshold is set from the take's own percentiles, and the picture survives on the contrast that
is left. At 700 Hz there is no contrast left and the raster is half found and mostly wrong.

The clock row is the failure a raster mode has instead of a bit slip. The receiver's
pixel period is fixed at 97.96 samples; a transmitter 2 % fast is a sixth of a pixel out
after one glyph and two columns out by the end of the line, so the columns lean and the
gap columns stop being empty (0.28 of them blank). Both failing rows are pinned from
below in the table, and the self check additionally holds the 2 % case, so a receiver
that started following the clock slowly would fail rather than read as an improvement.

## Streaming Hellschreiber, `make -C support/digi hell-stream`

`drivers/digi/hell_rx.c` is the file a firmware build would compile: one sample in, one keyed pixel out,
the pixel clock searched rather than assumed. It is graded on the reference's own takes with the
reference's own picture score, so the column that matters is the difference between the two.

| case | streaming, wrong pixels | cells exact | reference | what it shows |
|---|---|---|---|---|
| clean (122.5 Bd exactly) | 4.9 % | 4 / 30 | **0.0 %** | the grid misses the truth, see below |
| clock +1 % | **3.5 %** | **30 / 30** | 36.0 % | searching the clock beats assuming it |
| clock +2 % | **11.2 %** | 20 / 30 | 37.0 % | the same, less completely |
| clock -2 % | 11.3 % | 4 / 30 | 39.7 % | |
| tone +450 Hz | 22.8 % | 7 / 28 | 14.7 % | off tune costs both, the reference more slowly |
| noise only | never locks (0.11) | - | - | the discriminator does not fire on nothing |

`sizeof(HellRx)` is 904 bytes with 65 candidate clocks spanning 94 to 102 samples in eighths of a sample -
against 28,296 bytes for the SITOR-B receiver's grid. Feld-Hell is cheap to hold and hard to lock.

### The three ways this receiver was wrong before it worked, all found by measurement

**A phase-slot grid measures nothing on this mode.** The first design kept, for each candidate period, one
accumulator per phase slot, and scored how much of the envelope's variance sat between the slots - the
same shape of argument that works for SITOR-B. The numbers were 0.001, 0.001, 0.002 across the whole grid
while the reference decoded the same take perfectly. The reason is the mode: a pixel is one constant level,
so every phase inside a pixel holds the same value and there is nothing to separate. What differs between
pixels is the *sequence*, not the phase. 7,764 bytes bought a statistic that could not rise; the replacement
is 904.

**Purity is real but too blunt to choose.** Scoring the share of pixel blocks that land clearly outside the
midpoint of the two level clusters fixed the clock choice enough to move clean from 37 % wrong pixels to
4.9 % and pick 98 rather than 99 samples. But at eighth-of-a-sample resolution the candidates from 98.25 to
99.125 all score 0.951: a plateau, and the winner is decided by noise. It also costs a lock at 0.88 while
white noise sits at 0.72 by arithmetic - a normal spread has that much outside a quarter of its own range -
so the margin between signal and noise in this metric is narrower than it looks.

**Straddling, measured over the whole block, is destroyed by the envelope filter.** The sharp test is the
early/late one: a block that spans a keying edge contains both levels, so counting such blocks measures
clock error directly. Implemented naively it produced a 28 % straddle rate on a clean take - indistinguishable
from noise, and the receiver never locked. The cause is the 40-sample boxcar that turns the keyed tone into a
level: its output ramps for about 20 samples either side of an edge, which is two fifths of a pixel, so every
block contains the ramp. Watching only the middle three fifths of each block fixed it, and that is the version
in the table: noise falls to 0.11 and the real takes stay at 0.90-1.00, which is a discriminator with room in
it rather than a threshold in the middle of a distribution.

### The defect that is still open, and why it is the same one as everywhere else in this tree

On the exact-nominal row the receiver reads 4.9 % of pixels wrong and 4 cells in 30 intact, while on
`clk +1 %` - where the transmitter's period happens to land on a grid point - it reads 3.5 % and 30 of 30.
The truth at 12 kHz is 97.959 samples; the grid offers 97.9375 and 98.0625 and picks something nearer 98.1.
A few hundredths of a sample per pixel accumulate into two whole pixels over a line, and in a raster mode a
clock error is a seam rather than a slip: the picture reads until the boundary walks across a glyph, and the
cells that straddle it are the ones lost.

The fixes are finer resolution (`HELL_PERIOD_STEP` 4 buys one thirty-second of a sample at 193 candidates,
2.9 KB, roughly 200 ops per sample) or tracking the clock after lock instead of choosing it. That is the same
wall SITOR-B hit from the other side, where a 1.5 % rate error is two integer steps and cannot be followed at
all. Both point at one missing component: a held fractional clock - a measured period carried as a fraction
and nudged every pixel, rather than a list of whole-sample candidates picked from once. It is the next piece
of DSP work in this directory, and it is worth doing once, for both modes.

**This receiver is not in the firmware.** It compiles, it is in the bench, it is in CI, and `files.mak` does
not list it: a mode earns its place by decoding traffic that was not generated by the same assumptions, and
Hell has no on-air capture behind it yet - the NAVTEX receiver has `ddk2-4583.wav`, this one has nothing but
the table above.


## MFSK16, `make -C support/digi mfsk16`

Sixteen tones 15.625 Hz apart, one tone per symbol at 15.625 baud: four bits a symbol,
64 ms of signal per decision, 234 Hz wide. A symbol that the receiver reads correctly
is worth four bits and a symbol lost to the channel is worth nothing, which is why
the mode is built around a code and an interleaver rather than around the tones.

Which parts of this reference modem are the mode's and which are the bench's matters, and the table
only means something if that is said plainly:

* **the mode's**: the tone grid, the rate, four bits per symbol, the IZ8BLY Varicode alphabet with
  its `00` separators, the rate 1/2 constraint length 7 convolutional code, and the fact that coded
  bits are spread over a block of symbols.
* **the bench's**: the interleaver is eight symbols rather than the mode's ten concatenated 4x4
  diagonal interleavers, and the take ends with the idle NUL and one start bit. The NUL is not
  decoration: a Varicode code may end in zeros (every digit does), so zero padding after the text
  cannot say where the last code ended - `5` followed by padding reads as `<`. A character that is
  always sent, plus the one start bit that lets it be framed, does say so.

Measured on a 26 character message at 12 kHz, SNR quoted over the whole 12 kHz band. Every noise
row is the mean of five seeds, with the seed range in parentheses:

| case | CER % | err/len | symbols heard |
|---|---|---|---|
| clean / 20 dB / 10 dB / 10 dB + QSB / 3 dB / -6 dB | 0.0 | 0/26 | 100.0 |
| -12 dB | 0.0 (0.0-0.0) | 0/26 | 93.0 |
| -16 dB | 0.0 (0.0-0.0) | 0/26 | 23.9 |
| -18 dB | 36.9 (0.0-103.8) | 9.6/26 | 6.8 |
| **-20 dB** | **86.9** (61.5-100.0) | 22.6/26 | 1.4 |
| **-22 dB** | **98.5** (92.3-100.0) | 25.6/26 | 0.2 |
| burst over 2 symbols (128 ms) | 0.0 (0.0-0.0) | 0/26 | 98.1 |
| burst over 8 symbols (512 ms) | **26.9** (11.5-53.8) | 7.0/26 | 92.3 |
| tuning error +8 Hz (half a tone spacing) | **93.1** (88.5-100.0) | 24.2/26 | 17.9 |
| tuning error +16 Hz (a whole spacing) | **111.5** (100.0-119.2) | 29.0/26 | 93.4 |
| noise only, nothing transmitted | **100.0** (100.0-100.0) | 26/26 | 0.0 |

The matched filter is why the ladder goes this deep. A symbol is 64 ms of one tone, so the
correlation is only about 15.6 Hz wide against noise spread over 12 kHz: roughly 29 dB of processing
gain, before the rate 1/2 code adds its own. **The floor is -18 dB over the 12 kHz band**: 36.9 %
mean over five seeds, range 0.0-103.8, so the 0.0 % this row used to show was one lucky draw - and
it is the one row whose spread is too wide to pin from below, so its floor is 0.0 (see *Where the
failure rows' limits come from*). The 2.5 kHz equivalent is not 12 kHz + 6.8 dB: with noise shaped
to 500-3000 Hz (`make -C support/digi floors-2k5`), twelve noise seeds a point, the same take is
exact on every draw through -12.0 dB and still exact on the median draw at -12.5 and -13.0 dB, where
the worst draws reach 38.5 and 42.3 %. The coast is bimodal rather than smooth: at -13.5 dB the
median draw costs 7.7 % and the worst draw costs the whole message (0.0..103.8, mean 32.4), and by
-14.5 dB the median is 100 % (23.1..111.5). At -13.2 dB, where the arithmetic maps the 12 kHz -20 dB
row, the median draw is exact and the mean over twelve is 14.1 % across 0.0..100.0, not 86.9 %; at
-15.2 dB, where it maps the 12 kHz -22 dB row, the median is 96.2 % (50.0..103.8). The arithmetic
gives the onset, not the curve - the 12 kHz figure includes out-of-band noise the receiver does not
fully reject, so confining the noise to the channel is worth about 2 dB at the same median severity.

A single run of this measurement is worth almost nothing here, which is why it is quoted as a
median with its range: past -13 dB the first seed can land anywhere between a clean decode and none
at all, and the mean of the twelve draws describes no take that was ever decoded. Five seeds showed
the same spread (0.0..38.5 at -12.5 dB, 0.0..103.8 at -13.5); twelve say where the middle is, and
the median draw is the one whose text is printed beside it.

Those readings are guarded rather than quoted: `make -C support/digi floors-2k5-check` measures
eleven of the points again - the three mapped equivalents for each mode, MFSK16 at -12.5, -13.5 and
-14.5 dB, Olivia at -12.5, -13.0 and -13.2 dB - and fails when a point's median draw moves by more
than one character of the 26 character take (3.8 %) or the least and most wrong draws move by more
than two (7.7 %). CI runs it, so a receiver change that moved this coast has to be noticed and the
paragraph rewritten instead of leaving the numbers above quietly false. The full 0.5 dB curve is
still `floors-2k5`, which the job does not run.

Every noise row in this file was re-measured after a bench bug was found and fixed:
`rng_next()` never wrote its new state back, so each take received one Gaussian sample
repeated - a DC offset, which the tone correlators reject. No row below 0 dB had ever
really been tested against noise, and the WEFAX numbers moved when it was repaired too.

The burst rows are the interleaver: two symbols erased is eight bits spread over eight symbols,
which the rate 1/2 code repairs completely, and eight symbols erased is thirty two bits over
eight symbols, four in each of them, which it does not. The mean is 26.9 % (7.0 characters of 26,
range 11.5-53.8 over five seeds): with Varicode a character is about ten bits rather than six, so
the same thirty two damaged bits reach about a third of the message instead of an eighth of it.
The row is pinned from below so that a receiver which started hiding the damage would fail here.

The tuning rows are the tone spacing. The tones are exactly one cycle apart per symbol, so a 16 Hz
error puts the signal on the neighbouring tone: every symbol is heard strongly (93.4 % mean) and
111.5 % of the message is wrong (range 100.0-119.2) - more than 100 % because the receiver also
prints extra characters out of the noise. Half a spacing is worse in a different way - the energy
splits between two tones, the winning tone no longer holds half of it (17.9 % heard), and the
decisions scatter (93.1 %, range 88.5-100.0). There is no tuning search in the receiver; this is the
same cliff the RTTY and NAVTEX sections measure, at the width this mode's tones give it.

The noise-only row is the one that must never read as text. With nothing
transmitted the receiver still frames characters out of the noise, and the row
pins that text at a character error rate of 100 %, so a receiver that began
inventing spaces where the signal was silent would show up as a regression.

### Where the failure rows' limits come from

Seven rows here have to keep failing, and each one's floor is derived from the five draws it was
measured with rather than typed in. The rule is the lower one sided 95 % confidence bound of the
mean of five draws, `mean - 2.132 * s.e.`, with t = 2.132 for four degrees of freedom. A build is
only better on such a row when its mean falls below what the five draws support, so that is where
the row fails. The five draws themselves are in the case table (`floor_seeds`), so re-measuring
a row means editing its draws and the limit follows; nothing else has to be kept in step.

| row | five draws, CER % | mean | s.e. | floor |
|---|---|---|---|---|
| snr -18 | 0.0, 7.7, 73.1, 0.0, 103.8 | 36.9 | 21.6 | **0.0** (the bound is -9.2) |
| snr -20 | 61.5, 100.0, 96.2, 80.8, 96.2 | 86.9 | 7.2 | 71.7 |
| snr -22 | 100.0, 100.0, 100.0, 92.3, 100.0 | 98.5 | 1.5 | 95.2 |
| burst 8 symbols | 42.3, 11.5, 11.5, 15.4, 53.8 | 26.9 | 8.9 | 8.0 |
| tone +8 Hz | 92.3, 100.0, 92.3, 88.5, 92.3 | 93.1 | 1.9 | 89.1 |
| tone +16 Hz | 119.2, 119.2, 119.2, 100.0, 100.0 | 111.5 | 4.7 | 101.5 |
| noise only | 100.0 five times | 100.0 | 0.0 | 100.0 |

The -18 dB row is the one the rule refuses to pin: two of its five draws decode the message exactly,
so its bound is below zero and the limit is printed as 0.0 instead of a positive number invented to
look like a pin. That is a statement about the row - at -18 dB over 12 kHz this 26 character take is
inside the noise on some draws - and the failure on the ladder is carried by -20 and -22 dB, where
the spread is narrow enough for a floor. A receiver that started hiding damage at -18 dB would still
show up on the max side (70 %) and on the 100 % mean of the noise-only row.

The noise-only row's floor of exactly 100 % is the rule with no spread to widen it: every draw reads
the whole message wrong, one character of 26 would put the mean at 96.2 %, and that is a receiver
inventing text where nothing was sent.

The max side of these rows is not derived and does not pretend to be: it is a ceiling - 70, 100,
110 and 130 % - set above the mean of the row's own five draws (the check is on the mean, not on a
draw), so a row that gets *worse* fails before it can pass as noise.

## Olivia, `make -C support/digi olivia`

Thirty-two tones 31.25 Hz apart - 1000 Hz of signal, which is what the `32/1000` in
the mode's name counts - at 31.25 baud, one tone per symbol. A tone is five bits and
sixty-four tones are a block, so a block carries five seven bit characters: the mode's two
layers are the MFSK across the frequency axis and a Walsh function along the time axis.

This modem uses Olivia's own mapping, and there is no tone pair table in Olivia to use. Each
character is one of 128 Walsh sequences - sixty-four rows of the 64 x 64 Hadamard matrix
and their complements, so the character's seven bits are six for the row and one for the
sign - scrambled with `0xE257E6D0291574EC` rotated thirteen bits per character, and spread
diagonally over the block: character `b` puts its bit `t` into symbol `t`, bit position
`(b + t) mod 5`. The five bit symbols go to tones through the mode's Gray code. That is the
ARRL draft specification and fldigi's MFSK codec, and it is what this section measures.

Which parts are the bench's:

* **the bench's**: the stream starts with the length in its first two characters, because Olivia is a
  continuous stream with no framing of its own; the receiver decides each symbol from
  one whole 32 ms window rather than fldigi's overlapped FFT and separate symbol
  clock; and the phase is continuous rather than shifted at random by +-90 degrees per
  symbol, which the specification does only so that a repeated tone is not a pure tone.
* **the mode's**: the tone grid, the rate, one tone a symbol, the Walsh/Hadamard codewords, the
  scrambler, the diagonal spread, the Gray code, and five characters in a block.

Measured on the same 26 character message, same channel, same scoring as MFSK16. Every noise row
is the mean of five seeds, with the seed range in parentheses:

| case | CER % | err/len | symbols heard |
|---|---|---|---|
| clean / 20 dB / 10 dB / 10 dB + QSB / 0 dB | 0.0 | 0/26 | 100.0 |
| -6 dB | 0.0 | 0/26 | 94.9 |
| -12 dB | 0.0 (0.0-0.0) | 0/26 | 1.1 |
| -14 dB | 0.0 (0.0-0.0) | 0/26 | 0.2 |
| -16 dB | 0.0 (0.0-0.0) | 0/26 | 0.0 |
| **-18 dB** | **20.8** (11.5-34.6) | 5.4/26 | 0.0 |
| **-20 dB** | **73.1** (50.0-96.2) | 19.0/26 | 0.0 |
| burst over 2 symbols (64 ms) | 0.0 (0.0-0.0) | 0/26 | 99.6 |
| burst over 8 symbols (256 ms) | 0.0 (0.0-0.0) | 0/26 | 98.0 |
| tuning error +15 Hz (half a tone spacing) | 0.0 (0.0-0.0) | 0/26 | 6.8 |
| noise only, nothing transmitted | **100.0** (100.0-100.0) | 26/26 | 0.0 |

**The floor is -18 dB over the 12 kHz band**: 20.8 % mean over five seeds, range 11.5-34.6, and a
derived floor of 12.3 % (see *Where its floors come from*). The arithmetic 2.5 kHz equivalent, -11.2
dB, is about 1.8 dB too high: with noise shaped to 500-3000 Hz (`make -C support/digi floors-2k5`),
twelve noise seeds a point, every draw is exact through -11.0 dB and the median draw is still exact
through -12.5 dB, where the worst draws cost up to 11.5 %. The first point whose median draw loses
a character is -13.0 dB (3.8 %, range 0.0..88.5, mean 13.1), and at -13.2 dB - where the arithmetic
maps the 12 kHz -20 dB row - the median is 3.8 % across 0.0..92.3, where that row's own mean is
73.1 %. At -11.2 dB, where it maps the 12 kHz -18 dB row, every draw of the shaped channel is exact
rather than 20.8 %. The Walsh layer is why it goes that deep. A symbol error is one bit in each
of five codewords of sixty-four bits, and the rows of the matrix are 32 apart, so the transform
reads the right one through a great deal of damage, and the soft decisions keep working long after
no symbol has a winning tone: at -16 dB the heard column is 0.0 % and the text is still exact.

Above the floor the soft bits carry it. At -12 dB only 1.1 % of symbols look heard and not one
character is wrong. That is the opposite of the old pair based stand-in, where 68.8 % of symbols
looked heard and sixteen characters of twenty-six came back wrong.

The burst rows still cost nothing, which is the measurement rather than a missing case: two
erased symbols is ten bit errors over ten codewords and eight erased symbols is forty, both
well inside what a 64 bit code with distance 32 repairs. The tuning row is the more interesting
one: at half a tone spacing only 6.8 % of symbols have a winning tone that holds half the
energy, the energy is split between two neighbours, and every character still comes back.

The noise-only row is the one that must never read as
text: 100 % of the message wrong, no symbol heard.

### Where its floors come from

The same rule as MFSK16: the lower one sided 95 % bound of the mean of the five draws a row was
measured with, `mean - 2.132 * s.e.`, with the draws kept in the case table.

| row | five draws, CER % | mean | s.e. | floor |
|---|---|---|---|---|
| snr -18 | 34.6, 19.2, 23.1, 11.5, 15.4 | 20.8 | 4.0 | 12.3 |
| snr -20 | 80.8, 96.2, 69.2, 50.0, 69.2 | 73.1 | 7.6 | 56.9 |
| noise only | 100.0 five times | 100.0 | 0.0 | 100.0 |

Both noise rows here have a spread narrow enough to pin, unlike MFSK16's -18 dB: the Walsh layer's
floor is 12.3 % below a mean of 20.8 % rather than a bound under zero, which is what makes it the
mode to use when the channel is bad.

## Receiving a DWD teletype station on the radio

Nothing needs adding to the firmware; the path exists and was traced end to end, because it is not
obvious and there is **no menu item for it** - there are zero `MENU_RTY_*` rows, which is by design,
since digi parameters are front-panel only in this radio (PSK speed works the same way).

1. Mode button into RTTY.
2. **Encoder 2 is `SFT`** (shift) and cycles all six values via `change_and_limit_int(..., 0,
   RTTY_SHIFT_NUM-1)` in `ui_driver.c`. DWD needs **450**.
3. **Encoder 1 is `BD`** (baud). DWD needs **50**.
4. Tune so the pair sits on the modem's filters: for a +/-225 Hz emission on 14467.3 kHz that is
   14466.160 kHz, putting the tones at 915 and 1365 Hz. On live radio the shift matters a great deal
   (table above), so tuning is not optional here.

Both settings persist: `EEPROM_RTTY_SHIFT_IDX` = word 387 and `EEPROM_RTTY_SPEED_IDX` =
word 388, each with min/max limits, added in commit `979cfdf6`. Boot order re-applies
them instead of leaving the compile-time 170 Hz / 45 Bd initialisers in the modem:
`TransceiverStateInit` -> `UiDriver_Init` (which runs `UiDriver_LoadSavedConfigurationAtStartup`
-> `UiConfiguration_LoadEepromValues`) -> `AudioDriver_Init` (which calls `Rtty_Modem_Init`).
If that ordering ever changes, the symptom is a radio that shows the right setting
on screen but demodulates at 170 Hz / 45 Bd until an encoder is touched - which is
the same species of fault as the EEPROM address collision that destroyed 22 settings.

Two things to keep in mind when picking a sample: NAVTEX is **512 kHz in the United States** and
518 kHz internationally, so the receiver has to be in range of the service being tested; and for
weather fax, remote receivers add their own AGC, squelch and noise reducer unless those are turned
off, which is exactly the distortion the bench should be shown rather than hiding it.

## Where this goes next

1. Envelope sync capture for fax, graded by the skew case that currently exposes the gap.
2. Lift `cw_decoder.c` in the same way (dependency list above) and get CW word error rate against
   the fixed `CW_ONE_BIT_SAMPLE_COUNT` detector as its baseline.
3. RTTY: chase the fast-fading failure with the numbers in front of us.
4. Only then decide about FT8 RX, where the RAM question is per board rather than per mode.

## Mode identification, `./digi_bench gate-check`

The four `-wav` verbs below the RTTY one are each pointed at one mode, and a recording from a
WebSDR waterfall is usually not the mode you aimed at. Feeding a WEFAX picture to the Olivia decoder
produces confident gibberish, which is worse than nothing because it looks like a decode. So a gate
sits in front of each of them: the take's shape is measured and compared against the shape the mode
has, and the decode is refused when it clearly belongs to something else.

```
./digi_bench mode-id take.wav        # what a take looks like, and which modes would accept it
./digi_bench gate-check              # the calibration matrix, exits non-zero when a row is wrong
```

The measurement is one-second windows (the `capture-check` window, for the same reason: a longer
window blurs the edges of the narrow modes), a Goertzel spectrum in 1 Hz bins from 250 to 3000 Hz,
the median bin of each window taken as the noise floor, and the narrowest run of bins standing more
than 3 dB over it, weighted by how much they stand over it. The floor subtraction is the whole
trick: measured against a fixed level, the width of an Olivia signal is the width of the hiss, not
of the mode, and every row of `gate-check` fails.

| mode | width (Hz) | centre (Hz) | fraction | accepted from SNR |
|---|---|---|---|---|
| NAVTEX | 186 | 1700 | 0.26 | +3 dB |
| MFSK16 | 258 | 1123 | 0.38 | +3 dB |
| Olivia 16/500 | 1121 | 1573 | 0.46 | +3 dB |
| Hell 122.5 | 107 | 1154 | 0.06 | +3 dB |
| RTTY | 176 | 1702 | 0.29 | not gated |

Each mode has a box in width, centre and fill fraction, and the boxes are disjoint except that
NAVTEX and RTTY overlap badly: RTTY's two tones at 170 Hz spacing are 4 bins wide out of a measured
176, which is the shape of a single tone. It is therefore not gated at all - it already has
`capture-check`, which is mode-specific and never calls a modem, and a gate that also refused
RTTY would refuse RTTY.

Three answers, not two. Below about +3 dB a mode's own shape is no longer measurable - the width
that comes back is the width of the noise - so a take that measures outside every box is passed
through rather than refused, on the principle that the point of these verbs is weak signals: a gate
that refused them would be worse than no gate. It refuses only when the shape says the take belongs
to a mode that is not the one being asked. Cross-mode refusal is the reliable part, and it works
down to the floor of the test: a NAVTEX take offered to Olivia, MFSK16 or Hell is refused with a
message naming the mode it does look like.

What the gate does not do: it cannot tell a real transmission from a synthetic one, since that is
`capture-check`'s job and only RTTY has it; it cannot tell Olivia 16/500 from Olivia 8/1000, whose
shape is the same box; and it is not a demodulator, which is why `gate-check` asserts nothing about
what any modem says.

## Streaming SITOR-B, `./digi_bench sitor-stream`

`navtex.c` is a reference modem: it takes a whole buffer, searches 118 to 122 samples per bit,
`malloc`s. None of that belongs in a radio, so the port lives in `drivers/digi/sitor_rx.{c,h}` -
one sample in, one character out, two quadrature resonators on the tone pair, the bit period
measured by folding detected transitions onto each candidate period, seven-boundary alignment
scored by 4-of-7 validity, a sixteen-character ring for the time-diversity pair, no `malloc`, no
`printf`, no whole-buffer pass.

**It does not pass its table yet, and the table says so.** Current rows:

```
case            CER %     conf  bit ms   fix  lost  rev  reference CER
clean           100.0      1.00   10.0     32    29    0        0.0
snr 20          100.0      0.00   10.0      0     0    0        0.0
rate -3 %        93.0      0.79   10.3      0     0    0       78.3
```

The measurements along the way are worth keeping, because two of them were not known before:

- The reference modem needs the bit rate to be within about 1.7 % of 100 Bd, and a recording that
  fails it decodes at 78 to 100 % error. That is the window 118 to 122 samples, and nothing else.
  A sound card a few tens of ppm off is inside it; a WebSDR stream that has been resampled twice is
  not. This is why `sitor-stream` generates its takes by time scaling and keeps the rate rows.
- Time scaling a take moves the bit rate *and* the tones together: an 8 % rate error carries the pair
  130 Hz away, which is past the 85 Hz the two fixed resonators follow. So a receiver that intends to
  tolerate rate error also has to acquire the tone pair, which `sitor_rx.c` does not yet do. The
  table's rate rows are kept inside 3 % for that reason, and the pair search is listed below as work,
  not quietly avoided.
- Two bugs the table caught that reading the code did not: the group-boundary search asked for more
  bits than a NAVTEX message contains (280 is about forty characters and a 46-character message is
  861 bits, so the decision was never reached and the receiver sat in alignment forever); and the
  bit order cannot be detected from 4-of-7 validity at all, because the reverse of a constant-ratio
  code is another constant-ratio code - only the SIA/SIB phasing alternation distinguishes them, and
  it has to be scored while the boundary is still being found, because the phasing preamble is twenty
  characters and the boundary needs forty to settle.

Not claimed, and still open: alignment under noise (every row with noise in it never leaves
SEARCH_ALIGN), the diversity pairing (the clean row emits each block twice, which is what the 32
`fix` and 29 `lost` are), and the tone-pair search. The firmware build does not compile `sitor_rx.c`
yet and nothing on the radio uses it. `sitor-stream` is not in CI for the same reason: it fails.

## The convolution RX filter, `make -C support/digi convolution-check`


One limit on what this section measures, and it bit here once. `convolution_probe` compiles the shipped
`audio_convolution.c`, but it substitutes its **own** complex FFT for the CMSIS `arm_cfft_f32` the radio
links. So every figure below - the passband ripple, the skirt, the image rejection, the cost per sample -
is a statement about the arithmetic and the geometry, and none of them is a statement about the ARM
library's twiddle tables or its staged radix path. When a radio image came back silent while the host
said the filter worked, the honest reading was that the host had never tested the thing in question, and
that is why "measured off the radio" should never be quoted as clearing a transform instance. (The
silence itself turned out to be RF gain and AGC on the radio, with both geometries reporting `in == out`,
nothing dropped, and real signal on both sides of the filter - which is also worth recording: the counters
said the firmware was healthy while it was being blamed.)

`USE_CONVOLUTION` is an alternative SSB/CW receiver filter, committed as 3434f9db (2018-08-22) and
commented out in every board configuration since. Instead of the two 199-tap Hilbert FIRs and the
`i+q` / `i-q` sum that make the sideband decision today, it convolves the decimated I/Q with a
1024-tap windowed-sinc complex bandpass, in the frequency domain, in eight partitions, and takes the
real part as the audio: the filter and the demodulator become the same operation, and the cost stops
depending on how steep the shape is. The maths is Warren Pratt's wdsp partitioned convolution.

It had not been buildable since 1c09d16c (2019-02-02), and building it turned out to be the small
part. Nine things were wrong, the first six in the order they were found:

1. `Sample_Buffer` in `audio_driver.h` used `SAMPLE_BUFFER_SIZE` from a header that includes it. That
   one line is why 95 of the 476 translation units failed, rather than one file.
2. The FIFOs enqueued `mmb.nr_audio_buff[]` - the noise reducer's storage - as their own blocks, and
   `Sample_in_buffer_add()` took an `NR_Buffer*` while `Sample_out_buffer_remove()` gave back an
   `NR_Buffer*` read from a `Sample_Buffer*` array. The two types are only the same shape because
   `NR_BUFFER_SIZE` and `SAMPLE_BUFFER_SIZE` happen to agree. The queues now own their blocks.
3. `AudioDriver_SetConvolutionFilter()` - the function that turns the impulse response into the
   frequency masks the worker multiplies by - was called from nowhere. Every mask was zero, so the
   path could only have produced silence even if it had run.
4. The block was filtered at the decimated rate while the filter was designed against
   `IQ_SAMPLE_RATE_F` (48 kHz), which moves the passband by the decimation factor; and `cbs.DF`, the
   divisor the interrupt used for the block size, was never assigned anywhere in the tree.
5. The forward FFT is done **in place**, so by the time the code slid the second half of `fftin` into
   its first half to form the next block's overlap history, `fftin` held a *spectrum*. wdsp does not
   have this problem because its plan writes to a separate buffer. Measured against a plain complex
   FIR over the same taps the values ran to 1e12, and the difference was 1197 times the reference.
   The history now comes from the input block, which still holds the samples.
6. The mask generator right-justifies each partition, and the note above it says "take output from
   left side of output buff, discard right side" - with the author's own "unsure how to interpret
   that" next to it. The code kept the right half. That is not a delay: the two halves of an
   overlap-save block are the clean part and the wrap-corrupted part, so the filter in use was not
   the filter that was designed. Measured, sideband separation went from 12.3 dB to 144 dB.

Three more were in the setup rather than in the filter, and the harness passed all three until it was
made to compare with the radio instead of with itself:

7. **The sidebands were swapped.** The designer was carried over with `e^{-jwn}` taps, and the masks
   here are a forward transform of them, so a band given as positive frequencies passed the negative
   ones: USB received the lower sideband. The check for exactly this was green, because the probe's
   model of the Hilbert pair was written `table[k] * x[n-k]`, while `audio_filter.c` hands the tables
   to `arm_fir_f32()` unreversed and CMSIS pairs `pCoeffs[0]` with the *oldest* sample. The model was
   mirrored, the filter was mirrored, and the two agreed. With the model in the radio's tap order the
   check failed on the committed code, and the designer's sign is what was corrected.
8. **The band ignored where the filter path sits.** It was always `width/10 .. width`. A CW path is
   300 or 500 Hz wide *around the sidetone*, 500 to 950 Hz up, so all ten 300 Hz paths and three of
   the five 500 Hz ones had no overlap with the band at all (-110 dB at the note), and the offset
   1.8 kHz and 2.3 kHz paths lost their top end. The band is now the path's: `offset +- width/2`,
   with a 50 Hz floor where the path starts at zero, which is where the Hilbert pair is 4 dB down.
9. **The gain was `rate / width`.** The windowed sinc already has a passband gain of one, so the
   audio came out 7 dB hot at 2.7 kHz, 22 dB at 500 Hz and 26 dB at 300 Hz, and stepped with every
   change of filter. The `i+q` sum it replaces has a gain of two on every table, so it is two.

The setup is one function now, `AudioDriver_ConvolutionSetFilterPath()`, which the radio and the probe
both call: the probe used to type its own copy, with different edges and a different gain, which is why
8 and 9 could not show. Section 8 of the probe then takes the radio's own `FilterPathInfo[]` - the
Makefile cuts the table out of `audio_filter.c` - and puts a tone through the centre of every SSB and CW
path, both sidebands: it has to come out within 1 dB of the level the path in use delivers, and 60 dB
down on the other side of zero. Each of 7, 8 and 9 put back makes it fail.

Today the option compiles in all 95 units that can see it (`make build-config-check` holds it there),
it links, and `convolution_probe.c` runs the shipped module on a host. What it is worth, measured by
putting one complex tone through each path at the 2.7 kHz / 12 kHz filter path:

| | convolution, 1024 taps | Hilbert pair + `i+q`, 199 taps |
|---|---|---|
| sideband separation at +-1350 Hz | **110 dB** | 88 dB |
| response at +3000 / +4500 / +6000 Hz, above the 2.7 kHz edge | -110 / -111 / -112 dB | 0.0 / -85 / -94 dB (its IQ filter closes at 3.5 kHz, see below) |
| arithmetic per sample, counted as it runs (steady state) | **356** | 796 |
| CPU time per audio sample on the F407 at 168 MHz | **3.2 us, 3.8 % of the core** | 7.1 us, 8.5 % |
| the same 1024-tap shape as an ordinary FIR | 8192 per sample | - |
| audio delay, 2.7 kHz SSB path at 12 kHz | 11.0 ms (10.0 of buffering, 1.0 in the taps) | 8.25 ms |
| audio delay, 500 Hz CW path at 12 kHz | 15.0 ms (10.0 of buffering, 5.0 in the taps) | 8.25 ms |
| static RAM the module adds | **54 KiB** | none, the tables live in flash |
| flash | +44,320 bytes, 41,296 of them the minimum phase design and the 4096-point transform it uses | - |

The op counts are steady state, and that is what the CPU row multiplies. The probe's own run averages read
372 and 786, which is what its counter prints when it is asked: the convolution run divides four blocks of
tail flush by the stream length (+16 per sample) and the pair run includes the first 199 samples, in which
the window is still filling (-10 per sample). Steady state is what the radio pays on every sample it
actually filters, so it is what every table here bills.

So the answer to "might it be better" is yes on both selectivity and cost per sample, at the price of
about 3 ms of delay on SSB and 7 ms on CW at this geometry, 54 KiB of RAM and 44 KB of flash. The delay
is a geometry choice: at `-DFFT_CONVOLUTION_SIZE=64` this filter is *earlier* than the Hilbert pair, see
the table below and "Minimum phase" after it.

The geometry is a build choice, and all three settings below were measured with this same harness:

```
make -C support/digi convolution-check EXTRA_DEFS='-DFFT_CONVOLUTION_SIZE=64'
```

The tap count and the impulse array follow from the block size (`CONVOLUTION_MAX_NO_OF_COEFFS` is
`CONVOLUTION_MAX_NO_OF_BLOCKS * FFT_CONVOLUTION_SIZE / 2` unless something overrides it, and
`AudioDriver_ConvolutionSetFilterPath()` clamps to the same product), so one flag moves the whole geometry
rather than leaving the arrays sized for a different build.

All three were measured with the same harness, on the same signals. The ripple, the skirt and both
rejection columns come from sweeping 216 whole-cycle offsets through each path twice, once on each side
of zero, so the passband curve and the image curve are the same measurement asked of the two halves of
the spectrum; the last two columns are from the linked image rather than from `sizeof`:

| `-DFFT_CONVOLUTION_SIZE` | block | taps | audio delay at 12 kHz, SSB 2.7 kHz / CW 500 Hz | in-band ripple | skirt, -6 to -60 dB | image, worst in band | ops / sample, steady state | `.bss` cost | stack left |
|---|---|---|---|---|---|---|---|---|---|
| 256 (as committed) | 128 | 1024 | 11.0 / 15.0 ms | 0.00 dB | **34 Hz** | -110.3 dB | 356 | 55,472 B | 40,836 B |
| 128 | 64 | 512 | 5.7 / 9.2 ms | 0.00 dB | 71 Hz | -110.3 dB | 324 | 31,920 B | 64,388 B |
| 64 | 32 | 256 | **2.9 / 5.9 ms** | 0.00 dB | 144 Hz | -110.2 dB | **292** | 20,144 B | 76,164 B |
| none: the Hilbert pair in use | - | 199 x 2 | 8.25 ms | (shaped later, see below) | 127 Hz, at 3.5 kHz | **-75.8 dB** | 796 | 0 | 96,304 B |

**What the arithmetic is in time.** The op counts above are measured; the clock is arithmetic. A Cortex-M4F
issues one single-precision operation per cycle, and the CMSIS and compiled loops spend roughly that much
again on loads, stores and loop control, so 1.5 cycles per counted operation at 168 MHz is the estimate
the CPU-time figures are drawn with, and 1.0 to 2.0 is the range they are uncertain by:

| | normal receiver (199-tap pair) | conv 256 | conv 128 | conv 64 |
|---|---|---|---|---|
| CPU time per 12 kHz sample | 7.1 us | **3.2 us** | 2.9 us | 2.6 us |
| share of one core | 8.5 % | **3.8 %** | 3.5 % | 3.1 % |
| the same, at 1.0 - 2.0 cycles per operation | 4.7 - 9.5 us | 2.1 - 4.2 us | 1.9 - 3.9 us | 1.7 - 3.5 us |
| one worker pass | none, it runs per sample | 0.41 ms every 10.7 ms | 0.19 ms every 5.3 ms | 0.08 ms every 2.7 ms |

Two things follow that the op count alone does not say. The pair's 8.5 % is spread evenly through the audio
interrupt, one sample at a time, while the filter's 3.8 % arrives in bursts: a whole 128-sample block, two
transforms and eight partitions, is about 0.43 ms of work in the worker every 10.7 ms, which is the block
part of the `G` reading in the measurement build. And the smaller geometries are cheaper in time as well as
RAM: 3.1 % at geometry 64 against 3.8 % at 256, for the skirt and for what section 11 charges at DC.
Neither number is a measurement of this chip: the check is `L`/`P` on the debug line, and `G` for the
burst, which is what the measurement build exists for.

The `.bss` and stack columns were re-read from four clean links after `CONVOLUTION_MAX_NO_OF_COEFFS`
became derived from the block size rather than a fixed 2048, which is 8 KiB at the default geometry and
12 KiB at the other two: `_ebss` 0x200087c8 without the option and 0x20016074, 0x20010474, 0x2000d674 at
256, 128 and 64, against a stack top of 0x2001fff8. Text moves 44,320 bytes at 256 and 3,504 at 128 and
64 - the difference being the 4096-point transform, which only the default geometry asks for.

This is the trade, and it is a real one rather than a free lunch: fewer taps buys latency and RAM with
the skirt. 1024 taps gets from -6 dB to -60 dB in 34 Hz and 256 taps takes 144 Hz to do it. The audio
band itself is flat at all three (the ripple column is measured from 300 to 2400 Hz, and it read 1.1 and
2.9 dB at the two smaller geometries only while the band started at 270 Hz and their wider low skirt
reached into that window). What the wider skirt costs at the low end instead is the other sideband close
to the carrier: at 256 taps a signal within about 100 Hz of zero on the wrong side is rejected no better
than the Hilbert pair rejects it, where 1024 taps has it 100 dB down 10 Hz out.

The image column is the reason the option exists: a tone on the *rejected* side of the carrier, at any
offset inside the audio band, comes out 34 dB lower than it does from the path in use today. That is
what a Hilbert pair made of two 199-tap real FIRs costs - its quadrature error is largest at the ends of
the band, which is exactly where a digimode or a badly centred voice sits. The three convolution figures
are one number because they are one decision: -110 dB is the floor the minimum phase design is given
(see below), and all three geometries reach it. In linear phase they read -131, -131 and -113 dB, the
first two of which are the float32 floor of the measurement. -75.8 dB for the path in use is a real
figure.

The Hilbert row's corner at 3.5 kHz is not sloppiness on the radio's part - its IQ filter is deliberately
wider than the audio band and the post-demod filters finish the shape afterwards, so the two rows are not
cut from the same cloth, and the harness prints the comparison instead of scoring it.

#### Every path, not just the two

The figures above are the 2.7 kHz SSB path; the geometry table's skirt and ripple are the 500 Hz CW
one. `path_sweep.c` sweeps all 62 SSB and CW paths of `FilterPathInfo[]`, a shape and a bill for each, so
that neither "it is the better filter" nor its cost rests on two examples:

```sh
sh support/digi/convolution_graphs/build-sweep.sh /path/to/mchf-eclipse /tmp/pathsweep.csv
```

Each row is that path's own `width` and `offset`, designed at the rate its blocks carry, and swept on the
same grid for both filters. `ripple` is peak to peak over the band's interior, the outer tenth of the
width left out at each edge because a windowed sinc is 6 dB down at its nominal edges by design;
`skirt` is -6 to -60 dB above the top edge; `image` is the worst level in the mirror of that same
interior. A `-` in a skirt column means the filter measured is already 60 dB down at the band edge, so
there is no skirt to measure; `n/c` in a stock column means the stock table's -6 dB point falls inside
the path's band, so the number would be that cutoff rather than the path's. The sweep prints the measured
edge level in the CSV's `stock_edge_db` column.

Three things the two-path comparison could not show.

**The pair is two different filters.** 44 of the 62 paths run the 199-tap table decimated to 12 kHz. The
18 at 3.8 kHz and wider run an 89-tap table and *do not decimate at all*: `use_decimatedIQ` in
`audio_driver.c` tests for `i_rx_new_coeffs`, and those rows point at `i_rx_4k5` / `5k` / `6k` / `7k5` /
`10k`, so their blocks arrive at 48 kHz and the pair runs at 48 kHz whatever `sample_rate_dec` says in
the table. Measured, the pair costs 8.5 % of the core on the row that has been quoted all along, and
15.3 % on every path from 3.8 kHz up; the convolution filter costs 3.8 % or 15.3 %. On the wide paths it
is not cheaper in operations at all - 89 taps x 4 operations is 356 per sample, the same number the
measured convolution bills - it is only much better shaped.  Six of those wide rows use a table whose
-6 dB point is already inside the path, so their stock shape columns are n/c below.

**The skirt follows the rate, not the path.** It is one geometry, 1024 taps: 36.5 Hz from -6 to -60 dB on
every 12 kHz path, and 145 Hz at 48 kHz, where the same taps are four times shorter in time. Against the
pair that is 3.5x steeper on the narrow paths (127 Hz) and 7.9x on the comparable wide rows (1143 Hz at
3.8-4.4 kHz).

**Image rejection is where the two differ most, and the pair is worst just where its band is widest.**
-72 to -85 dB on the comparable 199-tap rows, but -24 to -56 dB on the comparable 89-tap rows from
3.8 kHz up: an 89-tap Hilbert pair at 48 kHz is poor quadrature at the top of a 10 kHz band. The
convolution filter is -110 dB on all 62, because its floor comes from the design, not from the rate. The
stock ripple figures on the wide rows that remain comparable reach 1.8 dB; the largest, up to 11.1 dB,
measure the IQ filter's own corner and are on rows marked n/c because their table is already rolling off
inside the band. The post-demod IIR finishes the shape after that corner, but it cannot restore what the
table removed; the rows whose table closes inside the path are marked n/c for that reason. For every
comparable path, no post-demod filter can reject an image, so the image column does stand for both sides.

The op counts here are steady state, the same convention as the first table above: 356 for the convolution
filter at this geometry and 796 for the pair, times 1.5 cycles at 168 MHz - the same model as above.

| id | path | width | centre | rate | stock ripple | stock skirt | stock image | stock CPU | conv ripple | conv skirt | conv image | conv CPU |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 | 500Hz | 300 | 500 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 5 | 550Hz | 300 | 550 | 12 kHz | 0.00 | 127.0 | -77.5 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 6 | 600Hz | 300 | 600 | 12 kHz | 0.00 | 127.0 | -78.5 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 7 | 650Hz | 300 | 650 | 12 kHz | 0.00 | 127.0 | -79.5 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 8 | 700Hz | 300 | 700 | 12 kHz | 0.00 | 127.0 | -79.7 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 9 | 750Hz | 300 | 750 | 12 kHz | 0.00 | 127.0 | -80.4 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 10 | 800Hz | 300 | 800 | 12 kHz | 0.00 | 127.0 | -81.3 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 11 | 850Hz | 300 | 850 | 12 kHz | 0.00 | 127.0 | -82.0 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 12 | 900Hz | 300 | 900 | 12 kHz | 0.00 | 127.0 | -82.8 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 13 | 950Hz | 300 | 950 | 12 kHz | 0.00 | 127.0 | -83.5 | 8.53 % | 0.06 | 36.5 | -110.4 | 3.81 % |
| 14 | 550Hz | 500 | 550 | 12 kHz | 0.00 | 127.0 | -75.8 | 8.53 % | 0.00 | 36.5 | -110.4 | 3.81 % |
| 15 | 650Hz | 500 | 650 | 12 kHz | 0.00 | 127.0 | -77.5 | 8.53 % | 0.00 | 36.5 | -110.4 | 3.81 % |
| 16 | 750Hz | 500 | 750 | 12 kHz | 0.00 | 127.0 | -79.5 | 8.53 % | 0.00 | 36.5 | -110.4 | 3.81 % |
| 17 | 850Hz | 500 | 850 | 12 kHz | 0.00 | 127.0 | -81.3 | 8.53 % | 0.00 | 36.5 | -110.4 | 3.81 % |
| 18 | 950Hz | 500 | 950 | 12 kHz | 0.00 | 127.0 | -82.0 | 8.53 % | 0.00 | 36.5 | -110.4 | 3.81 % |
| 19 | LPF | 1400 | 700 | 12 kHz | 0.00 | 127.0 | -71.9 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 20 | BPF | 1400 | 775 | 12 kHz | 0.00 | 127.0 | -72.3 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 21 | LPF | 1600 | 800 | 12 kHz | 0.00 | 127.0 | -72.3 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 22 | BPF | 1600 | 875 | 12 kHz | 0.00 | 127.0 | -72.6 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 23 | 1.1k | 1800 | 1125 | 12 kHz | 0.00 | 127.0 | -76.6 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 24 | 1.3k | 1800 | 1275 | 12 kHz | 0.00 | 127.0 | -79.5 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 25 | 1.4k | 1800 | 1425 | 12 kHz | 0.00 | 127.0 | -82.0 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 26 | 1.6k | 1800 | 1575 | 12 kHz | 0.00 | 127.0 | -83.5 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 27 | 1.7k | 1800 | 1725 | 12 kHz | 0.00 | 127.0 | -85.4 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 28 | LPF | 1800 | 900 | 12 kHz | 0.00 | 127.0 | -72.3 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 29 | LPF | 2100 | 1050 | 12 kHz | 0.00 | 127.0 | -73.7 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 30 | BPF | 2100 | 1125 | 12 kHz | 0.00 | 127.0 | -73.7 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 31 | 1.3k | 2300 | 1275 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 32 | 1.4k | 2300 | 1412 | 12 kHz | 0.00 | 127.0 | -78.5 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 33 | 1.6k | 2300 | 1562 | 12 kHz | 0.00 | 127.0 | -81.0 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 34 | 1.7k | 2300 | 1712 | 12 kHz | 0.00 | 127.0 | -82.8 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 35 | LPF | 2300 | 1150 | 12 kHz | 0.00 | 127.0 | -73.7 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 36 | LPF | 2500 | 1250 | 12 kHz | 0.00 | 127.0 | -74.9 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 37 | BPF | 2500 | 1325 | 12 kHz | 0.00 | 127.0 | -75.1 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 38 | LPF | 2700 | 1350 | 12 kHz | 0.00 | 127.0 | -75.1 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 39 | BPF | 2700 | 1425 | 12 kHz | 0.00 | 127.0 | -75.2 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 40 | LPF | 2900 | 1450 | 12 kHz | 0.00 | 127.0 | -75.1 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 41 | BPF | 2900 | 1525 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 42 | LPF | 3200 | 1600 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 43 | BPF | 3200 | 1675 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 44 | LPF | 3400 | 1700 | 12 kHz | 0.00 | 127.0 | -76.3 | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 45 | BPF | 3400 | 1775 | 12 kHz | 0.00 | 127.0 | -77.5 | 8.53 % | 0.00 | 36.5 | -110.3 | 3.81 % |
| 46 | LPF | 3600 | 1800 | 12 kHz | n/c | n/c | n/c | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 47 | BPF | 3600 | 1875 | 12 kHz | n/c | n/c | n/c | 8.53 % | 0.00 | 36.5 | -110.2 | 3.81 % |
| 48 | LPF | 3800 | 1900 | 48 kHz | 1.76 | 1143.0 | -24.1 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 49 | BPF | 3800 | 1975 | 48 kHz | 1.52 | 1143.0 | -26.0 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 50 | LPF | 4000 | 2000 | 48 kHz | 1.60 | 1143.0 | -25.3 | 15.26 % | 0.00 | 145.0 | -110.4 | 15.26 % |
| 51 | LPF | 4200 | 2100 | 48 kHz | 1.45 | 1143.0 | -26.6 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 52 | LPF | 4400 | 2200 | 48 kHz | 1.31 | 1143.0 | -27.5 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 53 | LPF | 4600 | 2300 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.2 | 15.26 % |
| 54 | LPF | 4800 | 2400 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 55 | LPF | 5000 | 2500 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 56 | LPF | 5500 | 2750 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.2 | 15.26 % |
| 57 | LPF | 6000 | 3000 | 48 kHz | 0.82 | 1264.0 | -26.6 | 15.26 % | 0.00 | 145.0 | -110.2 | 15.26 % |
| 58 | LPF | 6500 | 3250 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 59 | LPF | 7000 | 3500 | 48 kHz | n/c | n/c | n/c | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 60 | LPF | 7500 | 3750 | 48 kHz | 0.38 | 1250.0 | -36.3 | 15.26 % | 0.00 | 145.0 | -110.2 | 15.26 % |
| 61 | LPF | 8000 | 4000 | 48 kHz | 0.11 | 1270.0 | -40.5 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 62 | LPF | 8500 | 4250 | 48 kHz | 0.06 | 1270.0 | -45.6 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 63 | LPF | 9000 | 4500 | 48 kHz | 0.03 | 1270.0 | -51.9 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 64 | LPF | 9500 | 4750 | 48 kHz | 0.01 | 1270.0 | -56.3 | 15.26 % | 0.00 | 145.0 | -110.3 | 15.26 % |
| 65 | LPF | 10000 | 5000 | 48 kHz | 0.04 | 1270.0 | -56.3 | 15.26 % | 0.00 | 145.0 | -110.2 | 15.26 % |

`n/c`: rows 46, 47, 53, 54, 55, 56, 58 and 59.  The sweep measures the stock response at each path's
top edge relative to its passband, and withholds the three shape columns when that edge is at or below
-6 dB - the table's own cutoff is inside the path, so the number would score the cutoff instead of the
path.  The measured edges are -26.0, -75.4, -7.9, -12.3, -6.1, -17.1, -16.2 and -37.4 dB in that order.
The tables are the 199-tap 12 kHz table on the 3.6 kHz rows, the 4.5 kHz table on 4.6/4.8, the 5 kHz
table on 5.0/5.5 and the 6 kHz table on 6.5/7.0.  The nearest unmarked rows are 4.4 kHz at -4.8 dB,
6.0 kHz at -5.6, 7.5 kHz at -5.5 and 10.0 kHz at -4.3; their -6 dB point is above the top edge, so they
stay comparable.  The stock CPU figure is the real cost of the pair and stays on every row.  The chain
sweep below measures the post-demod IIR and AGC; it does not change this list.

#### What the operator hears: through the post-demod IIR and AGC

The table above ends at the demodulated audio. The next stages are not common to every path: the
3.6-4.8 kHz paths run a pre-AGC IIR at 12 kHz, the 5.0-7.5 kHz paths have none, and the 8.0-10 kHz
paths only run an anti-alias IIR after the output interpolation. The WDSP AGC is common. The `--chain`
sweep runs the real `audio_agc.c` at its factory settings (`AGC_WDSP_*_DEFAULT`), cold-started for each
path, on an equal-amplitude passband reference and top-edge two-tone, after the path's pre-AGC IIR and
then the post-AGC anti-alias IIR. The two-tone amplitude before the path filter is `A=0.05` (the bench
level used below) and `A=0.005`, a station 20 dB weaker; the level sweep below runs from `A=0.5` to
`A=0.0005`. The common decimation and
interpolation FIRs are omitted: they multiply both sides equally. "Pre-IIR" and "anti-alias IIR" are
stage counts; 0 means the path has none. All levels are dB relative to the passband reference;
"stock deficit" is conv minus stock after the AGC, so a positive value means the stock top is lower.

| path | pre-IIR | anti-alias IIR | stock top after IIR | conv top after IIR | stock top after AGC (nominal) | conv top after AGC (nominal) | stock deficit (nominal) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 46 3.6k LPF | 10 | 6 | -26.90 | -6.90 | -26.90 | -6.90 | 20.00 |
| 47 3.6k BPF | 10 | 6 | -84.99 | -15.61 | -84.97 | -15.61 | 69.36 |
| 53 4.6k LPF | 10 | 6 | -9.34 | -7.46 | -8.68 | -7.46 | 1.22 |
| 54 4.8k LPF | 10 | 6 | -12.88 | -6.64 | -12.36 | -6.64 | 5.72 |
| 55 5.0k LPF | 0 | 0 | -6.09 | -6.02 | -6.09 | -6.02 | 0.07 |
| 56 5.5k LPF | 0 | 0 | -17.13 | -6.02 | -17.11 | -6.02 | 11.09 |
| 58 6.5k LPF | 0 | 0 | -16.16 | -6.02 | -16.15 | -6.02 | 10.13 |
| 59 7.0k LPF | 0 | 0 | -37.37 | -6.02 | -37.50 | -6.02 | 31.48 |
| 52 4.4k LPF | 10 | 6 | -6.14 | -7.39 | -5.48 | -7.39 | -1.91 |
| 57 6.0k LPF | 0 | 0 | -5.64 | -6.02 | -5.64 | -6.02 | -0.38 |
| 60 7.5k LPF | 0 | 0 | -5.48 | -6.02 | -5.47 | -6.02 | -0.55 |
| 65 10.0k LPF | 0 | 6 | -4.81 | -6.52 | -4.81 | -6.52 | -1.71 |

The n/c list does not change. On 5.0, 5.5, 6.5 and 7.0 both IIR columns are zero: this is the same
measurement as the main table, and the AGC moves the top by at most 0.2 dB. On 4.6 and 4.8 the IIRs
apply to both sides, so the stock top falls with the conv top: -9.34/-12.88 dB against -7.46/-6.64 dB,
and after the AGC the stock is still 1.2/5.7 dB below. The AGC's 0.5-0.7 dB of compression is the only
movement and does not close the gap. On 3.6 the stock top is 20/69 dB down; the IIR cannot restore the
3.5 kHz table cutoff.

No new row becomes n/c. The 4.4 kHz chain top is -6.14 dB, below -6, but the stock table's own edge is
-4.81 dB, the conv chain top is -7.39 dB, and the stock side is still above the conv. The n/c rule is
the stock table's -6 dB point inside the path, not a post-demod edge that is common to both sides. The
6.0, 7.5 and 10.0 kHz rows stay comparable: their stock chain tops are -5.64, -5.48 and -4.81 dB, all
above the conv side.

The second level is in the CSV as `stock_agc_top_weak` and `conv_agc_top_weak`. `--chain-levels`
turns that single check into a 31 point sweep from 20 dB above the bench level to 40 dB below it in
2 dB steps, `A=0.5` down to `A=0.0005`: one row per path and level, carrying both after-AGC tops, the
deficit and the reference tone's absolute level after the AGC. The top of that
range is where the AGC compresses: at `A=0.5` the two-tone peak before the path filter reaches the
AGC's own `max_input` of 4096 counts, and the reference tone comes out only 4.7-5.4 dB above its
nominal level for 20 dB more input. The bottom is past the AGC's knee: `min_volts` is
`out_target/(var_gain*max_gain)` = 180 counts at the factory settings, about 7 dB below the bench
level, so below `A` about 0.022 the AGC is a fixed 20 dB gain and its output falls 1:1 with the input.
A sweep that only went down would have measured that fixed gain everywhere below `A=0.02` and said
nothing about compression.

Measured over the whole 60 dB, neither top moves by more than 0.002 dB at the table's resolution, and
the deficit by more than 0.001 dB. The stock top's 0.002 dB is row 47, on a -85 dB response, at
-22 dB; the conv top's is the 700 Hz path at -4 dB. Neither grows as the level falls. Rows 53 and
55, the two closest to a crossing, read 1.221 and 0.074 dB at every one of the 31 levels. The set of
rows whose stock top is below the conv top is exactly the eight n/c rows at every level, and no row
changes sign, so no station level makes another row non-comparable. The n/c list itself is the stock
table's linear edge, which no AGC setting can move.

That is what a single gain on the whole audio block should do: it changes the absolute level, not the
ratio between two simultaneous tones, and it does so even while compressing 15 dB across the top half
of the sweep. The weak column stays in the CSV as the -20 dB point of the same grid.

`make -C support/digi chain-levels-check` reruns the sweep and fails on any top or deficit movement
past 0.01 dB, on a sweep that no longer carries all 62 documented paths (ids 4-65), or on any level
whose positive-deficit set is not the eight n/c rows, so the paragraph above is a checked result
rather than a remembered one.

The image ratio is unchanged. The IIRs and the AGC are real; at a given audio frequency they multiply
the wanted and image components by the same real factor, so the ratio cannot improve. The stock rows
that were n/c for shape stay n/c for image; on rows 53-59 the base stock image is still -22 to -33 dB
against -110 dB for the convolution filter. The chain's stock image columns are n/c on the same n/c rows
for the same reason the main table withholds them.

What the operator hears is the same verdict with the level made comfortable. The AGC normalizes the
overall level but cannot restore the missing top: after it, the stock n/c rows are 0.07 to 69 dB below
the conv side at the top. The 5.0 kHz row is almost level-equal at the edge (0.07 dB), but its stock
skirt is 1256 Hz and its image -21.8 dB against 145 Hz and -110.3 dB, which is why it is n/c. The 5.0,
5.5, 6.5 and 7.0 rows have no IIR and are unchanged apart from at most 0.2 dB of AGC movement; the
4.6 and 4.8 rows are 1.2 and 5.7 dB down after the AGC. That ordering holds across the level sweep:
from 20 dB above to 40 dB below the bench level no top or deficit moves by more than 0.002 dB. The
chain CSV is regenerated with
`sh support/digi/convolution_graphs/build-sweep.sh "$PWD" /tmp/pathsweep-chain.csv --chain`.

#### Minimum phase: why the delay column is not 53 ms

The designer makes a windowed sinc, which is linear phase: symmetric about its middle, so everything it
passes comes out half its length late. For 1024 taps at 12 kHz that is 42.6 ms, on top of the block. This
file used to say "the block is what you wait for, not the number of taps", from a measurement that
compared the pipeline with a plain FIR *over the same taps* and so cancelled exactly that half out. Put a
tone through and time it, and the linear phase filter is 52.6 ms late at geometry 256, 25.9 at 128 and
12.6 at 64 - against 8.25 ms for the Hilbert pair.

Nothing about selecting a band needs the symmetry. `convolution_minimum_phase()` turns the designer's
taps into the minimum phase taps with the same magnitude response, at every filter change: the logarithm
of the magnitude, its cepstrum, the cepstrum folded onto positive time, and back. Same passband, same
skirts, same number of taps and masks, and the delay is then set by how narrow the band is rather than
by how many taps describe it - 1.0 ms in the taps for the 2.7 kHz SSB path, 5.0 ms for the 500 Hz CW
path, at all three geometries give or take a millisecond.

What it costs, and what it does not:

* **No RAM.** The transform it works in has to be several times the response, and four times is what
  the filter's own buffers hold - the masks, which are about to be rewritten, and the worker's history,
  which is about to be cleared. So the design runs in those (`design_scratch` in `ConvolutionBuffers`).
  That is only legal because the design closes the seam for its own duration: a worker pass landing in
  the middle of the cepstrum leaves all 4096 mask floats NaN, the band cache then holds them until the
  filter width changes, and the block counters never notice, because a queue of NaN moves exactly as
  fast as a queue of audio. `conv_design_busy` in `audio_convolution.c` is what stops that, and probe
  section 12 tests it by running a worker pass from inside a design - which is what the audio interrupt
  and the worker it pends do to a design on a radio. Until that flag existed the only thing holding the
  invariant was `ads.af_disabled` at the call site, and that flag is a `bool` used as a nesting counter
  (`RadioManagement_SetDemodMode()` takes it, `AudioDriver_SetProcessingChain()` clears it on the way
  out, thirteen lines before the mode change finishes), so the condition was real but load-bearing.
* **Flash, at the default geometry only.** Four times 1024 taps is the 4096-point CMSIS transform,
  whose tables nothing else in the radio links: 41,296 bytes, measured as the difference between a
  minimum phase link (421,017 bytes of text) and `-DCONVOLUTION_LINEAR_PHASE` (379,721). At 128 and 64
  the design needs 1024- and 2048-point transforms, both of which are already linked for the filter
  itself, and those images move 3,504 bytes in text.
* **The stop band is where it is told to be.** The construction needs a floor under the logarithm, and
  the floor is the stop band that comes out. It is set to -110 dB (`CONVOLUTION_MINPHASE_FLOOR`), which
  is what the linear phase taps reach above the band, and at four times the response in float32 that is
  also about as far as the arithmetic is exact: the passband is unchanged to 0.00 dB.
* **The delay is not the same at every frequency.** It is least in the middle of the band and rises
  towards the edges - for the 2.7 kHz path about 1 ms in the middle, 6 ms a hundred Hz inside an edge.
  That is inaudible on voice and CW and is the wrong property for a mode that needs equal delay, none of
  which this filter is handed.
* **All of the ringing comes after the signal.** A filter this steep rings whatever its phase; linear
  phase splits it before and after a keyed edge, minimum phase puts it all after, and through the
  500 Hz path at geometry 256 that is an overshoot of about a fifth that dies away over some 30 ms.
  Whether that is better or worse on the ear than the same ringing ahead of the dit is for a radio
  to say.
* **Time at each filter change, measured on a host and not yet on a radio.** A design is four transforms
  plus a logarithm, an exponential, a sine and a cosine per point, and the audio is held off for the
  whole of it - the RX processor does not run while the processing chain is being rebuilt, so this is
  gap in what the operator hears, not a background cost. Measured on a 2 GHz x86 at -O2: 1.3 ms, or
  1,720,320 butterfly operations against the 33,280 one block of filtering costs, i.e. fifty-two blocks
  of the filter's own transform work in one go. An F407 is single-issue at 168 MHz and takes its `logf`
  and `expf` from software, so expect several times the host figure, which is worth counting with the
  DWT cycle counter the bench already uses before this geometry is chosen by ear. `-DCONVOLUTION_LINEAR_PHASE`
  skips the four big transforms altogether, so a build that must not pause on a filter change has a
  way to avoid it.

`-DCONVOLUTION_LINEAR_PHASE` leaves the taps as the designer makes them, for hearing the difference on
one radio. Section 9 of the probe times a tone through both paths and fails if the taps cost more than
half of what linear phase would (or, built with that switch, if they do not cost exactly that), and
section 10 fails if anything at all comes out of a filter that has just been changed and fed silence -
which is what a design left in the worker's history sounds like, and what the previous filter's last
audio sounded like before the history was cleared at all. Section 11 is the low edge (below) and section
12 is the design-under-interrupt test described in the first bullet above. Section 13 belongs to the
measurement build below: it checks that the fields that build prints count what their names say, which
is the only part of that build a host can see - the timing fields read 0 there, because DWT does not
exist off the chip, and the section says so rather than reporting a zero as a measurement.

#### The low edge, and what geometry 64 does not do

The band is the filter path's own width around its own centre, except that a path whose band reaches
0 Hz gets a low edge (`CONVOLUTION_LOW_CUT_HZ`, 50 Hz) rather than reaching down to DC. It is not
cosmetic: the AGC's own DC removal is switched on for AM and SAM only (`audio_driver.c:559`), so on
USB, LSB and CW - the three modes this filter takes over - there is nothing downstream that takes DC
out. On the 2.7 kHz path the edge holds DC 124 dB down where removing it passes it 6 dB down, within a
factor of two of the audio sitting on top of it.

The wide paths are the limit of what an edge can do, and probe section 11 now says so. They run at the
full 48 kHz, where 50 Hz is a thousandth of the rate, and a response of a few hundred taps cannot make a
wall that narrow:

| | DC, 2.7 kHz path at 12 kHz | DC, worst path at 48 kHz | the Hilbert pair in use, same paths |
|---|---|---|---|
| geometry 256 | -118 dB | -9.5 dB | -9.7 / -0.2 dB |
| geometry 128 | -35 dB | -4.0 dB | -9.7 / -0.2 dB |
| geometry 64 | -19 dB | -1.9 dB | -9.7 / -0.2 dB |

At 256 and 128 the filter is better than the pair it replaces everywhere, which is what the check holds
it to. At 64 it is not: on the narrow paths DC is 9.5 dB down against the pair's 9.7, and on the widest
paths 1.9 dB, so `make -C support/digi convolution-check EXTRA_DEFS=-DFFT_CONVOLUTION_SIZE=64` fails
section 11 and says which paths. That is a real cost of the low-latency geometry rather than a bug to
tune out, and the options if it matters are to raise the edge with the rate (which cuts the bass of wide
SSB), to keep the geometry on the decimated paths only, or to put a DC blocker in front of the filter and
drop the edge.

#### The measurement build: what to flash and what to write down

Everything above is measured on a host. The three numbers that decide whether `USE_CONVOLUTION` stops
being a build option are not host numbers, and no amount of bench work substitutes for them: how much
audio a filter change eats, what a block of filtering costs on the chip, and how close the stack comes to
the 55 KiB of `.bss` the filter just grew. So there is a build whose only job is to answer them,
`-DCONVOLUTION_MEASURE`, which times the pipeline with the DWT cycle counter (`misc/profiling.h`, the
same one the `L nn%` readout uses, already started at boot by `profileTimedEventInit()`), samples the
main stack pointer in all three contexts, and prints it on the debug overlay.

```sh
# the measurement image: the filter, the counters, the peaks, and the timing
make clean && make -j4 all \
  CONFIGFLAGS='-DUI_BRD_MCHF -DRF_BRD_MCHF -DUSE_CONVOLUTION -DCONVOLUTION_MEASURE -DCONVOLUTION_LEVEL_STATS'
# the control, for a radio that is silent on the image above: every counter and the same geometry,
# with USB/LSB/CW routed back to the Hilbert pair the stock firmware runs
make clean && make -j4 all \
  CONFIGFLAGS='-DUI_BRD_MCHF -DRF_BRD_MCHF -DUSE_CONVOLUTION -DCONVOLUTION_OFF_FOR_TEST -DCONVOLUTION_MEASURE -DCONVOLUTION_LEVEL_STATS'
```

What those two commands produce, and what they cost in the image itself:

| build | text | `.bss` | stack headroom at link time |
|---|---|---|---|
| measurement, geometry 256 | 422,753 | 149,524 | 40,748 |
| same with `CONVOLUTION_OFF_FOR_TEST` | 378,265 | 141,212 | 49,056 |
| same with `-DFFT_CONVOLUTION_SIZE=128` | 381,937 | 125,972 | 64,300 |
| same with `-DCONVOLUTION_LINEAR_PHASE` | 381,401 | 149,524 | 40,748 |
| convolution, no measurement | 421,017 | 149,436 | 40,836 |
| as it ships, option off | 376,697 | 93,964 | 96,304 |

The instrumentation is 1,736 bytes of text and 88 of `.bss`, which is why it is a build rather than
always-on: it costs a clock read per call, not per sample, but the RAM has nowhere to come from.

All four measurement images are built and left in the root, so a session does not need a laptop between
steps: `fw-mchf-convmeasure.bin` is the one to start on, `fw-mchf-convofftest.bin` is the control for
step 6, and `fw-mchf-convmeasure128.bin` / `fw-mchf-convmeasurelinear.bin` are the two answers to a bad
`G` reading - half the block delay, and no cepstrum at all. Built with the commands above, at geometry
256 unless the name says otherwise. Each has a `.dfu`, `.elf` and `.map` beside it; the `.elf` is for
reading the same fields from a debugger, where they are plain globals rather than a rotating line.

**The images land in the repository root, not in `build/`.** `fw-mchf.elf`/`.bin`/`.dfu`/`.map` are what
the last link wrote; `build/fw-f4/` holds images from whatever release build was last run there and is
not refreshed by `make all` - which is how a table in this file quoted 96,408 bytes of headroom for
weeks when the current link gave 96,304, and how a size read from `build/fw-f4/fw-mchf.elf` reported a
binary from November 18 as though it had just been built. Check the timestamp on the file you measure.
The named copies above survive the next rebuild; `fw-*` is gitignored, so nothing of them reaches the
repository, and the tree itself is left built as it ships (option off).

Readout: Debug menu, *Debug info on LCD* (`MENU_DEBUG_ENABLE_INFO`) - the same switch that brings up the
`L nn% P nnn%` load line and the encoder lines. Under it the convolution line cycles, about a second
per reading, in this order.

**Flashing the `.bin`.** `dfu-util -a 0 -s 0x08010000:leave -D fw-mchf-convmeasure.bin`, radio in DFU mode
(hold Band+ through power-on until "STM Device in DFU Mode" answers, and keep it held until the upload
finishes - `useful_manuals/bootloader_doc.md`:234). The address is not decoration: the F4 links the
firmware at `rom ORIGIN = 0x08010000`, because the bootloader owns the first 64 KiB, and
`objcopy -O binary` throws addresses away, so a raw `.bin` does not say where it goes - you have to. The
first two words of `fw-mchf-convmeasure.bin` are `0x2001FFF8` and a reset handler of `0x08010211`, which
is the same thing said from the image itself. A `.bin` sent to `0x08000000` overwrites the bootloader
instead of the firmware, which is a much worse day than a bad radio image. The `.dfu` files are the
prefixed form (`hex2dfu` from the `.hex`, which does carry addresses) and are what DfuSeDemo wants - the
Windows doc says so in as many words at `bootloader_doc.md`:219 - so use `.bin` with `dfu-util` and the
address, or `.dfu` with DfuSeDemo and none.

| line | what it is | the number that matters |
|---|---|---|
| `C999/999 d99 n99 b9` | blocks in/out since the last refresh, drops, unsized slices, slices refused in a design | `d` must stay 0 while you listen |
| `P i812 o799` | loudest sample in and out, x1000 | `o0` with `i` alive is the silent-filter case |
| `G10700/10700 us f2` | the hole one change makes, queue flush to the first sample back: last / worst since reset, and frames the design itself refused | see the thresholds below |
| `W123 I12 O13` | worker pass average, worst-case audio-interrupt slice in and out | `W` against 667 us |
| `M999 K99 3/5` | where a design's time went (cepstrum / partitions), designs run over setups called | `3/5` says the cache works |
| `U1234 F5678` | main stack bytes used at its deepest, bytes clear of `.bss` (`F!n`: n bytes below it) | `F` is the margin the linker cannot give |

The fields cap at four to six digits, so a capped field means "at least that": `G99999` is a fail, not a
measurement, and `F!` is the stack having crossed into `.bss`, not a margin.

**The session, in the order that wastes the fewest radio-hours.** Write down the mode and filter width
beside every number; a filter change on a 250 Hz CW path and one on the 2.7 kHz SSB path are different
codes running at different rates, and the averages of the two are meaningless.

1. Baseline first, on the image as it ships (option off): note `L`/`P` in USB on a 2.7 kHz filter and in
   CW on 500 Hz. Every load figure below is against these two numbers, not against the design note's
   "20 % of an STM32F7".
2. Flash `fw-mchf-convmeasure.bin`. Same two settings. `L`/`P` now include the filter; `W`, `I`, `O` say
   where the difference came from. A worker pass above 667 us means blocks cannot keep up and `d` starts
   climbing - which is the failure, not the number.
3. Filter changes, one at a time, watching `G`: width 2.7k to 500 to 200, offset changes, USB to LSB,
   and one CAT-driven frequency step that crosses a filter path. Each is one design or several; the
   second field of `G` is the worst of the session and `f` is the frames the filter refused for the last
   one. Then listen: change a width on a weak station and on a strong one, and say out loud whether you
   hear a tick, a pause, or nothing.
4. `M`/`K` for the same changes, which decides the fix if `G` is bad: if `M` dominates, the 4096-point
   cepstrum is the cost and `-DCONVOLUTION_LINEAR_PHASE` removes it; if `K` dominates, it is the
   partitions, and the fix is the smaller geometry or fewer of them.
5. `U`/`F` after ten minutes of tuning, TX/RX swapping, and a waterfall page - the deepest the radio has
   been, sampled in the interrupt, the worker and the design.
6. If the audio is wrong or missing at all, flash `fw-mchf-convofftest.bin` without touching anything
   else. Same counters, same geometry, same debug line; the demodulation goes back to the pair. Audible
   on this image and silent on the measurement image puts the fault inside the filter, and the `P i/o`
   and `C` lines say which half of it.
7. Ten minutes of unattended listening on a real signal, then read `d` and `n`. Anything other than 0 is
   a queue that is not being served, in a case the bench cannot reach because it drives the seam by hand.

**Thresholds, with the arithmetic that gives them.** The IQ interrupt runs at 1500 Hz, so one frame of
audio is 667 us and `f1` is the most the design can cost and still be a pause rather than a click. `G`
also carries the queue flush and the refill of the first block, so its floor is one block of buffering
whatever the design did - 10.7 ms at 12 kHz on a decimated path, 2.7 ms at 48 kHz on a full-rate one.

| reading | acceptable | what to do if it is not |
|---|---|---|
| `G` last, `f` | one block + <= 667 us (about 11.4 ms at 12 kHz, 3.4 ms at 48 kHz), `f` <= 1 | `-DCONVOLUTION_LINEAR_PHASE` (removes `M`), or a smaller geometry (removes most of `M` and shrinks `K`), or move the design out of the held-off window into the worker a partition at a time |
| `G` | one block + 700-3000 us, `f` 2-4 | a soft tick on every filter change. Usable, and worth deciding on by ear rather than by table: some operators will never notice, and a CW operator stepping width constantly will |
| `W` | < 667 us | the worker cannot keep up. Fewer partitions, which is the smaller geometry, at the DC cost section 11 charges you |
| `L`, `P` | the pair's figures plus what you are willing to pay | the option is not free; if it eats the headroom the noise blanker needs, that is the answer, and `d` climbing on a noisy band is the same answer twice |
| `F` | > 4000 bytes | do not run that configuration. 4 kB is not a derived limit - it is the size of the unknown: the known heap asks are a 12-byte canary and the USB class handles, and the nesting the sampler does not see (a PendSV preempted by the audio ISR) is one more frame per level. Below that, the smaller geometry, or `FFT_CONVOLUTION_SIZE=128`, which is 23.5 KiB better on the link |
| `d`, `n` after step 7 | 0 | a leak in the queues, which is what section 3's counters were added to find; take `C` and `P` off the radio and go back to the bench |

On a 2 GHz x86 at -O2 the design alone is 1.3 ms with `f` 2, the cepstrum (four transforms, one of them
4096-point) the bulk of it; `G` on the radio is that plus a block of buffering. If the F407 is two to
eight times the host on this code - float `logf`/`expf` from software, single-issue at 168 MHz - the
design is 3 to 10 ms and 5 to 16 frames, which is a click. That range is the whole reason this build exists rather than a guess about which end it lands on.

One thing the build cannot tell you: whether 10.7 ms of block delay is acceptable to hear. That is not a
counter, and the answer changes between SSB and CW, so it has to be listened to on the same radio, with
`-DFFT_CONVOLUTION_SIZE=128` (5.7 ms) built the same way for comparison.

Probe section 6 is the one place the control build is checked rather than trusted: with
`CONVOLUTION_OFF_FOR_TEST` defined it stops asking "only the measured modes are replaced" and asserts
the opposite - that no mode is handed to the filter - because an image that claims to be the control and
still replaces USB would make "audible here, silent there" mean nothing. Running the probe with that
switch is therefore meaningful, and `make -C support/digi convolution-check
EXTRA_DEFS='-DCONVOLUTION_MEASURE -DCONVOLUTION_OFF_FOR_TEST'` is the measurement build's control: 19
checks, of which the one that changed personality is the point.

`make build-config-check` and the probe both run the committed geometry. The `EXTRA_DEFS` line above is
what the harness was made to accept, and it keeps a stamp of the flags it was built with so that a
measurement cannot come from an object compiled against a different one. Read the -140 dB figures with care: the harness computes in float32, so
past roughly -140 dB you are seeing the arithmetic, not a bench measurement of a filter. The 88 dB
versus 110 dB gap is real.

The RAM is the reason the option stays off, and what it really costs is stack headroom. Linking with it
moves the ELF's `.bss` from 93,964 to 125,884 bytes at `FFT_CONVOLUTION_SIZE=128` (`_ebss` 0x200087c8 to
0x20010474), so what is left between the end of `.bss` and the stack top (0x2001fff8) falls from 96,304
to 64,388 bytes - twelve KiB better than the figures this paragraph carried until the impulse array was
made to follow the block size instead of a fixed 2048 taps. Two earlier versions of these numbers were
wrong in a way worth keeping: one said 93,860 to 157,276 with a 32,800 residue, and 93,860 is `.bss`
*plus* `.ccm` - the F407 has two RAM regions, so `arm-none-eabi-size`'s total column adds them and is not
a number to budget with. Nothing bounds that space: `_sbrk()`
comes from libnosys, bumps a pointer up from `_end` and has no ceiling to compare against, and the
`ASSERT(__StackLimit >= __HeapLimit)` in `linker/arm-gcc-link_f4_main.ld` compares the heap's *start*
with the top of RAM, so it cannot fire however full the radio is. (The waterfall bitmap, which sits in
CCM, is not one of the callers: the `malloc()` at `ui_spectrum.c:2128` is inside a `/* */` block and the
function around it is never called from anywhere. This file said otherwise until it was checked.) What
the heap is asked for in this configuration is the 12 byte canary word in `uhsdr_canary.c` and the USB
class handles, so 64,388 bytes is in practice the main stack's room at geometry 128, down from 96,304.
At the committed geometry 256 it is 40,836.

What would have to happen before this could be switched on:

* the arrays have to come down, or the RAM they compete with has to be found. That is now a build
  choice rather than an edit - see the geometry table below - and part of it has already been taken:
  the impulse array now follows the block size instead of being sized for 2048 taps at every geometry,
  which is 8 KiB back at 256 and 12 KiB at 128 and 64. What is left is sized by partitions times block,
  so a short block with few partitions costs little selectivity when the stop band is already 110 dB
  deep - but see the low edge above before choosing the shortest one.
* the block latency has to be listened to, and it depends on the filter path: the block is a fixed 128
  samples, so it is 2.7 ms of audio at 48 kHz and 10.7 ms at the decimated 12 kHz the narrow SSB
  filters use. The AGC, the notch, the post-demod filters and the audio output all still run on the
  maintained side and receive samples that much later, which affects sidetone and any changeover.
  That needs a radio, not a harness.
* which modes are replaced has to be, and was, got wrong. `CONVOLUTION_USED_FOR()` is now a positive
  list - `DEMOD_USB`, `DEMOD_LSB`, `DEMOD_CW` - because written as an exclusion list (not AM, not SAM,
  not FM, not the two-channel modes) it also matched `DEMOD_DIGI`, the mode that carries RTTY and BPSK,
  whose demodulators in `rtty.c` and `psk.c` are fed from the audio this filter replaces. The 2018 branch
  knew: the worker's own last line is "TODO: deal with Digimodes and FM". Section 6 of the probe checks
  the table mode by mode now, which caught the exclusion-list version as soon as it was put back.
* what the radio does have for digital modes, since it is easy to think otherwise: RTTY and BPSK
  (`DEMOD_DIGI`, `USE_RTTY_PROCESSOR` is on in the shipping configuration) and the CW decoder. The FT8
  library under `drivers/digi/ft8_lib` is not compiled into the image - nothing outside that directory
  includes it, and the only `ft8` symbols in a linked firmware belong to the FT817 CAT protocol - and
  NAVTEX, Hellschreiber, MFSK16, Olivia and weather fax exist here only as reference modems that run on
  a host, which is what this directory is for.
* `nfor` is clamped to the arrays instead of asserted, so a wider response than 8 partitions x 128
  samples silently gets fewer taps.

One more thing the harness checks, because getting it wrong is not subtle: a design above zero has to
select the sideband the `i+q` sum selects, and a design below zero the one `i-q` selects, measured by
which side of zero each path puts its band energy on. `AudioDriver_RxProcessor()` and
`AudioDriver_SetProcessingChain()` are written to agree with that pair. This check is only as good as
its model of the `i+q` path, and the model has to use `arm_fir_f32()`'s tap order - see 7 above for what
it passed while it did not.

Re-measure with `make -C support/digi convolution-check`. Two of its checks are load bearing and were
each demonstrated to fail: the partitioned pipeline has to reproduce a plain complex FIR over the same
taps, run both ways round the interrupt boundary (8 samples a slice at the decimated rate, 32 at the
full rate, because `IQ_BLOCK_SIZE` is 48000/1500 and neither is a block), and that catches 5 and 6
above; and the sideband separation has to be at least what the Hilbert pair gives, where 12.3 dB was
the failing number. It compiles `audio_convolution.c` as text
because the mask and history arrays are file static, and it needs no radio and no ARM toolchain.
The FFT the host build gives it is `convolution_fft.c`, which is not trusted: the FIR comparison in
check 2 uses no transform at all, so if that transform were wrong the two would disagree.

CI: the `digi-bench` job in `.github/workflows/config-upgrade-check.yml` runs twenty steps - the
filter design check (`support/digi/design_rtty_filters.py`), `make digi-bench` (the self checks),
the RTTY shift, SNR and reference error character error rate matrix, that matrix again with the long
message, the negative control that tells the receiver the other symbol speed so the table has to
fail (`break-rx`), the shift setting curve, the tuning error sweep, the recorded DDK2 fixture and
the per-character check of that take (`make -C support/digi rtty-ddk2` and `fixture-align`), the
two-path sweep (`rtty-multipath`), the transmitter's keying through the gate (`tx-check`), the four
reference modems: NAVTEX, Hellschreiber, MFSK16 and Olivia, and the recording paths for the four of
them through a WAV with a lead (`record-check`), the capture decoder's self test, a built
conformant stream that has to walk, ten streams that have to be refused and a recorder's answer that
has to agree with the stream it came from (`oracle-check`, which needs no WebSDR client), the gate
run on the pinned take and on the fixture that take is quoted from (`capture-check` and
`fixture-check`), the shaped 2.5 kHz floors against the readings this file documents
(`floors-2k5-check`), and the convolution RX filter against the SSB stage in use
(`convolution-check`, which measures the filter, checks the mode table, sweeps the passband, puts a
tone through every SSB and CW filter path, times the audio delay and checks that a filter change is
silent).
`make -C support/digi wefax` and `floors-2k5` are targets of their own and
are not among the twenty. `make digi-bench` is also available on its own; it is
one of the twenty, not the whole job.

### The hole that made the convolution images silent, and what the probe could not see

`USE_CONVOLUTION` used to compile the RX interpolation setup out entirely:

```c
#ifdef USE_CONVOLUTION
// TODO: insert interpolation filter settings for convolution filter HERE
#else
    for (int chan = 0; chan < NUM_AUDIO_CHANNELS; chan++) { arm_fir_interpolate_init_f32(...); }
#endif
```

Whenever the feature was built in, `INTERPOLATE_RX[]` was never initialized - for any mode. The runtime
guard `if (INTERPOLATE_RX[0].phaseLength > 0)` then skipped the stage, and the convolution filter
replaces the *sideband-selecting* FIR, not the output interpolation: audio filtered at the decimated
rate still has to be raised back to the DAC rate here. It was not, so the buffer the DAC reads was only
partly written and USB/LSB/CW were silent.

The symptom set came from that one cause with no filter fault in it. FM works because its path does not
use this stage. The waterfall and the S-meter work because they are computed before demodulation. The
counters reported `in == out`, nothing dropped and real level on both sides of the filter, because all
of it was true - the samples just never reached the speaker.

`static` state is also why the reports disagreed with each other. `phaseLength` survives a mode change,
so a radio that had been sitting on a path that did initialize it could give SSB working audio until the
next reboot, while one flashed and powered up cold could not. Geometry looked like the variable because
each geometry was tested in its own power cycle, and the throughput, queue, coefficient-count and
transform-instance theories were all built on a difference that was really cold `.bss`.

The setup now runs unconditionally. Images `fwv-2.12.2c128f` and `fwv-2.12.2c256f` carry the fix.

What this says about the probe is worth keeping too: `convolution_probe` drives the filter and measures
its output directly. It never calls `AudioDriver_SetProcessingChain()` and never touches
`INTERPOLATE_RX`, so no figure it prints - passband, skirt, image rejection, cost per sample - could
have shown this. "Measured off the radio, therefore the radio is fine" is the exact shape of mistake
this bug rewarded, and it is the second time in one week that a host result was quoted about something
the host does not exercise.
### What this filter is, measured against the design note that proposed it

Upstream keeps a page for exactly this idea - `df8oe/UHSDR` wiki, *Low-latency filtering: Partitioned
Convolution*, by DD4WH - and reading it against `audio_convolution.c` says plainly what we have and what
we do not.

The note's own numbers: 1024 complex coefficients partitioned into **eight blocks**, `size = 128`,
`FFT_size = 256`, decimation 4 from 48 ksps, latency `(size * DF) / SR` = **10.67 ms**, about 27,136
multiplies and 18,944 adds per 128 new samples, which the note costs at **20 % of a 216 MHz STM32F7**,
and about 40 KB of memory. The latency formula is the one the bench probe uses too (`FFT/2` over the
IQ rate - 48000/4 = 12000, so 256 gives 10.67 ms identically), which is why the two sets of numbers line
up at all.

**What we now compile is the note's scheme, not a special case of it.** `CONVOLUTION_MAX_NO_OF_BLOCKS`
is 8 and the response is 1024 taps at the default geometry, which is what the note describes; the
one-block version this section used to document was the state of the branch before the partition loop,
the mask cache and the input history were fixed, and it could not have worked - the count of partitions
was the reason the tap count was capped at `FFT/2`, so the filter had 128 taps and a 127 Hz skirt at
10.7 ms rather than 34 Hz at the same 10.7 ms. Two things the note says are still worth repeating,
because they are the two questions people ask this filter:

* **Partitioning is what buys selectivity at a fixed latency, and it is not free.** Eight blocks of 128
  samples reach 1024 taps at the same 10.67 ms that a single block of 128 would cost, but every input
  block has to be multiplied against every partition, so the arithmetic per sample grows with the
  number of partitions rather than falling: measured, 356 ops per audio sample at eight partitions
  against 324 at a shorter block (`-DFFT_CONVOLUTION_SIZE=128`, 512 taps, 5.7 ms of delay). It is a
  latency-per-selectivity trade, not a CPU trick, and it does nothing for the question "the F4 seems not
  powerful enough for c256" - the answer to that is the smaller geometry, or the load readout on the
  radio.
* **What the eight blocks are worth against the receiver as it ships is measured, not quoted.** The
  tables above are from this tree, running the shipped module: 34 Hz of skirt where the Hilbert pair
  gives 127 Hz (and its wall is at 3.5 kHz, not at the filter width), 110 dB of opposite sideband where
  the pair gives 75.8 dB worst-case in band, and 356 arithmetic operations per sample where the pair and
  its `i+q` sum bill 796. The note's 20 % of an STM32F7 belongs to the note; whether c256 fits a 168 MHz
  F407 is what `L nn%` and `P nnn%` on the debug line are for, and no radio has been asked yet.

The cost the note does not carry, because its geometry is the one it chose, is DC: see "The low edge, and
what geometry 64 does not do" above. At 1024 taps on a 12 kHz path the 50 Hz edge holds DC 118 dB down;
at 256 taps on a 48 kHz path it holds it 2 dB down, which is why the harness now fails that geometry on
those paths rather than recommending it as a free way to buy latency.

### Why the noise blanker is a waterfall problem and not an audio problem

The default blanker (`USE_OBSOLETE_NOISEBLANKER 0`) is DL2FW's linear-prediction design, described on the
upstream *Noise blanker* page and visible at `audio_nr.c:2330-2493`: autocorrelate a 128-sample frame,
solve Levinson-Durbin for ten LPCs, run the inverse FIR to strip speech, run the matched FIR to find
impulses, threshold at `2.5 * sqrt(sigma2 * LPC_power)`, then reconstruct each impulse from forward and
backward predictions. It is not a peak detector - the peak detector is the *obsolete* branch, and saying
otherwise is a mistake worth correcting because it inverts the cost: the LPC path is worth tens of
thousands of multiply-accumulates per frame, which is what the debug line's `NB nnnus` was measuring at
90-120 microseconds.

Two things follow, and both are about where it runs rather than what it computes:

* `AudioNr_HandleNoiseReduction` is called from `UiDriver_TaskHandler_HighPrioTasks()`, the **main loop**,
  which is the same loop that draws the waterfall. Its guard is `is_dsp_nb_active() || is_dsp_nr()`, so
  **switching on the blanker switches on the whole spectral NR chain as well** - 256-point FFTs, sqrt-Hann
  windows, overlap-add - and does so whether or not the operator asked for noise reduction.
* That chain's own latency is 21.3 ms at 48 ksps (256 x 4 / 48000, stated on the *Noise reduction* page),
  which is more than double c256's 10.67 ms. Anyone judging the filter's keying feel with NR or NB on is
  measuring the NR, not the filter.

So "c256 with NB is glitchy in the waterfall" is not the filter starving the audio ISR. It is main-loop
work: a 100-microsecond block in the loop that also owns the display, arriving in bursts, exactly when
impulses do. The fix is measurement and placement, not a cheaper blanker - and the A/B between c128 and
stock should be run with both NB and NR off, or the comparison is not of the filters.

## The streaming SITOR-B receiver, rewritten around what the mode actually gives away

`drivers/digi/sitor_rx.c` is now a different receiver from the one first committed here, and the
difference is which property of the mode it trusts. The first version *measured* the bit period from
the autocorrelation of the tone transitions. That is a clever-sounding thing to do, and it is the
reason the receiver would not lock at 20 dB of noise where the reference modem in `navtex.c` decodes
without a single character error: transitions are sparse after the 50 Hz bandpass, and their spacing
carries much less evidence than the alphabet does. The rewrite takes the clock from the objective
function the mode itself supplies.

What is now used, in the order of strength:

1. **The 4-of-7 share is the synchroniser.** A grid of 1080 candidate clocks - every integer period
   from 116 to 124 samples at 12 kHz, every phase within each - is fed one bit per its own period,
   and each candidate keeps an aged 4-of-7 share for each of the seven group boundaries. Random bits
   are 4-of-7 35/128 = 27% of the time, so noise cannot score well and the number is simultaneously
   the lock criterion, the confidence figure and the mode test. It holds down to 10 dB, where the
   transition method produced nothing at all.
2. **The phasing preamble confirms, and does not acquire.** Twenty alternating SIA/SIB characters are
   a 14-bit pattern repeated ten times, so a candidate sampling in the middle of a bit can chain six
   near-matches at Hamming distance two and win everything it is asked. The first version of this let
   the phasing pick the clock; it locked with total confidence on the wrong candidate and printed
   garbage at 0.90. The phasing history is now consulted *only* on the candidate the alphabet picked,
   and it answers the two questions the alphabet provably cannot: the bit order (SIA/SIB reversed are
   0x78/0x66, which are not the phasing pair, so the order is detectable) and where the blocks of ten
   start.
3. **A character is emitted once, on its repeat copy, and only in the second half of a block.** Slot
   *j* holds the same character as *j−5* only for the five residues after the block start; for the
   five before it, *j−5* reaches into the previous block and is a different character. Pairing
   unconditionally therefore mixes two characters and prints its neighbour's - which is what this
   receiver did, producing `UHDDR` for `UHSDR` at a perfect 0/784 bit error rate. A receiver that
   joined mid-message does not know where the block began, so the residues where its own two copies
   agree are used to find out: five consecutive classes hot and five cold.

Five bugs stood between the first version and a passing table, and each is worth naming because none
of them was exotic:

- `grid_reset()` cleared the counter that ties a candidate index to an absolute sample. After the
  first unlock, "phase 119" no longer meant "the last sample of a bit", every candidate was fed at an
  arbitrary point inside the bit, and the receiver printed deterministic garbage at 0.97 confidence.
- "a character ends here" was tested on a bit counter that only moves once per period, so it stayed
  true for the other 119 samples and each character was stamped about seventeen times.
- The phasing chain was cleared by every non-matching bit. Pair matches are seven bits apart, so
  between them there are six non-matches, and the run could therefore never exceed 1: the strongest
  evidence in the mode was thrown away by the code that was supposed to count it.
- The run length demanded a stride of fourteen bits when word boundaries are seven apart, so it
  broke on every second match even before the above.
- The shift state was zeroed, which is *figures*, so the whole message was looked up in the wrong half
  of the alphabet.

### What it measures, and what it costs

`make -C support/digi sitor-stream` grades the receiver on two numbers, because character error rate
alone conflates them: **wrong** counts characters printed that are not what was sent (a defect), and
**CER** counts those plus the first ~14 characters it cannot print because it was still acquiring
(the price of streaming).

| | result |
|---|---|
| clean, 20 dB, 10 dB, 10 dB + QSB, ±50 Hz tuning | **0 wrong characters**, conf 1.00, clock 10.0 ms |
| reversed bit order | 0 wrong, order detected from the phasing |
| noise only, nothing transmitted | stays unlocked, conf 0.00 - it refuses rather than prints |
| burst over one copy | 0 wrong; the repeat carries it |
| bit rate 1.5% off | prints up to 10 / 6 wrong characters, pinned in CI as a ceiling on a known defect |
| bit rate 3% off | refuses: the integer grid cannot follow it |

For the radio: it compiles for ARM and is in `files.mak`, and because nothing instantiates it the
linker drops it - the shipping image's `.bss` is unchanged at 32,352 bytes and its symbols do not
appear in the map at all.  (That figure used to read 93,860 here, which is `.bss` plus `.ccm`: this
target has two RAM regions and the size tool's total column adds them, so it is not the number to
budget a 128 KB part with.) A NAVTEX mode would therefore cost **32,316 bytes of RAM** for one instance
(the grid's registers, aged scores and phasing history), which is the same order as the 128-point
convolution geometry and fits in the headroom the stock build leaves. It must run on the decimated
audio path: at 48 kHz the same percentage span would need 15,840 candidates, so the grid narrows
itself to ±0.2% and says so in the source, which is another way of saying the mode belongs at 12 kHz.

### Why the head of a message is lost, and why an earlier lock did not fix it

The table spends 14 of 46 characters on acquisition, and the obvious repair - let a candidate that is
decisively ahead of the field win after 60 bits instead of 150 - does not work.  It was implemented,
measured, found to change nothing, and reverted rather than left in as code that advertises a capability
it does not have.

The reason came from a temporary print in `grid_decide`, on the clean row:

| decision | time | winner | its score | runner-up | best far-away clock |
|---|---|---|---|---|---|
| 1 | 0.40 s | none | 0 | 0 | 0 |
| 2 | 0.80 s | 470 | 68 | 68 | 68 |
| 3 | 1.20 s | 470 | 89 | 89 | 89 |
| 4 | 1.60 s | 470 | 102 | 102 | **104** |
| 5 | 2.00 s | 470 | 113 | 113 | 110 |

The scores are tied rather than separated: they start near the middle of their range and age toward the
truth, so at 0.8 s a candidate two samples of period away is exactly as convinced as the winner and there
is no margin to test.  What breaks the tie at decision 4 is the arithmetic bias toward the nominal period,
not evidence from the signal - and the row where a *different* clock scores higher than the winner (104
against 102) is the clearest statement of that: the bias is choosing, and a lock that fired earlier would
be choosing on nothing.

It is the same limitation the rate rows show from the other side.  The grid step at 12 kHz is one sample of
a 120-sample bit, 0.83 per cent, so a transmitter 1.5 per cent off is two steps away and cannot be tracked:
those rows print nothing useful, and the `bit ms` column reads 10.0 on them because an integer grid has
nothing else to offer.  One component would move both numbers: a fractional clock that tracks - transition
intervals measured, a non-integer period held, character boundaries following it - which would leave the
alphabet to choose between seven group boundaries instead of 1080 candidate clocks.  That is a redesign of
the tracking stage rather than a threshold to tune, so it is the next piece of work here rather than a row
in this table.

## NAVTEX in the radio, `USE_NAVTEX`

The streaming receiver described above is now a mode of the radio rather than only a host program.
It is selectable as `NAVX` in the digital-mode list alongside `RTTY` and `BPSK`, is fed from the same
12 kHz decimated stream the RTTY modem uses (inside the same `sampleRateDecim == 12000` guard), and
prints received characters through the same routine RTTY and BPSK use.  Nothing in the audio or UI
path is new, which is why the change is small; what is new is that it is a build option, off by
default, because of where its RAM goes.

`sizeof(SitorRx)` is 28,296 bytes at the default grid and almost all of it is the grid of candidate bit
clocks - the thing that lets the receiver lock down to 10 dB of noise without being told the text.  That
grid lives in the same 128 KB as the convolution filter's buffers.  Stack headroom, read from the
linker's own `_ebss` against `_estack`; every row re-measured on a fresh link after the impulse array was
made to follow the block size, which is 8 KiB back at geometry 256 and 12 KiB at 128 and 64 (the row for
convolution 256 with NAVTEX was 3,616 bytes before that change, which is the same eight KiB):

| build | headroom | verdict |
|---|---|---|
| as it ships today, neither option | 96,304 bytes | - |
| `USE_NAVTEX` alone, grid at its default width | 67,144 | fits |
| `USE_NAVTEX` alone, `-DSITOR_GRID_SPAN=1` | 84,648 | fits, roomiest mode build |
| convolution `FFT_CONVOLUTION_SIZE=128`, no NAVTEX | 64,388 | fits |
| convolution `FFT_CONVOLUTION_SIZE=256`, no NAVTEX | 40,836 | fits |
| convolution 128 **plus** `USE_NAVTEX` | 35,228 | fits |
| convolution **256** plus `USE_NAVTEX` | **11,676** | links, and still the tightest combination here |
| convolution **256** plus `USE_NAVTEX -DSITOR_GRID_SPAN=1` | 29,180 | fits |

Two corrections, both of them this file believing a measurement it had stopped checking.

The table above this commit said convolution 128 plus NAVTEX left **136 bytes** and that 256 plus the
narrow grid left 184, and it is those two rows that got the flash pack's `VERSIONS.txt` to reject
`stick-c256nav`.  They were transposed: 136 was `USE_NAVTEX` with **256** at the wide span, and the
combination described as hopeless - 256 with the narrow grid - actually leaves 21,120 bytes.  The image
that was turned down is built and in the pack now.

The other correction is the reason it was mis-measured at all.  `SITOR_GRID_SPAN` was documented as the
thing that decides whether the two features fit together - span 4 for 32,316 bytes, span 1 "for about a
tenth of that" - and it did not do it.  `SITOR_MAX_GRID` was a hard 1300 that no span could change, and
every per-candidate array (`win14`, `seen`, `score`, `margin`, the phasing history) was sized by that
constant, so narrowing the grid moved only `base[]` and `ccur[]`: a span-1 build saved **48 bytes**, and
the radio showed 19,056 of headroom at span 4 against 19,104 at span 1.  The bound is derived from the
knob now - `(2*SPAN+1) * (nominal period + SPAN + 1)` - which makes span 1 10,792 bytes instead of 28,296
and gives back the 17.5 KB the comment had been promising.  A span-1 run of the table decodes identically
at the default span on every row that matters (0 wrong characters on clean, 20 dB, 10 dB, QSB and tuning;
the two rate rows keep the defects they are pinned for), so the narrow grid is cheaper and not worse under
what has been measured, and the CI table still runs the wide span so that what is graded is what the
default build ships.

`SITOR_GRID_NOMINAL` is the other half of that bound: the arrays are sized for the 120-sample bit period
that 100 Bd at 12 kHz produces, which is the only rate the radio feeds this receiver.  Initialising it at
48 kHz wants 480-sample bits, so the grid is cut at the bound and says so through `SitorRx_GridClipped()`
rather than passing a truncated search off as the one that was measured.

With neither convolution nor NAVTEX the image grows by 8 bytes of flash, the `NAVX` label and the
enum entry, and no RAM.  The enum entry is appended rather than inserted because `ts.digital_mode`
is stored in the EEPROM image.

### What it still lacks, in order of how much it matters when actually copying a station

1. **The text line is shared.**  Received characters go into the one 44-character strip that system
   messages also use, so changing a menu item or a mode erases the middle of a transmission.  The
   radio has no second text area: `UiDriver_TextMsgPutChar` writes a scrolling buffer whose size comes
   from the layout, and on the 160x100 board that strip is a single row at y=92.  Anything better is a
   UI decision, not a DSP one - retain the message in its own buffer and page it, or give NAVTEX a
   block of screen and accept that it borrows the spectrum's space.
2. **No lock or confidence indication.**  The receiver knows, and prints it in the bench table, and
   none of it reaches the screen.  This is the number that separates a station from noise, and without
   it the operator learns to wait for letters to appear.
3. **The encoders still say RTTY.**  `DEMOD_DIGI`'s tuning and shift knobs are labelled and stored for
   RTTY; NAVTEX ignores them, since its tone pair and rate are fixed by the mode.
4. **Nothing is saved.**  A NAVTEX message that scrolls off the strip is gone; there is no log.
5. Rate error between 1 and 3 % is refused rather than followed, which the wide grid narrows but does
   not close: fractional clock recovery or resampling is the fix, and the two pinned ceiling rows in
   the table above are the measurement of that gap.
