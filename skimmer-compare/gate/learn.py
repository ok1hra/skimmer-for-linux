#!/usr/bin/env python3
"""learn.py — the learned feed gate, step 2: features from the decode tap.

feasibility.py showed that what the spot log carries is too little to tell
L's real stations from its busts (TEST AUC 0.67). The decode tap has what L
knew when it sent a spot: this replays the taps under the live policy (or
reuses rows already replayed), puts the replayed spots into compare in
place of L's real feed, and gives every L episode in RBN coverage:

  features  at its FIRST spot, from the gate rows (causal — nothing after it):
            own    the station record (hearings, re-reads, age, last read),
                   the newest candidate row of the call (score, decoder
                   confidence, SNR, WPM, DE/CQ seen, dictionary, idle tokens)
            window the call's rows in the last WIN_S: how many, how often it
                   was the top candidate read, mean/min confidence
            near   other calls within NEAR_KHZ in the last WIN_S: how many,
                   the best-heard one, and whether one is a similar call
                   (a misread pair) and how well it was heard
            shape  call length, digits, suffix, '/', MASTER.SCP, band, night
  label     feasibility.label(): POS / ERR / SUSPECT / UNCONF / UNSURE

The parts are fixed in split-<engine>-tap.json the first time (the tap data
starts after the old split's test: it is untouched reserve); the weights are
fitted on TRAIN (POS vs ERR), the threshold is chosen on VAL, TEST is
reported once. The filter only DROPS spots: an L episode below the
threshold loses all its spots, scored against the fixed set of stations
(feasibility.scores) under compare's and the strict referee.

The threshold: the lowest L bust (--select) that loses at most --max-lost %
of the confirmed stations L caught on VAL. (feasibility's "recall L >= R"
cannot hold: on ordinary days L is below R before any filter.)

  ./learn.py --rows DIR                    rows replayed earlier (<policy>-<band>-<start>.jsonl)
  ./learn.py                               replay every tap now (fresh_s 120)
  ./learn.py --ini feed-gate.ini           also write the weights and threshold
  ./learn.py --eval feed-gate.ini          no fit: score those fixed weights on the
                                           reserve (the data after the test)
"""
import argparse
import glob
import json
import math
import os
import re
import sys
import tempfile
import time
from collections import Counter, defaultdict, deque

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import importlib.util                                          # noqa: E402

from skimcmp.arbiter import RbnIndex, load_scp                 # noqa: E402
from skimcmp.config import Config                              # noqa: E402
from skimcmp.intervals import Membership                       # noqa: E402
from skimcmp.logs import Store                                 # noqa: E402
from skimcmp.match import analyze, similar                     # noqa: E402
from skimcmp.windows import loose_bands                        # noqa: E402
from feasibility import (EVIDENCE_S, RSpots, auc, fit_lr, label, lkey,  # noqa: E402
                         load_split, predict, scores, utc)


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


replay_eval = _load("replay_eval", os.path.join(HERE, "replay-eval.py"))

WIN_S = 120                  # the window before the first spot
NEAR_KHZ = 0.15              # another call this close is a neighbour
BANDS = ("160m", "80m", "40m", "30m", "20m", "17m", "15m")

NAMES = (["hear", "rep", "age", "st_read", "st_sc", "sc", "conf", "snr", "wpm", "de",
          "row_cq", "dict", "parts", "idle",
          "w_rows", "w_top", "w_conf", "w_conf_min",
          "n_near", "near_hear", "near_sim", "near_sim_hear",
          "len", "digits", "suffix", "slash", "scp", "night"]
         + ["b" + b for b in BANDS])


# ---- rows ---------------------------------------------------------------------------

def ensure_rows(a):
    """{start: {band: rows path}} — replayed now unless --rows has them."""
    taps = sorted(glob.glob(os.path.join(replay_eval.LEARN, "tap-*-*.log")))
    tre = re.compile(r"tap-(\w+)-(\d{8}-\d{6})\.log$")
    out = defaultdict(dict)
    rows_dir = a.rows or tempfile.mkdtemp(prefix="learn-rows-")
    for tap in taps:
        band, start = tre.search(tap).groups()
        if a.taps and start not in a.taps.split(","):
            continue
        rows = os.path.join(rows_dir, "%s-%s-%s.jsonl" % (a.policy, band, start))
        if not os.path.exists(rows):
            print("  replaying %s %s (%s)" % (band, start, a.policy_args or "default"))
            replay_eval.replay(tap, rows, [x for x in a.policy_args.split(",") if x])
        out[start][band] = rows
    return out


def row_spots(path, band):
    return replay_eval.spots_of(path, band)


def shape(call):
    digits = [i for i, ch in enumerate(call) if ch.isdigit()]
    return {"len": len(call), "digits": len(digits),
            "suffix": len(call) - 1 - digits[-1] if digits else len(call),
            "slash": float("/" in call)}


def band_features(path, band, wants):
    """{episode key: features} for the episodes of one band. wants:
    [(t, call, f, key)] — the first spot of each episode, t its whole second.
    One pass over the rows in file order, keeping the last WIN_S of
    candidate rows; an episode's features are taken AT its spot row — the
    state the engine had when the line went out (the shadow gate in
    feed_gate.c computes the same, there and then)."""
    win = deque()                         # (w, hz kHz, call, hear, conf, top)
    last = {}                             # call → its newest row (dict)
    out = {}
    pending = defaultdict(list)
    for t, call, f, key in wants:
        pending[(call, t)].append((f, key))

    def emit(w0, call, f, key):
        rows = [r for r in win if r[0] >= w0 - WIN_S]
        own = [r for r in rows if r[2] == call]
        near = {}
        for w, hz, c, hear, conf, top in rows:
            if c != call and abs(hz - f) <= NEAR_KHZ + 1e-9:
                near[c] = max(near.get(c, 0), hear)
        sims = [h for c, h in near.items() if similar(c, call, 2) is not None]
        d = last.get(call) or {}
        st = d.get("st") or {}
        lg = lambda v: math.log1p(v) if v > -1 else 0.0
        out[key] = {
            "hear": lg(st.get("hear", 0)), "rep": lg(st.get("rep", 0)),
            "age": lg(st.get("age", 0)), "st_read": lg(st.get("read", 0)),
            "st_sc": st.get("sc", 0.0),
            "sc": d.get("sc", 0.0), "conf": d.get("conf", 0.0),
            "snr": d.get("snr", 0.0), "wpm": d.get("wpm", 0.0),
            "de": float(d.get("de", 0)), "row_cq": float(d.get("cq", 0)),
            "dict": float(d.get("dict", 0)), "parts": d.get("parts", 0),
            "idle": lg(d.get("idle", 0)),
            "w_rows": lg(len(own)),
            "w_top": (sum(r[5] for r in own) / len(own)) if own else 0.0,
            "w_conf": (sum(r[4] for r in own) / len(own)) if own else 0.0,
            "w_conf_min": min((r[4] for r in own), default=0.0),
            "n_near": lg(len(near)),
            "near_hear": lg(max(near.values(), default=0)),
            "near_sim": float(bool(sims)), "near_sim_hear": lg(max(sims, default=0)),
        }

    with open(path) as fh:
        for line in fh:
            if line.startswith('{"ev":"spot"'):
                d = json.loads(line)
                hit = pending.get((d["call"], int(d["w"])))
                if hit:
                    emit(d["w"], d["call"], *hit.pop(0))
                    if not hit:
                        del pending[(d["call"], int(d["w"]))]
                    if not pending:
                        break
                continue
            if not line.startswith('{"ev":"c"'):
                continue
            d = json.loads(line)
            w = d["w"]
            st = d.get("st") or {}
            c = st.get("call") or d.get("call")
            if not c:
                continue
            win.append((w, d["hz"] / 1000.0, c, st.get("hear", 0), d.get("conf", 0.0),
                        float(d.get("call") == c)))
            last[c] = d
            while win and win[0][0] < w - WIN_S:
                win.popleft()
    return out


# ---- main ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--config", default=os.path.join(os.path.dirname(HERE), "compare.ini"))
    ap.add_argument("--engine", default="cw-v2")
    ap.add_argument("--rows", help="directory of replayed rows (reused, filled if missing)")
    ap.add_argument("--taps", help="comma-separated headless starts (default: every tap)")
    ap.add_argument("--policy", default="fresh120", help="name of the replayed policy")
    ap.add_argument("--policy-args", default="--fresh-s=120",
                    help="skimmer-tap-replay args of that policy (comma-separated)")
    ap.add_argument("--split", default="50,25,25")
    ap.add_argument("--gap-min", type=float, default=60)
    ap.add_argument("--split-file", help="default gate/split-<engine>-tap.json")
    ap.add_argument("--resplit", action="store_true", help="spends the old test")
    ap.add_argument("--select", choices=("err", "strict", "compare"), default="compare",
                    help="L bust measure the threshold minimises on VAL")
    ap.add_argument("--max-lost", type=float, default=1.0,
                    help="%% of L's confirmed stations the threshold may lose on VAL")
    ap.add_argument("--ini", help="write the weights and the threshold here")
    ap.add_argument("--eval", metavar="INI",
                    help="no fit: score this gate (weights + threshold) on the reserve")
    a = ap.parse_args()
    a.split_file = a.split_file or os.path.join(HERE, "split-%s-tap.json" % a.engine)

    rows = ensure_rows(a)
    if not rows:
        raise SystemExit("no taps in %s" % replay_eval.LEARN)
    cfg = Config(a.config)
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    scp = load_scp(cfg.scp)
    P = dict(cfg.params)

    # ---- the replayed spots in place of L's real feed over the tap spans ---------------
    rep, spans = [], []
    for start, by in sorted(rows.items()):
        sp = []
        for band, path in by.items():
            sp += row_spots(path, band)
        if sp:
            spans.append((min(s.t for _, s in sp) - 60, max(s.t for _, s in sp) + 60))
            rep += sp

    def band_of(f):
        for b, l_, h_ in cfg.local_windows:
            if l_ <= f <= h_:
                return b
        return None

    inside = Membership(spans)
    store.L = sorted([s for s in store.L if not (s.t in inside and band_of(s.f))]
                     + [s for _, s in rep], key=lambda s: s.t)
    now = time.time()
    A = {"compare": analyze(store, cfg.local_windows, cfg.remote_windows, scp, P, now,
                            a.engine, cfg.default_engine),
         "strict": analyze(store, cfg.local_windows, cfg.remote_windows, set(), P, now,
                           a.engine, cfg.default_engine)}
    A0 = A["compare"]
    covered = Membership(A0.rbn_up)
    Rs = RSpots(A0.Rin)

    # ---- one row per L episode inside the taps -----------------------------------------
    epmap = {(e.call, e.band, e.t0): e for e in A0.eps["L"]}
    data = []
    for ev in A0.events:
        if ev["L"] is None or ev["t0"] not in covered or ev["t0"] not in inside:
            continue
        ep = epmap[lkey(ev)]
        lab, kind, _ = label(ep, store.rbn, Rs, ev["cat"] in ("match", "nonCQ") or ev["bR"])
        data.append({"key": lkey(ev), "ep": ep, "t0": ep.t0, "lab": lab, "kind": kind})
    data.sort(key=lambda d: d["t0"])

    feats = {}
    for start, by in sorted(rows.items()):
        for band, path in sorted(by.items()):
            wants = [(d["ep"].spots[0].t, d["ep"].call, d["ep"].spots[0].f, d["key"])
                     for d in data if d["ep"].band == band]
            feats.update(band_features(path, band, wants))
    for d in data:
        x = dict(feats.get(d["key"]) or {k: 0.0 for k in NAMES})
        x.update(shape(d["ep"].call))
        x["scp"] = float(d["ep"].call in scp)
        h = time.gmtime(d["t0"]).tm_hour
        x["night"] = float(h >= 18 or h < 6)
        for b in BANDS:
            x["b" + b] = float(d["ep"].band == b)
        d["f"] = x

    # ---- the fixed parts ---------------------------------------------------------------
    class Args:
        pass
    sa = Args()
    sa.split_file, sa.resplit, sa.split, sa.gap_min, sa.engine = (
        a.split_file, a.resplit, a.split, a.gap_min, a.engine)
    span, created = load_split(sa, data, store.rbn.last_t)

    def part_of(t0, t1):
        lo, hi = t0 - EVIDENCE_S, t1 + EVIDENCE_S
        for name, (x, y) in span.items():
            if x <= lo and hi <= y:
                return name
        return "reserve" if lo > span["test"][1] else "straddle"

    part = {d["key"]: part_of(d["ep"].t0, d["ep"].t1) for d in data}
    print("engine %s — %d L episodes in the taps and RBN coverage; split %s %s" % (
        a.engine, len(data), a.split_file, "CREATED now" if created else "(fixed, reused)"))
    for name in ("train", "val", "test"):
        c = Counter(d["lab"] for d in data if part[d["key"]] == name)
        print("  %-5s %s – %s  POS %5d  ERR %4d  SUSPECT %4d  UNCONF %4d  UNSURE %4d" % (
            name, utc(span[name][0]), utc(span[name][1]), c["POS"], c["ERR"], c["SUSPECT"],
            c["UNCONF"], c["UNSURE"]))
    pc = Counter(part.values())
    print("  left out: %d straddling a boundary, %d after the test (reserve)"
          % (pc["straddle"], pc["reserve"]))

    # a reserve episode whose label evidence is not all recorded yet stays out
    for d in data:
        if part[d["key"]] == "reserve" and d["ep"].t1 + EVIDENCE_S > store.rbn.last_t:
            part[d["key"]] = "unripe"
    if a.eval:
        # ---- the fixed gate of an ini, no fit ------------------------------------------
        import configparser
        g = configparser.ConfigParser()
        if not g.read(a.eval):
            raise SystemExit("cannot read %s" % a.eval)
        wk = {k: [float(x) for x in v.split()] for k, v in g.items("weights")}
        w = np.array([g.getfloat("gate", "bias")] + [wk[k][0] for k in NAMES])
        mu = np.array([wk[k][1] for k in NAMES])
        sd = np.array([wk[k][2] for k in NAMES])
        thr_ini = g.getfloat("gate", "threshold")
        c = Counter(d["lab"] for d in data if part[d["key"]] == "reserve")
        rs = [d["t0"] for d in data if part[d["key"]] == "reserve"]
        print("\nEVAL %s (threshold %.2f) — reserve %s – %s: POS %d  ERR %d  SUSPECT %d  "
              "UNCONF %d  UNSURE %d  (%d still unripe)"
              % (os.path.basename(a.eval), thr_ini, utc(min(rs)) if rs else "-",
                 utc(max(rs)) if rs else "-", c["POS"], c["ERR"], c["SUSPECT"], c["UNCONF"],
                 c["UNSURE"], sum(v == "unripe" for v in part.values())))
    else:
        # ---- fit on TRAIN: POS vs provable errors ---------------------------------------
        tr = [d for d in data if part[d["key"]] == "train" and d["lab"] in ("POS", "ERR")]
        X = np.array([[d["f"][k] for k in NAMES] for d in tr], float)
        y = np.array([d["lab"] == "POS" for d in tr], float)
        mu, sd = X.mean(0), X.std(0)
        sd[sd == 0] = 1
        w = fit_lr((X - mu) / sd, y)
        print("\nfitted on TRAIN: %d POS / %d ERR" % (int(y.sum()), int(len(y) - y.sum())))
        print("coefficients (per 1 sd; + = more likely real):\n  " + "  ".join(
            "%s %+.2f" % (k, c) for k, c in sorted(zip(NAMES, w[1:]), key=lambda x: -abs(x[1]))
            if abs(c) >= 0.05))

    def probs(name):
        ds = [d for d in data if part[d["key"]] == name]
        if not ds:
            return ds, np.zeros(0)
        return ds, predict(w, (np.array([[d["f"][k] for k in NAMES] for d in ds], float) - mu) / sd)

    for name in ("train", "val", "test") + (("reserve",) if a.eval else ()):
        ds, p = probs(name)
        if not ds:
            continue
        m = np.array([d["lab"] in ("POS", "ERR") for d in ds])
        yy = np.array([d["lab"] == "POS" for d in ds], float)
        um = np.array([d["lab"] != "UNSURE" for d in ds])
        print("%-5s AUC POS vs ERR %.3f   POS vs ERR+SUSPECT+UNCONF %.3f" % (
            name, auc(p[m], yy[m]), auc(p[um], yy[um])))

    # ---- sweep / pick / test -------------------------------------------------------------
    def evs_of(name, which):
        return [e for e in A[which].events if e["t0"] in covered and e["t0"] in inside
                and part_of(e["t0"], e["t1"]) == name
                and not (name == "reserve" and e["t1"] + EVIDENCE_S > store.rbn.last_t)]

    def sweep(name, thrs):
        ds, p = probs(name)
        E = {k: evs_of(name, k) for k in A}
        out = []
        for thr in thrs:
            drop = {d["key"] for d, pp in zip(ds, p) if pp < thr}
            kept = [d for d, pp in zip(ds, p) if pp >= thr]
            dropped = Counter(d["lab"] for d, pp in zip(ds, p) if pp < thr)
            s = {k: scores(E[k], drop) for k in A}
            err = sum(d["lab"] == "ERR" for d in kept) / len(kept) if kept else float("nan")
            caught = sum(e["cL"] for e in E["compare"] if e["inU"])
            out.append((thr, dropped, s, err, caught))
        return out

    def show(name, res):
        if name in span:
            print("\n%s %s – %s" % (name.upper(), utc(span[name][0]), utc(span[name][1])))
        else:
            print("\n%s (after %s)" % (name.upper(), utc(span["test"][1])))
        print("   thr  dropped POS/ERR/UNC  lost |  compare: recall L  R   bust L   R |"
              "  strict: recall L  R   bust L   R | ERR L")
        for thr, dr, s, err, _ in res:
            c, st = s["compare"], s["strict"]
            print("  %4.2f  %5d %5d %5d  %5d |  %15.1f %4.1f %6.1f %4.1f | %15.1f %4.1f %6.1f %4.1f | %4.1f"
                  % (thr, dr["POS"], dr["ERR"], dr["UNCONF"], c["lost"],
                     100 * c["rL"], 100 * c["rR"], 100 * c["bL"], 100 * c["bR"],
                     100 * st["rL"], 100 * st["rR"], 100 * st["bL"], 100 * st["bR"], 100 * err))

    grid = [0.0] + [x / 100 for x in range(5, 100, 5)]
    if a.eval:
        show("reserve", sweep("reserve", sorted({0.0, thr_ini, 0.05, 0.15, 0.2, 0.3})))
        print("  (the gate's own threshold is %.2f; 0.00 = today's feed)" % thr_ini)
        return
    val = sweep("val", grid)
    show("val", val)
    bust = lambda s, err: {"err": err, "strict": s["strict"]["bL"],
                           "compare": s["compare"]["bL"]}[a.select]
    ok = [r for r in val if r[2]["compare"]["lost"] <= a.max_lost / 100 * r[4]]
    thr = min(ok, key=lambda r: (bust(r[2], r[3]), -r[0]))[0]
    print("VAL picks thr %.2f: lowest %s bust of L losing at most %.1f %% of L's confirmed "
          "stations" % (thr, a.select, a.max_lost))
    show("test", sweep("test", [0.0, thr]))
    print("  (TEST is reported once, at the threshold VAL picked; 0.00 = today's feed)")

    if a.ini:
        with open(a.ini, "w") as fh:
            fh.write("# feed-gate.ini — learned by gate/learn.py %s\n"
                     "# p = 1 / (1 + exp(-(bias + sum(w_k * (x_k - mean_k) / sd_k))));"
                     " spot only when p >= threshold\n"
                     "[gate]\nbias = %.6f\nthreshold = %.2f\nwin_s = %d\nnear_khz = %.3f\n"
                     "split = %s\n\n[weights]\n" % (
                         time.strftime("%Y-%m-%dT%H:%MZ", time.gmtime()), w[0], thr, WIN_S,
                         NEAR_KHZ, os.path.basename(a.split_file)))
            for k, c, m, s in zip(NAMES, w[1:], mu, sd):
                fh.write("%s = %.6f %.6f %.6f\n" % (k, c, m, s))
        print("wrote %s" % a.ini)


if __name__ == "__main__":
    main()
