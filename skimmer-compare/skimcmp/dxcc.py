"""dxcc.py — a call's DXCC as skimmer-headless tells it, and its [filter].

A port of src/headless/dxcc.c (itself SkMap.locate of web/map.js and
IP-rotator's canonicalizeDxccPrefixAlias): the call's longest part (less
/P, /M, /MM, /AM, /QRP, /digit), its canonical prefix (the special cases
by hand, then the longest alias prefix), and the label of that prefix or
of its first 4, 3, 2, 1 characters; the same for the part itself. The
tables are parsed from the same web/map/*.js files headless builds in. A
change in dxcc.c needs its twin here — test-compare checks the port
against skimmer-dxcc-test when a build of it is at hand.

HeadlessFilter is headless.ini's [filter] as skim_hd_config_call_pass
applies it: bad (prefixes, whole calls, country names), then call (a
caseless regex with regex=true, else a caseless substring). The mycall
rule (drop busts of the own call) is left out: compare excludes the own
call anyway.
"""
import os
import re

LABEL_RE = re.compile(r'\[-?\d+,-?\d+,"([^"]+)",-?\d+,"([^"]*)"\]')
ALIAS_RE = re.compile(r'addAliases\(aliasMap,\s*"([^"]+)",\s*\[([^\]]*)\]\)')
QUOTED = re.compile(r'"([^"]+)"')
# other names a DXCC goes by, for the bad filter's country names (dxcc.c ALSO)
ALSO = {"EU": "BELORUSSIA BYELORUSSIA", "UA2": "RUSSIA", "R1FJ": "RUSSIA"}
EXACT = ("JD1/M", "JD1/O", "EA6", "EA8", "EA9", "CT3", "CU", "IS0", "3B9")
UK = (("GI", "2I", "MI", "GI"), ("GM", "2M", "MM", "GM"), ("GW", "2W", "MW", "GW"),
      ("GD", "2D", "MD", "GD"), ("GJ", "2J", "MJ", "GJ"), ("GU", "2U", "MU", "GU"))
SKIP = ("P", "M", "MM", "AM", "QRP")


def _up(s):
    """g_ascii_strup: ASCII letters only."""
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


def _dig(c):
    return "0" <= c <= "9"


def _upper(c):
    return "A" <= c <= "Z"


class Dxcc:
    def __init__(self, map_dir):
        """map_dir: the web/map directory (map50.js, dxcc_prefix_aliases.js).
        OSError when it cannot be read."""
        self.pfx = {}          # prefix → (name, also)
        self.alias = {}        # alias prefix → canonical prefix
        with open(os.path.join(map_dir, "map50.js"), encoding="utf-8", errors="replace") as fh:
            js = fh.read()
        p = js.find("DXCC_PREFIX_LABELS")
        end = js.find("];", p) if p >= 0 else -1
        if p >= 0 and end >= 0:
            for m in LABEL_RE.finditer(js, p, end):
                self.pfx.setdefault(m.group(1), (m.group(2), ALSO.get(m.group(1))))
        with open(os.path.join(map_dir, "dxcc_prefix_aliases.js"), encoding="utf-8",
                  errors="replace") as fh:
            js = fh.read()
        for m in ALIAS_RE.finditer(js):
            for a in QUOTED.findall(m.group(2)):
                self.alias[_up(a)] = m.group(1)      # a later one wins, as aliasMap[] =
        self.alias_max = max((len(a) for a in self.alias), default=0)
        self.memo = {}

    def canonical(self, s):
        if not s:
            return ""
        c = lambda i: s[i] if i < len(s) else ""
        for e in EXACT:
            if s.startswith(e):
                return e
        if s[0] in "ANW" and c(1) == "H" and _dig(c(2)):
            return "KH" + s[2]
        if s[0] == "L" and "2" <= c(1) <= "9":
            return "LU"
        for a, b, d, canon in UK:
            if s.startswith(a) or s.startswith(b) or s.startswith(d):
                return canon
        if s[0] == "M" or s.startswith(("2E", "GB", "GX", "MX")):
            return "G"
        r = s[0] == "R"
        u = s[0] == "U" and "A" <= c(1) <= "I"
        if (r and (_dig(c(1)) or (_upper(c(1)) and _dig(c(2))))) or \
                (u and (_dig(c(2)) or (_upper(c(2)) and _dig(c(3))))):
            if s.startswith("UA2"):
                return "UA2"
            d = next((x for x in s if _dig(x)), "")
            return "UA/AS" if d in ("0", "8", "9") else "UA/E"
        for k in range(min(len(s), self.alias_max), 0, -1):
            hit = self.alias.get(s[:k])
            if hit is not None:
                return hit
        return s

    def by_prefix(self, cand):
        d = self.pfx.get(cand)
        if d is not None:
            return cand
        for n in range(min(4, len(cand)), 0, -1):
            if cand[:n] in self.pfx:
                return cand[:n]
        return None

    def locate(self, call):
        """The DXCC prefix label of call, or None."""
        hit = self.memo.get(call, False)
        if hit is not False:
            return hit
        base = ""
        for x in _up(call).split("/"):
            if not x or x in SKIP or (len(x) == 1 and _dig(x)):
                continue
            if len(x) > len(base):
                base = x                 # the first of the longest
        b = "".join(c for c in base if _upper(c) or _dig(c) or c == "/")
        canon = self.canonical(b)
        d = self.by_prefix(canon) if canon else None
        if d is None and base:
            d = self.by_prefix(base)
        self.memo[call] = d
        return d

    def name_has(self, call, text_upper):
        d = self.locate(call)
        if d is None:
            return False
        name, also = self.pfx[d]
        return text_upper in _up(name) or bool(also and text_upper in _up(also))


def read_filter(path):
    """headless.ini's [filter] {key: value}, None when the file cannot be read."""
    sec, kv = None, {}
    try:
        with open(path, errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("["):
                    sec = line.strip("[]").strip()
                elif sec == "filter" and "=" in line and not line.startswith(("#", ";")):
                    k, _, v = line.partition("=")
                    kv[k.strip()] = v.strip()
    except OSError:
        return None
    return kv


class HeadlessFilter:
    """passes(call): would skimmer-headless's [filter] let call out?"""

    def __init__(self, kv, dxcc=None):
        """kv: read_filter()'s dict; dxcc: a Dxcc, needed for country names
        (ValueError without one when bad= names a country)."""
        self.call = kv.get("call", "")
        self.regex = kv.get("regex", "false").lower() in ("true", "1", "yes")
        self.bad = kv.get("bad", "")
        self.re = re.compile(self.call, re.I) if self.call and self.regex else None
        self.sub = _up(self.call) if self.call and not self.regex else None
        self.pfx, self.names = [], []
        for t in re.split(r"[,;]", self.bad):
            x = _up(t.strip())
            if not x:
                continue
            letters = all(_upper(c) or c in " .-'" for c in x)
            if letters and sum(_upper(c) for c in x) >= 4:
                self.names.append(x)
            elif all(_upper(c) or _dig(c) or c == "/" for c in x):
                self.pfx.append(x)
            else:
                raise ValueError("[filter] bad: %r is neither a prefix, a call nor a country" % x)
        if self.names and dxcc is None:
            raise ValueError("[filter] bad names a country, but the DXCC map data is missing")
        self.dxcc = dxcc

    def __bool__(self):
        return bool(self.call or self.pfx or self.names)

    def describe(self):
        out = []
        if self.call:
            out.append("call=%s%s" % (self.call, "" if self.regex else " (substring)"))
        if self.bad:
            out.append("bad=%s" % self.bad)
        return ", ".join(out) or "no filter"

    def bad_call(self, call):
        up = _up(call)
        parts = up.split("/")
        for b in self.pfx:
            if up.startswith(b) or b in parts:
                return True
        return any(self.dxcc.name_has(up, n) for n in self.names)

    def passes(self, call):
        if (self.pfx or self.names) and self.bad_call(call):
            return False
        if self.re is not None:
            return self.re.search(call) is not None
        if self.sub is not None:
            return self.sub in _up(call)
        return True
