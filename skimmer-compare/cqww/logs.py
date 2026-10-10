#!/usr/bin/env python3
"""logs.py — the CQ WW CW 2012 public logs as the referee for spot scoring.

  logs.py fetch [--jobs 4]     download every Cabrillo log once
  logs.py build                parse them into truth.json

Logs land in ~/.cache/skimmer-cqww2012/logs/ (an interrupted fetch resumes).
truth.json holds:

  senders   every call that submitted a log (it was on the air)
  worked    call -> number of DISTINCT logs that worked it
  runs      sender -> [[unix_minute, band, kHz, worked_call], ...] — where
            and when the sender itself was heard (its own QSO lines)
  rbn       call -> number of distinct RBN skimmers that spotted it on the
            contest days (rbn/2012112{4,5}.zip from data.reversebeacon.net;
            OL5Q — the reference skimmer itself — does not count)

A call counts as real (score.py) when it sent a log, at least two
independent logs worked it (one log alone may hold its own bust), or at
least two independent RBN skimmers spotted it.
"""
import argparse
import concurrent.futures as cf
import datetime
import json
import os
import re
import sys
import urllib.request
import zipfile

BASE = "https://cqww.com/publiclogs/2012cw/"
CACHE = os.path.expanduser("~/.cache/skimmer-cqww2012")
LOGS = os.path.join(CACHE, "logs")
TRUTH = os.path.join(CACHE, "truth.json")

QSO_RE = re.compile(r"^QSO:\s+(\d+)\s+\S+\s+(\d{4}-\d\d-\d\d)\s+(\d{4})\s+(\S+)\s+\S+\s+\S+\s+(\S+)")


def band_of(khz):
    for lo, hi, b in ((1800, 2000, "160m"), (3500, 4000, "80m"), (7000, 7300, "40m"),
                      (14000, 14350, "20m"), (21000, 21450, "15m"), (28000, 29700, "10m")):
        if lo <= khz <= hi:
            return b
    return None


def get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "skimmer-for-linux cqww referee"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def fetch(jobs):
    os.makedirs(LOGS, exist_ok=True)
    index = get(BASE, timeout=300).decode("utf-8", "replace")
    names = sorted(set(re.findall(r"href='([^'/]+\.log)'", index)))
    todo = [n for n in names if not os.path.exists(os.path.join(LOGS, n))]
    print(f"{len(names)} logs listed, {len(todo)} to fetch", file=sys.stderr)

    def one(n):
        data = get(BASE + n)
        tmp = os.path.join(LOGS, n + ".part")
        with open(tmp, "wb") as fh:
            fh.write(data)
        os.replace(tmp, os.path.join(LOGS, n))
        return n

    done = 0
    with cf.ThreadPoolExecutor(jobs) as ex:
        for fut in cf.as_completed([ex.submit(one, n) for n in todo]):
            try:
                fut.result()
            except Exception as e:  # noqa: BLE001 — report and go on
                print(f"fetch failed: {e}", file=sys.stderr)
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{len(todo)}", file=sys.stderr)


def build():
    senders, worked, runs = set(), {}, {}
    for n in sorted(os.listdir(LOGS)):
        if not n.endswith(".log"):
            continue
        me, rows, seen = None, [], set()
        with open(os.path.join(LOGS, n), errors="replace") as fh:
            for line in fh:
                if line.startswith("CALLSIGN:"):
                    me = line.split(":", 1)[1].strip().upper()
                    continue
                m = QSO_RE.match(line)
                if not m:
                    continue
                khz = int(m.group(1))
                band = band_of(khz)
                if not band:
                    continue
                try:
                    t = datetime.datetime.strptime(m.group(2) + m.group(3), "%Y-%m-%d%H%M")
                except ValueError:           # a hand-edited log's broken time
                    continue
                minute = int(t.replace(tzinfo=datetime.timezone.utc).timestamp() // 60)
                mycall, call = m.group(4).upper(), m.group(5).upper()
                me = me or mycall
                rows.append([minute, band, khz, call])
                seen.add(call)
        if not me:
            continue
        senders.add(me)
        runs[me] = rows
        for c in seen:
            worked[c] = worked.get(c, 0) + 1
    spotters = {}
    for d in ("20121124", "20121125"):
        zp = os.path.join(CACHE, "rbn", d + ".zip")
        if not os.path.exists(zp):
            print(f"no {zp} — RBN evidence left out", file=sys.stderr)
            continue
        with zipfile.ZipFile(zp) as z, z.open(d + ".csv") as fh:
            for raw in fh:
                f = raw.decode("utf-8", "replace").split(",")
                if len(f) < 13 or f[12].strip() != "CW" or f[0].startswith("OL5Q"):
                    continue
                spotters.setdefault(f[5].upper(), set()).add(f[0].upper().split("-")[0])
    rbn = {c: len(v) for c, v in spotters.items()}
    with open(TRUTH, "w") as fh:
        json.dump({"senders": sorted(senders), "worked": worked, "runs": runs,
                   "rbn": rbn}, fh)
    print(f"{len(senders)} senders, {len(worked)} worked calls, "
          f"{sum(1 for v in worked.values() if v >= 2)} worked by >= 2 logs, "
          f"{sum(1 for v in rbn.values() if v >= 2)} RBN calls with >= 2 skimmers -> {TRUTH}",
          file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["fetch", "build"])
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    fetch(a.jobs) if a.cmd == "fetch" else build()


if __name__ == "__main__":
    main()
