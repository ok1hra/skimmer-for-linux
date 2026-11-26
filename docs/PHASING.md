# Two-antenna phasing for the skimmer: the design

Status: a design for work still ahead (Dan, 2026-11-25). Built so far
(2026-11-25): the combiner and its meters in the engine
(`src/engine/combiner.c`, `phase_meter.c`, gate `phase`) and P0's tool
(`skimmer-phase-bench`, gate `phase-bench`); the pipeline phases a band
when its config says so and is fed both antennas (`skim_pipeline_feed2`,
`skimmer-replay` with `SKIM_REPLAY_B`). The agent pairs two streams and
stamps them with an epoch and a sample index (P1, agent 0.4, not on the
board yet). skimmer-headless runs a band from both inputs with `ip=ab`
(P2): `[phase]`, the client's stamps, a pairer per band, status.json's
`phase`; the band's own IQ (waterfall, FT8/JS8/WSPR) is the band-level
combination (P4), and the waterfall's Φ switches phasing on and off live
(`POST /api/phase`). Tried on rp-f08908 (agent 0.4) 2026-11-26 with IN2
empty: both IPs paired, one epoch, coherence 0.00, gain 0 — MVDR gives A,
as it should. `[record]` still takes A. It is written so that another
agent can implement it phase by phase. Each phase ends with an offline gate, as the rest of the engine
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
it (diagonal loading) so that a near-singular R_n (one overwhelming
interferer, or B a copy of A) does not blow the weights up. The loading is
0.1 % of the trace, not 1 %: at 30 dB INR the trace is the interferer's,
and 1 % of it costs ~4 dB of null depth, 0.1 % ~1 dB (gate `phase`: B a
copy of A stays finite with no gain).

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

### Who decides between "enhance" and "null"? Nobody: one objective, two layers

There is no switch to choose between "enhance the signal" and "null the
interference". A decoder needs the highest **signal to interference plus
noise ratio** (SINR) at its input, and the max-SINR weight w ∝ R_n⁻¹ v
does both at once, in the proportion that pays:

- no interferer: R_n ∝ I, and the weight is MRC (enhancement);
- one interferer above the noise: R_n⁻¹ turns the weight towards its
  null, as far as the signal can afford;
- a weak signal next to an interferer: still the same formula. It may
  even lower the signal's power when it lowers the interference more,
  because the SINR is what decodes. Normalising the output noise keeps the
  levels the decoders and the gates expect.

What differs between signals is not the goal but **how well v and R_n can
be estimated**. A strong station gives v at once. A weak one does not:
from power alone, its marks cannot be told from the noise. Hence two
layers.

**Layer 1: the group's interference canceller, for everything.** Per group
of channels (a few kHz), R_n comes from the quiet channels, and the
weights null what dominates there. With no station known, the steering is
"no preference": antenna A, or the whitened sum. This layer runs on every
channel and on the band-level output. It lowers the floor everywhere, so
that weak stations rise out of it and get **detected** in the first
place. Detection, the waterfall and FT8/JS8/WSPR live on this layer.

**Layer 2: per decoder, decision-directed: we know what we decode.** Once
a decoder (CW v3/v4 on its channel, an RTTY pair) locks on a station, its
own decisions steer that channel's weights:

- The decoder's mark probability p(n) (CW: the keyed envelope / Viterbi
  path; RTTY: the tone decisions) gates the covariances in that very
  channel: R_s += α·p·x xᴴ during marks, R_n += α·(1 − p)·x xᴴ during
  spaces. R_n then holds exactly what disturbs this station at its
  frequency (in-channel QRM included). R_s − R_n is the station alone.
- Integrating over many marks (seconds; tens of seconds for a deep-weak
  station) gives v even where a power gate never could. That is where a
  weak station gains most.
- w = R_n⁻¹ v (max SINR), with the phase reference on antenna A and the
  output noise normalised as below.
- **Safety**: decision-directed loops can chase their own errors. Adapt
  slowly, weight by the decoder's confidence, fall back to layer 1 when
  the decoder loses lock or when the combined SINR estimate is not above
  layer 1's, and never let layer 2's weight leave the channel (each
  decoder steers only its own input).

So the channel in a group where a strong station, a weak one and an
interferer sit side by side gets: layer 1 for the whole group (the
interferer nulled), and each locked decoder's own refinement in its own
channel. The other channels keep layer 1. "One phaser per decoder input"
is exactly layer 2. Its cost is the same few MACs per channel sample, plus
a probability stream from the decoder to the combiner, which needs a small
hook in `decode.h` (an optional `mark_prob` output per sample block).
Implement layer 1 first (P3), then layer 2 (P3b).

**Built (2026-11-26).** `decode.h` `mark_prob`: CW v2 and v3 give each
sample's mark probability from their own soft envelope between their
space and mark levels while the lattice holds a station (−1 otherwise);
v4, DeepCW and RTTY do not yet. The combiner (`skim_combiner_mark`)
learns R_mark and R_space in the channel (p ≥ 0.75 / ≤ 0.25, τ 5 s); after
3 s of marks R_mark − R_space gives v, R_space⁻¹·v the weight, which
replaces layer 1's only while it promises 0.5 dB more SINR on the same
estimates, and lets go 30 s after the decoder lost the station.
`[phase] layer2` (default true); status.json's `phase.layer2` counts the
channels on it. What it is for, measured: with ONE strong interferer two
antennas have one null and nulling it fixes the weight's direction, so
layer 1 is already near the optimum there (a station 12° from a 40 dB
interferer: both ≈ 8 dB in theory; gate: layer 2 engages, decodes, no
worse). Its own case is the interference in the station's channel that
the group's quiet channels never see: as loud as the station, its own
signature — layer 1 −5.6 dB SINR, layer 2 +21.1 dB (gate `phase`, true
marks, measured on the components).

### What two antennas can and cannot do

Two antennas give two degrees of freedom **at each frequency**. Per
channel that means one null while the wanted signal is kept, or the
wanted signal maximised (MRC). Never two nulls in the same channel. What
the per-channel design adds over one band-wide weight (Linrad's Fixed, or
a weight applied in the FPGA):

- **Each station its own weight**: MRC/MVDR per channel lifts every
  station at once, each with its own arrival phase and amplitude
  (diversity against QSB and polarisation). A band-wide weight helps one
  direction and can hurt the rest.
- **Different interferers in different parts of the band**: each channel
  nulls whatever dominates its own neighbourhood (R_n is estimated in
  groups of a few kHz). Two sources that occupy different frequencies are
  both nulled, each where it is.
- **Not possible with two antennas**: two interferers on the same
  frequencies from different directions (e.g. two broadband noise sources
  both covering the whole band). The channel can null only one of them,
  and MVDR then finds the best compromise. More nulls at one frequency
  need more antennas (N antennas → N − 1 nulls), so a third input and
  another board.
- **Null depth** is limited by how well the estimate holds. Linrad's
  figure: a 1 % amplitude mismatch leaves about −40 dB. The interferer
  must also stand above the noise in the quiet channels to be learned at
  all.

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
a few times a second, not every sample. Measured on hra2 with
`skimmer-phase-bench` (a 48 kHz synthetic pair, 30 s): the second
channelizer costs 0.3 % of one core, the combiner with its three SNR
meters 0.8 %. Both scale with the rate (×4 at 192 kHz). The channelizer
is not what limits the number of phased bands; the board's DDCs are (8).

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

## Measuring what it gives

The combiner has both antennas' channels and its output at the same
instants, so A alone, B alone and the combination are compared on the same
stations at the same time — no second decoder (Dan, 2026-11-25: no shadow
decoder). `src/engine/phase_meter.c` runs three shadow SNR meters
(SNR_2500, each against its own band floor) on A, B and Y.

Locally (a station, a channel):

- **G = SNR_Y − max(SNR_A, SNR_B)**, the gain over the better antenna,
  where the channel carries a station; **G_A = SNR_Y − SNR_A**, the gain
  over today's skimmer (IN1 only).
- **share_B**: B's part of the output noise (which antenna "carried" it).
- **align = |vᴴ·u_n|²**: the station's signature against the noise's
  principal direction. Near 1, nothing can separate the two.
- Estimates from the combiner's own covariances, of the adaptive weight
  whatever the mode applies (**gain_est**, **drop_est**): in modes A, B
  and Fixed they show what phasing *would* give while the operator compares
  by ear.

Broadly (a noise group of 32 channels, the band):

- **|γ|²**, the coherence of the quiet channels, and **D_max =
  −10·log10(1 − |γ|²)**: the deepest null the interference allows.
- **drop**: the floor the weight takes off antenna A's, A's own signals
  kept at their gain — the interference suppression actually reached.
  drop / D_max is how much of the possible the estimate gets.
- A band summary a minute: medians of G, G_A, drop (and p90), the share of
  groups whose drop is ≥ 3 dB, |γ|², D_max, share_B, same_dir (the share of
  stations with align > 0.99).

What the numbers tell (gate `phase`, all against the synthetic truth within
1 dB): no interferer → G ≈ +3 dB on equal antennas, drop 0, |γ|² 0; a
coherent interferer → drop ≈ D_max; IN2 empty → |γ|² ≈ 0 and G ≈ 0 (the
page can say so); B a copy of A (a splitter) → |γ|² ≈ 1, same_dir ≈ 1 and
G ≈ 0 — drop then reads huge but means nothing, since every signal comes as
the noise does. The per-channel "stations" include a band-limited
interferer's edges (they look like signals against the quiet neighbours;
same_dir tells them); the statistics of stations therefore count the spots,
each with its channel's numbers.

Where they show:

- **A second line in the waterfall's spectrum** for a phased band: per
  channel, Gain (G where a station is) / Interference (drop) / Coherence,
  one byte a channel (0.25 dB) once a second over the waterfall socket.
- **A coarse waterfall in Statistics** (built: `src/headless/phase_hist.c`,
  `GET /api/phase_hist`, Statistics → Phasing): channel = pixel, row =
  minute, two planes (G, drop), 7 days by the minute and 90 days by the
  quarter hour, `<state>/phase-<band>.bin`, only for phased bands (≈ 30 MB
  a band at 768 channels); rows in modes A or B are estimates and
  drawn greyed. The band's summary of each row (medians of gain, gain over
  A, drop and its p90, coherence, D_max, B's share, stations) lives in the
  same file and is drawn beside it — not in stats.bin (no stats v9: the
  history concerns phased bands only, and stats.bin's 84 MB need no
  migration for it).
- The spots: SNR_A, SNR_B and G with each spot (the station table, the
  archive's extra columns, a tooltip in the DX cluster).

`skimmer-phase-bench A.cf32 B.cf32` prints the same numbers for a recorded
pair (rp-spike pair or `[record]`): summary lines in time for MVDR and MRC
side by side, the groups, the stations, the measured lag and the CPU.

## Settings and coexistence

- **Off by default.** A band without `ip=ab` is byte for byte as before:
  no second channelizer, no meters, no history file, nothing extra on the
  waterfall socket (the golden gate).
- `[bands] 20m=14045500 cw ip=ab [phase=mvdr|mrc|fixed|a|b]`: one receiver
  on IP A (IN1) and one on IP B (IN2) at one centre; `in=` not allowed on
  that line. `[phase]`: mode, layer2, tc_s, gate_db, history, wf_line.
  `[phase.<band>]`: the Fixed weight and its memories (the panel writes
  them).
- Refused by the config check (exit 2; greyed out in the form with the
  reason): no skimmer agent on the board, different rates on the two IPs,
  no free receiver on either IP (a phased band takes one on each: 8 at
  most), `ip=ab` with `@worker` (v1 phases locally only).
- Settings → Bands: the IP column offers "A+B Φ" (`ip=ab`; refused while
  the two IPs run two rates); a band's `phase=` is kept through the form.
- The waterfall's phasing panel (⚙ beside Φ; built): the mode (MVDR, MRC,
  Fixed, A, B), the Fixed weight as Linrad has it (gain dB and phase °,
  fine and coarse steps; "from MVDR" takes MVDR's band-level weight as the
  fixed one — adapt, then fix), three memories, Save: the band's phase=
  and `[phase.<band>]` (fixed_gain_db, fixed_phase_deg, m1..m3) into the
  config with a backup, no restart. In Fixed the line shows what the
  hand-set weight does; in A and B what phasing would give.
- Adding or removing `ip=ab` is a settings change (a restart). The mode and
  the Fixed weight change live from the panel (POST `/api/phase`,
  managers), saved to the config only on Save; the meters keep running in
  mode A, so the line and the history show what phasing would give.
- **The band never fails because of B**: IP B taken by another client, its
  stream lost, or the alignment lost (a new epoch) → the band decodes A,
  the weights reset, and phasing resumes when B is back and aligned. The
  status says which ("Φ → A (IP B busy)"). An empty IN2 needs no rule:
  MVDR degrades to A by itself.
- Thermal guard / v4_guard: a new first step — a phased band drops to A
  (stops its second channelizer) before any band is paused.
- Downstream: decoders, feed and statistics count the band once (spots
  from Y). Until the band-level W(f) (P4), FT8/JS8/WSPR and the waterfall
  take A; `[record]` writes the pair.

## What changes where

### The board (agent, `rp/agent/skimmer-rp.c`)

1. **Paired streams.** When one marked (SKRP) client asks for the same
   frequencies on both instances (A on IN1, B on IN2), the agent resets
   both FIFO sets in one register sequence (as `rp-spike pair` did, lag 0)
   and starts both EP6 threads at once.
2. **A sample index in the stream**, for marked clients only (built in
   agent 0.4: the wire format is in `docs/REDPITAYA.md`). Pavel's EP6
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
7. **Statistics**: see "Measuring what it gives" below.

## An FPGA of our own: what changes, what does not

The design runs on the stock bitstream, and nothing in it waits for an
FPGA of our own. It is built so that one can come in later behind the same
interfaces:

- **The sample index is an interface, not an implementation.** The
  skimmer aligns the two streams by (epoch, sample index) from the stream.
  Today the agent counts samples since the paired FIFO reset. A future
  bitstream would latch a hardware counter at the common reset. The client
  and the combiner do not change.
- **Adaptive weights make the NCO phase irrelevant.** On stock, the phase
  offset between the instances after a retune is unknown, and the weights
  absorb it in a second or two. An FPGA that resets both NCOs together
  would make the offset reproducible. That serves **Fixed** mode and
  memories (a manual null stays put across retunes) and saves the
  re-convergence after a retune. It is not needed for MVDR/MRC.
- **More phased bands, wider bands.** A phased band takes two DDCs, one on
  each IP. On stock, with agent 0.3, that means 8 phased bands at up to
  192–384 kHz. DMA in our own FPGA would make 16 × 384 kHz cheap for the
  ARM. Wider DDCs (768 kHz – 3 MHz) would phase a whole band in one go
  (10 m, 80 m). The skimmer side already works per band and per rate, so
  it only sees more or wider streams.
- **ADC overload per input.** It does not change the combining, but it
  tells when one antenna overdrives the ADC: an overdriven input ruins
  every null. Show it beside the coherence.
- **Combining in the FPGA (a hybrid, considered and not chosen as the
  base).** The skimmer would estimate a band-wide weight W (or a short
  FIR, for the frequency-dependent phase) and the FPGA would apply it
  before the DDC output. That sends one stream per band instead of two,
  halving the link load, the ARM load and the channelizers. But it gives
  up per-channel MVDR/MRC: one weight for the band can null one
  interferer, not lift every station. It is worth having **as an option**
  for bands that only need an interference null when the board runs out
  of DDCs or the PC out of CPU. The weights come from the same R_n
  estimate, through the agent (a register write), so it is an
  "apply here or apply there" switch, not another design.
- **What the FPGA cannot do** is the per-station part. It sees no signal,
  and that is why the base design combines in the skimmer.

## Phases, each with its gate

| | what | gate (offline) |
|---|---|---|
| P0 | **Measure first.** Record pairs on real air with `rp-spike pair` (longer, 96 kHz, two real antennas, or the antenna and its interference); `skimmer-phase-bench` (built) computes MRC and MVDR per channel, the groups' coherence and the measured lag | numbers: the SNR gain and null depth on real air (if both are small for the station's antennas, stop here) |
| P1 | agent: paired start, sample index, epoch — **built** (agent 0.4, `docs/REDPITAYA.md`; not on the board yet) | `rp-agent`: both sim streams carry one epoch and a contiguous index, the same index is the same instant (phase 0.000°, a sample off = 3.75°); an overflow on A starts a new epoch on both, aligned again; an unmarked stream keeps Pavel's C&C; a paired start beside it resets nothing |
| P2 | config `ip=ab`, `[phase]`, `phase=`; the client's paired start and stamps (`set_rx_iq_ix_cb`: blocks with epoch + index, loss filled by the index); a pairer per band in `source_hpsdr_p1.c` (same epoch, same index → `iq2_cb`; A's unmatched front and A while B is missing, late past 0.25 s or unstamped go alone); status.json `phase` — **built**. `[record]` writes the pair (`-b.cf32`, each .meta naming the other: replay and bench find B from A's file) | `rp-two-ip`: a phased band streams paired from both IPs (one epoch, IN1 at A, IN2 at B); the agent sim's noise (a function of the sample number, on both inputs) stays coherent end to end (\|γ\|² 0.99, nulled 47 dB — a pairer off by 4096 samples reads 0.00); IP B taken → IN1 alone, said so; B back → phased again; two rates refused. `config`: ip=ab and its refusals |
| P3 | `SkimCombiner` (MVDR/MRC/fixed, gating, phase reference, noise normalisation), `SkimPhaseMeter`, and the pipeline (`SkimPipelineConfig.phased`: B's channelizer, the combiner in the drain, the Y meter as the band's shadow SNR, B missing → A, `push2`/`feed2`, a snapshot for the page, the spots' SNR_A/SNR_B/gain) — **built** | gate `phase`: MRC ≥ 2.5 dB, one interferer nulled ≥ 20 dB, two in two halves each where it is, B a copy of A, empty IN2, mode A bit for bit, Fixed, the meters against the truth; the whole pipeline spots a CW station 8 dB under a common interferer on each antenna (neither alone does), B missing decodes A, mode A shows the estimates. **Golden byte-identical** |
| P4 | band-level W(f): waterfall, digi_rx, WSPR — **built** (`band_combine.c`: WOLA, √Hann, half overlapping, N = 2048 at 96 kHz; the weights per channel from the combiner's group canceller, MVDR towards e_A with no station steering, scaled to w_A = 1 so A's signals keep their gain and the interference leaves the floor; interpolated to every bin; A itself while the weights are A alone). The waterfall's Φ switches the band between its mode and A, the line over the spectrum shows gain / interference / coherence per channel (`GET /api/phase`) | gate `phase`: unit weights give A back exactly (2048 frames late), mode A changes nothing, MVDR takes a 30 dB interferer 18.6 dB off the band. Still to do: the FT8/WSPR gates on a synthetic pair |
| P5 | web panel, coherence trace, stats v9 | headless Chrome screenshots; stats migration test |
| P6 | live: one band phased against the same band single (skimmer-compare: spots, calls, SNR) | Dan's verdict on air |

## Open questions for P0

- (2026-11-25: IN2 is empty at the station; Dan will say when an antenna
  can go there — ask then, not before.) What does the station have on IN2? A second antenna (spaced how far, for
  which bands?), a noise antenna for nulling, or another polarisation?
  The answer sets the first mode to tune (MVDR for nulling, MRC for
  diversity).
- Does the phase offset between the two instances change at a retune? F0
  could not tell. With adaptive weights it does not matter, but Fixed mode
  would need re-tuning after each retune.
- The channelizer's real CPU cost per band, measured on this machine:
  that decides how many bands can be phased at once.
