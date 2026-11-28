"""config.py — compare.ini and the matching parameters.

The parameters have defaults here and compare.ini may override them
([match]); the public page cannot. Out-of-range or malformed values fall back
to the base value instead of failing.
"""
import configparser
import os
import re
import sys

from .callfilter import CallFilter, filter_plan
from .dxcc import Dxcc, HeadlessFilter, read_filter
from .windows import load_local, parse_since, remote_plan

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
    "held_df_khz":   (0.3, 0.05, 2,   "held frequency: a call only MASTER.SCP vouches for, where RBN hears another call within this many kHz …"),
    "held_dt_min":   (5,   1,   60,   "… within this many minutes of the episode …"),
    "held_n":        (3,   1,   100,  "… at least this many times, is that station's frequency, not the call's: out of recall"),
    "lost_pct":      (1.0, 0,   100,  "local health: a status snapshot with this much packet loss or more excludes its interval"),
    "silence_min":   (15,  1,   240,  "suspicious silence: a band silent this many minutes on one side …"),
    "silence_other": (5,   1,   1000, "… while the other side spotted at least this many there"),
    "dead_min":      (20,  1,   1440, "dead feed: one side spotted nothing at all, on any band, for this many compared minutes …"),
    "dead_other":    (10,  1,   10000, "… while the other side spotted at least this many — that time is not compared (radio taken by another client, stuck decoder)"),
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


# calls the filter check tries besides MASTER.SCP's: one of many countries
# and the forms a prefix rule may read otherwise (portable, special event)
PROBES = ("OK1ABC", "OL5Y", "DL1ABC", "DA0HQ", "G4ABC", "M0ABC", "GM3ABC", "EI2AB", "F5ABC",
          "ON4ABC", "PA3ABC", "HB9ABC", "OE1ABC", "I2ABC", "EA3ABC", "CT1ABC", "SP5ABC",
          "OM3ABC", "HA5ABC", "YO3ABC", "LZ1ABC", "SV1ABC", "9A2AB", "S51AB", "YU1AB", "E71AB",
          "Z31AB", "ZA1AB", "LY2AB", "YL2AB", "ES5AB", "OH2AB", "SM5ABC", "LA9AB", "OZ1ABC",
          "TF3AB", "UR5ABC", "US0AB", "EW1ABC", "EU1AB", "EV1A", "ER1AB", "4L1AB", "4X1AB",
          "TA1AB", "UN7AB", "UK8AB", "EX8AB", "UA1ABC", "UA3ABC", "RA3AB", "R3AB", "RN9AB",
          "UA9ABC", "UA0ABC", "UA2FF", "RA2FA", "R1FJ", "R2026DX", "UAA0AA", "RA/OK1ABC",
          "DL/UA3ABC", "UA3ABC/P", "OK1ABC/UA9", "K1ABC", "W1AW", "N2AB", "KH6AB", "AH6AB",
          "KL7AB", "VE3ABC", "XE1AB", "PY2AB", "LU1AB", "L21AB", "CE3AB", "ZS6AB", "5B4AB",
          "JA1ABC", "BY1AB", "HL2AB", "VU2AB", "VK2ABC", "ZL2AB", "9M2AB", "YB1AB", "A61AB")


def _scp_calls(path):
    calls = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip().upper()
                if line and not line.startswith(("#", "!!")):
                    calls.add(line)
    except OSError:
        pass
    return calls


def filter_drift(headless_ini, cf, scp_path, map_dir):
    """A warning when the newest [local filter] entry does not filter as
    headless.ini's [filter] (call, regex, bad — prefixes, calls, country
    names) does now, tried call by call on MASTER.SCP and PROBES: L refuses
    a call compare keeps (W credited with it), or the other way round (data
    left out for nothing)."""
    kv = read_filter(headless_ini)
    if kv is None:
        return ""
    try:
        try:
            hf = HeadlessFilter(kv)
        except ValueError:
            if not kv.get("bad"):
                raise
            hf = HeadlessFilter(kv, Dxcc(map_dir))     # a country name: the map's data
    except (OSError, ValueError, re.error) as e:
        return "cannot check the local skimmer's [filter] against compare.ini [local filter]: %s" % e
    last = cf.plan[-1][1] if cf.plan else None
    if not hf and last is None:
        return ""
    keeps = (lambda c: True) if last is None else (lambda c: last.search(c) is not None)
    calls = sorted(_scp_calls(scp_path) | set(PROBES))
    l_out = [c for c in calls if not hf.passes(c) and keeps(c)]
    c_out = [c for c in calls if hf.passes(c) and not keeps(c)]
    if not l_out and not c_out:
        return ""
    eg = lambda xs: ", ".join(xs[:6]) + (" …" if len(xs) > 6 else "")
    say = []
    if l_out:
        say.append("%d the skimmer refuses but compare keeps (W credited with them: %s)"
                   % (len(l_out), eg(l_out)))
    if c_out:
        say.append("%d the skimmer spots but compare leaves out (%s)" % (len(c_out), eg(c_out)))
    return ("the local skimmer's [filter] (%s) and compare.ini [local filter]'s last line (%s) "
            "disagree on %d of %d calls tried: %s — add a line \"<UTC since> = <regex>\" "
            "(or \"<UTC since> = bad=<its bad list>\")"
            % (hf.describe(), last.pattern if last is not None else "empty",
               len(l_out) + len(c_out), len(calls), "; ".join(say)))


class Config:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        here = os.path.dirname(self.path)
        # "=" only: [remote windows] keys are times, "2026-10-08T20:41"
        cp = configparser.ConfigParser(delimiters=("=",), inline_comment_prefixes=("#", ";"))
        if not cp.read(self.path):
            raise SystemExit("cannot read %s" % self.path)

        def p(key, default):
            v = os.path.expanduser(cp.get("paths", key, fallback=default))
            return v if os.path.isabs(v) else os.path.join(here, v)

        self.logs = p("logs", "logs")
        self.headless_ini = p("headless_ini", "~/.config/skimmer-for-linux/headless.ini")
        self.scp = p("scp", "~/.config/skimmer-for-linux/master.scp")
        self.local_windows = load_local(self.headless_ini)
        self.remote_windows = remote_plan(cp)          # [(since, [(lo, hi)])]
        if not self.local_windows or not self.remote_windows:
            raise SystemExit("no local windows (%s) or no remote windows (%s)"
                             % (self.headless_ini, self.path))
        # the web page's map data: headless tells a call's country from it
        self.map_dir = p("map", "../web/map")
        self.call_filter = CallFilter(filter_plan(cp, self.map_dir))    # L's [filter] by time
        self.filter_about = cp.get("local", "filter_about", fallback="", raw=True)
        self.filter_warning = filter_drift(self.headless_ini, self.call_filter, self.scp,
                                           self.map_dir)
        if self.filter_warning:
            sys.stderr.write("WARNING: %s\n" % self.filter_warning)
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
        self.port = cp.getint("web", "port", fallback=8075)
        # how often the page's numbers (data.json) are recomputed, seconds
        self.every = max(15, cp.getint("web", "every_s", fallback=60))
        # the page joined over sessions: one decoder (engine) from a UTC time
        # on (since), every session recorded since then — this one included
        # and the ones still to come; both empty = the current session, its
        # newest decoder
        self.web_engine = cp.get("web", "engine", fallback="").strip()
        self.web_since = parse_since(cp.get("web", "since", fallback="") or "")
        if cp.get("web", "since", fallback="").strip() and self.web_since is None:
            print("compare: [web] since = %r is no UTC time, ignored"
                  % cp.get("web", "since"), file=sys.stderr)
        base = {k: v[0] for k, v in PARAMS.items()}
        self.params = clamp_params(dict(cp.items("match")) if cp.has_section("match") else {},
                                   base)
