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
  remote plan: R spotted only its CW segments (40 m with a hole at
    7035–7045) until 13:00, then the whole band inside its receiver → L's
    calls in the hole and above 7070 count only after 13:00; one below
    7000 never does (R's windows stop at the band edge)
  paused bands: L's status says 20 m paused (the thermal guard, CW and
    RTTY) 12:30–13:00 → R's 20 m stations from the last snapshot without
    the pause to the first one after are not compared, 40 m is; RTTY: RBN's
    20 m spot in the pause is not counted
  call filter: L filters RU/BY calls from 13:00 (compare.ini [local
    filter]) → R's UA3BEF before 13:00 is only-R, its RA1AAA after is not
    compared (one filtered R spot), OK1RRR after still only-R; a headless.ini
    filtering with another regex is warned about
  dead feed: L's feed up and its status healthy, but no L spot for 45 min
    while R spots 12 stations → that time is not compared, R's 12 are no
    only-R and stay out of the histogram; session B's 30 min L silence
    beside only 6 R spots stays compared

  ./test-compare.py            exit 0 = everything passed
"""
import calendar
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.arbiter import RbnIndex, load_scp  # noqa: E402
from skimcmp.config import Config               # noqa: E402
from skimcmp.logs import Store, catalog, parse_spot  # noqa: E402
from skimcmp.match import analyze, compared_time, curve_at, engine_summary, snr_curve  # noqa: E402
from skimcmp.rtty import analyze_rtty           # noqa: E402
from skimcmp.stats import summarize             # noqa: E402
from skimcmp.windows import Windows, loose_bands  # noqa: E402

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
    bd = S["breakdown"]
    # found_scp: found stations only MASTER.SCP vouches for — R's only-R G4CCC
    check(bd["L"] == {"found": 2, "found_scp": 0, "lost": 1, "missed": 1, "unverified": 1,
                      "unresolved": 0,
                      "nonCQ": 1, "episodes": 5,
                      "calls": {"found": 2, "lost": 1, "missed": 1, "unverified": 2,
                                "unresolved": 0, "nonCQ": 1, "own": 5}}
          and bd["R"] == {"found": 3, "found_scp": 1, "lost": 0, "missed": 1, "unverified": 0,
                          "unresolved": 0, "nonCQ": 0, "episodes": 3,
                          "calls": {"found": 3, "lost": 0, "missed": 1, "unverified": 0,
                                    "unresolved": 0, "nonCQ": 0, "own": 3}}
          and bd["calls_real"] == 4 and bd["n"] == 4,
          "breakdown: L's EA3G bust costs recall, its ON4HHH bust only the bust rate;"
          " R's G4CCC is found by MASTER.SCP alone %s" % bd)
    check(all(bd[X]["found"] + bd[X]["lost"] + bd[X]["missed"] == r["n"]
              and bd[X]["lost"] + bd[X]["unverified"] == bu[X]["k"] for X in "LR"),
          "breakdown: found + lost + missed = recall n, lost + unverified = bust k")
    hr = S["hourly"]
    check([h["t"] for h in hr] == [epoch("12:00:00"), epoch("13:00:00")]
          and all(sum(h[X][k] for h in hr) == bd[X][k] for X in "LR"
                  for k in ("found", "lost", "missed", "unverified", "unresolved", "nonCQ", "episodes"))
          and all(h["common_s"] > 0 for h in hr),
          "hourly: two compared hours whose episode counts add up to the whole range")
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


def web(root, A_T, B_T):
    print("=== web — the public page and nothing else")
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
        get = lambda path, **kw: urllib.request.urlopen(
            urllib.request.Request(base + path, **kw), timeout=5)

        def status(path, **kw):
            try:
                return get(path, **kw).status
            except urllib.error.HTTPError as err:
                return err.code

        D = None
        for _ in range(100):
            D = json.load(get("/data.json"))
            if D.get("state") == "ok":
                break
            time.sleep(0.1)
        check(D.get("state") == "ok", "data.json: the current session's numbers (%s)" % D.get("state"))
        check(D["names"]["decoder"] == "DeepCW" and D["session"]["stopped"],
              "the current session is the newest (%s, decoder DeepCW, stopped)" % B_T)
        check(set(D["recall"]) >= {"L", "W"} and "R" not in D["recall"]
              and all({"L", "W", "bustL", "bustW"} <= set(b) for b in D["snr"]["bins"]),
              "the sides are L and W; every SNR bin has recall and bust rate of both")
        check(D.get("warnings") == [], "a stopped session warns about no feed port (%s)"
              % D.get("warnings"))
        raw = get("/data.json").read().decode()
        check(not any(x in raw for x in ("ZZ0ZZ", "192.168", "127.0.0.1", root, "OK1HRA")),
              "data.json carries no call, address or path")
        check(get("/data.json?range=all&engine=cw-v2&df_khz=9").read().decode() == raw,
              "a query string changes nothing")
        check(D["session"]["T"] == B_T and D["counts"]["rbn_spots"] >= 0
              and {"bands", "downloads"} <= set(D) and "rbn_s" in D["time"],
              "data.json names the session, the RBN reference, the bands and the downloads")
        paths = {d["path"] for d in D["downloads"]}
        check(paths == {"download/events.csv", "download/spots-L.tsv", "download/spots-W.tsv",
                        "download/rbn.tsv.gz"}, "four downloads listed (%s)" % sorted(paths))
        r = get("/download/spots-W.tsv")
        body = r.read().decode()
        check("ZZ0ZZ" in body and "192.168" not in body and "connected" not in body
              and r.headers.get("Content-Disposition", "").startswith("attachment")
              and B_T in r.headers.get("Content-Disposition", ""),
              "spots-W.tsv: W's spot lines, no connection lines, an attachment named by the session")
        ev = get("/download/events.csv").read().decode().splitlines()
        check(ev[0].startswith("first_utc,") and len(ev) == 1 + D["counts"]["events"],
              "events.csv: a header and one row per compared episode (%d)" % (len(ev) - 1))
        import gzip as _gz
        rb = _gz.decompress(get("/download/rbn.tsv.gz").read()).decode()
        check(rb.startswith("# RBN spots"), "rbn.tsv.gz is gzip text")
        check(status("/download/../compare.ini") == 404 and status("/download/") == 404
              and status("/download/x.csv") == 404, "only the four downloads exist under /download/")
        r = get("/")
        html = r.read().decode()
        check("<title>Skimmer compare</title>" in html, "the page is served")
        csp = r.headers.get("Content-Security-Policy", "")
        check("default-src 'none'" in csp and "frame-ancestors 'none'" in csp
              and r.headers.get("X-Content-Type-Options") == "nosniff"
              and "Python" not in r.headers.get("Server", "Python"),
              "security headers: CSP, nosniff; the Server header names no Python")
        check(all(status(x) == 200 for x in ("/app.js", "/style.css", "/index.html")),
              "the page's files are served")
        check(all(status(x) == 404 for x in ("/api/summary", "/api/events", "/api/raw", "/events",
                                             "/../compare.ini", "/%2e%2e/compare.ini",
                                             "/web/app.js", "/compare-web.py", "/logs/")),
              "every other path is 404 (the old API, path traversal, the sources)")
        check(status("/", method="POST", data=b"x") == 501 and status("/", method="PUT", data=b"x") == 501,
              "POST and PUT are not served")
        check(status("/", method="HEAD") == 200, "HEAD is served")
        c = socket.create_connection(("127.0.0.1", port), timeout=5)
        c.sendall(b"\x16\x03\x01garbage\r\n\r\n")
        reply = c.recv(4096)
        c.close()
        check(b"400" in reply and b"Python" not in reply and b"<" not in reply,
              "a malformed request gets a bare 400, no HTML, no Python banner (%r)" % reply[:40])
        idle = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(8)]
        time.sleep(0.2)
        try:
            refused = status("/") != 200
        except OSError:
            refused = True
        for c in idle:
            c.close()
        time.sleep(0.2)
        check(refused and status("/") == 200,
              "8 idle connections from one address: a 9th is closed; once they go, it is served")
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


def resumed_session():
    print("=== resumed session — capture-spots.sh -C continues a stopped run")
    root = tempfile.mkdtemp(prefix="compare-res-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    now = time.time()
    stamp = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
    run = time.strftime("%Y%m%d-%H%M%S", time.gmtime(now - 600))
    with open(os.path.join(logs, "events-%s.log" % run), "w") as fh:
        fh.write("%s recording as OK1HRA since %s UTC\n" % (stamp(now - 600), run))
        fh.write("%s stopped — write check OK\n" % stamp(now - 300))
        fh.write("%s resumed session %s as OK1HRA — appending to its files\n" % (stamp(now - 299), run))
        fh.write("%s local up: 1 spots (+1)\n" % stamp(now - 30))
    with open(os.path.join(logs, "local-%s.log" % run), "w") as fh:
        fh.write("#%s\tconnected to 127.0.0.1:7302\n" % stamp(now - 600))
    store = Store(logs, lambda: RbnIndex(loose_bands([("40m", 7000, 7091)], 2.0), set()))
    store.poll()
    s = store.current(time.time())
    check(s is not None and s.T == run and s.stopped is None,
          "a resumed session is not stopped (stopped=%s)" % (s and s.stopped))
    cat = catalog(logs, time.time())
    check(len(cat) == 1 and cat[0]["live"] and cat[0]["stopped"] is None,
          "catalog() sees the resumed session live (%s)" % cat)


def feed_port_warning():
    print("=== feed port — the recorder reads another port than the local feed is on")
    root = tempfile.mkdtemp(prefix="compare-port-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    now = time.time()
    stamp = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
    run = time.strftime("%Y%m%d-%H%M%S", time.gmtime(now - 600))
    ev = os.path.join(logs, "events-%s.log" % run)
    with open(ev, "w") as fh:
        fh.write("%s recording as OK1HRA since %s UTC\n" % (stamp(now - 600), run))
        fh.write("%s local skimmer feed confirmed on port 7302\n" % stamp(now - 590))
    store = Store(logs, lambda: RbnIndex(loose_bands([("40m", 7000, 7091)], 2.0), set()))
    store.poll()
    s = store.sessions[run]
    check(s.port_warn is None, "a confirmed port: no warning")
    with open(ev, "a") as fh:
        fh.write("%s WARNING: the local skimmer serves its feed on port 7373, but the recorder"
                 " reads 127.0.0.1:7302 (-l)\n" % stamp(now - 300))
        fh.write("%s local up: 1 spots (+0)\n" % stamp(now - 240))
    store.poll()
    w = s.port_warn
    check(w == {"since": epoch_of(stamp(now - 300)), "served": 7373, "read": 7302},
          "a moved feed port: warned with both ports and the time (%s)" % w)
    with open(ev, "a") as fh:
        fh.write("%s WARNING: the local skimmer reports its telnet feed OFF (feed_port 0) —"
                 " headless.ini [feed] call= empty, or port= taken by another service (ss -tlnp)?\n"
                 % stamp(now - 200))
    store.poll()
    check(s.port_warn is not None and s.port_warn["served"] == 0, "the feed off: warned (%s)" % s.port_warn)
    with open(ev, "a") as fh:
        fh.write("%s stopped — write check OK\n" % stamp(now - 100))
        fh.write("%s resumed session %s as OK1HRA — appending to its files\n" % (stamp(now - 99), run))
    store.poll()
    check(s.port_warn is None, "a resumed recorder checks the port anew: the warning goes")
    with open(ev, "a") as fh:
        fh.write("%s WARNING: the local skimmer serves its feed on port 7373, but the recorder"
                 " reads 127.0.0.1:7302 (-l)\n" % stamp(now - 90))
        fh.write("%s local skimmer feed confirmed on port 7373\n" % stamp(now - 60))
    store.poll()
    check(s.port_warn is None, "the port confirmed again: the warning goes")
    with open(ev, "a") as fh:
        fh.write("%s WARNING: the local skimmer reports its telnet feed OFF (feed_port 0) —"
                 " headless.ini [feed] call= empty, or port= taken by another service (ss -tlnp)?\n"
                 % stamp(now - 50))
        fh.write("%s the local skimmer's feed moved to port 7400 — the recorder follows it"
                 " (was 127.0.0.1:7373)\n" % stamp(now - 40))
    store.poll()
    check(s.port_warn is None, "off, then the recorder follows the feed to a new port: no warning")
    with open(ev, "a") as fh:
        fh.write("%s local skimmer feed confirmed on port 7400\n" % stamp(now - 40))
    store.poll()
    check(s.port_warn is None, "… nor once it is confirmed there")


def epoch_of(z):
    return calendar.timegm(time.strptime(z, "%Y-%m-%dT%H:%M:%SZ"))


def rbn_overlap():
    print("=== RBN overlap — a short run beside the recorder logged the same RBN")
    root = tempfile.mkdtemp(prefix="compare-rbn-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(logs, "rbn-20261001-100000.log"), "w") as fh:
        fh.write(B("10:00:30", "DL1AAA-#", 7010.0, "UA9AAA"))
        fh.write(B("11:00:00", "DL1AAA-#", 7020.0, "UA9BBB"))
        fh.write(B("12:00:00", "DL1AAA-#", 7030.0, "UA9CCC"))
    with open(os.path.join(logs, "rbn-20261001-100020.log"), "w") as fh:
        fh.write(B("10:00:30", "DL1AAA-#", 7010.0, "UA9AAA"))
    store = Store(logs, lambda: RbnIndex(loose_bands([("40m", 7000, 7091)], 2.0), set()))
    store.poll()
    rbn = store.rbn
    check(rbn.last_t == epoch("12:00:00"),
          "last_t is the newest RBN spot, not the last one read (%s)" % rbn.last_t)
    check(rbn.n == 3 and rbn.late == 1, "the overlap's copy is dropped (n %d, late %d)"
          % (rbn.n, rbn.late))
    near = [c for _, _, _, c, _ in rbn.near("40m", epoch("10:59:00"), epoch("12:30:00"),
                                             7025.0, 10.0)]
    check(near == ["UA9BBB", "UA9CCC"], "near() bisects a sorted band (%s)" % near)


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


def remote_plan_test():
    print("=== remote plan — what R could spot, by time")
    w = Windows([("40m", 6992.0, 7088.0)], [(7000.0, 7035.0), (7045.0, 7070.0)], 0.5)
    check(w.band(7040.0) is None and w.band(7020.0) == "40m" and w.band(7050.0) == "40m",
          "a band in two remote parts: the hole is not compared")
    check(w.bands == [("40m", 7000.5, 7069.5)] and len(w.parts("40m")) == 2,
          "…shown once, as two parts (%s)" % w.bands)
    root = tempfile.mkdtemp(prefix="compare-rplan-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nOK1HOL\nOK1TOP\nOK1MID\nOK1NEW\nOK1LOW\nOK1RRR\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091\n"
                 "[remote windows]\n2026-09-01 = 7000-7035 7045-7070\n"
                 "2026-10-01T13:00 = 7000-7091\n[rbn]\nexclude = OK1HRA\n")
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302"),
           L("12:10:00", 7040.0, "OK1HOL"), L("12:20:00", 7080.0, "OK1TOP"),
           L("12:30:00", 7020.0, "OK1MID"), L("13:10:00", 7040.0, "OK1NEW"),
           L("13:20:00", 6995.0, "OK1LOW")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"), R("13:30:00", 7085.0, "OK1RRR")]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", []), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    cfg = Config(os.path.join(root, "compare.ini"))
    check(len(cfg.remote_windows) == 2, "two remote plans read (%d)" % len(cfg.remote_windows))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    ev = {e["call"]: e["cat"] for e in An.events}
    check("OK1HOL" not in ev and "OK1TOP" not in ev,
          "before 13:00: L in R's hole and above its segment is not compared")
    check(ev.get("OK1MID") == "only-L" and ev.get("OK1NEW") == "only-L",
          "inside R's segment, and the hole after 13:00: only-L")
    check("OK1LOW" not in ev, "below 7000 kHz: never compared")
    check(ev.get("OK1RRR") == "only-R", "R at 7085 after 13:00: only-R")
    check(An.win.bands == [("40m", 7000.5, 7087.5)],
          "the bands shown: R's newest windows (%s)" % An.win.bands)


def call_filter_test():
    print("=== call filter — a call L may not spot is no miss of L")
    root = tempfile.mkdtemp(prefix="compare-filter-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    rx = r"^(?!^(?:R[A-Z]?|U[A-I0]|E[UVW])[0-9]{1,3}[A-Z]{1,4}(?:/[A-Z0-9]+)?$).+"
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n[filter]\nregex=true\ncall=%s\n" % rx)
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nUA3BEF\nRA1AAA\nOK1RRR\nOK1LLL\n")
    ini = ("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
           "[remote]\nwindows = 7000-7091\n[local filter]\n2022-01-01 =\n"
           "2026-10-01T13:00 = %s\n[rbn]\nexclude = OK1HRA\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write(ini % rx)
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302"), L("13:40:00", 7030.0, "OK1LLL")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"),
           R("12:30:00", 7010.0, "UA3BEF"), R("13:20:00", 7020.0, "RA1AAA"),
           R("13:30:00", 7050.0, "OK1RRR")]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", []), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    cfg = Config(os.path.join(root, "compare.ini"))
    check(cfg.call_filter and not cfg.filter_warning, "the filter read, the same as headless.ini's")
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"), None, "cw-v2", cfg.call_filter)
    ev = {e["call"]: e["cat"] for e in An.events}
    check(ev.get("UA3BEF") == "only-R", "UA3BEF before the filter: only-R (%s)" % ev.get("UA3BEF"))
    check("RA1AAA" not in ev, "RA1AAA after the filter: not compared")
    check(ev.get("OK1RRR") == "only-R" and ev.get("OK1LLL") == "only-L",
          "OK1RRR / OK1LLL after it: only-R / only-L")
    S = summarize(An, epoch("12:00:00"), epoch("14:00:00"))
    check((S["counts"]["filtered_R"], S["counts"]["filtered_L"]) == (1, 0),
          "one R CQ spot left out, no L spot (%s/%s)"
          % (S["counts"]["filtered_R"], S["counts"]["filtered_L"]))
    Nf = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    check({e["call"] for e in Nf.events} >= {"RA1AAA"}, "without call_filter: unchanged, RA1AAA only-R")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write(ini % "^OK")
    check("filters calls with" in Config(os.path.join(root, "compare.ini")).filter_warning,
          "compare.ini ending with another regex than headless.ini's: warned")


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


def paused_bands():
    print("=== paused bands — a band the local skimmer paused on its own is not compared")
    root = tempfile.mkdtemp(prefix="compare-paused-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n20m=14040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nUA9AAA\nUA9BBB\nUA9CCC\nUA9DDD\nUA9EEE\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091 14000-14091\n[rbn]\nexclude = OK1HRA\n")
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"),
           R("12:27:00", 14020.0, "UA9CCC"),     # 20 m, between "on" 12:25 and "off" 12:30
           R("12:40:00", 14020.0, "UA9AAA"),     # 20 m paused (the thermal guard)
           R("12:40:00", 7020.0, "UA9DDD"),      # 40 m runs on
           R("13:30:00", 14030.0, "UA9BBB")]     # 20 m back
    rbn = [mark("12:00:00", "connected to telnet.reversebeacon.net:7000"),
           RT("12:41:00", "DK0XX-#", 14085.0, "UA9EEE"),    # RTTY on 20 m while paused
           RT("13:41:00", "DK0XX-#", 14085.0, "UA9EEE")]    # … and after
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", rbn), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    with open(os.path.join(logs, "status-%s.jsonl" % T), "w") as fh:
        for m in range(0, 120, 5):
            off = "thermal" if 30 <= m <= 60 else None
            bands = [{"band": "40m", "centre_hz": 7040000, "paused": None,
                      "rtty": {"paused": None}},
                     {"band": "20m", "centre_hz": 14040000, "paused": off,
                      "rtty": {"paused": off}}]
            fh.write("%s\t%s\n" % (ts("%02d:%02d:00" % (12 + m // 60, m % 60)), json.dumps(
                {"streaming": True, "lost_pct": 0, "rate": 96000, "bands": bands})))
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    ev = {e["call"]: e for e in An.events}
    check(An.off.get("20m") == [(epoch("12:25:00"), epoch("13:05:00"))] and "40m" not in An.off,
          "20 m off from the last snapshot without the pause to the first one after (%s)" % An.off)
    check("UA9AAA" not in ev and "UA9CCC" not in ev,
          "R's 20 m stations while L had 20 m paused (and at its edge) are no only-R")
    check("UA9DDD" in ev and ev["UA9DDD"]["cat"] == "only-R",
          "40 m at the same time is compared: R's UA9DDD is only-R")
    check("UA9BBB" in ev and ev["UA9BBB"]["cat"] == "only-R",
          "20 m back: R's UA9BBB is only-R")
    S = summarize(An, epoch("12:00:00"), epoch("14:00:00") + 1)
    off = {b["band"]: b["off_s"] for b in S["bands"]}
    check(off.get("20m") == 2400 and off.get("40m") == 0,
          "the band table: 20 m off 40 min, 40 m 0 (%s)" % off)
    X = analyze_rtty(store, load_scp(cfg.scp), cfg.params, epoch("23:00:00"))
    b = X["bands"].get("20m", {})
    check(b.get("rbn") == 1 and b.get("matched") == 0,
          "RTTY: RBN's UA9EEE on 20 m counts only after the pause (%s)" % b.get("rbn"))


def RT(hms, spotter, f, call, cm="CQ"):
    """An RTTY line: skimmer-headless and RBN name the mode, 45 BPS."""
    return "%s\tDX de %-9s %7.2f  %-12s RTTY  20 dB  45 BPS  %-6s 1200Z\n" % (
        ts(hms), spotter + ":", f, call, cm)


def rtty():
    print("=== RTTY — the local RTTY against RBN, kept out of the CW comparison")
    sp = parse_spot(0, "DX de OK1HRA-#:   7035.5  OK1RTY       RTTY  24 dB  45 BPS  CQ      1351Z")
    check(sp and sp.mode == "RTTY" and sp.call == "OK1RTY" and sp.wpm == 45 and sp.cm == "CQ",
          "skimmer-headless's RTTY line: mode RTTY, 45 Bd, CQ")
    sp = parse_spot(0, "DX de OK1HRA-#:   7035.5  OK1RTY         24 dB  45 BPS                1351Z")
    check(sp and sp.mode == "RTTY", "a line without the mode word: BPS = RTTY")
    sp = parse_spot(0, "DX de OK1HRA-#:   1826.1  RZ3DX          15 dB  29 WPM                1727Z")
    check(sp and sp.mode == "CW", "… WPM = CW (CW Skimmer Server)")
    root = tempfile.mkdtemp(prefix="compare-rtty-")
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    with open(os.path.join(root, "headless.ini"), "w") as fh:
        fh.write("[radio]\nrate=96000\n[bands]\n40m=7040000\n20m=14040000\n")
    with open(os.path.join(root, "master.scp"), "w") as fh:
        fh.write("# test\nLZ2BBB\nDL1CW\n")
    with open(os.path.join(root, "compare.ini"), "w") as fh:
        fh.write("[paths]\nlogs = logs\nheadless_ini = headless.ini\nscp = master.scp\n"
                 "[remote]\nwindows = 7000-7091 14000-14091\n[rbn]\nexclude = OK1HRA\n")
    T = "20261001-120000"
    loc = [mark("12:00:00", "connected to 127.0.0.1:7302"),
           L("12:05:00", 7010.0, "DL1CW"),
           RT("12:10:00", "OK1HRA-#", 7045.00, "JA1AAA"), RT("12:12:00", "OK1HRA-#", 7045.02, "JA1AAA"),
           RT("12:30:00", "OK1HRA-#", 7060.00, "LZ2BBB"), RT("12:40:00", "OK1HRA-#", 7066.00, "XX9XQ"),
           RT("12:42:00", "OK1HRA-#", 14080.00, "W1RTY"),
           mark("13:00:00", "disconnected — retry in 10 s"),
           mark("13:10:00", "connected to 127.0.0.1:7302")]
    rem = [mark("12:00:00", "connected to 192.168.1.201:7301"), R("12:06:00", 7010.0, "DL1CW")]
    rbn = [mark("12:00:00", "connected to telnet.reversebeacon.net:7000"),
           B("12:07:00", "DK0XX-#", 7010.0, "DL1CW")]
    rbn += [RT("12:11:%02d" % i, s, 7045.05, "JA1AAA") for i, s in enumerate(("DK0XX-#", "G4ZZZ-#", "SM5YY-#"))]
    rbn += [RT("12:20:00", "DK0XX-#", 7050.00, "VK3CCC"),        # L missed it, one spotter
            RT("12:21:00", "DK0XX-#", 7100.00, "PY2OUT"),        # outside the windows
            RT("13:05:00", "DK0XX-#", 7055.00, "ZL1DOWN"),       # while the L feed was down
            RT("12:43:00", "DK0XX-#", 14080.00, "W1RTY")]        # 20 m decodes no RTTY
    plan = [{"band": "40m", "centre_hz": 7040000, "rtty": {"engine": "rtty"}},
            {"band": "20m", "centre_hz": 14040000, "rtty": None}]
    status = ["%s\t%s\n" % (ts("%02d:%02d:00" % (12 + m // 60, m % 60)), json.dumps(
        {"streaming": True, "lost_pct": 0, "rate": 96000, "bands": plan}))
        for m in range(1, 120, 5)]
    events = ["%s recording as OK1HRA since %s UTC\n" % (ts("12:00:00"), T),
              "%s stopped — write check OK\n" % ts("14:00:00")]
    for kind, lines in (("local", loc), ("remote", rem), ("rbn", rbn), ("events", events)):
        with open(os.path.join(logs, "%s-%s.log" % (kind, T)), "w") as fh:
            fh.writelines(lines)
    with open(os.path.join(logs, "status-%s.jsonl" % T), "w") as fh:
        fh.writelines(status)
    cfg = Config(os.path.join(root, "compare.ini"))
    store = Store(cfg.logs, lambda: RbnIndex(loose_bands(cfg.local_windows, 2.0), cfg.exclude))
    store.poll()
    check([sp.call for sp in store.L] == ["DL1CW"] and len(store.Lr) == 5,
          "the local feed split: CW in L (1), RTTY in Lr (5)")
    An = analyze(store, cfg.local_windows, cfg.remote_windows, load_scp(cfg.scp), cfg.params,
                 epoch("23:00:00"))
    calls = {e["call"] for e in An.events}
    check(calls == {"DL1CW"}, "the CW comparison sees no RTTY call (%s)" % sorted(calls))
    X = analyze_rtty(store, load_scp(cfg.scp), cfg.params, epoch("23:00:00"))
    b = X["bands"].get("40m", {})
    check(abs(X["time_s"] - 110 * 60) < 61,
          "RTTY time: the L feed up with RBN, 12:00–14:00 less the 10 min outage (%d s)" % X["time_s"])
    check(list(X["bands"]) == ["40m"], "only 40 m decodes RTTY (%s)" % list(X["bands"]))
    check(b.get("rbn") == 2 and b.get("matched") == 1 and b.get("recall") == 0.5,
          "RBN's JA1AAA spotted, VK3CCC missed; outside the window / while down ignored: recall 50 %")
    check(b.get("rbn_strong") == 1 and b.get("recall_strong") == 1.0,
          "JA1AAA heard by 3 RBN skimmers: strong, caught")
    check((b.get("local"), b.get("local_rbn"), b.get("local_scp"), b.get("local_none")) == (3, 1, 1, 1),
          "L's three: JA1AAA confirmed by RBN, LZ2BBB by MASTER.SCP, XX9XQ by nobody")
    check([x["call"] for x in X["busts"]] == ["XX9XQ"] and abs(b.get("bust") - 1 / 3) < 1e-9,
          "bust: XX9XQ, 1 of 3")


if __name__ == "__main__":
    root = tempfile.mkdtemp(prefix="compare-test-")
    root, logs, A, Bs = build(root)
    core(root, logs, A, Bs)
    web(root, A, Bs)
    current_session()
    resumed_session()
    feed_port_warning()
    band_plan()
    rbn_overlap()
    split_episodes()
    remote_plan_test()
    call_filter_test()
    dead_feed()
    rtty()
    paused_bands()
    print("  (files in %s)" % root)
    print("\n%s (%d failure%s)" % ("FAIL" if fails else "PASS", fails, "" if fails == 1 else "s"))
    sys.exit(1 if fails else 0)
