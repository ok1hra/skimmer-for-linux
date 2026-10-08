"""windows.py — which frequencies both skimmers can hear.

Local windows come from what skimmer-headless reported it was listening to
(centre ± rate/2 per band in its status snapshots; its own config where a
connection has no snapshot), remote windows from compare.ini — what R can
spot, which changes with its configuration, so they are a plan by time. A
band may have several remote parts (CW Skimmer Server's CW segments left a
hole at 7035–7045 kHz). Only the intersection is compared, minus a guard at
each edge where one receiver's filter may already roll off.
All frequencies are kHz.
"""
import calendar
import configparser
import time
from bisect import bisect_right


def load_local(path):
    """[(band, lo, hi)] from headless.ini, sorted by frequency."""
    cp = configparser.ConfigParser(inline_comment_prefixes=("#", ";"))
    if not cp.read(path) or not cp.has_section("bands"):
        return []
    rate = cp.getint("radio", "rate", fallback=96000)
    out = []
    for name, centre in cp.items("bands"):
        try:
            c = int(centre) / 1000.0
        except ValueError:
            continue
        out.append((name, c - rate / 2000.0, c + rate / 2000.0))
    return sorted(out, key=lambda w: w[1])


def snapshot_windows(d):
    """((band, lo, hi), ...) from a /status.json snapshot, or None."""
    rate, bands = d.get("rate"), d.get("bands")
    if not rate or not isinstance(bands, list):
        return None
    out = []
    for b in bands:
        try:
            name, c = str(b["band"]), float(b["centre_hz"]) / 1000.0
        except (KeyError, TypeError, ValueError):
            continue
        out.append((name, c - rate / 2000.0, c + rate / 2000.0))
    return tuple(sorted(out, key=lambda w: w[1])) or None


def parse_ranges(text):
    """'1800-1891 3500-3591' → [(1800.0, 1891.0), (3500.0, 3591.0)]"""
    out = []
    for tok in text.replace(",", " ").split():
        lo, sep, hi = tok.partition("-")
        try:
            if sep:
                out.append((float(lo), float(hi)))
        except ValueError:
            pass
    return sorted(out)


def parse_since(s):
    """'2026-10-08T20:41' or '2026-10-08' (UTC) → epoch, or None."""
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return calendar.timegm(time.strptime(s.strip().rstrip("Z"), fmt))
        except ValueError:
            pass
    return None


def remote_plan(cp):
    """[(since epoch, [(lo, hi)])] from compare.ini: [remote windows] lines
    "<UTC time> = ranges", each in force until the next; without them the
    one [remote] windows for all time."""
    plan = []
    if cp.has_section("remote windows"):
        for k, v in cp.items("remote windows"):
            t, r = parse_since(k), parse_ranges(v)
            if t is not None and r:
                plan.append((t, r))
    if not plan:
        r = parse_ranges(cp.get("remote", "windows", fallback=""))
        plan = [(0, r)] if r else []
    return sorted(plan)


def remote_at(plan, t):
    """The remote windows in force at t (the first entry before it begins)."""
    i = bisect_right([s for s, _ in plan], t) - 1
    return plan[max(i, 0)][1]


class Windows:
    def __init__(self, local, remote, guard_khz):
        self.local = local                    # [(band, lo, hi)]
        self.remote = remote                  # [(lo, hi)]
        self.common = []                      # [(band, lo, hi)] — guard applied; a band
                                              #   may have several parts
        self.bands = []                       # [(band, lo, hi)] — one per band, covering
        self.pairs = {}                       # band → (local (lo,hi), remote (lo,hi))
        for name, lo, hi in local:
            parts, rs = [], []
            for rlo, rhi in remote:
                a, b = max(lo, rlo), min(hi, rhi)
                if b - a > 2 * guard_khz:
                    parts.append((name, a + guard_khz, b - guard_khz))
                    rs.append((rlo, rhi))
            if parts:
                self.common += parts
                self.bands.append((name, parts[0][1], parts[-1][2]))
                self.pairs[name] = ((lo, hi), (rs[0][0], rs[-1][1]))
        self.remote_only = [(lo, hi) for lo, hi in remote
                            if not any(min(hi, h) > max(lo, l) for _, l, h in local)]
        self.local_only = [n for n, _, _ in local if n not in self.pairs]

    def band(self, f):
        """Common-window band of f, or None outside the compared range."""
        for name, lo, hi in self.common:
            if lo <= f <= hi:
                return name
        return None

    def region(self, band):
        """The span that covers both raw windows of a band (histograms)."""
        (l0, l1), (r0, r1) = self.pairs[band]
        return min(l0, r0), max(l1, r1)

    def parts(self, band):
        return [(lo, hi) for n, lo, hi in self.common if n == band]

    def to_json(self):
        return {
            "common": [{"band": n, "lo": lo, "hi": hi} for n, lo, hi in self.common],
            "local": [{"band": n, "lo": lo, "hi": hi} for n, lo, hi in self.local],
            "remote": [{"lo": lo, "hi": hi} for lo, hi in self.remote],
            "remote_only": [{"lo": lo, "hi": hi} for lo, hi in self.remote_only],
            "local_only": self.local_only,
        }


def loose_bands(local, margin_khz):
    """Local windows widened by a margin — what the RBN index keeps."""
    return [(n, lo - margin_khz, hi + margin_khz) for n, lo, hi in local]
