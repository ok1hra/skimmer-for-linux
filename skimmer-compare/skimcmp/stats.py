"""stats.py — the numbers: recall + McNemar, bust rate, latency, sensitivity,
per band, over time, and the frequency histograms — for a time range.

Every metric is reported on its own; nothing is folded into one score.
A verdict is given only when the test says the difference is unlikely to be
chance (p < 0.05) and there is enough data; otherwise "undecided".
"""
import math
from statistics import median

from .intervals import clip, subtract, total

ALPHA = 0.05
MIN_DISCORDANT = 20
LAT_EDGES = [-300, -120, -60, -30, -10, -3, 3, 10, 30, 60, 120, 300]
SNR_STEP = 5
WPM_EDGES = [15, 20, 25, 30, 35]


def wilson(k, n, z=1.96):
    if n == 0:
        return None, None
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return max(0.0, c - h), min(1.0, c + h)


def binom_two_sided(k, n):
    """Exact two-sided p of k successes in n trials at p = 1/2 (McNemar,
    sign test)."""
    if n == 0:
        return 1.0
    k = min(k, n - k)
    lg = math.lgamma
    tail = sum(math.exp(lg(n + 1) - lg(i + 1) - lg(n - i + 1) - n * math.log(2))
               for i in range(k + 1))
    return min(1.0, 2 * tail)


def two_prop(k1, n1, k2, n2):
    """Two-sided p of a difference of two proportions (pooled z test)."""
    if n1 == 0 or n2 == 0:
        return 1.0
    p = (k1 + k2) / (n1 + n2)
    se = math.sqrt(p * (1 - p) * (1 / n1 + 1 / n2))
    if se == 0:
        return 1.0
    z = abs(k1 / n1 - k2 / n2) / se
    return math.erfc(z / math.sqrt(2))


def prop(k, n):
    lo, hi = wilson(k, n)
    return {"k": k, "n": n, "p": (k / n) if n else None, "lo": lo, "hi": hi}


def quantile(v, q):
    if not v:
        return None
    v = sorted(v)
    i = (len(v) - 1) * q
    a, b = int(math.floor(i)), int(math.ceil(i))
    return v[a] + (v[b] - v[a]) * (i - a)


def recall_block(evs):
    U = [e for e in evs if e["inU"]]
    n = len(U)
    kL = sum(e["cL"] for e in U)
    kR = sum(e["cR"] for e in U)
    b = sum(e["cL"] and not e["cR"] for e in U)
    c = sum(e["cR"] and not e["cL"] for e in U)
    p = binom_two_sided(b, b + c)
    diff = (b - c) / n if n else None
    lo = hi = None
    if n:
        se = math.sqrt(max(0.0, b + c - (b - c) ** 2 / n)) / n
        lo, hi = diff - 1.96 * se, diff + 1.96 * se
    if b + c < MIN_DISCORDANT:
        verdict = "undecided — only %d stations seen by just one skimmer (need %d)" % (
            b + c, MIN_DISCORDANT)
        winner = None
    elif p >= ALPHA:
        verdict = "undecided — no significant difference (p = %.2g)" % p
        winner = None
    else:
        winner = "L" if b > c else "R"
        verdict = "%s finds more stations: %d vs %d found by one only (p = %.2g)" % (
            winner, max(b, c), min(b, c), p)
    return {"n": n, "L": prop(kL, n), "R": prop(kR, n), "b": b, "c": c, "p": p,
            "diff": diff, "diff_lo": lo, "diff_hi": hi, "winner": winner,
            "verdict": verdict}


def bust_block(evs):
    nL = sum(e["L"] is not None for e in evs)
    nR = sum(e["R"] is not None for e in evs)
    kL = sum(e["bL"] for e in evs)
    kR = sum(e["bR"] for e in evs)
    p = two_prop(kL, nL, kR, nR)
    if min(nL, nR) < 30:
        winner, verdict = None, "undecided — too few episodes"
    elif p >= ALPHA:
        winner, verdict = None, "undecided — no significant difference (p = %.2g)" % p
    else:
        winner = "L" if kL / nL < kR / nR else "R"
        verdict = "%s busts fewer calls: %.1f %% vs %.1f %% (p = %.2g)" % (
            winner, 100 * min(kL / nL, kR / nR), 100 * max(kL / nL, kR / nR), p)
    return {"L": prop(kL, nL), "R": prop(kR, nR), "p": p, "winner": winner,
            "verdict": verdict}


def latency_block(evs):
    lat = [e["lat"] for e in evs if e["cat"] == "match" and e["lat"] is not None]
    Lf = sum(x > 3 for x in lat)            # R later → L first
    Rf = sum(x < -3 for x in lat)
    ties = len(lat) - Lf - Rf
    p = binom_two_sided(Lf, Lf + Rf)
    if Lf + Rf < MIN_DISCORDANT:
        winner, verdict = None, "undecided — too few matched stations"
    elif p >= ALPHA:
        winner, verdict = None, "undecided — no significant difference (p = %.2g)" % p
    else:
        winner = "L" if Lf > Rf else "R"
        verdict = "%s spots first: %d vs %d times (median R−L %+.0f s, p = %.2g)" % (
            winner, max(Lf, Rf), min(Lf, Rf), median(lat), p)
    edges = [-math.inf] + LAT_EDGES + [math.inf]
    hist = [{"lo": edges[i], "hi": edges[i + 1],
             "n": sum(edges[i] <= x < edges[i + 1] for x in lat)}
            for i in range(len(edges) - 1)]
    for h in hist:
        h["lo"] = None if h["lo"] == -math.inf else h["lo"]
        h["hi"] = None if h["hi"] == math.inf else h["hi"]
    return {"n": len(lat), "median": median(lat) if lat else None,
            "q1": quantile(lat, .25), "q3": quantile(lat, .75),
            "L_first": Lf, "R_first": Rf, "ties": ties, "p": p, "winner": winner,
            "verdict": verdict, "hist": hist}


def bins_block(evs, key, edges):
    U = [e for e in evs if e["inU"] and e[key] is not None]
    edges = [-math.inf] + edges + [math.inf]
    out = []
    for i in range(len(edges) - 1):
        lo, hi = edges[i], edges[i + 1]
        g = [e for e in U if lo <= e[key] < hi]
        b = sum(e["cL"] and not e["cR"] for e in g)
        c = sum(e["cR"] and not e["cL"] for e in g)
        out.append({"lo": None if lo == -math.inf else lo,
                    "hi": None if hi == math.inf else hi, "n": len(g),
                    "L": prop(sum(e["cL"] for e in g), len(g)),
                    "R": prop(sum(e["cR"] for e in g), len(g)),
                    # of the stations R caught, how many L caught too: with
                    # events on R's scale this is the one per-bin number no
                    # side's SNR estimate can bias
                    "LofR": prop(sum(e["cL"] and e["cR"] for e in g),
                                 sum(e["cR"] for e in g)),
                    "b": b, "c": c, "p": binom_two_sided(b, b + c)})
    return out


def snr_edges(evs):
    v = [e["snr"] for e in evs if e["inU"] and e["snr"] is not None]
    if not v:
        return [10, 15, 20, 25, 30]
    lo = int(math.floor(quantile(v, .05) / SNR_STEP)) * SNR_STEP
    hi = int(math.ceil(quantile(v, .95) / SNR_STEP)) * SNR_STEP
    return list(range(lo + SNR_STEP, hi, SNR_STEP)) or [lo + SNR_STEP]


def band_rows(A, evs):
    rows = []
    for band, lo, hi in A.win.common:
        g = [e for e in evs if e["band"] == band]
        U = [e for e in g if e["inU"]]
        lat = [e["lat"] for e in g if e["cat"] == "match"]
        rows.append({
            "band": band, "lo": lo, "hi": hi, "n": len(U),
            "L": prop(sum(e["cL"] for e in U), len(U)),
            "R": prop(sum(e["cR"] for e in U), len(U)),
            "match": sum(e["cat"] == "match" for e in g),
            "onlyL": sum(e["cat"] == "only-L" and e["inU"] for e in g),
            "onlyR": sum(e["cat"] == "only-R" and e["inU"] for e in g),
            "nonCQ": sum(e["cat"] == "nonCQ" for e in g),
            "bustL": sum(e["bL"] for e in g), "bustR": sum(e["bR"] for e in g),
            "epsL": sum(e["L"] is not None for e in g),
            "epsR": sum(e["R"] is not None for e in g),
            "p": binom_two_sided(sum(e["cL"] and not e["cR"] for e in U),
                                 sum(e["cL"] != e["cR"] for e in U)),
            "lat": median(lat) if lat else None})
    return rows


def timeline(A, evs, ta, tb, common):
    span = tb - ta
    step = 3600 if span <= 3 * 86400 else 21600 if span <= 21 * 86400 else 86400
    t = ta - ta % step
    rows = []
    while t < tb:
        u = t + step
        g = [e for e in evs if t <= e["t0"] < u]
        U = [e for e in g if e["inU"]]
        rows.append({"t": t, "n": len(U), "L": sum(e["cL"] for e in U),
                     "R": sum(e["cR"] for e in U),
                     "bustL": sum(e["bL"] for e in g), "bustR": sum(e["bR"] for e in g),
                     "common_s": total(clip(common, t, u))})
        t = u
    spans = clip(A.spans, ta, tb)
    # compared, but while the local skimmer ran another decoder
    other = clip([(a, b) for a, b, eng in A.segments
                  if A.engine is not None and eng != A.engine], ta, tb)
    # everything else in the range that was not compared: outages inside a
    # session and the gaps with no recording at all
    outages = subtract(subtract([(ta, min(tb, A.computed))], common), other)
    rbn_down = []
    for a, b in spans:
        cur = a
        for c, d in clip(A.rbn_up, a, b):
            if c > cur:
                rbn_down.append((cur, c))
            cur = d
        if cur < b:
            rbn_down.append((cur, b))
    return {"step": step, "rows": rows, "outages": outages, "other": other,
            "rbn_down": rbn_down,
            "silences": [s for s in A.silences if s["t1"] > ta and s["t0"] < tb]}


def histograms(A, ta, tb):
    out = []
    for band, clo, chi in A.win.common:
        lo, hi = A.win.region(band)
        lo, hi = math.floor(lo), math.ceil(hi)
        n = hi - lo
        L, R, Rall = [0] * n, [0] * n, [0] * n
        for src, cq_only, dst in ((A.Lall, True, L), (A.Rall, True, R), (A.Rall, False, Rall)):
            for sp in src:
                if not (ta <= sp.t < tb) or not (lo <= sp.f < hi):
                    continue
                if cq_only and sp.cm not in ("CQ", "TEST", "QRZ"):
                    continue
                dst[int(sp.f - lo)] += 1
        (l0, l1), (r0, r1) = A.win.pairs[band]
        out.append({"band": band, "lo": lo, "hi": hi, "local": [l0, l1],
                    "remote": [r0, r1], "common": [clo, chi], "L": L, "R": R, "Rall": Rall})
    return out


def overview(A, ta, tb):
    """The headline numbers of one comparison (one local decoder)."""
    evs = [e for e in A.events if ta <= e["t0"] < tb]
    rec, bust, lat = recall_block(evs), bust_block(evs), latency_block(evs)
    return {"common_s": total(clip(A.common, ta, tb)), "n": rec["n"],
            "recall": {k: rec[k] for k in ("L", "R", "b", "c", "p", "winner")},
            "bust": {k: bust[k] for k in ("L", "R", "p", "winner")},
            "latency": {k: lat[k] for k in ("n", "median", "L_first", "R_first", "p", "winner")}}


def summarize(A, ta, tb):
    evs = [e for e in A.events if ta <= e["t0"] < tb]
    common = clip(A.common, ta, tb)
    cats = {}
    for e in evs:
        k = e["cat"]
        if k in ("only-L", "only-R") and not e["inU"]:
            k += " unverified"
        if k == "bust" and not e["inU"]:
            k = "bust unresolved"
        cats[k] = cats.get(k, 0) + 1
    rec = recall_block(evs)
    bust = bust_block(evs)
    lat = latency_block(evs)
    verdicts = [("Recall", rec["winner"], rec["verdict"]),
                ("Precision", bust["winner"], bust["verdict"]),
                ("Speed", lat["winner"], lat["verdict"])]
    return {
        "range": {"from": ta, "to": tb},
        "params": A.params,
        "windows": A.win.to_json(),
        "sessions": A.sessions,
        "time": {"span_s": total(clip(A.spans, ta, tb)), "common_s": total(common),
                 "rbn_s": total(clip(A.rbn_up, ta, tb))},
        "counts": {
            "spots_L": sum(ta <= sp.t < tb for _, sp in A.Lin),
            "spots_R": sum(ta <= sp.t < tb for _, sp in A.Rcq),
            "spots_R_all": sum(ta <= sp.t < tb for _, sp in A.Rin),
            "events": len(evs), "rbn_spots": A.rbn_n, "rbn_own": A.rbn_own},
        "cats": cats,
        "recall": rec, "bust": bust, "latency": lat,
        "snr": {"offset": A.snr_offset, "offset_n": A.snr_offset_n, "scale": "R",
                "curve": [list(p) for p in A.snr_curve],
                "bins": bins_block(evs, "snr", snr_edges(evs))},
        "wpm": {"bins": bins_block(evs, "wpm", WPM_EDGES)},
        "bands": band_rows(A, evs),
        "timeline": timeline(A, evs, ta, tb, common),
        "hist": histograms(A, ta, tb),
        "verdicts": [{"metric": m, "winner": w, "text": t} for m, w, t in verdicts],
    }
