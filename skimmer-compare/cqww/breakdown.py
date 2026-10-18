#!/usr/bin/env python3
"""breakdown.py — where a variant loses against CW Skimmer, and how it busts.

  breakdown.py VARIANT --band 40m --hours 2012-11-25T02[,...] [--examples 20]

Reads skimmer-compare/logs/cqww/VARIANT/ (spots + parts/*.decodes.log,
parts/*.meta.json from run.py). For every TRUE unit R heard and the
variant did not, the decode log around R's frequency (±200 Hz after the
L−R offset) and time (R's first..last spot ±2 min) says which basket:

  a  silent   — no decoded text there at all
  a  garbage  — text, but no token within edit distance 2 of the call
  b  misread  — a token within edit distance 1..2 (the call torn or mutated)
  c  read     — the exact call is in the text: extractor / station / gate lost it

For every BUST unit of the variant the true call it was nearest to is
compared in Morse: one element added or dropped, a dit<->dah swap, a
character boundary misplaced (split/merge), or something else.
"""
import argparse
import collections
import glob
import json
import os
import re

import score

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "logs", "cqww")
MORSE = {
    'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.',
    'H': '....', 'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.',
    'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-', 'U': '..-',
    'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..', '0': '-----',
    '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....', '6': '-....',
    '7': '--...', '8': '---..', '9': '----.', '/': '-..-.'}
DLOG = re.compile(r"^(\d\d):(\d\d):(\d\d)\s+(\d+\.\d+)\s+\S+\s+\S+\s*(?:L(\d+\.\d+)\s+)?\|(.*)\|$")


def decodes(vdir, band):
    """[(utc, khz, text)] from every part's decode log."""
    out = []
    for meta in glob.glob(os.path.join(vdir, "parts", f"{band}-*.meta.json")):
        with open(meta) as fh:
            t0 = json.load(fh)["t0"]
        log = meta[:-len(".meta.json")] + ".spots.raw.decodes.log"
        if not os.path.exists(log):
            continue
        with open(log, errors="replace") as fh:
            for line in fh:
                m = DLOG.match(line.rstrip("\n"))
                if m and "aux" not in line:
                    t = t0 + int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
                    out.append((t, float(m.group(4)), m.group(6), m.group(5) or ""))
    out.sort()
    return out


def text_near(dec, khz, t_lo, t_hi):
    """Concatenated text per decode frequency (one channel's stream each)."""
    streams = collections.defaultdict(list)
    for t, f, txt, lane in dec:
        if t_lo <= t <= t_hi and abs(f - khz) <= 0.2:
            streams[lane or round(f, 2)].append(txt)
    return ["".join(v) for v in streams.values()]


def tokens(s):
    return [w for w in re.split(r"[\s\xb7]+", s) if w]


def morse(call):
    return [MORSE.get(c, "?") for c in call]


def bust_kind(bust, true):
    """Classify a misread in Morse terms."""
    if len(bust) == len(true):
        diffs = [(b, t) for b, t in zip(bust, true) if b != t]
        if len(diffs) == 1:
            b, t = (MORSE.get(diffs[0][0], ""), MORSE.get(diffs[0][1], ""))
            if len(b) == len(t):
                return "swap dit/dah" if sum(x != y for x, y in zip(b, t)) == 1 else "char replaced"
            if abs(len(b) - len(t)) == 1 and (b.startswith(t) or t.startswith(b)
                                               or b.endswith(t) or t.endswith(b)):
                return "element added" if len(b) > len(t) else "element dropped"
            return "char replaced"
        return "several chars"
    if "".join(morse(bust)) == "".join(morse(true)):
        return "split" if len(bust) > len(true) else "merge"
    if len(bust) < len(true) and (true.startswith(bust) or true.endswith(bust)):
        return "truncated"
    if len(bust) > len(true) and (bust.startswith(true) or bust.endswith(true)):
        return "extra char"
    return "other"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("variant")
    ap.add_argument("--band", required=True)
    ap.add_argument("--hours", required=True)
    ap.add_argument("--examples", type=int, default=15)
    a = ap.parse_args()
    hours = a.hours.split(",")
    vdir = os.path.join(OUT, a.variant)
    spots = sorted(glob.glob(os.path.join(vdir, f"{a.band}-*.spots")))
    ev = score.evaluate(a.band, hours, {a.variant: spots})
    R, V, lab, U, off = ev["R"], ev["V"][a.variant], ev["lab"], ev["U"], ev["off"][a.variant]
    is_true = ev["is_true"]
    dec = decodes(vdir, a.band)

    # --- misses: R's TRUE units the variant did not spot
    miss = [k for k, v in lab["R"].items() if v == "TRUE" and k not in V]
    basket = collections.Counter()
    by_snr = collections.defaultdict(collections.Counter)
    ex = collections.defaultdict(list)
    for k in miss:
        call = k[0]
        rs = R[k]
        khz = sorted(s.khz for s in rs)[len(rs) // 2] + off
        texts = text_near(dec, khz, min(s.t for s in rs) - 150, max(s.t for s in rs) + 150)
        toks = [w for s in texts for w in tokens(s)]
        if not texts:
            b = "a-silent"
        elif call in toks or any(call in s for s in texts):
            b = "c-read"
        else:
            d = min((score.edit(call, w) for w in toks), default=9)
            b = "b-misread" if d <= 2 else "a-garbage"
        basket[b] += 1
        snr = max(s.snr for s in rs)
        by_snr[b][min(snr // 10 * 10, 30)] += 1
        if len(ex[b]) < a.examples:
            near = min(toks, key=lambda w: score.edit(call, w)) if toks else ""
            ex[b].append(f"{call:10} {khz:8.1f} {snr:3d} dB  nearest '{near}'  "
                         f"text: {' | '.join(s[-60:] for s in texts)[:160]}")
    print(f"{a.variant} {a.band} {a.hours}: R TRUE units {sum(1 for v in lab['R'].values() if v == 'TRUE')}, "
          f"missed by L {len(miss)} (offset {off:+.2f} kHz)")
    for b, n in sorted(basket.items()):
        bins = " ".join(f"{s:>2}+:{c:3d}" for s, c in sorted(by_snr[b].items()))
        print(f"  {b:10} {n:4d}   by R dB: {bins}")
    for b in sorted(ex):
        print(f"\n[{b}]")
        for line in ex[b]:
            print("  " + line)

    # --- busts: what kind of misread
    kinds = collections.Counter()
    bex = collections.defaultdict(list)
    true_calls = {k[0] for k in U}
    for k, v in lab[a.variant].items():
        if v != "BUST":
            continue
        call = k[0]
        cands = [tc for tc in true_calls if score.edit(call, tc) <= 2]
        if not cands:
            continue
        tc = min(cands, key=lambda c: (score.edit(call, c), -len(c)))
        kd = bust_kind(call, tc)
        kinds[kd] += 1
        if len(bex[kd]) < 8:
            bex[kd].append(f"{call}<-{tc}")
    nb = sum(kinds.values())
    print(f"\nL busts: {nb}")
    for kd, n in kinds.most_common():
        print(f"  {kd:16} {n:4d}  {' '.join(bex[kd])}")


if __name__ == "__main__":
    main()
