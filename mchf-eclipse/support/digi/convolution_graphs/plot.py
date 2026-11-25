import csv, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

out = sys.argv[1]
ram_kb = {256: float(sys.argv[2]), 128: float(sys.argv[3]), 64: float(sys.argv[4])}
ops = {"stock": 796, 256: 356, 128: 324, 64: 292}          # steady state per audio sample; the probe's run averages read 786 / 372 / 331 / 295
# Those op counts are the measured bill; this turns them into time on the radio.  A Cortex-M4F issues one
# single-precision operation per cycle, and the CMSIS and compiled loops spend roughly that much again on
# loads, stores and loop control, so 1.5 cycles per counted operation is the estimate and 1.0 to 2.0 the
# range it is drawn with.  No radio has been timed yet: the DWT counters behind `L`/`P` and the measurement
# build are the check, and they measure the whole receiver, so this panel is what they should agree with.
CYC, CYC_LO, CYC_HI, MHZ = 1.5, 1.0, 2.0, 168.0
cpu = {k: v * CYC / MHZ for k, v in ops.items()}            # microseconds of F407 time per 12 kHz sample
cpu_lo = {k: v * CYC_LO / MHZ for k, v in ops.items()}
cpu_hi = {k: v * CYC_HI / MHZ for k, v in ops.items()}
core = lambda v: v * 12000.0 / 1e6 * 100.0                    # percent of one core at 12 kHz, v in us per sample

def load(name):
    d = {"ssb": [], "cw": [], "level": [], "burst_ssb": [], "burst_cw": [], "meta": {}}
    for r in csv.reader(open(name)):
        if r[0] == "meta": d["meta"][r[1]] = (float(r[2]), float(r[3]))
        elif r[0] == "level": d["level"].append((int(r[1]), r[2], float(r[3]), float(r[4]), float(r[5]), float(r[6]), float(r[7]), float(r[8])))
        else: d[r[0]].append(tuple(float(x) for x in r[1:]))
    return d

before = load("before_256.csv")
after = {g: load("after_%d.csv" % g) for g in (256, 128, 64)}
C = {"stock": "#222222", "before": "#d62728", 256: "#1f77b4", 128: "#17a2a2", 64: "#ff8c1a"}
FLOOR = -150
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3, "figure.dpi": 130, "savefig.bbox": "tight"})
col = lambda rows, i: [max(r[i], FLOOR) for r in rows]
xs = lambda rows, k=1.0: [r[0] / k for r in rows]
NOTE = "Measured at the point where the two receivers differ. The filter path's own audio IIR runs after both and is not included."

# 1 ---- sideband and band, 2.7 kHz SSB path
fig, ax = plt.subplots(figsize=(11, 5.2))
ax.axvspan(0.05, 2.7, color="#2ca02c", alpha=0.10, label="wanted: USB audio, 50 Hz to 2.7 kHz")
ax.plot(xs(after[256]["ssb"], 1000), col(after[256]["ssb"], 1), color=C["stock"], lw=1.6, label="normal receiver (Hilbert pair, I+Q)")
ax.plot(xs(before["ssb"], 1000), col(before["ssb"], 2), color=C["before"], lw=1.4, label="convolution as committed: other sideband, 7 dB too loud")
ax.plot(xs(after[256]["ssb"], 1000), col(after[256]["ssb"], 2), color=C[256], lw=1.6, label="convolution after the fixes (geometry 256, 1024 taps)")
ax.axvline(0, color="k", lw=0.6)
ax.set(xlim=(-6, 6), ylim=(FLOOR, 25), xlabel="signal offset from the tuned frequency (kHz); positive is the USB side",
       ylabel="audio level for a unit tone (dB)", title="2.7 kHz SSB filter with USB selected: what each receiver passes")
ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.13), fontsize=9, ncol=2, frameon=False)
fig.text(0.5, -0.13, NOTE + "  Below about -140 dB the radio's float32 arithmetic, not the filter, sets the floor.", ha="center", fontsize=8, color="#555555")
fig.savefig(os.path.join(out, "1-sideband-2k7-ssb.png")); plt.close(fig)

# 2 ---- selectivity: the two edges, per geometry
fig, (a, b) = plt.subplots(1, 2, figsize=(13, 5))
for ax, lo, hi, title in ((a, 2300, 4300, "Upper edge: how sharply the 2.7 kHz filter closes"),
                          (b, -400, 400, "Around the carrier: low edge and the other sideband")):
    ax.plot(xs(after[256]["ssb"]), col(after[256]["ssb"], 1), color=C["stock"], lw=1.8, label="normal receiver")
    for g in (256, 128, 64):
        ax.plot(xs(after[g]["ssb"]), col(after[g]["ssb"], 2), color=C[g], lw=1.5, label="convolution, geometry %d (%d taps)" % (g, after[g]["meta"]["geometry"][1]))
    ax.set(xlim=(lo, hi), ylim=(-130, 12), xlabel="signal offset from the tuned frequency (Hz)", title=title)
    ax.axhline(0, color="k", lw=0.4)
a.set_ylabel("audio level for a unit tone (dB)")
a.axvline(2700, color="#2ca02c", ls="--", lw=1); a.text(2715, 8.5, "filter width 2.7 kHz", color="#2ca02c", fontsize=8)
b.axvline(0, color="k", lw=0.6); b.axvline(50, color="#2ca02c", ls="--", lw=1); b.text(58, 8.5, "low edge 50 Hz", color="#2ca02c", fontsize=8)
b.text(-390, -14, "other sideband\n(must be rejected)", fontsize=9, color="#555555"); b.text(230, -14, "wanted sideband", fontsize=9, color="#555555")
h, l = a.get_legend_handles_labels()
fig.legend(h, l, loc="upper center", bbox_to_anchor=(0.5, 0.02), fontsize=9, ncol=4, frameon=False)
fig.text(0.5, -0.08, NOTE, ha="center", fontsize=8, color="#555555")
fig.savefig(os.path.join(out, "2-selectivity-edges.png")); plt.close(fig)

# 3 ---- the 500 Hz CW path
fig, ax = plt.subplots(figsize=(11, 5.2))
ax.axvspan(500, 1000, color="#2ca02c", alpha=0.10, label="the filter path's band: 500 Hz around a 750 Hz sidetone")
ax.plot(xs(after[256]["cw"]), col(after[256]["cw"], 1), color=C["stock"], lw=1.6, label="normal receiver (its 500 Hz shape comes from the IIR that follows)")
ax.plot(xs(before["cw"]), col(before["cw"], 2), color=C["before"], lw=1.4, label="convolution as committed: 50-500 Hz, other sideband, 22 dB too loud")
ax.plot(xs(after[256]["cw"]), col(after[256]["cw"], 2), color=C[256], lw=1.6, label="convolution after the fixes, geometry 256")
ax.plot(xs(after[64]["cw"]), col(after[64]["cw"], 2), color=C[64], lw=1.3, label="convolution after the fixes, geometry 64")
ax.axvline(750, color="#2ca02c", ls="--", lw=1); ax.axvline(0, color="k", lw=0.6)
ax.set(xlim=(-1500, 1800), ylim=(FLOOR, 40), xlabel="signal offset from the tuned frequency (Hz); the CW note is heard at +750 Hz",
       ylabel="audio level for a unit tone (dB)", title="500 Hz CW filter (750 Hz sidetone): where each receiver listens")
ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.13), fontsize=9, ncol=2, frameon=False)
fig.text(0.5, -0.19, NOTE, ha="center", fontsize=8, color="#555555")
fig.savefig(os.path.join(out, "3-cw-500hz.png")); plt.close(fig)

# 4 ---- level at the centre of every SSB/CW path
rows_b, rows_a = before["level"], after[256]["level"]
n = len(rows_a); x = list(range(n))
labels = ["%s @ %d" % ("%gk" % (r[2] / 1000) if r[2] >= 1000 else "%g" % r[2], r[3]) for r in rows_a]
fig, ax = plt.subplots(figsize=(15, 5.6))
bl = [max(r[6], r[7]) for r in rows_b]                       # on whichever sideband it actually passed
loud = [(i, v) for i, v in zip(x, bl) if v > -20]; silent = [i for i, v in zip(x, bl) if v <= -20]
ax.plot(x, [r[5] for r in rows_a], "o", color=C["stock"], ms=6, label="normal receiver")
ax.plot([i for i, _ in loud], [v for _, v in loud], "s", color=C["before"], ms=5, label="convolution as committed (on whichever sideband it passed)")
ax.plot(silent, [-17] * len(silent), "v", color=C["before"], ms=8, label="convolution as committed: silent (-80 dB or lower)")
ax.plot(x, [r[6] for r in rows_a], "D", color=C[256], ms=3.5, label="convolution after the fixes")
ax.set_xticks(x); ax.set_xticklabels(labels, rotation=90, fontsize=6.5)
ax.set(ylim=(-20, 36), xlim=(-1, n), ylabel="audio level for a unit tone (dB)", xlabel="filter path: width @ centre of its audio band, in Hz, in the radio's table order",
       title="A tone at the centre of each of the %d SSB/CW filter paths: how loud it comes out" % n)
ax.legend(loc="upper right", fontsize=9)
fig.savefig(os.path.join(out, "4-level-every-path.png")); plt.close(fig)

# 5 ---- latency: a tone switched on at t = 0
fig, (a, b) = plt.subplots(1, 2, figsize=(13, 4.8), sharey=True)
for ax, tag, title in ((a, "burst_ssb", "2.7 kHz SSB path, 1 kHz tone switched on at t = 0"), (b, "burst_cw", "500 Hz CW path, 750 Hz tone switched on at t = 0")):
    s = after[256]["meta"][tag + "_delay_ms"][0]
    ax.plot(xs(after[256][tag]), [r[1] for r in after[256][tag]], color=C["stock"], lw=1.8, label="normal receiver: %.2f ms" % s)
    for g in (64, 128, 256):
        ax.plot(xs(after[g][tag]), [r[2] for r in after[g][tag]], color=C[g], lw=1.5, label="convolution, geometry %d: %.1f ms" % (g, after[g]["meta"][tag + "_delay_ms"][1]))
    ax.axhline(0.5, color="k", lw=0.5, ls=":"); ax.axvline(0, color="k", lw=0.6)
    ax.set(xlim=(-10, 90), ylim=(-0.03, 1.2), xlabel="time after the signal appears (ms)", title=title)
    ax.legend(loc="lower right", fontsize=9, title="time to half level")
a.set_ylabel("audio envelope (1 = full level)")
fig.text(0.5, -0.03, "Through the firmware's own queues and worker at 12 kHz. Delay of the AGC, the audio IIR and the codec is the same for both and is not included.", ha="center", fontsize=8, color="#555555")
fig.savefig(os.path.join(out, "5-latency-tone-onset.png")); plt.close(fig)

# 6 ---- what it costs
names = ["normal", "conv\n256", "conv\n128", "conv\n64"]
cols = [C["stock"], C[256], C[128], C[64]]
delay = [after[256]["meta"]["burst_ssb_delay_ms"][0]] + [after[g]["meta"]["burst_ssb_delay_ms"][1] for g in (256, 128, 64)]
skirt = []
for rows, i in [(after[256]["ssb"], 1)] + [(after[g]["ssb"], 2) for g in (256, 128, 64)]:
    pts = [(r[0], r[i]) for r in rows if r[0] > 1500]; top = max(v for _, v in pts[:200])
    f6 = next(f for f, v in pts if v <= top - 6); f60 = next(f for f, v in pts if v <= top - 60)
    skirt.append(f60)
keys = ["stock", 256, 128, 64]
cpu_us = [cpu[k] for k in keys]
fig, axs = plt.subplots(1, 5, figsize=(19, 4.2))
for ax, vals, title, fmt in ((axs[0], delay, "Delay through the filter (ms)", "%.2f"), (axs[1], skirt, "Offset where a signal is 60 dB down (Hz)", "%.0f"),
                             (axs[2], [ops[k] for k in keys], "Arithmetic per sample (operations)", "%.0f"),
                             (axs[3], cpu_us, "CPU time per 12 kHz sample on the F407 (us)", "%.2f"),
                             (axs[4], [0, ram_kb[256], ram_kb[128], ram_kb[64]], "Static RAM added on the F4 (kB)", "%.1f")):
    err = None
    if ax is axs[3]:
        err = [[cpu_us[i] - cpu_lo[k] for i, k in enumerate(keys)], [cpu_hi[k] - cpu_us[i] for i, k in enumerate(keys)]]
    bars = ax.bar(names, vals, color=cols, yerr=err, capsize=4, ecolor="#555555")
    for i, (r, v) in enumerate(zip(bars, vals)):
        text = fmt % v + ("\n%.1f %%" % core(v) if ax is axs[3] else "")
        ax.text(r.get_x() + r.get_width() / 2, v + (err[1][i] if err else 0), text,
                ha="center", va="bottom", fontsize=8.5)
    ax.set_title(title, fontsize=9.5); ax.tick_params(axis="x", labelsize=9); ax.grid(axis="x", visible=False)
    ax.set_ylim(0, max(vals) * (1.75 if err else 1.15))
axs[3].set_title("CPU time per sample (us) and % of the core\n168 MHz, 1.5 cycles/op (bars 1.0-2.0)", fontsize=9)
fig.subplots_adjust(wspace=0.32)
fig.suptitle("What the convolution filter costs and buys: 2.7 kHz SSB path at 12 kHz, normal receiver against the three geometries", y=1.03)
fig.savefig(os.path.join(out, "6-cost-and-benefit.png")); plt.close(fig)
print("skirt Hz (stock, 256, 128, 64):", skirt, " delay ms:", delay)
for k, n in zip(keys, names):
    print("cpu: %-4s %3d ops  %.2f us/sample  (%.2f-%.2f)  %.1f %% of the core at 168 MHz"
          % (k, ops[k], cpu[k], cpu_lo[k], cpu_hi[k], core(cpu[k])))
