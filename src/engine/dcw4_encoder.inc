/* dcw4_encoder.inc — the DeepCW v2 narrow encoder, written once against an
 * 8-lane vector API that the including file defines (V8, vzero, vload,
 * vstore, vset1, vbcast, vadd, vsub, vmul, vrcp, vmax, vfma, vexp, vhsum,
 * vhmax, ssqrt), together with DCW4_ENCODE and DCW4_MATMUL (the entries'
 * names) and optionally DCW4_ROWS: the rows of a GEMM block and the queries
 * of an attention block (6; an ISA whose 8 lanes take two of its 16
 * registers sets fewer, so that 4 · DCW4_ROWS accumulators fit). vrcp is
 * 1 / x to about 1 ulp (a division costs as much as the GEMM it follows).
 *
 * Activations are [time][channel] with the channels as the SIMD lanes. The
 * operations follow e04's model_narrow.onnx node for node and in its order
 * of rounding (bias after the product, the FFN's 0.5 after the bias, the
 * residual add last; q and k each scaled by 0.5, here q by 0.25, which is
 * exact), so the logits match ONNX Runtime's to float rounding (gate
 * dcw4-kernel).
 *
 * SDRIAK's C++ (deepcw_narrow_kernel.inc, bubnikv/sdriak 850a08d) takes the
 * row count as a template parameter; here every block function is
 * always_inline with the rows a constant at each call, so the compiler
 * unrolls the row loops and keeps the accumulators in registers as the
 * template did. Everything is static: each ISA's copy stays in its own
 * translation unit.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */

#ifndef DCW4_ROWS
#define DCW4_ROWS 6
#endif

#if DCW4_ROWS < 1 || DCW4_ROWS > 6
#error "the scores buffer holds 6 query rows (dcw4_work_floats)"
#endif

#define DCW4_INLINE static inline __attribute__((always_inline))
#define DCW4_UNROLL _Pragma("GCC unroll 8")

/* the depthwise conv's channels per pass, in 8-lane vectors: all 64 where
 * 8 accumulators fit the registers, else two passes of 32 */
#define DCW4_DW_VECS (DCW4_ROWS >= 6 ? DCW4_D / 8 : DCW4_D / 16)

enum { DCW4_STORE, DCW4_SILU, DCW4_RESIDUAL };

DCW4_INLINE unsigned dcw4_round16(unsigned n) { return (n + 15u) & ~15u; }

DCW4_INLINE V8 dcw4_sigmoid8(V8 x) { return vrcp(vadd(vset1(1.0f), vexp(vsub(vzero(), x)))); }

/* v · sigmoid(v) over n floats (n a multiple of 8). */
DCW4_INLINE void dcw4_silu_inplace(float *p, size_t n) {
  for (size_t i = 0; i < n; i += 8) {
    const V8 v = vload(p + i);
    vstore(p + i, vmul(v, dcw4_sigmoid8(v)));
  }
}

/* R rows of Y = X W + b (W [K][N], N a multiple of 16), then the epilogue:
 * STORE Y = v; SILU Y = v * sigmoid(v) (over the finished rows, outside the
 * register-blocked loop); RESIDUAL Y += v * scale. */
DCW4_INLINE void dcw4_gemm_rows(const int R, const int E, const float *X, unsigned ldx,
                                unsigned K, const float *W, unsigned N, const float *bias,
                                float *Y, unsigned ldy, float scale) {
  for (unsigned n = 0; n < N; n += 16) {
    V8 a0[6], a1[6];
    DCW4_UNROLL
    for (int r = 0; r < R; r++) {
      a0[r] = vzero();
      a1[r] = vzero();
    }
    const float *w = W + n;
    for (unsigned k = 0; k < K; k++, w += N) {
      const V8 w0 = vload(w), w1 = vload(w + 8);
      DCW4_UNROLL
      for (int r = 0; r < R; r++) {
        const V8 x = vbcast(X + (size_t)r * ldx + k);
        a0[r] = vfma(x, w0, a0[r]);
        a1[r] = vfma(x, w1, a1[r]);
      }
    }
    const V8 b0 = vload(bias + n), b1 = vload(bias + n + 8);
    DCW4_UNROLL
    for (int r = 0; r < R; r++) {
      float *y = Y + (size_t)r * ldy + n;
      V8 v0 = vadd(a0[r], b0), v1 = vadd(a1[r], b1);
      if (E == DCW4_RESIDUAL) {
        const V8 s = vset1(scale);
        v0 = vadd(vload(y), vmul(v0, s));
        v1 = vadd(vload(y + 8), vmul(v1, s));
      }
      vstore(y, v0);
      vstore(y + 8, v1);
    }
  }
  if (E == DCW4_SILU) {
    for (int r = 0; r < R; r++)
      dcw4_silu_inplace(Y + (size_t)r * ldy, N);
  }
}

/* Every row of a GEMM: blocks of DCW4_ROWS, then the rows left. */
DCW4_INLINE void dcw4_gemm(const int E, const float *X, unsigned ldx, unsigned M, unsigned K,
                           const float *W, unsigned N, const float *bias, float *Y,
                           unsigned ldy, float scale) {
  unsigned m = 0;
  for (; m + DCW4_ROWS <= M; m += DCW4_ROWS)
    dcw4_gemm_rows(DCW4_ROWS, E, X + (size_t)m * ldx, ldx, K, W, N, bias,
                   Y + (size_t)m * ldy, ldy, scale);
  const float *x = X + (size_t)m * ldx;
  float *y = Y + (size_t)m * ldy;
  switch (M - m) {
#if DCW4_ROWS > 5
  case 5: dcw4_gemm_rows(5, E, x, ldx, K, W, N, bias, y, ldy, scale); break;
#endif
#if DCW4_ROWS > 4
  case 4: dcw4_gemm_rows(4, E, x, ldx, K, W, N, bias, y, ldy, scale); break;
#endif
#if DCW4_ROWS > 3
  case 3: dcw4_gemm_rows(3, E, x, ldx, K, W, N, bias, y, ldy, scale); break;
#endif
#if DCW4_ROWS > 2
  case 2: dcw4_gemm_rows(2, E, x, ldx, K, W, N, bias, y, ldy, scale); break;
#endif
#if DCW4_ROWS > 1
  case 1: dcw4_gemm_rows(1, E, x, ldx, K, W, N, bias, y, ldy, scale); break;
#endif
  default: break;
  }
}

static void dcw4_gemm_store(const float *X, unsigned ldx, unsigned M, unsigned K, const float *W,
                            unsigned N, const float *bias, float *Y, unsigned ldy) {
  dcw4_gemm(DCW4_STORE, X, ldx, M, K, W, N, bias, Y, ldy, 1.0f);
}
static void dcw4_gemm_silu(const float *X, unsigned ldx, unsigned M, unsigned K, const float *W,
                           unsigned N, const float *bias, float *Y, unsigned ldy) {
  dcw4_gemm(DCW4_SILU, X, ldx, M, K, W, N, bias, Y, ldy, 1.0f);
}
static void dcw4_gemm_residual(const float *X, unsigned ldx, unsigned M, unsigned K,
                               const float *W, unsigned N, const float *bias, float *Y,
                               unsigned ldy, float scale) {
  dcw4_gemm(DCW4_RESIDUAL, X, ldx, M, K, W, N, bias, Y, ldy, scale);
}

/* LayerNormalization over the 64 channels of each row: the mean, the
 * variance from the deviations (two passes: E[x²] − E[x]² cancels where
 * the residual stream's mean is large), then (x − mean) · (1 / sd) · g + b.
 * y may be x. */
static void dcw4_layer_norm(const float *x, unsigned M, const float *g, const float *b, float *y) {
  for (unsigned m = 0; m < M; m++) {
    const float *p = x + (size_t)m * DCW4_D;
    float *q = y + (size_t)m * DCW4_D;
    V8 s = vzero();
    for (unsigned c = 0; c < DCW4_D; c += 8)
      s = vadd(s, vload(p + c));
    const float mean = vhsum(s) / (float)DCW4_D;
    const V8 vm = vset1(mean);
    V8 s2 = vzero();
    for (unsigned c = 0; c < DCW4_D; c += 8) {
      const V8 d = vsub(vload(p + c), vm);
      s2 = vfma(d, d, s2);
    }
    const float var = vhsum(s2) / (float)DCW4_D;
    const V8 inv = vset1(1.0f / ssqrt(var + 1e-5f));
    for (unsigned c = 0; c < DCW4_D; c += 8)
      vstore(q + c, vfma(vmul(vsub(vload(p + c), vm), inv), vload(g + c), vload(b + c)));
  }
}

/* R queries of one head: scores over every key, softmax, the weighted sum
 * of the values. kt [16][T1p] (the head's keys, transposed, zero padded to
 * T1p = T1 rounded up to 16), vh [T1][16] (its values), sc [R][T1p]. The
 * softmax's max comes out of the score loop (whole blocks of real keys
 * only), the exponentials and their sum in one pass, and the 1 / sum
 * scales the 16 outputs rather than the T1 weights. */
DCW4_INLINE void dcw4_attn_rows(const int R, const float *qkv, unsigned i, unsigned T1, unsigned h,
                                const float *kt, const float *vh, float *sc, float *out) {
  const unsigned T1p = dcw4_round16(T1);
  const unsigned full = T1 & ~15u;
  float q[6][DCW4_HEAD_DIM];
  for (int r = 0; r < R; r++) {
    for (unsigned d = 0; d < DCW4_HEAD_DIM; d++)
      q[r][d] = qkv[(size_t)(i + r) * 3 * DCW4_D + h * DCW4_HEAD_DIM + d] * 0.25f;
  }
  V8 vmx[6];
  DCW4_UNROLL
  for (int r = 0; r < R; r++)
    vmx[r] = vset1(-3.0e38f);
  /* 16 keys a block: 2R independent FMA chains */
  for (unsigned j = 0; j < T1p; j += 16) {
    V8 a0[6], a1[6];
    DCW4_UNROLL
    for (int r = 0; r < R; r++) {
      a0[r] = vzero();
      a1[r] = vzero();
    }
    for (unsigned d = 0; d < DCW4_HEAD_DIM; d++) {
      const float *k = kt + (size_t)d * T1p + j;
      const V8 k0 = vload(k), k1 = vload(k + 8);
      DCW4_UNROLL
      for (int r = 0; r < R; r++) {
        const V8 qd = vbcast(&q[r][d]);
        a0[r] = vfma(qd, k0, a0[r]);
        a1[r] = vfma(qd, k1, a1[r]);
      }
    }
    DCW4_UNROLL
    for (int r = 0; r < R; r++) {
      vstore(sc + (size_t)r * T1p + j, a0[r]);
      vstore(sc + (size_t)r * T1p + j + 8, a1[r]);
      if (j < full)
        vmx[r] = vmax(vmx[r], vmax(a0[r], a1[r]));
    }
  }
  float inv[6];
  for (int r = 0; r < R; r++) {
    float *s = sc + (size_t)r * T1p;
    float mx = vhmax(vmx[r]);
    for (unsigned j = full; j < T1; j++)
      mx = s[j] > mx ? s[j] : mx;
    const V8 m = vset1(mx);
    V8 vs = vzero();
    for (unsigned j = 0; j < full; j += 8) {
      const V8 e = vexp(vsub(vload(s + j), m));
      vstore(s + j, e);
      vs = vadd(vs, e);
    }
    float tail = 0.0f;
    if (full < T1) {
      for (unsigned j = full; j < T1p; j += 8)
        vstore(s + j, vexp(vsub(vload(s + j), m)));
      for (unsigned j = full; j < T1; j++)
        tail += s[j];
    }
    inv[r] = 1.0f / (vhsum(vs) + tail);
  }
  V8 o0[6], o1[6];
  DCW4_UNROLL
  for (int r = 0; r < R; r++) {
    o0[r] = vzero();
    o1[r] = vzero();
  }
  const float *v = vh;
  for (unsigned j = 0; j < T1; j++, v += DCW4_HEAD_DIM) {
    const V8 v0 = vload(v), v1 = vload(v + 8);
    DCW4_UNROLL
    for (int r = 0; r < R; r++) {
      const V8 p = vbcast(sc + (size_t)r * T1p + j);
      o0[r] = vfma(p, v0, o0[r]);
      o1[r] = vfma(p, v1, o1[r]);
    }
  }
  DCW4_UNROLL
  for (int r = 0; r < R; r++) {
    float *o = out + (size_t)(i + r) * DCW4_D + h * DCW4_HEAD_DIM;
    const V8 s = vset1(inv[r]);
    vstore(o, vmul(o0[r], s));
    vstore(o + 8, vmul(o1[r], s));
  }
}

/* Multi-head self-attention without a mask: qkv [T1][192] -> out [T1][64].
 * kt [16][T1p], vh [T1][16], sc [6][T1p]: one head's keys, values, scores. */
static void dcw4_attention(const float *qkv, unsigned T1, float *out, float *kt, float *vh,
                           float *sc) {
  const unsigned T1p = dcw4_round16(T1);
  for (unsigned h = 0; h < DCW4_HEADS; h++) {
    for (unsigned d = 0; d < DCW4_HEAD_DIM; d++) {
      float *k = kt + (size_t)d * T1p;
      const float *src = qkv + DCW4_D + h * DCW4_HEAD_DIM + d;
      for (unsigned j = 0; j < T1; j++)
        k[j] = src[(size_t)j * 3 * DCW4_D];
      for (unsigned j = T1; j < T1p; j++)
        k[j] = 0.0f;
    }
    for (unsigned j = 0; j < T1; j++) {
      const float *src = qkv + (size_t)j * 3 * DCW4_D + 2 * DCW4_D + h * DCW4_HEAD_DIM;
      vstore(vh + (size_t)j * DCW4_HEAD_DIM, vload(src));
      vstore(vh + (size_t)j * DCW4_HEAD_DIM + 8, vload(src + 8));
    }
    unsigned i = 0;
    for (; i + DCW4_ROWS <= T1; i += DCW4_ROWS)
      dcw4_attn_rows(DCW4_ROWS, qkv, i, T1, h, kt, vh, sc, out);
    switch (T1 - i) {
#if DCW4_ROWS > 5
    case 5: dcw4_attn_rows(5, qkv, i, T1, h, kt, vh, sc, out); break;
#endif
#if DCW4_ROWS > 4
    case 4: dcw4_attn_rows(4, qkv, i, T1, h, kt, vh, sc, out); break;
#endif
#if DCW4_ROWS > 3
    case 3: dcw4_attn_rows(3, qkv, i, T1, h, kt, vh, sc, out); break;
#endif
#if DCW4_ROWS > 2
    case 2: dcw4_attn_rows(2, qkv, i, T1, h, kt, vh, sc, out); break;
#endif
#if DCW4_ROWS > 1
    case 1: dcw4_attn_rows(1, qkv, i, T1, h, kt, vh, sc, out); break;
#endif
    default: break;
    }
  }
}

/* GLU over pointwise_in's 128 channels: x [T1][128] -> y [T1][64]. */
static void dcw4_glu(const float *x, unsigned T1, float *y) {
  for (unsigned t = 0; t < T1; t++) {
    const float *p = x + (size_t)t * DCW4_FF;
    float *q = y + (size_t)t * DCW4_D;
    for (unsigned c = 0; c < DCW4_D; c += 8)
      vstore(q + c, vmul(vload(p + c), dcw4_sigmoid8(vload(p + DCW4_D + c))));
  }
}

/* The depthwise conv (k 15, zero pad 7) and its SiLU: x [T1][64] -> y. */
static void dcw4_dwconv_silu(const float *x, unsigned T1, const float *w, const float *b,
                             float *y) {
  const unsigned half = DCW4_DW_KERNEL / 2;
  for (unsigned t = 0; t < T1; t++) {
    const unsigned j0 = t < half ? half - t : 0;
    const unsigned j1 = t + half >= T1 ? half + T1 - t : DCW4_DW_KERNEL;
    for (unsigned c0 = 0; c0 < DCW4_D; c0 += 8 * DCW4_DW_VECS) {
      V8 acc[DCW4_DW_VECS];
      DCW4_UNROLL
      for (unsigned c = 0; c < DCW4_DW_VECS; c++)
        acc[c] = vzero();
      for (unsigned j = j0; j < j1; j++) {
        const float *p = x + (size_t)(t + j - half) * DCW4_D + c0;
        const float *k = w + (size_t)j * DCW4_D + c0;
        DCW4_UNROLL
        for (unsigned c = 0; c < DCW4_DW_VECS; c++)
          acc[c] = vfma(vload(p + 8 * c), vload(k + 8 * c), acc[c]);
      }
      float *q = y + (size_t)t * DCW4_D + c0;
      DCW4_UNROLL
      for (unsigned c = 0; c < DCW4_DW_VECS; c++) {
        const V8 v = vadd(acc[c], vload(b + c0 + 8 * c));
        vstore(q + 8 * c, vmul(v, dcw4_sigmoid8(v)));
      }
    }
  }
}

/* The macaron FFN's half step: h += 0.5 · W2 SiLU(W1 LN(h)). */
static void dcw4_ffn(float *h, unsigned T1, const float *ng, const float *nb, const float *w1,
                     const float *b1, const float *w2, const float *b2, float *y, float *big) {
  dcw4_layer_norm(h, T1, ng, nb, y);
  dcw4_gemm_silu(y, DCW4_D, T1, DCW4_D, w1, DCW4_FF, b1, big, DCW4_FF);
  dcw4_gemm_residual(big, DCW4_FF, T1, DCW4_FF, w2, DCW4_D, b2, h, DCW4_D, 0.5f);
}

void DCW4_MATMUL(const float *X, unsigned M, unsigned K, const float *W, unsigned N,
                 const float *b, float *Y) {
  dcw4_gemm_store(X, K, M, K, W, N, b, Y, N);
}

void DCW4_ENCODE(const Dcw4Net *net, const float *feat, const float *pe, unsigned T1,
                 float *logits, float *work) {
  const unsigned T1p = dcw4_round16(T1);
  float *h   = work;                               /* [T1][64] the residual stream */
  float *y   = h + (size_t)T1 * DCW4_D;            /* [T1][64]   */
  float *z   = y + (size_t)T1 * DCW4_D;            /* [T1][64]   */
  float *big = z + (size_t)T1 * DCW4_D;            /* [T1][192]  */
  float *kt  = big + (size_t)T1 * 3 * DCW4_D;      /* [16][T1p]  */
  float *vh  = kt + (size_t)DCW4_HEAD_DIM * T1p;   /* [T1p][16]  */
  float *sc  = vh + (size_t)DCW4_HEAD_DIM * T1p;   /* [6][T1p]   */

  dcw4_gemm_store(feat, DCW4_FEAT, T1, DCW4_FEAT, net->proj_w, DCW4_D, net->proj_b, h, DCW4_D);
  dcw4_layer_norm(h, T1, net->proj_ng, net->proj_nb, h);
  for (size_t i = 0; i < (size_t)T1 * DCW4_D; i += 8)
    vstore(h + i, vadd(vload(h + i), vload(pe + i)));

  for (unsigned l = 0; l < DCW4_LAYERS; l++) {
    const Dcw4Layer *L = &net->layer[l];
    dcw4_ffn(h, T1, L->ffn1_ng, L->ffn1_nb, L->ffn1_w1, L->ffn1_b1, L->ffn1_w2, L->ffn1_b2, y, big);

    dcw4_layer_norm(h, T1, L->attn_ng, L->attn_nb, y);
    dcw4_gemm_store(y, DCW4_D, T1, DCW4_D, L->qkv_w, 3 * DCW4_D, L->qkv_b, big, 3 * DCW4_D);
    dcw4_attention(big, T1, z, kt, vh, sc);
    dcw4_gemm_residual(z, DCW4_D, T1, DCW4_D, L->out_w, DCW4_D, L->out_b, h, DCW4_D, 1.0f);

    dcw4_layer_norm(h, T1, L->conv_ng, L->conv_nb, y);
    dcw4_gemm_store(y, DCW4_D, T1, DCW4_D, L->pwin_w, DCW4_FF, L->pwin_b, big, DCW4_FF);
    dcw4_glu(big, T1, z);
    dcw4_dwconv_silu(z, T1, L->dw_w, L->dw_b, y);
    dcw4_gemm_residual(y, DCW4_D, T1, DCW4_D, L->pwout_w, DCW4_D, L->pwout_b, h, DCW4_D, 1.0f);

    dcw4_ffn(h, T1, L->ffn2_ng, L->ffn2_nb, L->ffn2_w1, L->ffn2_b1, L->ffn2_w2, L->ffn2_b2, y, big);
    dcw4_layer_norm(h, T1, L->final_ng, L->final_nb, h);
  }
  dcw4_layer_norm(h, T1, net->out_ng, net->out_nb, h);
  dcw4_gemm_store(h, DCW4_D, T1, DCW4_D, net->cls_w, DCW4_CLASSES_PAD, net->cls_b, logits,
                  DCW4_CLASSES_PAD);
}
