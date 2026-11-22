/* dcw4_kernel.h — e04's DeepCW v2 narrow model (deepcw-engine
 * model_narrow.onnx, 0.2 M parameters) as one hand-written forward pass on
 * the CPU: no ONNX Runtime, no GPU. The network behind CW v4.
 *
 * The weights come from a blob that tools/deepcw_narrow_extract.py writes
 * from the pinned model file (sha256 fc80c4ba…, deepcw-engine 79104fb); the
 * blob is pinned too (SKIM_DCW4_BLOB_SHA256), records the model's sha256,
 * and is built into the engine as a GResource (weights/dcw4.gresource.xml).
 * The forward pass reproduces the model node for node:
 *  - the front end's QDQ: the input and the first ReLU quantised to uint8
 *    (zero point 0) and back, the conv weights int8 with per-channel scales,
 *    dequantised as ONNX DequantizeLinear does;
 *  - the two convs, GroupNorm over time (as InstanceNormalization), ReLU;
 *  - the projection, LayerNorm, the sinusoidal position code;
 *  - 3 macaron Conformer blocks at d 64 and the log-softmax head.
 * Its log-probs match ONNX Runtime's to about 1e-5 with the same greedy
 * text (gate dcw4-kernel). The encoder (dcw4_encoder.inc) runs on AVX2 +
 * FMA where the CPU has them, else on SSE2 (x86-64), else on plain arrays.
 *
 * Input: a tile [T][3] — per 16 ms frame the magnitudes of the line's
 * 12.5 Hz bin and its two neighbours, normalised as the model was trained
 * (noise_q90_log1p). Output: [ceil(T / 2)][42] log-probabilities, one CTC
 * frame per two input frames; the classes are SKIM_DCW4_ALPHABET, then the
 * word space (40) and the blank (41).
 *
 * A C port of SDRIAK's deepcw_narrow_fused.{h,cpp} (bubnikv/sdriak
 * 850a08d, GPL-3.0-or-later). The weights are e04's (MIT, Copyright (c)
 * 2026 e04): weights/deepcw-v4-narrow.NOTICE, and the blob's own "notice".
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_DCW4_BLOB_SHA256  "253ffb73161a09f3ee7d2c23d03106e4d64ecd8b846ac6a7ec7f48fe81820dc3"
#define SKIM_DCW4_MODEL_SHA256 "fc80c4baec72d756af1dbce9b70357a731b0b1a6b8306815bcfc61b6403c46e5"
#define SKIM_DCW4_BINS     3
#define SKIM_DCW4_CLASSES  42
#define SKIM_DCW4_SPACE    40
#define SKIM_DCW4_BLANK    41
/* class k < 40 is SKIM_DCW4_ALPHABET[k] */
#define SKIM_DCW4_ALPHABET ",./0123456789?ABCDEFGHIJKLMNOPQRSTUVWXYZ"

/* The encoder's instruction set. AUTO: the best this build and CPU have
 * (avx2, then sse on x86-64, else generic). */
typedef enum {
  SKIM_DCW4_ISA_AUTO = 0,
  SKIM_DCW4_ISA_GENERIC,
  SKIM_DCW4_ISA_SSE,
  SKIM_DCW4_ISA_AVX2,
} SkimDcw4Isa;

const char *skim_dcw4_isa_name(SkimDcw4Isa isa);
/* "auto", "generic", "sse", "avx2"; FALSE for another name */
gboolean    skim_dcw4_isa_from_name(const char *name, SkimDcw4Isa *isa);
/* What this build and CPU run, the best first; returns how many (≤ 3). */
guint       skim_dcw4_isas_available(SkimDcw4Isa out[3]);

typedef struct _SkimDcw4Net SkimDcw4Net;

/* The blob (verify: against SKIM_DCW4_BLOB_SHA256); NULL + error when it is
 * not this model's or isa does not run here. The blob is copied. */
SkimDcw4Net *skim_dcw4_net_new(const guint8 *blob, gsize len, gboolean verify,
                               SkimDcw4Isa isa, GError **error);
/* The built-in blob (always verified). */
SkimDcw4Net *skim_dcw4_net_new_builtin(SkimDcw4Isa isa, GError **error);
/* The process's one built-in net on the best ISA (SKIM_DCW4_ISA, else
 * auto), loaded on first use; NULL + *error (static text) if it cannot be.
 * Thread-safe. */
const SkimDcw4Net *skim_dcw4_net_shared(const char **error);
void         skim_dcw4_net_free(SkimDcw4Net *net);

/* The encoder skim_dcw4_net_run takes when not told otherwise. */
SkimDcw4Isa  skim_dcw4_net_isa(const SkimDcw4Net *net);

/* Output frames for T input frames (stride 2). */
static inline guint skim_dcw4_output_frames(guint T) { return (T + 1) / 2; }

/* tile [T][3] -> logp [skim_dcw4_output_frames(T)][42]. Thread-safe: each
 * calling thread keeps its own scratch (freed when the thread ends).
 * force: another available encoder than the net's (the gate compares them
 * all); one that does not run here runs the net's. */
void skim_dcw4_net_run(const SkimDcw4Net *net, const float *tile, guint T, float *logp,
                       SkimDcw4Isa force);

/* The model's input normalisation (model_narrow.onnx.json
 * "noise_q90_log1p"), in place on x[n]: the noise level is the 90th
 * percentile of x's magnitudes (the sorted value at floor(0.9 (n − 1)))
 * over sqrt(−2 ln 0.1), floored at peak · 1e-3 and 1e-5; then
 * x = log1p(x / noise). */
void skim_dcw4_normalize(float *x, gsize n);

G_END_DECLS
