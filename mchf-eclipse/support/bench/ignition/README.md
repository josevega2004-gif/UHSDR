# Ignition-noise bench box (Bench A)

A controllable, repeatable ignition-noise simulator built around an SI5351 module and any
FK743M5-class dev board, for testing the mcHF/UHSDR impulse suppression without waiting for a
cold morning and a car.

## Principle

The SI5351 holds a ~7 MHz carrier enabled but the module's MUTE line is held high (= muted).
A "spark" is a 1..40 µs window where the core drops MUTE: the power edges at the window's start
and end are broadband transients that reach the receiver exactly like an ignition click. The
pattern engine schedules those windows with rate, width, period jitter and width jitter, and a
seed so any test run can be reproduced byte-identically.

## Wiring (jumper wires are fine on the control side)

| SI5351 module | FK743M5 board |
|---|---|
| VCC | 5 V (module regulator) or 3.3 V direct |
| GND | ground star point |
| SDA / SCL | I2C1 pins (module carries its own pull-ups) |
| MUTE | one GPIO (push-pull), e.g. PB0 |
| CLK0 out | -> attenuator -> coax pigtail -> mcHF ANT |

Attenuator between module and radio — the 8 mA square-wave output will otherwise overload the
input; both stages are π-pads in 50 Ω:

* stage 1, 40 dB: shunt 10 Ω / series 49.9 Ω / shunt 10 Ω
* stage 2, 20 dB: shunt 5.6 Ω / series 49.9 Ω / shunt 5.6 Ω

Build the pads dead-bug at the module output, shortest possible run to the coax, one ground
point. Do not breadboard the RF leg.

## Firmware

Drop `ignition_core.{c,h}`, `si5351.{c,h}`, `si5351_port.h` and `port_stm32f743.c` into a
CubeMX project for the board (I2C1 400 kHz, one output GPIO `BENCH_MUTE`, one UART with RX
interrupt enabled; check the three macros at the top of the port file). `make` at host level is
NOT the target build — on the host it compiles the test instead.

## Console (UART, 115200)

```
e<n>      events per second (25 idle-ish, 50 city, 100+ accelerating)
w<n>      spark window, microseconds (1..40)
j<n>      period jitter percent (0..50, 20 default)
k<n>      width jitter percent
seed<n>   reproduce a previous run exactly
f<n>      carrier Hz (radio tunes to this)
o         start, x stop, i status
```

## Measuring with it (the reason this exists)

For each row, log: S-meter swing, audible residual, and whether the audio de-sensitizes.

| box setting | mcHF NB | shows |
|---|---|---|
| e50 w5 j20, off | 0 | raw baseline, pumping |
| e50 w5 j20 | 1 / 5 / 10 ramp | blanker onset, mid, aggressive |
| e100 w10 j30 | 5..15 | dense/high-rpm case |
| f<freq> + `w40` | 15 | long-burst worst case |

Keep the pad value fixed during a session and write the box `seed` into the notes; a second
session with the same seed and pad settings is directly comparable.

## Files / provenance

* `si5351.c/h` + `LICENSE`: vendored, unmodified math, from https://github.com/afiskon/stm32-si5351
  commit 532fbe9 (MIT). Local port: the two HAL/I2C glue lines replaced by `si5351_port.h` so
  the identical driver runs against host stubs.
* `ignition_core.c/h`: this project's pattern engine + console, portable C.
* `port_stm32f743.c`: dev-board glue (not built on host).
* `main_host.c`: host test — carrier math, spark timing, seed determinism, console parser.

## Next step on this hardware

The same board + a TCXO SI5351 becomes the FT8 symbol beacon (Bench B plan): tone writes every
160 ms using the vendored `drivers/digi/ft8_lib` encoder.
