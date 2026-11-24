# CW v4 — DeepCW v2 narrow on the CPU

CW v4 ("v4.0 DeepCW", config key `v4`) is e04's DeepCW v2 **narrow** model
run by a hand-written forward pass in C: no ONNX Runtime, no GPU, the
weights built into the engine. The plan and its decisions (Dan, 2026-11-22):
the narrow model, our own C kernel, CTC posteriors used to confirm calls
against MASTER.SCP (FIX tokens, as v3), a compute check that warns, the
governor pausing the least busy band under overload, v3.1 staying the
default until a live A/B. The old DeepCW backend keeps its key `deepcw` and
is labelled "DeepCW v1".

## Why the narrow model

From SDRIAK's measurements (bubnikv/sdriak,
`decoder_modules/cw_engine/DeepCW.md`, on a C++ port of this engine's
channelizer and v3), the narrow model at stitcher setting L 1 s, G 2 s,
S 3 s against v3:

- AWGN, 10–40 WPM: 2.5–5 dB more sensitive at 10 % CER;
- fading up to 1 Hz spread: 7–14 dB, floors 2.5–4× lower;
- impulse noise: far ahead; no false text in 20 h of noise;
- worse: 45 WPM, 10–30 Hz flutter, 2 Hz/s drift, the channel's edge with
  the default prototype, spurious spaces before digits at ≤ 12 WPM.

The wide v2 model costs ~20× more and reads worse under fast fading; v1
is AGPL and never bundled. The narrow model is MIT since deepcw-engine
79104fb (2026-10-27), so it is built in.

## Provenance

| What | From | Licence |
|---|---|---|
| model `model_narrow.onnx` | e04/deepcw-engine `79104fbe1ea0abd09eb27c6ebf9c6bab364d0d7f`, sha256 `fc80c4ba…46e5` | MIT, (c) 2026 e04 |
| `weights/deepcw-v4-narrow.bin` | `tools/deepcw_narrow_extract.py` on that file, sha256 `253ffb73…0dc3` (= SDRIAK's pin) | MIT, e04's weights; `weights/deepcw-v4-narrow.NOTICE` |
| `tools/deepcw_narrow_extract.py` | SDRIAK `850a08d965c0f8b78a05fd5ed209c00ff4cd83fb`, byte-identical | GPL-3.0-or-later |
| `src/engine/dcw4_kernel.c`, `dcw4_encoder.{h,inc}`, `dcw4_enc_{avx2,sse,generic}.c` | C port of SDRIAK `850a08d` `deepcw_narrow_fused.{h,cpp}`, `deepcw_narrow_kernel{.h,.inc,_avx2,_sse,_generic}.cpp` | GPL-3.0-or-later |

A new model = a new blob, a new `SKIM_DCW4_BLOB_SHA256` and
`SKIM_DCW4_MODEL_SHA256` in `dcw4_kernel.h`, and the extractor's `--check`
(a NumPy forward pass against ONNX Runtime; needs `onnx`, `numpy`,
`onnxruntime` in a venv).

## The port is the original, bit for bit

The C templates' row counts became `always_inline` functions with the rows
a constant at each call. Against SDRIAK's C++ built from the same commit
(same blob, same tiles; T 1–4200 frames, +6 to +30 dB per bin), on this
CPU: avx2, sse and generic each **622,440 log-probs, 0 differ**. What the
gate sees between encoders and against ONNX Runtime is therefore the
kernel's own float rounding, as SDRIAK measured it:

- log-probs up to ~4e-4 apart where float32 is ill-conditioned (a frame
  between two classes; SDRIAK: ONNX Runtime itself 3.4e-4 from float64);
- so the gate (`dcw4-kernel`) compares probabilities: |Δp| ≤ 1e-4 and the
  same greedy text. Measured: avx2 3.8e-5, sse 2.5e-5 against generic;
  ONNX Runtime 1.21 5.4e-5 against avx2 (`SKIM_DCW4_ONNX`).

Encoders run the same model but do not round alike (FMA, rcpps, Cephes exp
against libm), so a reference that must not depend on the CPU (the golden
gate) runs `SKIM_DCW4_ISA=generic`.

## Cost on hra2

`skimmer-dcw4-bench` (i5-1135G7, 2 threads, the live skimmer running), one
6 s window = 375 frames (L1 + G2 + S3), a channel's window every 3 s:

| encoder | ms a window | windows/s a thread | keyed channels a thread | ms CPU per keyed channel-second |
|---|---|---|---|---|
| avx2 | 1.94 | 515 | ~1,550 | 0.63 |
| sse | 5.19 | 192 | ~580 | 1.87 |
| generic | 15.5 | 64 | ~190 | 5.0 |

The network only; SDRIAK puts it at ~97 % of a keyed channel's cost (the
channel's DFT and the stitcher are the rest). Idle channels run no
inference (the keyed-line gate).

Idle channels store nothing either: the frames between two overs that no
window covered are the stitcher's gap, kept as its two ends, and a take
reads it cut to word_boundary + 1 frames (every frame of it reads the same
uncovered blank, and the blank run's length is only tested against the word
boundary) — byte-identical to upstream's zero row per frame (gate
dcw4-stream: its digest, 2 h gaps). Upstream's way took a row of 42 floats
for every frame since a channel's last over: on hra2 (v0.8.11–0.8.13, 6
bands × 768 channels) ~10 GB an hour until the OOM killer, 2026-11-23/24.

## From posteriors to what the engine reads

The rest of the engine (extractor, station table, RBN gate, learned gate)
reads text and one confidence per event; nothing per character. CW v4
(`decode_cw_v4.c`) keeps `decode.h` as it is and hands the network's
probabilities over through what is already there:

- **Words, whole.** The stitcher's committed characters gather into a word
  that goes out only when it closes: on a word gap, after 2.01 s of blank
  (the stitcher marks a gap only BETWEEN two characters, so an over's last
  word would otherwise wait for the next over), or on a TX hold's final
  window. One event = one word and its space.
- **Confidence** of an event = the mean posterior of its word's characters.
  The learned gate's `conf` features were fitted on v2's taps — off-scale
  for v4; the gate stays in shadow (a refit needs v4 taps).
- **The call reader** (`dcw4_callread.c`): a closed word with a digit and a
  letter is scored with CTC's forward sum against MASTER.SCP's calls
  within two edits (and, joined to the word before it, against the joined
  call: the narrow model tears a call before a digit at ≤ 12 WPM). A
  dictionary call that beats the raw reading and every other candidate by
  2 nats goes out as a FIX token (`WORD\x01CALL\x02 `). Only dictionary
  calls are ever written in — the RBN rule (M4 before M6). Its evidence:
  the stitcher keeps the committed frames' averaged log-probs for 12 s.
- **The over break** `· ` after 3 s of blank behind the last word (v3's
  mark; the extractor's runs, CQ window and despace scan key on it).
- **Words to the pane only**: `SKIM_DCW4_DROP_P` (a word whose weakest
  character is under it goes to `take_aux_text`) — off until measured.
- **Frequency and SNR per word.** The DeepCW v1 front end's offset is a
  frame EMA with a 0.3 s time constant: within a second of an over's end
  it follows the noise, and v4's words go out 2–5 s after the air — the
  dcw4-stream gate caught a +30 Hz tone reported at −7 Hz. v4 measures
  the offset on each keyed window (parabolic on the line bin's and its
  neighbours' keyed-frame sums) and the SNR only while the gate is open
  (a hangover window holds no line), and each word carries the values of
  the window that committed it.
- **Speed** from the keying (the DeepCW v1 dit histogram), the PARIS count
  over a committed stretch as the fallback: a short over never commits the
  6 characters over 2 s that count needs.

## Compute check and load guard

- **Compute check** (`skim_decode_cw_v4_bench`, `/api/v4bench`, Settings →
  Decoding): windows a second on the configured workers for 2 s, cached in
  `<state>/v4-bench.ini` per CPU model, encoder, workers and build. Capacity
  = windows/s × step (keyed channels carried in real time). Need = the
  week's most CW decoders active at once (stats `active`, summed over the
  bands) × 1.5 — the gate opens on lines that never make text too; 150 a
  CW band without history. Verdict: ok ≤ ½ capacity, tight ≤ capacity,
  short over it. It only warns: Save asks for a confirmation when the
  verdict is short and v4 is chosen.
- **Load guard** (`[decode] v4_guard`, default on; `governor.c`): a tick
  that finds its channel's last window still waiting is skipped — late.
  Over 5 % of a minute's ticks late for 3 of the last 5 minutes pauses the
  v4 band with the fewest spots a minute (the thermal guard's count), the
  next 10 min later if still late, one v4 band always stays; back LIFO
  after 20 min without a late minute. Its pauses are its own (`v4-load` on
  the page, amber), beside the thermal guard's.

## The golden gate

v4's jobs replay g20m-day and g40m-night with `SKIM_DCW4_ISA=generic`:
the encoders round differently (FMA, rcpps — which differs between Intel
and AMD — Cephes exp against libm), and a replay on avx2 and on generic
already differs in a spot's 0.1 kHz and timing. Generic costs ~10 min a
slice; two slices keep the gate's length near what it was.

## First replays (golden slices, inline, avx2; v3 = the golden reference)

| slice | v3 feed lines | v4 feed lines | calls only v3 | calls only v4 (in MASTER.SCP) |
|---|---|---|---|---|
| g20m-day (600 s) | 16 | 25 | YB2NFD | 8 (8) |
| g40m-rtty (JARTS, 600 s) | 35 | 80 | HS0ZEE LZ3ZZ R3R R6OG SM5NBE | 43 (40) |

No ground truth here: the three v4-only calls outside MASTER.SCP on the
contest slice (HS0ZFV — v3 has HS0ZEE —, K2Z, N2V) are likely busts. The
live A/B against CW Skimmer Server (tap-score) is what decides.

What the replays found and fixed before this:
- **Words closed an over late.** A reading left pending past the frozen
  point held the blank count, so the over's last word stayed open until
  the next over — its frames out of the 12 s history by then, read as
  certain blanks, and a SHORTER call won by ~27 nats (YT1SU → YT1S, RD4M →
  D4M). Now the word closes and the over breaks when the gate closes after
  its hangover, a reading needs every frame of its word, and the history
  is made on a channel's first over and let go at its end (52 KB a keyed
  channel — not 768 × 63 KB a band).
- **Torn calls.** Scoring the torn reading ("9E T") with its word gap
  against the joined one penalised a gap the model may never have held
  (the stitcher's 2.01 s blank rule makes gaps too): 9E T → M9T. A join
  now needs two pieces within 1 s, neither a call; the joined word in
  MASTER.SCP stands (E A1EYL → EA1EYL, the call v4 had lost), else a near
  call may win as for one word.
- Corrections on these two slices: 25 and 23, all but three joins.
