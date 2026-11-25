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
- **Inputs live**, two ways. The manager's: `skimmer-rp ctl sel a|b MASK`
  (rp_manage's `sel`, kept in start.sh — what an external client such as
  SkimSrv gets). The skimmer's own, per stream, inside Protocol 1: the ADC
  assignment word of the multi-ADC Hermes boards (C0 = 0x1C, 2 bits a
  receiver, C1 RX1–4, C2 RX5–8 — RX8 beyond the standard's seven), sent
  only when the discovery showed the agent, with a start that carries
  `SKRP` in bytes 4–7. Only such a marked client's word counts (one that
  arrives before its start is kept for it); when its stream ends the
  manager's inputs come back. Another client's word is ignored: an
  external client's inputs are the manager's choice. Pavel's server never
  sees the word (`hpsdr_p1.c` sends it to the agent only).
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

## Both IPs for the skimmer (config, `source_hpsdr_p1.c`)

`[bands]` lines take `ip=b` (the band comes from IP B, mvl0, DDC 8–15) and
`in=2` (from the ADC input IN2): up to 8 bands an IP, 16 in all. IP A is
`[radio] host` as always (searched as before); once it streams, IP B starts
with the ip=b bands — its address from `[rp] b_host`, else from the agent's
discovery reply (bytes 0x1D–0x20: the other instance's IPv4), at `[rp]
b_rate` (default the radio's rate), with A's clock correction. B busy
(another client) or silent is retried every 3 s and never stops A or the
skimmer; it is taken over only with `--take-over` for that start. Every
band carries its own IP's rate (`SkimHdBand.rate`): the pipelines, the
waterfall (`hello.bands[].rate`), FT8/JS8/WSPR's dial coverage. A worker's
bands share one rate; every band on ip=b is refused (that is a single IP:
put B's address in `[radio] host`). Gate `rp-two-ip`: skimmer-headless on
the real agent in `--sim`.

The statistics (version 8) keep 16 bands and four series of the board:
FPGA °C (peak), its load (managers and the LAN only), Mb/s from each IP
(every client's, from the agent's tx bytes) — from the agent's status
every 10 s while it runs the board.

## 384 kHz

Since 2026-11-25 the config takes 384 kHz (`[radio] rate`, `[rp] b_rate`)
with 4 receivers an IP at that rate (`SKIM_HD_IP_RX_384`: F0's safe
figure; agent 0.3 may carry more, not measured live). The engine already
ran 384 kHz from TCI: 3072 CW channels a band. The cost is on the PC:
one band with CW and RTTY took **54 % of a core at 384 kHz against 8 % at
96 kHz** (hra2, the agent's sim tone, 20 s) — about 7×, more than the 4× of
the rate. Settings says so beside the option. Worth a profile before
anyone runs several bands at 384 kHz.

## The page

- **Settings → Radio**: Layout (`[rp] mode`: P1 = both inputs into one set
  of 16 receivers, each band its input; two antennas = IN1 → IP A, IN2 → IP
  B, the input then follows the IP), Rate IP B, Address IP B (empty = the
  agent names it).
- **Settings → Red Pitaya**: a diagram drawn from the form as edited (the
  inputs, the two receiver sets with their bands and rate, the IPs and who
  takes them — this skimmer, another client streaming, free), the board's
  state, Connect (the password field appears only when the board does not
  know the key), Install/Reinstall, Uninstall, "even while another client
  streams", and on an IP this skimmer takes no band from the inputs another
  client gets (`sel`, kept on the board).
- **Settings → Bands**: IP and input columns; receivers numbered per IP
  (A·RX1…, B·RX1…), 8 an IP; the Mb/s each IP and both together.
- **Statistics → Red Pitaya** (only with the agent, or its history): FPGA
  °C, uptime, load (not public), each IP streaming and its Mb/s (the
  client's address for managers only); FPGA °C and Mb/s A/B over time.
  status.json `redpitaya` carries it; `status_public` scrubs addresses and
  the load and memory.

## Managing the board from the skimmer (`src/headless/rp_manage.c`)

Settings (`/api/redpitaya`, managers) or `skimmer-headless --redpitaya
connect|probe|status|install|uninstall [HOST]` — the system's `ssh`, no SSH
library:

- **Connect.** The skimmer's own ed25519 key (`<state>/rp/id_ed25519`), the
  board's host key accepted on the first connect and checked after
  (`<state>/rp/known_hosts`; a re-imaged card needs that file removed), one
  ControlMaster connection kept 10 min (a probe then takes 0.6 s). When
  the board does not know the key, the root password is asked for once:
  ssh's askpass is the skimmer itself, fed over a unix socket — never in
  argv, the environment or a file, never stored — and that one run appends
  the key to `/root/.ssh/authorized_keys`.
- **Why no `lbu commit`.** The board runs from RAM; `lbu commit` would save
  the key, but also every other change pending in its /etc (rp-f08908 had
  some, `lbu status` 2026-11-25). Instead the install keeps the key on the
  card (`apps/skimmer_rp/authorized_keys`) and the skimmer's start.sh puts
  it back at each boot. Until the install, a reboot forgets the key: the
  password once more.
- **Probe.** EEPROM `hw_rev`, `uname -m`, Alpine's release, `/dev/xdevcfg`,
  Pavel's bitstream on the card, whether start.sh is ours, the agent's
  version, the stock servers, eth0's and mvl0's addresses — and Protocol 1's
  busy byte from each IP.
- **Install** (STEMlab 122.88-16 on Pavel's image only): the agent to
  `apps/skimmer_rp/` (sha256 checked after the copy) with a `stop.sh`, so
  that starting one of Pavel's apps from his web page stops it like any
  other; his start.sh kept once as `start.sh.pavel`; a start.sh that loads
  the same bitstream, makes mvl0 as his does and starts the agent with the
  inputs (`sel=` line); then that start.sh run. Another client streaming
  from either IP refuses it unless the manager confirms (`take_over`) —
  this skimmer's own stream does not count. A reinstall keeps the card's
  inputs and the first backup.
- **The board's SSH host keys.** Pavel's image makes new ones at every
  boot (only the RSA key is in its overlay — measured 2026-11-25: ed25519,
  ecdsa, dsa dated the boot), so a pinned host key fails after any reboot.
  The install copies `/etc/ssh/ssh_host_*` to `apps/skimmer_rp/hostkeys/`
  and the skimmer's start.sh puts them back (and restarts sshd) when they
  differ: once installed, the board keeps one identity. Before the install
  a reboot shows a new key: the skimmer refuses it with the key alone
  (`host_key_changed`) and trusts it only when the root password is typed
  again (its old entry removed from known_hosts first).
- **Socket paths.** ssh's master socket and the askpass's live in
  `<runtime>/skimmer-rp`, or `/tmp/skimmer-rp-<uid>` (0700, ours, no
  symlink) when that path would pass a unix socket's 108 bytes; neither:
  no kept master.
- **Verified on rp-f08908, 2026-11-25** as a new user would: uninstall,
  reboot (the key gone from RAM), a skimmer state with no key and no
  known_hosts, connect with the password, install, reboot — the agent
  started by itself, the same host key, the skimmer's key still accepted;
  SkimSrv (mvl0, 8 RX) and the released skimmer (eth0, 5 RX) streamed from
  the agent with no client change, 0 FIFO overflows, the agent ≈ 38 % of a
  core for 13 × 96 kHz. A rule: after development on the board, run this
  again before a release (Dan, 2026-11-25).
- **Uninstall**: start.sh.pavel back, the agent and its directory gone,
  Pavel's start.sh run. The key stays in RAM until the next boot.
- **Inputs** (`sel`): live through `ctl sel`, and into start.sh's `sel=`
  when kept.
- **Status**: `ctl status` every 10 s while the agent runs the board.

The gate `rp-manage` runs all of it against a fake board (a fake ssh, the
real agent in `--sim`).

## F0 — what the stock bitstream can do beyond Protocol 1

`rp/spike/rp-spike` (run from /tmp, refuses any instance whose stock server
still runs) measured on 2026-11-25 19:06–19:09 UTC, both stock servers
stopped for it (antenna on IN1, centre 7.300 MHz / 7.030 MHz):

1. **Nothing above 384 kHz.** Rate register 80, 40 and 20 still give
   384 000 S/s (the FIFO fills at the reg-160 pace) and the samples are
   garbage: −3.4 dBFS rms, peaks over full scale, a flat spectrum without
   the broadcast carriers reg 160 shows 60–70 dB over the floor. 160 is the
   decimator's minimum. A channel wider than 384 kHz needs our own FPGA
   build — there is no "extended-lite" on the stock bitstream.
2. **The two instances are coherent.** RX1 of both on IN1, one frequency,
   FIFOs reset back to back, three resets in a row: sample lag 0, phase
   A·conj(B) 39.9° / 39.7° / 40.3° (std ≤ 0.27° over 2 s, drift < 1°),
   levels equal to 0.01 dB. A constant, calibratable offset — enough for the
   future phasing of two antennas at ≤ 384 kHz. Not yet known: whether the
   40° survives a retune (the NCO phase accumulators are not reset; a
   calibration after each tune is the safe assumption).
3. **The ARM is the limit, not the link.** One core reads ≈ 3.1 M FIFO
   words/s (8 RX × 384 kHz = 100 %; 8 × 192 k = 44 %, 8 × 96 k = 27 %,
   8 × 48 k = 18 %, the polling loop alone ≈ 18 %) — the same at reg 80/40,
   since the FIFO runs at 384 k. The UDP ceiling of 1032-byte datagrams
   (sendmmsg, 25 a call) is ≈ 28 000 pkt/s = 240 Mb/s a core, ≈ 470 Mb/s
   with two senders on the two cores (to hra2 at 1 Gb/s, 0 lost).

What an instance (one thread, one core, as in Pavel's server) can carry —
P1 puts 2 × ⌊504 / (6·n + 2)⌋ samples of n receivers in a datagram:

| receivers × rate | datagrams/s | Mb/s | FIFO + send ≈ core |
|---|---|---|---|
| 8 × 96 kHz | 4 800 | 41 | 27 + 17 = 45 % |
| 8 × 192 kHz | 9 600 | 83 | 44 + 34 = 80 % |
| 4 × 384 kHz | 10 100 | 87 | 47 + 36 = 85 % |
| 8 × 384 kHz | 19 200 | 165 | 100 + 69 — does not fit |

So both IPs together: up to 16 × 192 kHz (≈ 165 Mb/s, both cores ≈ 80 %),
384 kHz only with ≤ 4 receivers on that IP. Settings must enforce it. Ways
to more, untried: UDP GSO (`UDP_SEGMENT`, kernel 5.4 has it) for the send
side; for the FIFO side only our own FPGA (DMA instead of word reads).

**Agent 0.3 (measured 2026-11-25 21:19–21:20 UTC, rp-spike `burst`,
`rdcost`, `blast … gso`):**

- **FIFO reads.** A NEON load of 128 or 256 bits at a FIFO's base pops 2
  or 4 samples: the same spectrum as 64-bit loads, no repeated word. LDM
  (32-bit beats) reads garbage. 8 RX × 384 kHz cost 81 % of a core with
  64-bit loads and 54 % with 256-bit ones. 8 × 96 kHz: 27 % polling every
  200 µs, 20 % sleeping until a 256-sample batch is due, 14 % with 256-bit
  loads on top.
- **Only the active FIFOs.** 5 RX instead of 8 at 96 kHz: 13 %.
- **UDP GSO** (one send a batch, cut into 1032-byte datagrams by the
  kernel). 200 Mb/s cost 39 % of a core instead of 86 % with sendmmsg. The
  ceiling rises from 240 to 580 Mb/s a core, 0 lost.
- **Live, both clients** (the skimmer's 5 RX + SkimSrv's 8 RX at 96 kHz):
  the board ~22 % busy (two cores) with 0.3, against ~43 % with 0.2.
  So both IPs at 8 × 384 kHz (≈ 330 Mb/s) now fit in the ARM's budget on
  paper; not yet run live.

What this means for the modes: the **extended mode on the stock bitstream**
is P1 with the input chosen per IP (IN1 → IP A, IN2 → IP B), 8 × ≤ 384 kHz
each — the agent already does that (`ctl sel`). Wider channels
(768 k–3 M a slot) wait for our own FPGA (F6b, Vivado).
