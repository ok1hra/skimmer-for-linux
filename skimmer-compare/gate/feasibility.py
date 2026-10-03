#!/usr/bin/env python3
"""feasibility.py — step 0 of the learned feed gate: how far can a filter
that only SUPPRESSES some of L's spots go against VE3NEA?

The compare logs hold only the spots L sent, so this is a lower bound on
what a learned gate can do: it cannot bring back what the hand gate threw
away, nor pick another candidate than the extractor did. Every L episode of
the chosen decoder, inside RBN coverage, gets:

  features  what L itself knew when it first spotted the call: first spot
            SNR/WPM, MASTER.SCP, call shape, band, L's own history
            (--hindsight adds episode length and spot count, an upper bound)
  label     POS     another RBN spotter (±10 min, ±1 kHz) or R heard the call
            ERR     provably wrong at the first spot: RBN or R heard, on the
                    spot's frequency both BEFORE and AFTER it (so the other
                    station held it at that moment, not one that came along a
                    minute later), a SIMILAR call within 100 Hz (a misread) or a
                    DIFFERENT station calling within 60 Hz (the frequency was
                    taken — L's call is a ghost or misplaced)
            SUSPECT a similar call within 300 Hz or another caller within 60 Hz
                    merely NEAR the spot (±2 min): proves nothing — a QSY, a
                    station taking over the frequency — so it is no negative;
                    hand-check only
            UNCONF  nobody confirmed it, nothing contradicts it: a weak or
                    rare station only L heard, or an error nobody can prove —
                    reported, sampled for a hand check (--sample), and NOT a
                    negative unless --neg unconf says so
            UNSURE  RBN heard the call within 3 kHz / 30 min: present, the
                    frequency is off — left out

Three disjoint stretches of time, in order: the weights are fitted on
TRAIN, the threshold is chosen on VAL, and TEST is looked at once, for the
report. An episode belongs to a part only if ALL the evidence its label
looks at (t0 − 30 min … t1 + 30 min) lies inside it — a long episode
straddling a boundary is left out. The boundaries are fixed in
split-<engine>.json the first time and kept: data recorded after the test
stretch is a reserve for a future test, never folded in (--resplit starts
over, which spends the old test).

--verdicts FILE takes Dan's hand verdicts (a filled --sample file) over the
automatic label: real → POS; bust, and qso (a real station, but answering —
not the frequency's owner, so wrong as a CQ spot) → ERR.

Scores are taken against a FIXED set of stations: an L episode the filter
drops still counts in the recall denominator as a miss — never leaves it,
which is how compare itself would rescore a night and hides the loss. Each
threshold also reports how many confirmed stations it threw away. Two
referees side by side: compare's (RBN → MASTER.SCP) and strict (RBN or the
other skimmer only), plus L's provable-error rate.

  ./feasibility.py                         50/25/25 % split, 60 min gaps
  ./feasibility.py --sample 60 unconf.tsv  + a hand-check sample of UNCONF
"""
import argparse
import math
import os
import random
import sys
import time
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from skimcmp.arbiter import RbnIndex, load_scp                 # noqa: E402
from skimcmp.config import Config                              # noqa: E402
from skimcmp.intervals import Membership                       # noqa: E402
from skimcmp.logs import Store                                 # noqa: E402
from skimcmp.match import CQ_WORDS, analyze, similar           # noqa: E402
from skimcmp.windows import loose_bands                        # noqa: E402

CONFIRM_S, CONFIRM_KHZ = 10 * 60, 1.0      # POS: compare's referee window
WIDE_S, WIDE_KHZ = 30 * 60, 3.0            # UNSURE: present, frequency off
ERR_S = 300                                # ERR evidence is looked for this far
                                           # around the FIRST spot, and must HOLD
                                           # the frequency across it: heard there
BRACKET_S = 10                             # both before and after t0 (± this)
MISREAD_KHZ = 0.1                          # a similar call this close, or …
TAKEN_KHZ = 0.06                           # … a different caller this close (two
                                           # CQs 100–150 Hz apart are normal on a
                                           # full band; within 60 Hz they are not)
SUSPECT_S, SUSPECT_KHZ = 120, 0.3          # weaker: merely near — hand-check only
EVIDENCE_S = WIDE_S                        # a label looks this far past t0 / t1
DAY_S = 6 * 3600                           # UNCONF sub-kind: RBN had it today
NEAR_HZ = 0.3              # kHz: a similar call this close …
NEAR_S = 600               # … within this long before → "similar neighbour"
CROWD_KHZ = 0.2            # distinct calls L spotted this close …
CROWD_S = 3600             # … in the last hour: a garbage / ghost zone

CAUSAL = ["snr", "wpm", "scp", "len", "digits", "suffix", "slash",
          "seen_before", "seen_band", "near_similar", "crowd",
          "b160", "b80", "b40", "b30", "b20", "b15", "night"]
HINDSIGHT = ["log_spots", "log_dur"]


# ---- features -----------------------------------------------------------------------

class LHistory:
    """What L's own feed said before an episode started."""

    def __init__(self, Lin):
        self.spots = sorted((sp.t, b, sp.f, sp.call) for b, sp in Lin)
        self.T = [s[0] for s in self.spots]
        self.first, self.first_band = {}, {}
        for t, b, f, c in self.spots:
            self.first.setdefault(c, t)
            self.first_band.setdefault((c, b), t)

    def before(self, ep):
        t0 = ep.t0
        seen = self.first.get(ep.call, t0) < t0 - 1
        seen_band = self.first_band.get((ep.call, ep.band), t0) < t0 - 1
        near, crowd = False, set()
        i, j = bisect_left(self.T, t0 - CROWD_S), bisect_left(self.T, t0)
        for t, b, f, c in self.spots[i:j]:
            if b != ep.band or c == ep.call:
                continue
            if abs(f - ep.f) <= CROWD_KHZ:
                crowd.add(c)
            if t >= t0 - NEAR_S and abs(f - ep.f) <= NEAR_HZ and similar(c, ep.call, 2) is not None:
                near = True
        return {"seen": seen, "seen_band": seen_band, "near": near, "crowd": len(crowd)}


def features(ep, Lhist, scp):
    first = ep.spots[0]
    c = ep.call
    digits = [i for i, ch in enumerate(c) if ch.isdigit()]
    suffix = len(c) - 1 - digits[-1] if digits else len(c)
    hours = time.gmtime(ep.t0).tm_hour
    h = Lhist.before(ep)
    f = {
        "snr": first.snr, "wpm": first.wpm, "scp": float(c in scp),
        "len": len(c), "digits": len(digits), "suffix": suffix,
        "slash": float("/" in c),
        "seen_before": float(h["seen"]), "seen_band": float(h["seen_band"]),
        "near_similar": float(h["near"]), "crowd": math.log1p(h["crowd"]),
        "night": float(hours >= 18 or hours < 6),
        "log_spots": math.log(len(ep.spots)), "log_dur": math.log1p(ep.t1 - ep.t0),
    }
    for b in ("160", "80", "40", "30", "20", "15"):
        f["b" + b] = float(ep.band == b + "m")
    return f


# ---- labels -------------------------------------------------------------------------

class RSpots:
    """R's spots (CQ or not) per band, by time."""

    def __init__(self, Rin):
        self.by = defaultdict(list)
        for b, sp in Rin:
            self.by[b].append((sp.t, sp.f, sp.call, sp.cm in CQ_WORDS))
        for v in self.by.values():
            v.sort()
        self.T = {b: [x[0] for x in v] for b, v in self.by.items()}

    def near(self, band, ta, tb, f, df):
        v, T = self.by.get(band, []), self.T.get(band, [])
        return [x for x in v[bisect_left(T, ta):bisect_right(T, tb)] if abs(x[1] - f) <= df]


def label(ep, rbn, Rs, r_heard):
    """(label, kind, evidence) for an L episode — see the module doc."""
    call, band, f0, t0 = ep.call, ep.band, ep.spots[0].f, ep.t0
    if r_heard or rbn.spotters_of(call, band, ep.t0 - CONFIRM_S, ep.t1 + CONFIRM_S,
                                  ep.f, CONFIRM_KHZ):
        return "POS", "confirmed", ""
    if rbn.spotters_of(call, band, ep.t0 - WIDE_S, ep.t1 + WIDE_S, ep.f, WIDE_KHZ):
        return "UNSURE", "freq-off", ""
    # (t, call, kHz, calling?) heard by RBN (it publishes callers) or by R
    near = [(x[0], x[3], x[2], True)
            for x in rbn.near(band, t0 - ERR_S, t0 + ERR_S, f0, SUSPECT_KHZ, limit=1000)]
    near += [(t, c, fr, cq) for (t, fr, c, cq) in
             Rs.near(band, t0 - ERR_S, t0 + ERR_S, f0, SUSPECT_KHZ)]
    near = [x for x in near if x[1] != call]

    def holds(c, df, calling):
        """c was heard within df of the spot both before and after t0."""
        ts = [t for t, x, fr, cq in near
              if x == c and abs(fr - f0) <= df and (cq or not calling)]
        return bool(ts) and min(ts) <= t0 + BRACKET_S and max(ts) >= t0 - BRACKET_S

    alike = sorted({c for _, c, _, _ in near if similar(c, call, 2) is not None})
    callers = sorted({c for _, c, fr, cq in near if cq and abs(fr - f0) <= TAKEN_KHZ})
    sim = [c for c in alike if holds(c, MISREAD_KHZ, False)]
    if sim:
        return "ERR", "misread", "/".join(sim[:3])
    taken = [c for c in callers if holds(c, TAKEN_KHZ, True)]
    if taken:
        return "ERR", "taken", "/".join(taken[:3])
    close = {c for t, c, _, _ in near if abs(t - t0) <= SUSPECT_S}
    if any(c in close for c in alike):
        return "SUSPECT", "similar-near", "/".join([c for c in alike if c in close][:3])
    if any(c in close for c in callers):
        return "SUSPECT", "caller-near", "/".join([c for c in callers if c in close][:3])
    if rbn.spotters_of(call, band, t0 - DAY_S, ep.t1 + DAY_S, f0, 1000):
        return "UNCONF", "on-band-today", ""
    return "UNCONF", "never-confirmed", ""


# ---- what L decoded around a spot (headless [decode] log=true) -----------------------

DECODE_DIR = os.path.expanduser("~/.local/share/skimmer-for-linux/headless")
# decoded text around the first spot: the call was read BEFORE it went out
CTX_BEFORE_S, CTX_AFTER_S, CTX_KHZ = 90, 20, 0.1
CTX_GAP_S = 5                              # a pause this long shows as " ¦ "


def decode_context(eps):
    """{episode key: decoded text around its first spot} from headless's raw
    decode logs (local wall-clock times, one file per band and start day,
    running past midnight), plus [(band, t_from, t_to)] the logs cover."""
    import glob
    import re
    want = defaultdict(list)                   # band → [(t, f, key)]
    for ep in eps:
        want[ep.band].append((ep.t0, ep.spots[0].f, (ep.call, ep.band, ep.t0)))
    for v in want.values():
        v.sort()
    head = re.compile(r"^--- session (\d{4})-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d) .* (\d+m), dds")
    line = re.compile(r"^(\d\d):(\d\d):(\d\d)\s+(\d+\.\d+)\s+\d+wpm\s+-?\d+dB \|(.*)\|$")
    ctx, cover = defaultdict(list), []
    for path in sorted(glob.glob(os.path.join(DECODE_DIR, "decodes-*.log"))):
        band, day0, prev, t_first = None, None, None, None
        with open(path, errors="replace") as fh:
            for raw in fh:
                m = head.match(raw)
                if m:
                    if band and t_first is not None:
                        cover.append((band, t_first, prev))
                    y, mo, d, hh, mi, ss, band = m.groups()
                    day0 = time.mktime((int(y), int(mo), int(d), 0, 0, 0, 0, 0, -1))
                    prev, t_first = None, None
                    continue
                m = line.match(raw)
                if not m or day0 is None:
                    continue
                t = day0 + int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
                while prev is not None and t < prev - 43200:   # past midnight
                    day0 += 86400
                    t += 86400
                prev = t
                if t_first is None:
                    t_first = t
                W = want.get(band)
                if not W:
                    continue
                i = bisect_left(W, (t - CTX_AFTER_S,))
                f = float(m.group(4))
                while i < len(W) and W[i][0] <= t + CTX_BEFORE_S:
                    if abs(W[i][1] - f) <= CTX_KHZ:
                        ctx[W[i][2]].append((t, m.group(5)))
                    i += 1
        if band and t_first is not None:
            cover.append((band, t_first, prev))
    out = {}
    for k, v in ctx.items():
        v.sort(key=lambda x: x[0])            # stable: a second's fragments keep order
        txt, last = "", None
        for t, x in v:
            if last is not None and t - last >= CTX_GAP_S:
                txt += " ¦ "
            txt += x                           # fragments carry their own spaces
            last = t
        out[k] = " ".join(txt.split())
    return out, cover


# ---- the fixed split, the hand verdicts ----------------------------------------------

def load_split(a, data, rbn_last):
    """{part: (from, to)} epoch spans — read from the split file, or drawn
    from the data now and written there. Returns (spans, created?)."""
    import json
    a.split_file = a.split_file or os.path.join(HERE, "split-%s.json" % a.engine)
    if not a.resplit and os.path.exists(a.split_file):
        with open(a.split_file) as fh:
            js = json.load(fh)
        return {k: tuple(js[k]) for k in ("train", "val", "test")}, False
    cuts = [float(x) for x in a.split.split(",")]
    ts = sorted(d["t0"] for d in data)
    tA = ts[int(len(ts) * cuts[0] / sum(cuts))]
    tB = ts[int(len(ts) * (cuts[0] + cuts[1]) / sum(cuts))]
    g = a.gap_min * 60 / 2
    # the test ends where the recorded evidence ends: an episode still on the
    # air has RBN spots yet to come, and its label is not final
    span = {"train": (ts[0] - EVIDENCE_S, tA - g), "val": (tA + g, tB - g),
            "test": (tB + g, rbn_last)}
    with open(a.split_file, "w") as fh:
        json.dump({**{k: list(v) for k, v in span.items()},
                   "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                   "note": "fixed TRAIN/VAL/TEST spans (epoch s); later data is a reserve"},
                  fh, indent=1)
    return span, True


def load_verdicts(path):
    """{(call, band, t0): verdict} from a --sample file, filled rows only."""
    import calendar
    out = {}
    try:
        with open(path) as fh:
            rows = fh.read().splitlines()[1:]
    except OSError:
        return out
    for r in rows:
        f = r.split("\t")
        if len(f) < 3 or not f[-1].strip():
            continue
        t = calendar.timegm(time.strptime(f[0], "%Y-%m-%d %H:%M:%S"))
        out[(f[3], f[1], t)] = f[-1].strip().lower()
    return out


# ---- scoring against a fixed set of stations ---------------------------------------

def lkey(ev):
    return (ev["L"]["call"], ev["band"], ev["L"]["t0"]) if ev["L"] else None


def scores(evs, drop):
    """recall L/R over the FIXED universe, bust L/R, with the dropped L
    episodes taken out of L's catches and out of L's bust count."""
    U = kL = kR = nL = bL = nR = bR = lost = 0
    for e in evs:
        d = e["L"] is not None and lkey(e) in drop
        if e["inU"]:
            U += 1
            kL += e["cL"] and not d
            kR += e["cR"]
            lost += e["cL"] and d
        if e["L"] is not None and not d:
            nL += 1
            bL += e["bL"]
        if e["R"] is not None:
            nR += 1
            bR += e["bR"]
    f = lambda k, n: k / n if n else float("nan")
    return {"rL": f(kL, U), "rR": f(kR, U), "bL": f(bL, nL), "bR": f(bR, nR),
            "U": U, "lost": lost}


# ---- logistic regression ------------------------------------------------------------

def fit_lr(X, y, l2=1.0, iters=50):
    """Newton/IRLS with an L2 penalty (not on the intercept); X standardized."""
    n, d = X.shape
    Xb = np.hstack([np.ones((n, 1)), X])
    w = np.zeros(d + 1)
    R = l2 * np.eye(d + 1)
    R[0, 0] = 0
    for _ in range(iters):
        p = 1 / (1 + np.exp(-Xb @ w))
        g = Xb.T @ (p - y) + R @ w
        H = (Xb * (p * (1 - p))[:, None]).T @ Xb + R
        step = np.linalg.solve(H, g)
        w -= step
        if np.abs(step).max() < 1e-8:
            break
    return w


def predict(w, X):
    return 1 / (1 + np.exp(-(np.hstack([np.ones((len(X), 1)), X]) @ w)))


def auc(p, y):
    order = np.argsort(p)
    r = np.empty(len(p))
    r[order] = np.arange(1, len(p) + 1)
    npos, nneg = y.sum(), len(y) - y.sum()
    if not npos or not nneg:
        return float("nan")
    return (r[y == 1].sum() - npos * (npos + 1) / 2) / (npos * nneg)


# ---- main ---------------------------------------------------------------------------

def utc(t):
    return time.strftime("%m-%d %H:%MZ", time.gmtime(t))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--config", default=os.path.join(os.path.dirname(HERE), "compare.ini"))
    ap.add_argument("--engine", default="cw-v2")
    ap.add_argument("--split", default="50,25,25",
                    help="train,val,test %% of L episodes in time order (a new split only)")
    ap.add_argument("--gap-min", type=float, default=60, help="dead time between the parts")
    ap.add_argument("--split-file", help="fixed spans (default gate/split-<engine>.json)")
    ap.add_argument("--resplit", action="store_true",
                    help="draw the spans anew from all data now — spends the old test")
    ap.add_argument("--verdicts", help="a filled --sample file: hand verdicts over the labels")
    ap.add_argument("--neg", choices=("err", "unconf"), default="err",
                    help="negatives: provable errors only, or also every unconfirmed call")
    ap.add_argument("--select", choices=("err", "strict", "compare"), default="err",
                    help="bust measure the threshold minimises on VAL (with recall L >= R)")
    ap.add_argument("--keep", choices=("compare", "strict", "both"), default="compare",
                    help="referee whose recall L >= R the chosen threshold must keep on VAL")
    ap.add_argument("--hindsight", action="store_true", help="add episode-length features")
    ap.add_argument("--sample", nargs=2, metavar=("N", "FILE"),
                    help="write N random UNCONF episodes with their context for a hand check")
    a = ap.parse_args()

    cfg = Config(a.config)
    bands = loose_bands(cfg.local_windows, 2.0)
    store = Store(cfg.logs, lambda: RbnIndex(bands, cfg.exclude))
    store.poll()
    scp = load_scp(cfg.scp)
    P = dict(cfg.params)
    now = time.time()
    A = {"compare": analyze(store, cfg.local_windows, cfg.remote_windows, scp, P, now,
                            a.engine, cfg.default_engine),
         "strict": analyze(store, cfg.local_windows, cfg.remote_windows, set(), P, now,
                           a.engine, cfg.default_engine)}
    A0 = A["compare"]
    covered = Membership(A0.rbn_up)
    rbn = store.rbn
    Rs = RSpots(A0.Rin)
    Lhist = LHistory(A0.Lin)

    # ---- one row per L episode -------------------------------------------------
    rows = [ev for ev in A0.events if ev["L"] is not None and ev["t0"] in covered]
    epmap = {(e.call, e.band, e.t0): e for e in A0.eps["L"]}
    data = []
    for ev in rows:
        ep = epmap[lkey(ev)]
        lab, kind, why = label(ep, rbn, Rs, ev["cat"] in ("match", "nonCQ") or ev["bR"])
        data.append({"key": lkey(ev), "ep": ep, "t0": ep.t0, "f": features(ep, Lhist, scp),
                     "lab": lab, "kind": kind, "why": why})
    data.sort(key=lambda d: d["t0"])

    # ---- hand verdicts over the automatic labels ------------------------------------
    if a.verdicts:
        hand = load_verdicts(a.verdicts)
        for d in data:
            v = hand.get((d["ep"].call, d["ep"].band, d["t0"]))
            if v in ("real", "bust", "qso"):
                d["lab"], d["kind"] = ("POS", "hand-real") if v == "real" else ("ERR", "hand-" + v)

    # ---- train / val / test: fixed spans, whole evidence inside -----------------------
    span, fresh = load_split(a, data, rbn.last_t)
    n = len(data)

    def part_of(t0, t1):
        lo, hi = t0 - EVIDENCE_S, t1 + EVIDENCE_S
        for name, (a_, b_) in span.items():
            if a_ <= lo and hi <= b_:
                return name
        return "reserve" if lo > span["test"][1] else "straddle"

    part = {d["key"]: part_of(d["ep"].t0, d["ep"].t1) for d in data}

    def evs_of(name, which):
        return [e for e in A[which].events if e["t0"] in covered
                and part_of(e["t0"], e["t1"]) == name]

    print("engine %s — %d L episodes in RBN coverage; split %s %s" % (
        a.engine, n, a.split_file, "CREATED now" if fresh else "(fixed, reused)"))
    for name in ("train", "val", "test"):
        c = Counter(d["lab"] for d in data if part[d["key"]] == name)
        k = Counter(d["kind"] for d in data if part[d["key"]] == name
                    and d["lab"] in ("ERR", "SUSPECT", "UNCONF"))
        print("  %-5s %s – %s  POS %4d  ERR %4d  SUSPECT %4d  UNCONF %4d  UNSURE %4d   (%s)" % (
            name, utc(span[name][0]), utc(span[name][1]), c["POS"], c["ERR"], c["SUSPECT"],
            c["UNCONF"], c["UNSURE"], ", ".join("%s %d" % kv for kv in sorted(k.items()))))
    pc = Counter(part.values())
    print("  left out: %d straddling a boundary, %d after the test span (reserve)" % (
        pc["straddle"], pc["reserve"]))

    # ---- fit on TRAIN --------------------------------------------------------------
    names = CAUSAL + (HINDSIGHT if a.hindsight else [])
    neg = {"ERR"} | ({"UNCONF"} if a.neg == "unconf" else set())
    tr = [d for d in data if part.get(d["key"]) == "train" and d["lab"] in {"POS"} | neg]
    X = np.array([[d["f"][k] for k in names] for d in tr], float)
    y = np.array([d["lab"] == "POS" for d in tr], float)
    mu, sd = X.mean(0), X.std(0)
    sd[sd == 0] = 1
    w = fit_lr((X - mu) / sd, y)
    print("\nnegatives: %s; fitted on %d POS / %d NEG" % (
        "provable errors" if a.neg == "err" else "provable errors + unconfirmed",
        int(y.sum()), int(len(y) - y.sum())))
    print("coefficients (per 1 sd; + = more likely real):  " + "  ".join(
        "%s %+.2f" % (k, c) for k, c in sorted(zip(names, w[1:]), key=lambda x: -abs(x[1]))
        if abs(c) >= 0.05))

    def probs(name):
        ds = [d for d in data if part.get(d["key"]) == name]
        p = predict(w, (np.array([[d["f"][k] for k in names] for d in ds], float) - mu) / sd)
        return ds, p

    for name in ("val", "test"):
        ds, p = probs(name)
        m = np.array([d["lab"] in {"POS", "ERR"} for d in ds])
        yy = np.array([d["lab"] == "POS" for d in ds], float)
        print("%-4s AUC POS vs ERR %.3f   POS vs ERR+UNCONF %.3f" % (
            name, auc(p[m], yy[m]),
            auc(p[np.array([d["lab"] != "UNSURE" for d in ds])],
                yy[np.array([d["lab"] != "UNSURE" for d in ds])])))

    # ---- sweep a part ----------------------------------------------------------------
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
            out.append((thr, dropped, s, err))
        return out

    def show(name, res):
        print("\n%s %s – %s" % (name.upper(), utc(span[name][0]), utc(span[name][1])))
        print("   thr  dropped POS/ERR/UNC  lost |  compare: recall L  R   bust L   R |"
              "  strict: recall L  R   bust L   R | ERR L")
        for thr, dr, s, err in res:
            c, st = s["compare"], s["strict"]
            print("  %4.2f  %5d %5d %5d  %5d |  %15.1f %4.1f %6.1f %4.1f | %15.1f %4.1f %6.1f %4.1f | %4.1f"
                  % (thr, dr["POS"], dr["ERR"], dr["UNCONF"], st["lost"],
                     100 * c["rL"], 100 * c["rR"], 100 * c["bL"], 100 * c["bR"],
                     100 * st["rL"], 100 * st["rR"], 100 * st["bL"], 100 * st["bR"], 100 * err))

    grid = [0.0] + [x / 100 for x in range(5, 100, 5)]
    val = sweep("val", grid)
    show("val", val)

    def bust_of(s, err):
        return {"err": err, "strict": s["strict"]["bL"], "compare": s["compare"]["bL"]}[a.select]

    refs = ("compare", "strict") if a.keep == "both" else (a.keep,)
    s0 = val[0][2]
    for k in ("compare", "strict"):
        if s0[k]["rL"] < s0[k]["rR"]:
            print("\nnote: under the %s referee L's recall is below R's with no filter at all "
                  "(%.1f vs %.1f %%) — a filter that only drops spots cannot close that"
                  % (k, 100 * s0[k]["rL"], 100 * s0[k]["rR"]))
    ok = [r for r in val if all(r[2][k]["rL"] >= r[2][k]["rR"] for k in refs)]
    if not ok:
        print("no threshold keeps recall L >= R on VAL under the %s referee(s)" % "+".join(refs))
        return
    thr = min(ok, key=lambda r: (bust_of(r[2], r[3]), -r[0]))[0]
    print("VAL picks thr %.2f (lowest %s bust while recall L >= R under the %s referee)"
          % (thr, a.select, "+".join(refs)))
    show("test", sweep("test", [0.0, thr]))
    print("  (test is reported once, at the threshold VAL picked; 0.00 = today's feed)")

    # ---- a hand-check sample of the unconfirmed ----------------------------------------
    if a.sample:
        k, path = int(a.sample[0]), a.sample[1]
        if os.path.exists(path) and load_verdicts(path):
            print("\n%s already holds hand verdicts — not overwritten; give another file" % path)
            return
        pool = [d for d in data if d["lab"] == "UNCONF"]
        ctx, cover = decode_context([d["ep"] for d in pool])
        # with the raw decodes at hand the verdict is far easier: prefer those
        seen = [d for d in pool if any(b == d["ep"].band and lo <= d["t0"] <= hi
                                        for b, lo, hi in cover)]
        if len(seen) >= k:
            pool = seen

        def read(d):
            """Did L decode the call there in the 90 s before it went out?"""
            return d["ep"].call in "".join(ctx.get(d["key"], "").split()).replace("·", "")

        # A call L never even read before it went out is a ghost by L's own
        # record (2026-10-03: 98 % of the "on band today" UNCONF) — nothing to
        # judge. The ones it DID read and nobody confirmed are the question:
        # take all of those, plus a few ghosts as a control.
        random.seed(20261003)
        was_read = [d for d in pool if read(d)]
        ghosts = [d for d in pool if not read(d)]
        pick = random.sample(was_read, min(k, len(was_read)))
        pick += random.sample(ghosts, min(max(5, k // 6), len(ghosts)))
        pick.sort(key=lambda d: d["t0"])
        with open(path, "w") as fh:
            fh.write("utc\tband\tkhz\tcall\tsnr\twpm\tspots\tscp\tkind\t"
                     "rbn_same_call_±6h(first…last)\trbn_near_±300Hz_±2min\tR_near\t"
                     "L_decoded_-90s…+20s_±100Hz\tcall_read\tverdict(real/bust/?)\n")
            for d in pick:
                ep = d["ep"]
                f0, t0 = ep.spots[0].f, ep.t0
                today = sorted(rbn.of_call(ep.call, ep.band, t0 - DAY_S, ep.t1 + DAY_S,
                                           limit=1000))
                near = rbn.near(ep.band, t0 - ERR_S, t0 + ERR_S, f0, MISREAD_KHZ, limit=50)
                rnear = Rs.near(ep.band, t0 - ERR_S, t0 + ERR_S, f0, MISREAD_KHZ)
                fh.write("\t".join(map(str, [
                    time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t0)), ep.band,
                    "%.1f" % f0, ep.call, ep.spots[0].snr, ep.spots[0].wpm, len(ep.spots),
                    int(ep.call in scp), d["kind"],
                    ("%d× %s … %s" % (len(today), *("%s@%.1f" % (
                        time.strftime("%H:%M", time.gmtime(x[0])), x[2])
                        for x in (today[0], today[-1])))) if today else "-",
                    " ".join(sorted({x[3] for x in near})) or "-",
                    " ".join(sorted({x[2] for x in rnear})) or "-",
                    ctx.get(d["key"], "-")[:400].replace("\t", " "),
                    "yes" if read(d) else "no", ""])) + "\n")
        print("\nhand-check sample → %s: all %d UNCONF calls L read before spotting (of %d), "
              "+ %d ghosts it never read, as a control" % (
                  path, len(was_read), len(pool), len(pick) - min(k, len(was_read))))


if __name__ == "__main__":
    main()
