#!/usr/bin/env python3
"""read-vs-feed.py — where R's stations are lost: in the reading or the feed.

  read-vs-feed.py TRUTH RUNDIR [RUNDIR …] --band 20m [--min-prom 6]

Step 0.2 of docs/WEAK-SIGNAL-PLAN.md. TRUTH is oracle-truth.py's file for
one recording and band (R's spots as intervals, carrier from the IQ);
RUNDIR holds a replay of that recording (iq/<band>.cf32.decodes.log, its
.meta, and rows-<band>.jsonl from skimmer-tap-replay). Each R interval is

  spot   L's feed sent R's call within ±200 Hz, from t0 − 600 s (the
         feed sends a station once per fold) to t1 + 180 s
  read   not sent, but the decoder's text near the carrier (±80 Hz, the
         interval ±15 s, spaces dropped) holds the call
  near   neither; the text holds a token 1–2 edits from the call
  miss   nothing like it

— per bin of R's SNR and split by whether R saw the station calling CQ
(L's feed spots CQ callers only; R spots S&P answers too) (R's own scale), so a change can be told apart: a
better reader moves miss/near to read/spot, a better feed read to spot.
"""
import argparse
import bisect
import json
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skimcmp.logs import epoch, parse_spot  # noqa: E402

BINS = [("<10", 10), ("10-15", 15), ("15-20", 20), ("20+", 999)]
DEC_RE = re.compile(r"^(\d\d):(\d\d):(\d\d)\s+(\d+\.\d+)\s+\d+wpm\s+-?\d+dB \|(.*)\|$")


def edits(a, b, cap=3):
    if abs(len(a) - len(b)) > cap:
        return cap
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


def start_unix(rundir, band):
    with open(f"{rundir}/iq/{band}.cf32.meta") as fh:
        for line in fh:
            if line.startswith("start_unix:"):
                return float(line.split(":", 1)[1])
    raise SystemExit("no start_unix")


def load_dec(path):
    t, rows = [], []
    for line in open(path, errors="replace"):
        m = DEC_RE.match(line.rstrip("\n"))
        if m:
            t.append(int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]))
            rows.append((float(m[4]) * 1e3, m[5]))
    return t, rows


def load_spots(path, t0):
    out = {}
    for line in open(path):
        if not line.startswith('{"ev":"spot"'):
            continue
        r = json.loads(line)
        out.setdefault(r["call"], []).append((r["w"] - t0, r["hz"]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("rundir", nargs="+")
    ap.add_argument("--band", required=True)
    ap.add_argument("--min-prom", type=float, default=6.0)
    ap.add_argument("--remote", required=True, help="R's log (CQ flags)")
    a = ap.parse_args()
    ivs = []
    for line in open(a.truth):
        if line.startswith("#"):
            continue
        t0, t1, hz, wpm, call, snr, prom, n = line.split()
        if float(prom) >= a.min_prom:
            ivs.append((float(t0), float(t1), float(hz), call, int(snr)))
    t0u = start_unix(a.rundir[0], a.band)
    cqs = {}
    for line in open(a.remote, errors="replace"):
        if "\t" not in line or line[0] == "#":
            continue
        ts, body = line.rstrip("\n").split("\t", 1)
        t = epoch(ts)
        if t is None or not (t0u <= t <= t0u + 60000):
            continue
        sp = parse_spot(t, body)
        if sp and "CQ" in sp.cm.split():
            cqs.setdefault(sp.call, []).append(t - t0u)
    ivs = [iv + (any(iv[0] <= x <= iv[1] for x in cqs.get(iv[3], ())),) for iv in ivs]
    print(f"{'run':20s} {'mode':3s} {'R dB':6s} {'n':>5s} {'spot':>6s} {'read':>6s} {'near':>6s} {'miss':>6s}")
    for rd in a.rundir:
        t0u = start_unix(rd, a.band)
        dt, drows = load_dec(f"{rd}/iq/{a.band}.cf32.decodes.log")
        spots = load_spots(f"{rd}/rows-{a.band}.jsonl", t0u)
        cnt = {(b, q): [0, 0, 0, 0] for b, _ in BINS for q in (True, False)}
        for t0, t1, hz, call, snr, cq in ivs:
            b = (next(name for name, hi in BINS if snr < hi), cq)
            if any(t0 - 600 <= ts <= t1 + 180 and abs(h - hz) < 200
                   for ts, h in spots.get(call, ())):
                cnt[b][0] += 1
                continue
            i0 = bisect.bisect_left(dt, t0 - 15)
            i1 = bisect.bisect_right(dt, t1 + 15)
            txt = "".join(drows[i][1] for i in range(i0, i1)
                          if abs(drows[i][0] - hz) < 80)
            flat = txt.replace(" ", "").replace("·", "")
            if call in flat:
                cnt[b][1] += 1
            elif any(edits(tok, call) <= 2 for tok in txt.replace("·", " ").split()
                     if len(tok) >= 3):
                cnt[b][2] += 1
            else:
                cnt[b][3] += 1
        name = rd.rstrip("/").split("/")[-1]
        for q in (True, False):
            tot = [sum(cnt[(b, q)][k] for b, _ in BINS) for k in range(4)]
            for b, _ in BINS + [("all", 0)]:
                c = tot if b == "all" else cnt[(b, q)]
                n = sum(c)
                if not n:
                    continue
                print(f"{name:20s} {'CQ ' if q else 'S&P'} {b:6s} {n:5d} " +
                      " ".join(f"{100.0 * x / n:5.1f}%" for x in c))
        print()


if __name__ == "__main__":
    sys.exit(main())
