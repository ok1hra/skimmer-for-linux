#!/usr/bin/env python3
"""combine-oracle.py — how far soft-combining the repeats could carry a call.

  combine-oracle.py TRUTH RUNDIR --band B --remote RLOG [--fit 5 --sep 3 --ev 2]

Step 6 of docs/WEAK-SIGNAL-PLAN.md, measured before it is built. RUNDIR is
a replay with SKIM_ORACLE_MODE=combine: its err-<band>.txt holds one
"ocomb:" line per word (and per word with the one before, for torn calls)
the decoder closed inside an R interval — R's call laid onto the word's
samples (forced, relative to the free reading of the span: ≤ 0), the best
dictionary call near it or near the word, and the evidence per sample:

  ocomb: t chan_hz CALL word k N sf dC dRaw COMP dComp ev

For each of R's CQ intervals the spans are taken best-first by dC, never
two that overlap, up to three, and only spans that could be a copy of the
call at all (dC ≥ −3·fit). One reading passes as the word re-read does
(SKIM_V2_RESCORE): dC ≥ −fit, dC − dComp ≥ sep, ev ≥ ev. Combined, n
readings pass when their mean dC ≥ −fit and Σ (dC − dComp) ≥ sep. The
competitor is each span's own best, so the combined margin is a lower
bound; "nb" counts the intervals where the neighbours, summed the same
way, beat R's call. Because the fit is a mean over spans taken
best-first, more readings can only lower it: M1 / M≤3 drop the fit and
count margin alone (one reading / up to three summed) — the most that
combining could add, with no guard against a wrong neighbour. d1 and m1 are the best span's dC and margin
(percentiles 10/25/50/75/90). Split by what the base feed made of the
station (read-vs-feed.py's spot / read / near / miss) and by R's SNR.
"""
import argparse
import importlib.machinery
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
rvf = importlib.machinery.SourceFileLoader(
    "rvf", os.path.join(HERE, "read-vs-feed.py")).load_module()

BINS = rvf.BINS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("truth")
    ap.add_argument("rundir")
    ap.add_argument("--base", help="the base run for spot/read classes "
                    "(default RUNDIR)")
    ap.add_argument("--band", required=True)
    ap.add_argument("--remote", required=True)
    ap.add_argument("--min-prom", type=float, default=6.0)
    ap.add_argument("--fit", type=float, default=5.0)
    ap.add_argument("--sep", type=float, default=3.0)
    ap.add_argument("--ev", type=float, default=2.0)
    a = ap.parse_args()
    base = a.base or a.rundir

    occ = {}
    for line in open(f"{a.rundir}/err-{a.band}.txt", errors="replace"):
        if not line.startswith("ocomb: "):
            continue
        p = line.split()
        t, call, N = float(p[1]), p[3], int(p[6])
        dc, dcomp, ev = float(p[8]), float(p[11]), float(p[12])
        if dc <= -900:
            continue
        occ.setdefault(call, []).append((t - N / 250.0, t, dc, dcomp, ev))

    ivs = []
    for line in open(a.truth):
        if line.startswith("#"):
            continue
        t0, t1, hz, wpm, call, snr, prom, n = line.split()
        if float(prom) >= a.min_prom:
            ivs.append((float(t0), float(t1), float(hz), call, int(snr)))
    t0u = rvf.start_unix(base, a.band)
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
    dt, drows = rvf.load_dec(f"{base}/iq/{a.band}.cf32.decodes.log")
    spots = rvf.load_spots(f"{base}/rows-{a.band}.jsonl", t0u)

    KEYS = ["n", "any", "1", "2", "3", "nb", "M1", "M3"]
    d1s, m1s = {}, {}
    cnt = {}
    for t0, t1, hz, call, snr in ivs:
        if not any(t0 <= x <= t1 for x in cqs.get(call, ())):
            continue
        # the base feed's verdict, as read-vs-feed.py has it
        if any(t0 - 600 <= ts <= t1 + 180 and abs(h - hz) <= 1500
               for ts, h in spots.get(call, ())):
            cls = "spot"
        else:
            i0 = rvf.bisect.bisect_left(dt, t0 - 15)
            i1 = rvf.bisect.bisect_right(dt, t1 + 15)
            txt = "".join(drows[i][1] for i in range(i0, i1)
                          if abs(drows[i][0] - hz) < 80)
            flat = txt.replace(" ", "").replace("·", "")
            if call in flat:
                cls = "read"
            elif any(rvf.edits(tok, call) <= 2
                     for tok in txt.replace("·", " ").split()
                     if len(tok) >= 3):
                cls = "near"
            else:
                cls = "miss"
        b = next(name for name, hi in BINS if snr < hi)
        c = cnt.setdefault((cls, b), dict.fromkeys(KEYS, 0))
        c["n"] += 1
        spans = [o for o in occ.get(call, ()) if t0 - 2 <= o[1] <= t1 + 8 and
                 o[4] >= a.ev]
        if not spans:
            continue
        c["any"] += 1
        spans.sort(key=lambda o: -o[2])
        d1s.setdefault((cls, b), []).append(spans[0][2])
        m1s.setdefault((cls, b), []).append(max(-999.0, spans[0][2] - spans[0][3]))
        pick = []
        for o in (o for o in spans if o[2] >= -3 * a.fit):
            if all(o[1] <= q[0] or o[0] >= q[1] for q in pick):
                pick.append(o)
            if len(pick) == 3:
                break
        best = 0
        for n in (1, 2, 3):
            if len(pick) < n:
                break
            sel = pick[:n]
            mean_dc = sum(o[2] for o in sel) / n
            marg = sum(o[2] - o[3] for o in sel)
            if mean_dc >= -a.fit and marg >= a.sep:
                best = n
                break
        if best:
            c[str(best)] += 1
        if sum(o[3] - o[2] for o in pick) >= a.sep:
            c["nb"] += 1
        # margin only: R's call against its neighbours, no fit to the free
        # reading (the fit is a mean, so more readings can never raise it)
        if pick and pick[0][2] - pick[0][3] >= a.sep:
            c["M1"] += 1
        if pick and sum(o[2] - o[3] for o in pick) >= a.sep:
            c["M3"] += 1
    print(f"{a.band} {os.path.basename(a.rundir.rstrip('/'))}: R's CQ "
          f"stations; passes with 1 / 2 / 3 readings (cumulative), nb = a "
          f"neighbour wins the 3-sum")
    def pct(v):
        v = sorted(v)
        return " ".join(f"{v[min(len(v) - 1, int(q * len(v)))]:6.0f}"
                        for q in (0.1, 0.25, 0.5, 0.75, 0.9)) if v else "-"
    print(f"{'base':5s} {'R dB':6s} {'n':>4s} {'spans':>6s} {'1':>5s} "
          f"{'≤2':>5s} {'≤3':>5s} {'nb':>5s} {'M1':>5s} {'M≤3':>5s}  d1 / m1")
    for cls in ("spot", "read", "near", "miss"):
        for b, _ in BINS:
            c = cnt.get((cls, b))
            if not c:
                continue
            n = c["n"]
            print(f"{cls:5s} {b:6s} {n:4d} {c['any']:6d} {c['1']:5d} "
                  f"{c['1'] + c['2']:5d} {c['1'] + c['2'] + c['3']:5d} "
                  f"{c['nb']:5d} {c['M1']:5d} {c['M3']:5d}  {pct(d1s.get((cls, b), []))} / "
                  f"{pct(m1s.get((cls, b), []))}")


if __name__ == "__main__":
    sys.exit(main())
