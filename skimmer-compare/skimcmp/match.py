"""match.py — from two spot streams to compared events.

Only spots inside the common frequency windows and the common time (both
feeds connected, local skimmer healthy, running the chosen decoder) take
part — the local windows being the bands it was listening to at the time — each local decoder is a comparison of its own. Both sides re-spot on
their own schedule, so spots are first merged into EPISODES: one call on
one band, spots joined while the gap is at most gap_min. Then:

  match    an L and an R episode of the same call overlap in time (± slack)
           and frequency (± df); an episode left over still counts as caught
           by the other side when that side spotted the call near it (one
           long episode of one side may span several of the other's) — the
           event then lists the leftover episode only, its partner being
           counted where it was matched
  nonCQ    an L episode the remote heard only as DE / without CQ — neither
           a catch nor a plain miss for R
  bust     unmatched L and R episodes of DIFFERENT but similar calls at the
           same frequency and time; the referee decides who was right
  only-L / only-R   the rest

Each event carries the referee's verdict (rbn / scp / none) and whether it
belongs to the recall universe (a real station: confirmed, or both skimmers
agree), who caught it and who busted it.
"""
import time
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from statistics import median

from .arbiter import RANK, Arbiter
from .intervals import Membership, intersect, normalize, subtract, total
from .windows import Windows

CQ_WORDS = {"CQ", "TEST", "QRZ"}


def hm(t):
    return time.strftime("%H:%M", time.gmtime(t))


class Episode:
    __slots__ = ("side", "call", "band", "spots", "t0", "t1", "f", "snr", "wpm", "used")

    def __init__(self, side, call, band, spots):
        self.side, self.call, self.band, self.spots = side, call, band, spots
        self.t0, self.t1 = spots[0].t, spots[-1].t
        self.f = median(s.f for s in spots)
        self.snr = median(s.snr for s in spots)
        self.wpm = median(s.wpm for s in spots)
        self.used = False

    def json(self, v=None):
        d = {"call": self.call, "t0": self.t0, "t1": self.t1, "n": len(self.spots),
             "f": round(self.f, 1), "snr": self.snr, "wpm": self.wpm}
        if v is not None:
            d["v"], d["rbn"] = v
        return d


def episodes(side, items, gap_s):
    by = defaultdict(list)
    for band, sp in items:
        by[(sp.call, band)].append(sp)
    out = []
    for (call, band), lst in by.items():
        lst.sort(key=lambda s: s.t)
        cur = [lst[0]]
        for sp in lst[1:]:
            if sp.t - cur[-1].t > gap_s:
                out.append(Episode(side, call, band, cur))
                cur = [sp]
            else:
                cur.append(sp)
        out.append(Episode(side, call, band, cur))
    out.sort(key=lambda e: e.t0)
    return out


def levenshtein(a, b, limit):
    if abs(len(a) - len(b)) > limit:
        return limit + 1
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        if min(cur) > limit:
            return limit + 1
        prev = cur
    return prev[-1]


def similar(a, b, k):
    """Edit distance for a bust pair, or None if the calls are not alike."""
    if a == b or min(len(a), len(b)) < 3:
        return None
    if a.startswith(b) or b.startswith(a) or a.endswith(b) or b.endswith(a):
        return abs(len(a) - len(b))
    d = levenshtein(a, b, k)
    return d if d <= k else None


class Analysis:
    pass


CURVE_STEP = 5      # dB of L's SNR per curve point
CURVE_MIN_N = 5     # fewer pairs in a bin than this → the bin has no point


def snr_curve(l_snr, diffs, fallback):
    """R − L as a function of L's SNR: [(L centre, median R − L, n)].

    The two scales do not differ by a constant — L's peak/noise ratio is
    floored near its squelch and squashed against R's — so one median would
    misplace the weak and the strong ends. One point per CURVE_STEP bin of
    L's SNR with enough pairs; with no such bin, one point at the overall
    median (a constant shift)."""
    bins = defaultdict(list)
    for x, d in zip(l_snr, diffs):
        bins[int(x // CURVE_STEP)].append((x, d))
    pts = [(round(median(x for x, _ in g), 1), median(d for _, d in g), len(g))
           for _, g in sorted(bins.items()) if len(g) >= CURVE_MIN_N]
    return pts or [(0.0, fallback, len(diffs))]


def curve_at(pts, x):
    """R − L at L's SNR x: linear between points, flat beyond the ends."""
    if x <= pts[0][0]:
        return pts[0][1]
    for (x0, y0, _), (x1, y1, _) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


def compared_time(store, P, now, default_engine, default_windows=()):
    """Sessions, recorded spans, RBN coverage, the common time split by
    local decoder: [(a, b, engine)] and by local band plan: [(a, b, windows)]."""
    sessions = store.ordered()
    cur = store.current(now)
    latest = cur.T if cur else None
    info, spans, rbn_up, segs, wsegs = [], [], [], [], []
    for s in sessions:
        end = s.end(s.T == latest, now)
        if end <= s.start:
            continue
        lup = s.up("local", end)
        c = intersect(lup, s.up("remote", end))
        c = subtract(c, s.unhealthy(P["lost_pct"]))
        engines = {}
        for a, b, eng in s.engines(lup, default_engine):
            for x, y in intersect(c, [(a, b)]):
                segs.append((x, y, eng))
                engines[eng] = engines.get(eng, 0) + y - x
        for a, b, w in s.windows(lup):
            wsegs.extend((x, y, w) for x, y in intersect(c, [(a, b)]))
        rbn_up += s.up("rbn", end)
        spans.append((s.start, end))
        info.append({"T": s.T, "start": s.start, "end": end,
                     "live": s.T == latest and s.stopped is None and end == now,
                     "common_s": sum(b - a for a, b in c), "engines": engines,
                     "has_rbn": "rbn" in s.tails})
    segs.sort()
    wsegs.sort(key=lambda w: w[0])
    # a connection that never said its bands: the plan nearest in time that
    # did, not today's config — that may already list a band added since
    known = [(a, w) for a, _, w in wsegs if w]
    for i, (a, b, w) in enumerate(wsegs):
        if w is None:
            near = min(known, key=lambda k: abs(k[0] - a), default=None)
            wsegs[i] = (a, b, near[1] if near else default_windows)
    return info, spans, normalize(rbn_up), segs, wsegs


class BandPlan:
    """The local band plan in force at time t: the common-window band of a
    spot heard then, or None. Adding a band must not count it as missed by
    the local skimmer in the time before it listened there."""

    def __init__(self, wsegs, remote_w, guard_khz):
        self.segs = wsegs
        self.starts = [a for a, _, _ in wsegs]
        self.win = {}
        for _, _, w in wsegs:
            if w not in self.win:
                self.win[w] = Windows(list(w), remote_w, guard_khz)

    def band(self, t, f):
        i = bisect_right(self.starts, t) - 1
        if i < 0 or t > self.segs[i][1]:
            return None
        return self.win[self.segs[i][2]].band(f)


def engine_summary(segs):
    """{engine: {"s": compared seconds, "last": end of its newest segment}}"""
    out = {}
    for a, b, eng in segs:
        e = out.setdefault(eng, {"s": 0, "last": 0})
        e["s"] += b - a
        e["last"] = max(e["last"], b)
    return out


def analyze(store, local_w, remote_w, scp, P, now, engine=None, default_engine="cw-v2"):
    with store.lock:
        return _analyze(store, local_w, remote_w, scp, P, now, engine, default_engine)


def _analyze(store, local_w, remote_w, scp, P, now, engine, default_engine):
    A = Analysis()
    A.params, A.version, A.computed = dict(P), store.version, now
    A.engine = engine
    gap, slack = P["gap_min"] * 60, P["slack_min"] * 60
    df, bust_df, bust_dt = P["df_khz"], P["bust_df_hz"] / 1000.0, P["bust_dt_s"]

    # ---- time: sessions, common time (of this decoder), RBN coverage ------------
    A.sessions, A.spans, A.rbn_up, segs, wsegs = compared_time(store, P, now, default_engine,
                                                               tuple(local_w))
    A.segments = segs
    A.common = normalize([(a, b) for a, b, eng in segs if engine is None or eng == engine])
    inside = Membership(A.common)
    plan = BandPlan(wsegs, remote_w, P["guard_khz"])
    # the bands shown: every band listened to in the compared time, at its
    # newest centre; the configured ones when nothing was compared
    newest = {}
    for a, b, w in wsegs:
        if total(intersect(A.common, [(a, b)])) > 0:
            newest.update((n, (n, lo, hi)) for n, lo, hi in w)
    win = A.win = Windows(sorted(newest.values(), key=lambda w: w[1]) or local_w,
                          remote_w, P["guard_khz"])

    # ---- spots inside the compared range ---------------------------------------
    A.Lall, A.Rall = list(store.L), list(store.R)
    Lin, Rin = [], []
    for src, dst in ((A.Lall, Lin), (A.Rall, Rin)):
        for sp in src:
            b = plan.band(sp.t, sp.f) if sp.t in inside else None
            if b:
                dst.append((b, sp))
    Lin = [(b, sp) for b, sp in Lin if sp.cm in CQ_WORDS]
    Rcq = [(b, sp) for b, sp in Rin if sp.cm in CQ_WORDS]
    Rnon = defaultdict(list)
    for b, sp in Rin:
        if sp.cm not in CQ_WORDS:
            Rnon[(sp.call, b)].append(sp)
    A.Lin, A.Rin, A.Rcq = Lin, Rin, Rcq

    Leps = episodes("L", Lin, gap)
    Reps = episodes("R", Rcq, gap)
    arb = Arbiter(store.rbn, scp, P["rbn_win_min"] * 60, P["rbn_df_khz"])

    # ---- 1. same call: match ------------------------------------------------------
    Rby = defaultdict(list)
    for e in Reps:
        Rby[(e.call, e.band)].append(e)
    pairs = []
    for le in Leps:
        best = None
        for re_ in Rby.get((le.call, le.band), ()):
            if re_.used or re_.t0 > le.t1 + slack or re_.t1 < le.t0 - slack:
                continue
            if abs(re_.f - le.f) > df:
                continue
            key = (-(min(le.t1, re_.t1) - max(le.t0, re_.t0)), abs(re_.t0 - le.t0))
            if best is None or key < best[0]:
                best = (key, re_)
        if best:
            le.used = best[1].used = True
            pairs.append((le, best[1]))

    # ---- 1b. leftovers the other side spotted anyway ---------------------------------
    # Episodes split on each side's own re-spot schedule, so one L episode can
    # overlap several R episodes but match only one of them. A leftover whose
    # call the other side spotted (CQ) within ± slack and ± df was caught by
    # both: paired with that spot's episode if still free, otherwise covered.
    def spot_index(eps):
        idx = defaultdict(list)
        for ep in eps:
            for sp in ep.spots:
                idx[(ep.band, ep.call)].append((sp.t, sp.f, ep))
        for v in idx.values():
            v.sort(key=lambda x: x[0])
        return idx

    covered = []                                 # (leftover episode, covering episode)
    for mine, theirs in ((Reps, spot_index(Leps)), (Leps, spot_index(Reps))):
        for ep in mine:
            if ep.used:
                continue
            near = [o for t, f, o in theirs.get((ep.band, ep.call), ())
                    if ep.t0 - slack <= t <= ep.t1 + slack and abs(f - ep.f) <= df]
            if not near:
                continue
            free = [o for o in near if not o.used]
            ep.used = True
            if free:
                free[0].used = True
                pairs.append((free[0], ep) if ep.side == "R" else (ep, free[0]))
            else:
                covered.append((ep, near[0]))

    # ---- 2. R heard it, but not as CQ ---------------------------------------------
    noncq = []
    for le in Leps:
        if le.used:
            continue
        hits = [sp for sp in Rnon.get((le.call, le.band), ())
                if le.t0 - slack <= sp.t <= le.t1 + slack and abs(sp.f - le.f) <= df]
        if hits:
            le.used = True
            noncq.append((le, hits))

    # ---- 3. different but similar calls at the same spot: bust pairs --------------
    bucket = defaultdict(list)                   # (band, f/bust_df) → R episodes' spots
    for re_ in Reps:
        if re_.used:
            continue
        for sp in re_.spots:
            bucket[(re_.band, int(sp.f // bust_df))].append((sp, re_))
    cands = {}
    for le in Leps:
        if le.used:
            continue
        for a in le.spots:
            k = int(a.f // bust_df)
            for kk in (k - 1, k, k + 1):
                for b, re_ in bucket.get((le.band, kk), ()):
                    if abs(a.t - b.t) > bust_dt or abs(a.f - b.f) > bust_df:
                        continue
                    d = similar(le.call, re_.call, P["bust_edit"])
                    if d is None:
                        continue
                    key = (d, abs(a.t - b.t), abs(a.f - b.f))
                    old = cands.get((id(le), id(re_)))
                    if old is None or key < old[0]:
                        cands[(id(le), id(re_))] = (key, le, re_)
    busts = []
    for key, le, re_ in sorted(cands.values(), key=lambda x: x[0]):
        if le.used or re_.used:
            continue
        le.used = re_.used = True
        busts.append((le, re_))

    # ---- SNR offset R − L from the matched pairs ----------------------------------
    diffs, pairs_l = [], []
    for le, re_ in pairs:
        rts = [s.t for s in re_.spots]
        for s in le.spots:
            i = bisect_left(rts, s.t)
            near = [re_.spots[j] for j in (i - 1, i) if 0 <= j < len(rts)]
            near = [r for r in near if abs(r.t - s.t) <= 300]
            if near:
                r = min(near, key=lambda r: abs(r.t - s.t))
                diffs.append(r.snr - s.snr)
                pairs_l.append(s.snr)
    A.snr_offset = median(diffs) if diffs else 0.0
    A.snr_offset_n = len(diffs)
    A.snr_curve = snr_curve(pairs_l, diffs, A.snr_offset)

    # ---- events -------------------------------------------------------------------
    Lt = [sp.t for _, sp in Lin]
    Rt = [sp.t for _, sp in Rin]

    def hint(ep):
        """What the other side and RBN heard where this unverified call was."""
        ta, tb = ep.t0 - bust_dt, ep.t1 + bust_dt
        other, ot, tag = (Rin, Rt, "R") if ep.side == "L" else (Lin, Lt, "L")
        c = Counter(sp.call for b, sp in other[bisect_left(ot, ta):bisect_right(ot, tb)]
                    if b == ep.band and abs(sp.f - ep.f) <= 0.5 and sp.call != ep.call)
        r = Counter(x[3] for x in store.rbn.near(ep.band, ta, tb, ep.f, 0.5)
                    if x[3] != ep.call)
        return ([[tag, k, n] for k, n in c.most_common(3)] +
                [["RBN", k, n] for k, n in r.most_common(3)])

    def common_snr(le, re_):
        # R is the referee scale: L's SNR is a peak/noise envelope ratio,
        # floored near its squelch and squashed, so averaging it in would put
        # every weak station L caught into a higher bin than the ones it missed.
        if re_ is not None:
            return round(re_.snr, 1)
        if le is not None:
            return round(le.snr + curve_at(A.snr_curve, le.snr), 1)
        return None

    def common_wpm(le, re_):
        v = [e.wpm for e in (le, re_) if e is not None]
        return round(sum(v) / len(v), 1) if v else None

    events = []

    def add(cat, le, re_, call, v, inU, cL, cR, bL=False, bR=False, lat=None,
            hint_=None, vL=None, vR=None, note=None):
        eps = [e for e in (le, re_) if e is not None]
        events.append({
            "cat": cat, "band": eps[0].band, "call": call,
            "t0": min(e.t0 for e in eps), "t1": max(e.t1 for e in eps),
            "f": round(eps[0].f, 1),
            "L": le.json(vL) if le else None, "R": re_.json(vR) if re_ else None,
            "v": v[0], "rbn": v[1], "inU": inU, "cL": cL, "cR": cR, "bL": bL, "bR": bR,
            "snr": common_snr(le, re_), "wpm": common_wpm(le, re_), "lat": lat,
            "hint": hint_ or [], "note": note})

    def verdict(ep):
        return arb.verdict(ep.call, ep.band, ep.t0, ep.t1, ep.f)

    for le, re_ in pairs:
        v = arb.verdict(le.call, le.band, min(le.t0, re_.t0), max(le.t1, re_.t1), le.f)
        if v[0] == "none":
            v = ("agree", 0)
        add("match", le, re_, le.call, v, True, True, True, lat=re_.t0 - le.t0)

    for ep, other in covered:
        v = verdict(ep)
        if v[0] == "none":
            v = ("agree", 0)
        le, re_ = (ep, None) if ep.side == "L" else (None, ep)
        add("match", le, re_, ep.call, v, True, True, True,
            note="%s spotted it too, in its %s–%s episode (counted there)"
                 % (other.side, hm(other.t0), hm(other.t1)))

    for le, hits in noncq:
        v = verdict(le)
        if v[0] == "none":
            v = ("agree", 0)
        words = sorted({h.cm or "—" for h in hits})
        add("nonCQ", le, None, le.call, v, False, True, False,
            note="R heard %d× as %s" % (len(hits), "/".join(words)))

    for le, re_ in busts:
        vL, vR = verdict(le), verdict(re_)
        rl, rr = RANK[vL[0]], RANK[vR[0]]
        if rl == rr == 2:
            # both calls confirmed elsewhere: two real stations, not a bust
            le.used = re_.used = False
            continue
        if rl > rr:
            add("bust", le, re_, le.call, vL, True, True, False, bR=True, vL=vL, vR=vR,
                note="R busted %s as %s" % (le.call, re_.call))
        elif rr > rl:
            add("bust", le, re_, re_.call, vR, True, False, True, bL=True, vL=vL, vR=vR,
                note="L busted %s as %s" % (re_.call, le.call))
        else:
            add("bust", le, re_, le.call, ("none", 0), False, False, False, vL=vL, vR=vR,
                note="unresolved: %s vs %s, referee cannot tell" % (le.call, re_.call))

    for side, eps in (("L", Leps), ("R", Reps)):
        for e in eps:
            if e.used:
                continue
            v = verdict(e)
            real = v[0] != "none"
            le, re_ = (e, None) if side == "L" else (None, e)
            add("only-" + side, le, re_, e.call, v, real, side == "L" and real,
                side == "R" and real, bL=side == "L" and not real,
                bR=side == "R" and not real, hint_=None if real else hint(e))

    events.sort(key=lambda ev: ev["t0"])
    for i, ev in enumerate(events):
        ev["id"] = i
    A.events = events
    A.eps = {"L": Leps, "R": Reps}

    # ---- suspicious silences, per band ----------------------------------------------
    A.silences = []
    times = defaultdict(list)
    for b, sp in Lin:
        times[("L", b)].append(sp.t)
    for b, sp in Rin:
        times[("R", b)].append(sp.t)
    silence, need = P["silence_min"] * 60, P["silence_other"]
    for a, z in A.common:
        for band, _, _ in win.common:
            for side, other in (("L", "R"), ("R", "L")):
                ts = times[(side, band)]
                os_ = times[(other, band)]
                pts = [a] + ts[bisect_left(ts, a):bisect_right(ts, z)] + [z]
                for x, y in zip(pts, pts[1:]):
                    if y - x < silence:
                        continue
                    n = bisect_left(os_, y) - bisect_right(os_, x)
                    if n >= need:
                        A.silences.append({"side": side, "band": band, "t0": x, "t1": y,
                                           "other": n})
    A.silences.sort(key=lambda s: s["t0"])
    A.rbn_n, A.rbn_own = store.rbn.n, store.rbn.own
    return A
