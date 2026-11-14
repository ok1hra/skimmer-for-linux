#!/usr/bin/env python3
"""kws-oracle.py — is R's call in L's audio, found without L's words?

  kws-oracle.py TRUTH RUNDIR --base BASERUN --band B --remote RLOG [--sep 3]

Step 5 of docs/WEAK-SIGNAL-PLAN.md, measured before it is built. RUNDIR is
a replay with SKIM_ORACLE_MODE=kws: its err-<band>.txt holds one "okws:"
line per 3 s on the channel that owns one of R's stations, the last 6 s of
envelope searched for R's call by forced alignment (a free reading, a word
gap, the call, a word gap, a free reading) against the best free reading:

  okws: t chan_hz CALL wpm N scale F F/N lvl_db dC NEAR dNear | DEC:d …

dC ≤ 0 is what the call costs over the free reading at its best place;
NEAR is the dictionary call nearest R's that fits best (a wrong read),
DEC the calls of R's other stations (a false alarm). For each of R's CQ
intervals the best window is taken. The call is found at threshold θ when
dC ≥ θ and it beats every neighbour by sep, except a neighbour the call
holds or that holds the call (G5V in G5VZ — that tie is the boundary's,
counted apart as "sub"). The false-alarm rate is the share of decoy
scores ≥ θ over all windows: a reader that tries M calls per window
raises M times that many. Split by the base feed's verdict (spot / read
/ near / miss, as read-vs-feed.py) and R's SNR.
"""
import argparse
import importlib.machinery
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
rvf = importlib.machinery.SourceFileLoader(
    "rvf", os.path.join(HERE, "read-vs-feed.py")).load_module()

BINS = rvf.BINS
THETAS = (-5.0, -10.0, -20.0, -40.0)


def classify(call, t0, t1, hz, spots, dt, drows):
    if any(t0 - 600 <= ts <= t1 + 180 and abs(h - hz) <= 1500
           for ts, h in spots.get(call, ())):
        return "spot"
    i0 = rvf.bisect.bisect_left(dt, t0 - 15)
    i1 = rvf.bisect.bisect_right(dt, t1 + 15)
    txt = "".join(drows[i][1] for i in range(i0, i1)
                  if abs(drows[i][0] - hz) < 80)
    if call in txt.replace(" ", "").replace("·", ""):
        return "read"
    if any(rvf.edits(tok, call) <= 2 for tok in txt.replace("·", " ").split()
           if len(tok) >= 3):
        return "near"
    return "miss"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("rundir")
    ap.add_argument("--base", required=True)
    ap.add_argument("--band", required=True)
    ap.add_argument("--remote", required=True)
    ap.add_argument("--min-prom", type=float, default=6.0)
    ap.add_argument("--sep", type=float, default=3.0)
    ap.add_argument("--dump", action="store_true",
                    help="print the per-interval rows (for pooling)")
    a = ap.parse_args()

    win = {}
    dec_all = []
    for line in open(f"{a.rundir}/err-{a.band}.txt", errors="replace"):
        if not line.startswith("okws: "):
            continue
        head, _, tail = line.partition("|")
        p = head.split()
        t, call, dc, near, dn = float(p[1]), p[3], float(p[10]), p[11], float(p[12])
        decs = [float(x.rsplit(":", 1)[1]) for x in tail.split()]
        dec_all.extend(decs)
        win.setdefault(call, []).append((t, dc, near, dn, max(decs) if decs else -999.0))

    ivs = []
    for line in open(a.truth):
        if line.startswith("#"):
            continue
        t0, t1, hz, wpm, call, snr, prom, n = line.split()
        if float(prom) >= a.min_prom:
            ivs.append((float(t0), float(t1), float(hz), call, int(snr)))
    t0u = rvf.start_unix(a.base, a.band)
    cqs = {}
    for line in open(a.remote, errors="replace"):
        if "\t" not in line or line[0] == "#":
            continue
        ts, body = line.rstrip("\n").split("\t", 1)
        t = rvf.epoch(ts)
        if t is None or not (t0u <= t <= t0u + 60000):
            continue
        sp = rvf.parse_spot(t, body)
        if sp and "CQ" in sp.cm.split():
            cqs.setdefault(sp.call, []).append(t - t0u)
    dt, drows = rvf.load_dec(f"{a.base}/iq/{a.band}.cf32.decodes.log")
    spots = rvf.load_spots(f"{a.base}/rows-{a.band}.jsonl", t0u)

    rows = []
    for t0, t1, hz, call, snr in ivs:
        if not any(t0 <= x <= t1 for x in cqs.get(call, ())):
            continue
        cls = classify(call, t0, t1, hz, spots, dt, drows)
        b = next(name for name, hi in BINS if snr < hi)
        ws = [w for w in win.get(call, ()) if t0 <= w[0] <= t1 + 8]
        if not ws:
            rows.append((cls, b, None, None, None, 0))
            continue
        # the best window; the neighbour that beats it by the most there
        best = max(ws, key=lambda w: w[1])
        sub = best[2] in call or call in best[2]
        rows.append((cls, b, best[1], best[1] - best[3], sub, len(ws)))

    if a.dump:
        for r in rows:
            print("row", a.band, *r)
        for d in dec_all:
            print("dec", d)
        return
    report(rows, dec_all, a.sep, f"{a.band} {os.path.basename(a.rundir.rstrip('/'))}")


def report(rows, dec_all, sep, title):
    print(f"{title}: R's CQ stations, best 6 s window; found = dC ≥ θ and "
          f"{sep:g} clear of every dictionary neighbour (sub = tied with a "
          f"neighbour inside the call)")
    print(f"{'base':5s} {'R dB':6s} {'n':>4s} {'win':>4s} " +
          " ".join(f"{'θ' + format(t, 'g'):>7s}" for t in THETAS) +
          f" {'sub':>4s} {'nbust':>5s}  dC p25/50/75")
    for cls in ("spot", "read", "near", "miss"):
        for b, _ in BINS:
            rs = [r for r in rows if r[0] == cls and r[1] == b]
            if not rs:
                continue
            w = [r for r in rs if r[2] is not None]
            found = [sum(1 for r in w if r[2] >= th and (r[3] >= sep or r[4]))
                     for th in THETAS]
            sub = sum(1 for r in w if r[2] >= THETAS[0] and r[4] and r[3] < sep)
            bust = sum(1 for r in w if r[2] >= THETAS[0] and not r[4] and r[3] < sep)
            d = sorted(r[2] for r in w)
            pct = (" ".join(f"{d[min(len(d) - 1, int(q * len(d)))]:6.0f}"
                            for q in (0.25, 0.5, 0.75)) if d else "-")
            print(f"{cls:5s} {b:6s} {len(rs):4d} {len(w):4d} " +
                  " ".join(f"{x:7d}" for x in found) +
                  f" {sub:4d} {bust:5d}  {pct}")
    n = len(dec_all)
    print(f"decoy windows×calls {n}: share ≥ θ " +
          " ".join(f"θ{t:g} {100.0 * sum(1 for d in dec_all if d >= t) / max(n, 1):.3f}%"
                   for t in THETAS))


if __name__ == "__main__":
    sys.exit(main())
