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
ADC IN1 ─┐                                         ┌─ 384 k ─┐
ADC IN2 ─┼─ per slot: input select → NCO × mixer ─ CIC ÷160 ─ FIR ÷2 ─ HB ÷2 ─ HB ÷2 ─ HB ÷2
         │            (sync + apply strobes)       (fixed)    (all slots, time-shared)  │
         │                                                    │ 384 k  192 k   96 k  48 k
         └─ peak / clip per input ──────────┐                 └───┬────┴──────┴─────┘
  64-bit sample counter (reset at sync) ────┴─ packer: each slot taken at its own rate,
                                               tagged 64-bit words → DMA → DDR ring
```

1. **Slots, not banks.** N slots (target 24, at least 22; step S1 sets it
   by synthesis), each with its own input (IN1/IN2), frequency and rate
   (48/96/192/384 kHz). An IP is no longer a bank: the agent maps clients
   to slots.
2. **NCO and mixer per slot, as Pavel's.** His `dds.v` (SFDR of a 24-bit
   table + Taylor correction) and one DSP per I and Q. Two changes: the
   phase accumulator is cleared by a **sync strobe** common to all slots,
   and a new phase increment takes effect only at an **apply strobe**
   (double-buffered), so a pair of slots changes frequency on the same
   clock. The sine's sign stays Pavel's (`NEGATIVE_SINE`): the stream's
   orientation is the stock one, which the client already conjugates.
   A slot that is off feeds zero to its mixer, so its CIC stops toggling
   (power).
3. **CIC with a fixed R = 160** (122.88 MHz → 768 kHz), 6 stages, M = 1.
   Fixed R and a narrower input (20–24 bits; S1 measures both) give
   44 + 20…24 = 64–68-bit integrators instead of 92, and no rate logic.
   The rate is chosen later, per slot. Expected ≈ 30 % fewer LUT/FF per
   CIC; S1 measures it.
4. **FIR ÷2, 768 → 384 kHz, all slots in one time-shared filter**
   (Xilinx FIR Compiler, 2N channels for I and Q), Pavel's coefficients
   (`filters/rx_fir_0.r`: CIC droop compensation for R = 160, passband to
   0.48 of the output rate). With 48 channels: ≈ 3 × his MACs, but the
   data per MAC fits the same RAMB18s better; S1 measures it.
5. **Three half-band ÷2 stages, all slots each** (384 → 192 → 96 →
   48 kHz). Every slot is computed at every rate (cheap: the rates halve
   each stage), and the packer takes each slot from the stage that matches
   its rate. Passband 0.474, stopband from 0.526 of the output rate (the
   alias of the edge lands outside CW Skimmer's usable 45.5 / 91 / 182 kHz),
   −96 dB as Pavel's FIR. A slot at 384 k takes the FIR's output directly.
   Every slot of one rate goes through the same stages: a pair (IN1, IN2,
   one frequency, one rate) has the same latency, sample for sample.
6. **Sample counter and stamps.** A 64-bit ADC-clock counter cleared at
   sync. Every output tick of 48 kHz (2 560 clocks) the packer writes a
   header word with the tick count; a slot at rate r has r/48 k samples a
   tick. The sample index of slot s is tick × r/48 k + j, the same for
   both slots of a pair by construction.
7. **ADC peak and clip per input.** The largest |sample| of IN1 and IN2
   over 256 ticks (5.3 ms), and a clip flag (a sample at ±32767/−32768)
   — into the stream every 256 ticks and into status registers.
8. **The packer and the DMA.** Each stage's output is filtered to the
   slots of its rate and turned into tagged 64-bit words: 3 bytes I and 3
   bytes Q in network order (P1's own layout), a slot byte, a tag byte.
   Header words carry tag 0xFF (tick count) and 0xFE (peaks, clips, the
   overflow flag). The four streams merge round-robin — the tags make the
   order between them irrelevant — into Pavel's `axis_ram_writer` (AXI
   HP0, 64-bit) writing a ring in DDR. The ARM polls the write pointer and
   reads what is new. No backpressure reaches the DSP: a ring overrun or a
   full FIFO sets the sticky overflow flag, and the agent starts a new
   epoch (sync) as it does today.
9. **The ring's place.** The board's image (Pavel's Alpine 3.12, kernel
   5.4.49) gives Linux 0x00000000–0x1DFFFFFF; the top 32 MB from
   0x1E000000 is free and is what his `sdr_receiver_wide_122_88` uses
   over `/dev/mem`. Newer images have `/dev/cma` instead. The ring's base
   address is a register, so the agent picks: 8 MB at 0x1E000000 on the
   old image, a CMA buffer on a new one. 8 MB is ≈ 0.3 s at the full load.
10. **Identity.** A status register holds `SKRF` and the bitstream's
    version; the agent refuses a bitstream it does not know, and the
    stock one is told apart by its absence.
11. **OUT1 test tone (optional).** One more NCO into Pavel's
    `axis_red_pitaya_dac`: frequency, amplitude, on/off. For the bench
    only (`docs/PHASING.md`, "Calibration").

What is not in it: combining in the FPGA (PHASING.md keeps it as an
option, a later one), wider DDCs than 384 kHz, transmit.

## The ARM side

- **The agent** keeps Protocol 1 on both IPs for CW Skimmer and Quisk: a
  P1 client with n receivers at rate r gets n free slots at r, and the
  agent builds EP6 frames from those slots' words. Busy when no slots are
  free.
- **The skimmer's stream** is the agent's own UDP format (P1 cannot mix
  rates): one packet carries one slot — `SKRS`, version, slot, rate,
  flags (clip IN1/IN2, overflow), epoch, the 64-bit index of the first
  sample, then up to 240 samples of 24-bit I/Q (1 460 bytes). Control in
  `SKRC` datagrams on the same port: the slot table (frequency, rate,
  input), start, stop, keep-alive. The skimmer's client is a sibling of
  `hpsdr_p1.c` with the same IQ callback; a phased pair arrives aligned
  by index.
- **Cost.** A word is one 8-byte load and a 6-byte store into its slot's
  packet: ≈ 3.5 M words/s at the full load, a few per cent of a core —
  against 54 % of a core for 8 × 384 k of FIFO reads over AXI GP today.
  Uncached reads of the ring are measured in S3.

## Heat

The board already reads 87 °C on stock (the Zynq's commercial grade is
specified to 85 °C junction). This design should draw about as much as
stock: fewer FFs toggling in the CICs, no 128-deep FIFO reads keeping a
core busy, idle slots silent — S1/S3 report Vivado's estimate and the
XADC on the board. A fan on the heatsink is cheap insurance whatever
happens here.

## Steps

| | what | done when |
|---|---|---|
| S0 | `fpga/`: the parts of red-pitaya-notes the build needs (cores, scripts, cfg; MIT, pinned in `fpga/VENDOR.md`), Pavel's project built from our tree | the same design builds, timing met, his utilisation |
| S1 | the slot design synthesised for N = 20, 22, 24 and CIC widths 20/24 (synthesis + implementation, no board) | N chosen; LUT/FF/BRAM/DSP/power in this file |
| S2 | the new blocks in Verilog (sync/apply NCO, packer, peaks, counter, tone) with xsim testbenches against a C model (`fpga/model/`): a tone's frequency, level and phase per rate, alias rejection, a pair's phase and index, the words' format | the testbenches pass |
| S3 | the full bitstream on the board, loaded into the FPGA from RAM (`/dev/xdevcfg`; a reboot or Pavel's .bit gives stock back), read with a test program: tones from the OUT1 loop, the ring's read cost, XADC | numbers in `docs/REDPITAYA.md` |
| S4 | agent 0.6: the new bitstream, the ring, P1 on slots, `SKRS`/`SKRC`; the `rp-agent` gate's simulator emulates the ring | gate green; CW Skimmer on IP B as before |
| S5 | the skimmer: the `SKRS` client and source, config (any band any rate, up to N slots, pairs), Settings and Statistics (peaks, clips) | gates green, golden byte-identical |
| S6 | installed by the skimmer (the .bit shipped like the agent), the new-user install check, live with every band | Dan's verdict |

Vivado runs on hra2 with the skimmer service stopped (and is restarted
after each run); never during a contest.
