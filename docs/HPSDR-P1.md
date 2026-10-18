# HPSDR Protocol 1 source — Red Pitaya: protocol facts and measurements

The second IQ source next to TCI (`src/engine/hpsdr_p1.c`, selected with
`SkimPipelineConfig.source = SKIM_PIPELINE_SOURCE_HPSDR`, in the app under
Preferences → Radio → IQ source). It reads straight from a receiver that speaks
openHPSDR Protocol 1 over UDP. It was written for, and measured against,
**Pavel Demin's Red Pitaya `sdr_receiver_hpsdr`** (red-pitaya-notes,
`projects/sdr_receiver_hpsdr/server/sdr-receiver-hpsdr.c`). That is the server
Quisk's "Red Pitaya" profile (`hermes/quisk_hardware.py`, `use_rx_udp = 10`)
and CW Skimmer Server's HermesIntf talk to. There is no TCI server in front of
such a receiver, and sdr-for-linux does not drive it (on P1 it supports only
the Hermes Lite 2), so the skimmer drives the radio itself.

## Why a direct source rather than a bridge

A bridge (an HPSDR → TCI relay) would have kept the engine TCI-only. It would
also have added a second process and a WebSocket hop just to deliver one
receiver. The P1 client is ~500 lines of GLib + POSIX UDP. It hands the pipeline
the same callback shape as the TCI client (`iq, nframes, rate, centre`), so
nothing downstream changed. Multi-band skimming uses the same client with N
receivers, see below.

## Facts about the server (read from the source, confirmed live 2026-09-29)

- **Discovery** `EF FE 02` + 60 zero bytes → a 60-byte reply:
  `EF FE <2+streaming> mac[6] code(25) board(1) "R_PITAYA" nrx(8)`. Byte 2 is
  **3 while the server streams to anyone**. It does not say to whom, so a
  stream left running by our own crashed session looks exactly like Quisk's.
- **One client.** A start (`EF FE 04 01..03`) points EP6 at the start's
  sender address **and port**. Whoever streamed before simply stops receiving.
  The server has **no watchdog**: a stream to a dead client runs until someone
  sends a stop.
- **EP2 control and stop are accepted from ANY address.** A second client that
  keeps refreshing its control frames would retune the owner's receivers, and a
  stop from anyone kills the owner's stream. So after our stream is gone, the
  client sends nothing: no refresh, and no stop on free (gated by
  `hpsdr-client`).
- EP2 fields used (C0 with the MOX bit clear):
  - `C0=0x00`: `C1[1:0]` sets the rate, 0/1/2 = **48/96/192 kHz**. The server
    has no 384 kHz. `C4[5:3]` sets the number of receivers minus one.
  - `C0=0x04`: the RX1 frequency, **in Hz, big-endian**. RX2…RX7 are
    `0x06…0x10` and RX8 is `0x24`. The server itself turns Hz into a phase
    increment against its **nominal 125 MHz** clock, so the clock error ends
    up in every frequency (see below).
- EP6: 1032-byte packets, `EF FE 01 06 seq[4]` + 2 × (`7F 7F 7F C0..C4` +
  504 bytes). Each sample carries `I[3] Q[3]` per receiver (24-bit big-endian,
  signed) plus a 16-bit mic word. With one receiver that is 63 samples per
  frame and 126 per packet, so 192 kHz means ~1524 packets/s (~12.6 Mb/s).
- ADC input selection per receiver (IN1/IN2) is a command-line argument of the
  server on the RP. It cannot be set over the wire.

## Spectrum orientation — the wire is RF-inverted, ingest conjugates

Measured on the RP at 192.168.1.21, 2026-09-29, 96 kHz. Taking the EP6 data as
`I + jQ`, a +5 kHz centre step (14 040 → 14 045 kHz) moved every stable carrier
by **+10 kHz** (14 032 230 → 14 042 234, 14 050 441 → 14 060 516,
14 030 449 → 14 040 453 Hz). That is a mirror. After conjugating, the same
carriers kept their absolute frequency across both centres
(14 029 477/484, 14 047 770/766, 14 037 938/934 Hz). This matches the rest of
the HPSDR family: the raw DDC feed is RF-inverted, and sdr-for-linux
conjugates the same way before it puts IQ on TCI. `IQ_CONJ` in `hpsdr_p1.c` is
the one place this lives, and the `hpsdr-client` gate fails if it is flipped.

## Clock correction (ppm)

The server computes the DDC phase from a nominal 125 MHz, so a sampling clock
that runs `ppm` fast puts every signal `f·ppm·1e-6` Hz **low**. The client
sends `center / (1 + ppm·1e-6)` as the frequency word. The residual scaling of
the sample rate is ~0.02 Hz at ±6 kHz and is ignored.

Measured on the RP at 192.168.1.21, 2026-09-29, against the RWM carriers with
no correction: 4 996 kHz showed −19.2 Hz and 9 996 kHz showed −37.9 Hz. Both
give **+3.8 ppm**, and the error doubles with frequency, which marks a clock
error rather than a fixed offset. With `clock_ppm = 3.81` the 4 996 kHz carrier
sat at −1.4 Hz. Uncorrected, the error would reach ~107 Hz on 28 MHz, which is
more than the RBN tolerates. How to measure it: set 0, then compute
`ppm = (true − shown) / true × 10⁶` on a known carrier (RWM 4 996 / 9 996 /
14 996 kHz, or RBN spots of strong stations).

## Link budget — Wi-Fi

On the operator's Wi-Fi (−40 dBm, 144 Mb/s link rate) 48 and 96 kHz arrived
whole (0–0.3 % lost). 192 kHz lost **27–33 %** of its packets. The ceiling was
about 1100 packets/s, and the kernel reported `RcvbufErrors 0`, so the packets
never reached the machine at all. The client zero-fills sequence gaps to keep
stream time continuous for the decoders, counts them, and the status line shows
the percentage. 192 kHz wants a wired link. Eight receivers at 192 kHz would be
~77 Mb/s of UDP.

## Multi-band: skimmer-headless

`skimmer-headless` (`src/headless_main.c`, no GTK) starts one client with up to
8 receivers (`skim_hpsdr_client_start_multi`) and feeds each receiver into its
own pipeline (`SKIM_PIPELINE_SOURCE_EXTERNAL`, IQ via `skim_pipeline_push`).
All the pipelines spot into one telnet feed. Config lives in
`~/.config/skimmer-for-linux/headless.ini` (`[radio]`, `[bands]` in receiver
order, `[decode]`, `[feed]`, `[status]`). Status goes to a console table and to
a web page with `/status.json`.

- The server has **one rate for all receivers**. EP2 carries rate + receiver
  count and every receiver's frequency, two control words per packet.
- **The FFTW planner is not thread-safe.** Six pipelines build their channel
  banks on six engine threads from the first IQ block, and they crashed inside
  `fftwf`'s planner (2026-09-29). Every plan create/destroy in the engine now
  takes `skim_fftw_lock()` (`src/engine/fftw_lock.c`). The `channelizer` gate
  builds 200 banks on 8 threads, and without the lock it segfaults 3 runs in 3.
- Measured on the RP at 192.168.1.21, 2026-09-29, **wired**, with 6 × 96 kHz
  (160/80/40/30/20/15 m): ~3 700 packets/s (26 samples per packet at 6 RX),
  **0 % lost**, 96 kS/s per band, no queue drops. The whole process used
  70–85 % of one core on an i5-10210U (8 threads). One 96 kHz pipeline
  replays at ~40× real time.
- "Active decoders" on the status page counts channels decoding running text.
  The decoders emit a one-character fragment on noise every now and then:
  160 m at night gave 1 895 fragments in 20 s, most of them single
  characters. A per-channel character score with a 10 s decay and a threshold
  of 8 tells those apart from keyed CW at 2–3 characters per second.

## Tools

- `skimmer-hpsdr-probe <host> [rate] [center_hz] [seconds] [dump.cf32] [ppm]`
  prints the discovery reply, packets/s, losses, the effective rate and
  absolute spectrum peaks. It can also write cf32 + `.meta`, which
  `skimmer-replay` and `SKIM_IQ_FILE` read unchanged. It refuses a busy radio.
- The `hpsdr-client` gate (`src/hpsdr_test.c`) runs a mock of the server with
  the semantics above.
