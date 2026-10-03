#!/usr/bin/env python3
"""replay-eval.py — feed policies measured on the air L actually heard.

skimmer-headless with [feed] learn=true records a decode tap per band
(gatelog.h). skimmer-tap-replay feeds a tap back through the same
extractor → station table → RBN gate under any policy. This runs the
replays and puts each policy's spots into compare IN PLACE OF L's real
feed, over the stretch the taps cover:

  faithfulness  the default policy's replayed spots against the spots L
                really sent then (same call and band, ±10 s, ±150 Hz) — a
                replay that does not reproduce the live feed proves nothing
  per policy    L's spots, episodes, recall L/R and bust L/R under compare's
                referee (RBN → MASTER.SCP) and the strict one (RBN or the
                other skimmer), over a FIXED set of stations: the stations
                any policy caught, or R, or confirmed — so a policy that
                drops a real station is charged for it

  ./replay-eval.py                                  default vs fresh_s 60/120
  ./replay-eval.py --policy base= --policy fresh90=--fresh-s=90
  ./replay-eval.py --taps 20261003-115202           one headless start only

The replays run inside the flatpak SDK when ~/.cache/skimmer-flatpak exists
(Dan's Debian 12), natively otherwise. Rows land in --out (scratch).
"""
import argparse
import copy
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from skimcmp.arbiter import RbnIndex, load_scp                 # noqa: E402
from skimcmp.config import Config                              # noqa: E402
from skimcmp.logs import Spot, Store                           # noqa: E402
from skimcmp.match import analyze                              # noqa: E402
from skimcmp.windows import loose_bands                        # noqa: E402

TREE = os.path.dirname(os.path.dirname(HERE))
LEARN = os.path.expanduser("~/.local/share/skimmer-for-linux/headless/learn")
FLATPAK = os.path.expanduser("~/.cache/skimmer-flatpak")
CONFIRM_S = 10 * 60          # compare's referee window: the last stretch of a
                             # tap has no evidence yet, so it is not scored
MATCH_S, MATCH_KHZ = 10, 0.15


def replay(tap, rows, args):
    exe = os.path.join(TREE, "builddir", "skimmer-tap-replay")
    cmd = [exe, "--rows", rows] + args + [tap]
    if os.path.isdir(FLATPAK) and not os.path.exists("/.flatpak-info"):
        sh = "LD_LIBRARY_PATH=%s/deps/lib %s" % (
            FLATPAK, " ".join("'%s'" % c for c in cmd))
        cmd = ["flatpak", "run", "--user", "--filesystem=home", "--filesystem=/tmp",
               "--share=network", "--command=bash", "org.gnome.Sdk//50", "-c", sh]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("tap-replay failed on %s:\n%s" % (tap, r.stderr[-2000:]))
    return r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ""


def spots_of(rows, band):
    out = []
    with open(rows) as fh:
        for line in fh:
            if '"ev":"spot"' not in line:
                continue
            d = json.loads(line)
            t = int(d["w"])
            out.append((band, Spot(t, round(d["hz"] / 1000.0, 1), d["call"], int(round(d["snr"])),
                                   int(round(d["wpm"])), "CQ", "REPLAY", "")))
    return out


def faithful(live, rep):
    """(share of live spots the replay has, share of replay spots live has)"""
    def hit(a, pool):
        return any(b == a[0] and s.call == a[1].call and abs(s.t - a[1].t) <= MATCH_S
                   and abs(s.f - a[1].f) <= MATCH_KHZ for b, s in pool.get(a[1].call, ()))
    idx = defaultdict(list)
    for x in rep:
        idx[x[1].call].append(x)
    lidx = defaultdict(list)
    for x in live:
        lidx[x[1].call].append(x)
    a = sum(hit(x, idx) for x in live)
    b = sum(hit(x, lidx) for x in rep)
    return a, len(live), b, len(rep)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--config", default=os.path.join(os.path.dirname(HERE), "compare.ini"))
    ap.add_argument("--taps", help="headless start (UTC, as in tap-<band>-<START>.log); "
                                   "default: every start in the learn dir")
    ap.add_argument("--policy", action="append",
                    help="name=skimmer-tap-replay args (comma-separated); repeatable")
    ap.add_argument("--out", help="where the rows go (default: a temp dir)")
    ap.add_argument("--engine", default="cw-v2")
    a = ap.parse_args()
    pols = a.policy or ["default=", "fresh60=--fresh-s=60", "fresh120=--fresh-s=120"]
    pols = [(p.split("=", 1)[0], [x for x in p.split("=", 1)[1].split(",") if x]) for p in pols]
    out = a.out or tempfile.mkdtemp(prefix="replay-eval-")
    os.makedirs(out, exist_ok=True)

    taps = sorted(glob.glob(os.path.join(LEARN, "tap-*-%s.log" % (a.taps or "*"))))
    if not taps:
        raise SystemExit("no taps in %s" % LEARN)
    tre = re.compile(r"tap-(\w+)-(\d{8}-\d{6})\.log$")
    span = [None, None]
    for tap in taps:                                   # the wall-clock span
        with open(tap) as fh:
            for line in fh:
                if line[:2] in ("T ", "C "):
                    w = int(line.split()[2]) / 1e6
                    span[0] = w if span[0] is None else min(span[0], w)
                    span[1] = w if span[1] is None else max(span[1], w)

    print("taps: %d file(s), %s – %sZ" % (len(taps), time.strftime("%m-%d %H:%M", time.gmtime(span[0])),
                                          time.strftime("%H:%M", time.gmtime(span[1]))))
    rep = {}
    for name, args in pols:
        rep[name] = []
        for tap in taps:
            band, start = tre.search(tap).groups()
            rows = os.path.join(out, "%s-%s-%s.jsonl" % (name, band, start))
            replay(tap, rows, args)
            if not os.path.exists(rows):
                raise SystemExit("tap-replay wrote no rows to %s" % rows)
            rep[name] += spots_of(rows, band)
        print("  replayed %-10s %s → %d spots" % (name, " ".join(args) or "(default policy)",
                                                   len(rep[name])))

    cfg = Config(a.config)
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    scp = load_scp(cfg.scp)
    P = dict(cfg.params)
    lo, hi = span[0], min(span[1], store.rbn.last_t or span[1]) - CONFIRM_S
    bands = {tre.search(t).group(1) for t in taps}

    def band_of(f):
        for b, l_, h_ in cfg.local_windows:
            if l_ <= f <= h_:
                return b
        return None

    live = [(band_of(s.f), s) for s in store.L if span[0] <= s.t <= span[1]]
    live = [(b, s) for b, s in live if b in bands]
    first = pols[0][0]
    ra, rn, rb, rm = faithful(live, rep[first])
    print("\nfaithfulness (%s replay vs L's real feed, same call+band, ±%d s, ±%.0f Hz):"
          % (first, MATCH_S, MATCH_KHZ * 1000))
    print("  %d of %d live spots replayed (%.1f %%), %d of %d replayed spots live (%.1f %%)"
          % (ra, rn, 100 * ra / max(rn, 1), rb, rm, 100 * rb / max(rm, 1)))
    if hi <= lo:
        print("\nno scored stretch yet: the taps must cover more than the %d min the "
              "referee looks ahead" % (CONFIRM_S // 60))
        return

    # ---- each policy's spots in place of L's real feed --------------------------------
    def run(spots):
        st = copy.copy(store)
        st.L = sorted([s for s in store.L if not (span[0] <= s.t <= span[1]
                                                  and band_of(s.f) in bands)]
                      + [s for _, s in spots], key=lambda s: s.t)
        res = {}
        for ref, d in (("compare", scp), ("strict", set())):
            A = analyze(st, cfg.local_windows, cfg.remote_windows, d, P, time.time(),
                        a.engine, cfg.default_engine)
            evs = [e for e in A.events if lo <= e["t0"] <= hi and e["band"] in bands]
            res[ref] = evs
        return res

    R = {name: run(rep[name]) for name, _ in pols}
    # the fixed set: a station any policy caught as real, or R caught, per referee
    print("\nscored %s – %sZ (%d min), bands %s" % (
        time.strftime("%H:%M", time.gmtime(lo)), time.strftime("%H:%M", time.gmtime(hi)),
        (hi - lo) // 60, " ".join(sorted(bands, key=lambda b: int(b[:-1]), reverse=True))))
    print("  %-10s %6s %5s |  compare: recall L   R  bust L    R |  strict: recall L   R  bust L    R"
          % ("policy", "spots", "eps"))
    for ref in ("compare", "strict"):
        U = set()
        for name, _ in pols:
            U |= {(e["call"], e["band"]) for e in R[name][ref] if e["inU"]}
        for name, _ in pols:
            got = {(e["call"], e["band"]) for e in R[name][ref] if e["inU"] and e["cL"]}
            rgot = {(e["call"], e["band"]) for e in R[name][ref] if e["inU"] and e["cR"]}
            nL = sum(e["L"] is not None for e in R[name][ref])
            bL = sum(e["bL"] for e in R[name][ref])
            nR = sum(e["R"] is not None for e in R[name][ref])
            bR = sum(e["bR"] for e in R[name][ref])
            R[name][ref + "_row"] = (100 * len(got) / max(len(U), 1), 100 * len(rgot) / max(len(U), 1),
                                     100 * bL / max(nL, 1), 100 * bR / max(nR, 1), nL, len(U))
    for name, _ in pols:
        c, s = R[name]["compare_row"], R[name]["strict_row"]
        nsp = sum(lo <= x.t <= hi for _, x in rep[name])
        print("  %-10s %6d %5d |  %15.1f %4.1f %6.1f %4.1f |  %14.1f %4.1f %6.1f %4.1f"
              % (name, nsp, c[4], c[0], c[1], c[2], c[3], s[0], s[1], s[2], s[3]))
    print("  (recall over the fixed set: %d stations compare, %d strict — every station any "
          "policy or R caught as real)" % (R[first]["compare_row"][5], R[first]["strict_row"][5]))
    print("rows: %s" % out)


if __name__ == "__main__":
    main()
