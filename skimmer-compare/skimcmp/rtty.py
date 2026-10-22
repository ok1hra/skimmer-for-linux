"""rtty.py — the local RTTY spots against the Reverse Beacon Network.

There is no second RTTY skimmer at the antenna, so RBN is the reference:
the RTTY spots other skimmers sent from inside the local RTTY windows
while the local skimmer decoded RTTY there and RBN was connected.

  recall   RBN RTTY episodes (a call on a band, spots merged while the gap
           is at most gap_min) the local feed also spotted — same call and
           band, overlapping in time ± slack_min, |Δf| ≤ df_khz. A station
           that only far-away skimmers hear counts too, so this is a floor,
           to be followed over time; "strong" = heard by 3 or more RBN
           spotters, the stations an antenna in Europe most likely hears.
  bust     local RTTY episodes the referee does not know: no other
           skimmer spotted the call (any mode) within rbn_win_min and
           rbn_df_khz, and MASTER.SCP does not list it.

The local RTTY windows come from the status snapshots (the bands whose
"rtty" is not null); the feed must not have been unhealthy (lost_pct).
"""
from collections import defaultdict

from .arbiter import Arbiter
from .intervals import Membership, intersect, normalize, subtract, total
from .logs import Spot
from .match import CQ_WORDS, episodes

STRONG = 3                     # RBN spotters that make a station "strong"


def rtty_time(store, P, now):
    """[(a, b, windows)]: local up ∩ RBN up − unhealthy, with the RTTY
    windows of the local connection then (only those with any)."""
    cur = store.current(now)
    latest = cur.T if cur else None
    out = []
    for s in store.ordered():
        end = s.end(s.T == latest, now)
        if end <= s.start:
            continue
        lup = s.up("local", end)
        c = intersect(lup, s.up("rbn", end))
        c = subtract(c, s.unhealthy(P["lost_pct"]))
        for a, b, w in s.rtty_windows(lup):
            if w:
                out.extend((x, y, w) for x, y in intersect(c, [(a, b)]))
    return sorted(out, key=lambda x: x[0])


def _band(segs, t, f):
    for a, b, w in segs:
        if a <= t <= b:
            for name, lo, hi in w:
                if lo <= f <= hi:
                    return name
            return None
    return None


def _rbn_spots(rbn, segs):
    """RBN RTTY spots inside the RTTY time and windows, as Spot."""
    out = []
    for (band, ci), pos in rbn.by_call.items():
        call = rbn.calls[ci]
        T, F, S, N = rbn.t[band], rbn.f[band], rbn.s[band], rbn.snr[band]
        for p in pos:
            t, f = T[p], F[p]
            b = _band(segs, t, f)
            if b:
                out.append((b, Spot(t, f, call, N[p], 45, "CQ", rbn.spotters[S[p]], "", "RTTY")))
    return out


def analyze_rtty(store, scp, P, now, t0=None, t1=None):
    """The RTTY comparison over [t0, t1] (None = all) as a dict."""
    gap, slack, df = P["gap_min"] * 60, P["slack_min"] * 60, P["df_khz"]
    segs = rtty_time(store, P, now)
    if t0 is not None or t1 is not None:
        lo, hi = t0 if t0 is not None else float("-inf"), t1 if t1 is not None else float("inf")
        segs = [(max(a, lo), min(b, hi), w) for a, b, w in segs if b > lo and a < hi]
    common = normalize([(a, b) for a, b, _ in segs])
    inside = Membership(common)

    Lin = []
    for sp in store.Lr:
        if sp.t in inside and sp.cm in CQ_WORDS:
            b = _band(segs, sp.t, sp.f)
            if b:
                Lin.append((b, sp))
    Rin = _rbn_spots(store.rbn_rtty, segs)
    Leps, Reps = episodes("L", Lin, gap), episodes("R", Rin, gap)

    by = defaultdict(list)
    for le in Leps:
        by[(le.call, le.band)].append(le)
    bands = defaultdict(lambda: {"rbn": 0, "rbn_strong": 0, "matched": 0, "matched_strong": 0,
                                 "local": 0, "local_rbn": 0, "local_scp": 0, "local_none": 0})
    for re_ in Reps:
        strong = len({sp.spotter for sp in re_.spots}) >= STRONG
        B = bands[re_.band]
        B["rbn"] += 1
        B["rbn_strong"] += strong
        hit = any(le.t0 <= re_.t1 + slack and le.t1 >= re_.t0 - slack and abs(le.f - re_.f) <= df
                  for le in by.get((re_.call, re_.band), ()))
        B["matched"] += hit
        B["matched_strong"] += hit and strong

    arb = Arbiter(store.rbn, scp, P["rbn_win_min"] * 60, P["rbn_df_khz"])
    busts = []
    for le in Leps:
        v, n = arb.verdict(le.call, le.band, le.t0, le.t1, le.f)
        B = bands[le.band]
        B["local"] += 1
        B["local_" + v] += 1
        if v == "none":
            busts.append({"call": le.call, "band": le.band, "f": round(le.f, 2), "t0": le.t0,
                          "t1": le.t1, "spots": len(le.spots)})

    def rates(B):
        B["recall"] = B["matched"] / B["rbn"] if B["rbn"] else None
        B["recall_strong"] = B["matched_strong"] / B["rbn_strong"] if B["rbn_strong"] else None
        B["bust"] = B["local_none"] / B["local"] if B["local"] else None
        return B

    tot = {k: sum(B[k] for B in bands.values())
           for k in ("rbn", "rbn_strong", "matched", "matched_strong", "local", "local_rbn",
                     "local_scp", "local_none")}
    return {
        "time_s": total(common),
        "bands": {b: rates(B) for b, B in sorted(bands.items())},
        "total": rates(tot),
        "busts": sorted(busts, key=lambda x: x["t0"]),
    }
