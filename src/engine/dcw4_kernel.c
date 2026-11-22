/* dcw4_kernel.c — see dcw4_kernel.h.
 *
 * The blob reader, the QDQ front end (0.6 of a window's 61 MMAC, plain
 * loops over the output channels), the position code and the log-softmax
 * head; the encoder is dcw4_encoder.inc. Built with -ffp-contract=off: the
 * front end must round as ONNX Runtime does (the first GroupNorm feeds a
 * uint8 quantiser, so a last-bit difference moves a feature a whole 0.04
 * step), and SDRIAK measured it bit-exact that way on an FMA CPU.
 *
 * A C port of SDRIAK's deepcw_narrow_fused.cpp (bubnikv/sdriak 850a08d,
 * GPL-3.0-or-later).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "dcw4_kernel.h"
#include "dcw4_encoder.h"

#include <gio/gio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define DCW4_X86 1
#endif

/* the built-in blob: weights/dcw4.gresource.xml (gnome.compile_resources) */
extern GResource *skim_dcw4_weights_get_resource(void);
#define DCW4_RES_PATH "/skimmer-for-linux/dcw4/deepcw-v4-narrow.bin"

#define MAGIC        "DCWNARW1"
#define VERSION      1u
#define NAME_LEN     48u
#define HEADER_BYTES (8u + 8u + 64u)
#define ENTRY_BYTES  (NAME_LEN + 4u * 2u + 4u * 4u + 4u * 2u)

/* the front end: conv 0 1 → 12 channels, stride (2, 2), GroupNorm 6;
 * conv 1 12 → 24, stride (1, 2), GroupNorm 8 */
#define C0 12u
#define C1 24u
#define G0 6u
#define G1 8u
/* the convs as im2col products: their taps, their outputs padded to 16 lanes */
#define K0 9u
#define N0 16u
#define K1 (C0 * 3u * 2u)
#define N1 32u
#define PE_ROWS 2048u            /* the position code kept for windows up to 65 s */

struct _SkimDcw4Net {
  float      *arena;             /* every float weight, 64-byte aligned slices */
  gsize       arena_n, arena_cap;
  Dcw4Net     net;
  float       in_scale[2];       /* the QDQ scales of conv 0's and conv 1's inputs */
  const float *w0;               /* [9][16]: [kt][kf][out], dequantised */
  const float *w1;               /* [72][32]: [cin][kt][kf 1..2][out]   */
  const float *zero;             /* [32] zeros: the convs' bias         */
  const float *gn_scale[2], *gn_bias[2], *gamma[2], *beta[2];
  const float *freq;             /* [32]                                */
  float      *pe;                /* [PE_ROWS][64]                       */
  SkimDcw4Isa isa;
};

/* ---- instruction sets ----------------------------------------------------- */

static gboolean cpu_has_avx2_fma(void) {
#if defined(DCW4_X86) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return FALSE;
#endif
}

const char *skim_dcw4_isa_name(SkimDcw4Isa isa) {
  switch (isa) {
  case SKIM_DCW4_ISA_AUTO: return "auto";
  case SKIM_DCW4_ISA_SSE:  return "sse";
  case SKIM_DCW4_ISA_AVX2: return "avx2";
  default:                 return "generic";
  }
}

gboolean skim_dcw4_isa_from_name(const char *name, SkimDcw4Isa *isa) {
  static const SkimDcw4Isa all[] = { SKIM_DCW4_ISA_AUTO, SKIM_DCW4_ISA_GENERIC,
                                     SKIM_DCW4_ISA_SSE, SKIM_DCW4_ISA_AVX2 };
  for (guint i = 0; name && i < G_N_ELEMENTS(all); i++) {
    if (g_strcmp0(name, skim_dcw4_isa_name(all[i])) == 0) {
      *isa = all[i];
      return TRUE;
    }
  }
  return FALSE;
}

guint skim_dcw4_isas_available(SkimDcw4Isa out[3]) {
  guint n = 0;
  if (dcw4_avx2_compiled() && cpu_has_avx2_fma()) { out[n++] = SKIM_DCW4_ISA_AVX2; }
  if (dcw4_sse_compiled()) { out[n++] = SKIM_DCW4_ISA_SSE; }
  out[n++] = SKIM_DCW4_ISA_GENERIC;
  return n;
}

static gboolean isa_runs(SkimDcw4Isa isa) {
  SkimDcw4Isa have[3];
  const guint n = skim_dcw4_isas_available(have);
  for (guint i = 0; i < n; i++) {
    if (have[i] == isa) { return TRUE; }
  }
  return FALSE;
}

/* ---- the blob --------------------------------------------------------------- */

static guint32 le32(const guint8 *p) {
  return (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 | (guint32)p[3] << 24;
}

typedef struct {
  char    name[NAME_LEN + 1];
  guint32 dtype, ndim, dims[4], offset, bytes;
} Entry;

typedef struct {
  SkimDcw4Net  *m;
  const guint8 *blob;
  Entry        *entries;
  guint32       count;
  char         *bad;             /* the first failure */
} Bind;

/* n floats from the arena, at a 64-byte boundary, zeroed. The arena is
 * sized up front, so earlier slices never move. */
static float *arena_alloc(SkimDcw4Net *m, gsize n) {
  const gsize at = (m->arena_n + 15u) & ~(gsize)15u;
  g_assert(at + n <= m->arena_cap);
  m->arena_n = at + n;
  return m->arena + at;
}

static const Entry *find(Bind *b, const char *name, guint32 dtype, guint ndim, const guint32 *dims) {
  for (guint32 i = 0; i < b->count; i++) {
    const Entry *e = &b->entries[i];
    if (strcmp(e->name, name) != 0) { continue; }
    gboolean ok = e->dtype == dtype && e->ndim == ndim;
    gsize n = 1;
    for (guint d = 0; d < ndim; d++) {
      ok = ok && e->dims[d] == dims[d];
      n *= dims[d];
    }
    ok = ok && e->bytes == n * (dtype == 0 ? 4u : 1u);
    if (!ok) {
      if (!b->bad) { b->bad = g_strdup_printf("tensor %s has another type or shape", name); }
      return NULL;
    }
    return e;
  }
  if (!b->bad) { b->bad = g_strdup_printf("no tensor %s", name); }
  return NULL;
}

#define DIMS(...) (const guint32[]){ __VA_ARGS__ }, \
                  (guint)(sizeof((const guint32[]){ __VA_ARGS__ }) / sizeof(guint32))

static const float *f32_(Bind *b, const char *name, const guint32 *dims, guint ndim) {
  const Entry *e = find(b, name, 0, ndim, dims);
  if (!e) { return NULL; }
  float *dst = arena_alloc(b->m, e->bytes / 4);
  for (guint32 i = 0; i < e->bytes / 4; i++) {
    const guint32 u = le32(b->blob + e->offset + 4 * i);
    memcpy(dst + i, &u, 4);
  }
  return dst;
}
#define F32(b, name, ...) f32_((b), (name), DIMS(__VA_ARGS__))

static const gint8 *i8_(Bind *b, const char *name, const guint32 *dims, guint ndim) {
  const Entry *e = find(b, name, 1, ndim, dims);
  return e ? (const gint8 *)(b->blob + e->offset) : NULL;
}
#define I8(b, name, ...) i8_((b), (name), DIMS(__VA_ARGS__))

/* pe[t][2i] = sin(t · f_i), pe[t][2i + 1] = cos(t · f_i), the angle in
 * float as the model's Mul computes it. */
static void fill_position_code(const float *freq, guint t0, guint t1, float *pe) {
  for (guint t = t0; t < t1; t++) {
    float *row = pe + (gsize)(t - t0) * DCW4_D;
    for (guint i = 0; i < DCW4_D / 2; i++) {
      const float a = (float)t * freq[i];
      row[2 * i] = sinf(a);
      row[2 * i + 1] = cosf(a);
    }
  }
}

static gboolean bind(SkimDcw4Net *m, const guint8 *blob, gsize len, GError **error) {
#define FAIL(...) do { g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, __VA_ARGS__); \
                       g_free(b.entries); g_free(b.bad); return FALSE; } while (0)
  Bind b = { m, blob, NULL, 0, NULL };
  if (len < HEADER_BYTES || memcmp(blob, MAGIC, 8) != 0) {
    FAIL("not a DeepCW narrow fused-weights file");
  }
  if (le32(blob + 8) != VERSION) {
    FAIL("fused-weights version %u, expected %u", le32(blob + 8), VERSION);
  }
  b.count = le32(blob + 12);
  if (memcmp(blob + 16, SKIM_DCW4_MODEL_SHA256, 64) != 0) {
    FAIL("written from another model file than model_narrow.onnx fc80c4ba…");
  }
  if (len < HEADER_BYTES + (gsize)b.count * ENTRY_BYTES) { FAIL("truncated"); }
  b.entries = g_new0(Entry, b.count);
  for (guint32 i = 0; i < b.count; i++) {
    const guint8 *p = blob + HEADER_BYTES + (gsize)i * ENTRY_BYTES;
    Entry *e = &b.entries[i];
    memcpy(e->name, p, NAME_LEN);
    e->name[NAME_LEN] = '\0';
    e->dtype = le32(p + NAME_LEN);
    e->ndim = le32(p + NAME_LEN + 4);
    for (int d = 0; d < 4; d++) { e->dims[d] = le32(p + NAME_LEN + 8 + 4 * d); }
    e->offset = le32(p + NAME_LEN + 24);
    e->bytes = le32(p + NAME_LEN + 28);
    if (e->ndim > 4 || (gsize)e->offset + e->bytes > len) { FAIL("tensor out of range"); }
  }

  /* every tensor, its alignment, the dequantised convs and the padded classifier */
  m->arena_cap = len / 4 + 16 * ((gsize)b.count + 8) + K0 * N0 + K1 * N1 + N1 +
                 (DCW4_D + 1) * DCW4_CLASSES_PAD;
  m->arena = g_aligned_alloc0(m->arena_cap, sizeof(float), 64);
  m->arena_n = 0;

  /* ---- front end: dequantise the int8 weights as DequantizeLinear does */
  const guint co[2] = { C0, C1 }, ci[2] = { 1, C0 }, groups[2] = { G0, G1 };
  for (guint c = 0; c < 2; c++) {
    char nm[NAME_LEN];
#define P(s) (g_snprintf(nm, sizeof nm, "fe%u.%s", c, (s)), nm)
    const float *sc = F32(&b, P("in_scale"), 1);
    const gint8 *w = I8(&b, P("w"), co[c], ci[c], 3, 3);
    const float *ws = F32(&b, P("w_scale"), co[c]);
    const gint8 *wz = I8(&b, P("w_zp"), co[c]);
    m->gn_scale[c] = F32(&b, P("gn_scale"), groups[c]);
    m->gn_bias[c] = F32(&b, P("gn_bias"), groups[c]);
    m->gamma[c] = F32(&b, P("gamma"), co[c]);
    m->beta[c] = F32(&b, P("beta"), co[c]);
#undef P
    if (!sc || !w || !ws || !wz || !m->gn_scale[c] || !m->gn_bias[c] || !m->gamma[c] ||
        !m->beta[c]) {
      FAIL("%s", b.bad);
    }
    m->in_scale[c] = sc[0];
    /* im2col weights [in][kt][kf][out], the outputs padded to a whole
     * 16-lane block. conv 0: [9][16]. conv 1: [12][3][kf 1..2][32] — its
     * frequency tap 0 always reads the padding (its one output frequency
     * is centred on input 0 of 2), and a tap on zero adds nothing. */
    const gboolean c1 = c == 1;
    float *dq = arena_alloc(m, c1 ? (gsize)K1 * N1 : (gsize)K0 * N0);
    for (guint o = 0; o < co[c]; o++) {
      for (guint i = 0; i < ci[c]; i++) {
        for (guint k = 0; k < 9; k++) {
          const int q = w[((gsize)o * ci[c] + i) * 9 + k];
          const float v = (float)(q - (int)wz[o]) * ws[o];
          if (!c1) {
            dq[(gsize)k * N0 + o] = v;
          } else if (k % 3 != 0) {
            dq[(((gsize)i * 3 + k / 3) * 2 + (k % 3 - 1)) * N1 + o] = v;
          }
        }
      }
    }
    if (c == 0) { m->w0 = dq; } else { m->w1 = dq; }
  }
  m->zero = arena_alloc(m, N1);

  /* ---- the encoder */
  Dcw4Net *n = &m->net;
  n->proj_w = F32(&b, "proj.w", DCW4_FEAT, DCW4_D);
  n->proj_b = F32(&b, "proj.b", DCW4_D);
  n->proj_ng = F32(&b, "proj_norm.g", DCW4_D);
  n->proj_nb = F32(&b, "proj_norm.b", DCW4_D);
  m->freq = F32(&b, "pe.freq", DCW4_D / 2);
  gboolean ok = n->proj_w && n->proj_b && n->proj_ng && n->proj_nb && m->freq;
  for (guint l = 0; l < DCW4_LAYERS && ok; l++) {
    Dcw4Layer *L = &n->layer[l];
    char nm[NAME_LEN];
#define P(s) (g_snprintf(nm, sizeof nm, "l%u.%s", l, (s)), nm)
    L->ffn1_ng = F32(&b, P("ffn1.norm.g"), DCW4_D);
    L->ffn1_nb = F32(&b, P("ffn1.norm.b"), DCW4_D);
    L->ffn1_w1 = F32(&b, P("ffn1.w1"), DCW4_D, DCW4_FF);
    L->ffn1_b1 = F32(&b, P("ffn1.b1"), DCW4_FF);
    L->ffn1_w2 = F32(&b, P("ffn1.w2"), DCW4_FF, DCW4_D);
    L->ffn1_b2 = F32(&b, P("ffn1.b2"), DCW4_D);
    L->ffn2_ng = F32(&b, P("ffn2.norm.g"), DCW4_D);
    L->ffn2_nb = F32(&b, P("ffn2.norm.b"), DCW4_D);
    L->ffn2_w1 = F32(&b, P("ffn2.w1"), DCW4_D, DCW4_FF);
    L->ffn2_b1 = F32(&b, P("ffn2.b1"), DCW4_FF);
    L->ffn2_w2 = F32(&b, P("ffn2.w2"), DCW4_FF, DCW4_D);
    L->ffn2_b2 = F32(&b, P("ffn2.b2"), DCW4_D);
    L->attn_ng = F32(&b, P("attn_norm.g"), DCW4_D);
    L->attn_nb = F32(&b, P("attn_norm.b"), DCW4_D);
    L->qkv_w = F32(&b, P("qkv.w"), DCW4_D, 3 * DCW4_D);
    L->qkv_b = F32(&b, P("qkv.b"), 3 * DCW4_D);
    L->out_w = F32(&b, P("out.w"), DCW4_D, DCW4_D);
    L->out_b = F32(&b, P("out.b"), DCW4_D);
    L->conv_ng = F32(&b, P("conv.norm.g"), DCW4_D);
    L->conv_nb = F32(&b, P("conv.norm.b"), DCW4_D);
    L->pwin_w = F32(&b, P("pw_in.w"), DCW4_D, DCW4_FF);
    L->pwin_b = F32(&b, P("pw_in.b"), DCW4_FF);
    L->dw_w = F32(&b, P("dw.w"), DCW4_DW_KERNEL, DCW4_D);
    L->dw_b = F32(&b, P("dw.b"), DCW4_D);
    L->pwout_w = F32(&b, P("pw_out.w"), DCW4_D, DCW4_D);
    L->pwout_b = F32(&b, P("pw_out.b"), DCW4_D);
    L->final_ng = F32(&b, P("final_norm.g"), DCW4_D);
    L->final_nb = F32(&b, P("final_norm.b"), DCW4_D);
#undef P
    const float *all[] = { L->ffn1_ng, L->ffn1_nb, L->ffn1_w1, L->ffn1_b1, L->ffn1_w2,
                           L->ffn1_b2, L->attn_ng, L->attn_nb, L->qkv_w, L->qkv_b, L->out_w,
                           L->out_b, L->conv_ng, L->conv_nb, L->pwin_w, L->pwin_b, L->dw_w,
                           L->dw_b, L->pwout_w, L->pwout_b, L->ffn2_ng, L->ffn2_nb,
                           L->ffn2_w1, L->ffn2_b1, L->ffn2_w2, L->ffn2_b2, L->final_ng,
                           L->final_nb };
    for (guint i = 0; i < G_N_ELEMENTS(all); i++) { ok = ok && all[i]; }
  }
  n->out_ng = F32(&b, "out_norm.g", DCW4_D);
  n->out_nb = F32(&b, "out_norm.b", DCW4_D);
  const float *cw = F32(&b, "cls.w", DCW4_D, DCW4_CLASSES);
  const float *cb = F32(&b, "cls.b", DCW4_CLASSES);
  if (!ok || !n->out_ng || !n->out_nb || !cw || !cb) { FAIL("%s", b.bad ? b.bad : "?"); }
  /* the classifier padded to 48 columns (zeros) */
  float *pw = arena_alloc(m, (gsize)DCW4_D * DCW4_CLASSES_PAD);
  float *pb = arena_alloc(m, DCW4_CLASSES_PAD);
  for (guint k = 0; k < DCW4_D; k++) {
    memcpy(pw + (gsize)k * DCW4_CLASSES_PAD, cw + (gsize)k * DCW4_CLASSES,
           DCW4_CLASSES * sizeof(float));
  }
  memcpy(pb, cb, DCW4_CLASSES * sizeof(float));
  n->cls_w = pw;
  n->cls_b = pb;

  m->pe = g_new0(float, (gsize)PE_ROWS * DCW4_D);
  fill_position_code(m->freq, 0, PE_ROWS, m->pe);
  g_free(b.entries);
  g_free(b.bad);
  return TRUE;
#undef FAIL
}

void skim_dcw4_net_free(SkimDcw4Net *m) {
  if (!m) { return; }
  g_aligned_free(m->arena);
  g_free(m->pe);
  g_free(m);
}

SkimDcw4Net *skim_dcw4_net_new(const guint8 *blob, gsize len, gboolean verify, SkimDcw4Isa isa,
                               GError **error) {
  if (isa != SKIM_DCW4_ISA_AUTO && !isa_runs(isa)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                "the %s encoder is not available on this CPU or build", skim_dcw4_isa_name(isa));
    return NULL;
  }
  if (verify) {
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, blob, len);
    const gboolean same = g_strcmp0(sha, SKIM_DCW4_BLOB_SHA256) == 0;
    if (!same) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "not the narrow model's fused weights (sha256 %.8s…, expected %.8s…)", sha,
                  SKIM_DCW4_BLOB_SHA256);
    }
    g_free(sha);
    if (!same) { return NULL; }
  }
  SkimDcw4Net *m = g_new0(SkimDcw4Net, 1);
  if (!bind(m, blob, len, error)) {
    skim_dcw4_net_free(m);
    return NULL;
  }
  SkimDcw4Isa have[3];
  skim_dcw4_isas_available(have);
  m->isa = isa == SKIM_DCW4_ISA_AUTO ? have[0] : isa;
  return m;
}

SkimDcw4Net *skim_dcw4_net_new_builtin(SkimDcw4Isa isa, GError **error) {
  GBytes *bytes = g_resource_lookup_data(skim_dcw4_weights_get_resource(), DCW4_RES_PATH,
                                         G_RESOURCE_LOOKUP_FLAGS_NONE, error);
  if (!bytes) { return NULL; }
  gsize len = 0;
  const guint8 *p = g_bytes_get_data(bytes, &len);
  SkimDcw4Net *m = skim_dcw4_net_new(p, len, TRUE, isa, error);
  g_bytes_unref(bytes);
  return m;
}

const SkimDcw4Net *skim_dcw4_net_shared(const char **error) {
  static gsize once = 0;
  static SkimDcw4Net *net = NULL;
  static char *err = NULL;
  if (g_once_init_enter(&once)) {
    SkimDcw4Isa isa = SKIM_DCW4_ISA_AUTO;
    const char *want = g_getenv("SKIM_DCW4_ISA");
    GError *e = NULL;
    if (want && *want && !skim_dcw4_isa_from_name(want, &isa)) {
      err = g_strdup_printf("SKIM_DCW4_ISA=%s: not auto, avx2, sse or generic", want);
    } else if (!(net = skim_dcw4_net_new_builtin(isa, &e))) {
      err = g_strdup(e->message);
      g_error_free(e);
    }
    g_once_init_leave(&once, 1);
  }
  if (error) { *error = err; }
  return net;
}

SkimDcw4Isa skim_dcw4_net_isa(const SkimDcw4Net *m) { return m->isa; }

/* ---- the forward pass ----------------------------------------------------- */

/* The QDQ pair of a uint8 tensor with zero point 0: round half to even,
 * clamp to 0..255, back to float (ONNX QuantizeLinear, DequantizeLinear). */
static inline float qdq_u8(float x, float scale) {
  float q = nearbyintf(x / scale);
  q = q < 0.0f ? 0.0f : (q > 255.0f ? 255.0f : q);
  return q * scale;
}

/* Eigen's vectorised sum (redux_vec_linear) as ONNX Runtime's x64 build
 * runs it, on SSE packets of 4: the packets from the first 16-byte aligned
 * element (start) in two interleaved accumulators, their sum reduced as
 * (p0 + p2) + (p1 + p3), then the unaligned head, then the tail. */
static float eigen_sum4(const float *v, gsize n, gsize start) {
  enum { P = 4 };
  const gsize s0 = MIN(start, n);
  const gsize aligned = (n - s0) / P * P, aligned2 = (n - s0) / (2 * P) * (2 * P);
  const gsize end = s0 + aligned, end2 = s0 + aligned2;
  if (aligned == 0) {
    float r = v[0];
    for (gsize i = 1; i < n; i++) { r = r + v[i]; }
    return r;
  }
  float p0[P], p1[P];
  for (gsize l = 0; l < P; l++) { p0[l] = v[s0 + l]; }
  if (aligned > P) {
    for (gsize l = 0; l < P; l++) { p1[l] = v[s0 + P + l]; }
    for (gsize i = s0 + 2 * P; i < end2; i += 2 * P) {
      for (gsize l = 0; l < P; l++) {
        p0[l] = p0[l] + v[i + l];
        p1[l] = p1[l] + v[i + P + l];
      }
    }
    for (gsize l = 0; l < P; l++) { p0[l] = p0[l] + p1[l]; }
    if (end > end2) {
      for (gsize l = 0; l < P; l++) { p0[l] = p0[l] + v[end2 + l]; }
    }
  }
  float r = (p0[0] + p0[2]) + (p0[1] + p0[3]);
  for (gsize i = 0; i < s0; i++) { r = r + v[i]; }
  for (gsize i = end; i < n; i++) { r = r + v[i]; }
  return r;
}

/* GroupNorm as the model computes it, to the bit of ONNX Runtime's CPU
 * InstanceNormalization: x planar [C][W] (the model's own layout, each
 * group's channels contiguous, W the frames × frequencies of one channel),
 * groups of C / G channels. Per group: the mean as Eigen's sum over the
 * group (its head aligned as an aligned buffer puts it) / n; the squared
 * deviations' sum (an expression: no aligned head); inv = 1 / sqrt(sq / n
 * + ε); y = x · (inv · scale) + (bias − mean · inv · scale); then the
 * per-channel gamma and beta, and the ReLU. sq: W · C / G floats. */
static void group_norm_relu(float *x, gsize W, guint C, guint G, const float *gs,
                            const float *gb, const float *gamma, const float *beta, float *sq) {
  const gsize n = W * (C / G);
  for (guint g = 0; g < G; g++) {
    float *v = x + g * n;
    const float mean = eigen_sum4(v, n, (4 - (g * n) % 4) % 4) / (float)n;
    for (gsize i = 0; i < n; i++) {
      const float d = v[i] - mean;
      sq[i] = d * d;
    }
    const float inv = 1.0f / sqrtf(eigen_sum4(sq, n, 0) / (float)n + 1e-5f);
    const float cs = inv * gs[g];
    const float sh = gb[g] - mean * cs;
    for (guint c = g * (C / G); c < (g + 1) * (C / G); c++) {
      float *row = x + (gsize)c * W;
      for (gsize i = 0; i < W; i++) {
        float y = row[i] * cs + sh;
        y = y * gamma[c] + beta[c];
        row[i] = y > 0.0f ? y : 0.0f;
      }
    }
  }
}

/* A thread's scratch: grown on demand, freed when the thread ends. */
typedef struct {
  float *xq, *col, *mm, *planar, *sq, *c0, *feat, *pe, *work, *logits;
  gsize  n_xq, n_col, n_mm, n_planar, n_sq, n_c0, n_feat, n_pe, n_work, n_logits;
} Scratch;

static void scratch_free(gpointer p) {
  Scratch *s = p;
  g_free(s->xq); g_free(s->col); g_free(s->mm); g_free(s->planar); g_free(s->sq);
  g_free(s->c0); g_free(s->feat); g_free(s->pe); g_free(s->work); g_free(s->logits);
  g_free(s);
}

static GPrivate scratch_key = G_PRIVATE_INIT(scratch_free);

/* buf holds at least n floats (contents kept up to the old size). */
static float *grow(float **buf, gsize *have, gsize n) {
  if (n > *have) {
    *buf = g_renew(float, *buf, n);
    *have = n;
  }
  return *buf;
}

static void zero(float *p, gsize n) { memset(p, 0, n * sizeof(float)); }

/* tile [T][3] -> feat [T1][24]: QDQ, conv 3×3 stride (2, 2), GroupNorm 6,
 * ReLU, QDQ, conv 3×3 stride (1, 2), GroupNorm 8, ReLU — to the bit of
 * ONNX Runtime on the same ISA: each conv an im2col product (its taps in
 * (input channel, time, frequency) order, one FMA chain from zero as
 * MLAS's SGEMM; mul + add on the generic path, as MLAS without FMA), the
 * GroupNorms in the model's planar layout. */
static void front_end(const SkimDcw4Net *m, const float *tile, guint T, float *feat, Scratch *s,
                      SkimDcw4Isa isa) {
  const Dcw4MatmulFn matmul = isa == SKIM_DCW4_ISA_AVX2 ? dcw4_matmul_avx2
                            : isa == SKIM_DCW4_ISA_SSE  ? dcw4_matmul_sse
                                                        : dcw4_matmul_generic;
  const guint T1 = skim_dcw4_output_frames(T);
  const gsize R0 = (gsize)T1 * 2;          /* conv 0's outputs: frames × 2 frequencies */
  float *xq = grow(&s->xq, &s->n_xq, (gsize)T * SKIM_DCW4_BINS);
  for (gsize i = 0; i < (gsize)T * SKIM_DCW4_BINS; i++) { xq[i] = qdq_u8(tile[i], m->in_scale[0]); }
  /* conv 0: col [T1 · 2][9] x w0 [9][16] */
  float *col = grow(&s->col, &s->n_col, MAX(R0 * K0, (gsize)T1 * K1));
  zero(col, R0 * K0);
  for (guint t = 0; t < T1; t++) {
    for (guint fo = 0; fo < 2; fo++) {
      float *row = col + ((gsize)t * 2 + fo) * K0;
      for (guint dt = 0; dt < 3; dt++) {
        const int ti = (int)(2 * t + dt) - 1;
        for (guint df = 0; df < 3; df++) {
          const int fi = (int)(2 * fo + df) - 1;
          if (ti >= 0 && ti < (int)T && fi >= 0 && fi < (int)SKIM_DCW4_BINS) {
            row[dt * 3 + df] = xq[(gsize)ti * SKIM_DCW4_BINS + fi];
          }
        }
      }
    }
  }
  float *mm = grow(&s->mm, &s->n_mm, MAX(R0 * N0, (gsize)T1 * N1));
  matmul(col, (unsigned)R0, K0, m->w0, N0, m->zero, mm);
  /* planar [12][T1][2], GroupNorm, ReLU, QDQ, back to c0 [T1][2][12] */
  float *planar = grow(&s->planar, &s->n_planar, MAX(R0 * C0, (gsize)T1 * C1));
  for (gsize r = 0; r < R0; r++) {
    for (guint c = 0; c < C0; c++) { planar[c * R0 + r] = mm[r * N0 + c]; }
  }
  float *sq = grow(&s->sq, &s->n_sq, MAX(R0 * (C0 / G0), (gsize)T1 * (C1 / G1)));
  group_norm_relu(planar, R0, C0, G0, m->gn_scale[0], m->gn_bias[0], m->gamma[0], m->beta[0], sq);
  float *c0 = grow(&s->c0, &s->n_c0, R0 * C0);
  for (gsize r = 0; r < R0; r++) {
    for (guint c = 0; c < C0; c++) { c0[r * C0 + c] = qdq_u8(planar[c * R0 + r], m->in_scale[1]); }
  }
  /* conv 1: col [T1][12 · 3 · 2] (time taps outside the window are 0) x w1 [72][32] */
  zero(col, (gsize)T1 * K1);
  for (guint t = 0; t < T1; t++) {
    float *row = col + (gsize)t * K1;
    for (guint dt = 0; dt < 3; dt++) {
      const int ti = (int)(t + dt) - 1;
      if (ti < 0 || ti >= (int)T1) { continue; }
      const float *src = c0 + (gsize)ti * 2 * C0;
      for (guint c = 0; c < C0; c++) {
        row[(c * 3 + dt) * 2] = src[c];
        row[(c * 3 + dt) * 2 + 1] = src[C0 + c];
      }
    }
  }
  matmul(col, T1, K1, m->w1, N1, m->zero, mm);
  for (guint t = 0; t < T1; t++) {
    for (guint c = 0; c < C1; c++) { planar[(gsize)c * T1 + t] = mm[(gsize)t * N1 + c]; }
  }
  group_norm_relu(planar, T1, C1, G1, m->gn_scale[1], m->gn_bias[1], m->gamma[1], m->beta[1], sq);
  for (guint t = 0; t < T1; t++) {
    for (guint c = 0; c < C1; c++) { feat[(gsize)t * C1 + c] = planar[(gsize)c * T1 + t]; }
  }
}

void skim_dcw4_net_run(const SkimDcw4Net *m, const float *tile, guint T, float *logp,
                       SkimDcw4Isa force) {
  if (T == 0) { return; }
#if defined(DCW4_X86)
  /* denormals flushed for the run (FTZ, DAZ), the caller's mode restored */
  const unsigned csr = _mm_getcsr();
  _mm_setcsr(csr | 0x8040u);
#endif
  Scratch *s = g_private_get(&scratch_key);
  if (!s) {
    s = g_new0(Scratch, 1);
    g_private_set(&scratch_key, s);
  }
  const SkimDcw4Isa isa = force != SKIM_DCW4_ISA_AUTO && isa_runs(force) ? force : m->isa;
  const guint T1 = skim_dcw4_output_frames(T);
  float *feat = grow(&s->feat, &s->n_feat, (gsize)T1 * C1);
  front_end(m, tile, T, feat, s, isa);
  const float *pe = m->pe;
  if (T1 > PE_ROWS) {
    float *tmp = grow(&s->pe, &s->n_pe, (gsize)T1 * DCW4_D);
    fill_position_code(m->freq, 0, T1, tmp);
    pe = tmp;
  }
  float *work = grow(&s->work, &s->n_work, dcw4_work_floats(T1));
  float *logits = grow(&s->logits, &s->n_logits, (gsize)T1 * DCW4_CLASSES_PAD);
  const Dcw4EncodeFn encode = isa == SKIM_DCW4_ISA_AVX2 ? dcw4_encode_avx2
                            : isa == SKIM_DCW4_ISA_SSE  ? dcw4_encode_sse
                                                        : dcw4_encode_generic;
  encode(&m->net, feat, pe, T1, logits, work);
  /* LogSoftmax over the 42 classes */
  for (guint t = 0; t < T1; t++) {
    const float *z = logits + (gsize)t * DCW4_CLASSES_PAD;
    float *o = logp + (gsize)t * DCW4_CLASSES;
    float mx = z[0];
    for (guint k = 1; k < DCW4_CLASSES; k++) { mx = MAX(mx, z[k]); }
    float sum = 0.0f;
    for (guint k = 0; k < DCW4_CLASSES; k++) { sum += expf(z[k] - mx); }
    const float ls = logf(sum);
    for (guint k = 0; k < DCW4_CLASSES; k++) { o[k] = z[k] - mx - ls; }
  }
#if defined(DCW4_X86)
  _mm_setcsr(csr);
#endif
}

/* ---- the input normalisation ---------------------------------------------- */

static int cmp_float(const void *a, const void *b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

void skim_dcw4_normalize(float *x, gsize n) {
  if (n == 0) { return; }
  const double quantile = 0.9;                       /* noise_level_quantile */
  const double rayleigh = sqrt(-2.0 * log(1.0 - quantile));
  float *s = g_memdup2(x, n * sizeof(float));
  qsort(s, n, sizeof(float), cmp_float);
  const gsize qi = (gsize)floor(quantile * (double)(n - 1));
  const double q = s[qi], peak = s[n - 1];
  g_free(s);
  /* NOISE_LEVEL_MIN_RELATIVE_TO_PEAK 1e-3, NOISE_LEVEL_MIN 1e-5 */
  const double noise = MAX(MAX(q / rayleigh, peak * 1e-3), 1e-5);
  const float inv = (float)(1.0 / noise);
  for (gsize i = 0; i < n; i++) { x[i] = log1pf(x[i] * inv); }
}
