#!/usr/bin/env python3
"""sink.py PORT SECONDS — the PC end of `rp-spike blast`: datagrams, Mb/s and
sequence gaps (bytes 4..7, big-endian) received, a line a second."""
import socket
import struct
import sys
import time

port, secs = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 << 20)
s.bind(("0.0.0.0", port))
s.settimeout(1.0)
got = lost = byts = 0
last = None
t0 = tick = None
while True:
    try:
        d = s.recv(2048)
    except socket.timeout:
        if t0 and time.monotonic() - t0 > secs:
            break
        continue
    now = time.monotonic()
    if t0 is None:
        t0 = tick = now
    seq = struct.unpack(">I", d[4:8])[0]
    if last is not None and seq != (last + 1) & 0xFFFFFFFF:
        lost += (seq - last - 1) & 0xFFFFFFFF
    last = seq
    got += 1
    byts += len(d) + 42
    if now - tick >= 1.0:
        dt = now - t0
        print(f"{dt:6.1f} s  {got / dt:8.0f} pkt/s  {byts * 8 / dt / 1e6:7.1f} Mb/s  "
              f"lost {lost} ({100.0 * lost / max(1, got + lost):.2f} %)", flush=True)
        tick = now
    if now - t0 > secs:
        break
dt = (time.monotonic() - t0) if t0 else 0
print(f"total: {got} datagrams, {byts * 8 / max(dt, 1e-9) / 1e6:.1f} Mb/s, lost {lost}")
