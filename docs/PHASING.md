# Two-antenna phasing for the skimmer: the design

Status: a design for work still ahead (Dan, 2026-11-25). Nothing of it is
built. It is written so that another agent can implement it phase by
phase. Each phase ends with an offline gate, as the rest of the engine
does (CLAUDE.md). Sources: Linrad's phasing window (SM5BSZ, "Linrad phasing
window v6", sm5bsz.com/linuxdsp/run/phasing/), the F0 measurements in
`docs/REDPITAYA.md`, and the standard results for two-element arrays
(maximal-ratio combining, MVDR). A choice made here rests on one of these;
where a number is a guess it says so.

## The question

The board has two coherent inputs (IN1, IN2: two antennas, or one
antenna's two polarisations) and up to 16 DDCs. Where should the two be
combined, so that each signal is as strong as it can be and the
interference (a local noise source, a broadcast splatter, a powerline) as
weak as it can be?

- **In the FPGA.** It knows nothing about the signals. A combination there
  is one fixed weight (or one filter) for the whole band, and it has to be
  steered from outside. This is Linrad's "Fixed" and an MFJ-1026 in
  hardware. It can null one interferer for the whole band. It cannot make
  every station strong at once, because each station arrives with its own
  phase and amplitude difference between the antennas.
- **Before each decoder, one phaser per signal.** Correct in principle, but
  it raises the question of how many phasers and what happens to the rest
  of the band.
- **Inside the decoder** (diversity: decode A and B and pick, or combine
  the soft decisions). That is 2–3× the decoder CPU, and it is still worse
  than combining first.

**Answer: combine in the skimmer, per channelizer channel, with adaptive
weights. Then every channel of the band is phased at once.** The
channelizer already splits each band into 768 channels of 125 Hz (CW; 384
× 250 Hz for RTTY). Run it on both antennas' IQ. Then, in every channel,
combine the two channel signals with the weights that are best for that
channel. "One phaser per signal" then costs a few multiply-adds per
channel sample: 2 complex MACs for the output and 3 for the covariance.
That is tiny next to the decoders. No channel is left unphased, and none is
phased for a station it does not carry. A band-wide interferer is nulled
in the same step (MVDR below). The decoders stay as they are: they get one
channel signal, as today.

The consumers that read the band's IQ rather than the channels (the
waterfall, FT8/FT4, JS8 and WSPR through `digi_rx`, the recorder) get a
band-level combination: per-frequency weights W(f) on an FFT of the band,
derived from the same estimates. See "Band-level output".

## Why adaptive weights need no calibration

The F0 measurement on the stock bitstream (2026-11-25): RX1 of both
instances on IN1, one frequency, FIFOs reset back to back. The result was
sample lag 0, a phase difference of 39.7–40.3° that repeated over three
resets, a standard deviation ≤ 0.27° over 2 s, and equal levels. The
phase offset after a retune is not known: the NCO accumulators are not
reset.

Adaptive weights estimate the relative gain and phase of each channel
from the data itself. A constant offset, wherever it comes from (cables,
NCO phase, the antennas' own delays, which grow linearly with frequency),
is part of what they estimate, and it falls out. What the design needs
from the hardware is only this:

1. **One clock**: the board's single ADC clock, which is a given.
2. **Sample alignment** between the two streams, known to within a
   sample. Lag 0 after a common reset was measured. A drift or an unknown
   offset would decorrelate the channels.
3. **Stable offsets** over the estimation window (seconds). Measured
   drift was < 1° over 2 s.

So the stock bitstream is enough. An FPGA of our own (NCO phase reset, a
hardware time stamp) would be a convenience, not a requirement.

## The combiner, per channel

Notation: in channel k, x(n) = [a(n), b(n)]ᵀ, the two antennas' complex
channel samples. The output is y(n) = wᴴ x(n).

### Estimates

Use exponentially averaged 2×2 covariances. The time constant T is 1 s to
start with, at the channel's sample rate.

- **R_s(k)**: covariance in channel k, updated only while the channel
  carries a signal (its power more than 6 dB over the band's noise floor:
  key-down for CW, both tones for RTTY), and held otherwise. CW keying
  would otherwise average the station with the noise between its
  elements.
- **R_n(k)**: covariance of the noise and interference near channel k,
  from the quiet channels of its neighbourhood: e.g. the 30 % lowest-power
  channels of a group of 32 (4 kHz), averaged, then interpolated across
  groups. A local broadband interferer dominates exactly these quiet
  channels. That is how R_n learns its spatial signature without being
  told. The phase of R_n varies slowly with frequency (the interferer's
  path difference is a delay). Groups of a few kHz keep up with that.
  Fitting a delay τ and a phase φ across the band is the refinement if
  groups prove too coarse.

### Weights

Use **MVDR / maximum SINR**, w(k) ∝ R_n(k)⁻¹ v(k), where v(k) is the
principal eigenvector of R_s(k) (the station's spatial signature).

Closed form for 2×2 Hermitian R = [[r11, r12], [r12*, r22]]: the largest
eigenvalue is λ = (r11 + r22)/2 + √(((r11 − r22)/2)² + |r12|²), and
v = [r12, λ − r11]ᵀ (or [λ − r22, r12*]ᵀ when that is better
conditioned), normalised. R_n⁻¹ for a 2×2 needs one division. Regularise
it (diagonal loading of 1 % of the trace) so that a near-singular R_n (one
overwhelming interferer) does not blow the weights up.

What this does in each case:

| case | result |
|---|---|
| no interferer, noise equal and independent on the two antennas | R_n ∝ I, w = v: **maximal-ratio combining**, up to +3 dB SNR, much more when one antenna fades (QSB, polarisation) |
| one strong local interferer | R_n⁻¹ puts a null on it in every channel, and the station keeps as much of its signal as the null allows |
| the station and the interferer from the same "direction" (identical signatures) | nothing can separate them. MVDR then keeps the station, so it degrades to no gain rather than to a loss |

Other modes, as Linrad has them:

- **Fixed**: a manual gain (dB) and phase (°) of B against A for the whole
  band, the same weight in every channel. Linrad's procedure carries over:
  equalise the noise levels, then turn the phase towards the null. Keep
  Linrad's "O" (swap the outputs: the orthogonal combination, which
  carries what the null removed), "T" (A alone / B alone) and three
  memories.
- **A only / B only**: for comparison and diagnosis.
- **Adapt then fix** (Linrad's semi-automatic way): take the band-wide
  weight MVDR found on the interferer and hold it.

### Continuity

The decoders see one stream, and the weights must not modulate it:

- **Phase reference.** Scale w so that its A component is real and
  positive. The output phase then follows antenna A's, continuously, as
  the weights move. RTTY (and any future PSK) needs this. CW v3's envelope
  does not, but it does not hurt.
- **Hold on key-up.** R_s is gated, so the weights do not move between
  elements.
- **Slow adaptation**: T ≈ 0.5–2 s, to be measured on recordings.
  Faster tracks QSB better but adds amplitude noise.
- **Normalise the output noise**, so that the band's noise floor, and
  with it the decoders' SNR thresholds and the gates, stays where it was
  with one antenna: divide by √(wᴴ R_n w).

### Cost

Per channel and sample: 2 complex MACs for y, about 3 complex
accumulations for R_s and R_n (gated), and a 2×2 eigen solve plus inverse
a few times a second, not every sample. Over 768 channels this is well
below 1 % of one core. The real cost is the **second channelizer** for the
band, roughly the same as the first. Measure it before deciding how many
bands to phase: `skimmer-replay` on a pair recording.

## Band-level output

For the waterfall, `digi_rx` (FT8, FT4, JS8), WSPR and `[record]`:

- **W(f) on an overlap-save FFT** of the band (N = 8192 at 96 kHz, as the
  waterfall uses), one complex weight per bin, applied as y(f) = W(f)ᴴ X(f).
- **Where W comes from**:
  - *interference null*: from R_n(f) alone, MVDR towards a "no preferred
    direction" signature, or simply the minimum-output-power combination
    in the bins where the coherence |γ(f)|² = |r12|²/(r11 r22) is high,
    smoothed over frequency;
  - *Fixed*: the manual weight;
  - *follow the channels*: W interpolated from the per-channel weights
    (smoothed so that the band output has no steps).
- **Per digital dial (optional, later)**: FT8, JS8 and WSPR each look at a
  3 kHz window. A small dedicated combiner per dial (the same MVDR on the
  window's FFT bins of each period) is cheap and gives every station of
  that window its own phasing in one go.

Diagnostics: the **coherence spectrum |γ(f)|²** across the band is the
picture of the common interference (near 1 = one source both antennas
hear alike: nullable). Show it in the waterfall as an extra trace, beside
A / B / combined selectors.

## What changes where

### The board (agent, `rp/agent/skimmer-rp.c`)

1. **Paired streams.** When one marked (SKRP) client asks for the same
   frequencies on both instances (A on IN1, B on IN2), the agent resets
   both FIFO sets in one register sequence (as `rp-spike pair` did, lag 0)
   and starts both EP6 threads at once.
2. **A sample index in the stream**, for marked clients only. Pavel's EP6
   C&C bytes are constant, and CW Skimmer reads some of them, so the
   index must not appear for anyone else. Put a 48–64-bit "samples since
   the paired reset" counter, plus an epoch number, in the C&C bytes of a
   frame. The client then aligns the two streams exactly, whatever the
   packet loss or start skew. A FIFO overflow on either side resets both
   and bumps the epoch, and the client restarts the alignment and the
   weights' averages.
3. **Rates**: both streams of a phased band at one rate (any of
   48–384 kHz). The ARM budget from F0 applies to each IP: with agent 0.3,
   8 × 192 kHz on each. That allows 8 phased bands at 96–192 kHz, using
   all 16 DDCs.
4. **An FPGA of our own (later, optional)**: NCO phase reset and a
   hardware sample counter would make the index exact by construction,
   and per-input ADC peak/overload registers would tell whether one antenna
   is overdriven. Not needed for the first version.

### The skimmer

1. **Config**: a band taken from both inputs, e.g. `20m=14045500 cw
   ip=ab` (IP A on IN1 and IP B on IN2 at one centre). `SkimHdBand`
   already has room for this (`stream[2]` was planned). `[phase]` holds the
   defaults: mode = mvdr | mrc | fixed | a | b, T, the gate in dB; and per
   band overrides with the Fixed weight and its memories.
2. **Source** (`source_hpsdr_p1.c`): start A and B as a pair, align by
   the sample index, and hand the band two IQ blocks with one timestamp
   (a new callback `iq2_cb`; a band from one input keeps `iq_cb`). Loss is
   zero-filled on both, aligned.
3. **Engine**: a `SkimCombiner` between two channelizers and the band's
   decoders (`src/engine/combiner.c`, GLib-only). `decode.h` is
   unchanged. The pipeline gets "two channelizers + combiner" in place of
   one channelizer when the band is phased.
4. **Band level**: `src/engine/band_combine.c` (overlap-save W(f)) feeds
   `waterfall`, `digi_rx`, `wspr` and `record`.
5. **Recording**: `[record]` writes both inputs (a .cf32 pair with one
   .meta). Recorded pairs are what every later measurement and gate runs
   on.
6. **Web**: a phasing panel per band, modelled on Linrad's window: gain
   dB, phase °, slow and fast arrows, O / T / − / I, three memories, and
   Adapt / Fixed per band. The waterfall gets A / B / combined and the
   coherence trace.
7. **Statistics** (stats v9): per phased band, the null depth (the noise
   floor with the combiner against the better single antenna, in dB), the
   SNR gain of the spots (combined against the better single, from the
   channels' estimates), and the share of spots each antenna "carried"
   (which component of w dominated).

## Phases, each with its gate

| | what | gate (offline) |
|---|---|---|
| P0 | **Measure first.** Record pairs on real air with `rp-spike pair` (longer, 96 kHz, two real antennas, or the antenna and its interference); a Python notebook computes MRC and MVDR per channel and band coherence | numbers: the SNR gain and null depth on real air (if both are small for the station's antennas, stop here) |
| P1 | agent: paired start, sample index, epoch | `rp-agent`: two sim streams carry identical indices; an overflow bumps the epoch on both |
| P2 | config `ip=ab`, source pairing and alignment, `[record]` pairs | `rp-two-ip`: a paired band gets aligned blocks (sim tone, same phase difference each run) |
| P3 | `SkimCombiner` (MVDR/MRC/fixed, gating, phase reference, noise normalisation) | synthetic: two antennas with independent Rayleigh fading plus one common broadband interferer with a fixed signature. MRC gains ≥ 2.5 dB with no interferer, MVDR nulls the interferer ≥ 20 dB, CW decodes not worse than the better antenna. **Golden byte-identical** for unphased bands |
| P4 | band-level W(f): waterfall, digi_rx, WSPR | the FT8/WSPR gates on a synthetic pair (decodes ≥ the better antenna) |
| P5 | web panel, coherence trace, stats v9 | headless Chrome screenshots; stats migration test |
| P6 | live: one band phased against the same band single (skimmer-compare: spots, calls, SNR) | Dan's verdict on air |

## Open questions for P0

- What does the station have on IN2? A second antenna (spaced how far, for
  which bands?), a noise antenna for nulling, or another polarisation?
  The answer sets the first mode to tune (MVDR for nulling, MRC for
  diversity).
- Does the phase offset between the two instances change at a retune? F0
  could not tell. With adaptive weights it does not matter, but Fixed mode
  would need re-tuning after each retune.
- The channelizer's real CPU cost per band, measured on this machine:
  that decides how many bands can be phased at once.
