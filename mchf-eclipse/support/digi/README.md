# Digital mode host bench

Grades the radio's own modems on a PC. Nothing in this directory is compiled into the firmware.

```
make -C support/digi test     # self checks, non zero exit on regression   (also: make digi-bench)
make -C support/digi rtty     # RTTY character error rate, modem looped back on itself
make -C support/digi rtty-long  # the same matrix, eight copies of the message (one character 0.26 %)
make -C support/digi wefax    # synthetic HF weather fax through the reference demodulator
make -C support/digi rtty-ddk2  # the one on air case: a real DDK2 recording off the NA5B WebSDR
make -C support/digi capture-check  # is a recording fit to be pinned: pair, bit clock, continuity
make -C support/digi fixture-check  # do fixtures/MANIFEST.md's numbers still describe the wav
./digi_bench rtty-tune     # how much tuning error the demodulator survives, in Hz and ppm
./digi_bench capture-check <file.wav> [shift] [45|50 baud]
./digi_bench rtty-wav <file.wav> [shift] ["expected text"] [45|50 baud] [max CER %]
                            # decode a recorded file with the firmware demodulator; the optional
                            # CER limit turns a pinned recording into a graded case
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
  clock     : period 20.000 ms (357 transitions), scatter 8.8 % of a bit
  dropouts  : 21 gap(s) below 15 % of the median, longest 2 ms (0.2 % of the take)
  ACCEPT    : the pair, the bit clock and the continuity are all inside the gate
```

The four numbers, and why each one is measured the way it is:

- **tones** are refined on 5 ms windows that hold one tone only, so a dial offset reports as an
offset instead of a pair the modem cannot filter. 5 ms matters: at 20 ms a straddling bit reads as
both tones at once and any pair looks well behaved.
- **bimodality** is the fraction of those 5 ms windows holding one tone or both. A two tone signal
being switched reads about 0.86 single tone and 0.14 both, the both being transitions; a single
carrier or a stretch of noise reads near 0.0 single and 1.0 both, whatever its spectrum looks like.
- **clock** fits the bit period to the times the signal changes tone, then measures how far those
transitions scatter off that grid, as a percentage of a bit. Each transition is the zero crossing of
the tone discriminator, interpolated between the last 5 ms window that settled on the old tone and
the first that settled on the new one: the window centre trails the change and folds that smear into
every reading. A Baudot character is 7.5 bits long - one start, five data, 1.5 stop - so the
transitions at a character boundary sit half a bit away from the ones inside it. The period is
fitted on the squared phase, which cancels that half bit and leaves the bit itself, and the two
phases come back as two centres that the scatter is measured against. Without the split the fixture
fitted to 18.75 ms - 6 % short of the rate the modem decodes it at - with 13.9 % scatter, and that
shape looked like a recorder rate error; with it the same audio reads 20.000 ms and 8.8 %, so the
take's 2.7 % CER is signal quality, not a stretched time base. A clean synthetic take measures
20.000 ms and 0.1 %, and a 45.45 baud take measures 22.000 ms with 2.2 % and is refused on the
period alone, though its pair and its scatter look perfect. The fit searches 30 % either side of
the nominal period rather than at it, so a take at the wrong rate reports the rate it carries: a
40 baud take reads 25.00 ms and 40.0 baud, and the rejection line names that beside the period.
- **dropouts** are 1 ms slots below 15 % of the median level, reported as a count and the longest
run. Trimming the ends of a take does not remove a stall in the middle of one.

The limits are set just outside the fixture's own numbers (single tone above 0.55, both below 0.35,
period within 5 %, scatter under 12 % of a bit, no gap over 5 ms). The period window passes both
reference takes at their nominal period and refuses a 45.45 baud take at 10 % long. The scatter
limit is a third above the fixture's 8.8 % and inside the 10.6-13.6 % the five noise takes on hand
read, so scatter alone does not refuse those; what refuses them is the period, and none of the five
holds one (16.5 to 25.7 ms measured). A take that fails these is not evidence about the modem either
way. The gate has already earned its place: a
24 s take of the 10.1 MHz band whose free decode was `VXTYGJJUKD` reads 234 Hz apart with 0.66 of
its windows holding both tones and is refused, while a synthetic 50 baud take - the same program,
the same filters, where the pair really is switched - reads 0.00 both tones and 20.000 ms and is
accepted.

`rtty-wav` takes any 16 bit PCM WAV, any sample rate, mono or stereo: it resamples to the 12 kHz
the demodulator is pinned at and trims gain from the file peak, so a quiet recording still reaches
its thresholds. **Note for real signals**: `rtty.c` filters are built around a mark of 915 Hz, while
amateur RTTY puts mark and space at 2125/2295 Hz, so a live ham RTTY recording has to be tuned (or
spectrally translated) so the pair lands at 915/1085 Hz audio. NAVTEX style pairs at 1615/1785 Hz
are outside the filter bank as well, which is a real limit on reusing this modem for that service.

## What is real and what is scaffolding

`drivers/audio/rtty.c` is compiled from this Makefile, together with its own `rtty.h` (the bench
hooks it exposes live in the file behind `HOST_BENCH`, which the radio build never defines, and
nothing in the modem's own path is changed), and so is `drivers/audio/audio_agc.c`, whose cold-start
reset hook is behind the same guard: every case runs through the radio's audio AGC before the
demodulator, so the modem sees the level the receiver would hand it rather than the modulator's
own output. Both the transmitter (`Rtty_Modulator_GenSample`), the AGC
(`AudioAgc_RunAgcWdsp`) and the receiver (`Rtty_Demodulator_ProcessSample`) in the numbers below
are firmware code. The only substitutes:

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
operator's notch/peak/lowshelf biquad, which stays a passthrough until a shape is picked and is a
station setting this bench does not measure, and the impulse blanker, which needs an impulse and no
case sends one. The AGC runs on the factory configuration from `audio_agc.h` (MED, 70 dB slope,
20 dB threshold), because the firmware loads those from EEPROM; it is fed in ADC counts and its
output scaled back into the bench's +-1 samples, because a +-1 input is 72 dB below the level its
`max_input`/`out_target` are written for and its thresholds would then mean something else. Every
case starts it cold: the AGC carries gain, ring buffer and hang counter from one transmission to
the next, which would make a level row depend on the case before it, the same order dependency the
WAV check had. `-Wno-sign-compare` is the target build's own flag and is passed to the firmware sources only, so
those compile as they ship while the bench's own code stays under the full warning set. Moving
the chain in found the WAV bug - the round trip had never reset the decoder, so it started from
the previous case's filter state and only passed because that state happened to be harmless - and
it withdrew the loopback's ability to grade the shift until the transmitter's tones could be
held where they belong while only the receiver's setting moved, which is what `Rtty_BenchTxSpaceTone`
is for and what the shift bullet above records.

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
  hard-wired to the 915 Hz bandpass (`rtty_bp_12khz_915`) and the space filter is picked from the
  shift. Evaluated on the real coefficient tables as `|H(e^jw)|` of the `xv`/`yv` recursion, that
  915 Hz filter peaks at 913.5 Hz with its 3 dB band at 865-965 Hz (about 100 Hz wide), and rejects
  the space tone by 22.9 dB at 170 Hz shift, 26.1 dB at 200, 43.1 dB at 425, 44.7 dB at 450 and
  83.9 dB at 850, but by only 10.2 dB at 85. On a clean generated two-tone recording the ATC
  comparison can therefore decide from the mark channel alone - which is why decoding a 170 Hz-shift
  *synthetic* recording returns 0.0% CER
  whichever shift is selected, as the code's own comment predicts (*"works quite well with
  mark-only/space-only"*). **Do not generalise that.** The same test on a live DDH 8 capture gives:

  | selected shift | 85 | 170 | 200 | 425 | **450** | 850 |
  |---|---|---|---|---|---|---|
  | live decode | fails | fails | fails | full text | **full text** | fails |

  So on air the space filter's selectivity is doing real work that an equal two-tone synthetic
  signal never asks of it: with both tones at full level the 915 Hz filter keeps the right polarity
  whatever the space filter does, so those rows decode at 0.0 % even when the receiver is given the
  850 Hz space filter for a 170 Hz signal. One row was built to break that: `weak mark 170` sends the
  mark tone 30 dB below the space tone, past the 22.9 dB by which the mark filter rejects the space
  tone, so the mark channel inverts and the space filter has to carry the bits. That row stopped
  grading anything the moment the shift setting started to move both ends of the loop: the
  transmitter took its shift from the receiver's filter, so "the 850 Hz setting" was really an
  850 Hz signal with an 850 Hz filter and decoded perfectly. `Rtty_BenchTxSpaceTone()` separates
  them - the signal stays at 170 Hz shift while only the setting moves - and with that, and the
  mark tone 30 dB down, the setting is graded again: 0.0 % at 170 and at 200 Hz (adjacent bands),
  **97.9 %** at 85, 425, 450 and 850. The clean two-tone signal still decodes at 0.0 % with every
  setting, because the mark channel carries the bits whatever the space filter does; that is
  measured, printed next to the other column by `rtty-shift`, and it is why the guard row needs the
  weak mark. A live capture, of course, still needs the tones on the modem's own frequencies.
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
CER is measured after aligning away the lead-in junk, so it describes the modem in steady state.

| case | CER % |
|---|---|
| clean, 170 / 425 / 850 Hz shift | 0.0 |
| AWGN 20, 10, 7 dB | 0.0 |
| QSB 0.4 Hz, 6 dB deep | 0.0 |
| QSB 2 Hz, 10 dB deep | **6.2** (3/48, and **2.3** in `rtty-long`) |
| weak mark, 30 dB below the space tone | 0.0 with the 170 Hz space filter, **97.9** with the 850 Hz one |
| 170 Hz signal decoded with the 850 Hz setting, mark 30 dB down | **97.9** (47/48), against 0.0 % at 170 and 200 Hz |
| weak mark + carrier 300 Hz above the space tone at +20 dB | 0.0 at 170 Hz, **31.2** with the carrier at +30 dB (11.7 in `rtty-long`); **97.9** at either carrier level once the setting is 850 Hz |
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
neither length and carry a limit in both: fast fading at 6.2 % short and 2.3 % long, and a station
sent at 45.45 Bd but decoded as 50 Bd at 70.8 % and 69.4 %.

The AGC changes what this table says, and the honest version is that the earlier one was partly a
harness artefact. The demodulator has no input level control of its own; it inherits whatever the
audio AGC does. The bench used to feed the modem directly and assume the rest, then ran the AGC on
a +-1 float signal - which is 72 dB below the ADC-count level the AGC is written for (`max_input`
and `out_target` are both `ADC_CLIP_WARN_THRESHOLD`, 4096, and the firmware's audio buffer carries
converter counts). Fed in its own units, from a cold start for every case, the AGC does its job: a
station 30 dB down decodes as well as one at nominal level, 0.0 % (0/48 and 0/392) at -30, -10, 0
and +10 dB. The level rows therefore record the AGC working, not the modem being level proof.

Two things still break it, and they are measured with the same chain. Fast fading, 2 Hz at 10 dB
deep, used to lose most of the message (58.3 %) when the level was left to chance; through the AGC
it costs 6.2 % (3/48) at 45.45 Bd and 2.3 % (9/392) over the long message. A strong signal next door
is the remaining cliff: an unmodulated carrier 300 Hz above the space tone is harmless at +20 dB
over the wanted signal and costs **31.2 %** at +30 dB, with the weak mark tone in the same row
holding the bits on the space filter. That is a filter rejection limit, not a level one - which is
what makes it the interesting failure, since the AGC cannot help with it. It reproduces without a
radio or an antenna.

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

The window is about **+8 % / -7 %**, and it is not symmetric: the decoder follows a transmitter
that is fast up to +8 % and a slow one only to -7 %. The failure either way is a cliff rather than a slope -
+8 % is exact and +8.5 % already costs characters - and past it the text collapses to a handful of
surviving characters. Why the DPLL tolerates one direction more than the other is measured but not
explained here.

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
| 14 Hz | 0.0 / 0.0 | 0.0 / 0.0 | 0.0 / 0.0 |
| 50 Hz | 0.0 / 0.0 | **16.7** / 0.0 | 0.0 / 0.0 |
| 70 Hz | 0.0 / 0.0 | **100** / 0.0 | 0.0 / 0.0 |
| 85 Hz | **93.8 / 93.8** | **100 / 100** | 0.0 / 0.0 |
| 100 Hz | **87.5 / 87.5** | **89.6 / 97.9** | 0.0 / 0.0 |
| 140 Hz | 87.5 / 87.5 | 87.5 / 95.8 | **100 / 97.9** |

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

Our own side, measured rather than assumed: **warm**, with the software TCXO on and the radio nulled
against WWV, the residual is about **+-10 Hz at 14 MHz** (+-0.7 ppm), so roughly 20 Hz on 10 m and
36 Hz on 6 m. What it takes to get there is the part that has to be maintained, not just set: the
Si570's uncompensated swing is -203..+24 Hz at 14 MHz (16 ppm) over 0..99 C, `soft_tcxo.c` removes it with
a per-unit table (on by default, and its own header says the curve varies from unit to unit), and
`ts.freq_cal` nulls what is left in 0.1 ppm steps (range +-149.9 ppm). One number cannot follow
drift, so the calibration has to be redone.

Warm-up is part of that, not a detail. The table is referenced to 43 C and its header only claims
the curve for an Si570 and an MCP9801 that are thermally bonded, so before the two settle the
correction is chasing a temperature the sensor has not seen yet. Near room temperature the curve
runs about 5 Hz per C at 14 MHz (-99 Hz at 20 C against -75 Hz at 25 C), so every 6 C of sensor lag
is a third of the 170 Hz window, and a cold radio is the opposite of a calibrated one. The +-10 Hz
above is the warm figure; nothing here measures the first minutes, and the bench has no thermal
model to do it.

Si5351A boards get no temperature correction at all:
`Si5351a_PrepareNextFrequency()` takes the temperature factor and discards it, so there the +-10 Hz
holds only at the temperature it was calibrated at. The audio and symbol clocks do not enter into it
- a ppm of the 12 kHz side clock moves a 915 Hz tone by 0.001 Hz and a 22 ms bit by 22 ns - the
whole error is the RF reference times the band.

## Weather fax, `make -C support/digi wefax`

Synthetic 120 lines/min broadcast at 12 kHz: 5 ms sync tip at 1857 Hz, picture tone 1500 Hz (black)
to 2300 Hz (white), 300 columns per line, deterministic test chart (gradient, bars, two blobs, frame).
The reference demodulator finds the tip, then gives every pixel its own exact two-bin DFT over the
pixel slot, so no filter bank has to be tuned; per sample that is a handful of multiply accumulates.

| case | lines | chart correlation |
|---|---|---|
| clean / 20 dB / 12 dB | 12/12 | 0.975 / 0.974 / 0.973 |
| 12 dB + 2 Hz QSB, + skew | 12/12 | 0.973 |
| 8 dB with QSB and skew | 12/12 | 0.972 |
| black and white swapped | 12/12 | **-0.945** |
| 1 ppm on 20 m (14 Hz off), 5 ppm on 20 m (70 Hz) | 12/12 | 0.975 / 0.976 |
| 2 ppm on 10 m (56 Hz), 1 ppm on 6 m (50 Hz) | 12/12 | 0.976 |

The negative correlation is the useful number: the picture is recovered perfectly with the polarity
upside down, which is why a station parameter (or an automatic decision) is required before this is
usable on air.

A frequency error is applied to the fax generator the same way it is to the RTTY pair: every tone
of the emission moves by ppm x band while the demodulator's bins stay at 1857/1500/2300 Hz. Unlike
RTTY, the correlation barely reacts - Pearson removes the constant grey bias the offset causes, so
a band that is 5 ppm off still correlates at 0.976. The mean column is where it shows: 199.5 clean
against 208.2 at 70 Hz, about 3 % of the 800 Hz tone spacing. Correlation alone would therefore pass
those rows at every offset, and the wrong reason is worse than no assertion: the four ppm rows pin
the decoded grey mean as well (200.5..203.5 at 14 Hz, 205..209 at 56 Hz, 204.5..208 at 50 Hz,
206..210 at 70 Hz), so an offset that stopped moving the tone, or that moved it much further,
fails here. At 250 Hz off (5 ppm on 6 m) two of twelve sync tips are no longer found and the picture
is gone at mean 183.4, correlation 0.339, which is not a row because it would have to pass before
it could be pinned. The fax tones are 800 Hz apart and the sync window is 5 ms, so it weathers a
few ppm on HF and shares the 6 m limit with RTTY.

### Known gaps, all of them measured

* **The sync capture does not track the tip.** With each line skewed by up to +-120 samples the
  spacing metric stays at 0.0, i.e. it locks to the grid of the first line. The check asserts only
  what is true (still readable, correlation 0.975) and prints the gap instead of hiding it. An
  envelope based rising edge capture is the first real work item; a two stage version using a
  12 sample window was tried and is worse, because 1 ms of audio resolves nothing at 400 Hz tone
  spacing.
* **Left/right steering is invisible to this metric by construction**: the ground truth follows the
  transmitted scan order, and the reference decoder has no phasing reference. A real receiver needs
  the steer control, not a smarter score.
* Real station parameters (576 lines/min family, pilot levels, actual phasing signals, the
  frequency plans used by specific shore stations) are not modelled yet.
* No recorded on-air samples: the radio has no audio input and no mass storage, so a corpus means
  capturing I/Q or audio some other way. That is the highest value thing to solve before porting
  this into the firmware, because everything above is a synthetic channel.

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
tuning   text frame:  GET /~~param?f=<Hz>&band=<x>&lo=<Hz>&hi=<Hz>&mode=<n>&name=<who>
```

`lo`/`hi` are the audio passband edges (1400..2000 Hz is already the right shape for SITOR/NAVTEX
or weather-fax tones). `name` is shown to the receiver's operator, so fill something honest in.
These are shared receivers with a limited number of user slots: short sessions only.

### The collector, `capture/websdr_capture.py`

It does the transport half and nothing else: connect, tune, write the raw stream to `part-NNN.stream`
with one big-endian 32-bit length before each WebSocket message, resume at the next index, and
reconnect with backoff after a drop. A part is `--part-seconds` of wall clock, since with no decode
there is no sample count to reach. A fragment left by an attempt that was cut short keeps its own
index and is recorded `"complete": false`, so the next attempt cannot truncate it:

```
./websdr_capture.py --freq-khz 4581.41 --part-seconds 5 --parts 2 --out /tmp/proof
  part-000.stream     5.1 s      36780 bytes     41 messages  2026-11-20T05:37:48Z
  part-001.stream     5.0 s      36783 bytes     41 messages  2026-11-20T05:37:53Z
./websdr_capture.py --freq-khz 4581.41 --part-seconds 4 --parts 1 --out /tmp/proof   # resumes
  part-002.stream     4.0 s      29529 bytes     33 messages  2026-11-20T05:37:59Z
```

The first two files were still byte identical (md5 unchanged) after that second run, and every file
consumed exactly under the framing - 41, 41, 33, 46 and 25 messages of 518 to 909 bytes, no leftover
bytes. **None of it is gradable.** There is no WAV writer and no `--assemble` (`--assemble` is
rejected as an unknown argument), and `digi_bench capture-check` reads a WAV, so nothing in this tree
can score a `.stream` as audio. The DPCM decoder for these bytes is still unvalidated - see the
"What is proven here and what is not" list below - so a part is what the receiver sent and no more.

The DDH 8 capture quoted below was taken from NA5B. The station list, baud rates and shifts came
from the DWD table on the Franks-Weather page,
http://weather.mailasail.com/franks-weather/radio-teletype-weather-broadcasts - 50 Bd with 225 Hz
either side of the carrier on 4583/7646/10100.8/11039/14467.3 kHz, and 50 Bd with 42.5 Hz on
147.3 kHz. The receiver's own tuning readout is what turns those numbers into the audio pair the
modem sees; do not take the dial frequency for the tone pair.

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
  `S*Ot + Ot/2` stays float until `>> 4`). **That port is unfinished and unvalidated**; nothing in
  this repository decodes it yet;
* **the capture recipe this section used to give is wrong for the build that serves NA5B today**.
  `soundapplet.rec_start()` / `rec_finish()` **do exist** in this client, and they are the route that
  works: `rec_start()` allocates a 64 kB chunk and appends one more per audio block the socket
  delivers (about 4.1 s of audio at 8 kHz), and `rec_finish()` returns
  `{wavdata: [ArrayBuffer, ...], len, sr}` at the stream rate (`soundapplet.g`, 8000 Hz here). It
  needs no user gesture, no DPCM port and no server cooperation, and it fills from the WebSocket, so
  it works whether or not the page is rendering audio. What does **not** work here is the
  `soundapplet.p` ScriptProcessor tap: this client builds its node with
  `createScriptProcessor(4096, 0, 1)` - zero input channels - so `onaudioprocess` hands out a **zero
  length** `inputBuffer` (and a zero length `outputBuffer` in a tab that is not playing), and a tap
  on it records nothing at all. The "peak 8 of 32767" capture came from that tap, not from the
  recorder; the recorder was never the problem.
  Three things do bite in practice. A bare `setfreq()` only moves the displayed frequency, so use
  `setfreqif_fut('4581.84')` or the receiver stays where it was and the take is hum and rumble
  rather than FSK. The passband matters as much as the dial: `setmf('usb', 0.85, 1.45)` around the
  pair drops out-of-band noise by five orders of magnitude in the recording, where the default
  50-2660 Hz passband leaves the modem fighting it. And getting the bytes out of the browser is the
  remaining wrinkle: a `fetch` POST from the page to loopback is refused by the private network
  policy, but a top-level navigation is not, so the page can hand a base64 blob to a local collector
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
**DDK2** transmission (4583 kHz, 00:00-24:00 UTC, F1B, 50 Bd, +/-225 Hz), recorded through the NA5B
WebSDR client's own recorder on 19 November 2026 at about 20:45 local time on the recording
workstation - that clock reading was never logged against UTC, so it is not a UTC time - on a dial of
4581.84 kHz in USB with an 850-1450 Hz passband. The client's recorder ran for **64.1 s**, and only
the **last 38 s** of it were transferred out of the browser, since the hand-off is one top-level
navigation with one blob in it; every offset below is an offset into that 38 s window, not into the
whole take. That 38 s was decimated 2:1 to 4 kHz from the client's 8 kHz stream rate (the pair sits
below 1.4 kHz, more than 600 Hz clear of the 2 kHz Nyquist limit) and trimmed to the one clean
repetition, 19.1-32.8 s of the transferred 38 s. `fixtures/MANIFEST.md` carries the same provenance
as fields, so a re-capture is checked against numbers rather than against this prose.

The pinned text is the CQ line these stations send; the DDH 8 copy in the section above shows it with
that transmitter's own call sign and frequency list:

```
expected: CQ CQ CQ DE DDK2 DDH7 DDK9 FREQUENCIES   4583 KHZ   7646 KHZ   10100.8 KHZ
decoded : CQ CQ CQ DE DDK2 DDH7 DDK59FREQUENCIES   4583 KHZ   7646 KHZ   101008 KHZ
CER     : 2.7 %
```

`101008` is not a decode error: `norm_baudot()` keeps only A-Z, 0-9 and space, so the decimal point
never reaches the report. The one real error is the `5` at the `DDK9 FREQUENCIES` junction, and it is
**not** a shift-handling defect. `rtty.c` has no unshift-on-space at all - only codes 27 and 31
change `charSetMode`, and a space is `' '` in both tables - so `'5'`, which is `RTTYSymbols[16]`,
means code 16 arrived while the decoder sat in FIGS. A synthesised stream of this text decodes at
**0.0 %**, and the same stream with `[FIGS][9][SP]` replaced by `[FIGS][16][9]` reproduces
`DDK59FREQUENCIES` at 2.7 %: the decoder prints what arrives. Three takes of the same transmission
read `DDK9`, `DDK95` and `DDK59` there, so the corruption came with the signal rather than with the
logic. Two wrong characters of the **73** scored is 2.74 %, which the report rounds to the 2.7 % this
case pins; the target fails it above 3 %.

### The pair, measured

The average spectrum is the tempting measurement and the wrong one. Its humps are combs of 3.33 Hz
teeth from the idle `RYRY...` pattern, with keying sidebands at +-50 and +-100 Hz, so "the peak" is
whichever tooth is tallest and the answer comes out pulled inward: **441 +- 3 Hz** (932.0 / 1370.6).

Single tone runs only - 55 ms or longer, transitions trimmed, so neither the comb nor the other
tone's sidebands can bias the peak - give space **927.9 Hz** (n = 24, sd 0.44) and mark **1375.4 Hz**
(n = 15, sd 0.87), separation **447.5 +- 0.2 Hz**. Three estimators agree: per run maximum likelihood
median 447.54, a length extrapolation that removes the fixed window bias 447.51, and the phase slope
across runs 447.62. **447.5 Hz** is the number to use. On this take that is 0.55 % below the nominal
450 Hz, and the WWV check below holds the audio axis to 0.03 %, eighteen times smaller, so the offset
belongs to the emission and not to the reading.

The receiver then decides the shift setting, and it is not close: **450 Hz decodes at 2.7 % CER**,
425 Hz at 49.3 %, 170 Hz at 30.1 %, 850 Hz at 32.9 %. The measured 447.5 Hz sits 2.5 Hz from the
450 Hz setting and 22.5 Hz from the 425 Hz one, which is why that is the setting to receive it with:
the pair is a 450 Hz class emission that this take measures 0.55 % low, not a measurement that cannot
tell the two apart. 50 Bd is confirmed the same way: 20.0 ms bit period from the transition
spacing, and 2.7 % at 50 Bd against 37.0 % at 45.45 Bd. Both tones sit about 11 Hz above
the modem's 915/1365 Hz filters, which moves the pair without touching the separation.

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
QRM, so it says nothing about weak or fading signals, and its 2.7 % is one particular slip - a code
16 where a space belonged - not an error rate to quote for the mode. What it does pin is that the
decoder handles the DWD shifting correctly on real audio, and that any modem change which moves this
output has to be explained.

### The second on-air case, DDK9 on 10100.8 kHz, is not pinned

DDK9 was attempted on 19 November 2026 and **no fixture was committed**, because no take could be
verified. NA5B, the receiver behind the DDK2 case, had nothing there that evening: RMS 2300-2440 over
two passbands, with no FSK pair anywhere from 400 to 2500 Hz, which is a noise floor - 10 MHz was
shut from Texas to Europe.

Twente (`websdr.ewi.utwente.nl`) did show a 450 Hz / **50.0 Bd** shape at **924.0 / 1371.0 Hz**, but
nothing decodes through it: not this take, not DDK2, and not a synthesised 450 Hz / 50 Bd stream at
Twente's 7119 Hz rate, which reads 4.1 %. That puts the fault in the capture path rather than the
decoder, so a 90 %-CER line from it must not be pinned as ground truth. The rest of the sweep is no
better: DF0HTE (Friedrichshafen) and ES4O (Estonia) hold only a weak carrier in noise there (5 ms
windows, 60 % of them with both tones within 6 dB), and Hack Green has no 10 MHz band; Twente's dial
sits 1.025 kHz off its marker and ES4O's 105 Hz.

The comparison this case lacks is a propagation problem, not a bench one: retry on NA5B when 10 MHz
is open to Europe, and prove any new receiver by copying DDK2 through it to a few percent before
trusting a 10100.8 kHz take. A receiver's S-meter is not evidence; the tone pair plus a clean decode
is.

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

Both settings persist: `EEPROM_RTTY_SHIFT_IDX` = word 387 and `EEPROM_RTTY_SPEED_IDX` = word 388,
each with min/max limits, added in commit `979cfdf6`. Boot order re-applies them instead of leaving
the compile-time 170 Hz / 45 Bd initialisers in the modem:
`TransceiverStateInit` -> `UiDriver_Init` (which runs `UiDriver_LoadSavedConfigurationAtStartup` ->
`UiConfiguration_LoadEepromValues`) -> `AudioDriver_Init` (which calls `Rtty_Modem_Init`). If that
ordering ever changes, the symptom is a radio that shows the right setting on screen but demodulates
at 170 Hz / 45 Bd until an encoder is touched - which is the same species of fault as the EEPROM
address collision that destroyed 22 settings.

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

CI: the `digi-bench` job in `.github/workflows/config-upgrade-check.yml` runs seven steps - the
filter design check (`support/digi/design_rtty_filters.py`), `make digi-bench` (the self checks),
the RTTY shift, SNR and reference error character error rate matrix, that matrix again with the long
message, the shift setting curve, the tuning error sweep, and the recorded DDK2 fixture
(`make -C support/digi rtty-ddk2`). `make -C support/digi wefax` is a target of its own and is not
one of the seven. `make digi-bench` is also available on its own; it is one of the seven, not the
whole job.
