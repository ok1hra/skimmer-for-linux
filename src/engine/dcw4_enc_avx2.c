/* dcw4_enc_avx2.c — the DeepCW v2 narrow encoder on AVX2 + FMA (8 lanes,
 * one __m256). This file alone is compiled with -mavx2 -mfma; dcw4_kernel.c
 * runs it only where the CPU has both. A C port of SDRIAK's
 * deepcw_narrow_kernel_avx2.cpp (bubnikv/sdriak 850a08d).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "dcw4_encoder.h"

#if defined(__AVX2__) && defined(__FMA__)

#include <immintrin.h>

typedef __m256 V8;

static inline V8 vzero(void) { return _mm256_setzero_ps(); }
static inline V8 vload(const float *p) { return _mm256_loadu_ps(p); }
static inline void vstore(float *p, V8 a) { _mm256_storeu_ps(p, a); }
static inline V8 vset1(float x) { return _mm256_set1_ps(x); }
static inline V8 vbcast(const float *p) { return _mm256_broadcast_ss(p); }
static inline V8 vadd(V8 a, V8 b) { return _mm256_add_ps(a, b); }
static inline V8 vsub(V8 a, V8 b) { return _mm256_sub_ps(a, b); }
static inline V8 vmul(V8 a, V8 b) { return _mm256_mul_ps(a, b); }
/* 1 / a: the 12-bit estimate and one Newton step, about 1 ulp */
static inline V8 vrcp(V8 a) {
  const V8 r = _mm256_rcp_ps(a);
  return _mm256_mul_ps(r, _mm256_fnmadd_ps(a, r, _mm256_set1_ps(2.0f)));
}
static inline V8 vmax(V8 a, V8 b) { return _mm256_max_ps(a, b); }
static inline V8 vfma(V8 a, V8 b, V8 c) { return _mm256_fmadd_ps(a, b, c); }

static inline float vhsum(V8 a) {
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

static inline float vhmax(V8 a) {
  __m128 s = _mm_max_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
  s = _mm_max_ps(s, _mm_movehl_ps(s, s));
  s = _mm_max_ss(s, _mm_movehdup_ps(s));
  return _mm_cvtss_f32(s);
}

static inline float ssqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }

static inline V8 vexp(V8 x) {
  /* n = round(x / ln 2), r = x − n ln 2 (in two parts, |r| ≤ ln 2 / 2),
   * e^r by Cephes' polynomial, 2^n added into the exponent bits. Below
   * −87.3 the result is ~1e-38, not 0: nothing it weighs adds up. */
  x = _mm256_min_ps(x, _mm256_set1_ps(88.0f));
  x = _mm256_max_ps(x, _mm256_set1_ps(-87.33654f));
  const V8 n = _mm256_round_ps(_mm256_mul_ps(x, _mm256_set1_ps(1.44269504088896341f)),
                               _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
  x = _mm256_fnmadd_ps(n, _mm256_set1_ps(0.693359375f), x);
  x = _mm256_fnmadd_ps(n, _mm256_set1_ps(-2.12194440e-4f), x);
  V8 y = _mm256_set1_ps(1.9875691500E-4f);
  y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f));
  y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));
  y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f));
  y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
  y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f));
  y = _mm256_fmadd_ps(y, _mm256_mul_ps(x, x), _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
  const __m256i e = _mm256_slli_epi32(_mm256_cvtps_epi32(n), 23);
  return _mm256_castsi256_ps(_mm256_add_epi32(_mm256_castps_si256(y), e));
}

#define DCW4_ENCODE dcw4_encode_avx2
#define DCW4_MATMUL dcw4_matmul_avx2
#include "dcw4_encoder.inc"

int dcw4_avx2_compiled(void) { return 1; }

#else

void dcw4_encode_avx2(const Dcw4Net *n, const float *f, const float *p, unsigned t, float *l,
                      float *w) { (void)n; (void)f; (void)p; (void)t; (void)l; (void)w; }
void dcw4_matmul_avx2(const float *x, unsigned m, unsigned k, const float *w, unsigned n,
                      const float *b, float *y) {
  (void)x; (void)m; (void)k; (void)w; (void)n; (void)b; (void)y;
}
int dcw4_avx2_compiled(void) { return 0; }

#endif
