/* dcw4_enc_sse.c — the DeepCW v2 narrow encoder on SSE2, x86-64's baseline
 * (CPUs without AVX2: Intel before Haswell, the Atom-class Celerons and
 * Pentiums up to Jasper Lake, AMD before Excavator). Each 8-lane vector is
 * two __m128; x86-64 has 16 vector registers, so the GEMM and attention
 * blocks are 2 rows. No FMA: a · b + c rounds twice. A C port of SDRIAK's
 * deepcw_narrow_kernel_sse.cpp (bubnikv/sdriak 850a08d).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "dcw4_encoder.h"

#if defined(__SSE2__)

#include <emmintrin.h>

typedef struct { __m128 a, b; } V8;

static inline V8 mk(__m128 a, __m128 b) { V8 r = { a, b }; return r; }
static inline V8 vzero(void) { return mk(_mm_setzero_ps(), _mm_setzero_ps()); }
static inline V8 vload(const float *p) { return mk(_mm_loadu_ps(p), _mm_loadu_ps(p + 4)); }
static inline void vstore(float *p, V8 x) {
  _mm_storeu_ps(p, x.a);
  _mm_storeu_ps(p + 4, x.b);
}
static inline V8 vset1(float x) { return mk(_mm_set1_ps(x), _mm_set1_ps(x)); }
static inline V8 vbcast(const float *p) {
  const __m128 d = _mm_load1_ps(p);
  return mk(d, d);
}
static inline V8 vadd(V8 x, V8 y) { return mk(_mm_add_ps(x.a, y.a), _mm_add_ps(x.b, y.b)); }
static inline V8 vsub(V8 x, V8 y) { return mk(_mm_sub_ps(x.a, y.a), _mm_sub_ps(x.b, y.b)); }
static inline V8 vmul(V8 x, V8 y) { return mk(_mm_mul_ps(x.a, y.a), _mm_mul_ps(x.b, y.b)); }
static inline V8 vmax(V8 x, V8 y) { return mk(_mm_max_ps(x.a, y.a), _mm_max_ps(x.b, y.b)); }
/* x · y + c, rounded twice */
static inline V8 vfma(V8 x, V8 y, V8 c) {
  return mk(_mm_add_ps(_mm_mul_ps(x.a, y.a), c.a), _mm_add_ps(_mm_mul_ps(x.b, y.b), c.b));
}

static inline __m128 rcp4(__m128 x) {
  const __m128 r = _mm_rcp_ps(x);
  return _mm_mul_ps(r, _mm_sub_ps(_mm_set1_ps(2.0f), _mm_mul_ps(x, r)));
}
static inline V8 vrcp(V8 x) { return mk(rcp4(x.a), rcp4(x.b)); }

static inline float hsum4(__m128 s) {
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
  return _mm_cvtss_f32(s);
}
static inline float hmax4(__m128 s) {
  s = _mm_max_ps(s, _mm_movehl_ps(s, s));
  s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
  return _mm_cvtss_f32(s);
}
static inline float vhsum(V8 x) { return hsum4(_mm_add_ps(x.a, x.b)); }
static inline float vhmax(V8 x) { return hmax4(_mm_max_ps(x.a, x.b)); }
static inline float ssqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }

static inline __m128 exp4(__m128 x) {
  /* n = round(x / ln 2) (cvtps2dq: to nearest, the default MXCSR mode),
   * r = x − n ln 2 (in two parts), e^r by Cephes' polynomial, 2^n added
   * into the exponent bits; ~1e-38 below −87.3 */
  x = _mm_min_ps(x, _mm_set1_ps(88.0f));
  x = _mm_max_ps(x, _mm_set1_ps(-87.33654f));
  const __m128i ni = _mm_cvtps_epi32(_mm_mul_ps(x, _mm_set1_ps(1.44269504088896341f)));
  const __m128 n = _mm_cvtepi32_ps(ni);
  x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(0.693359375f)));
  x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(-2.12194440e-4f)));
  __m128 y = _mm_set1_ps(1.9875691500E-4f);
  y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.3981999507E-3f));
  y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(8.3334519073E-3f));
  y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(4.1665795894E-2f));
  y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.6666665459E-1f));
  y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(5.0000001201E-1f));
  y = _mm_add_ps(_mm_mul_ps(y, _mm_mul_ps(x, x)), _mm_add_ps(x, _mm_set1_ps(1.0f)));
  const __m128i e = _mm_slli_epi32(ni, 23);
  return _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(y), e));
}
static inline V8 vexp(V8 x) { return mk(exp4(x.a), exp4(x.b)); }

#define DCW4_ENCODE dcw4_encode_sse
#define DCW4_MATMUL dcw4_matmul_sse
#define DCW4_ROWS 2
#include "dcw4_encoder.inc"

int dcw4_sse_compiled(void) { return 1; }

#else

void dcw4_encode_sse(const Dcw4Net *n, const float *f, const float *p, unsigned t, float *l,
                     float *w) { (void)n; (void)f; (void)p; (void)t; (void)l; (void)w; }
void dcw4_matmul_sse(const float *x, unsigned m, unsigned k, const float *w, unsigned n,
                     const float *b, float *y) {
  (void)x; (void)m; (void)k; (void)w; (void)n; (void)b; (void)y;
}
int dcw4_sse_compiled(void) { return 0; }

#endif
