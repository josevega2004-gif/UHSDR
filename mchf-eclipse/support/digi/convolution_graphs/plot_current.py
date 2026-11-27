#!/usr/bin/env python3
"""Plot current stock-vs-convolution comparisons from compare.c CSVs.

usage: plot_current.py OUTDIR CONV256.csv CONV128x16.csv CONV64x32.csv
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

FLOOR = -140.0
STOCK = "#222222"
C256 = "#1f77b4"
C128 = "#17a2a2"
C64 = "#ff8c1a"
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3,
                     "figure.dpi": 130, "savefig.bbox": "tight"})


def load(path):
    d = {"ssb": [], "cw": [], "burst_ssb": [], "burst_cw": [], "level": [], "meta": {}}
    for row in csv.reader(open(path)):
        tag = row[0]
        if tag == "meta":
            d["meta"][row[1]] = (float(row[2]), float(row[3]))
        elif tag in ("ssb", "cw", "burst_ssb", "burst_cw"):
            d[tag].append(tuple(float(x) for x in row[1:]))
        elif tag == "level":
            d["level"].append((int(row[1]), row[2], float(row[3]), float(row[4]),
                               float(row[5]), float(row[6]), float(row[7]), float(row[8])))
    return d


def curve(rows, col):
    return [r[0] for r in rows], [max(r[col], FLOOR) for r in rows]


def save(fig, outdir, name):
    path = os.path.join(outdir, name)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote", path)


def main():
    if len(sys.argv) != 5:
        raise SystemExit("usage: plot_current.py OUTDIR CONV256.csv CONV128x16.csv CONV64x32.csv")
    outdir, p256, p128, p64 = sys.argv[1:]
    os.makedirs(outdir, exist_ok=True)
    c256, c128, c64 = load(p256), load(p128), load(p64)
    convs = [(c256, "256/8", C256), (c128, "128/16", C128), (c64, "64/32", C64)]
    stock_ssb_delay = c256["meta"]["burst_ssb_delay_ms"][0]
    stock_cw_delay = c256["meta"]["burst_cw_delay_ms"][0]

    # 1. selectivity against the stock Hilbert/FIR pair
    fig, (a, b) = plt.subplots(1, 2, figsize=(13.5, 5.0))
    f, y = curve(c256["ssb"], 1)
    a.plot(f, y, color=STOCK, lw=1.8, label="stock Hilbert/FIR pair")
    f, y = curve(c256["ssb"], 2)
    a.plot(f, y, color=C256, lw=1.8, label="convolution, 1024 taps")
    a.axvspan(50, 2700, color="#2ca02c", alpha=0.08)
    a.axvline(0, color="k", lw=0.6)
    a.text(120, 10, "wanted 50-2700 Hz", color="#2ca02c", fontsize=8)
    a.set(xlim=(-6000, 6000), ylim=(-130, 15),
          xlabel="offset from tuned frequency (Hz)",
          ylabel="audio level for a unit tone (dB)",
          title="2.7 kHz SSB, USB: stock vs convolution")
    a.legend(loc="lower center", fontsize=8.5)
    f, y = curve(c256["cw"], 1)
    b.plot(f, y, color=STOCK, lw=1.8, label="stock Hilbert/FIR pair")
    f, y = curve(c256["cw"], 2)
    b.plot(f, y, color=C256, lw=1.8, label="convolution, 1024 taps")
    b.axvspan(500, 1000, color="#2ca02c", alpha=0.08)
    b.axvline(0, color="k", lw=0.6)
    b.axvline(750, color="#2ca02c", ls="--", lw=1)
    b.text(765, 32, "750 Hz note", color="#2ca02c", fontsize=8)
    b.set(xlim=(-1500, 1800), ylim=(-130, 40),
          xlabel="offset from tuned frequency (Hz)",
          ylabel="audio level for a unit tone (dB)",
          title="500 Hz CW: stock vs convolution")
    b.legend(loc="lower center", fontsize=8.5)
    fig.text(0.5, -0.04, "Measured at the point where the two receivers differ. The path audio IIR and codec come after both and are not included.",
             ha="center", fontsize=8, color="#555555")
    save(fig, outdir, "1-stock-vs-conv-selectivity.png")

    # 2. tone-onset latency
    fig, (a, b) = plt.subplots(1, 2, figsize=(13.5, 5.0))
    for ax, tag, delay_name, title in (
            (a, "burst_ssb", "burst_ssb_delay_ms", "2.7 kHz SSB, 1 kHz tone switched on"),
            (b, "burst_cw", "burst_cw_delay_ms", "500 Hz CW, 750 Hz tone switched on")):
        t, y = curve(c256[tag], 1)
        ax.plot(t, y, color=STOCK, lw=1.8,
                label="stock Hilbert/FIR: %.2f ms" % c256["meta"][delay_name][0])
        for d, label, color in convs:
            t, y = curve(d[tag], 2)
            ax.plot(t, y, color=color, lw=1.5,
                    label="conv %s: %.2f ms" % (label, d["meta"][delay_name][1]))
        ax.axhline(0.5, color="k", lw=0.5, ls=":")
        ax.axvline(0, color="k", lw=0.6)
        ax.set(xlim=(-5, 80), ylim=(-0.03, 1.25),
               xlabel="time after the signal appears (ms)",
               ylabel="audio envelope (1 = full level)", title=title)
        ax.legend(loc="lower right", fontsize=8.5, title="time to half level")
    fig.text(0.5, -0.04, "Through the firmware queues and worker at 12 kHz. AGC, audio IIR and codec are after both and are not included.",
             ha="center", fontsize=8, color="#555555")
    save(fig, outdir, "2-stock-vs-conv-latency.png")

    # 3. cost and delay, stock against the three 1024-tap geometries
    names = ["stock", "conv 256/8", "conv 128/16", "conv 64/32"]
    colors = [STOCK, C256, C128, C64]
    ssb = [stock_ssb_delay] + [d["meta"]["burst_ssb_delay_ms"][1] for d, _, _ in convs]
    cw = [stock_cw_delay] + [d["meta"]["burst_cw_delay_ms"][1] for d, _, _ in convs]
    ops = [796, 372, 427, 583]
    cpu = [o * 1.5 / 168.0 for o in ops]
    core = [c * 1.2 for c in cpu]
    ram = [0.0] + [d["meta"]["ram_bytes"][0] / 1024.0 for d, _, _ in convs]
    x = list(range(4))
    fig, axs = plt.subplots(1, 4, figsize=(18, 4.6))
    w = 0.36
    axs[0].bar([i - w / 2 for i in x], ssb, width=w, color="#1f77b4", label="SSB")
    axs[0].bar([i + w / 2 for i in x], cw, width=w, color="#ff8c1a", label="CW")
    axs[0].set_title("Delay through the filter (ms)", fontsize=9.5)
    axs[0].legend(fontsize=8)
    axs[0].set_ylim(0, max(ssb + cw) * 1.35)
    axs[1].bar(x, ops, color=colors)
    axs[1].set_title("Arithmetic per sample (operations)", fontsize=9.5)
    axs[1].set_ylim(0, max(ops) * 1.25)
    axs[2].bar(x, cpu, color=colors)
    axs[2].set_title("CPU time per 12 kHz sample (us)", fontsize=9.5)
    axs[2].set_ylim(0, max(cpu) * 1.45)
    axs[3].bar(x, ram, color=colors)
    axs[3].set_title("Static RAM added (KiB)", fontsize=9.5)
    axs[3].set_ylim(0, max(ram) * 1.25)
    for ax in axs:
        ax.set_xticks(x)
        ax.set_xticklabels(names, rotation=20, ha="right", fontsize=8)
        ax.grid(axis="x", visible=False)
    for i, v in enumerate(ssb):
        axs[0].text(i - w / 2, v + 0.3, "%.2f" % v, ha="center", va="bottom", fontsize=7.5)
    for i, v in enumerate(cw):
        axs[0].text(i + w / 2, v + 0.3, "%.2f" % v, ha="center", va="bottom", fontsize=7.5)
    for i, v in enumerate(ops):
        axs[1].text(i, v + max(ops) * 0.02, "%d" % v, ha="center", va="bottom", fontsize=8)
    for i, v in enumerate(cpu):
        axs[2].text(i, v + max(cpu) * 0.02, "%.2f\n%.1f %%" % (v, core[i]), ha="center", va="bottom", fontsize=8)
    for i, v in enumerate(ram):
        axs[3].text(i, v + max(ram) * 0.02, "%.1f" % v, ha="center", va="bottom", fontsize=8)
    fig.suptitle("What the 1024-tap convolution buys and costs: stock Hilbert/FIR baseline, 2.7 kHz SSB", y=1.03)
    fig.text(0.5, -0.06, "CPU is the 1.5 cycles/op model at 168 MHz and 12 kHz, not a target measurement. RAM is the convolution module arrays.",
             ha="center", fontsize=8, color="#555555")
    save(fig, outdir, "3-stock-vs-conv-cost.png")

    # 4. every documented SSB/CW path at its centre
    rows = c256["level"]
    labels = ["%s @ %g" % (r[2], r[3]) for r in rows]
    xs = list(range(len(rows)))
    fig, ax = plt.subplots(figsize=(16, 5.5))
    ax.plot(xs, [r[5] for r in rows], "o", color=STOCK, ms=5, label="stock Hilbert/FIR pair")
    ax.plot(xs, [r[6] for r in rows], "D", color=C256, ms=3.2, label="convolution, 1024 taps")
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=90, fontsize=6.2)
    ax.set(ylim=(-5, 30), xlim=(-1, len(rows)),
           ylabel="audio level for a unit tone (dB)",
           xlabel="filter path: width @ centre, in Hz, in the radio table order",
           title="A tone at the centre of every SSB/CW path: stock vs convolution")
    ax.legend(loc="upper right", fontsize=9)
    fig.text(0.5, -0.03, "63 rows, including the appended VARA 2750 path. Both receivers are measured at the same point.",
             ha="center", fontsize=8, color="#555555")
    save(fig, outdir, "4-stock-vs-conv-every-path.png")


if __name__ == "__main__":
    main()
