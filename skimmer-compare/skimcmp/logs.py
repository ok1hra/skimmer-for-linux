"""logs.py — read the capture-spots.sh recordings as they grow.

A session is the set of files one recorder run wrote, all named after its
UTC start time <T>:

  local-<T>.log  remote-<T>.log  rbn-<T>.log
      <UTC>\\tDX de …         a received line
      #<UTC>\\tconnected to … a connection event (also disconnected, cannot connect)
  events-<T>.log   <UTC> message      ("stopped — write check …" ends the session,
                                       a later "resumed session …" (-C) continues it)
  status-<T>.jsonl <UTC>\\t{json}     local skimmer health every few minutes

Files are tailed: each poll reads only what was appended, keeps a trailing
partial line for the next poll, and picks up sessions that appear later.
A Store may hold only some sessions (only=…): compare-web loads just the
ones the chosen range needs — all of them were 2.7 GB of text, minutes to
read and 1.5 GB of memory. catalog() describes every session without
reading its spots.
"""
import calendar
import json
import os
import re
import threading

from .windows import snapshot_windows

RESUMED = "resumed session"   # capture-spots.sh -C: the session goes on
# what capture-spots.sh says about the local skimmer's telnet feed port (from
# its /status.json "feed_port"): a port other than the one it reads, or off
PORT_WRONG_RE = re.compile(r"WARNING: the local skimmer serves its feed on port (\d+), "
                           r"but the recorder reads \S*?:?(\d+) \(-l\)")
PORT_OFF = "reports its telnet feed OFF"
PORT_OK = "local skimmer feed confirmed on port"
FILE_RE = re.compile(r"^(local|remote|rbn|events|status)-(\d{8}-\d{6})\.(?:log|jsonl)$")
# one regex for all three dialects:
#   DX de OK1HRA-#:   1825.8  OK1CF        CW    30 dB  25 WPM  CQ      1727Z   skimmer-for-linux
#   DX de OK1HRA-#:   1826.1  RZ3DX          15 dB  29 WPM                1727Z   CW Skimmer Server
#   DX de KM3T-2-#:14024.9  OK1ABC       CW    18 dB  25 WPM  CQ      1832Z     RBN
SPOT_RE = re.compile(
    r"^DX de (\S+?):\s*(\d+(?:\.\d+)?)\s+(\S+)\s+(?:([A-Z][A-Z0-9]*)\s+)?"
    r"(-?\d+)\s+dB\s+(\d+)\s+(WPM|BPS)\b\s*(.*?)\s*\d{4}Z\s*$")

STALE_S = 180          # a live session whose events log is this quiet has died
CHUNK = 8 << 20        # bytes a Tail reads at a time

_day = {}


def epoch(s):
    """'2026-10-01T17:27:41Z' → Unix seconds; None if malformed."""
    try:
        d = _day.get(s[:10])
        if d is None:
            d = _day[s[:10]] = calendar.timegm(
                (int(s[:4]), int(s[5:7]), int(s[8:10]), 0, 0, 0))
        return d + int(s[11:13]) * 3600 + int(s[14:16]) * 60 + int(s[17:19])
    except (ValueError, IndexError):
        return None


def session_epoch(T):
    """'20261001-172728' → Unix seconds"""
    return calendar.timegm((int(T[:4]), int(T[4:6]), int(T[6:8]),
                            int(T[9:11]), int(T[11:13]), int(T[13:15])))


class Spot:
    __slots__ = ("t", "f", "call", "snr", "wpm", "cm", "spotter", "line", "mode")

    def __init__(self, t, f, call, snr, wpm, cm, spotter, line, mode="CW"):
        self.t, self.f, self.call, self.snr, self.wpm = t, f, call, snr, wpm
        self.cm, self.spotter, self.line, self.mode = cm, spotter, line, mode


def parse_spot(t, body):
    """The spot of a line; its mode as the line names it — CW Skimmer
    Server names none: CW for WPM, RTTY for BPS."""
    m = SPOT_RE.match(body)
    if not m:
        return None
    mode = m.group(4) or ("CW" if m.group(7) == "WPM" else "RTTY")
    return Spot(t, float(m.group(2)), m.group(3).upper(), int(m.group(5)),
                int(m.group(6)), m.group(8).strip().upper(), m.group(1).upper(), body,
                mode)


class Truncated(Exception):
    pass


class Cancelled(Exception):
    """A Store's load was cancelled (its cancel event set) between batches."""


class Tail:
    def __init__(self, path):
        self.path, self.off, self.part = path, 0, b""

    def read(self):
        """The complete lines appended since the last read, in batches of
        at most CHUNK bytes: a 568 MB RBN log read in one piece held ~3 GB."""
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return
        if size < self.off:
            raise Truncated(self.path)
        if size == self.off:
            return
        with open(self.path, "rb") as fh:
            fh.seek(self.off)
            while self.off < size:
                data = fh.read(min(CHUNK, size - self.off))
                if not data:
                    return
                self.off += len(data)
                lines = (self.part + data).split(b"\n")
                self.part = lines.pop()
                if lines:
                    yield [l.decode("utf-8", "replace").rstrip("\r") for l in lines]


class Session:
    def __init__(self, T):
        self.T = T
        self.start = session_epoch(T)
        self.tails = {}
        self.conn = {"local": [], "remote": [], "rbn": []}   # [(t, up)]
        self.health = []          # [(t, streaming, lost_pct, engine or None, windows or None)]
        self.stopped = None       # time of "stopped — write check"
        self.last_t = self.start  # newest timestamp seen in any of its files
        self.last_event = None    # newest events-log line
        # the recorder reads another port than the local feed is on (or the
        # feed is off): {"since", "served" (0 = off), "read"}, else None
        self.port_warn = None

    def seen(self, t):
        if t is not None and t > self.last_t:
            self.last_t = t

    def end(self, latest, now):
        """Where the session ends: its stop line, now while it is alive, or
        the last thing it wrote if the recorder died without one."""
        if self.stopped is not None:
            return self.stopped
        if latest:
            ref = self.last_event if self.last_event is not None else self.start
            if now - ref < STALE_S:
                return now
        return self.last_t

    def up(self, kind, end):
        """[(a, b)] while the feed was connected, closed at the session end."""
        out, since = [], None
        for t, up in self.conn[kind]:
            if up and since is None:
                since = t
            elif not up and since is not None:
                out.append((since, t))
                since = None
        if since is not None and end > since:
            out.append((since, end))
        return out

    def engines(self, local_up, default):
        """[(a, b, engine)] for each local connection. Changing the decoder
        means restarting the local skimmer, which drops its feed — so one
        connection runs one decoder: the one its first status snapshot names.
        Old recordings name none: the configured default. A connection with
        no snapshot in a session that does name decoders: 'unknown'."""
        named = any(h[3] for h in self.health)
        out = []
        for a, b in local_up:
            eng = next((h[3] for h in self.health if a <= h[0] <= b and h[3]), None)
            out.append((a, b, eng or ("unknown" if named else default)))
        return out

    def rtty_windows(self, local_up):
        """[(a, b, RTTY windows)] for each local connection: the bands its
        first status snapshot decodes RTTY on (() = none, or an old snapshot
        from before RTTY)."""
        return [(a, b, next((h[5] if len(h) > 5 else None
                             for h in self.health if a <= h[0] <= b and h[4]), None) or ())
                for a, b in local_up]

    def windows(self, local_up):
        """[(a, b, local windows or None)] for each local connection: the
        bands its first status snapshot lists — changing them restarts the
        local skimmer, like changing the decoder. None: no snapshot, or an
        old one without bands."""
        return [(a, b, next((h[4] for h in self.health if a <= h[0] <= b and h[4]), None))
                for a, b in local_up]

    def unhealthy(self, lost_limit):
        """[(a, b)] covered by a status snapshot that says the local skimmer
        was not streaming or was losing packets."""
        out, prev = [], None
        for t, streaming, lost, *_ in self.health:
            if not streaming or lost >= lost_limit:
                a = prev if prev is not None else t - 300
                out.append((max(a, t - 600), t))
            prev = t
        return out


class Store:
    """Everything recorded so far. poll() appends; readers take the lock."""

    def __init__(self, logs_dir, rbn_index, only=None, cancel=None):
        """only: the session names <T> to read (None: every session);
        cancel: a threading.Event that stops a poll between batches."""
        self.dir = logs_dir
        self.rbn_factory = rbn_index
        self.only = None if only is None else frozenset(only)
        self.cancel = cancel
        self.lock = threading.RLock()
        self.version = 0
        self.reset()

    def progress(self):
        """(bytes read, bytes in the files) — how far a load has come.
        Read without the lock: a poll holds it for the whole first read."""
        done = total = 0
        for s in list(self.sessions.values()):
            for t in list(s.tails.values()):
                done += t.off
                try:
                    total += os.path.getsize(t.path)
                except OSError:
                    total += t.off
        return done, total

    def reset(self):
        self.sessions = {}
        # Spot, chronological. L and R are CW — the comparison of two CW
        # skimmers; the local RTTY spots are Lr, RBN's RTTY spots rbn_rtty
        # (RTTY against RBN, rtty.py). The referee (rbn) takes every mode:
        # a call RBN heard in RTTY is a real call too.
        self.L, self.R, self.Lr = [], [], []
        self.rbn = self.rbn_factory()
        self.rbn_rtty = self.rbn_factory()
        self.version += 1

    def ordered(self):
        return [self.sessions[T] for T in sorted(self.sessions)]

    def current(self, now):
        """The session recording right now: the newest one with no stop line
        whose events log is still written; else simply the newest. A short
        run started and stopped beside a running recorder must not hide it."""
        ss = self.ordered()
        for s in reversed(ss):
            if (s.stopped is None and s.last_event is not None
                    and now - s.last_event < STALE_S):
                return s
        return ss[-1] if ss else None

    def poll(self):
        with self.lock:
            try:
                return self._poll()
            except Truncated:
                self.reset()           # a file shrank: rebuild from scratch
                return self._poll() or True

    def _poll(self):
        try:
            names = os.listdir(self.dir)
        except OSError:
            return False
        for n in names:
            m = FILE_RE.match(n)
            if not m:
                continue
            kind, T = m.group(1), m.group(2)
            if self.only is not None and T not in self.only:
                continue
            s = self.sessions.get(T)
            if s is None:
                s = self.sessions[T] = Session(T)
            if kind not in s.tails:
                s.tails[kind] = Tail(os.path.join(self.dir, n))
        changed = False
        for s in self.ordered():
            for kind in ("events", "status", "local", "remote", "rbn"):
                tail = s.tails.get(kind)
                if tail is None:
                    continue
                for lines in tail.read():
                    if self.cancel is not None and self.cancel.is_set():
                        raise Cancelled()
                    changed = True
                    for line in lines:
                        self._ingest(s, kind, line)
        if changed:
            self.version += 1
        return changed

    def _ingest(self, s, kind, line):
        if kind == "events":
            t = epoch(line[:20])
            if t is None:
                return
            s.last_event = t
            s.seen(t)
            if "stopped — write check" in line:
                s.stopped = t
            elif RESUMED in line:
                s.stopped = None
            if PORT_OK in line or RESUMED in line or "recording as " in line:
                s.port_warn = None       # a new start checks the port anew
            elif PORT_OFF in line:
                s.port_warn = {"since": t, "served": 0, "read": None}
            else:
                m = PORT_WRONG_RE.search(line)
                if m:
                    s.port_warn = {"since": t, "served": int(m.group(1)), "read": int(m.group(2))}
            return
        if kind == "status":
            ts, _, js = line.partition("\t")
            t = epoch(ts)
            if t is None:
                return
            try:
                d = json.loads(js)
            except ValueError:
                return
            s.health.append((t, bool(d.get("streaming", True)),
                             float(d.get("lost_pct") or 0.0), d.get("engine") or None,
                             snapshot_windows(d), snapshot_windows(d, rtty=True)))
            s.seen(t)
            return
        if line.startswith("#"):
            ts, _, msg = line[1:].partition("\t")
            t = epoch(ts)
            if t is None:
                return
            s.seen(t)
            if msg.startswith("connected"):
                s.conn[kind].append((t, True))
            elif msg.startswith(("disconnected", "cannot connect", "keepalive write failed")):
                s.conn[kind].append((t, False))
            return
        ts, _, body = line.partition("\t")
        if not body.startswith("DX de "):
            return
        t = epoch(ts)
        if t is None:
            return
        s.seen(t)
        sp = parse_spot(t, body)
        if sp is None:
            return
        if kind == "rbn":
            self.rbn.add(sp)
            if sp.mode == "RTTY":
                self.rbn_rtty.add(sp)
        elif sp.mode not in ("CW", "RTTY"):
            return
        else:
            lst = (self.R if kind == "remote" else self.L if sp.mode == "CW" else self.Lr)
            if lst and lst[-1].t > t:          # keep the lists sorted
                i = len(lst)
                while i > 0 and lst[i - 1].t > t:
                    i -= 1
                lst.insert(i, sp)
            else:
                lst.append(sp)


FEED_RE = re.compile(r"\b(local|remote|rbn) (up|DOWN)\b")


def _tail_lines(path, nbytes):
    """The complete lines in the last nbytes of a file ([] if unreadable)."""
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            fh.seek(max(0, size - nbytes))
            data = fh.read()
    except OSError:
        return []
    lines = data.decode("utf-8", "replace").split("\n")
    if size > nbytes:
        lines = lines[1:]            # the first one is cut
    return [l.rstrip("\r") for l in lines if l.strip()]


def catalog(logs_dir, now):
    """Every session in logs_dir without reading its spots: the tail of its
    events log says when it last wrote, whether it stopped and how its feeds
    were at the last minute line; the tail of its status log names the
    decoder. [{T, start, end, live, stopped, bytes, files, feeds, engine}],
    oldest first; "current" marks the session a recorder writes now (else
    the newest), as Store.current does."""
    try:
        names = os.listdir(logs_dir)
    except OSError:
        return []
    S = {}
    for n in names:
        m = FILE_RE.match(n)
        if not m:
            continue
        kind, T = m.group(1), m.group(2)
        path = os.path.join(logs_dir, n)
        try:
            st = os.stat(path)
        except OSError:
            continue
        s = S.setdefault(T, {"T": T, "start": session_epoch(T), "files": {}, "bytes": 0,
                             "mtime": 0, "paths": {}})
        s["files"][kind] = st.st_size
        s["paths"][kind] = path
        s["bytes"] += st.st_size
        s["mtime"] = max(s["mtime"], st.st_mtime)
    out = []
    for T in sorted(S):
        s = S[T]
        last_event = stopped = None
        resumed = False
        feeds = {}
        for line in reversed(_tail_lines(s["paths"].get("events", ""), 16384)):
            t = epoch(line[:20])
            if t is None:
                continue
            if last_event is None:
                last_event = t
            if RESUMED in line:
                resumed = True     # a stop line before it no longer ends it
            if stopped is None and not resumed and "stopped — write check" in line:
                stopped = t
            if not feeds and FEED_RE.search(line):
                feeds = {k: v == "up" for k, v in FEED_RE.findall(line)}
            if feeds and last_event is not None:
                break
        engine = None
        for line in reversed(_tail_lines(s["paths"].get("status", ""), 65536)):
            try:
                engine = json.loads(line.partition("\t")[2]).get("engine")
            except ValueError:
                continue
            break
        end = stopped or max(last_event or 0, s["mtime"], s["start"])
        out.append({"T": T, "start": s["start"], "end": end, "stopped": stopped,
                    "last_event": last_event,
                    "live": stopped is None and last_event is not None
                            and now - last_event < STALE_S,
                    "bytes": s["bytes"], "files": s["files"], "feeds": feeds,
                    "engine": engine, "current": False})
    cur = next((s for s in reversed(out) if s["live"]), out[-1] if out else None)
    if cur:
        cur["current"] = True
    return out
