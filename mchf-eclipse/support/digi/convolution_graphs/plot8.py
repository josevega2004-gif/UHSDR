import csv, sys
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
from scipy.ndimage import maximum_filter1d
def load(name):
    d = {"ssb": [], "cw": [], "burst_ssb": [], "burst_cw": [], "meta": {}}
    for r in csv.reader(open(name)):
        if r[0] == "meta": d["meta"][r[1]] = (float(r[2]), float(r[3]))
        elif r[0] in d: d[r[0]].append(tuple(float(x) for x in r[1:]))
    return d
mn = {g: load("min_%d.csv" % g) for g in (256, 128, 64)}; ln = {g: load("lin_%d.csv" % g) for g in (256, 128, 64)}
C = {"stock": "#222222", "lin": "#9aa0a6", 256: "#1f77b4", 128: "#17a2a2", 64: "#ff8c1a"}
plt.rcParams.update({"font.size": 10, "axes.grid": True, "grid.alpha": 0.3})
fig, (a, b, c) = plt.subplots(1, 3, figsize=(18, 5.2))
f = [r[0] for r in mn[256]["ssb"]]; lobe = 6          # 2 Hz rows: one sidelobe of 1024 taps at 12 kHz is about 12 Hz
a.plot(f, [max(v, -150) for v in maximum_filter1d([r[1] for r in mn[256]["ssb"]], 3)], color=C["stock"], lw=1.6, label="normal receiver")
a.plot(f, [max(v, -150) for v in maximum_filter1d([r[2] for r in ln[256]["ssb"]], lobe)], color=C["lin"], lw=3.0, label="convolution, linear phase (geometry 256)")
a.plot(f, [max(v, -150) for v in maximum_filter1d([r[2] for r in mn[256]["ssb"]], lobe)], color=C[256], lw=1.3, label="convolution, minimum phase (geometry 256)")
a.set(xlim=(-1500, 4500), ylim=(-150, 15), xlabel="signal offset from the tuned frequency (Hz)", ylabel="audio level for a unit tone (dB), sidelobe peaks",
      title="Selectivity is unchanged: 2.7 kHz SSB filter, USB")
a.legend(loc="lower center", fontsize=8.5)
for ax, tag, title in ((b, "burst_ssb", "2.7 kHz SSB path: 1 kHz tone switched on at t = 0"), (c, "burst_cw", "500 Hz CW path: 750 Hz tone switched on at t = 0")):
    t = [r[0] for r in mn[256][tag]]
    ax.plot(t, [r[1] for r in mn[256][tag]], color=C["stock"], lw=1.8, label="normal receiver: %.2f ms" % mn[256]["meta"][tag + "_delay_ms"][0])
    ax.plot(t, [r[2] for r in ln[256][tag]], color=C["lin"], lw=2.2, label="linear phase, geometry 256: %.1f ms" % ln[256]["meta"][tag + "_delay_ms"][1])
    for g in (256, 128, 64):
        ax.plot(t, [r[2] for r in mn[g][tag]], color=C[g], lw=1.5, label="minimum phase, geometry %d: %.1f ms" % (g, mn[g]["meta"][tag + "_delay_ms"][1]))
    ax.axhline(0.5, color="k", lw=0.5, ls=":"); ax.axvline(0, color="k", lw=0.6)
    ax.set(xlim=(-5, 70), ylim=(-0.03, 1.35), xlabel="time after the signal appears (ms)", title=title)
    ax.legend(loc="lower right", fontsize=8.5, title="time to half level")
b.set_ylabel("audio envelope (1 = full level)")
fig.text(0.5, -0.03, "Through the firmware's own design code, queues and worker at 12 kHz, on a host. The path's audio IIR, the AGC and the codec come after both receivers and are not included.", ha="center", fontsize=8.5, color="#555555")
fig.savefig(sys.argv[1], dpi=130, bbox_inches="tight")
