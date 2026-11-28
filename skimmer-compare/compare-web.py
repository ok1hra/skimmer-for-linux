#!/usr/bin/env python3
"""compare-web.py — the public page comparing two CW skimmers, live.

Follows the current recording session (what capture-spots.sh records in
logs/: the two skimmers' feeds, RBN, events, the local status) and serves one
page about it: L, the Linux skimmer (skimmer-headless), against W, the
Windows skimmer (CW Skimmer Server) — recall and bust rate by SNR, recall and
bust rate with their confidence intervals, recall over time.

  compare-web.py [-c compare.ini] [--listen ADDR] [--port N]

The page is static. The server answers GET and HEAD of five fixed paths and
nothing else; it reads no query parameter, no body, no cookie:

  /, /index.html, /app.js, /style.css   the page (web/, read once at start)
  /data.json                            the numbers of the current session
  /download/events.csv                  every compared station of the session
  /download/spots-L.tsv, spots-W.tsv    each skimmer's spot lines as received
  /download/rbn.tsv.gz                  the RBN spots the referee used (gzip)

data.json and the downloads are computed in the background every [web]
every_s seconds (the RBN file every RBN_EVERY_S) — never because of a
request. data.json is a whitelist: numbers only, no calls, no addresses, no
paths. The downloads carry calls (they are spots, public like RBN's) and
nothing else: no connection lines, no status snapshots. Read-only on logs/. Python 3 standard
library only.
"""
import argparse
import csv
import gzip
import io
import json
import math
import os
import re
import socket
import sys
import threading
import time
import traceback
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from skimcmp.arbiter import RbnIndex, load_scp          # noqa: E402
from skimcmp.config import Config                       # noqa: E402
from skimcmp.logs import Cancelled, Store, catalog      # noqa: E402
from skimcmp.match import analyze, compared_time, engine_summary  # noqa: E402
from skimcmp.stats import summarize                     # noqa: E402
from skimcmp.windows import loose_bands                 # noqa: E402

WEB = os.path.join(HERE, "web")
POLL_S = 2            # how often the logs are re-read
CATALOG_S = 2         # the session catalog is re-read at most this often
LOADING_S = 1         # while a session loads, data.json is rewritten this often
RBN_MARGIN_KHZ = 2.0  # RBN spots kept this far outside the local windows
MAX_CONN = 64         # connections served at once; more are closed at once
MAX_PER_IP = 8        # … and at most this many from one address
CONN_TIMEOUT_S = 10   # a connection that sends nothing for this long is closed
CONN_DEADLINE_S = 20  # a request must have arrived whole within this, however it trickles
SEND_DEADLINE_S = 600 # the answer must be sent within this (a large download, a slow link);
                      # each 64 kB of it within CONN_TIMEOUT_S
CHUNK = 65536
RBN_EVERY_S = 600     # the RBN download (MBs) is rebuilt at most this often

# path → (file in web/ or None for data.json, content type)
ROUTES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/index.html": ("index.html", "text/html; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/style.css": ("style.css", "text/css; charset=utf-8"),
    "/data.json": (None, "application/json; charset=utf-8"),
}
# the downloads: path → (what, content type, file name suffix)
DOWNLOADS = {
    "/download/events.csv": ("every compared station: time, band, kHz, kind, both calls, "
                             "referee, SNR, WPM", "text/csv; charset=utf-8", "events.csv"),
    "/download/spots-L.tsv": ("the Linux skimmer's CW spots as received",
                              "text/tab-separated-values; charset=utf-8", "spots-L.tsv"),
    "/download/spots-W.tsv": ("the Windows skimmer's spots as received (all comments)",
                              "text/tab-separated-values; charset=utf-8", "spots-W.tsv"),
    "/download/rbn.tsv.gz": ("the RBN spots of other skimmers on the compared bands — the referee",
                             "application/gzip", "rbn.tsv.gz"),
}
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; "
       "img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
HEADERS = (("Content-Security-Policy", CSP), ("X-Content-Type-Options", "nosniff"),
           ("X-Frame-Options", "DENY"), ("Referrer-Policy", "no-referrer"),
           ("Cross-Origin-Resource-Policy", "same-origin"), ("Cache-Control", "no-cache"))


def clean(o):
    """JSON cannot carry inf/nan; tuples become lists."""
    if isinstance(o, float):
        return None if math.isnan(o) or math.isinf(o) else round(o, 4)
    if isinstance(o, dict):
        return {k: clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    return o


# ---- the public projection: L and W, numbers only ----------------------------------
def lw(s):
    """The analysis names the sides L and R; the page L and W."""
    return re.sub(r"\bR\b", "W", s)


def prop(d):
    return {k: d.get(k) for k in ("k", "n", "p", "lo", "hi")}


def view(cfg, S, feeds, engine, T_name, live, scp_n, joined=None):
    """What data.json holds of one summarize() result — a whitelist."""
    label, what = cfg.engines.get(engine, (engine, ""))
    rec, bust, lat, T = S["recall"], S["bust"], S["latency"], S["timeline"]
    side = {"L": "L", "R": "W"}
    C = S["counts"]
    return {
        "state": "ok",
        "session": {"T": T_name, "start": S["range"]["from"], "end": S["range"]["to"],
                    "live": live, "stopped": bool(feeds.get("stopped")),
                    # [web] since: one decoder joined over these sessions
                    "joined": joined},
        "feeds": {k: bool((feeds.get(src) or {}).get("up"))
                  for k, src in (("L", "local"), ("W", "remote"), ("RBN", "rbn"))},
        # the recorder's word on the local feed's port: numbers only
        "warnings": [{"kind": "feed_port", "since": w["since"], "served": w["served"],
                      "read": w["read"]} for w in [feeds.get("port_warn")] if w],
        "names": {"L_about": cfg.local_about, "W_about": cfg.remote_about,
                  "decoder": label, "decoder_what": what},
        "time": {k: S["time"][k] for k in ("span_s", "common_s", "common_from", "common_to",
                                            "rbn_s")},
        "counts": {"spots_L": C["spots_L"], "spots_W": C["spots_R"], "spots_W_all": C["spots_R_all"],
                   "events": C["events"], "rbn_spots": C["rbn_spots"], "rbn_own": C["rbn_own"],
                   "scp_calls": scp_n,
                   "filtered_L": C["filtered_L"], "filtered_W": C["filtered_R"],
                   "phased_L": C.get("phased_L")},
        # L's callsign filter (compare.ini [local filter]): both sides are
        # compared only on the calls it lets through
        "filter": {"about": cfg.filter_about,
                   "since": next((s for s, p in cfg.call_filter.describe() if p), None),
                   "on": bool(cfg.call_filter)},
        "bands": [{"band": b["band"], "lo": b["lo"], "hi": b["hi"], "n": b["n"],
                   "L": prop(b["L"]), "W": prop(b["R"]), "only_L": b["onlyL"], "only_W": b["onlyR"],
                   "bust_L": b["bustL"], "eps_L": b["epsL"], "bust_W": b["bustR"], "eps_W": b["epsR"],
                   "nonCQ": b["nonCQ"], "held_L": b["heldL"], "held_W": b["heldR"], "p": b["p"],
                   "median_W_minus_L": b["lat"], "off_s": b.get("off_s", 0)}
                  for b in S["bands"]],
        "recall": {"L": prop(rec["L"]), "W": prop(rec["R"]), "n": rec["n"],
                   "only_L": rec["b"], "only_W": rec["c"], "p": rec["p"]},
        "bust": {"L": prop(bust["L"]), "W": prop(bust["R"]), "p": bust["p"]},
        # episodes on another station's frequency (match.py "held"): a real
        # call only MASTER.SCP vouches for, where RBN heard another one —
        # out of recall and of the bust rate, of all the side's episodes
        "held": {"L": prop(S["held"]["L"]), "W": prop(S["held"]["R"])},
        "latency": {"n": lat["n"], "median_W_minus_L": lat["median"],
                    "L_first": lat["L_first"], "W_first": lat["R_first"], "ties": lat["ties"]},
        "verdicts": [{"metric": v["metric"], "winner": side.get(v["winner"]),
                      "text": lw(v["text"])} for v in S["verdicts"]],
        "snr": {"pairs": S["snr"]["offset_n"],
                "bins": [{"lo": b["lo"], "hi": b["hi"], "n": b["n"],
                          "L": prop(b["L"]), "W": prop(b["R"]),
                          "bustL": prop(b["bustL"]), "bustW": prop(b["bustR"])}
                         for b in S["snr"]["bins"]]},
        "timeline": {
            "step": T["step"],
            "rows": [{"t": r["t"], "n": r["n"], "L": r["L"], "W": r["R"],
                      "bustL": r["bustL"], "bustW": r["bustR"], "common_s": r["common_s"]}
                     for r in T["rows"]],
            "outages": T["outages"], "other": T["other"],
            "dead": [{"side": side[d["side"]], "t0": d["t0"], "t1": d["t1"]} for d in T["dead"]],
            "silences": [{"side": side[s["side"]], "band": s["band"], "t0": s["t0"],
                          "t1": s["t1"], "other": s["other"]} for s in T["silences"]]},
    }


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


KIND = {"match": "match", "only-L": "only L", "only-R": "only W", "bust": "bust pair",
        "nonCQ": "W heard it not as CQ", "held": "another station's frequency"}


def cell(v):
    """No cell a spreadsheet would run as a formula."""
    v = str(v)
    return "'" + v if v[:1] in ("=", "+", "-", "@", "\t", "\r") and not _NUM.match(v) else v


_NUM = re.compile(r"^-?\d+(\.\d+)?$")


def events_csv(evs):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(["first_utc", "last_utc", "band", "khz", "kind", "real", "L_call", "W_call",
                "L_found", "W_found", "L_busted", "W_busted", "referee", "rbn_spotters",
                "snr_db_W_scale", "wpm", "first_spot_W_minus_L_s", "note"])
    for e in evs:
        w.writerow([cell(x) for x in [iso(e["t0"]), iso(e["t1"]), e["band"], e["f"], KIND.get(e["cat"], e["cat"]),
                    int(e["inU"]), (e["L"] or {}).get("call", ""), (e["R"] or {}).get("call", ""),
                    int(e["cL"]), int(e["cR"]), int(e["bL"]), int(e["bR"]), e["v"], e["rbn"],
                    "" if e["snr"] is None else e["snr"], "" if e["wpm"] is None else e["wpm"],
                    "" if e["lat"] is None else round(e["lat"]), lw(e["note"] or "")]])
    return out.getvalue().encode()


def spots_tsv(spots, what, ta=None):
    lines = ["# %s — received_utc<TAB>the line as the skimmer sent it\n" % what]
    lines += ["%s\t%s\n" % (iso(sp.t), sp.line.replace("\t", " ")) for sp in spots
              if ta is None or sp.t >= ta]
    return "".join(lines).encode()


def rbn_gz(rbn, ta=None):
    rows = []
    for band in rbn.t:
        T, F, Cc, Ss, N = rbn.t[band], rbn.f[band], rbn.c[band], rbn.s[band], rbn.snr[band]
        for p in range(len(T)):
            if ta is not None and T[p] < ta:
                continue
            rows.append((T[p], band, rbn.spotters[Ss[p]], round(F[p], 1), rbn.calls[Cc[p]], N[p]))
    rows.sort()
    buf = io.StringIO()
    buf.write("# RBN spots of other skimmers on the compared bands (our own spotter left out)\n"
              "utc\tband\tspotter\tkhz\tcall\tsnr_db\n")
    for t, band, sp, f, c, snr in rows:
        buf.write("%s\t%s\t%s\t%.1f\t%s\t%d\n" % (iso(t), band, sp, f, c, snr))
    return gzip.compress(buf.getvalue().encode(), 6, mtime=0)


# ---- the current session, followed in the background -----------------------------------
class App:
    def __init__(self, cfg):
        self.cfg = cfg
        bands = loose_bands(cfg.local_windows, RBN_MARGIN_KHZ)
        self.rbn_factory = lambda: RbnIndex(bands, cfg.exclude)
        self.scp = load_scp(cfg.scp)
        # self.store holds exactly the sessions self.sel; a new current
        # session is loaded beside it and swapped in when complete
        self.lock = threading.Lock()
        self.store, self.sel = None, None
        self.loader = None       # {"sel", "store", "cancel", "t0", "error"}
        self._cat = (0.0, [])
        self.data = b'{"state":"starting"}'
        self.downloads = {}      # path → bytes
        self.session_name = ""
        self.rbn_built = (None, 0.0)   # (session, when) of the RBN download

    def catalog(self):
        now = time.time()
        t, cat = self._cat
        if now - t >= CATALOG_S:
            cat = catalog(self.cfg.logs, now)
            self._cat = (now, cat)
        return cat

    def current(self):
        """The sessions the page covers (frozenset of their <T>): the one
        recording, and with [web] since every session that wrote since then."""
        since = self.cfg.web_since
        return frozenset(s["T"] for s in self.catalog()
                         if s["current"] or (since is not None and (s["end"] or s["start"]) >= since))

    def want(self, sel):
        """True when exactly the sessions sel are loaded; else start (or keep)
        loading them in the background and return False."""
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
        print("compare-web: loaded %s in %.0f s (%d L, %d R, %d RBN spots)" % (
            ", ".join(sorted(L["sel"])) or "no session", time.time() - L["t0"],
            len(st.L), len(st.R), st.rbn.n), flush=True)

    def progress(self):
        with self.lock:
            L = self.loader
        if L is None or L["store"] is None:
            return 0, 0
        return L["store"].progress()

    def poller(self):
        while True:
            time.sleep(POLL_S)
            st = self.store
            if st is None:
                continue
            try:
                st.poll()
            except Exception:
                traceback.print_exc()

    def feeds(self, st):
        """Is each feed of the current session connected right now?"""
        cur = next((c for c in self.catalog() if c["current"]), None)
        out = {}
        if cur is None or cur["T"] not in st.sessions:
            return out
        with st.lock:
            s = st.sessions[cur["T"]]
            for kind in ("local", "remote", "rbn"):
                ev = s.conn[kind]
                out[kind] = {"up": bool(ev and ev[-1][1]) and s.stopped is None}
            out["stopped"] = s.stopped
            out["port_warn"] = s.port_warn if s.stopped is None else None
        return out

    def snapshot(self):
        """data.json now: the current session's numbers, or "loading"."""
        if not self.want(self.current()):
            done, total = self.progress()
            return {"state": "loading", "read": done, "total": total}
        st, now, P = self.store, time.time(), dict(self.cfg.params)
        with st.lock:
            segs = compared_time(st, P, now, self.cfg.default_engine)[3]
            E = engine_summary(segs)
            # [web] engine, else the decoder of the newest compared time
            engine = self.cfg.web_engine or (
                max(E, key=lambda e: E[e]["last"]) if E else self.cfg.default_engine)
            A = analyze(st, self.cfg.local_windows, self.cfg.remote_windows, self.scp, P,
                        now, engine, self.cfg.default_engine, self.cfg.call_filter)
        live = [s for s in A.sessions if s["live"]] or A.sessions[-1:]
        ta, tb = (live[0]["start"], live[0]["end"]) if live else (now, now)
        T_name = live[0]["T"] if live else ""
        joined, name = None, T_name
        since = self.cfg.web_since
        if since is not None and A.sessions:
            # one decoder since a time, over every session of that time
            ta = max(since, min(s["start"] for s in A.sessions))
            joined = {"since": since, "sessions": [s["T"] for s in A.sessions]}
            name = "%s-since-%s" % (engine, time.strftime("%Y%m%d-%H%M", time.gmtime(since)))
        S = summarize(A, ta, tb + 1)
        D = view(self.cfg, S, self.feeds(st), engine, T_name, bool(live and live[0]["live"]),
                 len(self.scp), joined)
        self.build_downloads(st, A, ta, tb + 1, name if joined else "session " + T_name, now)
        self.session_name = re.sub(r"[^0-9A-Za-z.-]", "", name)
        D["downloads"] = [{"path": p.lstrip("/"), "what": DOWNLOADS[p][0],
                           "bytes": len(self.downloads[p])}
                          for p in DOWNLOADS if p in self.downloads]
        return D

    def build_downloads(self, st, A, ta, tb, T_name, now):
        d = dict(self.downloads)
        with st.lock:
            d["/download/events.csv"] = events_csv([e for e in A.events if ta <= e["t0"] < tb])
            d["/download/spots-L.tsv"] = spots_tsv(st.L, "Linux skimmer (L) CW spots, " + T_name, ta)
            d["/download/spots-W.tsv"] = spots_tsv(st.R, "Windows skimmer (W) spots, " + T_name, ta)
            sess, when = self.rbn_built
            if sess != T_name or now - when >= RBN_EVERY_S or "/download/rbn.tsv.gz" not in d:
                d["/download/rbn.tsv.gz"] = rbn_gz(st.rbn, ta)
                self.rbn_built = (T_name, now)
        self.downloads = d

    def publisher(self):
        while True:
            wait = self.cfg.every
            try:
                D = self.snapshot()
                if D["state"] == "loading":
                    wait = LOADING_S
                D["generated"] = time.time()
                D["every_s"] = self.cfg.every
                self.data = json.dumps(clean(D), separators=(",", ":")).encode()
            except Exception:
                traceback.print_exc()
            time.sleep(wait)


# ---- the server: five fixed paths, GET and HEAD -------------------------------------------
class Handler(BaseHTTPRequestHandler):
    app = None
    files = {}                       # web/ file → bytes, read once at start
    server_version = "skimmer-compare"
    sys_version = ""
    protocol_version = "HTTP/1.0"    # one request per connection
    timeout = CONN_TIMEOUT_S

    def log_message(self, *a):
        pass

    def version_string(self):
        return self.server_version

    def setup(self):
        super().setup()
        # a client that trickles its request byte by byte resets the idle
        # timeout each time: cut every connection at a hard deadline
        self.deadline = None
        self.arm(CONN_DEADLINE_S)

    def arm(self, seconds):
        conn = self.connection

        def cut():
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.deadline is not None:
            self.deadline.cancel()
        self.deadline = threading.Timer(seconds, cut)
        self.deadline.daemon = True
        self.deadline.start()

    def finish(self):
        self.deadline.cancel()
        try:
            super().finish()
        except OSError:
            pass

    def send_error(self, code, message=None, explain=None):
        """Malformed requests, unknown methods: a bare status, no HTML."""
        self.close_connection = True
        try:
            self.reply(code, b"%d\n" % code, "text/plain; charset=utf-8",
                       getattr(self, "command", None) == "HEAD")
        except OSError:
            pass

    def reply(self, code, body, ctype, head, disposition=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if disposition:
            self.send_header("Content-Disposition", disposition)
        self.send_header("Content-Length", str(len(body)))
        for k, v in HEADERS:
            self.send_header(k, v)
        self.end_headers()
        if not head:
            for i in range(0, len(body), CHUNK):     # CONN_TIMEOUT_S for each
                self.wfile.write(body[i:i + CHUNK])

    def answer(self, head):
        self.arm(SEND_DEADLINE_S)        # the request is in: now the time to send
        path = urllib.parse.urlsplit(self.path).path
        route = ROUTES.get(path)
        try:
            if path in DOWNLOADS:
                body = self.app.downloads.get(path)
                if body is None:
                    return self.reply(503, b"not ready yet\n", "text/plain; charset=utf-8", head)
                _, ctype, suffix = DOWNLOADS[path]
                T = self.app.session_name or "session"
                return self.reply(200, body, ctype, head,
                                  'attachment; filename="skimmer-compare-%s-%s"' % (T, suffix))
            if route is None:
                return self.reply(404, b"not found\n", "text/plain; charset=utf-8", head)
            name, ctype = route
            body = self.app.data if name is None else self.files[name]
            self.reply(200, body, ctype, head)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.answer(False)

    def do_HEAD(self):
        self.answer(True)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.count_lock = threading.Lock()
        self.total, self.per_ip = 0, {}

    def _take(self, ip):
        with self.count_lock:
            if self.total >= MAX_CONN or self.per_ip.get(ip, 0) >= MAX_PER_IP:
                return False
            self.total += 1
            self.per_ip[ip] = self.per_ip.get(ip, 0) + 1
            return True

    def _give(self, ip):
        with self.count_lock:
            self.total -= 1
            n = self.per_ip[ip] - 1
            if n:
                self.per_ip[ip] = n
            else:
                del self.per_ip[ip]

    def process_request(self, request, client_address):
        ip = client_address[0]
        if not self._take(ip):
            self.shutdown_request(request)       # full: close, do not queue
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self._give(ip)
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._give(client_address[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--config", default=os.path.join(HERE, "compare.ini"))
    ap.add_argument("--listen")
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    cfg = Config(a.config)
    for name, _ in set(ROUTES.values()):
        if name:
            with open(os.path.join(WEB, name), "rb") as fh:
                Handler.files[name] = fh.read()
    app = Handler.app = App(cfg)
    threading.Thread(target=app.poller, daemon=True).start()
    threading.Thread(target=app.publisher, daemon=True).start()
    listen, port = a.listen or cfg.listen, a.port if a.port is not None else cfg.port
    srv = Server((listen, port), Handler)
    print("compare-web: http://%s:%d/  logs %s  (the current session, every %d s)" % (
        listen, srv.server_address[1], cfg.logs, cfg.every), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
