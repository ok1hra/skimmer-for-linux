"""fixsrc.py — what put an L spot on the wire: the free text, or a call the
v3 call reader (decode_cw_v3.c, M8) wrote as a correction token.

The decode tap (gatelog.h) carries the reader's corrections as
"\\x01CALL\\x02" inside a decode's text. For every such correction the
channel's own text of the 10 s before it says what kind it was:

  C  the text read the call itself (whole, or torn into tokens)
  B  the text read a LONGER call that this one is the head of ("SM5D AJ"
     → SM5D): a cut
  A  the text read a different valid call within two edits (W1KE → W1KT)
  D  nothing in the text like it (a short call fitted into noise: E2E)

An L episode is the reader's when a correction of its call came on its
frequency (±400 Hz) in the 15 min before its first spot; its class is C when
any of those corrections was, else the last one's. Live 2026-11-15..17:
C 773 found / 94 found by MASTER.SCP alone / 10 busts; A 56/66/10;
B 48/40/16; D 70/162/4.

Python 3 standard library; the tap is pre-filtered by awk (a live tap is
GBs).
"""
import bisect
import glob
import os
import re
import subprocess

from .match import levenshtein

FIX = re.compile("\x01([^\x02]*)\x02")
SHAPE = re.compile(r"^(?=.*[A-Z])(?=.*[0-9])[A-Z0-9]{1,3}[0-9][A-Z0-9]{0,3}[A-Z]$")
WORDS = {"K", "KN", "BK", "CQ", "DE", "TEST", "TU", "UP", "PSE", "QRZ", "5NN", "599", "+",
         "AR", "SK", "DX", "EU", "NA", "R", "E", "T", "I", "EE", "73", "GL", "QRL", "QRL?",
         "?", "QRP", "POTA", "SOTA", "WWFF", "FD", "SST", "CWT", "CQDX", "K+"}

# one line per decode that carries a correction: wall s, hz, snr, text, and
# the same channel's text of the 10 s before it
AWK = r'''
$1=="T" { ix=$4; t=$3/1e6; s=substr($0, index($0,"|")+1); sub(/\|$/,"",s);
  if (index(s,"\001")) { c=""; n=cnt[ix]; for (k=n-60; k<=n; k++) if (k>0 && tm[ix,k]>=t-10) c=c tx[ix,k];
     printf "%.1f\t%s\t%s\t%s\t%s\n", t, $5, $7, s, c }
  n=++cnt[ix]; tm[ix,n]=t; tx[ix,n]=s; if (n>61) { delete tm[ix,n-61]; delete tx[ix,n-61] } }
'''


def classify(call, text):
    toks = [t for t in re.split(r"[ ·]+", text.upper()) if t]
    for i, t in enumerate(toks):
        if t == call and i + 1 < len(toks):
            z = toks[i + 1]
            if z not in WORDS and len(z) <= 3 and z.isalnum() and SHAPE.match(call + z):
                return "B"
    for i in range(len(toks)):
        j = ""
        for k in range(i, min(len(toks), i + 6)):
            j += toks[k]
            if len(j) > len(call) + 3:
                break
            if k > i and j != call and SHAPE.match(j) and j.startswith(call) and call not in toks:
                return "B"
    if call in toks:
        return "C"
    for i in range(len(toks)):
        j = ""
        for k in range(i, min(len(toks), i + 6)):
            j += toks[k]
            if j == call:
                return "C"
    for t in toks:
        if (t != call and len(t) >= 4 and SHAPE.match(t) and len(t) >= len(call)
                and levenshtein(call, t, 2) <= 2):
            return "A"
    return "D"


def band_of(path):
    m = re.match(r"tap-(\d+m)(?:-|\.)", os.path.basename(path))
    return m.group(1) if m else None


class FixSource:
    def __init__(self, paths):
        """paths: decode taps (tap-<band>.log or tap-<band>-<ts>.log); the
        RTTY taps (tap-<band>-rtty-…) are skipped."""
        self.fix = {}
        env = dict(os.environ, LC_ALL="C")
        awk = "mawk" if os.path.exists("/usr/bin/mawk") else "awk"
        procs = {}
        for p in paths:
            b = band_of(p)
            if b is None or "-rtty-" in os.path.basename(p):
                continue
            procs.setdefault(b, []).append(
                subprocess.Popen([awk, AWK, p], stdout=subprocess.PIPE, env=env))
        for b, ps in procs.items():
            rows = []
            for pr in ps:
                for ln in pr.communicate()[0].decode("utf-8", "replace").splitlines():
                    f = ln.split("\t")
                    if len(f) < 5:
                        continue
                    calls = [c for c in FIX.findall(f[3]) if c != "CQ"]
                    if not calls:
                        continue
                    text = FIX.sub("", f[4]) + FIX.sub("", f[3])
                    rows.append((float(f[0]), float(f[1]), calls, text))
            rows.sort(key=lambda r: r[0])
            self.fix[b] = (rows, [r[0] for r in rows])

    @classmethod
    def from_glob(cls, pattern):
        return cls(sorted(glob.glob(os.path.expanduser(pattern))))

    def source(self, band, call, f_khz, t0, back=900, fwd=5, df_hz=400):
        """'text', or the reader's class (C/B/A/D) of the episode."""
        rows, ts = self.fix.get(band, ([], []))
        i, j = bisect.bisect_left(ts, t0 - back), bisect.bisect_right(ts, t0 + fwd)
        cl = [classify(call, r[3]) for r in rows[i:j]
              if call in r[2] and abs(r[1] - f_khz * 1e3) <= df_hz]
        if not cl:
            return "text"
        return "C" if "C" in cl else cl[-1]


def outcome(e):
    """An L episode's fate: bust, found but only MASTER.SCP vouches for it,
    or good (confirmed by RBN or R, or a real call R heard without CQ)."""
    if e["bL"]:
        return "bust"
    if e["inU"] and e["cL"] and e["v"] == "scp" and e["cat"] != "match":
        return "scp"
    return "good"


def table(fs, events):
    """{source: {outcome: n}} over the L episodes of events."""
    out = {}
    for e in events:
        L = e["L"]
        if L is None or (e["cat"] == "bust" and not e["bL"] and not e["cL"]):
            continue
        s = fs.source(e["band"], L["call"], L["f"], L["t0"])
        d = out.setdefault(s, {"good": 0, "scp": 0, "bust": 0})
        d[outcome(e)] += 1
    return out
