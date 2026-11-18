# Digital mode host bench

Grades the radio's own modems on a PC. Nothing in this directory is compiled into the firmware.

```
make -C support/digi test     # self checks, non zero exit on regression   (also: make digi-bench)
make -C support/digi rtty     # RTTY character error rate, modem looped back on itself
make -C support/digi wefax    # synthetic HF weather fax through the reference demodulator
```

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
* until it does, the practical way to get a WAV from these receivers is the browser: tune by hand
  and use the client's own recorder, which already holds PCM in memory
  (`soundapplet.rec_start()` ... `soundapplet.rec_finish()` -> `{wavdata, len, sr}`).

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
