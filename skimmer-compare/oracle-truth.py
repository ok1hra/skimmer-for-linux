#!/usr/bin/env python3
"""oracle-truth.py — what R heard in a recording, for the decoder oracles.

  oracle-truth.py REC.cf32 REMOTE.log [REMOTE.log …] [--pre-s 30]
                  [--post-s 5] [--search-hz 55] > truth.txt

Step 0.1 of docs/WEAK-SIGNAL-PLAN.md: run the v2 decoder with a perfect
gate, clock or frequency where CW Skimmer Server (R) spotted a station,
and see how much more of R's stations L then reads. This builds the
oracle's file from R's spots of the recording's band and time.

R reports its frequency to 0.1 kHz, too coarse for a 45 Hz envelope, so
each spot's carrier is found in the IQ itself: 1 Hz periodogram averaged
over the PRE_S seconds before the spot, the peak within ±SEARCH_HZ of R's
frequency, parabolic interpolation. A strong neighbour within that window
can win — the "prom" column (peak over the window median, dB) shows how
clear each peak was.

Each spot becomes an interval [t − PRE_S, t + POST_S] in STREAM seconds
(from the recording's start_unix); intervals of the same call within
20 Hz that overlap merge. Output, one per line:

  t0_s t1_s carrier_hz wpm call r_snr_db prom_db nspots
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.logs import epoch, parse_spot  # noqa: E402


def meta(path):
    m = {}
    with open(path + ".meta") as fh:
        for line in fh:
            if ":" in line:
                k, v = line.split(":", 1)
                m[k.strip()] = v.strip()
    return float(m["start_unix"]), float(m["rate_hz"]), float(m["center_hz"]), \
        float(m["duration_s"])


def r_spots(paths, t0, t1, lo_hz, hi_hz):
    out = []
    for p in paths:
        with open(p, errors="replace") as fh:
            for line in fh:
                if line.startswith("#") or "\t" not in line:
                    continue
                ts, body = line.rstrip("\n").split("\t", 1)
                t = epoch(ts)
                if t is None or t < t0 or t > t1:
                    continue
                s = parse_spot(t, body)
                if s is None or s.mode != "CW":
                    continue
                if lo_hz <= s.f * 1e3 <= hi_hz:
                    out.append(s)
    return out


def carrier(iq, rate, center, t_end, pre_s, f_r, search_hz):
    """Peak of the averaged 1 Hz periodogram within ±search_hz of f_r."""
    n = int(rate)                                   # 1 s blocks, 1 Hz bins
    i1 = int(t_end * rate)
    i0 = max(0, i1 - int(pre_s * rate))
    nb = (i1 - i0) // n
    if nb < 2:
        return None
    k_r = int(round(f_r - center))                  # bin of R's frequency
    lo, hi = k_r - search_hz, k_r + search_hz + 1
    acc = np.zeros(hi - lo)
    win = np.hanning(n).astype(np.float32)
    for b in range(nb):
        x = iq[i0 + b * n:i0 + (b + 1) * n]
        X = np.fft.fft(x * win)
        idx = np.arange(lo, hi) % n                 # negative bins wrap
        acc += np.abs(X[idx]) ** 2
    j = int(np.argmax(acc))
    d = 0.0
    if 0 < j < len(acc) - 1:
        a, b, c = np.log(acc[j - 1:j + 2] + 1e-30)
        den = a - 2 * b + c
        d = 0.5 * (a - c) / den if den != 0 else 0.0
    prom = 10 * np.log10(acc[j] / max(np.median(acc), 1e-30))
    return center + lo + j + d, prom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("remote", nargs="+")
    ap.add_argument("--pre-s", type=float, default=30.0)
    ap.add_argument("--post-s", type=float, default=5.0)
    ap.add_argument("--search-hz", type=int, default=55)
    a = ap.parse_args()

    start, rate, center, dur = meta(a.rec)
    edge = 0.45 * rate
    spots = r_spots(a.remote, start, start + dur, center - edge, center + edge)
    iq = np.memmap(a.rec, dtype=np.complex64, mode="r")
    rows = []
    for s in spots:
        t = s.t - start
        got = carrier(iq, rate, center, t, a.pre_s, s.f * 1e3, a.search_hz)
        if got is None:
            continue
        fc, prom = got
        rows.append([max(0.0, t - a.pre_s), t + a.post_s, fc, s.wpm, s.call,
                     s.snr, prom, 1])
    rows.sort(key=lambda r: (r[4], r[0]))
    merged = []
    for r in rows:
        m = merged[-1] if merged else None
        if m and m[4] == r[4] and abs(m[2] - r[2]) < 20 and r[0] <= m[1]:
            m[1] = max(m[1], r[1])
            m[2] = r[2]                             # latest carrier wins
            m[3] = r[3]
            m[5] = max(m[5], r[5])
            m[6] = max(m[6], r[6])
            m[7] += 1
        else:
            merged.append(r)
    merged.sort(key=lambda r: r[0])
    print(f"# oracle truth {os.path.basename(a.rec)}: {len(spots)} R spots, "
          f"{len(merged)} intervals, pre {a.pre_s:g} s post {a.post_s:g} s")
    print("# t0_s t1_s carrier_hz wpm call r_snr_db prom_db nspots")
    for r in merged:
        print(f"{r[0]:.1f} {r[1]:.1f} {r[2]:.1f} {r[3]} {r[4]} {r[5]} "
              f"{r[6]:.1f} {r[7]}")


if __name__ == "__main__":
    main()
