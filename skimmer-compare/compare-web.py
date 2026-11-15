#!/usr/bin/env python3
"""compare-web.py — live comparison of two CW skimmers in a browser.

Reads what capture-spots.sh records (logs/: local, remote and RBN feeds,
events, local status), follows the running session, and serves one page
with the comparison: recall + McNemar, bust rate, latency, sensitivity by
SNR/WPM, per band, over time, and the list of every difference.

  compare-web.py [-c compare.ini] [--listen ADDR] [--port N]

Read-only: it never writes into logs/. Python 3 standard library only.

  GET /                   the page (web/)
  GET /api/summary?…      statistics for a range and matching parameters
  GET /api/events?…       every compared event in the range
  GET /api/raw?…          raw L/R/RBN lines around one event
  GET /events             Server-Sent Events: "the data changed"

Range: range=session|session:<T>|1h|6h|24h|today|custom|all (&from=&to=,
ISO UTC or Unix seconds; default: the current session). Parameters: any
name from skimcmp/config.py.

Only the sessions the range needs are read (all of logs/ was 2.7 GB of
text: minutes to start and 1.5 GB of memory). A range that needs other
sessions starts loading them in the background; until they are in, the API
answers {"loading": {...}} and the page shows how far the load has come.
"""
import argparse
import calendar
import json
import math
import os
import sys
import threading
import time
import traceback
import urllib.parse
from bisect import bisect_left, bisect_right
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.arbiter import RbnIndex, load_scp          # noqa: E402
from skimcmp.logs import Cancelled, Store, catalog       # noqa: E402
from skimcmp.config import PARAMS, Config, clamp_params  # noqa: E402
from skimcmp.match import analyze, compared_time, engine_summary  # noqa: E402
from skimcmp.rtty import analyze_rtty                     # noqa: E402
from skimcmp.stats import overview, summarize            # noqa: E402
from skimcmp.windows import loose_bands                  # noqa: E402

WEB = os.path.join(HERE, "web")
POLL_S = 2            # how often the logs are re-read
MIN_RECOMPUTE_S = 5   # new data recomputes at most this often
LIVE_RECOMPUTE_S = 30 # a live session's common time grows even with no spots
CATALOG_S = 2         # the session catalog is re-read at most this often
RBN_MARGIN_KHZ = 2.0  # RBN spots kept this far outside the local windows
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
         ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"}


def parse_time(s):
    if s is None or s == "":
        return None
    try:
        return float(s)
    except ValueError:
        pass
    s = s.strip().rstrip("Z").replace(" ", "T")
    for fmt in ("%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"):
        try:
            return calendar.timegm(time.strptime(s, fmt))
        except ValueError:
            pass
    return None


def clean(o):
    """JSON cannot carry inf/nan; tuples become lists."""
    if isinstance(o, float):
        return None if math.isnan(o) or math.isinf(o) else round(o, 4)
    if isinstance(o, dict):
        return {k: clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    return o


class App:
    def __init__(self, cfg):
        self.cfg = cfg
        bands = loose_bands(cfg.local_windows, RBN_MARGIN_KHZ)
        self.rbn_factory = lambda: RbnIndex(bands, cfg.exclude)
        self.scp = load_scp(cfg.scp)
        self.cache = {}
        self.rcache = {}
        self.clock = threading.Lock()
        # the loaded sessions: self.store holds exactly self.sel; gen counts
        # the swaps (analysis caches and the SSE version follow it)
        self.lock = threading.Lock()
        self.store, self.sel, self.gen = None, None, 0
        self.loader = None       # {"sel", "store", "cancel", "t0", "error"}
        self._cat = (0.0, [])

    def catalog(self):
        now = time.time()
        t, cat = self._cat
        if now - t >= CATALOG_S:
            cat = catalog(self.cfg.logs, now)
            self._cat = (now, cat)
        return cat

    def snapshot(self):
        with self.lock:
            return self.store, self.gen

    def version(self):
        st, gen = self.snapshot()
        return "%d.%d" % (gen, st.version if st else 0)

    def want(self, sel):
        """True when exactly the sessions sel are loaded; else start (or keep)
        loading them in the background — a different load still running is
        cancelled — and return False."""
        with self.lock:
            L = self.loader
            if L is not None and L["sel"] == sel and L["error"] is None:
                return False
            if L is not None:
                L["cancel"].set()
                self.loader = None
            if self.store is not None and self.sel == sel:
                return True
            L = self.loader = {"sel": sel, "cancel": threading.Event(), "t0": time.time(),
                               "error": None, "store": None}
        threading.Thread(target=self._load, args=(L,), daemon=True).start()
        return False

    def _load(self, L):
        st = Store(self.cfg.logs, self.rbn_factory, only=L["sel"], cancel=L["cancel"])
        L["store"] = st
        try:
            st.poll()
        except Cancelled:
            return
        except Exception as e:
            traceback.print_exc()
            L["error"] = str(e)
            return
        with self.lock:
            if self.loader is not L:
                return
            st.cancel = None
            self.store, self.sel, self.loader = st, L["sel"], None
            self.gen += 1
        with self.clock:
            self.cache.clear()
            self.rcache.clear()
        print("compare-web: loaded %s in %.0f s (%d L, %d R, %d RBN spots)" % (
            ", ".join(sorted(L["sel"])) or "no session", time.time() - L["t0"],
            len(st.L), len(st.R), st.rbn.n), flush=True)

    def loading(self):
        """How far the running load has come, or None."""
        with self.lock:
            L = self.loader
        if L is None:
            return None
        done, total = L["store"].progress() if L["store"] else (0, 0)
        want = sum(s["bytes"] for s in self.catalog() if s["T"] in L["sel"])
        return {"sessions": sorted(L["sel"]), "read": done, "total": max(total, want),
                "since": L["t0"], "error": L["error"]}

    def poller(self):
        while True:
            time.sleep(POLL_S)
            st = self.snapshot()[0]
            if st is None:
                continue
            try:
                st.poll()
            except Exception:
                traceback.print_exc()

    def needed(self, q, now):
        """The sessions the range in q needs (frozenset of <T>)."""
        cat = self.catalog()
        r = q.get("range", "session")
        if r == "all":
            return frozenset(s["T"] for s in cat)
        if r == "session" or r.startswith("session:"):
            T = r.partition(":")[2]
            if T and any(s["T"] == T for s in cat):
                return frozenset([T])
            return frozenset(s["T"] for s in cat if s["current"])
        hours = {"1h": 1, "6h": 6, "24h": 24}
        ta, tb = (cat[0]["start"] if cat else now), now
        if r in hours:
            ta = now - hours[r] * 3600
        elif r == "today":
            ta = now - now % 86400
        elif r == "custom":
            a, b = parse_time(q.get("from")), parse_time(q.get("to"))
            ta = a if a is not None else ta
            tb = b if b is not None else tb
        else:
            return frozenset(s["T"] for s in cat if s["current"])
        return frozenset(s["T"] for s in cat
                         if s["start"] <= tb and (s["live"] and now or s["end"]) >= ta)

    def sessions_list(self, A):
        """Every session for the page: the catalog, with the analysis's
        numbers for the loaded ones."""
        info = {s["T"]: s for s in (A.sessions if A else [])}
        out = []
        for c in self.catalog():
            a = info.get(c["T"])
            out.append({"T": c["T"], "start": c["start"],
                        "end": a["end"] if a else c["end"],
                        "live": a["live"] if a else c["live"], "current": c["current"],
                        "loaded": a is not None, "bytes": c["bytes"],
                        "common_s": a["common_s"] if a else None,
                        "has_rbn": "rbn" in c["files"], "engine": c["engine"]})
        return out

    def params(self, q):
        return clamp_params(q, self.cfg.params)

    def engines(self, st, P):
        """{engine: {"s", "last"}} over the loaded data, and the default
        choice: the decoder of the newest compared time."""
        with st.lock:
            segs = compared_time(st, P, time.time(), self.cfg.default_engine)[3]
        E = engine_summary(segs)
        default = max(E, key=lambda e: E[e]["last"]) if E else self.cfg.default_engine
        return E, default

    def engine(self, st, q, P):
        E, default = self.engines(st, P)
        e = q.get("engine")
        return (e if e in E else default), E

    def analysis(self, st, gen, P, engine):
        key = (gen, engine) + tuple(sorted(P.items()))
        with self.clock:
            now = time.time()
            A = self.cache.get(key)
            live = A is not None and any(s["live"] for s in A.sessions)
            if (A is None
                    or (A.version != st.version and now - A.computed >= MIN_RECOMPUTE_S)
                    or (live and now - A.computed >= LIVE_RECOMPUTE_S)):
                A = analyze(st, self.cfg.local_windows, self.cfg.remote_windows,
                            self.scp, P, now, engine, self.cfg.default_engine)
                self.cache[key] = A
                while len(self.cache) > 8:
                    self.cache.pop(min(self.cache, key=lambda k: self.cache[k].computed))
            return A

    def rtty(self, st, gen, P, ta, tb):
        """RTTY against RBN over [ta, tb] (rtty.py), recomputed like the CW
        analysis: on new data at most every MIN_RECOMPUTE_S."""
        key = (gen, ta, tb) + tuple(sorted(P.items()))
        with self.clock:
            now = time.time()
            hit = self.rcache.get(key)
            if hit is None or (hit[0] != st.version and now - hit[1] >= MIN_RECOMPUTE_S):
                with st.lock:
                    hit = (st.version, now, analyze_rtty(st, self.scp, P, now, ta, tb))
                self.rcache[key] = hit
                while len(self.rcache) > 8:
                    self.rcache.pop(min(self.rcache, key=lambda k: self.rcache[k][1]))
            return hit[2]

    def range(self, q, A):
        now = A.computed
        r = q.get("range", "session")
        first = A.spans[0][0] if A.spans else now
        ta, tb, label = first, now, "all data"
        hours = {"1h": 1, "6h": 6, "24h": 24}
        if r in hours:
            ta, label = now - hours[r] * 3600, "last %s" % r
        elif r == "today":
            ta, label = now - now % 86400, "today (UTC)"
        elif r == "session" or r.startswith("session:"):
            T = r.partition(":")[2]
            ss = ([s for s in A.sessions if s["T"] == T] if T else
                  [s for s in A.sessions if s["live"]] or A.sessions[-1:])
            if not ss and A.sessions:      # an unknown T: the session loaded for it
                ss = A.sessions[-1:]
            if ss:
                ta, tb, label = ss[0]["start"], ss[0]["end"], "session " + ss[0]["T"]
        elif r == "custom":
            a, b = parse_time(q.get("from")), parse_time(q.get("to"))
            ta = a if a is not None else first
            tb = b if b is not None else now
            label = "custom"
        return ta, tb + 1, label

    def feeds(self, st):
        """Is each feed of the current session connected right now? From the
        loaded data when the current session is loaded, else from the
        catalog (its events log's last minute line)."""
        cur = next((c for c in self.catalog() if c["current"]), None)
        if cur is None:
            return {}
        if st is None or cur["T"] not in st.sessions:
            eng = cur["engine"] or self.cfg.default_engine
            return {"session": cur["T"], "stopped": cur["stopped"], "engine": eng,
                    "engine_label": self.label(eng),
                    **{k: {"recorded": k in cur["files"],
                           "up": bool(cur["feeds"].get(k)) and cur["live"],
                           "since": None} for k in ("local", "remote", "rbn")}}
        with st.lock:
            s = st.sessions[cur["T"]]
            out = {}
            for kind in ("local", "remote", "rbn"):
                ev = s.conn[kind]
                out[kind] = {"recorded": kind in s.tails,
                             "up": bool(ev and ev[-1][1]) and s.stopped is None,
                             "since": ev[-1][0] if ev else None}
            eng = next((h[3] for h in reversed(s.health) if h[3]), None)
            out["engine"] = eng or (None if any(h[3] for h in s.health)
                                    else self.cfg.default_engine)
            out["engine_label"] = self.label(out["engine"]) if out["engine"] else None
            out["session"] = s.T
            out["stopped"] = s.stopped
            return out

    def label(self, engine):
        return self.cfg.engines.get(engine, (engine, ""))[0]

    def busy(self, q):
        """The reply while the range's sessions are still loading."""
        r = q.get("range", "session")
        return {"loading": self.loading(), "sessions": self.sessions_list(None),
                "feeds": self.feeds(self.snapshot()[0]), "now": time.time(),
                "range": {"key": r, "label": r}}

    def summary(self, q):
        if not self.want(self.needed(q, time.time())):
            return self.busy(q)
        st, gen = self.snapshot()
        P = self.params(q)
        engine, E = self.engine(st, q, P)
        A = self.analysis(st, gen, P, engine)
        ta, tb, label = self.range(q, A)
        S = summarize(A, ta, tb)
        S["engine"] = engine
        S["names"] = {"local": self.cfg.local_name, "remote": self.cfg.remote_name,
                      "local_about": self.cfg.local_about,
                      "remote_about": self.cfg.remote_about}
        S["engines"] = [{"key": e, "label": self.label(e),
                         "what": self.cfg.engines.get(e, ("", ""))[1],
                         "compared_s": E[e]["s"], "last": E[e]["last"]}
                        for e in sorted(E, key=lambda e: -E[e]["last"])]
        S["overview"] = []
        for e in S["engines"]:
            o = overview(A if e["key"] == engine else self.analysis(st, gen, P, e["key"]), ta, tb)
            o.update(key=e["key"], label=e["label"])
            S["overview"].append(o)
        S["rtty"] = self.rtty(st, gen, P, ta, tb)
        S["range"]["label"] = label
        S["range"]["key"] = q.get("range", "session")
        S["now"] = A.computed
        S["version"] = A.version
        S["sessions"] = self.sessions_list(A)
        S["loaded"] = sorted(self.sel or ())
        S["feeds"] = self.feeds(st)
        S["param_defs"] = {k: {"default": self.cfg.params[k], "min": v[1], "max": v[2],
                               "help": v[3]} for k, v in PARAMS.items()}
        S["scp_calls"] = len(self.scp)
        return S

    def events(self, q):
        if not self.want(self.needed(q, time.time())):
            return {"events": [], "loading": True}
        st, gen = self.snapshot()
        P = self.params(q)
        A = self.analysis(st, gen, P, self.engine(st, q, P)[0])
        ta, tb, _ = self.range(q, A)
        return {"events": [e for e in A.events if ta <= e["t0"] < tb]}

    def raw(self, q):
        P = self.params(q)
        band = q.get("band", "")
        f = float(q.get("f", 0) or 0)
        t0, t1 = int(float(q.get("t0", 0))), int(float(q.get("t1", 0)))
        calls = [c for c in q.get("calls", "").upper().split(",") if c]
        pad = 900
        out = {"L": [], "R": [], "rbn": []}
        st = self.snapshot()[0]
        if st is None:
            return out
        with st.lock:
            for side, lst in (("L", st.L), ("R", st.R)):
                i = bisect_left(lst, t0 - pad, key=lambda s: s.t)
                j = bisect_right(lst, t1 + pad, key=lambda s: s.t)
                for sp in lst[i:j]:
                    mine = sp.call in calls
                    if mine or abs(sp.f - f) <= 1.0:
                        out[side].append([sp.t, sp.line, mine])
            seen = set()
            win = P["rbn_win_min"] * 60
            for c in calls:
                for x in st.rbn.of_call(c, band, t0 - win, t1 + win):
                    seen.add(x)
            for x in st.rbn.near(band, t0 - P["bust_dt_s"], t1 + P["bust_dt_s"], f,
                                 P["rbn_df_khz"]):
                seen.add(x)
            out["rbn"] = sorted(seen)[:300]
        return out


class Handler(BaseHTTPRequestHandler):
    app = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        try:
            if u.path == "/events":
                return self.sse()
            api = {"/api/summary": self.app.summary, "/api/events": self.app.events,
                   "/api/raw": self.app.raw}.get(u.path)
            if api:
                return self.send(200, json.dumps(clean(api(q)), separators=(",", ":")))
            name = "index.html" if u.path in ("/", "") else u.path.lstrip("/")
            path = os.path.normpath(os.path.join(WEB, name))
            ext = os.path.splitext(path)[1]
            if not path.startswith(WEB + os.sep) or ext not in TYPES or not os.path.isfile(path):
                return self.send(404, "not found", "text/plain")
            with open(path, "rb") as fh:
                return self.send(200, fh.read(), TYPES[ext])
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            traceback.print_exc()
            try:
                self.send(500, json.dumps({"error": str(e)}))
            except OSError:
                pass

    def sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        v, last = None, 0
        try:
            while True:
                cur = self.app.version()
                now = time.time()
                if cur != v:
                    self.wfile.write(b"data: %s\n\n" % cur.encode())
                    v, last = cur, now
                elif now - last >= 15:
                    self.wfile.write(b": ping\n\n")
                    last = now
                self.wfile.flush()
                time.sleep(POLL_S)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--config", default=os.path.join(HERE, "compare.ini"))
    ap.add_argument("--listen")
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    cfg = Config(a.config)
    app = App(cfg)
    threading.Thread(target=app.poller, daemon=True).start()
    Handler.app = app
    listen, port = a.listen or cfg.listen, a.port or cfg.port
    srv = ThreadingHTTPServer((listen, port), Handler)
    srv.daemon_threads = True
    print("compare-web: http://%s:%d/  logs %s  (%d sessions; loading the current one)" % (
        listen, port, cfg.logs, len(app.catalog())), flush=True)
    app.want(app.needed({}, time.time()))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
