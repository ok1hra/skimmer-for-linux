"""arbiter.py — the referee: is a callsign real?

First the Reverse Beacon Network: the call is real when another skimmer
(never our own spotter call) spotted it on the same band near the same
frequency and time. When RBN has nothing, MASTER.SCP decides. Neither →
"none" (unverified — most likely a busted call).

The RBN index holds only spots inside the local windows (plus a margin),
in compact per-band arrays: a busy day is a few hundred thousand spots.
"""
from array import array
from bisect import bisect_left, bisect_right


class RbnIndex:
    def __init__(self, bands, exclude):
        self.bands = bands                       # [(band, lo, hi)] kHz
        self.exclude = set(exclude)
        self.calls, self.cid = [], {}
        self.spotters, self.sid = [], {}
        self.t = {b: array("q") for b, _, _ in bands}
        self.f = {b: array("f") for b, _, _ in bands}
        self.c = {b: array("l") for b, _, _ in bands}
        self.s = {b: array("l") for b, _, _ in bands}
        self.snr = {b: array("h") for b, _, _ in bands}
        self.by_call = {}                        # (band, cid) → array of positions
        self.n = 0
        self.own = 0                             # dropped: our own spotter
        self.late = 0                            # dropped: older than last_t
        self.last_t = None

    def _id(self, table, index, key):
        i = index.get(key)
        if i is None:
            i = index[key] = len(table)
            table.append(key)
        return i

    def add(self, sp):
        base = sp.spotter.split("-")[0]
        if base in self.exclude:
            self.own += 1
            return
        # Sessions are read one after another, and two that overlap (a short
        # run started beside the running recorder) recorded the same RBN
        # stream: an older spot is a duplicate. Dropping it keeps the arrays
        # sorted for bisect and last_t the true end of the RBN record.
        if self.last_t is not None and sp.t < self.last_t:
            self.late += 1
            return
        band = None
        for b, lo, hi in self.bands:
            if lo <= sp.f <= hi:
                band = b
                break
        if band is None:
            return
        ci = self._id(self.calls, self.cid, sp.call)
        si = self._id(self.spotters, self.sid, sp.spotter)
        pos = len(self.t[band])
        self.t[band].append(sp.t)
        self.f[band].append(sp.f)
        self.c[band].append(ci)
        self.s[band].append(si)
        self.snr[band].append(max(-32768, min(32767, sp.snr)))
        key = (band, ci)
        a = self.by_call.get(key)
        if a is None:
            a = self.by_call[key] = array("l")
        a.append(pos)
        self.n += 1
        self.last_t = sp.t

    def spotters_of(self, call, band, ta, tb, f, df):
        """Distinct other spotters of call on band in [ta, tb], |Δf| ≤ df."""
        ci = self.cid.get(call)
        if ci is None or band not in self.t:
            return set()
        pos = self.by_call.get((band, ci))
        if not pos:
            return set()
        T, F, S = self.t[band], self.f[band], self.s[band]
        return {S[p] for p in pos if ta <= T[p] <= tb and abs(F[p] - f) <= df}

    def near(self, band, ta, tb, f, df, limit=200):
        """[(t, spotter, f, call, snr)] on band in [ta, tb], |Δf| ≤ df."""
        if band not in self.t:
            return []
        T = self.t[band]
        i, j = bisect_left(T, ta), bisect_right(T, tb)
        F, C, S, N = self.f[band], self.c[band], self.s[band], self.snr[band]
        out = []
        for p in range(i, j):
            if abs(F[p] - f) <= df:
                out.append((T[p], self.spotters[S[p]], round(F[p], 1),
                            self.calls[C[p]], N[p]))
                if len(out) >= limit:
                    break
        return out

    def of_call(self, call, band, ta, tb, limit=200):
        ci = self.cid.get(call)
        if ci is None or band not in self.t:
            return []
        pos = self.by_call.get((band, ci)) or []
        T, F, S, N = self.t[band], self.f[band], self.s[band], self.snr[band]
        out = [(T[p], self.spotters[S[p]], round(F[p], 1), call, N[p])
               for p in pos if ta <= T[p] <= tb]
        return out[:limit]


def load_scp(path):
    calls = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip().upper()
                if line and not line.startswith("#"):
                    calls.add(line)
    except OSError:
        pass
    return calls


RANK = {"rbn": 2, "scp": 1, "none": 0}


class Arbiter:
    def __init__(self, rbn, scp, win_s, df_khz):
        self.rbn, self.scp, self.win, self.df = rbn, scp, win_s, df_khz
        self.cache = {}

    def verdict(self, call, band, t0, t1, f):
        """('rbn', n spotters) | ('scp', 0) | ('none', 0)"""
        key = (call, band, t0, t1, round(f, 1))
        v = self.cache.get(key)
        if v is None:
            n = len(self.rbn.spotters_of(call, band, t0 - self.win, t1 + self.win, f, self.df))
            if n:
                v = ("rbn", n)
            elif call in self.scp:
                v = ("scp", 0)
            else:
                v = ("none", 0)
            self.cache[key] = v
        return v
