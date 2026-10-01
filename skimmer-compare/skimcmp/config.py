"""config.py — compare.ini and the matching parameters.

The parameters have defaults here, compare.ini may override them, and every
request from the page may override them again (query string). Out-of-range
or malformed values fall back to the base value instead of failing.
"""
import configparser
import os

from .windows import load_local, parse_ranges

# name: (default, min, max, meaning) — the page shows the meaning as help
PARAMS = {
    "gap_min":       (20,  1,   240,  "episode: spots of one call on one band merge while the gap is at most this many minutes"),
    "slack_min":     (5,   0,   60,   "match: L and R episodes of a call may miss each other by this many minutes"),
    "df_khz":        (1.5, 0.1, 10,   "match: largest frequency difference of a matched call, kHz"),
    "bust_df_hz":    (300, 10,  3000, "bust pair: different calls this close in frequency, Hz"),
    "bust_dt_s":     (120, 5,   1800, "bust pair: … and this close in time, seconds"),
    "bust_edit":     (2,   0,   4,    "bust pair: … and at most this edit distance (or one a prefix/suffix of the other)"),
    "guard_khz":     (0.5, 0,   10,   "window edges: ignore this much at each edge of the common window, kHz"),
    "rbn_win_min":   (10,  1,   120,  "RBN referee: another skimmer spotted the call within this many minutes of the episode"),
    "rbn_df_khz":    (1.0, 0.1, 5,    "RBN referee: … within this many kHz"),
    "lost_pct":      (1.0, 0,   100,  "local health: a status snapshot with this much packet loss or more excludes its interval"),
    "silence_min":   (15,  1,   240,  "suspicious silence: a band silent this many minutes on one side …"),
    "silence_other": (5,   1,   1000, "… while the other side spotted at least this many there"),
}


def _num(v, like):
    return int(float(v)) if isinstance(like, int) else float(v)


def clamp_params(src, base):
    """base values overridden by src (dict of strings) where valid."""
    out = dict(base)
    for k, (d, lo, hi, _) in PARAMS.items():
        if k in src:
            try:
                v = _num(src[k], d)
            except (TypeError, ValueError):
                continue
            if lo <= v <= hi:
                out[k] = v
    return out


class Config:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        here = os.path.dirname(self.path)
        cp = configparser.ConfigParser(inline_comment_prefixes=("#", ";"))
        if not cp.read(self.path):
            raise SystemExit("cannot read %s" % self.path)

        def p(key, default):
            v = os.path.expanduser(cp.get("paths", key, fallback=default))
            return v if os.path.isabs(v) else os.path.join(here, v)

        self.logs = p("logs", "logs")
        self.headless_ini = p("headless_ini", "~/.config/skimmer-for-linux/headless.ini")
        self.scp = p("scp", "~/.config/skimmer-for-linux/master.scp")
        self.local_windows = load_local(self.headless_ini)
        self.remote_windows = parse_ranges(cp.get("remote", "windows", fallback=""))
        if not self.local_windows or not self.remote_windows:
            raise SystemExit("no local windows (%s) or no remote windows (%s)"
                             % (self.headless_ini, self.path))
        self.local_name = cp.get("local", "name", fallback="Local skimmer")
        self.local_about = cp.get("local", "about", fallback="")
        self.remote_name = cp.get("remote", "name", fallback="Remote skimmer")
        self.remote_about = cp.get("remote", "about", fallback="")
        self.default_engine = cp.get("local", "default_engine", fallback="cw-v2")
        # decoder key → (label, what it is)
        self.engines = {}
        if cp.has_section("engines"):
            for k, v in cp.items("engines"):
                label, _, what = v.partition("|")
                self.engines[k] = (label.strip() or k, what.strip())
        self.exclude = [c.strip().upper() for c in
                        cp.get("rbn", "exclude", fallback="").replace(",", " ").split()]
        self.listen = cp.get("web", "listen", fallback="0.0.0.0")
        self.port = cp.getint("web", "port", fallback=8074)
        base = {k: v[0] for k, v in PARAMS.items()}
        self.params = clamp_params(dict(cp.items("match")) if cp.has_section("match") else {},
                                   base)
