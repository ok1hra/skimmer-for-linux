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
