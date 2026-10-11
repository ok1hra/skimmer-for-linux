#!/usr/bin/env python3
"""gate.py — re-run only the post-decode path of a variant from its taps.

  gate.py SOURCE NEW [--env K=V ...] [--bin PATH] [--jobs 8] [-- TAP-REPLAY OPTS]

SOURCE is a run.py variant whose parts carry decode taps (.spots.tap).
Every part's tap goes through skimmer-tap-replay (extractor, station
table, feed gate — the same code skimmer-replay runs) and the spots land
in NEW exactly as run.py would have put them: same [lo, hi) windows, one
file per band-hour. Seconds instead of minutes: for gate and extractor
experiments the DSP need not run again.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(TREE, "skimmer-compare", "logs", "cqww")
FLATPAK = ["flatpak", "run", "--user", "--share=network", "--filesystem=home",
           "--filesystem=/tmp"]


def one(job):
    tap, out, lo, hi, binp, env, extra = job
    cmd = FLATPAK + [f"--env={kv}" for kv in env] + [
        f"--command={binp}", "org.gnome.Sdk//50", "--spots", out + ".raw"] + extra + [tap]
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    keep = []
    import datetime
    with open(out + ".raw") as fh:
        for line in fh:
            ts = datetime.datetime.strptime(line.split()[0], "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=datetime.timezone.utc).timestamp()
            if lo <= ts < hi:
                keep.append(line)
    with open(out, "w") as fh:
        fh.writelines(keep)
    return out


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source")
    ap.add_argument("new")
    ap.add_argument("--env", action="append", default=[], metavar="K=V")
    ap.add_argument("--bin", default=os.path.join(TREE, "builddir-cqww", "skimmer-tap-replay"))
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args(argv)
    src = os.path.join(OUT, a.source, "parts")
    dst = os.path.join(OUT, a.new)
    os.makedirs(os.path.join(dst, "parts"), exist_ok=True)
    with open(os.path.join(dst, "run.json"), "w") as fh:
        json.dump({"from": a.source, "env": a.env, "bin": a.bin, "extra": extra}, fh)
    jobs, groups = [], {}
    for meta in sorted(glob.glob(os.path.join(src, "*.meta.json"))):
        base = os.path.basename(meta)[:-len(".meta.json")]
        tap = os.path.join(src, base + ".spots.tap")
        if not os.path.exists(tap):
            sys.exit(f"{tap}: missing — the source variant ran without taps")
        with open(meta) as fh:
            m = json.load(fh)
        with open(os.path.join(dst, "parts", base + ".meta.json"), "w") as fh:
            json.dump(m, fh)
        out = os.path.join(dst, "parts", base + ".spots")
        jobs.append((tap, out, m["lo"], m["hi"], a.bin, a.env, extra))
        groups.setdefault(base.rsplit("-", 1)[0], []).append(out)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(one, jobs))
    for key, parts in groups.items():
        lines = []
        for p in parts:
            with open(p) as fh:
                lines += fh.readlines()
        lines.sort()
        with open(os.path.join(dst, f"{key}.spots"), "w") as fh:
            fh.writelines(lines)
    print(f"{a.new}: {len(jobs)} tap replays -> {dst}", file=sys.stderr)


if __name__ == "__main__":
    main()
