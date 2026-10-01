#!/usr/bin/env python3
"""test-capture.py — offline test of capture-spots.sh, no SDR, no real feeds.

Runs three mock telnet feeds (the skimmer-for-linux dialect, the CW Skimmer
Server dialect and the RBN dialect) plus a mock /status.json, drives capture-spots.sh against
them with shortened timers, and checks the files against what the mocks
actually sent.

  run 1 — correctness: a 3000-spot burst, a line split by an 8 s pause,
          25 s of silence (keepalives must arrive), a server-side drop and
          the reconnect, 200 more spots; every spot must be in the file
          exactly once, in order, intact.
  run 2 — the alarms: a read-only data file (WRITE ERROR), a truncated
          file (WRITE CHECK FAILED), the recorder frozen for 45 s like a
          suspended machine, and a killed recorder process; the RBN
          feed is off (-b "") and must leave no trace.

  ./test-capture.py            exit 0 = everything passed
"""
import http.server
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "capture-spots.sh")
fails = 0


def check(ok, what):
    global fails
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        fails += 1


def spot(tag, seq):
    return ("DX de OK1HRA-#:   7027.0  T%s%05d       CW    20 dB  20 WPM  CQ      2130Z"
            % (tag, seq))


class Feed(threading.Thread):
    """One mock telnet feed. Connection 1: burst, split line, silence,
    server-side close. Connection 2: more spots, then stays open."""

    def __init__(self, tag, port, banner, prompt, hello, burst=3000, more=200):
        super().__init__(daemon=True)
        self.tag, self.banner, self.prompt, self.hello = tag, banner, prompt, hello
        self.burst, self.more = burst, more
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.sent = []            # every DX line sent, in order
        self.logins = []
        self.keepalives = 0
        self.conns = 0
        self.split_line = None
        self.stop = False

    def reader(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(1024)
            except OSError:
                return
            if not d:
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    self.keepalives += 1
                elif not self.logins or len(self.logins) < self.conns:
                    self.logins.append(line.decode())

    def send(self, c, text):
        c.sendall(text.encode())

    def run(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.conns += 1
            n = self.conns
            threading.Thread(target=self.reader, args=(c,), daemon=True).start()
            self.send(c, self.banner + self.prompt)          # no newline
            t = time.time()
            while len(self.logins) < n and time.time() - t < 5:
                time.sleep(0.05)
            self.send(c, self.hello + "\r\n")
            if n == 1:
                for i in range(self.burst):
                    line = spot(self.tag, i)
                    self.sent.append(line)
                    self.send(c, line + "\r\n")
                line = spot(self.tag, 90000)                  # split by a pause
                self.split_line = line
                self.sent.append(line)
                self.send(c, line[:25])
                time.sleep(8)
                self.send(c, line[25:] + "\r\n")
                time.sleep(25)                                # silence
                c.close()                                     # server drops us
            else:
                for i in range(self.more):
                    line = spot(self.tag, 100000 + i)
                    self.sent.append(line)
                    self.send(c, line + "\r\n")
                    time.sleep(0.01)
                while not self.stop:
                    time.sleep(0.2)
                c.close()


class Status(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"radio":"mock","bands":[]}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_capture(out, lport, rport, sport, seconds, bport=None, extra_env=None):
    env = dict(os.environ, CAPTURE_TICK="10", CAPTURE_READ_T="3")
    env.update(extra_env or {})
    return subprocess.Popen(
        [SCRIPT, "-t", str(seconds), "-o", out, "-k", "15", "-S", "20",
         "-l", "127.0.0.1:%d" % lport, "-r", "127.0.0.1:%d" % rport,
         "-b", "127.0.0.1:%d" % bport if bport else "",
         "-w", "http://127.0.0.1:%d/status.json" % sport],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
        start_new_session=True)


def files(out):
    f = {k: None for k in ("local", "remote", "rbn", "events", "status")}
    for n in os.listdir(out):
        for k in f:
            if n.startswith(k + "-"):
                f[k] = os.path.join(out, n)
    return f


def dx_lines(path):
    r = []
    for l in open(path, encoding="utf-8", errors="replace"):
        l = l.rstrip("\n")
        if l.startswith("#"):
            continue
        ts, _, body = l.partition("\t")
        if body.startswith("DX de "):
            r.append((ts, body))
    return r


def inhibitors():
    """capture-spots sleep inhibitors held right now (a real recording may
    be running next to the test — count, do not just look for the name)."""
    out = subprocess.run(["systemd-inhibit", "--list", "--no-pager"],
                         capture_output=True, text=True).stdout
    return out.count("capture-spots")


def run1():
    print("=== run 1 — correctness")
    inh0 = inhibitors()
    out = tempfile.mkdtemp(prefix="capture-test1-")
    lp, rp, bp, sp = free_port(), free_port(), free_port(), free_port()
    local = Feed("L", lp, "skimmer-for-linux telnet feed\r\n", "Please enter your call: ",
                 "OK1HRA-#: Hello OK1HRA, spots follow.")
    remote = Feed("R", rp, "Welcome to the Skimmer Server Telnet cluster port!\r\n",
                  "Please enter your callsign:", "OK1HRA de SKIMMER 2026-09-30 10:00Z CwSkimmer >")
    rbn = Feed("B", bp, "Welcome to the Reverse Beacon Network telnet server.\r\n",
               "Please enter your call: ", "Hello OK1HRA, this is RBN.")
    httpd = http.server.HTTPServer(("127.0.0.1", sp), Status)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    local.start()
    remote.start()
    rbn.start()
    p = start_capture(out, lp, rp, sp, 75, bport=bp)
    output, _ = p.communicate(timeout=150)
    local.stop = remote.stop = rbn.stop = True
    httpd.shutdown()
    f = files(out)
    for feed, name in ((local, "local"), (remote, "remote"), (rbn, "rbn")):
        got = dx_lines(f[name])
        bodies = [b for _, b in got]
        check(bodies == feed.sent,
              "%s: all %d spots recorded exactly once, in order (file has %d)"
              % (name, len(feed.sent), len(bodies)))
        check(feed.split_line in bodies,
              "%s: the line split by an 8 s pause is intact" % name)
        check(all(re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", t) for t, _ in got),
              "%s: every line carries a UTC timestamp" % name)
        check(feed.logins[:2] == ["OK1HRA", "OK1HRA"],
              "%s: logged in as OK1HRA on both connections (%s)" % (name, feed.logins))
        check(feed.keepalives >= 1,
              "%s: keepalives arrived during the silence (%d)" % (name, feed.keepalives))
        text = open(f[name]).read()
        check(text.count("\tconnected to") == 2 and "\tdisconnected" in text,
              "%s: drop and reconnect marked in the file" % name)
    ev = open(f["events"]).read()
    beats = re.findall(r"write (OK|FAILED)", ev)
    check(len(beats) >= 5 and set(beats) == {"OK"},
          "events: %d heartbeats, all 'write OK'" % len(beats))
    check("stopped — write check OK" in ev, "events: final write check OK")
    check("sleep inhibitor held" in ev, "events: sleep inhibitor held")
    check("WARNING" not in ev and "ERROR" not in ev, "events: no warnings or errors")
    check(re.search(r"local\s+3201 spots", ev) and re.search(r"remote\s+3201 spots", ev)
          and re.search(r"rbn\s+3201 spots", ev),
          "events: summary says 3201 spots per feed")
    check(re.search(r"remote up: .* · rbn up: \d+ spots", ev),
          "events: the heartbeat counts the rbn feed")
    n = sum(1 for _ in open(f["status"])) if f["status"] else 0
    check(n >= 3, "status snapshots: %d lines" % n)
    check(not any(x.startswith(".state") for x in os.listdir(out)), "state dir removed")
    check(inhibitors() == inh0, "sleep inhibitor released")
    print("  (files in %s)" % out)


def run2():
    print("=== run 2 — the alarms")
    out = tempfile.mkdtemp(prefix="capture-test2-")
    lp, rp, sp = free_port(), free_port(), free_port()
    local = Feed("L", lp, "skimmer-for-linux telnet feed\r\n", "Please enter your call: ",
                 "OK1HRA-#: Hello OK1HRA, spots follow.", burst=50, more=0)
    remote = Feed("R", rp, "Welcome\r\n", "Please enter your callsign:", "OK1HRA de SKIMMER >",
                  burst=50, more=0)
    local.start()
    remote.start()
    # no status server: the recorder must say it is unavailable, once
    p = start_capture(out, lp, rp, sp, 150)
    time.sleep(5)
    f = files(out)
    os.chmod(f["local"], 0o444)                   # appends now fail
    # the split line of connection 1 arrives at ~8 s → one write error
    time.sleep(14)
    os.chmod(f["local"], 0o644)
    with open(f["remote"], "w"):                  # truncate: lines vanish
        pass
    time.sleep(12)
    os.kill(p.pid, signal.SIGSTOP)                # "suspend" the supervisor
    time.sleep(45)
    os.kill(p.pid, signal.SIGCONT)
    time.sleep(12)
    kids = subprocess.run(["pgrep", "-P", str(p.pid), "bash"], capture_output=True,
                          text=True).stdout.split()
    if kids:
        os.kill(int(kids[0]), signal.SIGKILL)     # a recorder dies
    time.sleep(12)
    os.killpg(p.pid, signal.SIGINT)               # Ctrl+C
    output, _ = p.communicate(timeout=60)
    local.stop = remote.stop = True
    ev = open(f["events"]).read()
    check("local: WRITE ERROR" in ev, "read-only file → WRITE ERROR logged")
    check(re.search(r"local: WRITE CHECK FAILED — \d+ received lines could not be written", ev),
          "… and the heartbeat's write check fails for local")
    check(re.search(r"remote: WRITE CHECK FAILED — .* holds 0 lines, the recorder wrote \d+", ev),
          "truncated file → WRITE CHECK FAILED for remote")
    check(re.search(r"WARNING: 0 min 4\d s missing|WARNING: 0 min [3-5]\d s missing", ev),
          "45 s freeze → 'suspended or frozen' warning")
    check("ERROR: a recorder process" in ev, "killed recorder → reported")
    check(ev.count("status http://") == 1 and "unavailable" in ev,
          "missing status page → reported once")
    check("stopped — write check FAILED" in ev, "Ctrl+C → final write check FAILED (as it should)")
    check("WRITE" in output and "WARNING" in output, "alarms reach the terminal too")
    check(f["rbn"] is None and "rbn up" not in ev and "rbn    off" in ev,
          "-b \"\" → no rbn file, no rbn in the heartbeat")
    print("  (files in %s)" % out)


if __name__ == "__main__":
    run1()
    run2()
    print("\n%s (%d failure%s)" % ("FAIL" if fails else "PASS", fails, "" if fails == 1 else "s"))
    sys.exit(1 if fails else 0)
