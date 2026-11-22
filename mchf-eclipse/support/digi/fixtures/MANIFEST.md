# Fixture manifest

Provenance for every recording the bench keeps. `*.wav` is ignored everywhere else; these files are
tracked on purpose (`!fixtures/*.wav`), so each one has to say where it came from.

## `ddk2-4583.wav` - DDK2 on 4583 kHz

| | |
| --- | --- |
| Station | **DDK2** (DWD Pinneberg): 4583 kHz, F1B, 50 Bd, +/-225 Hz, schedule 00:00-24:00 UTC |
| Receiver | NA5B WebSDR, `na5b.com:8901` (Washington DC area, FM18JS) |
| Capture time | 19 Nov 2026, about **20:45-20:52 local**, workstation at **UTC-06:00** - so about **02:45-02:52 UTC on 20 Nov**. The recorder itself never logged a clock reading; the window is derived from the written file at `20:52:00 -0600`, less the 64.1 s the recorder had been running |
| Dial | **4581.84 kHz**, USB |
| Passband | **850-1450 Hz**, i.e. `setmf('usb', 0.85, 1.45)` |
| Stream rate | **8000 Hz** (`soundapplet.g`); the client's audio axis was checked against WWV at 10 MHz and holds to 0.03 % (README, "Is the stream rate what it says it is?") |
| Recorded | **64.1 s** in the client's recorder; only the **last 38 s** were transferred out of the browser |
| Trim | **19.1-32.8 s of that 38 s window** (not of the 64.1 s) -> **13.7 s** kept |
| Stored as | 4 kHz mono 16-bit, 54 800 frames, **109 644 bytes**: decimated 2:1 from the 8 kHz stream, which is safe because the pair sits below 1.5 kHz |
| Tone pair | **927.9 / 1375.4 Hz**, separation **447.5 +/- 0.2 Hz**, from single-tone runs only; both tones sit about **11 Hz above the modem's 915/1365 Hz filters** |
| Decode | **2.7 % CER** at the receiver's **450 Hz shift / 50 Bd**; 425 Hz gives 49.3 %, 45.45 Bd gives 35.6 %. The same 2.7 % comes back at 8 kHz and 12 kHz made of different characters - see "The count is stable, the characters are not" |
| Re-verify | `make -C support/digi rtty-ddk2` - fails above 3 % CER |

### The count is stable, the characters are not

The file is stored at 4 kHz because 2:1 decimation of an 8 kHz stream loses nothing below 1.5 kHz,
but a re-derivation can end up at another rate - the native 8 kHz stream, or 12 kHz for the bench's
own TX path. Resampling this fixture with `sox` and decoding it again at 450 Hz / 50 Bd gives
**2.7 % in all three cases** (2 edits over 73 characters), and 8 kHz and 12 kHz print the same text
as each other. The two edits are not the same two:

| rate | decoded after `DDH7` | the two edits against the pinned text |
| --- | --- | --- |
| 4 kHz (stored) | `DDK59FREQUENCIES   4583 KHZ` | inserted `5` after the `DDK9`; the junction space is lost |
| 8 kHz (`sox -r 8000`) | `DDK9FREQUENCIES   583 KHZ` | the junction space is lost; the `4` of `4583` is missing |
| 12 kHz (`sox -r 12000`) | `DDK9FREQUENCIES   583 KHZ` | as 8 kHz |

So the junction space at `DDK9|FREQUENCIES` is charged at every rate, the extra character after the
`DDK9` only appears at 4 kHz, and at 8 and 12 kHz the second edit moves down the line and costs the
`4` of `4583` instead. Matching 2.7 % therefore does not mean a re-derivation reproduced this take's
errors: only the same two characters would. The characters track the resampling as much as the
rate - a plain linear interpolation of the 4 kHz file back to 8 or 12 kHz decodes exactly like the
4 kHz file, extra `5` and lost space included - so a re-derivation should say which resampler it
used before either reading is called a regression.

### How it was taken

1. Open the NA5B client and start its audio (`Chrome audio start`).
2. Tune with `setfreqif_fut('4581.84')`. A bare `setfreq()` only moves the readout: the receiver stays
   where it was and the take is hum instead of FSK.
3. `set_mode('usb')`, then `setmf('usb', 0.85, 1.45)`. The default 50-2660 Hz passband leaves the
   modem fighting out-of-band noise; the narrow one drops it by about five orders of magnitude.
4. `soundapplet.rec_start()`, wait, then `soundapplet.rec_finish()` for `{wavdata, len, sr}`.
5. Hand the bytes out of the page by top-level navigation to a local collector
   (`location.href = 'http://127.0.0.1:8765/?d=<base64>'`); a `fetch` POST to loopback is refused by
   the private-network policy. One navigation carries one blob, which is why only the last 38 s of the
   64.1 s take were moved.
6. Decimate 2:1 to 4 kHz, then trim to one clean repetition of the CQ line.

### Re-verifying a re-capture

Trust a take only if both hold: the tone pair measures a 447-450 Hz separation from single-tone runs
(not from the average spectrum, whose comb and keying sidebands pull the reading inward), and the
decode lands within a few percent of the pinned text. A receiver's S-meter says nothing; a clean
decode does. Every receiver has its own dial offset - 1.025 kHz on Twente's marker-to-dial, 105 Hz on
ES4O, 19 Hz on NA5B's WWV capture - so the dial in this table is NA5B's, not a general rule.
