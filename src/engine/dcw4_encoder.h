/* dcw4_encoder.h — the encoder of e04's DeepCW v2 narrow model as
 * hand-written loops: the shapes, the weight pointers, and one entry per
 * instruction set (dcw4_encoder.inc, compiled once per ISA by
 * dcw4_enc_{generic,sse,avx2}.c). dcw4_kernel.h is what the rest of the
 * engine uses; this header is its private half.
 *
 * Plain data and declarations only: dcw4_enc_avx2.c is compiled with
 * -mavx2 -mfma, so nothing here may be code the rest of the program shares.
 *
 * A C port of SDRIAK's deepcw_narrow_kernel.h (bubnikv/sdriak 850a08d,
 * GPL-3.0-or-later); docs/DEEPCW-V4.md has the provenance.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <stddef.h>

#define DCW4_D          64   /* model width                                   */
#define DCW4_FF        128   /* FFN and the conv module's pointwise_in        */
#define DCW4_HEADS       4
#define DCW4_HEAD_DIM   16
#define DCW4_LAYERS      3
#define DCW4_DW_KERNEL  15
#define DCW4_FEAT       24   /* front-end channels into the projection       */
#define DCW4_CLASSES    42
#define DCW4_CLASSES_PAD 48  /* the classifier padded to whole 16-lane blocks */

/* Every matrix is [in][out], the output channels contiguous. */
typedef struct {
  const float *ffn1_ng, *ffn1_nb, *ffn1_w1, *ffn1_b1, *ffn1_w2, *ffn1_b2;
  const float *attn_ng, *attn_nb, *qkv_w, *qkv_b, *out_w, *out_b;
  const float *conv_ng, *conv_nb, *pwin_w, *pwin_b, *dw_w, *dw_b, *pwout_w, *pwout_b;
  const float *ffn2_ng, *ffn2_nb, *ffn2_w1, *ffn2_b1, *ffn2_w2, *ffn2_b2;
  const float *final_ng, *final_nb;
} Dcw4Layer;

typedef struct {
  const float *proj_w, *proj_b, *proj_ng, *proj_nb;
  Dcw4Layer    layer[DCW4_LAYERS];
  const float *out_ng, *out_nb;
  const float *cls_w, *cls_b;          /* [64][48], [48]: columns 42.. zero */
} Dcw4Net;

/* Scratch floats an encoder needs for T1 encoder frames. */
size_t dcw4_work_floats(unsigned T1);

/* feat [T1][24] (the front end's output) and pe [T1][64] (the position
 * code) -> logits [T1][48] (before the log-softmax; columns 42.. unused).
 * work: dcw4_work_floats(T1) floats. */
typedef void (*Dcw4EncodeFn)(const Dcw4Net *net, const float *feat, const float *pe,
                             unsigned T1, float *logits, float *work);
/* Y [M][N] = X [M][K] W [K][N] + b (N a multiple of 16): the front end's
 * convs as im2col products. */
typedef void (*Dcw4MatmulFn)(const float *X, unsigned M, unsigned K, const float *W,
                             unsigned N, const float *b, float *Y);

void dcw4_encode_generic(const Dcw4Net *, const float *, const float *, unsigned, float *, float *);
void dcw4_encode_sse(const Dcw4Net *, const float *, const float *, unsigned, float *, float *);
void dcw4_encode_avx2(const Dcw4Net *, const float *, const float *, unsigned, float *, float *);
void dcw4_matmul_generic(const float *, unsigned, unsigned, const float *, unsigned, const float *, float *);
void dcw4_matmul_sse(const float *, unsigned, unsigned, const float *, unsigned, const float *, float *);
void dcw4_matmul_avx2(const float *, unsigned, unsigned, const float *, unsigned, const float *, float *);

/* The encoder is compiled in (an x86-64 build); the CPU check is the
 * caller's. */
int dcw4_avx2_compiled(void);
int dcw4_sse_compiled(void);
