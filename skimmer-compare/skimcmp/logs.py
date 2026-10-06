"""logs.py — read the capture-spots.sh recordings as they grow.

A session is the set of files one recorder run wrote, all named after its
UTC start time <T>:

  local-<T>.log  remote-<T>.log  rbn-<T>.log
      <UTC>\\tDX de …         a received line
      #<UTC>\\tconnected to … a connection event (also disconnected, cannot connect)
  events-<T>.log   <UTC> message      ("stopped — write check …" ends the session)
  status-<T>.jsonl <UTC>\\t{json}     local skimmer health every few minutes

Files are tailed: each poll reads only what was appended, keeps a trailing
partial line for the next poll, and picks up sessions that appear later.
"""
import calendar
import json
import os
import re
import threading

from .windows import snapshot_windows

FILE_RE = re.compile(r"^(local|remote|rbn|events|status)-(\d{8}-\d{6})\.(?:log|jsonl)$")
# one regex for all three dialects:
#   DX de OK1HRA-#:   1825.8  OK1CF        CW    30 dB  25 WPM  CQ      1727Z   skimmer-for-linux
#   DX de OK1HRA-#:   1826.1  RZ3DX          15 dB  29 WPM                1727Z   CW Skimmer Server
#   DX de KM3T-2-#:14024.9  OK1ABC       CW    18 dB  25 WPM  CQ      1832Z     RBN
SPOT_RE = re.compile(
    r"^DX de (\S+?):\s*(\d+(?:\.\d+)?)\s+(\S+)\s+(?:[A-Z][A-Z0-9]*\s+)?"
    r"(-?\d+)\s+dB\s+(\d+)\s+(?:WPM|BPS)\b\s*(.*?)\s*\d{4}Z\s*$")

STALE_S = 180          # a live session whose events log is this quiet has died

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
    __slots__ = ("t", "f", "call", "snr", "wpm", "cm", "spotter", "line")

    def __init__(self, t, f, call, snr, wpm, cm, spotter, line):
        self.t, self.f, self.call, self.snr, self.wpm = t, f, call, snr, wpm
        self.cm, self.spotter, self.line = cm, spotter, line


def parse_spot(t, body):
    m = SPOT_RE.match(body)
    if not m:
        return None
    return Spot(t, float(m.group(2)), m.group(3).upper(), int(m.group(4)),
                int(m.group(5)), m.group(6).strip().upper(), m.group(1).upper(), body)


class Truncated(Exception):
    pass


class Tail:
    def __init__(self, path):
        self.path, self.off, self.part = path, 0, b""

    def read(self):
        try:
            size = os.path.getsize(self.path)
        except OSError:
            return []
        if size < self.off:
            raise Truncated(self.path)
        if size == self.off:
            return []
        with open(self.path, "rb") as fh:
            fh.seek(self.off)
            data = fh.read(size - self.off)
        self.off += len(data)
        lines = (self.part + data).split(b"\n")
        self.part = lines.pop()
        return [l.decode("utf-8", "replace").rstrip("\r") for l in lines]


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

    def __init__(self, logs_dir, rbn_index):
        self.dir = logs_dir
        self.rbn_factory = rbn_index
        self.lock = threading.RLock()
        self.version = 0
        self.reset()

    def reset(self):
        self.sessions = {}
        self.L, self.R = [], []        # Spot, chronological
        self.rbn = self.rbn_factory()
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
                lines = tail.read()
                if lines:
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
                             snapshot_windows(d)))
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
        else:
            lst = self.L if kind == "local" else self.R
            if lst and lst[-1].t > t:          # keep the lists sorted
                i = len(lst)
                while i > 0 and lst[i - 1].t > t:
                    i -= 1
                lst.insert(i, sp)
            else:
                lst.append(sp)
