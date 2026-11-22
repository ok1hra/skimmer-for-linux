/* dcw4_enc_generic.c — the DeepCW v2 narrow encoder on plain arrays of 8
 * floats: every CPU, and the reference the other encoders are held to
 * (gate dcw4-kernel). Built with -ffp-contract=off, so a · b + c rounds
 * twice wherever it is compiled. A C port of SDRIAK's
 * deepcw_narrow_kernel_generic.cpp (bubnikv/sdriak 850a08d).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "dcw4_encoder.h"

#include <math.h>

typedef struct { float v[8]; } V8;

#define LANES(expr)                       \
  V8 r;                                   \
  for (int i = 0; i < 8; i++) r.v[i] = (expr); \
  return r

static inline V8 vzero(void) { V8 r = { { 0, 0, 0, 0, 0, 0, 0, 0 } }; return r; }
static inline V8 vload(const float *p) { LANES(p[i]); }
static inline void vstore(float *p, V8 a) { for (int i = 0; i < 8; i++) p[i] = a.v[i]; }
static inline V8 vset1(float x) { LANES(x); }
static inline V8 vbcast(const float *p) { return vset1(*p); }
static inline V8 vadd(V8 a, V8 b) { LANES(a.v[i] + b.v[i]); }
static inline V8 vsub(V8 a, V8 b) { LANES(a.v[i] - b.v[i]); }
static inline V8 vmul(V8 a, V8 b) { LANES(a.v[i] * b.v[i]); }
static inline V8 vrcp(V8 a) { LANES(1.0f / a.v[i]); }
static inline V8 vmax(V8 a, V8 b) { LANES(a.v[i] > b.v[i] ? a.v[i] : b.v[i]); }
static inline V8 vfma(V8 a, V8 b, V8 c) { LANES(a.v[i] * b.v[i] + c.v[i]); }
static inline V8 vexp(V8 a) { LANES(expf(a.v[i])); }
#undef LANES
static inline float vhsum(V8 a) {
  return ((a.v[0] + a.v[4]) + (a.v[2] + a.v[6])) + ((a.v[1] + a.v[5]) + (a.v[3] + a.v[7]));
}
static inline float vhmax(V8 a) {
  float m = a.v[0];
  for (int i = 1; i < 8; i++) m = a.v[i] > m ? a.v[i] : m;
  return m;
}
static inline float ssqrt(float x) { return sqrtf(x); }

#define DCW4_ENCODE dcw4_encode_generic
#define DCW4_MATMUL dcw4_matmul_generic
#include "dcw4_encoder.inc"

size_t dcw4_work_floats(unsigned T1) {
  const size_t T1p = (T1 + 15u) & ~15u;
  return (size_t)T1 * (3 * DCW4_D + 3 * DCW4_D) + T1p * (2 * DCW4_HEAD_DIM + 6);
}
