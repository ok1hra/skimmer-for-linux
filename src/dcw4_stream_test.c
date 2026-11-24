/* dcw4_stream_test.c — gate dcw4-stream: CW v4 from the network's output on.
 *
 *  N. The model's input normalisation (noise_q90_log1p).
 *  A. The posterior stitcher on synthetic CTC streams (SDRIAK's cases): one
 *     final window, overlapping windows (nothing doubled or lost, jittered
 *     windows read the same), the 2.01 s word gap, restart; the committed
 *     frames' rows readable for the call reader, the pending ones not.
 *  R. The call reader: CTC's score, a call the greedy reading dropped a
 *     character of (in MASTER.SCP) comes back, an ambiguous one does not,
 *     a call MASTER.SCP does not know is never written in, a torn word.
 *  B. The backend on synthetic keyed IQ (inference inline): the gate, the
 *     tone offset, whole words with their spaces, the over break after the
 *     over, a TX hold's final window, nothing from noise.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/callsign.h"
#include "engine/dcw4_callread.h"
#include "engine/dcw4_kernel.h"
#include "engine/dcw4_stitch.h"
#include "engine/decode_cw_v4.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define NC    SKIM_DCW4_CLASSES
#define BLANK SKIM_DCW4_BLANK
#define SPACE SKIM_DCW4_SPACE

static int g_fail = 0;
static void check(const char *what, gboolean ok) {
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) { g_fail++; }
}

static int cls_of(char c) {
  if (c == ' ') { return SPACE; }
  const char *p = strchr(SKIM_DCW4_ALPHABET, c);
  return p ? (int)(p - SKIM_DCW4_ALPHABET) : -1;
}

/* log-probs [T][42]: blank everywhere */
static float *blank_stream(gsize T) {
  float *m = g_new(float, T * NC);
  for (gsize t = 0; t < T; t++) {
    for (int c = 0; c < NC; c++) { m[t * NC + c] = logf(0.05f / 41.0f); }
    m[t * NC + BLANK] = logf(0.95f);
  }
  return m;
}

/* frame t: cls at p, blank at pb, the rest share what is left */
static void mix(float *m, gsize t, int cls, float p, float pb) {
  const float rest = (1.0f - p - pb) / 40.0f;
  for (int c = 0; c < NC; c++) { m[t * NC + c] = logf(MAX(rest, 1e-9f)); }
  m[t * NC + cls] = logf(p);
  m[t * NC + BLANK] = logf(MAX(pb, 1e-9f));
}

static void spike(float *m, gsize t, int cls) { mix(m, t, cls, 0.95f, 0.0f); }

/* "text" as two-frame spikes every `step` frames; ' ' = a space spike */
static gsize write_text(float *m, gsize t0, gsize step, const char *text) {
  gsize t = t0;
  for (const char *s = text; *s; s++, t += step) {
    spike(m, t, cls_of(*s));
    spike(m, t + 1, cls_of(*s));
  }
  return t;
}

static void emit(GString *text, gboolean *has, const SkimDcw4Take *t) {
  GString *w = g_string_new(NULL);
  gboolean pend_space = FALSE;
  for (guint i = 0; i < t->n_spans; i++) {
    if (t->spans[i].cls == SPACE) { if (w->len) { pend_space = TRUE; } continue; }
    if (pend_space) { g_string_append_c(w, ' '); pend_space = FALSE; }
    g_string_append_c(w, SKIM_DCW4_ALPHABET[t->spans[i].cls]);
  }
  if (w->len) {
    if (t->starts_word && *has) { g_string_append_c(text, ' '); }
    g_string_append(text, w->str);
    *has = TRUE;
  }
  g_string_free(w, TRUE);
}

/* the channel's way: a window every `step` frames ending at the current
 * frame (shifted by `jitter` frames on odd windows), then the final one */
static char *stream(SkimDcw4Stitch *st, const float *g, gsize T, gint64 step, int jitter,
                    gboolean *pending_seen) {
  GString *text = g_string_new(NULL);
  gboolean has = FALSE;
  SkimDcw4Take t;
  int k = 0;
  for (gint64 end = 2 * step; end <= (gint64)T; end += step, k++) {
    const gint64 s = skim_dcw4_stitch_window_start(st, end);
    const gsize n = (gsize)(end - s);
    float *w = g_memdup2(g + s * NC, n * NC * sizeof(float));
    if (jitter && (k & 1)) {
      float *sh = blank_stream(n);
      for (gsize f = 0; f + jitter < n; f++) { memcpy(sh + (f + jitter) * NC, w + f * NC, NC * sizeof(float)); }
      g_free(w);
      w = sh;
    }
    skim_dcw4_stitch_add_window(st, s, w, (guint)n, FALSE);
    g_free(w);
    skim_dcw4_stitch_take(st, FALSE, &t);
    emit(text, &has, &t);
    if (pending_seen && t.pending[0]) { *pending_seen = TRUE; }
  }
  const gint64 s = skim_dcw4_stitch_window_start(st, (gint64)T);
  skim_dcw4_stitch_add_window(st, s, g + s * NC, (guint)((gint64)T - s), TRUE);
  skim_dcw4_stitch_take(st, TRUE, &t);
  emit(text, &has, &t);
  return g_string_free(text, FALSE);
}

static char *final_text(SkimDcw4Stitch *st, gint64 at, const float *g, guint T) {
  skim_dcw4_stitch_add_window(st, at, g, T, TRUE);
  SkimDcw4Take t;
  skim_dcw4_stitch_take(st, TRUE, &t);
  GString *s = g_string_new(NULL);
  gboolean has = FALSE;
  emit(s, &has, &t);
  if (t.starts_word) { g_string_prepend_c(s, '^'); }
  return g_string_free(s, FALSE);
}

/* ---- a gap the channel did not decode (windows stop while the line is quiet) ---- */

static guint g_rows_max;

static void digest_take(GChecksum *ck, SkimDcw4Stitch *st, const SkimDcw4Take *t) {
  g_rows_max = MAX(g_rows_max, skim_dcw4_stitch_stored_rows(st));
  GString *d = g_string_new(NULL);
  g_string_append_printf(d, "n%u sw%d ew%d p|%s| psw%d c%" G_GINT64_FORMAT " b%" G_GINT64_FORMAT " f%"
                         G_GINT64_FORMAT " v%" G_GINT64_FORMAT ";",
                         t->n_spans, t->starts_word, t->ends_word, t->pending, t->pending_starts_word,
                         skim_dcw4_stitch_committed_end(st), skim_dcw4_stitch_blank_frames(st),
                         skim_dcw4_stitch_frozen_end(st), skim_dcw4_stitch_covered_end(st));
  for (guint i = 0; i < t->n_spans; i++) {
    float p = t->spans[i].p;
    guint32 pb;
    memcpy(&pb, &p, 4);
    g_string_append_printf(d, "%u:%" G_GINT64_FORMAT "-%" G_GINT64_FORMAT ":%08x,", t->spans[i].cls,
                           t->spans[i].start, t->spans[i].end, pb);
  }
  g_checksum_update(ck, (const guchar *)d->str, (gssize)d->len);
  const gint64 ce = skim_dcw4_stitch_committed_end(st);
  for (gint64 f = ce - 400; f < ce + 4; f++) {
    const float *r = skim_dcw4_stitch_row(st, f);
    g_checksum_update(ck, r ? (const guchar *)r : (const guchar *)"-", r ? NC * sizeof(float) : 1);
  }
  g_string_free(d, TRUE);
}

/* windows every `step` frames over [from, to) of g (absolute frames of a
 * stream of `len` frames; g covers it), none final: an over the gate ends */
static void gap_over(SkimDcw4Stitch *st, GChecksum *ck, GString *text, gboolean *has,
                     const float *g, gint64 from, gint64 to, gint64 step) {
  SkimDcw4Take t;
  for (gint64 end = from + 2 * step; end <= to; end += step) {
    const gint64 s = skim_dcw4_stitch_window_start(st, end);
    skim_dcw4_stitch_add_window(st, s, g + s * NC, (guint)(end - s), FALSE);
    g_rows_max = MAX(g_rows_max, skim_dcw4_stitch_stored_rows(st));
    skim_dcw4_stitch_take(st, FALSE, &t);
    emit(text, has, &t);
    digest_take(ck, st, &t);
  }
}

/* one case: over A at the stream's start or late, then over B `gap` frames
 * later; `edge`: B's first character on the first frame of its window */
static char *gap_case(const SkimDcw4StitchConfig *sc, double fps, gint64 a_at, gint64 gap,
                      gboolean edge, GChecksum *ck) {
  const gint64 step = (gint64)llround(sc->step_s * fps);
  const gint64 seg = (gint64)llround(sc->max_segment_s * fps);
  const gint64 a_end = a_at + 300, b0 = a_end + gap, len = b0 + 400;
  float *g = blank_stream((gsize)len);
  write_text(g, (gsize)a_at + 40, 8, "OK1HRA");
  /* a character in the last frames of A: still pending when its windows stop */
  spike(g, (gsize)a_end - 20, cls_of('K'));
  spike(g, (gsize)a_end - 19, cls_of('K'));
  const gint64 b_first = b0 + 2 * step - seg;     /* B's first window starts here */
  write_text(g, (gsize)(edge ? b_first : b_first + 30), 8, "5NN TU");
  GString *text = g_string_new(NULL);
  gboolean has = FALSE;
  SkimDcw4Stitch *st = skim_dcw4_stitch_new(sc, fps);
  if (a_at > 0) { skim_dcw4_stitch_restart(st, 0); }
  gap_over(st, ck, text, &has, g, a_at, a_end, step);
  gap_over(st, ck, text, &has, g, b0, len, step);
  SkimDcw4Take t;
  const gint64 s = skim_dcw4_stitch_window_start(st, len);
  skim_dcw4_stitch_add_window(st, s, g + s * NC, (guint)(len - s), TRUE);
  skim_dcw4_stitch_take(st, TRUE, &t);
  emit(text, &has, &t);
  digest_take(ck, st, &t);
  g_free(g);
  skim_dcw4_stitch_free(st);
  return g_string_free(text, FALSE);
}

/* ---- synthetic baseband ---------------------------------------------------------- */

static guint64 g_rng = 1;
static double urand(void) {
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
  return NULL;
}

/* key[n] at rate: text at wpm from `lead` s */
static guint keying(gboolean *key, guint n, double rate, double wpm, double lead, const char *text) {
  const double dot = 1.2 / wpm * rate;
  double t = lead * rate;
  memset(key, 0, n * sizeof(gboolean));
  for (const char *p = text; *p; p++) {
    if (*p == ' ') { t += 4 * dot; continue; }
    for (const char *e = morse(*p); e && *e; e++) {
      const double len = (*e == '-' ? 3 : 1) * dot;
      for (guint i = (guint)t; i < (guint)(t + len) && i < n; i++) { key[i] = TRUE; }
      t += len + dot;
    }
    t += 2 * dot;
  }
  return (guint)t;
}

/* complex baseband [n][2]: the keyed tone at off_hz (soft edges), amplitude
 * a over unit complex noise */
static float *baseband(const gboolean *key, guint n, double rate, double off_hz, double a) {
  float *iq = g_new(float, 2 * (gsize)n);
  double env = 0;
  for (guint i = 0; i < n; i++) {
    env += (((key && key[i]) ? 1.0 : 0.0) - env) * 0.35;
    const double ph = 2.0 * G_PI * off_hz * i / rate;
    iq[2 * i] = (float)(a * env * cos(ph) + gauss() * sqrt(0.5));
    iq[2 * i + 1] = (float)(a * env * sin(ph) + gauss() * sqrt(0.5));
  }
  return iq;
}

typedef struct { GString *text, *aux; guint events, ops; double off_worst, wpm; } Out;

static void feed(const SkimDecodeBackend *be, gpointer ch, const float *iq, guint n, Out *o) {
  for (guint i = 0; i < n; i += 64) {
    SkimDecode d;
    const guint k = MIN(64u, n - i);
    if (be->process(ch, iq + 2 * (gsize)i, k, &d)) {
      g_string_append(o->text, d.text);
      o->events++;
      o->off_worst = MAX(o->off_worst, fabs(d.freq_offset_hz - 30.0));
      o->wpm = d.speed;
    }
    SkimPaneOp op;
    while (be->take_pane_op(ch, &op)) {
      o->ops++;
      g_free(op.text);
      g_free(op.fresh);
    }
    char *aux = be->take_aux_text(ch);
    if (aux) { g_string_append(o->aux, aux); g_free(aux); }
  }
}

static void drain(const SkimDecodeBackend *be, gpointer ch, Out *o) {
  for (int i = 0; i < 50; i++) {
    SkimDecode d;
    if (be->process(ch, NULL, 0, &d)) {
      g_string_append(o->text, d.text);
      o->events++;
    }
  }
}

int main(void) {
  /* ---- N */
  printf("[N] normalisation\n");
  {
    float x[1000];
    g_rng = 7;
    for (int i = 0; i < 1000; i++) { x[i] = (float)(2.0 * sqrt(-2.0 * log(urand()))); }
    float s[1000];
    memcpy(s, x, sizeof s);
    skim_dcw4_normalize(x, 1000);
    float sorted[1000];
    memcpy(sorted, x, sizeof x);
    for (int i = 1; i < 1000; i++) {                 /* insertion sort, small */
      float v = sorted[i]; int j = i - 1;
      while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
      sorted[j + 1] = v;
    }
    check("the 90th percentile lands on log1p(sqrt(-2 ln 0.1))",
          fabs(sorted[(int)floor(0.9 * 999)] - log1p(sqrt(-2.0 * log(0.1)))) < 1e-3);
    float z[16] = { 0 };
    skim_dcw4_normalize(z, 16);
    gboolean zero = TRUE;
    for (int i = 0; i < 16; i++) { zero = zero && z[i] == 0.0f; }
    check("all zero stays zero (floor 1e-5)", zero);
    float p[16] = { 0 };
    p[7] = 1.0f;
    skim_dcw4_normalize(p, 16);
    check("a lone peak reads against peak·1e-3", fabs(p[7] - log1pf(1000.0f)) < 1e-3);
  }

  /* ---- A */
  printf("[A] posterior stitcher\n");
  SkimDcw4StitchConfig sc;
  skim_dcw4_stitch_config_default(&sc);
  const double fps = 250.0 / 4 / 2;
  {
    SkimDcw4Stitch *st = skim_dcw4_stitch_new(&sc, fps);
    float *g = blank_stream(200);
    write_text(g, 20, 8, "CQ DE");
    char *t = final_text(st, 0, g, 200);
    check("one final window commits everything", strcmp(t, "^CQ DE") == 0);
    check("committed frames are readable", skim_dcw4_stitch_row(st, 25) != NULL &&
          fabsf(skim_dcw4_stitch_row(st, 20)[cls_of('C')] - logf(0.95f)) < 0.2f);
    g_free(t); g_free(g); skim_dcw4_stitch_free(st);
  }
  for (int setting = 0; setting < 2; setting++) {
    SkimDcw4StitchConfig c = sc;
    if (setting == 1) { c.left_context_s = 4; c.step_s = 1; }        /* upstream's */
    const gsize T = 1200;
    float *g = blank_stream(T);
    gsize t = write_text(g, 40, 7, "CQ CQ DE OK1HRA OK1HRA K ");
    t = write_text(g, t + 30, 7, "TEST DE OK1HRA");
    write_text(g, t + 200, 7, "5NN TU");
    SkimDcw4Stitch *a = skim_dcw4_stitch_new(&c, fps), *b = skim_dcw4_stitch_new(&c, fps);
    gboolean pend = FALSE;
    const gint64 step = llround(c.step_s * fps);
    char *ra = stream(a, g, T, step, 0, &pend);
    char *rb = stream(b, g, T, step, 1, NULL);
    printf("      L%.0f S%.0f steady |%s|\n      jitter |%s|\n", c.left_context_s, c.step_s, ra, rb);
    check(setting ? "upstream 4, 2, 1: nothing doubled, nothing lost"
                  : "L1 G2 S3: nothing doubled, nothing lost",
          strcmp(ra, "CQ CQ DE OK1HRA OK1HRA K TEST DE OK1HRA 5NN TU") == 0);
    check("windows jittered by a frame read the same (dilation)", strcmp(ra, rb) == 0);
    check("pending text was offered before the commit", pend);
    check("pending frames are not readable", skim_dcw4_stitch_row(a, (gint64)T + 5) == NULL);
    g_free(ra); g_free(rb); g_free(g);
    skim_dcw4_stitch_free(a); skim_dcw4_stitch_free(b);
  }
  {
    SkimDcw4Stitch *st = skim_dcw4_stitch_new(&sc, fps);
    float *g = blank_stream(400);
    spike(g, 20, cls_of('A')); spike(g, 21, cls_of('A'));
    spike(g, 100, cls_of('B'));                    /* 80 frames = 2.56 s ≥ 2.01 s */
    spike(g, 120, cls_of('C'));                    /* 0.64 s: no gap */
    char *t = final_text(st, 0, g, 400);
    check("a blank run ≥ 2.01 s is a word gap, a shorter one is not", strcmp(t, "^A BC") == 0);
    g_free(t); g_free(g); skim_dcw4_stitch_free(st);
  }
  {
    SkimDcw4Stitch *st = skim_dcw4_stitch_new(&sc, fps);
    float *g = blank_stream(100);
    write_text(g, 10, 8, "OK1HRA");
    g_free(final_text(st, 0, g, 100));
    skim_dcw4_stitch_restart(st, 500);
    check("restart forgets the rows before it", skim_dcw4_stitch_row(st, 20) == NULL);
    float *h = blank_stream(100);
    write_text(h, 10, 8, "5NN");
    char *t = final_text(st, 500, h, 100);
    check("after restart the text starts a new word", strcmp(t, "^5NN") == 0);
    g_free(t); g_free(g); g_free(h); skim_dcw4_stitch_free(st);
  }

  {
    /* an over, then two hours of a quiet line (no windows), then the next
     * over: the read is the same as ever — the digest of every take (its
     * spans, posteriors, pending text, counters and the call reader's
     * rows) is v0.8.13's, whose stitcher stored a zero row for every frame
     * of the gap — and no row of the gap is stored (25 GB on hra2,
     * 2026-11-24) */
    const gint64 two_h = (gint64)llround(2 * 3600 * fps);
    struct { gint64 a_at, gap; gboolean edge; const char *text; } cs[] = {
      { 0, two_h, FALSE, "OK1HRA K 5NN TU" }, { 0, two_h, TRUE, "OK1HRA K 5NN TU" },
      { 5000, two_h, FALSE, "OK1HRA K 5NN TU" }, { 0, 700, FALSE, "OK1HRA K 5NN TU" },
      { 0, 90, TRUE, "OK1HRA 5NN TUK" },
    };
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    gboolean texts = TRUE;
    guint rows_gap = 0;
    for (guint i = 0; i < G_N_ELEMENTS(cs); i++) {
      g_rows_max = 0;
      char *t = gap_case(&sc, fps, cs[i].a_at, cs[i].gap, cs[i].edge, ck);
      texts = texts && strcmp(t, cs[i].text) == 0;
      if (cs[i].gap == two_h) { rows_gap = MAX(rows_gap, g_rows_max); }
      g_free(t);
    }
    printf("      most rows stored across a 2 h gap: %u\n", rows_gap);
    check("a gap of 2 h between overs: both read", texts);
    check("…every take as before the gap stopped being stored",
          strcmp(g_checksum_get_string(ck),
                 "ed04757f11f6b4ecfd806867c363b3a549bbf87170242f0b6d83d8fff9f79d8c") == 0);
    check("…and no row of the gap is stored (≤ 2 windows' rows)",
          rows_gap <= 2 * (guint)llround(sc.max_segment_s * fps));
    g_checksum_free(ck);
  }

  /* ---- R */
  printf("[R] call reader\n");
  {
    char *dir = g_dir_make_tmp("dcw4-XXXXXX", NULL);
    char *scp = g_build_filename(dir, "master.scp", NULL);
    g_file_set_contents(scp, "# test\nOK1HRA\nOK1HRB\nOK1HRC\nDL1ABC\nH1SH\nOK2ZZ\n", -1, NULL);
    GError *err = NULL;
    check("the test dictionary loads", skim_callsign_dict_load(scp, &err));
    g_clear_error(&err);

    float *g = blank_stream(80);
    write_text(g, 4, 5, "OK1HR");                  /* frames 4..29 */
    const float *rows[80];
    for (int i = 0; i < 80; i++) { rows[i] = g + (gsize)i * NC; }
    const double s_raw = skim_dcw4_ctc_score(rows, 50, "OK1HR");
    check("CTC: the text spiked scores far above another", s_raw > skim_dcw4_ctc_score(rows, 50, "OK1HB") + 5);
    check("CTC: a label longer than the frames cannot fit",
          skim_dcw4_ctc_score(rows, 3, "OK1HR") == -INFINITY);
    SkimDcw4CallreadConfig cfg;
    skim_dcw4_callread_config_default(&cfg);
    SkimDcw4CallreadResult res;
    check("a clean OK1HR stays (no evidence for a sixth character)",
          !skim_dcw4_callread(&cfg, rows, 50, "OK1HR", &res));
    /* a weak A the greedy reading dropped: 0.3 under blank 0.7 for 12 frames */
    for (int t = 30; t < 42; t++) { mix(g, t, cls_of('A'), 0.3f, 0.7f); }
    const gboolean won = skim_dcw4_callread(&cfg, rows, 50, "OK1HR", &res);
    printf("      OK1HR + weak A: %s margin %.2f (%u candidates)\n", won ? res.call : "-",
           res.margin, res.candidates);
    check("a dropped weak character: the dictionary's OK1HRA comes back",
          won && strcmp(res.call, "OK1HRA") == 0);
    /* A and B as weak as each other: no winner by 2 nats */
    for (int t = 30; t < 42; t++) {
      mix(g, t, BLANK, 0.7f, 0.7f);
      g[(gsize)t * NC + cls_of('A')] = logf(0.15f);
      g[(gsize)t * NC + cls_of('B')] = logf(0.15f);
    }
    check("A as likely as B: neither is written in",
          !skim_dcw4_callread(&cfg, rows, 50, "OK1HR", &res));
    /* a call the dictionary does not know is never written in */
    float *h = blank_stream(80);
    write_text(h, 4, 5, "OK9QQ");
    for (int t = 30; t < 42; t++) { mix(h, t, cls_of('X'), 0.3f, 0.7f); }
    for (int i = 0; i < 80; i++) { rows[i] = h + (gsize)i * NC; }
    check("OK9QQX is not in MASTER.SCP: nothing written in",
          !skim_dcw4_callread(&cfg, rows, 50, "OK9QQ", &res));
    /* a torn word: "H 1SH" read, the frames hold no word gap at all */
    float *j = blank_stream(80);
    write_text(j, 4, 5, "H1SH");
    for (int i = 0; i < 80; i++) { rows[i] = j + (gsize)i * NC; }
    for (int t = 0; t < 80; t++) { j[(gsize)t * NC + SPACE] = logf(1e-6f); }
    const gboolean jw = skim_dcw4_callread(&cfg, rows, 40, "H 1SH", &res);
    check("a torn word with no gap in the frames: the dictionary's H1SH", jw && strcmp(res.call, "H1SH") == 0);
    g_free(g); g_free(h); g_free(j);
    g_unlink(scp);
    g_rmdir(dir);
    g_free(scp); g_free(dir);
  }

  /* ---- B */
  printf("[B] the backend\n");
  g_setenv("SKIM_DCW4_SYNC", "1", TRUE);
  {
    GError *err = NULL;
    check("the network is available", skim_decode_cw_v4_available(&err));
    g_clear_error(&err);
    const SkimDecodeBackend *be = skim_decode_cw_v4();
    const double rate = 250.0;
    const guint n = (guint)(40 * rate);
    gboolean *key = g_new(gboolean, n);
    const guint end = keying(key, n, rate, 24, 2.0, "CQ CQ DE OK1HRA OK1HRA K");
    printf("      the over ends at %.1f s of 40\n", end / rate);
    g_rng = 11;
    float *iq = baseband(key, n, rate, 30.0, 6.0);      /* ~+15 dB in a bin */
    gpointer ch = be->channel_new(rate);
    Out o = { g_string_new(NULL), g_string_new(NULL), 0, 0, 0, 0 };
    feed(be, ch, iq, n, &o);
    printf("      text |%s| (%u events, %u pane ops)\n", o.text->str, o.events, o.ops);
    check("reads the over: CQ CQ DE OK1HRA OK1HRA K", strstr(o.text->str, "CQ CQ DE OK1HRA OK1HRA K") != NULL);
    check("every word goes out with its space", o.text->len && o.text->str[o.text->len - 1] == ' ');
    check("the over break after the over", strstr(o.text->str, "K \xC2\xB7 ") != NULL);
    check("the pane got a draft", o.ops > 0);
    printf("      offset %.1f Hz, level %.1f\n", be->tone_offset_hz(ch), be->level(ch));
    check("+30 Hz tone → offset +30 ± 4 Hz", fabs(be->tone_offset_hz(ch) - 30.0) < 4.0);
    check("every word carries the tone's offset (± 4 Hz), the over break too", o.off_worst < 4.0);
    printf("      speed %.1f WPM\n", o.wpm);
    check("keyed at 24 WPM → 24 ± 4 WPM", fabs(o.wpm - 24.0) < 4.0);
    be->channel_free(ch);

    /* a TX hold right after the call: its final window reads the tail */
    const guint n2 = (guint)(12 * rate);
    keying(key, n2, rate, 24, 2.0, "TEST OK1HRA");
    float *iq2 = baseband(key, n2, rate, -20.0, 6.0);
    ch = be->channel_new(rate);
    Out o2 = { g_string_new(NULL), g_string_new(NULL), 0, 0, 0, 0 };
    const guint cut = keying(key, n2, rate, 24, 2.0, "TEST OK1HRA") + (guint)(0.3 * rate);
    feed(be, ch, iq2, MIN(cut, n2), &o2);
    be->hold_begin(ch);
    drain(be, ch, &o2);
    printf("      hold |%s|\n", o2.text->str);
    check("TX hold: the final window commits the call", strstr(o2.text->str, "OK1HRA ") != NULL);
    be->channel_free(ch);

    /* noise alone */
    g_rng = 12;
    float *nz = baseband(NULL, n, rate, 0.0, 0.0);
    ch = be->channel_new(rate);
    Out o3 = { g_string_new(NULL), g_string_new(NULL), 0, 0, 0, 0 };
    feed(be, ch, nz, n, &o3);
    check("noise alone: no text", o3.text->len == 0);
    be->channel_free(ch);
    SkimCwV4Stats stt;
    skim_decode_cw_v4_stats(&stt, TRUE);
    printf("      %" G_GUINT64_FORMAT " windows, %" G_GUINT64_FORMAT " ticks, %.1f ms CPU a window\n",
           stt.windows, stt.ticks, stt.windows ? stt.cpu_ns / 1e6 / stt.windows : 0.0);
    check("the counters count", stt.windows > 0 && stt.ticks >= stt.windows - 2);
    g_free(key); g_free(iq); g_free(iq2); g_free(nz);
    g_string_free(o.text, TRUE); g_string_free(o.aux, TRUE);
    g_string_free(o2.text, TRUE); g_string_free(o2.aux, TRUE);
    g_string_free(o3.text, TRUE); g_string_free(o3.aux, TRUE);
  }

  printf("dcw4-stream: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
