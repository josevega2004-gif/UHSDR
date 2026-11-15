# ft8_lib vendor drop

Location: `drivers/digi/ft8_lib/` (decoder library) and `support/ft8/` (host bench).

- Upstream: https://github.com/kgoba/ft8_lib, commit `9fec6ca`, fetched 2026-11-15.
- License: MIT (see `LICENSE`, kept intact; individual file headers upstream).
- Contents copied unmodified: `ft8/` (decode, encode, ldpc, crc, constants, message, text)
  and `fft/` (kiss_fft, kiss_fftr, float32 configuration).
- Deliberately excluded: upstream Makefile/tests (replaced by `support/ft8/Makefile`),
  `demo/`+`common/` live at `support/ft8/` because they are host-only build tooling,
  and 51 of 54 test WAVs (23 MB) to keep the repository light; three fixtures remain:
  `websdr_test7.wav` (WebSDR capture, 18 reference decodes), and two 191111 recordings.

## Status in this firmware

Not compiled into any radio target yet. Plan:
1. host bench (here) proves decode behaviour with the exact library sources;
2. RX-only integration on H7 (1 MB RAM), then F7 (512 KB, int16 capture buffers);
3. TX + GNSS/RTC time sync afterwards.

## Notes for future local modifications

Keep upstream file headers, mark changes with a `// UHSDR:` comment line so a
re-vendor can diff cleanly. The radio build must not link `common/` or `demo/`.
