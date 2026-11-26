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
sh build-sweep.sh /path/to/mchf-eclipse /tmp/pathsweep-chain.csv --chain  # + IIR/AGC
sh build-sweep.sh /path/to/mchf-eclipse /tmp/pathsweep-levels.csv --chain-levels  # AGC level sweep
sh check-chain-levels.sh /path/to/mchf-eclipse                        # the same sweep, asserted
```

`path_sweep.c` is the exception to "these are the scripts behind the pictures": it draws nothing. It
sweeps all 62 SSB and CW filter paths - the shape of each against the shape of the pair it would replace,
and the arithmetic bill of each - and prints one CSV row per path. Those rows are the table in "Every
path, not just the two" in `../README.md`; run it again before quoting that table, because the numbers
there are the filter's and the path table's, not a drawing's.  Rows where the stock table's -6 dB point
is inside the path print `n/c` for the stock shape: the stock side would measure that cutoff rather than
the path.  The CSV's `stock_edge_db` column carries the measured edge level; these are the two 3.6 kHz rows
and six of the wide rows.  Their cost is still printed and comparable.

The same sweep with `--chain` runs each path through its pre-AGC IIR, the firmware WDSP AGC and the
post-AGC anti-alias IIR, and prints a second CSV with `stock_chain_*`, `conv_chain_*`, `*_top_rel` and
`*_agc_top` columns.  `*_agc_top_weak` is the -20 dB point of the level sweep below, kept in this CSV.  The AGC is the
real `drivers/audio/audio_agc.c`; the excitation is a passband reference plus the path's top edge.  The stock shape columns are `n/c` on the same eight rows.

`--chain-levels` prints that two-tone pass as a table of its own instead: one row per path and level,
`id,name,level_db,level,stock_agc_top,conv_agc_top,stock_deficit,stock_agc_ref`, sweeping 20 dB above
the bench level to 40 dB below it (`A=0.5` to `A=0.0005`) in 2 dB steps.  The weak column is then one
point on a measured grid rather than the only check, and the top of the range reaches the AGC's own
`max_input` so the compression half is covered.  Stock deficits, the set of rows with a positive
deficit and the `n/c` set do not change anywhere in that range; the largest movement of either top is
0.002 dB.  `stock_agc_ref` is the reference tone's absolute level after the AGC, which shows the AGC
compressing over the top 20 dB and running out of gain below its knee.  `check-chain-levels.sh`,
also `make -C ../digi chain-levels-check`, runs that sweep and fails if any top or the deficit moves
more than 0.01 dB from nominal, if a level loses a row, if the sweep stops covering the 62
documented paths (ids 4-65), or if the positive-deficit set stops being the eight n/c rows.

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
