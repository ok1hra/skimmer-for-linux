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

Range: range=all|1h|6h|24h|today|session|session:<T>|custom (&from=&to=,
ISO UTC or Unix seconds). Parameters: any name from skimcmp/config.py.
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
from skimcmp.config import PARAMS, Config, clamp_params  # noqa: E402
from skimcmp.match import analyze, compared_time, engine_summary  # noqa: E402
from skimcmp.stats import overview, summarize            # noqa: E402
from skimcmp.windows import loose_bands                  # noqa: E402

WEB = os.path.join(HERE, "web")
POLL_S = 2            # how often the logs are re-read
MIN_RECOMPUTE_S = 5   # new data recomputes at most this often
LIVE_RECOMPUTE_S = 30 # a live session's common time grows even with no spots
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
        from skimcmp.logs import Store
        self.cfg = cfg
        bands = loose_bands(cfg.local_windows, RBN_MARGIN_KHZ)
        self.store = Store(cfg.logs, lambda: RbnIndex(bands, cfg.exclude))
        self.scp = load_scp(cfg.scp)
        self.cache = {}
        self.clock = threading.Lock()
        self.store.poll()

    def poller(self):
        while True:
            time.sleep(POLL_S)
            try:
                self.store.poll()
            except Exception:
                traceback.print_exc()

    def params(self, q):
        return clamp_params(q, self.cfg.params)

    def engines(self, P):
        """{engine: {"s", "last"}} over all data, and the default choice:
        the decoder of the newest compared time."""
        with self.store.lock:
            segs = compared_time(self.store, P, time.time(), self.cfg.default_engine)[3]
        E = engine_summary(segs)
        default = max(E, key=lambda e: E[e]["last"]) if E else self.cfg.default_engine
        return E, default

    def engine(self, q, P):
        E, default = self.engines(P)
        e = q.get("engine")
        return (e if e in E else default), E

    def analysis(self, P, engine):
        key = (engine,) + tuple(sorted(P.items()))
        with self.clock:
            now = time.time()
            A = self.cache.get(key)
            live = A is not None and any(s["live"] for s in A.sessions)
            if (A is None
                    or (A.version != self.store.version and now - A.computed >= MIN_RECOMPUTE_S)
                    or (live and now - A.computed >= LIVE_RECOMPUTE_S)):
                A = analyze(self.store, self.cfg.local_windows, self.cfg.remote_windows,
                            self.scp, P, now, engine, self.cfg.default_engine)
                self.cache[key] = A
                while len(self.cache) > 8:
                    self.cache.pop(min(self.cache, key=lambda k: self.cache[k].computed))
            return A

    def range(self, q, A):
        now = A.computed
        r = q.get("range", "all")
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
            if ss:
                ta, tb, label = ss[0]["start"], ss[0]["end"], "session " + ss[0]["T"]
        elif r == "custom":
            a, b = parse_time(q.get("from")), parse_time(q.get("to"))
            ta = a if a is not None else first
            tb = b if b is not None else now
            label = "custom"
        return ta, tb + 1, label

    def feeds(self):
        """Is each feed of the current session connected right now?"""
        with self.store.lock:
            s = self.store.current(time.time())
            if s is None:
                return {}
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

    def summary(self, q):
        P = self.params(q)
        engine, E = self.engine(q, P)
        A = self.analysis(P, engine)
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
            o = overview(A if e["key"] == engine else self.analysis(P, e["key"]), ta, tb)
            o.update(key=e["key"], label=e["label"])
            S["overview"].append(o)
        S["range"]["label"] = label
        S["range"]["key"] = q.get("range", "all")
        S["now"] = A.computed
        S["version"] = A.version
        S["feeds"] = self.feeds()
        S["param_defs"] = {k: {"default": self.cfg.params[k], "min": v[1], "max": v[2],
                               "help": v[3]} for k, v in PARAMS.items()}
        S["scp_calls"] = len(self.scp)
        return S

    def events(self, q):
        P = self.params(q)
        A = self.analysis(P, self.engine(q, P)[0])
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
        st = self.store
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
                cur = self.app.store.version
                now = time.time()
                if cur != v:
                    self.wfile.write(b"data: %d\n\n" % cur)
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
    print("compare-web: http://%s:%d/  logs %s  (%d L, %d R, %d RBN spots)" % (
        listen, port, cfg.logs, len(app.store.L), len(app.store.R), app.store.rbn.n),
        flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
