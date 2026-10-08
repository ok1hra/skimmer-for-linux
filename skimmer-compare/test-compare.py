#!/usr/bin/env python3
"""test-compare.py — offline gate for skimcmp + compare-web.py, no feeds.

Writes a synthetic recording whose right answer is known in advance and
checks that the comparison finds exactly that:

  session A (12:00–14:00 UTC, stopped cleanly)
    DL1AAA  L and R CQ, RBN confirms          → match, R − L latency +60 s,
                                                 SNR offset R − L = +5 dB; events
                                                 sit on R's SNR scale
    OK1BBB  L only, RBN confirms              → only-L, real
    G4CCC   R only, in MASTER.SCP, no RBN     → only-R, real (SCP fallback)
    EA3G / EA3GEH  same spot, RBN knows EA3GEH → bust pair, L busted
    SP5DDD  L CQ, R heard it only as DE       → nonCQ (neither catch nor miss)
    ON4HHH  L only, RBN spot by OK1HRA itself → only-L unverified (own spotter
                                                 never confirms)
    RA1EEE  R on 17 m, outside the L windows   → ignored
    7089.8  L outside the common window       → ignored
    HB9FFF  R while the L feed was down       → ignored (not common time)
    F5GGG   R while L reported 5 % packet loss → ignored (unhealthy)
  session B (15:00–16:00)
    six R CQ spots on 20 m, L silent there for 30 min → suspicious silence;
    one L line written in two halves across polls → parsed exactly once
  decoders: session A's status snapshots name no decoder (→ the configured
    default cw-v2), session B's say deepcw → two separate comparisons
  current session: a recorder still running, and a newer one that was
    started and stopped beside it within a second → the running one is
    "the session", live, not the empty newer one
  band plan: L adds 17 m mid-session (restart, its status snapshots list
    the bands) → an R station on 17 m before that is ignored, one after it
    is only-R; both bands are shown; an earlier session with no status
    snapshots takes the nearest plan (40 m only), not the config's 17 m
  episodes split differently: L spots YU1AAA every 10 min for an hour (one
    episode), R at the start and 45 min later (two) → one match and one
    covered match, no only-R; mirrored for S5BBB (R long, L twice); and an
    L/R pair whose episode medians sit 4 kHz apart but whose spots meet is
    paired by the spots
  dead feed: L's feed up and its status healthy, but no L spot for 45 min
    while R spots 12 stations → that time is not compared, R's 12 are no
    only-R and stay out of the histogram; session B's 30 min L silence
    beside only 6 R spots stays compared

  ./test-compare.py            exit 0 = everything passed
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.arbiter import RbnIndex, load_scp  # noqa: E402
from skimcmp.config import Config               # noqa: E402
from skimcmp.logs import Store                  # noqa: E402
from skimcmp.match import analyze, compared_time, curve_at, engine_summary, snr_curve  # noqa: E402
from skimcmp.stats import summarize             # noqa: E402
from skimcmp.windows import loose_bands         # noqa: E402

fails = 0
DAY = "2026-10-01T"


def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails += 1


def ts(hms):
    return DAY + hms + "Z"


def dx(spotter, f, call, snr, wpm, cm, mode="CW    "):
    return "DX de %-9s %7.1f  %-12s %s%3d dB  %2d WPM  %-6s 1200Z" % (
        spotter + ":", f, call, mode, snr, wpm, cm)


def L(hms, f, call, snr=20, wpm=22):
    return "%s\t%s\n" % (ts(hms), dx("OK1HRA-#", f, call, snr, wpm, "CQ"))


def R(hms, f, call, snr=20, wpm=22, cm="CQ"):
    return "%s\t%s\n" % (ts(hms), dx("OK1HRA-#", f, call, snr, wpm, cm, mode=""))


def B(hms, spotter, f, call, snr=15):
    return "%s\t%s\n" % (ts(hms), dx(spotter, f, call, snr, 22, "CQ"))


def mark(hms, msg):
    return "#%s\t%s\n" % (ts(hms), msg)


def build(root):
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n20m=14040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nG4CCC\nEA3GEH\nHB9FFF\nF5GGG\nDL1AAA\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[local]\nname = Local test\ndefault_engine = cw-v2\n"
                 "[engines]\ncw-v2 = decoder v2 | classical\ndeepcw = DeepCW | neural\n"
                 "[remote]\nwindows = 7000-7091 14000-14091 18068-18159\n"
                 "[rbn]\nexclude = OK1HRA\n[web]\nlisten = 127.0.0.1\nport = 0\n")

    A = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7300"),
           L("12:05:00", 7010.0, "DL1AAA", snr=20), L("12:10:00", 7020.0, "OK1BBB"),
           L("12:15:00", 7010.0, "DL1AAA", snr=20),
           L("12:30:30", 7030.1, "EA3G"), L("12:40:00", 7040.0, "SP5DDD"),
           L("12:50:00", 7070.0, "ON4HHH"), L("12:55:00", 7089.8, "OUTSIDE"),
           mark("13:00:00", "disconnected — retry in 10 s"),
           mark("13:10:00", "connected to 127.0.0.1:7300")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"),
           R("12:06:00", 7010.0, "DL1AAA", snr=25), R("12:20:00", 14010.0, "G4CCC"),
           R("12:30:00", 7030.0, "EA3GEH"), R("12:41:00", 7040.0, "SP5DDD", cm="DE"),
           R("12:45:00", 18070.0, "RA1EEE"), R("13:05:00", 7050.0, "HB9FFF"),
           R("13:28:00", 7060.0, "F5GGG")]
    rbn = [mark("12:00:00", "connected to telnet.reversebeacon.net:7000"),
           B("12:07:00", "DK0XX-#", 7010.1, "DL1AAA"), B("12:11:00", "DK0XX-#", 7020.0, "OK1BBB"),
           B("12:31:00", "DK0XX-#", 7030.0, "EA3GEH"), B("12:50:30", "OK1HRA-#", 7070.0, "ON4HHH")]
    status = []
    for m in range(5, 120, 5):
        t = "%02d:%02d:00" % (12 + m // 60, m % 60)
        lost = 5.0 if t == "13:30:00" else 0.0
        status.append("%s\t%s\n" % (ts(t), json.dumps({"streaming": True, "lost_pct": lost})))
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), A),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", rbn), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, A)), "w") as fh:
            fh.writelines(lines)
    with open(os.path.join(logs, "status-%s.jsonl" % A), "w") as fh:
        fh.writelines(status)

    Bs = "20261001-150000"
    rem = [mark("15:00:00", "connected to 192.168.1.201:7301")] + [
        R("15:%02d:00" % (10 + 3 * i), 14010.0 + 5 * i, "ZZ%dZZ" % i) for i in range(6)]
    with open(os.path.join(logs, "local-%s.log" % Bs), "w") as fh:
        fh.write(mark("15:00:00", "connected to 127.0.0.1:7300"))
    with open(os.path.join(logs, "remote-%s.log" % Bs), "w") as fh:
        fh.writelines(rem)
    with open(os.path.join(logs, "status-%s.jsonl" % Bs), "w") as fh:
        for m in range(5, 60, 5):
            fh.write("%s\t%s\n" % (ts("15:%02d:00" % m),
                                    json.dumps({"streaming": True, "lost_pct": 0, "engine": "deepcw"})))
    with open(os.path.join(logs, "events-%s.log" % Bs), "w") as fh:
        fh.write("%s recording\n%s stopped — write check OK\n" % (ts("15:00:00"), ts("16:00:00")))
    return root, logs, A, Bs


def total_of(iv):
    return sum(b - a for a, b in iv)


def epoch(hms):
    import calendar
    return calendar.timegm(time.strptime(DAY + hms, "%Y-%m-%dT%H:%M:%S"))


def core(root, logs, A_T, B_T):
    print("=== core — known answers")
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    # session B: one L line arrives in two halves, across polls
    line = L("15:30:00", 14020.0, "DL9ZZZ")
    with open(os.path.join(logs, "local-%s.log" % B_T), "a") as fh:
        fh.write(line[:30])
    store.poll()
    with open(os.path.join(logs, "local-%s.log" % B_T), "a") as fh:
        fh.write(line[30:])
    store.poll()
    check(sum(sp.call == "DL9ZZZ" for sp in store.L) == 1,
          "a line split across polls is parsed exactly once")
    check(store.rbn.n == 3 and store.rbn.own == 1,
          "RBN: 3 spots indexed, our own OK1HRA spot dropped (%d, %d)" % (store.rbn.n, store.rbn.own))

    scp = load_scp(cfg.scp)
    An = analyze(store, cfg.local_windows, cfg.remote_windows, scp, cfg.params, epoch("23:00:00"))
    S = summarize(An, epoch("12:00:00"), epoch("14:00:00") + 1)
    ev = {(e["cat"], e["call"]): e for e in An.events if e["t0"] < epoch("14:00:00")}
    cats = S["cats"]
    want = {"match": 1, "only-L": 1, "only-R": 1, "bust": 1, "nonCQ": 1, "only-L unverified": 1}
    check(cats == want, "categories %s" % cats)
    m = ev.get(("match", "DL1AAA"))
    check(m is not None and m["v"] == "rbn" and m["lat"] == 60,
          "DL1AAA: match, confirmed by RBN, R − L = +60 s")
    check(("only-L", "OK1BBB") in ev and ev[("only-L", "OK1BBB")]["inU"]
          and ev[("only-L", "OK1BBB")]["v"] == "rbn", "OK1BBB: only L, real (RBN)")
    check(("only-R", "G4CCC") in ev and ev[("only-R", "G4CCC")]["v"] == "scp",
          "G4CCC: only R, real by MASTER.SCP fallback")
    b = ev.get(("bust", "EA3GEH"))
    check(b is not None and b["bL"] and not b["bR"] and b["cR"] and not b["cL"]
          and b["L"]["call"] == "EA3G", "EA3G vs EA3GEH: bust pair, L busted, R caught it")
    n = ev.get(("nonCQ", "SP5DDD"))
    check(n is not None and not n["inU"] and "DE" in n["note"], "SP5DDD: R heard it only as DE")
    o = ev.get(("only-L", "ON4HHH"))
    check(o is not None and not o["inU"] and o["bL"], "ON4HHH: our own RBN spot does not confirm it")
    calls = {e["call"] for e in An.events}
    check(not calls & {"RA1EEE", "OUTSIDE", "HB9FFF", "F5GGG"},
          "17 m, out-of-window, feed-down and unhealthy spots are all ignored")
    r = S["recall"]
    check(r["n"] == 4 and r["L"]["k"] == 2 and r["R"]["k"] == 3 and r["b"] == 1 and r["c"] == 2,
          "recall: 4 real stations, L 2, R 3, discordant 1 / 2")
    check(abs(r["p"] - 1.0) < 1e-9, "McNemar exact p = 1.0 for 1 vs 2 (%.4f)" % r["p"])
    bu = S["bust"]
    check(bu["L"]["k"] == 2 and bu["L"]["n"] == 5 and bu["R"]["k"] == 0 and bu["R"]["n"] == 3,
          "bust rate: L 2 of 5 episodes, R 0 of 3")
    check(S["snr"]["offset"] == 5 and S["snr"]["offset_n"] == 1, "SNR offset R − L = +5 dB from 1 pair")
    check(m is not None and m["snr"] == 25, "DL1AAA: a matched event sits at R's SNR (25 dB)")
    lofr = [(b["LofR"]["k"], b["LofR"]["n"]) for b in S["snr"]["bins"] if b["n"]]
    check(sum(k for k, _ in lofr) == 1 and sum(n for _, n in lofr) == 3,
          "L of R's catches: 1 of 3 (%s)" % lofr)
    check(ev[("only-L", "OK1BBB")]["snr"] == 25,
          "OK1BBB: L-only 20 dB mapped to R's scale by the +5 dB curve")
    pts = snr_curve([6, 7, 8, 9, 9, 31, 32, 33, 30, 34, 20],
                    [-8, -7, -9, -8, -8, 8, 7, 9, 8, 8, 0], 0)
    check([(p[1], p[2]) for p in pts] == [(-8, 5), (8, 5)],
          "SNR curve: one point per bin with ≥ 5 pairs (%s)" % pts)
    check(curve_at(pts, 5) == -8 and curve_at(pts, 40) == 8
          and abs(curve_at(pts, (pts[0][0] + pts[1][0]) / 2)) < 1e-9,
          "SNR curve: flat beyond the ends, linear between points")
    check(S["time"]["common_s"] == 7200 - 600 - 300,
          "common time 6300 s: 10 min feed outage and 5 min packet loss removed (%d)"
          % S["time"]["common_s"])
    sil = [s for s in An.silences if s["side"] == "L" and s["band"] == "20m"
           and s["t0"] >= epoch("15:00:00")]
    check(len(sil) == 1 and sil[0]["other"] == 6, "session B: L silent on 20 m while R spotted 6")
    check(not any(s["t0"] < epoch("14:00:00") for s in An.silences), "session A: no silence flagged")
    check(An.dead == [], "no dead feed: session B's L silence has only 6 R spots beside it")
    h40 = next(h for h in S["hist"] if h["band"] == "40m")
    check(h40["Rall"][7050 - h40["lo"]] == 0 and h40["Rall"][7060 - h40["lo"]] == 0,
          "histogram: HB9FFF (L feed down) and F5GGG (unhealthy) left out")
    check([s["T"] for s in An.sessions] == [A_T, B_T] and not any(s["live"] for s in An.sessions),
          "two sessions, both stopped")

    print("=== decoders — one comparison each")
    E = engine_summary(compared_time(store, cfg.params, epoch("23:00:00"), cfg.default_engine)[3])
    check({k: v["s"] for k, v in E.items()} == {"cw-v2": 6300, "deepcw": 3600},
          "compared time: cw-v2 6300 s (default, no decoder named), deepcw 3600 s (%s)"
          % {k: v["s"] for k, v in E.items()})
    V2 = analyze(store, cfg.local_windows, cfg.remote_windows, scp, cfg.params,
                 epoch("23:00:00"), "cw-v2", cfg.default_engine)
    DC = analyze(store, cfg.local_windows, cfg.remote_windows, scp, cfg.params,
                 epoch("23:00:00"), "deepcw", cfg.default_engine)
    check(all(e["t0"] < epoch("14:00:00") for e in V2.events) and len(V2.events) == 6,
          "cw-v2 comparison holds session A only (%d events)" % len(V2.events))
    check(all(e["t0"] >= epoch("15:00:00") for e in DC.events) and
          any(e["call"] == "DL9ZZZ" for e in DC.events),
          "deepcw comparison holds session B only")
    S2 = summarize(DC, 0, epoch("23:00:00"))
    check(total_of(S2["timeline"]["other"]) == 6300,
          "deepcw timeline marks the cw-v2 time as 'other decoder' (%d s)"
          % total_of(S2["timeline"]["other"]))


def web(root):
    print("=== web — the server answers")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    p = subprocess.Popen([os.path.join(HERE, "compare-web.py"), "-c", os.path.join(root, "compare.ini"),
                          "--port", str(port)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        base = "http://127.0.0.1:%d" % port
        for _ in range(50):
            try:
                urllib.request.urlopen(base + "/", timeout=1)
                break
            except OSError:
                time.sleep(0.1)
        get = lambda path: urllib.request.urlopen(base + path, timeout=5)
        S0 = json.load(get("/api/summary?range=all"))
        check(S0["engine"] == "deepcw" and [o["key"] for o in S0["overview"]] == ["deepcw", "cw-v2"],
              "default comparison: the newest decoder (deepcw); the overview lists both")
        check(S0["names"]["local"] == "Local test" and S0["engines"][1]["label"] == "decoder v2",
              "names and decoder labels come from compare.ini")
        S = json.load(get("/api/summary?range=session:20261001-120000&engine=cw-v2"))
        check(S["recall"]["n"] == 4 and S["range"]["label"] == "session 20261001-120000",
              "/api/summary for session A, cw-v2: 4 real stations")
        S2 = json.load(get("/api/summary?range=session:20261001-120000&engine=cw-v2&df_khz=0.05"))
        check(S2["params"]["df_khz"] == 1.5, "an out-of-range parameter falls back to the default")
        S3 = json.load(get("/api/summary?range=session:20261001-120000&engine=cw-v2&bust_edit=0"))
        check(S3["cats"].get("bust", 0) == 1,
              "bust_edit=0 still pairs EA3G/EA3GEH (prefix), parameters reach the matcher")
        E = json.load(get("/api/events?range=all&engine=cw-v2"))["events"]
        check(len(E) == 6, "/api/events lists every event of the comparison (%d)" % len(E))
        e = next(e for e in E if e["cat"] == "bust")
        raw = json.load(get("/api/raw?band=40m&f=%s&t0=%d&t1=%d&calls=EA3G,EA3GEH"
                            % (e["f"], e["t0"], e["t1"])))
        check(any(r[2] for r in raw["L"]) and any(r[2] for r in raw["R"])
              and any(x[3] == "EA3GEH" for x in raw["rbn"]),
              "/api/raw: the L, R and RBN lines of the bust pair")
        html = get("/").read().decode()
        check("<title>Skimmer compare</title>" in html, "the page is served")
        try:
            get("/../compare.ini")
            check(False, "path traversal is refused")
        except urllib.error.HTTPError as err:
            check(err.code == 404, "path traversal is refused")
        es = get("/events")
        check(es.readline().startswith(b"data: "), "/events streams the data version")
        es.close()
    finally:
        p.terminate()
        p.wait(5)


def current_session():
    print("=== current session — a short run beside a running recorder")
    root = tempfile.mkdtemp(prefix="compare-cur-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    now = time.time()
    stamp = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
    name = lambda t: time.strftime("%Y%m%d-%H%M%S", time.gmtime(t))
    run, short = name(now - 600), name(now - 120)
    with open(os.path.join(logs, "events-%s.log" % run), "w") as fh:
        fh.write("%s recording as OK1HRA since %s UTC\n" % (stamp(now - 600), run))
        fh.write("%s local up: 1 spots (+1)\n" % stamp(now - 30))
    with open(os.path.join(logs, "local-%s.log" % run), "w") as fh:
        fh.write("#%s\tconnected to 127.0.0.1:7302\n" % stamp(now - 600))
    with open(os.path.join(logs, "events-%s.log" % short), "w") as fh:
        fh.write("%s recording as OK1HRA since %s UTC\n" % (stamp(now - 120), short))
        fh.write("%s stopped — write check OK\n" % stamp(now - 119))
    store = Store(logs, lambda: RbnIndex(loose_bands([("40m", 7000, 7091)], 2.0), set()))
    store.poll()
    cur = store.current(time.time())
    check(cur is not None and cur.T == run,
          "Store.current() is the running recorder, not the newer stopped one")
    info = compared_time(store, {"lost_pct": 1.0}, time.time(), "cw-v2")[0]
    live = [i["T"] for i in info if i["live"]]
    check(live == [run], "only the running session is live (%s)" % live)


def split_episodes():
    print("=== split episodes — one long episode spans two of the other side")
    root = tempfile.mkdtemp(prefix="compare-split-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nYU1AAA\nS5BBB\nHA1CCC\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091\n[rbn]\nexclude = OK1HRA\n")
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302")]
    loc += [L("12:%02d:00" % m, 7010.0, "YU1AAA") for m in range(5, 56, 10)]
    loc += [L("13:10:00", 7020.0, "S5BBB"), L("13:55:00", 7020.0, "S5BBB")]
    loc += [L("14:10:00", 7031.0, "HA1CCC"), L("14:12:00", 7031.0, "HA1CCC"),
            L("14:14:00", 7031.0, "HA1CCC")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"),
           R("12:06:00", 7010.0, "YU1AAA"), R("12:50:00", 7010.0, "YU1AAA")]
    rem += [R("13:%02d:00" % m, 7020.0, "S5BBB") for m in range(5, 56, 10)]
    rem += [R("14:11:00", 7031.0, "HA1CCC"), R("14:13:00", 7035.0, "HA1CCC"),
            R("14:15:00", 7035.0, "HA1CCC")]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("15:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    by = {}
    for e in An.events:
        by.setdefault(e["call"], []).append(e)
    for call, side in (("YU1AAA", "R"), ("S5BBB", "L")):
        g = by.get(call, [])
        cov = [e for e in g if e["cat"] == "match" and (e["L"] is None or e["R"] is None)]
        check(len(g) == 2 and all(e["cat"] == "match" and e["cL"] and e["cR"] for e in g)
              and len(cov) == 1 and cov[0][side] is not None and "counted there" in cov[0]["note"],
              "%s: one match + one covered %s episode, no only-%s (%s)"
              % (call, side, side, [(e["cat"], e["L"] is not None, e["R"] is not None) for e in g]))
    g = by.get("HA1CCC", [])
    check(len(g) == 1 and g[0]["cat"] == "match" and g[0]["L"] and g[0]["R"],
          "HA1CCC: medians 4 kHz apart, spots meet → paired (%s)" % [e["cat"] for e in g])
    S = summarize(An, 0, epoch("23:00:00"))
    check(S["recall"]["n"] == 5 and S["recall"]["L"]["k"] == 5 and S["recall"]["R"]["k"] == 5,
          "recall: 5 real events, each caught by both")
    check(S["bust"]["L"]["n"] == 4 and S["bust"]["R"]["n"] == 4,
          "episodes counted once each: L 4, R 4 (%d, %d)" % (S["bust"]["L"]["n"], S["bust"]["R"]["n"]))


def dead_feed():
    print("=== dead feed — one side silent on every band is not compared")
    root = tempfile.mkdtemp(prefix="compare-dead-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nDL1AAA\nOK1BBB\n" + "".join("ZZ%dZZ\n" % i for i in range(12)))
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091\n[rbn]\nexclude = OK1HRA\n")
    T = "20261001-100000"
    loc = [mark("10:00:00", "connected to 127.0.0.1:7302"),
           L("10:02:00", 7010.0, "DL1AAA"), L("10:05:00", 7020.0, "OK1BBB"),
           L("10:50:00", 7010.0, "DL1AAA")]
    rem = [mark("10:00:00", "connected to 192.168.1.201:7301"),
           R("10:03:00", 7010.0, "DL1AAA"), R("10:04:00", 7020.0, "OK1BBB")] + [
           R("10:%02d:00" % (10 + 3 * i), 7030.0 + 3 * i, "ZZ%dZZ" % i) for i in range(12)] + [
           R("10:52:00", 7010.0, "DL1AAA")]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("10:00:00"), T),
              "%s stopped — write check OK\n" % ts("11:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", []), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    with open(os.path.join(logs, "status-%s.jsonl" % T), "w") as fh:
        for m in range(5, 60, 5):
            fh.write("%s\t%s\n" % (ts("10:%02d:00" % m),
                                    json.dumps({"streaming": True, "lost_pct": 0})))
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    check(An.dead == [(epoch("10:05:00"), epoch("10:50:00"), "L")],
          "L dead 10:05–10:50 (%s)" % An.dead)
    S = summarize(An, epoch("10:00:00"), epoch("11:00:00") + 1)
    check(S["time"]["common_s"] == 3600 - 45 * 60,
          "common time 900 s: the dead 45 min removed (%d)" % S["time"]["common_s"])
    check(not any(e["call"].startswith("ZZ") for e in An.events),
          "R's 12 stations heard while L was dead are not only-R")
    check(S["recall"]["n"] == 3 and S["recall"]["L"]["k"] == 3,
          "recall: DL1AAA twice (48 min apart) and OK1BBB, all caught by L")
    h = S["hist"][0]
    check(sum(h["R"]) == 3 and sum(h["Rall"]) == 3,
          "histogram: R's spots in the dead time left out (%d)" % sum(h["R"]))
    check([(d["side"], d["t0"]) for d in S["timeline"]["dead"]] == [("L", epoch("10:05:00"))]
          and not any(a < epoch("10:50:00") and b > epoch("10:05:00")
                      for a, b in S["timeline"]["outages"]),
          "timeline: the dead time listed as dead, not as an outage")
    P = dict(cfg.params, dead_other=13)
    An2 = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), P,
                  epoch("23:00:00"))
    check(An2.dead == [] and sum(e["call"].startswith("ZZ") for e in An2.events) == 12,
          "dead_other=13: 12 R spots are not enough — compared, 12 only-R")


def band_plan():
    print("=== band plan — a band added later is not missed before it")
    root = tempfile.mkdtemp(prefix="compare-plan-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n17m=18110000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nUA9AAA\nUA9BBB\nUA9CCC\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091 18068-18159\n[rbn]\nexclude = OK1HRA\n")
    E = "20261001-100000"                        # no status file at all
    for kind, lines in (("local", [mark("10:00:00", "connected to 127.0.0.1:7302")]),
                        ("remote", [mark("10:00:00", "connected to 192.168.1.201:7301"),
                                    R("10:30:00", 18080.0, "UA9CCC")]),
                        ("rbn", [mark("10:00:00", "connected to telnet.reversebeacon.net:7000"),
                                 B("10:31:00", "DK0XX-#", 18080.0, "UA9CCC")]),
                        ("events", ["%s recording\n%s stopped — write check OK\n"
                                    % (ts("10:00:00"), ts("11:00:00"))])):
        with open(os.path.join(logs, "%s-%s.log" % (kind, E)), "w") as fh:
            fh.writelines(lines)
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302"),
           mark("13:00:00", "disconnected — retry in 10 s"),
           mark("13:00:10", "connected to 127.0.0.1:7302")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"),
           R("12:30:00", 18070.0, "UA9AAA"), R("13:30:00", 18075.0, "UA9BBB")]
    rbn = [mark("12:00:00", "connected to telnet.reversebeacon.net:7000"),
           B("12:31:00", "DK0XX-#", 18070.0, "UA9AAA"), B("13:31:00", "DK0XX-#", 18075.0, "UA9BBB")]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", rbn), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    plan = lambda *b: [{"band": n, "centre_hz": c} for n, c in b]
    with open(os.path.join(logs, "status-%s.jsonl" % T), "w") as fh:
        for h, bands in ((12, plan(("40m", 7040000))),
                         (13, plan(("40m", 7040000), ("17m", 18110000)))):
            for m in range(5, 60, 5):
                fh.write("%s\t%s\n" % (ts("%02d:%02d:00" % (h, m)), json.dumps(
                    {"streaming": True, "lost_pct": 0, "rate": 96000, "bands": bands})))
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    ev = {e["call"]: e for e in An.events}
    check("UA9CCC" not in ev,
          "a session that never named its bands takes the nearest plan, not the config's 17 m")
    check("UA9AAA" not in ev, "17 m before L listened there: R's UA9AAA is ignored")
    check("UA9BBB" in ev and ev["UA9BBB"]["cat"] == "only-R" and ev["UA9BBB"]["inU"],
          "17 m after L added it: R's UA9BBB is only-R, real")
    check([n for n, _, _ in An.win.common] == ["40m", "17m"],
          "the bands shown: 40 m and 17 m (%s)" % [n for n, _, _ in An.win.common])


if __name__ == "__main__":
    root = tempfile.mkdtemp(prefix="compare-test-")
    root, logs, A, Bs = build(root)
    core(root, logs, A, Bs)
    web(root)
    current_session()
    band_plan()
    split_episodes()
    dead_feed()
    print("  (files in %s)" % root)
    print("\n%s (%d failure%s)" % ("FAIL" if fails else "PASS", fails, "" if fails == 1 else "s"))
    sys.exit(1 if fails else 0)
