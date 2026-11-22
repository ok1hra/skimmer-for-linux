/* dcw4_kernel_test.c — gate dcw4-kernel: CW v4's network (dcw4_kernel.h).
 *
 *  A. The built-in blob loads on every encoder this CPU runs; a blob with
 *     one byte changed, another file's magic or a cut copy is refused.
 *  B. Every encoder against the plain-array one, on synthetic keyed tiles
 *     (Morse at 12–35 WPM, +10 to +30 dB per bin over Rayleigh noise, windows
 *     of 9 frames to 67 s — past the kept position code): the
 *     probabilities within 1e-4 (|Δ p|: see max_pdiff) and the same
 *     greedy text.
 *  C. The network reads: a clean "CQ DE OK1HRA" tile comes back as that
 *     text, on every encoder; a tile of noise alone as nothing.
 *  D. Only with SKIM_DCW4_ONNX=<model_narrow.onnx> (the pinned file) and an
 *     ONNX Runtime ort_shim finds (SKIM_ORT_LIB): every tile of B through
 *     the model itself — |Δ p| ≤ 1e-4, the same greedy text. Without
 *     them D is skipped and said so (the gate still passes on A–C).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/dcw4_kernel.h"
#include "engine/ort_shim.h"

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); \
                              printf("\n"); } } while (0)

/* ---- synthetic tiles -------------------------------------------------------- */

static guint64 g_rng;
static double urand(void) {           /* xorshift64*, (0, 1) */
  g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
  return ((double)((g_rng * 2685821657736338717ull) >> 11) + 0.5) / 9007199254740992.0;
}
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * G_PI * urand()); }

static const char *morse(char c) {
  static const char *az[26] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..",
    ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-",
    ".--", "-..-", "-.--", "--.." };
  static const char *d09[10] = { "-----", ".----", "..---", "...--", "....-", ".....", "-....",
    "--...", "---..", "----." };
  if (c >= 'A' && c <= 'Z') { return az[c - 'A']; }
  if (c >= '0' && c <= '9') { return d09[c - '0']; }
  if (c == '/') { return "-..-."; }
  if (c == '?') { return "..--.."; }
  return NULL;
}

/* key[T]: text keyed at wpm from frame `lead` on, 16 ms frames; FALSE if
 * the text does not fit. */
static gboolean keying(const char *text, double wpm, guint lead, gboolean *key, guint T) {
  const double dot = 1.2 / wpm * 62.5;
  double t = lead;
  memset(key, 0, T * sizeof(gboolean));
  for (const char *p = text; *p; p++) {
    if (*p == ' ') { t += 4 * dot; continue; }        /* 3 after the letter + 4 = 7 */
    for (const char *e = morse(*p); e && *e; e++) {
      const double len = (*e == '-' ? 3 : 1) * dot;
      for (guint i = (guint)t; i < (guint)(t + len) && i < T; i++) { key[i] = TRUE; }
      t += len + dot;
    }
    t += 2 * dot;
  }
  return t < T;
}

/* tile [T][3]: a keyed line on the centre bin (its neighbours get the Hann
 * window's −6 dB leakage) over Rayleigh noise of unit power per bin,
 * normalised as the model's input. snr_db: the line over one bin's noise. */
static void make_tile(float *tile, const gboolean *key, guint T, double snr_db, guint64 seed) {
  const double a = pow(10.0, snr_db / 20.0);
  const double leak[3] = { 0.5, 1.0, 0.5 };
  g_rng = seed * 0x9E3779B97F4A7C15ull + 1;
  for (guint t = 0; t < T; t++) {
    for (guint b = 0; b < 3; b++) {
      const double re = gauss() * sqrt(0.5) + (key && key[t] ? a * leak[b] : 0.0);
      const double im = gauss() * sqrt(0.5);
      tile[t * 3 + b] = (float)hypot(re, im);
    }
  }
  skim_dcw4_normalize(tile, (gsize)T * 3);
}

/* greedy CTC: argmax per frame, repeats collapsed, blanks dropped, runs of
 * spaces one space, none at the ends */
static char *greedy(const float *logp, guint T1) {
  GString *s = g_string_new(NULL);
  int prev = -1;
  for (guint t = 0; t < T1; t++) {
    const float *r = logp + (gsize)t * SKIM_DCW4_CLASSES;
    int k = 0;
    for (int c = 1; c < SKIM_DCW4_CLASSES; c++) { if (r[c] > r[k]) { k = c; } }
    if (k != prev && k != SKIM_DCW4_BLANK) {
      if (k == SKIM_DCW4_SPACE) {
        if (s->len && s->str[s->len - 1] != ' ') { g_string_append_c(s, ' '); }
      } else {
        g_string_append_c(s, SKIM_DCW4_ALPHABET[k]);
      }
    }
    prev = k;
  }
  while (s->len && s->str[s->len - 1] == ' ') { g_string_truncate(s, s->len - 1); }
  return g_string_free(s, FALSE);
}

/* The largest difference of two log-prob matrices as probabilities: what
 * the stitcher reads (it averages, dilates and takes the argmax of them).
 * The log-probs themselves differ by up to ~4e-4 where float32 is
 * ill-conditioned (a frame between two classes; SDRIAK measured ONNX
 * Runtime itself 3.4e-4 off a float64 pass there), and by far more on
 * classes at e^-20, which mean nothing. */
static double max_pdiff(const float *a, const float *b, gsize n) {
  double m = 0;
  for (gsize i = 0; i < n; i++) { m = MAX(m, fabs(exp((double)a[i]) - exp((double)b[i]))); }
  return m;
}

typedef struct { guint T; double wpm, snr; guint64 seed; } Case;

/* snr: the line over one 12.5 Hz bin's noise — SNR500 ≈ snr − 14 dB (the
 * 20-point Hann's ENBW 18.75 Hz against 500 Hz) */
static const Case cases[] = {
  { 9, 25, 20, 1 },    { 64, 25, 20, 2 },   { 125, 18, 14, 3 },  { 375, 25, 30, 4 },
  { 375, 25, 14, 5 },  { 375, 25, 10, 6 },  { 437, 35, 20, 7 },  { 438, 12, 14, 8 },
  { 750, 18, 20, 9 },  { 750, 30, 10, 10 }, { 1000, 22, 14, 11 }, { 4200, 20, 14, 12 },
};
#define TEXT "CQ CQ DE OK1HRA OK1HRA K TU 5NN 15 ? / R"

int main(void) {
  GError *err = NULL;

  /* ---- A */
  SkimDcw4Isa have[3];
  const guint nisa = skim_dcw4_isas_available(have);
  printf("encoders here:");
  for (guint i = 0; i < nisa; i++) { printf(" %s", skim_dcw4_isa_name(have[i])); }
  printf("\n");
  CHECK(have[nisa - 1] == SKIM_DCW4_ISA_GENERIC, "generic is always available");
  SkimDcw4Net *net = skim_dcw4_net_new_builtin(SKIM_DCW4_ISA_AUTO, &err);
  CHECK(net != NULL, "the built-in blob loads: %s", err ? err->message : "");
  if (!net) { return 1; }
  CHECK(skim_dcw4_net_isa(net) == have[0], "auto takes the best encoder");
  const char *shared_err = NULL;
  CHECK(skim_dcw4_net_shared(&shared_err) != NULL, "the shared net loads: %s", shared_err);
  CHECK(skim_dcw4_output_frames(437) == 219 && skim_dcw4_output_frames(438) == 219,
        "stride 2, ceil");
  SkimDcw4Isa isa;
  CHECK(skim_dcw4_isa_from_name("avx2", &isa) && isa == SKIM_DCW4_ISA_AVX2 &&
        !skim_dcw4_isa_from_name("neon", &isa), "ISA names");

  GBytes *bytes = g_resources_lookup_data("/skimmer-for-linux/dcw4/deepcw-v4-narrow.bin", 0, &err);
  CHECK(bytes != NULL, "the blob is a registered resource");
  if (bytes) {
    gsize len = 0;
    const guint8 *orig = g_bytes_get_data(bytes, &len);
    guint8 *copy = g_memdup2(orig, len);
    SkimDcw4Net *n2 = skim_dcw4_net_new(copy, len, TRUE, SKIM_DCW4_ISA_GENERIC, NULL);
    CHECK(n2 != NULL, "a copy loads on generic");
    skim_dcw4_net_free(n2);
    copy[len / 2] ^= 1;
    g_clear_error(&err);
    n2 = skim_dcw4_net_new(copy, len, TRUE, SKIM_DCW4_ISA_AUTO, &err);
    CHECK(n2 == NULL && err && strstr(err->message, "sha256"), "one byte changed is refused");
    copy[len / 2] ^= 1;
    copy[0] = 'X';
    g_clear_error(&err);
    n2 = skim_dcw4_net_new(copy, len, FALSE, SKIM_DCW4_ISA_AUTO, &err);
    CHECK(n2 == NULL && err && strstr(err->message, "not a DeepCW"), "another magic is refused");
    copy[0] = 'D';
    g_clear_error(&err);
    n2 = skim_dcw4_net_new(copy, 1000, FALSE, SKIM_DCW4_ISA_AUTO, &err);
    CHECK(n2 == NULL && err, "a cut copy is refused");
    g_clear_error(&err);
    g_free(copy);
    g_bytes_unref(bytes);
  }

  /* ---- D's runtime, if any */
  const char *onnx = g_getenv("SKIM_DCW4_ONNX");
  SkimOrt *ort = NULL;
  SkimOrtSession *sess = NULL;
  if (onnx && *onnx) {
    gchar *data = NULL;
    gsize dlen = 0;
    gchar *sha = NULL;
    if (g_file_get_contents(onnx, &data, &dlen, NULL)) {
      sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data, dlen);
    }
    CHECK(sha && strcmp(sha, SKIM_DCW4_MODEL_SHA256) == 0, "%s is the pinned model_narrow.onnx",
          onnx);
    g_free(sha);
    g_free(data);
    ort = skim_ort_open(NULL, &err);
    if (ort) { sess = skim_ort_session_new(ort, onnx, 1, "cpu", &err); }
    CHECK(sess != NULL, "ONNX Runtime runs %s: %s", onnx, err ? err->message : "");
    g_clear_error(&err);
    if (sess) { printf("D: ONNX Runtime %s, %s\n", skim_ort_version(ort), skim_ort_library(ort)); }
  } else {
    printf("D: SKIP (SKIM_DCW4_ONNX not set)\n");
  }

  /* ---- B, D */
  double worst[3] = { 0, 0, 0 }, worst_ort = 0;
  guint ort_runs = 0;
  for (guint c = 0; c < G_N_ELEMENTS(cases); c++) {
    const Case *k = &cases[c];
    gboolean *key = g_new(gboolean, k->T);
    keying(TEXT TEXT TEXT TEXT TEXT TEXT, k->wpm, 3, key, k->T);
    float *tile = g_new(float, (gsize)k->T * 3);
    make_tile(tile, key, k->T, k->snr, k->seed);
    const guint T1 = skim_dcw4_output_frames(k->T);
    const gsize n = (gsize)T1 * SKIM_DCW4_CLASSES;
    float *ref = g_new(float, n), *got = g_new(float, n);
    skim_dcw4_net_run(net, tile, k->T, ref, SKIM_DCW4_ISA_GENERIC);
    char *ref_text = greedy(ref, T1);
    for (guint i = 0; i < nisa; i++) {
      skim_dcw4_net_run(net, tile, k->T, got, have[i]);
      const double d = max_pdiff(ref, got, n);
      char *text = greedy(got, T1);
      worst[i] = MAX(worst[i], d);
      CHECK(d <= 1e-4, "T %u %s: |Δ p| %.2e against generic", k->T,
            skim_dcw4_isa_name(have[i]), d);
      CHECK(strcmp(text, ref_text) == 0, "T %u %s: greedy text differs", k->T,
            skim_dcw4_isa_name(have[i]));
      g_free(text);
    }
    if (sess) {
      const int64_t dims[4] = { 1, 1, k->T, 3 };
      float *out = NULL;
      int64_t od[8];
      int ond = 0;
      if (skim_ort_run(sess, tile, dims, 4, &out, od, &ond, &err)) {
        CHECK(ond == 3 && od[1] == T1 && od[2] == SKIM_DCW4_CLASSES, "T %u: ORT's shape", k->T);
        skim_dcw4_net_run(net, tile, k->T, got, SKIM_DCW4_ISA_AUTO);
        const double d = max_pdiff(out, got, n);
        char *ot = greedy(out, T1), *gt = greedy(got, T1);
        worst_ort = MAX(worst_ort, d);
        ort_runs++;
        CHECK(d <= 1e-4, "T %u: |Δ p| %.2e against ONNX Runtime", k->T, d);
        CHECK(strcmp(ot, gt) == 0, "T %u: greedy text differs from ONNX Runtime's", k->T);
        g_free(ot);
        g_free(gt);
        g_free(out);
      } else {
        CHECK(FALSE, "T %u: ORT run: %s", k->T, err->message);
        g_clear_error(&err);
      }
    }
    printf("B: T %4u %4.0f WPM %+3.0f dB/bin: \"%.48s\"\n", k->T, k->wpm, k->snr, ref_text);
    g_free(ref_text);
    g_free(ref);
    g_free(got);
    g_free(tile);
    g_free(key);
  }
  for (guint i = 0; i < nisa; i++) {
    printf("B: %s, worst |Δ p| against generic %.2e\n", skim_dcw4_isa_name(have[i]), worst[i]);
  }
  if (sess) { printf("D: %u windows, worst |Δ p| against ONNX Runtime %.2e\n", ort_runs, worst_ort); }

  /* ---- C */
  {
    const guint T = 750;               /* 12 s */
    gboolean key[750];
    const gboolean fits = keying("CQ DE OK1HRA", 22, 40, key, T);
    CHECK(fits, "the text fits 12 s");
    float tile[750 * 3], logp[375 * SKIM_DCW4_CLASSES];
    make_tile(tile, key, T, 20, 99);
    for (guint i = 0; i < nisa; i++) {
      skim_dcw4_net_run(net, tile, T, logp, have[i]);
      char *text = greedy(logp, skim_dcw4_output_frames(T));
      CHECK(strcmp(text, "CQ DE OK1HRA") == 0, "%s reads \"%s\", not CQ DE OK1HRA",
            skim_dcw4_isa_name(have[i]), text);
      g_free(text);
    }
    make_tile(tile, NULL, T, 20, 100);
    skim_dcw4_net_run(net, tile, T, logp, SKIM_DCW4_ISA_AUTO);
    char *text = greedy(logp, skim_dcw4_output_frames(T));
    CHECK(text[0] == '\0', "noise reads \"%s\", not nothing", text);
    g_free(text);
  }

  if (sess) { skim_ort_session_free(sess); }
  if (ort) { skim_ort_close(ort); }
  skim_dcw4_net_free(net);
  printf("dcw4-kernel: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
