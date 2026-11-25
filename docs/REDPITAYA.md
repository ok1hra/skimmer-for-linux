# The managed Red Pitaya — the skimmer's own receiver

The skimmer can run its Red Pitaya itself instead of leaving it to whatever
image the board shipped with: it installs its agent (`rp/agent/skimmer-rp.c`)
on Pavel Demin's Alpine image, sets the board's mode and each receiver's ADC
input, and reads the board's state for the Statistics page. This file keeps
the board facts, the agent's protocol and the measurements behind the
choices. Protocol 1 itself is in `docs/HPSDR-P1.md`.

## The board (measured on rp-f08908, 2026-11-24)

- **STEMlab 122.88-16 SDR v1.1** (EEPROM `hw_rev=STEM_122-16SDR_v1.1`): a
  Zynq-7020 (2 × Cortex-A9 at 667 MHz, 220 DSP48, 140 BRAM36), 465 MB RAM, a
  dual 16-bit ADC at **122.88 MHz** (0–61.44 MHz in the first Nyquist zone),
  IN1 and IN2.
- Pavel Demin's Alpine 3.12 image (kernel 5.4.49-xilinx, 2020-06-28). It runs
  from RAM; the SD card (`/media/mmcblk0p1`, FAT) is mounted **read-only**.
  A change outside the card is lost at a reboot unless `lbu commit` saves it
  (`LBU_MEDIA=mmcblk0p1`; `/root` is not in the backup until
  `lbu include /root/.ssh`). No compiler on the board: the agent is built
  static for armhf (`rp/build-arm.sh`, a debian:12 podman container).
- `start.sh` on the card loads `sdr_receiver_hpsdr_122_88.bit` into the FPGA
  (`cat … > /dev/xdevcfg`, no reboot), makes the macvlan `mvl0` (eth0's MAC
  with byte 5 = FF) and starts two `sdr-receiver-hpsdr` servers:
  **eth0 = DDC 0–7, mvl0 = DDC 8–15**, each with its own IP, MAC and client.
  One bitstream serves both: loading another stops both IPs' streams.
- The FPGA's XADC read 87 °C on 2026-11-24 (`in_temp0_raw + offset × scale`);
  the Zynq-7020's commercial grade is specified to 85 °C junction.

## Register map of the stock bitstream (from Pavel's server, MIT)

| | eth0 instance | mvl0 instance |
|---|---|---|
| FIFO reset (bit 0) | cfg + 0 | cfg + 40 |
| rate (u16, CIC decimation) | cfg + 4 | cfg + 44 |
| input select (u8, bit r = RX r on IN2) | cfg + 6 | cfg + 46 |
| phase increments (u32 × 8, f / 122.88 MHz · 2³⁰) | cfg + 8 | cfg + 48 |
| FIFO fill (u16 × 8, 32-bit halves) | sts + 12 | sts + 28 |
| FIFOs (u64 reads pop one I/Q sample) | 0x40010000 + r·0x2000 | 0x40020000 + r·0x2000 |

cfg = 0x40001000, sts = 0x40000000. The rate register is the CIC's
decimation; a ÷2 FIR follows: 1280/640/320/160 = **48/96/192/384 kHz** (the
122.88 MHz server has 384 kHz, unlike the 125 MHz one HPSDR-P1.md was
written from). The input select is a plain register: the stock server
writes it once from its command line, the agent writes it whenever asked.
The stock EP6 headers are constant — no ADC overflow bit reaches the client.

## The agent (`rp/agent/skimmer-rp.c`)

One process replaces both stock servers on the same bitstream.

- **Protocol 1 as Pavel's, on each IP**: discovery, start/stop from anyone,
  EP2 (rate, receivers, frequencies), EP6 1032-byte packets built the same
  way. CW Skimmer's HermesIntf and Quisk cannot tell it from the stock
  server.
- **The marker**: a discovery with `SKRP` in bytes 4–7 of its padding gets
  `SKRP`, version major/minor, mode (0 = P1), the input mask and the
  instance (0 = eth0, 1 = mvl0) at bytes 0x14–0x1C. Byte 3 stays zero: it
  is part of the command word, and Pavel's server answers only `EF FE 02 00`.
  A plain discovery gets the stock reply byte for byte. The engine's client
  always asks with the marker (`SkimHpsdrInfo.agent`).
- **Inputs live**: `skimmer-rp ctl sel a|b MASK`.
- **Its own orphan stop**: Protocol 1 has no watchdog and SkimSrv sends its
  EP2 once, so "no control for N s" cannot mean "dead". A dead client's port
  answers each datagram with ICMP port unreachable; the socket has
  IP_RECVERR, and a stream that hears only those for `--orphan-s` (10 s)
  is stopped. A live client never sends ICMP to its own stream.
- **`skimmer-rp ctl status`**: one JSON line — board, agent version, mode,
  FPGA °C, load, memory, uptime, and per instance: streaming, client
  address/port, since, rate, receivers, input mask, frequencies, packets,
  FIFO overflows, orphan stops, the last FIFO reset (CLOCK_MONOTONIC ns),
  tx bytes/packets and link speed. The control socket is
  `/run/skimmer-rp.sock` (root only); the skimmer reads it over SSH.
- **`--sim`** replaces /dev/mem with synthetic FIFOs (receiver r: a tone
  (r+1) kHz above its centre, −12 dBFS on IN1, −26 dBFS on IN2), so the
  `rp-agent` gate runs the real agent against the engine's client on the
  build machine.

## F0 — what the stock bitstream can do beyond Protocol 1

`rp/spike/rp-spike` (run from /tmp, refuses any instance whose stock server
still runs) answers, before the extended mode is designed:

1. rate register 80 / 40 (768 kHz / 1.536 MS/s): valid IQ? can the ARM
   drain the FIFOs?
2. the two instances on one frequency after a back-to-back reset: a constant
   phase and a known sample offset? (the future phasing of two antennas)
3. the board's UDP ceiling (`blast` + `sink.py`).

Results: not measured yet — they need both instances idle.
