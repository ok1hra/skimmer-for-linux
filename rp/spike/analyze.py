#!/usr/bin/env python3
"""analyze.py — the PC end of rp-spike's captures (raw wire orientation,
RF-inverted: conjugated here, as the skimmer's client does).

  analyze.py spec FILE.cf32 [CARRIER_HZ ...]
      level (dBFS rms), noise floor, the strongest peaks at absolute RF;
      each CARRIER_HZ (e.g. RWM 4996000) is looked up: its error in Hz tells
      whether the rate and the frequency word are what we think
  analyze.py pair A.cf32 B.cf32
      the sample lag between the two streams (cross-correlation) and the
      phase of A·conj(B) over time — constant = coherent, usable for phasing
"""
import sys

import numpy as np


def load(path):
    meta = {}
    try:
        for line in open(path + ".meta"):
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    except OSError:
        pass
    x = np.fromfile(path, dtype=np.complex64).astype(np.complex128)
    return np.conj(x), float(meta.get("rate", "0")), float(meta.get("center_hz", "0"))


def spec(path, carriers):
    x, rate, centre = load(path)
    n = len(x)
    print(f"{path}: {n} samples, {n / rate:.2f} s at {rate:.0f} S/s, centre {centre:.0f} Hz")
    rms = np.sqrt(np.mean(np.abs(x) ** 2))
    print(f"  level {20 * np.log10(rms + 1e-20):.1f} dBFS rms, peak "
          f"{20 * np.log10(np.max(np.abs(x)) + 1e-20):.1f} dBFS")
    nfft = 1 << int(np.floor(np.log2(min(n, rate))))     # ~1 Hz bins at most
    segs = n // nfft
    win = np.hanning(nfft)
    p = np.zeros(nfft)
    for k in range(segs):
        p += np.abs(np.fft.fft(x[k * nfft:(k + 1) * nfft] * win)) ** 2
    p = np.fft.fftshift(p / max(segs, 1))
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate)) + centre
    db = 10 * np.log10(p + 1e-30)
    inner = np.abs(f - centre) < 0.4 * rate
    floor = np.median(db[inner])
    print(f"  {segs} × {nfft}-point FFT ({rate / nfft:.2f} Hz bins); floor {floor:.1f} dB (median, inner 80 %)")
    order = np.argsort(db[inner])[::-1]
    fi, di = f[inner], db[inner]
    shown = []
    for i in order:
        if len(shown) >= 8:
            break
        if all(abs(fi[i] - s) > 200 for s in shown):
            shown.append(fi[i])
            print(f"    peak {fi[i]:12.1f} Hz  {di[i] - floor:5.1f} dB over floor")
    for c in carriers:
        near = np.abs(f - c) < 300
        if not near.any():
            print(f"  carrier {c:.0f}: outside the capture")
            continue
        j = np.argmax(np.where(near, db, -1e9))
        print(f"  carrier {c:.0f}: found at {f[j]:.1f} Hz ({f[j] - c:+.1f} Hz), {db[j] - floor:.1f} dB over floor")
    # the edges: a valid CIC/FIR output falls off near ±rate/2, a wrong one aliases flat
    edge = np.abs(f - centre) > 0.47 * rate
    print(f"  floor at the edges (>47 % of the rate) {np.median(db[edge]) - floor:+.1f} dB vs inner")


def pair(pa, pb):
    a, rate, _ = load(pa)
    b, _, _ = load(pb)
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    m = 1 << int(np.floor(np.log2(min(n, 1 << 20))))
    fa, fb = np.fft.fft(a[:m]), np.fft.fft(b[:m])
    xc = np.fft.ifft(fa * np.conj(fb))
    lag = int(np.argmax(np.abs(xc)))
    if lag > m // 2:
        lag -= m
    print(f"{pa} vs {pb}: {n} samples at {rate:.0f} S/s; best lag {lag} samples "
          f"(correlation {np.abs(xc).max() / (np.linalg.norm(a[:m]) * np.linalg.norm(b[:m]) + 1e-30):.3f})")
    if lag > 0:
        a, b = a[lag:], b[:len(b) - lag]
    elif lag < 0:
        b, a = b[-lag:], a[:len(a) + lag]
    blk = max(1, int(rate // 10))                       # 0.1 s blocks
    ph = []
    for k in range(len(a) // blk):
        z = np.vdot(b[k * blk:(k + 1) * blk], a[k * blk:(k + 1) * blk])
        ph.append(np.angle(z))
    ph = np.unwrap(np.array(ph))
    print(f"  phase A·conj(B) over {len(ph)} blocks of 0.1 s: mean {np.degrees(ph.mean()):.2f}°, "
          f"std {np.degrees(ph.std()):.3f}°, drift {np.degrees(ph[-1] - ph[0]) if len(ph) else 0:.3f}°")
    ra = np.sqrt(np.mean(np.abs(a) ** 2))
    rb = np.sqrt(np.mean(np.abs(b) ** 2))
    print(f"  level A {20 * np.log10(ra):.2f} dBFS, B {20 * np.log10(rb):.2f} dBFS")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "spec":
        spec(sys.argv[2], [float(c) for c in sys.argv[3:]])
    elif len(sys.argv) == 4 and sys.argv[1] == "pair":
        pair(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)
