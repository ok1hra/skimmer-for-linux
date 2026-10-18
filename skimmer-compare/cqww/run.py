#!/usr/bin/env python3
"""run.py — replay CQ WW 2012 band-hours through skimmer-replay, in parallel.

  run.py VARIANT 40m:2012-11-25T02 80m:2012-11-25T02 ... [--jobs 7]
         [--bin PATH] [--env K=V ...] [--whole] [--warm 180]

Each band-hour is its four 15-minute files. Every file runs as its own
replay, streamed over ssh from the recording host, preceded by the last
--warm seconds of the file before it so the decoder, the extractor and
the station table are warm when the scored part starts; spots stamped
before the file's own start are dropped. --whole streams the hour as one
continuous replay instead (the reference the split is checked against).

Spots land in skimmer-compare/logs/cqww/VARIANT/<band>-<hour>.spots
(one file per band-hour, the parts concatenated in time order).
"""
import argparse
import concurrent.futures as cf
import datetime
import json
import os
import shlex
import struct
import subprocess
import sys
import tempfile
import time

HOST = ["ssh", "-p", "10648", "-o", "BatchMode=yes", "dan@192.168.1.200"]
ROOT = "/mnt/sdd/2012_cqww_cw"
HDR = 218                              # every recording's header is this long
RATE = 192000
SKEW = "0.46"                          # Q sampled early — see replay_main.c
CACHE = os.path.expanduser("~/.cache/skimmer-cqww2012")
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(TREE, "skimmer-compare", "logs", "cqww")
FLATPAK = ["flatpak", "run", "--user", "--share=network", "--filesystem=home",
           "--filesystem=/tmp"]


def systime(b):
    y, mo, _, d, h, mi, s, ms = struct.unpack("<8H", b)
    return datetime.datetime(y, mo, d, h, mi, s, tzinfo=datetime.timezone.utc).timestamp() + ms / 1000


def index():
    """[{path, size, t0, t1, centre}] for every recording, cached."""
    path = os.path.join(CACHE, "files.json")
    if os.path.exists(path):
        with open(path) as fh:
            return json.load(fh)
    cmd = (f"cd {ROOT}; for f in */*.wav; do printf '%s %s ' $f $(stat -c %s $f); "
           f"head -c 96 $f | od -An -tx1 -v | tr -d ' \\n'; echo; done")
    out = subprocess.run(HOST + [cmd], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        f, size, hx = line.split()
        b = bytes.fromhex(hx)
        rows.append({"path": f, "size": int(size), "t0": systime(b[0x2e:0x3e]),
                     "t1": systime(b[0x3e:0x4e]),
                     "centre": struct.unpack("<I", b[0x4e:0x52])[0]})
    os.makedirs(CACHE, exist_ok=True)
    with open(path, "w") as fh:
        json.dump(rows, fh)
    return rows


def wav_header(frames, t0, centre):
    """A header in the recordings' own layout: fmt (18) + auxi (164) + data."""
    dt = datetime.datetime.fromtimestamp(t0, datetime.timezone.utc)
    st = struct.pack("<8H", dt.year, dt.month, (dt.weekday() + 1) % 7, dt.day,
                     dt.hour, dt.minute, dt.second, int(round(dt.microsecond / 1000)) % 1000)
    auxi = st + st + struct.pack("<I", centre) + b"\0" * (164 - 36)
    fmt = struct.pack("<HHIIHHH", 1, 2, RATE, RATE * 4, 4, 16, 0)
    data = frames * 4
    body = (b"WAVE" + b"fmt " + struct.pack("<I", 18) + fmt +
            b"auxi" + struct.pack("<I", 164) + auxi +
            b"data" + struct.pack("<I", data))
    hdr = b"RIFF" + struct.pack("<I", len(body) + data) + body
    assert len(hdr) == HDR, len(hdr)
    return hdr


def band_hour_files(files, band, hour_t):
    fs = sorted((f for f in files if f["path"].startswith(band + "/")), key=lambda f: f["t0"])
    cur = [i for i, f in enumerate(fs) if hour_t - 60 <= f["t0"] < hour_t + 3600 - 60]
    return fs, cur


def replay(job):
    """Run one replay; return the path of its spots (filtered to [lo, hi))."""
    remote, hdr, lo, hi, out, bin_path, env, log = job
    with tempfile.NamedTemporaryFile(delete=False, suffix=".hdr") as fh:
        fh.write(hdr)
        hpath = fh.name
    envs = [f"--env=SKIM_REPLAY_IQ_SKEW={SKEW}", f"--env=SKIM_REPLAY_SPOTS={out}.raw",
            "--env=SKIM_DLOG_LANE=1", f"--env=SKIM_REPLAY_TAP={out}.tap"]
    envs += [f"--env={kv}" for kv in env]
    fp = " ".join(shlex.quote(x) for x in FLATPAK + envs +
                  [f"--command={bin_path}", "org.gnome.Sdk//50", "-"])
    ssh = " ".join(shlex.quote(x) for x in HOST + [remote])
    sh = f"( cat {shlex.quote(hpath)}; {ssh} ) | {fp} > {shlex.quote(log)} 2>/dev/null"
    t = time.time()
    for attempt in range(3):
        # a flatpak launch now and then fails among parallel starts (an
        # empty log, no spots file): start the part again
        subprocess.run(["bash", "-c", sh], check=False)
        if os.path.exists(out + ".raw"):
            break
        print(f"  {os.path.basename(out)}: no output, retrying", file=sys.stderr)
    os.unlink(hpath)
    keep = []
    with open(out + ".raw") as fh:
        for line in fh:
            ts = datetime.datetime.strptime(line.split()[0], "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=datetime.timezone.utc).timestamp()
            if lo <= ts < hi:
                keep.append(line)
    with open(out, "w") as fh:
        fh.writelines(keep)
    return out, time.time() - t


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("variant")
    ap.add_argument("bandhours", nargs="+", metavar="BAND:YYYY-MM-DDTHH")
    ap.add_argument("--jobs", type=int, default=7)
    ap.add_argument("--bin", default=os.path.join(TREE, "builddir-cqww", "skimmer-replay"))
    ap.add_argument("--env", action="append", default=[], metavar="K=V")
    ap.add_argument("--whole", action="store_true")
    ap.add_argument("--warm", type=float, default=180.0)
    a = ap.parse_args()

    files = index()
    vdir = os.path.join(OUT, a.variant)
    os.makedirs(os.path.join(vdir, "parts"), exist_ok=True)
    with open(os.path.join(vdir, "run.json"), "w") as fh:
        json.dump({"bin": a.bin, "env": a.env, "whole": a.whole, "warm": a.warm,
                   "bandhours": a.bandhours, "at": time.time()}, fh)
    jobs, groups = [], {}
    for bh in a.bandhours:
        band, hs = bh.split(":")
        ht = datetime.datetime.strptime(hs, "%Y-%m-%dT%H").replace(
            tzinfo=datetime.timezone.utc).timestamp()
        fs, cur = band_hour_files(files, band, ht)
        if not cur:
            sys.exit(f"{bh}: no recording")
        key = f"{band}-{hs}"
        groups[key] = []
        segs = [cur] if a.whole else [[i] for i in cur]
        for seg in segs:
            first, prev = fs[seg[0]], fs[seg[0] - 1] if seg[0] > 0 else None
            warm_b = 0
            if prev and a.warm > 0 and abs(prev["t1"] - first["t0"]) < 1.0:
                warm_b = min(int(a.warm * RATE) * 4, prev["size"] - HDR)
            frames = (warm_b + sum(fs[i]["size"] - HDR for i in seg)) // 4
            t0 = first["t0"] - warm_b / 4 / RATE
            cmd = f"cd {ROOT} && "
            if warm_b:
                cmd += f"tail -c {warm_b} {prev['path']}; "
            cmd += "; ".join(f"tail -c +{HDR + 1} {fs[i]['path']}" for i in seg)
            lo = ht if a.whole or seg[0] == cur[0] else first["t0"]
            hi = min(fs[seg[-1]]["t1"], ht + 3600)
            name = os.path.join(vdir, "parts", f"{key}-{int(first['t0']) % 3600 // 60:02d}")
            with open(name + ".meta.json", "w") as fh:     # breakdown.py: UTC
                json.dump({"t0": t0, "lo": lo, "hi": hi}, fh)  # of decode-log 0
            jobs.append((cmd, wav_header(frames, t0, first["centre"]), lo, hi,
                         name + ".spots", a.bin, a.env, name + ".out"))
            groups[key].append(name + ".spots")
    t = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for out, dt in ex.map(replay, jobs):
            print(f"  {os.path.basename(out)} {dt:.0f} s", file=sys.stderr)
    for key, parts in groups.items():
        lines = []
        for p in parts:
            with open(p) as fh:
                lines += fh.readlines()
        lines.sort()
        with open(os.path.join(vdir, f"{key}.spots"), "w") as fh:
            fh.writelines(lines)
    print(f"{a.variant}: {len(jobs)} replays in {time.time() - t:.0f} s -> {vdir}", file=sys.stderr)


if __name__ == "__main__":
    main()
