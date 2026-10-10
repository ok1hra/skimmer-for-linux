#!/usr/bin/env python3
"""score.py — replay spots (L) against CW Skimmer's own spots (R), CQ WW 2012.

  score.py --band 20m --hours 2012-11-25T11[,2012-11-25T12 ...]
           NAME=SPOTS_GLOB [NAME=SPOTS_GLOB ...] [--json OUT]

The unit is call × band × hour. A unit is

  TRUE    the call sent a log, >= 2 distinct logs worked it, or >= 2 RBN
          skimmers other than OL5Q spotted it (logs.py)
  BUST    not TRUE, and a TRUE call within edit distance 2 was spotted by
          either skimmer in the same band/hour within 300 Hz — a misread
  UNCONF  neither: not provably right, not provably a misread

Recall is counted against U = TRUE units heard by R or by ANY variant, so
every variant and R are scored on the same set. Bust rate = BUST units /
all units of that skimmer. Bins split R's TRUE units by R's SNR: the share
of them each variant heard. Frequencies of L are shifted by the median L−R
offset of the common TRUE units before the 300 Hz test (the recording's
centre and CW Skimmer's spot frequency differ by a constant).

R = spots.txt (default ~/.cache/skimmer-cqww2012/spots.txt), L = the lines
skimmer-replay writes with SKIM_REPLAY_SPOTS.
"""
import argparse
import collections
import datetime
import glob
import json
import math
import os
import re
import sys

CACHE = os.path.expanduser("~/.cache/skimmer-cqww2012")
MON = {m: i + 1 for i, m in enumerate(
    "I II III IV V VI VII VIII IX X XI XII".split())}
R_RE = re.compile(r"^\s*(\d+\.\d)\s+(\S+)\s+(\d+)-([IVX]+)-(\d{4})\s+(\d\d)(\d\d)Z\s+(-?\d+) dB\s+(\d+) WPM\s*(\S*)")
SNR_EDGES = [5, 10, 15, 20, 25, 30]
BANDS = ((1800, 2000, "160m"), (3500, 4000, "80m"), (7000, 7300, "40m"),
         (14000, 14350, "20m"), (21000, 21450, "15m"), (28000, 29700, "10m"))


def band_of(khz):
    for lo, hi, b in BANDS:
        if lo <= khz <= hi:
            return b
    return None


Spot = collections.namedtuple("Spot", "t khz call snr wpm cq")


def read_r(path, band, hours):
    out = []
    with open(path, errors="replace") as fh:
        for line in fh:
            m = R_RE.match(line)
            if not m:
                continue
            khz = float(m.group(1))
            if band_of(khz) != band:
                continue
            t = datetime.datetime(int(m.group(5)), MON[m.group(4)], int(m.group(3)),
                                  int(m.group(6)), int(m.group(7)),
                                  tzinfo=datetime.timezone.utc).timestamp()
            if int(t // 3600) not in hours:
                continue
            out.append(Spot(t + 30, khz, m.group(2).upper(), int(m.group(8)),
                            int(m.group(9)), m.group(10) in ("CQ", "DE")))
    return out


def read_l(paths, band, hours):
    out = []
    for p in paths:
        with open(p) as fh:
            for line in fh:
                f = line.split()
                if len(f) < 6:
                    continue
                t = datetime.datetime.strptime(f[0], "%Y-%m-%dT%H:%M:%SZ").replace(
                    tzinfo=datetime.timezone.utc).timestamp()
                khz = float(f[1])
                if band_of(khz) != band or int(t // 3600) not in hours:
                    continue
                out.append(Spot(t, khz, f[2].upper(), float(f[3]), float(f[4]), f[5] == "CQ"))
    return out


def units(spots):
    """(call, hour) -> list of spots."""
    u = collections.defaultdict(list)
    for s in spots:
        u[(s.call, int(s.t // 3600))].append(s)
    return u


def edit(a, b):
    if abs(len(a) - len(b)) > 2:
        return 3
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def mcnemar(b, c):
    """Two-sided exact McNemar p for b vs c discordant pairs."""
    n = b + c
    if n == 0:
        return 1.0
    k = min(b, c)
    p = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def evaluate(band, hours_iso, variants, r_path=None, truth_path=None,
             t_from=None, t_to=None):
    """Score variants {name: [spot files]} against R. Returns a dict with
    R, V (units), lab (verdicts), U, rows, bins, order, off, is_true."""
    r_path = r_path or os.path.join(CACHE, "spots.txt")
    truth_path = truth_path or os.path.join(CACHE, "truth.json")

    hours = {int(datetime.datetime.strptime(h, "%Y-%m-%dT%H").replace(
        tzinfo=datetime.timezone.utc).timestamp() // 3600) for h in hours_iso}
    with open(truth_path) as fh:
        tr = json.load(fh)
    senders = set(tr["senders"])
    worked = tr["worked"]

    rbn = tr.get("rbn", {})

    def is_true(call):
        return call in senders or worked.get(call, 0) >= 2 or rbn.get(call, 0) >= 2

    def utc(x):
        return datetime.datetime.strptime(x, "%Y-%m-%dT%H:%M").replace(
            tzinfo=datetime.timezone.utc).timestamp()
    lo = utc(t_from) if t_from else -1e18
    hi = utc(t_to) if t_to else 1e18

    def win(spots):
        return [s for s in spots if lo <= s.t < hi]

    R = units(win(read_r(r_path, band, hours)))
    V = {name: units(win(read_l(paths, band, hours))) for name, paths in variants.items()}

    # frequency offset of each variant vs R, from common TRUE units
    off = {}
    for name, L in V.items():
        d = []
        for k in set(L) & set(R):
            if is_true(k[0]):
                rf = sorted(s.khz for s in R[k])[len(R[k]) // 2]
                d.append(min((s.khz - rf for s in L[k]), key=abs))
        d.sort()
        off[name] = d[len(d) // 2] if d else 0.0

    # TRUE spots per hour, all skimmers, offset-corrected (for the bust test)
    true_near = collections.defaultdict(list)       # hour -> [(khz, call)]
    for src, U, o in [("R", R, 0.0)] + [(n, L, off[n]) for n, L in V.items()]:
        for (call, h), ss in U.items():
            if is_true(call):
                for s in ss:
                    true_near[h].append((s.khz - o, call))

    def verdict(call, h, ss, o):
        if is_true(call):
            return "TRUE"
        for s in ss:
            for khz, tc in true_near[h]:
                if abs(khz - (s.khz - o)) <= 0.3 and tc != call and edit(call, tc) <= 2:
                    return "BUST"
        return "UNCONF"

    lab = {"R": {k: verdict(k[0], k[1], ss, 0.0) for k, ss in R.items()}}
    for n, L in V.items():
        lab[n] = {k: verdict(k[0], k[1], ss, off[n]) for k, ss in L.items()}

    U = {k for src in lab for k, v in lab[src].items() if v == "TRUE"}
    rows = {}
    for src in lab:
        heard = set(lab[src])
        c = collections.Counter(lab[src].values())
        tot = sum(c.values())
        rows[src] = {
            "units": tot,
            "true": c["TRUE"], "bust": c["BUST"], "unconf": c["UNCONF"],
            "recall": len(heard & U) / len(U) if U else 0,
            "bust_rate": c["BUST"] / tot if tot else 0,
            "unconf_rate": c["UNCONF"] / tot if tot else 0,
            "offset_khz": off.get(src, 0.0),
        }

    # bins of R's SNR: share of R's TRUE units each variant heard
    def rbin(k):
        snr = max(s.snr for s in R[k])
        for e in SNR_EDGES:
            if snr < e:
                return f"<{e}"
        return f">={SNR_EDGES[-1]}"
    order = [f"<{e}" for e in SNR_EDGES] + [f">={SNR_EDGES[-1]}"]
    rtrue = [k for k, v in lab["R"].items() if v == "TRUE"]
    bins = {b: [k for k in rtrue if rbin(k) == b] for b in order}
    for n in V:
        rows[n]["bins"] = {b: (sum(1 for k in ks if k in lab[n]) / len(ks) if ks else None, len(ks))
                           for b, ks in bins.items()}
        only_l = sum(1 for k in U if k in lab[n] and k not in lab["R"])
        only_r = sum(1 for k in U if k in lab["R"] and k not in lab[n])
        rows[n]["only_l"], rows[n]["only_r"] = only_l, only_r
        rows[n]["mcnemar_p"] = mcnemar(only_l, only_r)
        lat = sorted(min(s.t for s in V[n][k]) - min(s.t for s in R[k])
                     for k in U if k in V[n] and k in R)
        rows[n]["latency_med_s"] = lat[len(lat) // 2] if lat else None

    return {"R": R, "V": V, "lab": lab, "U": U, "rows": rows, "bins": bins,
            "order": order, "off": off, "is_true": is_true}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--band", required=True)
    ap.add_argument("--hours", required=True,
                    help="UTC hours, YYYY-MM-DDTHH[,YYYY-MM-DDTHH...]")
    ap.add_argument("--r", default=os.path.join(CACHE, "spots.txt"))
    ap.add_argument("--truth", default=os.path.join(CACHE, "truth.json"))
    ap.add_argument("--from", dest="t_from", help="UTC YYYY-MM-DDTHH:MM, spots before are ignored")
    ap.add_argument("--to", dest="t_to", help="UTC YYYY-MM-DDTHH:MM, spots from here on are ignored")
    ap.add_argument("--json")
    ap.add_argument("--list", action="store_true",
                    help="print every unit's verdict")
    ap.add_argument("variants", nargs="+", metavar="NAME=GLOB")
    a = ap.parse_args()
    variants = {}
    for spec in a.variants:
        name, _, pat = spec.partition("=")
        paths = sorted(glob.glob(pat))
        if not paths:
            sys.exit(f"{name}: no file matches {pat}")
        variants[name] = paths
    a.hours = a.hours.split(",")
    ev = evaluate(a.band, a.hours, variants, a.r, a.truth, a.t_from, a.t_to)
    R, V, lab, U, rows, bins, order = (ev[k] for k in ("R", "V", "lab", "U", "rows", "bins", "order"))

    print(f"band {a.band}, hours {' '.join(a.hours)}: U = {len(U)} TRUE units")
    print(f"{'':10} {'units':>6} {'recall':>7} {'bust':>6} {'unconf':>7} "
          f"{'L-only':>7} {'R-only':>7} {'p':>7} {'lat s':>6} {'off kHz':>8}")
    for src, r in rows.items():
        extra = (f"{r['only_l']:7d} {r['only_r']:7d} {r['mcnemar_p']:7.3f} "
                 f"{(r['latency_med_s'] or 0):6.0f} {r['offset_khz']:+8.2f}") if src != "R" else ""
        print(f"{src:10} {r['units']:6d} {100 * r['recall']:6.1f}% {100 * r['bust_rate']:5.1f}% "
              f"{100 * r['unconf_rate']:6.1f}% {extra}")
    print("\nshare of R's TRUE units heard, by R's SNR:")
    print(f"{'':10} " + " ".join(f"{b:>9}" for b in order))
    print(f"{'(n)':10} " + " ".join(f"{len(bins[b]):9d}" for b in order))
    for n in V:
        print(f"{n:10} " + " ".join(
            f"{100 * rows[n]['bins'][b][0]:8.1f}%" if rows[n]["bins"][b][0] is not None else f"{'-':>9}"
            for b in order))
    if a.list:
        for src in lab:
            for (call, h), v in sorted(lab[src].items()):
                print(f"{src}\t{call}\t{h}\t{v}")
    if a.json:
        with open(a.json, "w") as fh:
            json.dump({"band": a.band, "hours": a.hours, "U": len(U), "rows": rows}, fh, indent=1)


if __name__ == "__main__":
    main()
