import sys, numpy as np
import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
try:
    from scipy import signal
except ImportError:
    signal = None
RATE, M = 12000.0, 1 << 17
fgrid = np.fft.fftfreq(M, 1 / RATE)

def firmware_design(N, lo, hi, window="bh4", beta=7.857, gain=2.0):
    # AudioDriver_CalcConvolutionFilterCoeffs(), rtype 1, as it stands after the fixes
    i = np.arange(N); m = 0.5 * (N - 1); pos = i - m
    ft_rad = 2 * np.pi * (hi - lo) / (2 * RATE); w_osc = np.pi * (hi + lo) / RATE
    sinc = np.sin(ft_rad * pos) / (np.pi * pos)
    if window == "bh4":
        c = np.cos(np.pi / m * i)
        w = 0.21747 + c * (-0.45325 + c * (0.28256 + c * (-0.04672)))
    else:
        w = np.kaiser(N, beta)
    return gain * sinc * w * np.exp(1j * pos * w_osc)

def min_phase(h):
    # same magnitude, zeros reflected inside the unit circle (homomorphic method); same length
    H = np.abs(np.fft.fft(h, M)); L = np.log(np.maximum(H, H.max() * 1e-7))
    c = np.fft.ifft(L); fold = np.zeros(M, complex); fold[0] = c[0]; fold[1:M // 2] = 2 * c[1:M // 2]; fold[M // 2] = c[M // 2]
    return np.fft.ifft(np.exp(np.fft.fft(fold)))[:len(h)]

def measure(h, lo, hi, tone):
    H = np.fft.fft(h, M); mag = 20 * np.log10(np.abs(H) + 1e-12)
    band = (fgrid > lo + 150) & (fgrid < hi - 150); ref = mag[band].max()
    up = np.where(fgrid > (lo + hi) / 2)[0]; up = up[np.argsort(fgrid[up])]
    f6 = fgrid[up][np.argmax(mag[up] <= ref - 6)]; f60 = fgrid[up][np.argmax(mag[up] <= ref - 60)]
    img = mag[(fgrid < -300) & (fgrid > -2400)].max() - ref
    gd = np.real(np.fft.fft(np.arange(len(h)) * h, M) / (H + 1e-30)) * 1000 / RATE
    at = lambda f: gd[np.argmin(np.abs(fgrid - f))]
    k = np.arange(len(h)); env = np.abs(np.cumsum(h * np.exp(-2j * np.pi * tone * k / RATE)))
    onset = np.argmax(env >= 0.5 * env[-1]) * 1000 / RATE
    return dict(skirt=f60 - f6, img=img, gd_mid=at((lo + hi) / 2), gd_edge=max(at(lo + 100), at(hi - 100)), onset=onset, mag=mag - ref, gd=gd)

def row(name, r):
    print("%-44s skirt %5.0f Hz  other sideband %7.1f dB  delay: mid-band %5.1f ms, 100 Hz inside an edge %5.1f ms, tone onset %5.1f ms" % (name, r["skirt"], r["img"], r["gd_mid"], r["gd_edge"], r["onset"]))

for title, lo, hi, tone in (("2.7 kHz SSB path, 50..2700 Hz", 50, 2700, 1000), ("500 Hz CW path, 500..1000 Hz", 500, 1000, 750)):
    print("\n=== %s, 12 kHz (filter only; add the block: 10 ms at geometry 256, 4.7 at 128, 2.0 at 64) ===" % title)
    res = {}
    for N in (1024, 512, 256):
        h = firmware_design(N, lo, hi)
        res["lin%d" % N] = measure(h, lo, hi, tone); row("as it is now: linear phase, %d taps" % N, res["lin%d" % N])
        res["min%d" % N] = measure(min_phase(h), lo, hi, tone); row("  same response, minimum phase, %d taps" % N, res["min%d" % N])
    for N in (512, 256, 128):
        res["k%d" % N] = measure(firmware_design(N, lo, hi, "kaiser"), lo, hi, tone); row("Kaiser window to 80 dB, linear phase, %d taps" % N, res["k%d" % N])
    res["kmin512"] = measure(min_phase(firmware_design(512, lo, hi, "kaiser")), lo, hi, tone); row("  Kaiser 80 dB, minimum phase, 512 taps", res["kmin512"])
    if signal is not None:
        # Weaver arrangement: shift the band centre to zero, an elliptic lowpass on I and on Q, shift back
        b, a = signal.ellip(8, 0.1, 80, (hi - lo) / 2, fs=RATE)
        imp = signal.lfilter(b, a, np.r_[1.0, np.zeros(4095)]) * 2.0
        h = imp * np.exp(2j * np.pi * (lo + hi) / 2 * np.arange(4096) / RATE)
        res["ellip"] = measure(h, lo, hi, tone); row("IIR: 8th order elliptic lowpass, shifted (Weaver)", res["ellip"])
    if lo == 50:
        fig, (a1, a2) = plt.subplots(1, 2, figsize=(14, 5)); o = np.argsort(fgrid); f = fgrid[o]
        show = [("lin1024", "as it is now: linear phase, 1024 taps", "#1f77b4"), ("min1024", "same response, minimum phase, 1024 taps", "#d62728"),
                ("k512", "Kaiser 80 dB, linear phase, 512 taps", "#2ca02c"), ("kmin512", "Kaiser 80 dB, minimum phase, 512 taps", "#ff8c1a")] + ([("ellip", "IIR: elliptic lowpass, shifted (Weaver)", "#7f3fbf")] if "ellip" in res else [])
        for key, label, colr in show:
            y = res[key]["mag"][o]
            if key != "ellip":   # draw the sidelobe peaks, not every null: one lobe is RATE/taps wide
                from scipy.ndimage import maximum_filter1d
                y = maximum_filter1d(y, size=max(1, M // int(key.lstrip("linmk"))))
            a1.plot(f, np.maximum(y, -150), color=colr, lw=1.5, label=label)
            inb = (f > lo + 20) & (f < hi - 20); a2.plot(f[inb], res[key]["gd"][o][inb], color=colr, lw=1.5)
        a1.set(xlim=(-1500, 4000), ylim=(-150, 8), xlabel="signal offset from the tuned frequency (Hz)", ylabel="level relative to the passband (dB), sidelobe peaks", title="Selectivity: 2.7 kHz SSB filter, USB")
        a2.set(xlim=(0, 2800), ylim=(0, 60), xlabel="audio frequency (Hz)", ylabel="delay through the filter (ms)", title="Delay across the passband (the block of buffering is extra)")
        for ax in (a1, a2): ax.grid(alpha=0.3)
        h_, l_ = a1.get_legend_handles_labels(); fig.legend(h_, l_, loc="upper center", bbox_to_anchor=(0.5, 0.03), ncol=3, frameon=False, fontsize=9)
        fig.savefig(sys.argv[1], dpi=130, bbox_inches="tight")
print("\nscipy available:", signal is not None)
