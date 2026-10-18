#!/usr/bin/env python3
"""corpus.py — a decoder-only benchmark on real channels of CQ WW 2012.

  corpus.py dump  BAND:YYYY-MM-DDTHH [--offset-khz 0.2] [--jobs 4]
  corpus.py bench BAND:YYYY-MM-DDTHH NAME [--env K=V ...] [--bbdec PATH]

dump replays the band-hour once (run.py's parts and warm-up) with
SKIM_DUMP_CHAN set to the two channels nearest every station CW Skimmer
heard there with a placed (TRUE) call, so the channelizer's own output of
those channels lands in ~/.cache/skimmer-cqww2012/chan/<band-hour>/<part>/.

bench runs the CW backend ALONE (bbdec) on those channel files — seconds,
not the ten minutes of a full replay — and reports the COPY RATE: the share
of R's TRUE units whose call appears as a token in the text of one of its
two channels during the part where R spotted it, by R's SNR, plus the
number of distinct valid-looking tokens within edit distance 1..2 of such a
call (misreads). A decoder change is screened here and confirmed with
run.py + score.py.
"""
import argparse
import collections
import datetime
import glob
import json
import os
import re
import subprocess
import sys

import run
import score

CHAN = os.path.join(score.CACHE, "chan")
BW = 125.0


def units_for(band, hour):
    ev = score.evaluate(band, [hour], {})
    R, lab = ev["R"], ev["lab"]["R"]
    return [(k, R[k]) for k, v in lab.items() if v == "TRUE"]


def chan_of(khz, centre_hz, nchan):
    k = round((khz * 1000 - centre_hz) / BW)
    frac = (khz * 1000 - centre_hz) / BW - k
    k2 = k + (1 if frac > 0 else -1)
    return [k % nchan, k2 % nchan]


def dump(a):
    band, hour = a.bandhour.split(":")
    files = run.index()
    ht = datetime.datetime.strptime(hour, "%Y-%m-%dT%H").replace(
        tzinfo=datetime.timezone.utc).timestamp()
    fs, cur = run.band_hour_files(files, band, ht)
    nchan = int(run.RATE / BW)
    us = units_for(band, hour)
    centre = fs[cur[0]]["centre"]
    chans = set()
    for k, ss in us:
        f = sorted(s.khz for s in ss)[len(ss) // 2] + a.offset_khz
        chans.update(chan_of(f, centre, nchan))
    root = os.path.join(CHAN, f"{band}-{hour}")
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "meta.json"), "w") as fh:
        json.dump({"centre": centre, "nchan": nchan, "offset_khz": a.offset_khz,
                   "chans": sorted(chans)}, fh)
    print(f"{len(us)} TRUE units, {len(chans)} channels", file=sys.stderr)
    # one run.py job per part, each with its own dump dir
    jobs = []
    for i in cur:
        first, prev = fs[i], fs[i - 1]
        warm_b = min(int(180 * run.RATE) * 4, prev["size"] - run.HDR)
        frames = (warm_b + first["size"] - run.HDR) // 4
        t0 = first["t0"] - warm_b / 4 / run.RATE
        part = f"{int(first['t0']) % 3600 // 60:02d}"
        d = os.path.join(root, part)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "meta.json"), "w") as fh:
            json.dump({"t0": t0, "lo": max(first["t0"], ht), "hi": first["t1"]}, fh)
        cmd = (f"cd {run.ROOT} && tail -c {warm_b} {prev['path']}; "
               f"tail -c +{run.HDR + 1} {first['path']}")
        env = [f"SKIM_DUMP_CHAN={','.join(str(c) for c in sorted(chans))}",
               f"SKIM_DUMP_DIR={d}"]
        jobs.append((cmd, run.wav_header(frames, t0, centre), 0, 0,
                     os.path.join(d, "spots"), a.bin, env, os.path.join(d, "out")))
    import concurrent.futures as cf
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for out, dt in ex.map(run.replay, jobs):
            print(f"  {out} {dt:.0f} s", file=sys.stderr)


TOK = re.compile(r"[A-Z0-9/]+")


def bench(a):
    band, hour = a.bandhour.split(":")
    root = os.path.join(CHAN, f"{band}-{hour}")
    with open(os.path.join(root, "meta.json")) as fh:
        meta = json.load(fh)
    us = units_for(band, hour)
    calls_all = {k[0] for k, _ in us}
    found = 0
    tot = 0
    by_snr = collections.defaultdict(lambda: [0, 0])
    misreads = set()
    for pdir in sorted(glob.glob(os.path.join(root, "[0-9][0-9]"))):
        with open(os.path.join(pdir, "meta.json")) as fh:
            pm = json.load(fh)
        files = sorted(glob.glob(os.path.join(pdir, "ch*.cf32")))
        cmd = run.FLATPAK + [f"--env={kv}" for kv in a.env] + [
            f"--command={a.bbdec}", "org.gnome.Sdk//50"] + files
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        text = collections.defaultdict(list)    # chan -> [(utc, txt)]
        cur = None
        for line in out.splitlines():
            if line.startswith("== "):
                cur = int(os.path.basename(line[3:])[2:-5])
                continue
            t, _, rest = line.partition(" |")
            text[cur].append((pm["t0"] + float(t), rest[:-1]))
        for k, ss in us:
            mine = [s for s in ss if pm["lo"] <= s.t < pm["hi"]]
            if not mine:
                continue
            f = sorted(s.khz for s in ss)[len(ss) // 2] + meta["offset_khz"]
            chs = chan_of(f, meta["centre"], meta["nchan"])
            t_lo, t_hi = min(s.t for s in mine) - 120, max(s.t for s in mine) + 60
            toks = set()
            for c in chs:
                s = "".join(x for t, x in text.get(c, []) if t_lo <= t <= t_hi)
                toks.update(TOK.findall(s.replace("\xb7", " ")))
            ok = k[0] in toks
            tot += 1
            found += ok
            b = min(max(s.snr for s in ss) // 10 * 10, 30)
            by_snr[b][0] += ok
            by_snr[b][1] += 1
            for w in toks:
                if w != k[0] and w not in calls_all and len(w) >= 3 and score.edit(w, k[0]) <= 2:
                    misreads.add(w)
    bins = " ".join(f"{b:>2}+:{100 * v[0] / v[1]:5.1f}%({v[1]})" for b, v in sorted(by_snr.items()))
    print(f"{a.name:14} copy {100 * found / max(tot, 1):5.1f}% ({found}/{tot})  misread tokens "
          f"{len(misreads):4d}   {bins}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=["dump", "bench"])
    ap.add_argument("bandhour")
    ap.add_argument("name", nargs="?", default="bench")
    ap.add_argument("--offset-khz", type=float, default=0.2)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--bin", default=os.path.join(run.TREE, "builddir-cqww", "skimmer-replay"))
    ap.add_argument("--bbdec", default=os.path.join(run.TREE, "builddir-cqww", "skimmer-cw-bench"))
    ap.add_argument("--env", action="append", default=[], metavar="K=V")
    a = ap.parse_args()
    dump(a) if a.cmd == "dump" else bench(a)


if __name__ == "__main__":
    main()
