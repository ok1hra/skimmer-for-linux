# The skimmer's own FPGA for the Red Pitaya: the design

Status: a design for work still ahead (2026-11-27, Dan's go to build it
step by step). Pavel Demin's `sdr_receiver_hpsdr_122_88` bitstream — the
one the managed board runs today (`docs/REDPITAYA.md`) — is the base;
what is kept from it, what changes and why is below, with the numbers
that decide it. Phasing's needs from the hardware are in
`docs/PHASING.md` ("Calibration").

## Why

Every HF band's CW and digital segments (no SSB), each from both inputs
for phasing, do not fit the stock bitstream:

| band | CW + digital (kHz) | width | rate |
|---|---|---|---|
| 160 m | 1810–1843 | 33 | 48 k |
| 80 m | 3500–3600 | 100 | 192 k |
| 60 m (if wanted) | 5351–5367 | 15 | 48 k |
| 40 m | 7000–7080 | 80 | 96 k |
| 30 m | 10100–10150 | 50 | 96 k |
| 20 m | 14000–14100 | 100 | 192 k |
| 17 m | 18068–18110 | 42 | 48 k |
| 15 m | 21000–21150 | 150 | 192 k |
| 12 m | 24890–24930 | 40 | 48 k |
| 10 m | 28000–28200 | 200 | 384 k |
| 6 m | 50000–50320 | 320 | 384 k |

(The usable part of a rate is CW Skimmer's: 45.5 / 91 / 182 / 364 kHz.)

10–11 bands × 2 inputs = **20–22 DDCs, each at its own rate**. The stock
bitstream has 16 DDCs in two banks of 8, one rate a bank (one per IP).
Per input that is 1.73 MS/s (3.46 MS/s both), 21 MB/s of 24-bit IQ,
≈ 175 Mb/s on the wire — the gigabit link (board eth0 → hra2 enp3s0) and
the ARM (agent 0.3: 580 Mb/s a core with GSO) carry it. The DDCs are the
limit, not the transport.

## What the stock design costs (measured 2026-11-27)

Pavel's repository at 71c3402a, Vivado 2025.2 on hra2, part
xc7z020clg400-1: built in 18 min, timing met (WNS +0.284 ns at
122.88 MHz). Whole design: LUT 32 098 / 53 200 (60 %), FF 54 617 (51 %),
**slices 89 %**, **BRAM 115 / 140 (82 %)**, DSP 134 / 220 (61 %). Vivado's
power estimate 2.84 W (PS 1.28 W, PL ≈ 1.36 W), junction 57.7 °C at 25 °C
ambient — the board's XADC read 87 °C on 2026-11-24.

Per bank of 8 receivers (the hierarchical report):

| block | count | LUT | FF | BRAM36 | DSP |
|---|---|---|---|---|---|
| CIC (6 stages, R programmable 160–2560, 24-bit in) | 16 (I and Q) | 778 each | 1 330 each | — | — |
| DDS (Pavel's `dds.v`: 2k-entry table + Taylor fix, 24-bit) | 8 | 117 each | 97 each | 1.5 each | 3 each |
| mixer (`dsp48`, 24 × 16) | 16 | — | — | — | 1 each |
| FIR ÷2 (16 channels, ≈ 470 taps, Fc 0.247) | 1 | 1 576 | 2 931 | 24 | 27 |
| FIFO (384 bit × 2048) | 1 | 45 | 74 | 21.5 | — |

So the fat parts are the **CICs** (78 % of the LUTs, 78 % of the FFs: a
programmable R up to 2560 makes each integrator 24 + 6·log2(2560) ≈ 92
bits wide) and the **FIFOs** (43 of the 115 BRAMs). Copying the chain to
22 receivers would need ≈ 44 k LUT, 75 k FF, 158 BRAM, 184 DSP — the BRAM
alone does not fit, and the slices would not either.

## The design

```
ADC IN1 ─┐                          per I and Q        8 channels each
ADC IN2 ─┼─ per slot: input select → NCO × mixer → CIC ÷8 ─→ CIC ÷20 ─→ FIR ÷2 ─ HB ÷2 ─ HB ÷2 ─ HB ÷2
         │            (sync + apply strobes)    (4 stages)  (6 stages)  (all slots, time-shared) │
         │                                                         384 k │  192 k   96 k   48 k │
         └─ peak / clip per input ─────────────┐                        └──┬──────┴──────┴──────┘
                                               └─ packer: each slot taken at its own rate,
                                                  tagged 64-bit words → DMA (ACP) → DDR ring
```

The bitstream is `fpga/projects/skimmer_rp_122_88/` (block design, our
HDL in `hdl/`, the filters' generators in `filters/`); `fpga/build.sh`
builds it and writes its cost into `tmp/NAME.summary`.

1. **Slots, not banks.** N slots (24; a multiple of 4), each with its own
   input (IN1/IN2), frequency and rate (48/96/192/384 kHz). An IP is no
   longer a bank: the agent maps clients to slots.
2. **NCO and mixer per slot, as Pavel's.** His `dds.v` (SFDR of a 24-bit
   table + Taylor correction) and one DSP per I and Q (24-bit product).
   Two changes: the phase accumulator is cleared by a **sync strobe**
   common to all slots (`skr_ctrl`: the run bit holds every NCO, CIC, FIR
   and the packer in reset; its rising edge is the sync), and a new phase
   increment takes effect only at an **apply strobe** (`skr_latch`), so a
   pair of slots changes frequency on the same clock. `NEGATIVE_SINE FALSE`:
   his current `dds.v` with TRUE gave the true orientation on the board,
   the stock (older) bitstream gives the RF-inverted one, and P1 clients
   and the skimmer's client expect that. A slot that is off feeds zero to
   its mixer, so its CICs stop toggling (power).
3. **Two CICs instead of one.** A single CIC ÷160 (6 stages) costs ≈ 800
   LUT and 900 FF per I or Q whether its rate is programmable (Pavel's)
   or fixed: its integrators run at 122.88 MHz and are 60–68 bits wide
   (S1, 2026-11-27: 24 slots this way filled 96 % of the slices). So: a
   **CIC ÷8 with 4 stages** per I and Q (122.88 → 15.36 MS/s, 28–36-bit
   integrators, ≈ 370 LUT), then a **CIC ÷20 with 6 stages shared by 8
   channels** (15.36 × 8 = 122.88 MS/s: one sample a clock, ≈ 970 LUT for
   all 8). The second CIC g takes channels g, g + 6, …, g + 42 (N = 24), so
   the six outputs side by side are channels 6k … 6k + 5 — the FIR sees
   them in order. The first CIC's alias rejection near each multiple of
   15.36 MHz is > 150 dB at ±192 kHz; its droop over the band is 0.001 dB.
4. **FIR ÷2, 768 → 384 kHz, all slots in one time-shared filter**
   (Xilinx FIR Compiler, 2N channels). 271 taps (`filters/fir.py`, Pavel's
   method: frequency sampling + Kaiser): flat to 0.002 dB with both CICs
   to 0.237 of 768 kHz (0.474 of the output rate), −96 dB from 0.263.
   Pavel's 444 taps had a narrower transition than CW Skimmer's 364 kHz
   needs.
5. **Three half-band ÷2 stages, all slots each** (384 → 192 → 96 →
   48 kHz; `filters/hb.py`, 239 taps, 121 not zero). Every slot is
   computed at every rate (cheap: the rates halve each stage), and the
   packer takes each slot from the stage that matches its rate. Passband
   0.474, stopband from 0.526 of the output rate (the alias of the edge
   lands outside CW Skimmer's usable 45.5 / 91 / 182 kHz), −96 dB as
   Pavel's FIR. A slot at 384 k takes the FIR's output directly. Every
   slot of one rate goes through the same stages: a pair (IN1, IN2, one
   frequency, one rate) has the same latency, sample for sample.
   **Coefficients 24 bits** (2 DSPs a multiply): with 18 bits the
   quantisation left the stopband at −80 dB. 23 fractional bits and the
   output at full precision (49 bits): bits 46:23 are the sample at unity
   gain, which the packer takes (saturated) and passes on as the next
   stage's 24-bit input. Losing 2 bits a stage to the filters' guard bits
   (the first build) would have put the quantisation 5 dB under the ADC's
   noise at 96 kHz.
6. **The sample index.** Every sample word carries its stage's output
   vector number since the sync (mod 128); for a slot at that stage's rate
   it is the sample index. The agent unwraps it, so two slots of one rate
   align exactly however and whenever they were set up — a pair needs no
   new sync. After every 48 kHz vector the packer writes a tick word (the
   48 kHz ticks since the sync).
7. **ADC peak and clip per input.** The largest |sample| of IN1 and IN2
   over 256 ticks (5.3 ms), and a clip flag (a sample at ±32767/−32768)
   — into the stream every 256 ticks and into status registers.
8. **The packer** (`hdl/skr_pack.v`, testbench `fpga/sim/skr_pack_tb.sv`)
   turns each stage's slots of its rate into 64-bit words, in memory byte
   order: 0..2 I and 3..5 Q (big-endian 24 bits, P1's own layout), 6 the
   slot (bits 4:0) and the vector number's bits 6:4, 7 = `01` + the rate
   (bits 5:4, 0 = 384 k … 3 = 48 k) + the vector number's bits 3:0. A
   tick word has tag 0xFF (0..5 the tick count, big-endian; 6 the
   flags), a peak word 0xFE (0..1 IN1's peak, 2..3 IN2's, 4 the clips in
   the window, 6 the flags); flags: bit 0 overflow (samples lost), bit 1
   IN1 clipped, bit 2 IN2 clipped, sticky until the next sync. A per-rate
   output shift (5 bits signed) is there for matching the stock levels.
   The four streams merge round-robin — the tags make the order between
   them irrelevant — into the DMA writer. No backpressure reaches the
   DSP: a full FIFO drops and sets the overflow flag, and the agent starts
   a new epoch (sync).
9. **The DMA and the ring.** `hdl/skr_ram_writer.v` (Pavel's
   `axis_ram_writer` with the cache attribute as an input) writes 128-byte
   bursts through the ACP; the ARM polls the burst counter (status word 2)
   and reads up to two bursts behind it. The board's image (Pavel's Alpine
   3.12, kernel 5.4.49) gives Linux 0x00000000–0x1DFFFFFF; the top 32 MB
   from 0x1E000000 is free and is what his `sdr_receiver_wide_122_88` used
   over `/dev/mem` — an uncached mapping, so the writes go non-cacheable
   (0011). Newer images have `/dev/cma`, mapped cached: there the writes
   go cacheable (1111) and coherent. The ring's base is a register
   (8 MB, ≈ 0.3 s at the full load).
10. **Identity.** Status word 0 holds `SKRF`, word 1 the version (7:0
    minor, 15:8 major, 23:16 the mixer's width, 31:24 N); the agent
    refuses a bitstream it does not know, and the stock one is told apart
    by its absence.
11. **OUT1 test tone (optional).** One more NCO into Pavel's
    `axis_red_pitaya_dac`: frequency, amplitude (0 = off). For the bench
    only (`docs/PHASING.md`, "Calibration").

Registers (cfg at 0x40000000, 32-bit words): 0 control (bit 0 run, 1
apply, 2 DMA on, 3 cacheable), 1 input (bit s = slot s on IN2), 2 enable,
3–6 the rate masks (384, 192, 96, 48 k), 7 the shifts (bits 4:0, 12:8,
20:16, 28:24), 8 the ring's base, 9 the tone's phase increment, 10 its
amplitude, 11 + s slot s's phase increment (f / 122.88 MHz · 2³²). Status
at 0x41000000: 0 `SKRF`, 1 version, 2 the writer's burst counter, 3 flags,
4 peaks (15:0 IN1, 31:16 IN2), 5–6 the tick count.

What is not in it: combining in the FPGA (PHASING.md keeps it as an
option, a later one), wider DDCs than 384 kHz, transmit.

## Measured (2026-11-27)

**Cost** (Vivado 2025.2, xc7z020clg400-1, N = 24, routed; stock = Pavel's
16 DDCs built from this tree, the same numbers as from his):

| | LUT | FF | slices | BRAM36 | DSP | power | WNS |
|---|---|---|---|---|---|---|---|
| stock, 16 DDCs | 32 098 (60 %) | 54 617 | 89 % | 115 (82 %) | 134 (61 %) | 2.84 W | +0.284 ns |
| 0.1: one CIC ÷160, 18-bit coefficients | 36 653 (69 %) | 58 036 | 96 % | 107 (76 %) | 174 (79 %) | 3.38 W | +0.194 ns |
| 0.2/0.3: two CICs, 24-bit coefficients | 30 932 (58 %) | 46 675 | 90 % | 122.5 (88 %) | 201 (91 %) | 3.20 W | +0.316 ns |

The DSPs are the DDS (3 each, 72), the mixers (48) and the filters (24 × 24
bits: 2 a multiply). Power is Vivado's estimate at default activity; the
board's XADC is the real figure (below).

**On the board** (rp-f08908, the bitstream loaded from RAM with the agent
stopped, `rp/spike/skrf`, stock reference captures with `rp-spike`
right before):

- Every slot at its exact rate, 48 000 ticks a second, no overflow, no
  unknown word, consecutive vector numbers (0.3's format), at 8 slots and
  at all 24 (4.22 MS/s: 6 × 384, 6 × 192, 4 × 96, 8 × 48 k).
- Two slots with one frequency, rate and input are **bit-identical**
  (48 k and 192 k checked): the time-shared filters keep the slots
  aligned sample for sample.
- **Orientation**: with `NEGATIVE_SINE FALSE` the stream is RF-inverted,
  exactly as stock: RWM (4996, 9996, 14996 kHz) at the same frequency to
  0.25 Hz after the client's conjugate (0.1 had TRUE and was mirrored).
- **Level**: at unity gain (shift 0) the noise floor equals stock's to
  0.1 dB at 48, 96 and 192 kHz (back-to-back captures, the same centre:
  −125.2 / −139.7 / −136.1 dBFS/Hz); 384 kHz within the band's own change
  between the captures. The shifts stay 0.
- **Reading the ring**: the ARM reads uncached DDR at 66 MB/s with 64-bit
  loads and 245 MB/s with NEON 32-byte loads; the full load is 34 MB/s.
  With NEON burst copies, reading and parsing all 24 slots took 46 % of a
  core and never fell behind more than 161 bursts (20 kB) of the 8 MB
  ring. With 64-bit loads (and saving every sample) it could not keep up.

## The ARM side (agent 0.6, `rp/agent/skimmer-rp.c`)

- **Which bitstream.** `--fpga auto` (the default) reads status word 0:
  `SKRF` = ours, anything else = stock. `--sim-skrf` simulates ours (the
  packer as a thread writing the ring), so every gate runs on both.
- **Protocol 1 on slots.** Both P1 instances stay as on stock (eth0, mvl0;
  discovery, EP2, EP6, the marker, pairing, the orphan stop): instance k
  takes slots 8k … 8k+7 at its rate, each receiver's frequency and input
  its slot's. CW Skimmer's HermesIntf and the released skimmer cannot tell
  (2026-11-27: SkimSrv on IP B at 8 × 96 k and the service on IP A ran
  11 min on the board, 0 overflows, 4 855 datagrams/s for SkimSrv, as on
  stock).
- **The reader** copies new bursts out of the ring with NEON and files
  each sample into its slot's buffer (65 536 samples) at its hardware
  index, unwrapped from the word's vector number. A P1 stream reads its
  receivers' buffers at one index range; a reset (start, overflow, a new
  rate or receiver count) re-anchors it — with its partner when paired —
  at a common index and starts a new epoch, as on stock. The ring running
  over (the reader 8 MB behind) means a new sync.
- **SKRS, the skimmer's stream** (P1 cannot mix rates): control `SKRC` to
  port 1025 of eth0 — start (n streams: id, Hz, rate 0 = 48 … 3 = 384 k,
  input 1|2), stop, keep-alive every second (5 s without one = stopped;
  no orphan can outlive its client). Data `SKRS` from that port, a
  datagram a stream: id, rate, flags (samples lost before it; IN1/IN2
  clipped since the sync), epoch (the syncs), the hardware index of its
  first sample, up to 240 samples of 24-bit I/Q, RF-inverted as P1
  (1 462 bytes); a slot's datagrams of a round in one GSO send. Two
  streams at one rate share the hardware timeline: one index, one
  instant. Slots come from 16 … 23 first, then from an instance no P1
  client streams on, which then answers busy and ignores a start.
  The marked discovery reply names the slots (byte 0x21) and the port
  (0x22–0x23). Engine client: `src/engine/rp_stream.c`.
- **The skimmer** (`[rp] stream=skrs`): every band one stream at its own
  rate (`rate=48|96|192|384` in `[bands]`, else `[radio] rate`) and input
  (`in=`), a phased band (`ip=ab` or `in=12`) two at its rate, lined up by
  the same pairer as two IPs; up to 16 bands in 24 slots. Settings →
  Radio → Stream, and a rate column in Bands.
- **Gates**: `rp-agent-skrf`, `rp-two-ip-skrf` (P1 on the simulated slots),
  `rp-stream` (16 SKRS streams at every rate, a pair aligned to 0.004°,
  beside P1, the stop and the keep-alive), `rp-skrs` (skimmer-headless with
  all HF + 6 m CW/digital segments, 40 m phased: |γ|² 1.000, nulled 46 dB).
- **Cost.** Reading and parsing the ring at 24 slots took 46 % of a core
  (skrf); the agent with 10 receivers of P1 (2 + 8 × 96 k) 17 % — agent
  0.3 on the stock FIFOs ≈ 22 % for 8.

## Heat

On 2026-11-27 morning: stock at the same load (agent 0.5, SkimSrv 8 ×
96 k + 2 × 96 k) read 80.6–81.6 °C on the XADC; ours (10 slots active)
went 79.7 → 83.4 °C in 11 min, still creeping — ≈ +2–3 °C. Vivado
estimates 3.20 W against stock's 2.84 W at default activity; idle slots
are silent. The board was at 87 °C on 2026-11-24 on stock; the Zynq's
commercial grade is specified to 85 °C junction. A fan on the heatsink is
cheap insurance, and with all 24 slots on it should be there.

## Steps

| | what | done when |
|---|---|---|
| S0 | `fpga/`: the parts of red-pitaya-notes the build needs (cores, scripts, cfg; MIT, pinned in `fpga/VENDOR.md`), Pavel's project built from our tree | **done** 2026-11-27: the same utilisation, timing met |
| S1 | the slot design synthesised and implemented | **done**: N = 24 (table above; the single CIC left 96 % of the slices, two CICs 90 %) |
| S2 | the new blocks in Verilog with xsim testbenches | **done** for the packer (`fpga/sim/skr_pack_tb.sv`: format, rates, rounding, shifts, saturation, vector numbers, ticks, clips; a wrong shift fails it). The Xilinx IP chain was checked on the board instead (S3) |
| S3 | the bitstream on the board from RAM, `rp/spike/skrf` | **done** (above): rates, identity of twin slots, orientation, the noise floor = stock's, the read cost. The OUT1 loop is still to do: it needs a cable at the bench |
| S4 | agent 0.6: the new bitstream, P1 on slots, SKRS | **done**; live on the board 11 min with SkimSrv and the service |
| S5 | the skimmer: SKRS client and source, config, Settings | **done** in code and gates; Statistics' clips/peaks are not shown yet |
| S6 | installed by the skimmer (the .bit shipped like the agent), the new-user install check, live with every band | after CQWW (2026-11-28/29), Dan's go |

Vivado runs on hra2 with the skimmer service stopped (and is restarted
after each run); never during a contest.
