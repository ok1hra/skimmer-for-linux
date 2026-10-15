#!/usr/bin/env python3
"""tap-score.py — score a decode-tap replay the way compare-web scores live.

  tap-score.py SESSION ROWSDIR [ROWSDIR …] [--to UTC] [--json OUT]

skimmer-tap-replay --rows writes every line the feed put on the wire as an
"ev":"spot" row. This builds a copy of compare's session SESSION in which
the local log is those lines instead of what the live skimmer sent (its
connection events kept), and runs the same analysis as compare-web over
it: recall and bust against CW Skimmer Server, per band and per bin of R's
SNR. A change to the extractor, the station table or the feed gate is then
measured on days of real air in minutes. Each ROWSDIR holds rows-<band>.jsonl
of one variant ("live": the live feed itself); --to cuts the range where the taps end.
"""
import argparse
import calendar
import glob
import json
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.arbiter import RbnIndex, load_scp           # noqa: E402
from skimcmp.config import Config                         # noqa: E402
from skimcmp.logs import Store                            # noqa: E402
from skimcmp.match import analyze                         # noqa: E402
from skimcmp.stats import summarize                       # noqa: E402
from skimcmp.windows import loose_bands                   # noqa: E402


def utc(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def spot_lines(rowsdir, call="OK1HRA-#"):
    out = []
    for path in glob.glob(os.path.join(rowsdir, "rows-*.jsonl")):
        with open(path) as fh:
            for line in fh:
                if not line.startswith('{"ev":"spot"'):
                    continue
                r = json.loads(line)
                t = r["w"]
                out.append((t, f"{utc(t)}\tDX de {call}:{r['hz'] / 1e3:9.1f}  {r['call']:<12} CW  "
                               f"{r['snr']:4.0f} dB {r['wpm']:3.0f} WPM  CQ      "
                               f"{time.strftime('%H%MZ', time.gmtime(t))}"))
    return out


def score(cfg, T, rowsdir, to):
    tmp = tempfile.mkdtemp(prefix="tapscore-")
    try:
        for kind in ("remote", "rbn", "events"):
            src = os.path.join(cfg.logs, f"{kind}-{T}.log")
            if os.path.exists(src):
                os.symlink(src, os.path.join(tmp, os.path.basename(src)))
        st = os.path.join(cfg.logs, f"status-{T}.jsonl")
        if os.path.exists(st):
            os.symlink(st, os.path.join(tmp, os.path.basename(st)))
        lines = []
        with open(os.path.join(cfg.logs, f"local-{T}.log"), errors="replace") as fh:
            for line in fh:
                if line.startswith("#") or (rowsdir == "live" and "\t" in line):
                    t = calendar.timegm(time.strptime(line.lstrip("#")[:20], "%Y-%m-%dT%H:%M:%SZ"))
                    lines.append((t, line.rstrip("\n")))
        if rowsdir != "live":                     # "live": the feed as it was
            lines += spot_lines(rowsdir)
        lines.sort(key=lambda x: x[0])
        with open(os.path.join(tmp, f"local-{T}.log"), "w") as fh:
            fh.write("\n".join(x[1] for x in lines) + "\n")
        bands = loose_bands(cfg.local_windows, 2.0)
        store = Store(tmp, lambda: RbnIndex(bands, cfg.exclude))
        store.poll()
        now = time.time()
        A = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp),
                    cfg.params, now, None, cfg.default_engine)
        s = store.sessions[T]
        ta = s.start
        tb = min(to or now, s.end(True, now))
        return summarize(A, ta, tb)
    finally:
        shutil.rmtree(tmp)


def show(name, S):
    r, b = S["recall"], S["bust"]
    print(f"== {name}: recall L {r['L']['p'] * 100:5.1f} R {r['R']['p'] * 100:5.1f}   "
          f"bust L {b['L']['p'] * 100:4.1f} ({b['L']['k']}/{b['L']['n']}) R {b['R']['p'] * 100:4.1f}"
          f"   n {r['n']}  L spots {S['counts']['spots_L']}")
    print("   L of R by R dB: " + "  ".join(
        f"{x['lo'] if x['lo'] is not None else '':>2}-{x['hi'] if x['hi'] is not None else '':<2} "
        f"{x['LofR']['p'] * 100:4.1f}" for x in S["snr"]["bins"]))
    print("   recall by band: " + "  ".join(
        f"{x['band']} {x['L']['p'] * 100:4.1f}/{x['R']['p'] * 100:4.1f}" for x in S["bands"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("session")
    ap.add_argument("rowsdirs", nargs="+")
    ap.add_argument("-c", "--config", default=os.path.join(HERE, "compare.ini"))
    ap.add_argument("--to", help="end of the scored range (UTC or Unix s)")
    ap.add_argument("--json")
    a = ap.parse_args()
    cfg = Config(a.config)
    to = None
    if a.to:
        try:
            to = float(a.to)
        except ValueError:
            to = calendar.timegm(time.strptime(a.to.rstrip("Z"), "%Y-%m-%dT%H:%M:%S"))
    out = {}
    for d in a.rowsdirs:
        S = score(cfg, a.session, d, to)
        show(d, S)
        out[d] = S
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(out, fh)


if __name__ == "__main__":
    main()
