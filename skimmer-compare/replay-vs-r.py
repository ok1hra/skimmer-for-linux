#!/usr/bin/env python3
"""replay-vs-r.py — score offline replays of a headless recording against R.

skimmer-headless [record] writes a band's IQ (cf32 + .meta with start_unix)
while R — CW Skimmer Server — spots the same air into remote-<T>.log.
Replaying that file through a changed decoder with SKIM_REPLAY_FEED=new
traces every feed line ("feed: CALL kHz dB wpm t=<stream s>" on stderr).
This script lines those lines up with R's CQ spots of the same band and
time and reports, per bin of R's SNR, how many of R's stations each replay
caught — the offline A/B the weak-signal work is judged on, before any of
it runs live.

  replay-vs-r.py REC.cf32 TRACE [TRACE …] [--remote LOG …] [--from S]
                 [--df-khz 1.0] [--slack-s 30]

TRACE is a file of the replay's stderr; label it with NAME=PATH. --from is
the SKIM_REPLAY_FROM the replays used (stream times are relative to it).
Without --remote, every logs/remote-*.log is searched.
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.logs import epoch, parse_spot  # noqa: E402

CQ_WORDS = {"CQ", "TEST", "QRZ"}
FEED_RE = re.compile(r"^feed:\s+(\S+)\s+(\d+(?:\.\d+)?)\s+kHz\s+(-?\d+)\s+dB\s+(\d+)\s+wpm\s+t=(\d+(?:\.\d+)?)")
EDGES = [5, 10, 15, 20, 25]


def meta(path):
    m = {}
    with open(path + ".meta") as fh:
        for line in fh:
            if ":" in line:
                k, v = line.split(":", 1)
                m[k.strip()] = v.strip()
    return float(m["start_unix"]), float(m["rate_hz"]), float(m["center_hz"]), \
        float(m["duration_s"])


def r_spots(paths, t0, t1, lo_khz, hi_khz):
    out = []
    for p in paths:
        with open(p, errors="replace") as fh:
            for line in fh:
                if line.startswith("#") or "\t" not in line:
                    continue
                ts, body = line.rstrip("\n").split("\t", 1)
                t = epoch(ts)
                if t is None or not t0 <= t <= t1:
                    continue
                sp = parse_spot(t, body)
                if sp and lo_khz <= sp.f <= hi_khz and set(sp.cm.split()) & CQ_WORDS:
                    out.append(sp)
    return out


def trace(path, base):
    out = []
    with open(path, errors="replace") as fh:
        for line in fh:
            m = FEED_RE.match(line.strip())
            if m:
                out.append((base + float(m.group(5)), float(m.group(2)), m.group(1).upper()))
    return out


def bin_of(snr):
    for i, e in enumerate(EDGES):
        if snr < e:
            return i
    return len(EDGES)


def label(i):
    lo = "" if i == 0 else str(EDGES[i - 1])
    hi = "" if i == len(EDGES) else str(EDGES[i])
    return ("< %s" % hi) if not lo else ("≥ %s" % lo) if not hi else "%s–%s" % (lo, hi)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rec")
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--remote", nargs="*")
    ap.add_argument("--from", dest="frm", type=float, default=0.0)
    ap.add_argument("--df-khz", type=float, default=1.0)
    ap.add_argument("--slack-s", type=float, default=30.0)
    a = ap.parse_args()

    start, rate, centre, dur = meta(a.rec)
    base = start + a.frm
    t0, t1 = base, start + dur
    half = rate / 2 / 1000 * 0.9                     # skip the filter skirts
    lo, hi = centre / 1000 - half, centre / 1000 + half
    remote = a.remote or sorted(glob.glob(os.path.join(HERE, "logs", "remote-*.log")))
    R = r_spots(remote, t0, t1, lo, hi)
    # R's stations: one per call, at its median SNR
    st = {}
    for sp in R:
        st.setdefault(sp.call, []).append(sp)
    truth = {c: (sorted(s.snr for s in v)[len(v) // 2], v[0].f) for c, v in st.items()}

    runs = []
    for tr in a.traces:
        name, path = tr.split("=", 1) if "=" in tr else (os.path.basename(tr), tr)
        runs.append((name, trace(path, base)))

    print("recording %s: %.0f kHz ± %.0f, %.0f s, R: %d CQ spots, %d stations"
          % (os.path.basename(a.rec), centre / 1000, half, t1 - t0, len(R), len(truth)))
    head = "%-8s %4s" % ("R SNR", "R") + "".join(" %14s" % n[:14] for n, _ in runs)
    print(head)
    nb = len(EDGES) + 1
    for i in range(nb):
        calls = [c for c, (s, _) in truth.items() if bin_of(s) == i]
        row = "%-8s %4d" % (label(i), len(calls))
        for _, lines in runs:
            k = sum(any(c == lc and abs(f - lf) <= a.df_khz and t0 <= lt <= t1 + a.slack_s
                        for lt, lf, lc in lines) for c in calls
                    for f in [truth[c][1]])
            row += " %8d %4.0f%%" % (k, 100.0 * k / len(calls) if calls else 0)
        print(row)
    print("%-8s %4s" % ("own only", "") + "".join(
        " %14d" % len({lc for _, _, lc in lines} - set(truth)) for _, lines in runs))
    for name, lines in runs:
        extra = sorted({lc for _, _, lc in lines} - set(truth))
        if extra:
            print("  %s, not spotted by R: %s" % (name, " ".join(extra)))


if __name__ == "__main__":
    main()
