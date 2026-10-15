#!/usr/bin/env python3
"""live-breakdown.py — where the live skimmer loses against CW Skimmer Server.

  live-breakdown.py EVENTS.json [EVENTS.json …] [--taps DIR] [--examples 12]

EVENTS.json is compare-web's /api/events for a range (one session each).
For every real station R spotted and L did not (only-R, inU), the decode
tap of that band (headless learn=true: what the extractor ate, with UTC
wall time) around R's frequency and time says which basket — the live
counterpart of cqww/breakdown.py:

  a-silent    no decoded text there at all
  a-garbage   text, but no token within edit distance 2 of the call
  b-misread   a token 1..2 edits from the call (torn or mutated)
  c-read      the exact call is in the text: extractor / station / gate lost it

Every L bust (a bust pair L lost) is typed in Morse like cqww/breakdown.py.
Output: baskets × R's SNR bin, × band, and examples.
"""
import argparse
import bisect
import collections
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "cqww"))
from breakdown import bust_kind, tokens  # noqa: E402

DF_KHZ = 0.25           # L's decode frequency vs R's spot (centre conventions)
DT_S = 150              # around R's first..last spot
SNR_EDGES = [10, 15, 20, 25, 30]


def edit(a, b):
    """Levenshtein distance."""
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def snr_bin(s):
    if s is None:
        return "?"
    lo = None
    for e in SNR_EDGES:
        if s < e:
            return f"<{e}" if lo is None else f"{lo}-{e}"
        lo = e
    return f"{lo}+"


def load_taps(tapdir, band, windows):
    """[(t, khz, ix, text)] sorted by t, only lines inside some event window."""
    if not windows:
        return []
    windows.sort()
    starts = [w[0] for w in windows]
    span = max(w[1] - w[0] for w in windows)
    out = []
    for path in sorted(glob.glob(os.path.join(tapdir, f"tap-{band}-*.log"))):
        with open(path, errors="replace") as fh:
            for line in fh:
                if line[0] != "T":
                    continue
                p = line.split(" ", 9)
                if len(p) < 10:
                    continue
                t = int(p[2]) / 1e6
                i = bisect.bisect_right(starts, t)
                j = bisect.bisect_left(starts, t - span)
                khz = float(p[4]) / 1e3
                if not any(windows[k][0] <= t <= windows[k][1]
                           and abs(windows[k][2] - khz) <= DF_KHZ
                           for k in range(j, i)):
                    continue
                txt = p[9].rstrip("\n")
                if txt.startswith("|") and txt.endswith("|"):
                    txt = txt[1:-1]
                out.append((t, khz, int(p[3]), txt))
    out.sort()
    return out


def texts_near(dec, times, khz, t_lo, t_hi):
    """Concatenated text per extractor slot near (khz, t_lo..t_hi)."""
    streams = collections.defaultdict(list)
    for k in range(bisect.bisect_left(times, t_lo), bisect.bisect_right(times, t_hi)):
        t, f, ix, txt = dec[k]
        if abs(f - khz) <= DF_KHZ:
            streams[ix].append(txt)
    return ["".join(v) for v in streams.values()]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("events", nargs="+")
    ap.add_argument("--taps", default=os.path.expanduser(
        "~/.local/share/skimmer-for-linux/headless/learn"))
    ap.add_argument("--examples", type=int, default=12)
    ap.add_argument("--dump", help="write every miss with its basket (JSON lines)")
    a = ap.parse_args()

    evs = []
    for p in a.events:
        with open(p) as fh:
            evs += json.load(fh)["events"]
    miss = [e for e in evs if e["cat"] == "only-R" and e["inU"] and e["R"]]
    busts = [e for e in evs if e["cat"] == "bust" and e.get("bL") and e["L"] and e["R"]]
    print(f"events {len(evs)}: only-R real {len(miss)}, L busts {len(busts)}")

    by_band = collections.defaultdict(list)
    for e in miss:
        by_band[e["band"]].append(e)

    basket = collections.Counter()
    by_snr = collections.defaultdict(collections.Counter)
    by_bnd = collections.defaultdict(collections.Counter)
    ex = collections.defaultdict(list)
    dump = open(a.dump, "w") if a.dump else None
    for band, es in sorted(by_band.items()):
        wins = [(e["R"]["t0"] - DT_S, e["R"]["t1"] + DT_S, e["R"]["f"]) for e in es]
        dec = load_taps(a.taps, band, wins)
        times = [d[0] for d in dec]
        print(f"  {band}: {len(es)} misses, {len(dec)} tap lines in their windows",
              file=sys.stderr)
        for e in es:
            call, r = e["call"], e["R"]
            texts = [s for s in texts_near(dec, times, r["f"], r["t0"] - DT_S,
                                           r["t1"] + DT_S) if s.strip("· ")]
            toks = [w for s in texts for w in tokens(s)]
            if not texts:
                b = "a-silent"
            elif call in toks or any(call in s for s in texts):
                b = "c-read"
            else:
                d = min((edit(call, w) for w in toks), default=9)
                b = "b-misread" if d <= 2 else "a-garbage"
            basket[b] += 1
            if dump:
                dump.write(json.dumps({"band": band, "call": call, "basket": b, "f": r["f"],
                                       "t0": r["t0"], "t1": r["t1"], "snr": r.get("snr"),
                                       "wpm": r.get("wpm")}) + "\n")
            by_snr[b][snr_bin(r.get("snr"))] += 1
            by_bnd[b][band] += 1
            if len(ex[b]) < a.examples:
                near = min(toks, key=lambda w: edit(call, w)) if toks else ""
                ex[b].append(f"{band:4} {call:10} {r['f']:8.1f} {r.get('snr')!s:>3} dB "
                             f"{r.get('wpm')!s:>3} wpm  nearest '{near}'  text: "
                             f"{' | '.join(s[-70:] for s in texts)[:170]}")

    bins = ["<10", "10-15", "15-20", "20-25", "25-30", "30+", "?"]
    print(f"\nmisses (only-R, real): {len(miss)}")
    print(f"  {'basket':10} {'n':>5}   " + " ".join(f"{x:>6}" for x in bins))
    for b in sorted(basket):
        print(f"  {b:10} {basket[b]:5d}   " + " ".join(f"{by_snr[b][x]:6d}" for x in bins))
    bands = sorted({e['band'] for e in miss}, key=lambda s: -float(s.rstrip('m')))
    print(f"\n  {'basket':10} " + " ".join(f"{x:>6}" for x in bands))
    for b in sorted(basket):
        print(f"  {b:10} " + " ".join(f"{by_bnd[b][x]:6d}" for x in bands))
    for b in sorted(ex):
        print(f"\n[{b}]")
        for line in ex[b]:
            print("  " + line)

    kinds = collections.Counter()
    bex = collections.defaultdict(list)
    for e in busts:
        lc, rc = e["L"]["call"], e["R"]["call"]
        k = bust_kind(lc, rc)
        kinds[k] += 1
        if len(bex[k]) < 6:
            bex[k].append(f"{e['band']:4} L {lc:10} R {rc:10} {e['R'].get('snr')!s:>3} dB")
    print(f"\nL busts by Morse kind: {len(busts)}")
    for k, n in kinds.most_common():
        print(f"  {k:16} {n:4d}   e.g. " + "; ".join(x.split('  ')[0] for x in bex[k][:3]))


if __name__ == "__main__":
    main()
