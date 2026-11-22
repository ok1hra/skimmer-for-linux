/* decode_cw_v4.c — see decode_cw_v4.h.
 *
 * The channel's front end, gate, SNR and stitcher glue follow SDRIAK's
 * DeepCW v2 channel (decode_deepcw2.cpp, bubnikv/sdriak 850a08d), whose
 * front end is the DeepCW v1 backend's (decode_deepcw.c here, OK1BR) and
 * whose SNR is OK1BR 4db0483 (gh#27). What is CW v4's own: words held
 * until they close, the call reader on them, the over break, the event
 * queue, the worker pool and its load counters.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw_v4.h"
#include "callsign.h"
#include "dcw4_callread.h"
#include "dcw4_kernel.h"
#include "dcw4_stitch.h"
#include "snr_meter.h"

#include <gio/gio.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KEEP     11          /* channel bins −5..+5 */
#define HALF     5
#define BIN_HZ   12.5
#define STRIDE   2           /* input frames per output frame */
#define NC       SKIM_DCW4_CLASSES
#define BREAK_MARK "\xC2\xB7 "          /* "· " the over break (v1, v3)  */
#define CALL_MARGIN_F 4                 /* output frames around a word    */
#define JOIN_GAP_S    1.0               /* a torn call's pieces at most   */

/* ---- switches --------------------------------------------------------------- */

typedef struct {
  double gate_db;
  gboolean sync, draft, callread;
  int    debug, workers;
  double drop_p, break_s, call_sep;
  SkimDcw4StitchConfig stitch;
} Tune;

static Tune  g_tune;
static gsize g_tune_once;
static guint g_workers_cfg;            /* skim_decode_cw_v4_set_workers */

static double env_d(const char *name, double dflt) {
  const char *v = g_getenv(name);
  return v && *v ? g_ascii_strtod(v, NULL) : dflt;
}

static void tune_init(void) {
  if (!g_once_init_enter(&g_tune_once)) { return; }
  Tune *t = &g_tune;
  skim_dcw4_stitch_config_default(&t->stitch);
  t->gate_db = env_d("SKIM_DCW4_GATE_DB", 6.0);
  t->sync = env_d("SKIM_DCW4_SYNC", 0) != 0;
  t->draft = env_d("SKIM_DCW4_DRAFT", 1) != 0;
  t->callread = env_d("SKIM_DCW4_CALLREAD", 1) != 0;
  t->debug = (int)env_d("SKIM_DCW4_DEBUG", 0);
  t->workers = (int)env_d("SKIM_DCW4_WORKERS", 0);
  t->drop_p = CLAMP(env_d("SKIM_DCW4_DROP_P", 0.0), 0.0, 1.0);
  t->break_s = MAX(env_d("SKIM_DCW4_BREAK", 3.0), 0.5);
  t->call_sep = env_d("SKIM_DCW4_CALL_SEP", 2.0);
  t->stitch.step_s = CLAMP(env_d("SKIM_DCW4_STEP", t->stitch.step_s), 0.25, 8.0);
  t->stitch.left_context_s = CLAMP(env_d("SKIM_DCW4_LEFT", t->stitch.left_context_s), 0.0, 8.0);
  t->stitch.freeze_guard_s = CLAMP(env_d("SKIM_DCW4_GUARD", t->stitch.freeze_guard_s), 0.5, 6.0);
  g_once_init_leave(&g_tune_once, 1);
}

/* ---- the worker pool ---------------------------------------------------------- */

/* A channel's mailbox: shared with its queued window, so the channel can
 * go while the window runs. */
typedef struct {
  gint     ref;
  gint     dying;
  GMutex   lock;
  gboolean ready;
  float   *logp;
  guint    To;
  gint64   s;
} Box;

typedef struct {
  Box    *box;
  float  *tile;
  guint   T;
  gint64  s;
  gint64  queued_us;
} Job;

static GThreadPool *g_pool;
static gsize        g_pool_once;
static guint        g_pool_workers;
static GMutex       g_stats_lock;
static SkimCwV4Stats g_stats;

static Box *box_new(void) {
  Box *b = g_new0(Box, 1);
  b->ref = 1;
  g_mutex_init(&b->lock);
  return b;
}

static void box_unref(Box *b) {
  if (!b || !g_atomic_int_dec_and_test(&b->ref)) { return; }
  g_mutex_clear(&b->lock);
  g_free(b->logp);
  g_free(b);
}

static gint64 thread_cpu_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (gint64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void job_run(gpointer data, gpointer user) {
  (void)user;
  Job *j = data;
  const double wait_ms = (g_get_monotonic_time() - j->queued_us) / 1000.0;
  if (!g_atomic_int_get(&j->box->dying)) {
    const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
    const guint To = skim_dcw4_output_frames(j->T);
    float *logp = g_new(float, (gsize)To * NC);
    const gint64 c0 = thread_cpu_ns();
    skim_dcw4_net_run(net, j->tile, j->T, logp, SKIM_DCW4_ISA_AUTO);
    const gint64 dc = thread_cpu_ns() - c0;
    g_mutex_lock(&j->box->lock);
    g_free(j->box->logp);
    j->box->logp = logp;
    j->box->To = To;
    j->box->s = j->s;
    j->box->ready = TRUE;
    g_mutex_unlock(&j->box->lock);
    g_mutex_lock(&g_stats_lock);
    g_stats.windows++;
    g_stats.cpu_ns += (guint64)dc;
    g_stats.wait_max_ms = MAX(g_stats.wait_max_ms, wait_ms);
    g_mutex_unlock(&g_stats_lock);
  }
  box_unref(j->box);
  g_free(j->tile);
  g_free(j);
}

void skim_decode_cw_v4_set_workers(guint n) { g_workers_cfg = n; }

static guint workers_wanted(void) {
  tune_init();
  if (g_tune.workers > 0) { return (guint)MIN(g_tune.workers, 256); }
  if (g_workers_cfg > 0) { return MIN(g_workers_cfg, 256u); }
  return MAX(g_get_num_processors() / 2, 1u);
}

guint skim_decode_cw_v4_workers(void) { return g_pool_workers ? g_pool_workers : workers_wanted(); }

static void pool_ensure(void) {
  if (!g_once_init_enter(&g_pool_once)) { return; }
  g_pool_workers = workers_wanted();
  g_pool = g_thread_pool_new(job_run, NULL, (gint)g_pool_workers, TRUE, NULL);
  g_once_init_leave(&g_pool_once, 1);
}

void skim_decode_cw_v4_stats(SkimCwV4Stats *out, gboolean reset_max) {
  g_mutex_lock(&g_stats_lock);
  *out = g_stats;
  if (reset_max) { g_stats.wait_max_ms = 0; }
  g_mutex_unlock(&g_stats_lock);
  out->queued = g_pool ? g_thread_pool_unprocessed(g_pool) : 0;
  out->workers = skim_decode_cw_v4_workers();
}

typedef struct {
  const SkimDcw4Net *net;
  const float       *tile;
  guint              T;
  gint64             until;
  guint              windows;
} BenchJob;

static gpointer bench_worker(gpointer p) {
  BenchJob *j = p;
  float *logp = g_new(float, (gsize)skim_dcw4_output_frames(j->T) * NC);
  skim_dcw4_net_run(j->net, j->tile, j->T, logp, SKIM_DCW4_ISA_AUTO);   /* warm */
  while (g_get_monotonic_time() < j->until) {
    skim_dcw4_net_run(j->net, j->tile, j->T, logp, SKIM_DCW4_ISA_AUTO);
    j->windows++;
  }
  g_free(logp);
  return NULL;
}

double skim_decode_cw_v4_step_s(void) {
  tune_init();
  return g_tune.stitch.step_s;
}

double skim_decode_cw_v4_bench(guint threads, double secs) {
  const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
  if (!net || threads == 0) { return 0.0; }
  tune_init();
  const SkimDcw4StitchConfig *sc = &g_tune.stitch;
  const guint T = (guint)llround((sc->left_context_s + sc->freeze_guard_s + sc->step_s) * 62.5);
  float *tile = g_new(float, (gsize)T * 3);
  guint64 r = 88172645463325252ull;
  for (guint t = 0; t < T; t++) {                /* dits and dahs over noise */
    const gboolean on = (t / 3) % 4 != 3 && (t / 12) % 3 != 2;
    for (guint b = 0; b < 3; b++) {
      r ^= r << 13; r ^= r >> 7; r ^= r << 17;
      tile[t * 3 + b] = (float)((double)(r >> 11) / 9007199254740992.0 +
                                (on && b == 1 ? 4.0 : on ? 2.0 : 0.0));
    }
  }
  skim_dcw4_normalize(tile, (gsize)T * 3);
  BenchJob *jobs = g_new0(BenchJob, threads);
  GThread **th = g_new(GThread *, threads);
  const gint64 t0 = g_get_monotonic_time();
  for (guint i = 0; i < threads; i++) {
    jobs[i] = (BenchJob){ net, tile, T, t0 + (gint64)(secs * 1e6), 0 };
    th[i] = g_thread_new("cw-v4-bench", bench_worker, &jobs[i]);
  }
  guint64 w = 0;
  for (guint i = 0; i < threads; i++) {
    g_thread_join(th[i]);
    w += jobs[i].windows;
  }
  const double wall = (g_get_monotonic_time() - t0) * 1e-6;
  g_free(jobs);
  g_free(th);
  g_free(tile);
  return wall > 0 ? (double)w / wall : 0.0;
}

gboolean skim_decode_cw_v4_available(GError **error) {
  const char *err = NULL;
  if (skim_dcw4_net_shared(&err)) { return TRUE; }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err ? err : "the network did not load");
  return FALSE;
}

char *skim_decode_cw_v4_runtime_info(void) {
  const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
  tune_init();
  return g_strdup_printf("%s, %u worker%s%s", net ? skim_dcw4_isa_name(skim_dcw4_net_isa(net)) : "none",
                         skim_decode_cw_v4_workers(), skim_decode_cw_v4_workers() == 1 ? "" : "s",
                         g_tune.sync ? ", inline" : "");
}

/* ---- per channel ---------------------------------------------------------------- */

/* An event for the extractor: one closed word (with its FIX), or the over
 * break. */
typedef struct {
  char  *text;
  double conf;
  double off_hz, snr_db;     /* measured on the window that committed it */
} Unit;

typedef struct {
  gboolean dead;
  double   rate;
  guint    N, hop;
  float   *win, *tw_re, *tw_im, *sring;
  guint    spos, sfill, since;
  float   *ring;
  guint    ring_frames;
  guint64  frames_abs;
  float   *tile;
  double   wsum2;                       /* Σ w² of the DFT window          */
  guint    tick_frames, tick_phase, min_pending_f, hangover_f;
  gboolean was_open;
  guint64  last_open;
  double   lvl_ema, off_ema, env_lo, noise_pw;
  double   off_win;                     /* the last keyed window's offset  */
  double   snr_db, wpm, ratio_db, duty;
  gboolean gate;
  int      line_bin;
  double   freq_hz;
  SkimDcw4Stitch *st;
  double   ofps;                        /* output frames a second          */
  gint64   wb_frames, break_frames;

  GQueue   units;                       /* Unit *                          */
  GString *aux;                         /* pane-only words                 */
  /* the word being read */
  GString *word;
  gint64   word_first, word_last;
  double   word_psum;
  float    word_pmin;
  /* the word before it, for a torn call */
  char     prev[16];
  gint64   prev_first, prev_last;
  double   last_conf;
  gboolean text_since_break;

  GString *draft, *pane_last;
  gsize    pane_last_fin;
  gboolean pane_open, pane_dirty;
  GQueue   ops;                         /* SkimPaneOp *                    */

  gboolean inflight;
  Box     *box;
  gboolean flush_want, flush_inflight, resync_due;
} V4;

static guint g_reg;

static float *ring_row(V4 *c, guint64 f) { return c->ring + (gsize)(f % c->ring_frames) * KEEP; }

static gpointer v4_channel_new(double rate) {
  tune_init();
  V4 *c = g_new0(V4, 1);
  g_queue_init(&c->units);
  g_queue_init(&c->ops);
  c->aux = g_string_new(NULL);
  c->word = g_string_new(NULL);
  c->draft = g_string_new(NULL);
  c->pane_last = g_string_new(NULL);
  c->box = box_new();
  c->noise_pw = NAN;
  c->rate = rate;
  const double nf = rate / BIN_HZ;
  const guint N = (guint)llround(nf);
  if (fabs(nf - N) > 1e-6 || N < 8 || !skim_dcw4_net_shared(NULL)) {
    static gboolean warned;
    if (!warned) {
      warned = TRUE;
      g_warning("cw-v4: %s — channel decodes nothing",
                N < 8 || fabs(nf - N) > 1e-6 ? "channel rate is no multiple of 12.5 Hz"
                                             : "the network did not load");
    }
    c->dead = TRUE;
    return c;
  }
  c->N = N;
  c->hop = MAX((guint)llround(rate * 0.016), 1u);           /* 16 ms: 4 at 250 Hz */
  c->win = g_new(float, N);
  c->tw_re = g_new(float, (gsize)KEEP * N);
  c->tw_im = g_new(float, (gsize)KEEP * N);
  for (guint n = 0; n < N; n++) {
    c->win[n] = 0.5f - 0.5f * cosf(2.0f * (float)G_PI * (float)n / (float)N);
    c->wsum2 += (double)c->win[n] * c->win[n];
    for (int k = -HALF; k <= HALF; k++) {
      const double a = 2.0 * G_PI * (double)k * (double)n / (double)N;
      c->tw_re[(k + HALF) * N + n] = (float)cos(a);
      c->tw_im[(k + HALF) * N + n] = (float)-sin(a);
    }
  }
  c->sring = g_new0(float, 2 * N);
  const double fps = rate / c->hop;                         /* input frames/s */
  const SkimDcw4StitchConfig *sc = &g_tune.stitch;
  c->ring_frames = MAX((guint)llround((sc->max_segment_s + 2.0) * fps), 64u);
  c->ring = g_new0(float, (gsize)c->ring_frames * KEEP);
  c->tile = g_new0(float, (gsize)c->ring_frames * SKIM_DCW4_BINS);
  c->tick_frames = MAX((guint)llround(sc->step_s * fps), 1u);
  c->tick_phase = (g_atomic_int_add(&g_reg, 1) * 7u) % c->tick_frames;
  c->min_pending_f = (guint)llround(sc->min_pending_s * fps);
  /* after the gate closes the windows keep coming until the freeze guard
   * has passed the over's end, so its tail commits */
  c->hangover_f = (guint)llround((sc->freeze_guard_s + sc->right_ramp_s + 2.0 * sc->step_s) * fps);
  c->ofps = fps / STRIDE;
  c->st = skim_dcw4_stitch_new(sc, c->ofps);
  c->wb_frames = (gint64)ceil(sc->word_boundary_s * c->ofps);
  c->break_frames = (gint64)ceil(g_tune.break_s * c->ofps);
  if (!g_tune.sync) { pool_ensure(); }
  return c;
}

static void unit_free(gpointer p) {
  Unit *u = p;
  g_free(u->text);
  g_free(u);
}

static void op_free(gpointer p) {
  SkimPaneOp *op = p;
  g_free(op->text);
  g_free(op->fresh);
  g_free(op);
}

static void v4_channel_free(gpointer state) {
  V4 *c = state;
  if (!c) { return; }
  g_atomic_int_set(&c->box->dying, 1);
  box_unref(c->box);
  g_queue_clear_full(&c->units, unit_free);
  g_queue_clear_full(&c->ops, op_free);
  g_string_free(c->aux, TRUE);
  g_string_free(c->word, TRUE);
  g_string_free(c->draft, TRUE);
  g_string_free(c->pane_last, TRUE);
  skim_dcw4_stitch_free(c->st);
  g_free(c->win); g_free(c->tw_re); g_free(c->tw_im); g_free(c->sring);
  g_free(c->ring); g_free(c->tile);
  g_free(c);
}

/* ---- the front end (decode_deepcw.c, OK1BR; SDRIAK's v2 channel) ------------- */

static void tick(V4 *c);

static void frame(V4 *c) {
  float *row = ring_row(c, c->frames_abs);
  float mags[KEEP];
  for (int b = 0; b < KEEP; b++) {
    const float *tr = c->tw_re + (gsize)b * c->N;
    const float *ti = c->tw_im + (gsize)b * c->N;
    float re = 0, im = 0;
    guint idx = c->spos;
    for (guint n = 0; n < c->N; n++) {
      const float w = c->win[n];
      const float x = c->sring[2 * idx] * w, y = c->sring[2 * idx + 1] * w;
      re += x * tr[n] - y * ti[n];
      im += x * ti[n] + y * tr[n];
      idx = (idx + 1 == c->N) ? 0 : idx + 1;
    }
    mags[b] = sqrtf(re * re + im * im);
    row[b] = mags[b];
  }
  int p = 0;
  for (int b = 1; b < KEEP; b++) { if (mags[b] > mags[p]) { p = b; } }
  float sum = 0;
  guint nfl = 0;
  for (int b = HALF - 3; b <= HALF + 3; b++) {
    if (b - p >= 2 || p - b >= 2) {
      sum += mags[b];
      nfl++;
    }
  }
  const float fl = nfl ? sum / nfl : mags[p];
  const double decay = exp(-(double)c->hop / (2.0 * c->rate));
  c->lvl_ema = MAX((double)mags[p], c->lvl_ema * decay);
  if (c->env_lo <= 0.0) { c->env_lo = fl; } else { c->env_lo += 0.01 * (fl - c->env_lo); }
  if (mags[p] > 4.0f * fl && mags[p] > 0.5 * c->lvl_ema && p > 0 && p < KEEP - 1) {
    const double l = log((double)MAX(mags[p - 1], 1e-9f));
    const double m = log((double)MAX(mags[p], 1e-9f));
    const double r = log((double)MAX(mags[p + 1], 1e-9f));
    const double den = l - 2.0 * m + r;
    const double delta = den != 0.0 ? CLAMP(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    const double off = ((double)p - HALF + delta) * BIN_HZ;
    c->off_ema += 0.05 * (off - c->off_ema);
  }
  c->frames_abs++;
  if ((c->frames_abs + c->tick_phase) % c->tick_frames == 0) { tick(c); }
}

/* The gate (line over the floor, keyed duty) and SNR in 500 Hz over input
 * frames [w0, w1); the line bin for the tile. */
static void window_stats(V4 *c, guint64 w0, guint64 w1) {
  const guint T = (guint)(w1 - w0);
  double prof[KEEP] = { 0 };
  for (guint64 f = w0; f < w1; f++) {
    const float *r = ring_row(c, f);
    for (int b = 0; b < KEEP; b++) { prof[b] += r[b]; }
  }
  int p = 0;
  for (int b = 1; b < KEEP; b++) { if (prof[b] > prof[p]) { p = b; } }
  double floor_sum = 0;
  guint floor_n = 0;
  for (int b = 0; b < KEEP; b++) {
    if (b - p >= 3 || p - b >= 3) {
      floor_sum += prof[b];
      floor_n++;
    }
  }
  const double line = prof[p] / T;
  const double floorv = floor_n ? floor_sum / floor_n / T : 1e-12;
  c->ratio_db = 20.0 * log10(MAX(line, 1e-12) / MAX(floorv, 1e-12));
  double mx = 0;
  for (guint64 f = w0; f < w1; f++) { mx = MAX(mx, (double)ring_row(c, f)[p]); }
  const double thr = 0.5 * (mx + floorv);
  guint on = 0;
  for (guint64 f = w0; f < w1; f++) { on += ring_row(c, f)[p] > thr; }
  c->duty = (double)on / T;
  /* the speed from the keying (decode_deepcw.c, OK1BR): the dit is the
   * first well-populated length of the line's on-runs, 1-frame glitches
   * out, refined over its ±1 neighbours — a short over (CQ and a call)
   * never commits the 6 characters over 2 s the PARIS count needs */
  if (c->ratio_db >= g_tune.gate_db && on >= 6) {
    guint hist[16] = { 0 };
    guint run = 0;
    for (guint64 f = w0; f <= w1; f++) {
      const gboolean k = f < w1 && ring_row(c, f)[p] > thr;
      if (k) { run++; continue; }
      if (run >= 2 && run < 16) { hist[run]++; }
      run = 0;
    }
    guint mode = 0;
    for (guint L = 2; L < 9; L++) { if (hist[L] > hist[mode]) { mode = L; } }
    guint dit = 0;
    for (guint L = 2; L < 9; L++) {
      if (hist[L] >= 3 && hist[L] * 5 >= hist[mode] * 2) { dit = L; break; }
    }
    if (dit) {
      double s = 0;
      guint n = 0;
      for (guint L = dit - 1; L <= dit + 1 && L < 16; L++) { s += (double)L * hist[L]; n += hist[L]; }
      const double wpm = 1.2 / ((s / n) * c->hop / c->rate);
      if (wpm >= 5.0 && wpm <= 60.0) { c->wpm = wpm; }
    }
  }
  /* the tone's offset over this window's keyed frames: the line bin and
   * its neighbours, parabolic on log magnitudes (the frame EMA follows
   * the noise within a second of the over's end; the words go out 2–5 s
   * after it, and their frequency is this) */
  if (p > 0 && p < KEEP - 1 && on >= 3) {
    double a[3] = { 0, 0, 0 };
    for (guint64 f = w0; f < w1; f++) {
      const float *r = ring_row(c, f);
      if (r[p] <= thr) { continue; }
      for (int k = 0; k < 3; k++) { a[k] += r[p - 1 + k]; }
    }
    const double l = log(MAX(a[0], 1e-12)), m = log(MAX(a[1], 1e-12)), r = log(MAX(a[2], 1e-12));
    const double den = l - 2.0 * m + r;
    const double delta = den != 0.0 ? CLAMP(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
    if (c->ratio_db >= g_tune.gate_db) { c->off_win = ((double)p - HALF + delta) * BIN_HZ; }
  }
  c->gate = c->ratio_db >= g_tune.gate_db && c->duty >= 0.03 && c->duty <= 0.97;
  c->line_bin = CLAMP(p, 1, KEEP - 2);
  /* OK1BR 4db0483 (gh#27): SNR in 500 Hz. The noise a bin holds: the band
   * floor's density (set_floor: a channel sample's noise power over the
   * channel's ENBW, snr_meter.h) through the DFT window (Σ w² · rate per
   * unit density); else the channel's own quiet bins (Rayleigh: E|X|² =
   * 4/π (E|X|)²). */
  double nb;
  if (isfinite(c->noise_pw) && c->noise_pw > 0) {
    const double enbw = SKIM_SNR_CW_ENBW * c->rate / 2.0;
    nb = c->noise_pw / enbw * c->rate * c->wsum2;
  } else {
    const double flm = MAX(c->env_lo, 1e-12);
    nb = (4.0 / G_PI) * flm * flm;
  }
  const int b0 = p > 0 ? p - 1 : p;
  const int b1 = p + 1 < KEEP ? p + 1 : p;
  double p3 = 0.0;
  guint non = 0;
  for (guint64 f = w0; f < w1; f++) {
    const float *r = ring_row(c, f);
    if (r[p] <= 0.7 * mx) { continue; }
    for (int b = b0; b <= b1; b++) { p3 += (double)r[b] * r[b]; }
    non++;
  }
  if (non && c->gate) {                 /* a hangover window holds no line */
    const guint nbins = (guint)(b1 - b0 + 1);
    const double sig = (p3 / non - nbins * nb) / (nbins == 3 ? 1.5 : 1.25);
    c->snr_db = 10.0 * log10(MAX(sig, 1e-3 * nb) / nb) + 10.0 * log10(1.5 * BIN_HZ / 500.0);
  }
}

/* ---- the pane draft ------------------------------------------------------------- */

static void queue_op(V4 *c, SkimPaneOpKind kind, const char *text, guint final_len) {
  SkimPaneOp *op = g_new0(SkimPaneOp, 1);
  op->kind = kind;
  op->final_len = final_len;
  op->text = g_strdup(text);
  g_queue_push_tail(&c->ops, op);
}

/* The pane shows the events not taken yet (FIX stripped) as final, then
 * the word being read and the stitcher's pending reading in gray. */
static void pane_compose(V4 *c) {
  if (!g_tune.draft || !c->pane_dirty) { return; }
  c->pane_dirty = FALSE;
  GString *reg = g_string_new(NULL);
  for (GList *l = c->units.head; l; l = l->next) {
    char *t = g_strdup(((Unit *)l->data)->text);
    skim_callsign_strip_fix(t);
    g_string_append(reg, t);
    g_free(t);
  }
  const gsize fin = reg->len;
  g_string_append(reg, c->word->str);
  g_string_append(reg, c->draft->str);
  if (reg->len == 0) {
    if (c->pane_open) {
      queue_op(c, SKIM_PANE_OP_CLOSE, "", 0);
      c->pane_open = FALSE;
      g_string_truncate(c->pane_last, 0);
      c->pane_last_fin = 0;
    }
    g_string_free(reg, TRUE);
    return;
  }
  if (c->pane_open && fin == c->pane_last_fin && strcmp(reg->str, c->pane_last->str) == 0) {
    g_string_free(reg, TRUE);
    return;
  }
  queue_op(c, c->pane_open ? SKIM_PANE_OP_SET : SKIM_PANE_OP_OPEN, reg->str, (guint)fin);
  c->pane_open = TRUE;
  g_string_assign(c->pane_last, reg->str);
  c->pane_last_fin = fin;
  g_string_free(reg, TRUE);
}

static gboolean v4_take_pane_op(gpointer state, SkimPaneOp *op) {
  V4 *c = state;
  SkimPaneOp *o = g_queue_pop_head(&c->ops);
  if (!o) { return FALSE; }
  *op = *o;
  g_free(o);
  return TRUE;
}

static char *v4_take_aux_text(gpointer state) {
  V4 *c = state;
  if (c->aux->len == 0) { return NULL; }
  char *t = g_strdup(c->aux->str);
  g_string_truncate(c->aux, 0);
  return t;
}

/* ---- words, calls, overs -------------------------------------------------------- */

static void push_unit(V4 *c, char *text, double conf) {
  Unit *u = g_new(Unit, 1);
  u->text = text;
  u->conf = conf;
  u->off_hz = c->off_win;
  u->snr_db = c->snr_db;
  g_queue_push_tail(&c->units, u);
  c->pane_dirty = TRUE;
}

static gboolean has_digit_letter(const char *w) {
  gboolean d = FALSE, a = FALSE;
  for (; *w; w++) {
    d = d || g_ascii_isdigit(*w);
    a = a || g_ascii_isalpha(*w);
  }
  return d && a;
}

/* The call reader on [first, last] ± a margin of committed rows. */
static gboolean read_call(V4 *c, const char *raw, gint64 first, gint64 last,
                          SkimDcw4CallreadResult *res) {
  const gint64 f0 = first - CALL_MARGIN_F;
  const gint64 f1 = MIN(last + CALL_MARGIN_F, skim_dcw4_stitch_committed_end(c->st) - 1);
  if (f1 < f0 || f1 - f0 + 1 > 4096) { return FALSE; }
  const guint n = (guint)(f1 - f0 + 1);
  const float **rows = g_new(const float *, n);
  for (guint i = 0; i < n; i++) {
    rows[i] = skim_dcw4_stitch_row(c->st, f0 + i);
    /* a frame of the word itself gone: no reading (a missing frame would
     * read as a certain blank, and a shorter call would win) */
    if (!rows[i] && f0 + (gint64)i >= first && f0 + (gint64)i <= last) {
      g_free(rows);
      return FALSE;
    }
  }
  SkimDcw4CallreadConfig cfg;
  skim_dcw4_callread_config_default(&cfg);
  cfg.sep = g_tune.call_sep;
  const gboolean won = skim_dcw4_callread(&cfg, rows, n, raw, res);
  if (g_tune.debug > 2 && won) {
    guint nul = 0;
    GString *am = g_string_new(NULL);
    for (guint i = 0; i < n; i++) {
      if (!rows[i]) { nul++; g_string_append_c(am, '#'); continue; }
      int b = 0;
      for (int k = 1; k < NC; k++) { if (rows[i][k] > rows[i][b]) { b = k; } }
      g_string_append_c(am, b == SKIM_DCW4_BLANK ? '-' : b == SKIM_DCW4_SPACE ? '_' : SKIM_DCW4_ALPHABET[b]);
    }
    fprintf(stderr, "cw-v4: read %s frames %" G_GINT64_FORMAT "..%" G_GINT64_FORMAT
            " committed_end %" G_GINT64_FORMAT " null %u raw %.1f call %s %.1f |%s|\n", raw, f0, f1,
            skim_dcw4_stitch_committed_end(c->st), nul, res->raw_score, res->call, res->call_score, am->str);
    g_string_free(am, TRUE);
  }
  g_free(rows);
  return won;
}

/* The word is complete: read it for a call, then out — whole, with its
 * FIX, or to the pane only when its weakest character is too weak. */
static void close_word(V4 *c) {
  if (c->word->len == 0) { return; }
  const guint n = (guint)c->word->len;
  const double conf = c->word_psum / n;
  char fix[16] = "";
  SkimDcw4CallreadResult res;
  if (g_tune.callread && n < 16) {
    if (has_digit_letter(c->word->str) &&
        read_call(c, c->word->str, c->word_first, c->word_last, &res)) {
      g_strlcpy(fix, res.call, sizeof fix);
    } else if (c->prev[0] && c->word_first - c->prev_last <= (gint64)(JOIN_GAP_S * c->ofps) &&
               strlen(c->prev) + n < 16 && !skim_callsign_is_valid(c->prev) &&
               !skim_callsign_is_valid(c->word->str)) {
      /* a call torn in two (the narrow model's spurious gap before a digit
       * at ≤ 12 WPM): neither piece a call, close together. The gap is the
       * only difference between the torn and the joined reading, and the
       * model may not even hold it (the stitcher's 2.01 s blank rule
       * makes gaps too) — so the joined word is read without it: in
       * MASTER.SCP it stands, else a near call may win as for one word. */
      char joined[32];
      g_snprintf(joined, sizeof joined, "%s%s", c->prev, c->word->str);
      if (has_digit_letter(joined)) {
        if (skim_callsign_dict_has(joined)) {
          g_strlcpy(fix, joined, sizeof fix);
          res.margin = 0;
          res.candidates = 1;
        } else if (read_call(c, joined, c->prev_first, c->word_last, &res)) {
          g_strlcpy(fix, res.call, sizeof fix);
        }
      }
    }
  }
  if (g_tune.debug && fix[0]) {
    fprintf(stderr, "cw-v4: %.2f kHz %s → %s (margin %.1f, %u candidates)\n", c->freq_hz / 1e3,
            c->word->str, fix, res.margin, res.candidates);
  }
  if (!fix[0] && g_tune.drop_p > 0 && c->word_pmin < g_tune.drop_p) {
    g_string_append(c->aux, c->word->str);
    g_string_append_c(c->aux, ' ');
  } else {
    push_unit(c, fix[0] ? g_strdup_printf("%s\x01%s\x02 ", c->word->str, fix)
                        : g_strdup_printf("%s ", c->word->str), conf);
    c->last_conf = conf;
    c->text_since_break = TRUE;
  }
  g_strlcpy(c->prev, n < sizeof c->prev ? c->word->str : "", sizeof c->prev);
  c->prev_first = c->word_first;
  c->prev_last = c->word_last;
  g_string_truncate(c->word, 0);
  c->pane_dirty = TRUE;
}

static void over_break(V4 *c) {
  skim_dcw4_stitch_forget(c->st);      /* the over is done: its rows too */
  if (!c->text_since_break) { return; }
  push_unit(c, g_strdup(BREAK_MARK), c->last_conf);
  c->text_since_break = FALSE;
  c->prev[0] = '\0';
}

/* ---- the stitcher glue (SDRIAK) ------------------------------------------------- */

static void flush_done(V4 *c);
static void submit(V4 *c, guint T, gint64 s, gboolean final);

static void apply(V4 *c, const float *logp, guint To, gint64 s, gboolean final) {
  if (To) {
    skim_dcw4_stitch_add_window(c->st, s, logp, To, final);
  } else if (final) {
    skim_dcw4_stitch_add_window(c->st, s, NULL, 0, TRUE);
  }
  SkimDcw4Take t;
  skim_dcw4_stitch_take(c->st, final, &t);
  if (t.n_spans) {
    if (t.starts_word) { close_word(c); }
    gint64 first = -1, last = -1;
    guint letters = 0;
    for (guint i = 0; i < t.n_spans; i++) {
      const SkimDcw4Span *sp = &t.spans[i];
      if (sp->cls == SKIM_DCW4_SPACE) {
        close_word(c);
        continue;
      }
      if (c->word->len == 0) {
        c->word_first = sp->start;
        c->word_psum = 0;
        c->word_pmin = 1.0f;
      }
      g_string_append_c(c->word, SKIM_DCW4_ALPHABET[sp->cls]);
      c->word_last = sp->end;
      c->word_psum += sp->p;
      c->word_pmin = MIN(c->word_pmin, sp->p);
      if (first < 0) { first = sp->start; }
      last = sp->end;
      letters++;
    }
    /* PARIS: 12 × characters / s over the committed stretch */
    const double secs = (double)(last - first + 1) / c->ofps;
    if (letters >= 6 && secs >= 2.0) {
      const double wpm = 12.0 * letters / secs;
      if (wpm >= 5.0 && wpm <= 60.0) { c->wpm = wpm; }
    }
    c->pane_dirty = TRUE;
  }
  const gint64 blank = skim_dcw4_stitch_blank_frames(c->st);
  if (t.ends_word || final || blank >= c->wb_frames) { close_word(c); }
  if (c->word->len == 0 && (blank >= c->break_frames || final)) { over_break(c); }
  g_string_truncate(c->draft, 0);
  if (g_tune.draft && !final && t.pending[0]) {
    if (t.pending_starts_word && c->word->len) { g_string_append_c(c->draft, ' '); }
    g_string_append(c->draft, t.pending);
  }
  c->pane_dirty = TRUE;
  if (g_tune.debug > 1) {
    fprintf(stderr, "cw-v4: %.2f kHz s=%" G_GINT64_FORMAT " To=%u%s ratio %.1f dB word |%s| "
            "pending |%s| committed %" G_GINT64_FORMAT "\n", c->freq_hz / 1e3, s, To,
            final ? " FINAL" : "", c->ratio_db, c->word->str, t.pending,
            skim_dcw4_stitch_committed_end(c->st));
  }
  if (final) { flush_done(c); }
}

static void window(V4 *c, gboolean final) {
  const guint64 E = c->frames_abs - c->frames_abs % STRIDE;
  const gint64 end_out = (gint64)(E / STRIDE);
  gint64 s = skim_dcw4_stitch_window_start(c->st, end_out);
  const gint64 ring_lo = E > c->ring_frames ? (gint64)((E - c->ring_frames + 1) / STRIDE) : 0;
  s = MAX(s, ring_lo);
  if (s >= end_out) {
    if (final) { flush_done(c); }
    return;
  }
  const guint64 w0 = (guint64)s * STRIDE;
  window_stats(c, w0, E);
  if (!final) {
    if (c->gate) {
      c->was_open = TRUE;
      c->last_open = E;
    }
    const gboolean hang = c->was_open && E - c->last_open < c->hangover_f;
    if (!c->gate && !hang) {
      /* the over ended: its last word goes now (a reading left pending
       * past the frozen point would hold the blank count — and the word —
       * until the next over), the break after it, no draft lingers */
      close_word(c);
      over_break(c);
      if (c->draft->len) {
        g_string_truncate(c->draft, 0);
        c->pane_dirty = TRUE;
      }
      return;
    }
  }
  const guint T = (guint)(E - w0);
  for (guint t = 0; t < T; t++) {
    const float *r = ring_row(c, w0 + t);
    for (int k = 0; k < 3; k++) { c->tile[(gsize)t * 3 + k] = r[c->line_bin - 1 + k]; }
  }
  skim_dcw4_normalize(c->tile, (gsize)T * 3);
  submit(c, T, s, final);
}

static void tick(V4 *c) {
  if (c->frames_abs < c->min_pending_f) { return; }
  if (c->inflight) {
    /* the last window is still queued or running: a skipped tick is the
     * load signal (only for a line that is keyed or in its hangover) */
    if (c->gate || (c->was_open && c->frames_abs - c->last_open < c->hangover_f)) {
      g_mutex_lock(&g_stats_lock);
      g_stats.ticks++;
      g_stats.skipped++;
      g_mutex_unlock(&g_stats_lock);
    }
    return;
  }
  window(c, FALSE);
}

static void submit(V4 *c, guint T, gint64 s, gboolean final) {
  g_mutex_lock(&g_stats_lock);
  if (!final) { g_stats.ticks++; }
  g_mutex_unlock(&g_stats_lock);
  if (g_tune.sync) {
    const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
    const guint To = skim_dcw4_output_frames(T);
    float *logp = g_new(float, (gsize)To * NC);
    const gint64 c0 = thread_cpu_ns();
    skim_dcw4_net_run(net, c->tile, T, logp, SKIM_DCW4_ISA_AUTO);
    g_mutex_lock(&g_stats_lock);
    g_stats.windows++;
    g_stats.cpu_ns += (guint64)(thread_cpu_ns() - c0);
    g_mutex_unlock(&g_stats_lock);
    apply(c, logp, To, s, final);
    g_free(logp);
    return;
  }
  Job *j = g_new(Job, 1);
  g_atomic_int_inc(&c->box->ref);
  j->box = c->box;
  j->tile = g_memdup2(c->tile, (gsize)T * SKIM_DCW4_BINS * sizeof(float));
  j->T = T;
  j->s = s;
  j->queued_us = g_get_monotonic_time();
  c->flush_inflight = final;
  c->inflight = TRUE;
  g_thread_pool_push(g_pool, j, NULL);
}

/* ---- TX hold ---------------------------------------------------------------------- */

static void restart(V4 *c) {
  skim_dcw4_stitch_restart(c->st, (gint64)((c->frames_abs + STRIDE - 1) / STRIDE));
  c->prev[0] = '\0';
  if (c->draft->len) {
    g_string_truncate(c->draft, 0);
    c->pane_dirty = TRUE;
  }
}

static void flush_done(V4 *c) {
  c->flush_inflight = FALSE;
  close_word(c);
  if (c->draft->len) {
    g_string_truncate(c->draft, 0);
    c->pane_dirty = TRUE;
  }
  if (c->resync_due) {
    c->resync_due = FALSE;
    restart(c);
  }
}

static void flush_try(V4 *c) {
  if (!c->flush_want || c->inflight) { return; }
  c->flush_want = FALSE;
  if (skim_dcw4_stitch_covered_end(c->st) <= skim_dcw4_stitch_committed_end(c->st)) {
    flush_done(c);
    return;
  }
  window(c, TRUE);
}

/* TX hold: the stitcher's final window (decode_morse.py Decoder.finalize:
 * no right ramp, everything committed), the last word closed. */
static void v4_hold_begin(gpointer state) {
  V4 *c = state;
  if (c->dead) { return; }
  c->flush_want = TRUE;
  flush_try(c);
}

static void v4_resync(gpointer state) {
  V4 *c = state;
  if (c->dead) { return; }
  c->sfill = c->spos = c->since = 0;
  if (c->flush_want) {
    flush_try(c);
    c->flush_want = FALSE;
  }
  if (c->inflight && c->flush_inflight) {
    c->resync_due = TRUE;
  } else {
    restart(c);
  }
  pane_compose(c);
}

/* ---- process ---------------------------------------------------------------------- */

static gboolean v4_process(gpointer state, const float *iq, guint nframes, SkimDecode *out) {
  V4 *c = state;
  memset(out, 0, sizeof *out);
  if (c->dead) { return FALSE; }
  if (c->inflight) {
    float *logp = NULL;
    guint To = 0;
    gint64 s = 0;
    g_mutex_lock(&c->box->lock);
    const gboolean got = c->box->ready;
    if (got) {
      logp = c->box->logp;
      c->box->logp = NULL;
      To = c->box->To;
      s = c->box->s;
      c->box->ready = FALSE;
    }
    g_mutex_unlock(&c->box->lock);
    if (got) {
      c->inflight = FALSE;
      apply(c, logp, To, s, c->flush_inflight);
      g_free(logp);
    }
  }
  flush_try(c);
  for (guint i = 0; iq && i < nframes; i++) {
    const guint w = (c->spos + c->sfill) % c->N;
    c->sring[2 * w] = iq[2 * i];
    c->sring[2 * w + 1] = iq[2 * i + 1];
    if (c->sfill < c->N) {
      c->sfill++;
    } else {
      c->spos = (c->spos + 1) % c->N;
    }
    if (c->sfill == c->N && ++c->since >= c->hop) {
      c->since = 0;
      frame(c);
    }
  }
  Unit *u = g_queue_pop_head(&c->units);
  if (!u) {
    pane_compose(c);
    return FALSE;
  }
  g_strlcpy(out->text, u->text, SKIM_DECODE_TEXT_MAX);
  out->confidence = CLAMP(u->conf, 0.0, 1.0);
  out->freq_offset_hz = u->off_hz;
  out->speed = c->wpm;
  out->snr_db = u->snr_db;
  out->pane_own = FALSE;
  unit_free(u);
  c->pane_dirty = TRUE;
  pane_compose(c);
  return TRUE;
}

static double v4_level(gpointer state) { return ((V4 *)state)->lvl_ema; }
/* the keyed windows' offset once there is one (the frame EMA before) */
static double v4_tone_offset_hz(gpointer state) {
  const V4 *c = state;
  return c->off_win != 0.0 ? c->off_win : c->off_ema;
}
static void v4_set_freq(gpointer state, double freq_hz) { ((V4 *)state)->freq_hz = freq_hz; }
static void v4_set_floor(gpointer state, double noise_pw) { ((V4 *)state)->noise_pw = noise_pw; }

const SkimDecodeBackend *skim_decode_cw_v4(void) {
  static const SkimDecodeBackend backend = {
    .name           = "cw-v4",
    .channel_new    = v4_channel_new,
    .channel_free   = v4_channel_free,
    .process        = v4_process,
    .level          = v4_level,
    .tone_offset_hz = v4_tone_offset_hz,
    .set_freq       = v4_set_freq,
    .set_floor      = v4_set_floor,
    .take_aux_text  = v4_take_aux_text,
    .take_pane_op   = v4_take_pane_op,
    .resync         = v4_resync,
    .hold_begin     = v4_hold_begin,
  };
  return &backend;
}
