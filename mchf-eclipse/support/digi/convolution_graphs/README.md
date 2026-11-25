# The convolution filter graphs

These are the scripts that produced the eight PNGs the convolution section of
`../README.md` was drawn from.  The images are not committed: they are 1.2 MB of output that
regenerates from the tree in about a minute, and a graph that cannot be re-run against the code it
describes is decoration.

```sh
sh build.sh /path/to/mchf-eclipse /tmp/compare.csv                     # sections 1-6
sh build.sh /path/to/mchf-eclipse /tmp/compare64.csv -DFFT_CONVOLUTION_SIZE=64
python3 plot.py /tmp /tmp/compare.csv /tmp/compare128.csv /tmp/compare64.csv   # graphs 1-6
python3 plot8.py /tmp                                                 # graph 8, minimum phase
python3 alternative_designs.py /tmp                                   # graph 7, candidate designs
sh build-sweep.sh /path/to/mchf-eclipse /tmp/pathsweep.csv            # every path, CSV
```

`path_sweep.c` is the exception to "these are the scripts behind the pictures": it draws nothing. It
sweeps all 62 SSB and CW filter paths - the shape of each against the shape of the pair it would replace,
and the arithmetic bill of each - and prints one CSV row per path. Those rows are the table in "Every
path, not just the two" in `../README.md`; run it again before quoting that table, because the numbers
there are the filter's and the path table's, not a drawing's.  The two 3.6 kHz rows print `n/c` for the
stock shape: their band reaches past the 199-tap table's 3.5 kHz cutoff, so the stock side would measure
the cutoff rather than the path.  Their cost is still printed and comparable.

`build.sh` compiles `compare.c` against the same shipped sources the probe uses - `audio_convolution.c`
as text, the host FFT in `../convolution_fft.c`, and `FilterPathInfo[]` cut out of `audio_filter.c` - so
its numbers are the radio's, and it takes the geometry flags (`-DFFT_CONVOLUTION_SIZE=...`) the same way
`make convolution-check EXTRA_DEFS=...` does.

Graph 6's fourth panel is the cost in time rather than in operations: the measured op count times 1.5 cycles
per operation at the F407's 168 MHz, with bars at 1.0 and 2.0 because that factor is a model and not a
measurement of the chip. The radio's own DWT counters (`L`/`P`, and `G` in the measurement build) are the
check on it.

`compare.c` predates the probe's later sections, so it does not cover the two things the probe checks
now and the graphs do not show: DC on the paths whose band reaches zero (probe section 11), and a worker
pass landing inside a filter design (section 12).  Graphs 5 and 6 show linear-phase delays and were
superseded by graph 8 when the taps became minimum phase.
