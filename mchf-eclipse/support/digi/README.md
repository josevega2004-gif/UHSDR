# Digital mode host bench

Grades the radio's own modems on a PC. Nothing in this directory is compiled into the firmware.

```
make -C support/digi test     # self checks, non zero exit on regression   (also: make digi-bench)
make -C support/digi rtty     # RTTY character error rate, modem looped back on itself
make -C support/digi wefax    # synthetic HF weather fax through the reference demodulator
./digi_bench rtty-wav <file.wav> [shift] ["expected text"]
                            # decode a recorded file with the firmware demodulator
```

`rtty-wav` takes any 16 bit PCM WAV, any sample rate, mono or stereo: it resamples to the 12 kHz
the demodulator is pinned at and trims gain from the file peak, so a quiet recording still reaches
its thresholds. **Note for real signals**: `rtty.c` filters are built around a mark of 915 Hz, while
amateur RTTY puts mark and space at 2125/2295 Hz, so a live ham RTTY recording has to be tuned (or
spectrally translated) so the pair lands at 915/1085 Hz audio. NAVTEX style pairs at 1615/1785 Hz
are outside the filter bank as well, which is a real limit on reusing this modem for that service.

## What is real and what is scaffolding

`drivers/audio/rtty.c` is compiled **unmodified** from this Makefile, together with its own
`rtty.h`. Both the transmitter (`Rtty_Modulator_GenSample`) and the receiver
(`Rtty_Demodulator_ProcessSample`) in the numbers below are firmware code. The only substitutes:

| file | replaces | why it is safe |
|---|---|---|
| `shim/ui_driver.h` | `UiDriver_TextMsgPutChar`, `ts.rtty_cwi` | the modem asks the UI layer for exactly these two things |
| `shim/radio_management.h` | `RadioManagement_Request_TxOff` | one call, its firing is itself a checked item |
| `shim/uhsdr_digi_buffer.h` via the real header | the TX character buffer | real enum and prototypes, scripted feed in `modem_host.c` |
| `shim/softdds.h`, `softdds_host.c` | the sine table DDS | computed instead of looked up; `test` measures the produced tone (915.0 Hz asked, 915.0 Hz measured) before grading anything with it |
| `shim/arm_math.h` | CMSIS types | `hardware/uhsdr_types.h` includes that header only for `float32_t` and friends |
| `shim/uhsdr_board.h`, `shim/profiling.h` | board switches, counters | empty; the switches the modem needs come from `-D` |

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
  shift. Measured on the real coefficient tables, that 915 Hz filter (3 dB band 1034-1136 Hz, about
  102 Hz wide) rejects the space tone by 20 dB at 170 Hz shift, 23 dB at 200, 35 dB at 425/450 and
  45 dB at 850, so on a clean generated two-tone recording the ATC comparison can decide from the
  mark channel alone - which is why decoding a 170 Hz-shift *synthetic* recording returns 0.0% CER
  whichever shift is selected, as the code's own comment predicts (*"works quite well with
  mark-only/space-only"*). **Do not generalise that.** The same test on a live DDH 8 capture gives:

  | selected shift | 85 | 170 | 200 | 425 | **450** | 850 |
  |---|---|---|---|---|---|---|
  | live decode | fails | fails | fails | full text | **full text** | fails |

  So on air the space filter's selectivity is doing real work that a two-tone synthetic signal never
  asks of it. Consequence for this bench: the loopback CER cases **cannot** grade the shift setting,
  and a live capture does need the tones placed on the modem's own filter frequencies.
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
| AWGN 20, 15, 12, 10, 7 dB | 0.0 |
| QSB 0.4 Hz, 6 dB deep | 0.0 |
| QSB 2 Hz, 10 dB deep | **50.0** (loses characters mid message) |

That reframes "a better RTTY decoder": SNR handling is not the weak point, **dynamic range under
fast fading is**. The demodulator has no input level control of its own, so it inherits whatever
the audio AGC is doing, and the bench reproduces the failure without a radio or an antenna.

## Weather fax, `make -C support/digi wefax`

Synthetic 120 lines/min broadcast at 12 kHz: 5 ms sync tip at 1857 Hz, picture tone 1500 Hz (black)
to 2300 Hz (white), 300 columns per line, deterministic test chart (gradient, bars, two blobs, frame).
The reference demodulator finds the tip, then gives every pixel its own exact two-bin DFT over the
pixel slot, so no filter bank has to be tuned; per sample that is a handful of multiply accumulates.

| case | lines | chart correlation |
|---|---|---|
| clean / 20 dB / 12 dB | 12/12 | 0.975 / 0.974 / 0.973 |
| 12 dB + 0.4 Hz QSB, + 2 Hz QSB, + skew | 12/12 | 0.973 |
| 8 dB with QSB and skew | 12/12 | 0.972 |
| black and white swapped | 12/12 | **-0.945** |

The negative correlation is the useful number: the picture is recovered perfectly with the polarity
upside down, which is why a station parameter (or an automatic decision) is required before this is
usable on air.

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
http://na5b.com:8901/                    NA5B, Texas       (US NAVTEX is 512 kHz, not 518!)
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
* the recorder route that used to be documented here, `soundapplet.rec_start()` /
  `rec_finish()`, **does not exist in the current build** - calling it captured an undefined buffer
  and read back as digital silence (peak 8 of 32767), which is how a silent capture got mistaken for
  a modem failure. The route that works on today's client, and which needs no user gesture, no DPCM
  port and no server cooperation, is to tap the node that already holds the PCM: `soundapplet.p` is a
  `ScriptProcessorNode` whose `onaudioprocess` delivers exactly what the page plays. Wrap it without
  removing the original handler, average 4:1 to get 48 kHz down to the modem's 12 kHz, hold the
  floats in an array, then build a 44-byte PCM header around them and trigger a Blob download. The
  browser's download directory is on the same host as this checkout, so the WAV appears on disk and
  goes straight into `digi_bench rtty-wav`. Tune with `document.querySelector('input[name=frequency]').value`
  plus `setfreqif_fut(value)`; keep any in-page `await` under about 13 s or the tool call times out
  (let the tap accumulate between short calls instead of blocking).

## First live decode through this harness

DDH 8, 14467.3 kHz, F1B, 50 Bd, +/-225 Hz (captured 11:41 UTC, 30 s at 12 kHz):

```
decoded: MSOHWWM PPUWWNW  6  8  3  MSO  12 WNW  6  8  4  MMO 23 Z A  34  3  MO 23 12Z SSW  5  2
```

Tuned so the received pair (14467.075 / 14467.525 kHz) lands on the modem's own 915 / 1365 Hz
filters. The same audio decoded at 45.45 Bd instead of 50 gives `6 78 MOTLYMV20UWNW`, so the baud
setting is what carries it, and the mark/space polarity in `RttyDecoder_demodulator()` is correct -
an inversion there would have produced letters that are not in the DWD message set at all.

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

CI: the `digi-bench` job in `.github/workflows/config-upgrade-check.yml` runs `make digi-bench`.
