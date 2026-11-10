#!/usr/bin/env python3
"""read-vs-feed.py — where R's stations are lost: in the reading or the feed.

  read-vs-feed.py TRUTH RUNDIR [RUNDIR …] --band 20m [--min-prom 6]

Step 0.2 of docs/WEAK-SIGNAL-PLAN.md. TRUTH is oracle-truth.py's file for
one recording and band (R's spots as intervals, carrier from the IQ);
RUNDIR holds a replay of that recording (iq/<band>.cf32.decodes.log, its
.meta, and rows-<band>.jsonl from skimmer-tap-replay). Each R interval is

  spot   L's feed sent R's call within ±1.5 kHz (compare's df_khz), from
         t0 − 600 s (the feed sends a station once per fold) to t1 + 180 s
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


def load_rows(path, t0):
    """--why: every "c" row and feed event per call, on stream time."""
    cand, evs = {}, {}
    for line in open(path):
        r = json.loads(line)
        t = r["w"] - t0
        if r["ev"] == "c":
            st = r.get("st")
            cand.setdefault(r["call"], []).append(
                (t, r["hz"], r["dict"], r.get("gate", 0),
                 (st["sc"], st["cq"], st["hear"]) if st else None))
        else:
            evs.setdefault(r["call"], []).append((t, r["ev"], r["hz"]))
    return cand, evs


WHY = ["cand:tok", "cand:torn", "cand:glue", "nostation", "cq:none",
       "cq:far", "score", "1read", "near", "drop", "fold", "nowire", "spot:hz", "other"]
CALLING = ("CQ", "TEST", "QRZ", "CWT")


def text_shape(call, txt):
    """How the call sits in the decoder's text: a token of its own, torn
    across tokens by stretched gaps, or glued inside a longer token."""
    toks = txt.replace("\u00b7", " ").split()
    if call in toks:
        return "tok"
    if any(call in t for t in toks):
        return "glue"
    return "torn"


def why(call, t0, t1, hz, cand, evs, min_score, txt):
    """Where a call that was read in the text got lost on its way out."""
    rows = [c for c in cand.get(call, ())
            if t0 - 600 <= c[0] <= t1 + 180 and abs(c[1] - hz) < 200]
    if not rows:             # the extractor never offered it
        return "cand:" + text_shape(call, txt)
    sts = [c for c in rows if c[4]]
    if not sts:
        return "nostation"   # a candidate, never a station record
    if not any(c[3] for c in rows):
        need = lambda c: min(0.80, min_score) if c[2] else min_score
        if not any(c[4][1] for c in sts):
            # no calling marker in the text at all, or one the extractor
            # did not tie to the call (garble between, a window too short)
            flat = txt.replace(" ", "")
            return "cq:far" if any(m in flat for m in CALLING) else "cq:none"
        if not any(c[4][1] and c[4][0] >= need(c) for c in sts):
            return "score"
        if not any(c[4][1] and c[4][0] >= need(c) and (c[4][2] >= 2 or c[2])
                   for c in sts):
            return "1read"
        return "near"        # or fresh / out of band: the rest of rbn_gate
    # a held call whose station was pruned drops with hz 0: match on time
    ev = {e[1] for e in evs.get(call, ())
          if t0 - 600 <= e[0] <= t1 + 180 and
          (abs(e[2] - hz) <= 1500 or e[2] == 0)}
    if any(e[1] == "spot" and t0 - 600 <= e[0] <= t1 + 180
           for e in evs.get(call, ())):
        return "spot:hz"     # it went out, beyond --spot-df
    if "fold" in ev:
        return "fold"
    if "drop" in ev and "send" not in ev:
        return "drop"        # held, failed the gate when the hold ended
    if "send" in ev:
        return "nowire"      # sent, spot_out's dedup kept it off the wire
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("rundir", nargs="+")
    ap.add_argument("--band", required=True)
    ap.add_argument("--min-prom", type=float, default=6.0)
    ap.add_argument("--spot-df", type=float, default=1500.0,
                    help="Hz between R's carrier and L's spot that still "
                         "counts as spotted (compare.ini df_khz)")
    ap.add_argument("--remote", required=True, help="R's log (CQ flags)")
    ap.add_argument("--why", action="store_true",
                    help="break the read-not-spotted cases down by where "
                         "the feed lost them (rows' c rows and events)")
    ap.add_argument("--min-score", type=float, default=0.90,
                    help="the run's [feed] min_score, for --why")
    ap.add_argument("--examples", type=int, default=0,
                    help="--why: print N cases per reason")
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
        if a.why:
            cand, evs = load_rows(f"{rd}/rows-{a.band}.jsonl", t0u)
            wcnt = {(b, q): {w: 0 for w in WHY} for b, _ in BINS
                    for q in (True, False)}
            wex = {w: [] for w in WHY}
        cnt = {(b, q): [0, 0, 0, 0] for b, _ in BINS for q in (True, False)}
        for t0, t1, hz, call, snr, cq in ivs:
            b = (next(name for name, hi in BINS if snr < hi), cq)
            if any(t0 - 600 <= ts <= t1 + 180 and abs(h - hz) <= a.spot_df
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
                if a.why:
                    w = why(call, t0, t1, hz, cand, evs, a.min_score, txt)
                    wcnt[b][w] += 1
                    wex[w].append((t0, hz, call, snr, cq))
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
        if a.why:
            print(f"{'read, not spotted':24s} {'R dB':6s} {'n':>4s} " +
                  " ".join(f"{w:>9s}" for w in WHY))
            for q in (True, False):
                for b, _ in BINS + [("all", 0)]:
                    c = ({w: sum(wcnt[(bb, q)][w] for bb, _ in BINS)
                          for w in WHY} if b == "all" else wcnt[(b, q)])
                    n = sum(c.values())
                    if n:
                        print(f"{name:20s} {'CQ ' if q else 'S&P'} {b:6s} "
                              f"{n:4d} " +
                              " ".join(f"{c[w]:9d}" for w in WHY))
            for w in WHY:
                for t0, hz, call, snr, cq in wex[w][:a.examples]:
                    print(f"  {w:9s} t={t0:8.0f} {hz / 1e3:10.2f} {call:10s} "
                          f"R {snr:3d} dB {'CQ' if cq else 'S&P'}")
            print()


if __name__ == "__main__":
    sys.exit(main())
