"""windows.py — which frequencies both skimmers can hear.

Local windows come from skimmer-headless's own config (centre ± rate/2 per
band), remote windows from compare.ini. Only the intersection is compared,
minus a guard at each edge where one receiver's filter may already roll off.
All frequencies are kHz.
"""
import configparser


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


class Windows:
    def __init__(self, local, remote, guard_khz):
        self.local = local                    # [(band, lo, hi)]
        self.remote = remote                  # [(lo, hi)]
        self.common = []                      # [(band, lo, hi)] — guard applied
        self.pairs = {}                       # band → (local (lo,hi), remote (lo,hi))
        for name, lo, hi in local:
            for rlo, rhi in remote:
                a, b = max(lo, rlo), min(hi, rhi)
                if b - a > 2 * guard_khz:
                    self.common.append((name, a + guard_khz, b - guard_khz))
                    self.pairs[name] = ((lo, hi), (rlo, rhi))
                    break
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
