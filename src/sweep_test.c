/*
 * skimmer-sweep — the weak-signal bench: how often a CQing station's call
 * reaches the RBN feed, as a function of its SNR, through the WHOLE offline
 * pipeline (channelizer → decoder → extractor → station table → feed
 * policy). Built because the live comparison against CW Skimmer gives a few
 * dozen stations below 10 dB a night (skimmer-compare, 2026-10-02: of R's
 * catches L had 20 / 35 / 54 % at < 5 / 5–10 / 10–15 dB).
 *
 * SNR is SNR500: key-down carrier power over the noise in 500 Hz (complex
 * AWGN, flat over the whole input). A fading station's SNR is its mean.
 *
 * Every run puts 12 stations into one 48 kHz band, 3.5 kHz apart, each
 * CQing for 60 s ("CQ CQ DE X X K" or "TEST X", half of them in MASTER.SCP),
 * then 12 s of noise for the settle hold. Three tiers per station:
 *   text   the call appears in the decoded text near its frequency
 *   table  the station table reports it within 100 Hz
 *   feed   the RBN policy sends it within 150 Hz — the number that counts
 * and the feed's wrong lines ("bad": a call no station sent, or far from it).
 *
 * Scenarios
 *   centre  tone within ±15 Hz of a channel centre, steady keyer
 *   edge    tone 50–62.5 Hz off a channel centre (between two channels)
 *   any     tone anywhere in the channel
 *   qsb     Rayleigh fading 0.1–1 Hz
 *   hand    fist jitter 5–15 %
 *   drift   ±1 Hz/s
 *   chirp   +20…40 Hz at every key-down, settling in ~15 ms
 *   nb10    a +10 dB neighbour 50 / 100 / 200 Hz away (one third each)
 *   nb20    the same at +20 dB
 *   noise   no station at all: bad lines per hour of pure noise
 *
 *   skimmer-sweep [--gate] [--scenarios a,b,..] [--snr lo:hi:step] [--reps N]
 *                 [--threads N] [--scp PATH] [--csv PATH] [--noise-s S]
 *
 * --gate runs a quick subset against a fixed SCP and checks the regression
 * floor (meson test 'weak-sweep'). Without --scp the full sweep loads
 * ~/.config/skimmer-for-linux/master.scp when it exists.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine/callsign.h"
#include "engine/pipeline.h"
#include "synth.h"

#define RATE      48000.0
#define CENTER    7030000.0
#define CHAN_HZ   125.0
#define ENV_RATE  1000.0                  /* keyed envelopes                 */
#define FADE_RATE 100.0                   /* fading gains                    */
#define NSTA      12
#define CQ_S      60.0
#define TAIL_S    12.0
#define BLK       4800
#define SIGMA     0.01                    /* noise sd per I/Q component      */

typedef enum {
  SC_CENTRE, SC_EDGE, SC_ANY, SC_QSB, SC_HAND, SC_DRIFT, SC_CHIRP, SC_NB10,
  SC_NB20, SC_NOISE, SC_COUNT
} Scenario;

static const char *SC_NAME[SC_COUNT] = {
  "centre", "edge", "any", "qsb", "hand", "drift", "chirp", "nb10", "nb20",
  "noise",
};

/* Calls the fixed gate SCP holds (real-looking, any allocation). */
static const char *SCP_POOL[] = {
  "OK1BR", "OK2LA", "DL1AAA", "G4YWL", "F5MNO", "EA5AHN", "SP5DDD",
  "HA8FK", "OE5POP", "ON4ANE", "IU8QTM", "SM5EIE", "YL3AJT", "UA3GDU",
  "S51ZZ", "9A5K", "LZ1HW", "YU1RA", "OM3PC", "HB9FFF", "K3ZM", "W2SO",
  "N4IJ", "KB6NU", "VE3NFN", "JA1XYZ", "VK2GR", "4X1MK", "CT1BMW", "GM4HBG",
  "R4FL", "UK8OC", "SA6AUT", "KD1MD", "NA2DX", "F4LTU", "AK4JK", "WW4MW",
  "HK3ZZ", "2E0NJK",
};

static const char *PREFIXES[] = {
  "OK", "OL", "DL", "DK", "G", "M", "F", "I", "IZ", "EA", "SP", "SQ", "HA",
  "OM", "S5", "YU", "LZ", "UR", "UT", "W", "K", "N", "VE", "JA", "PA", "ON",
};

typedef struct {
  char     call[16];
  gboolean in_scp, neighbour;
  int      sub;                           /* nb: Δf in Hz, else 0            */
  double   f;                             /* offset from CENTER, Hz          */
  double   amp, drift, chirp;
  GArray  *env;                           /* at ENV_RATE                     */
  float   *fre, *fim;                     /* at FADE_RATE, or NULL           */
  guint    nfade;
  GString *text;
  gboolean got_text, got_table, got_feed;
  guint    bad;                           /* wrong feed lines nearest to it  */
} Station;

typedef struct {
  Scenario sc;
  double   snr;
  guint    rep;
  guint32  seed;
  double   secs;
  Station  st[2 * NSTA];
  guint    nst;
  guint    bad_feed, bad_table, feed_lines;
} Run;

/* --- the call pools ------------------------------------------------------------- */

static GPtrArray *g_scp_calls;           /* calls the loaded SCP has         */

static void load_scp_list(const char *path) {
  g_scp_calls = g_ptr_array_new_with_free_func(g_free);
  char *data = NULL;
  if (!g_file_get_contents(path, &data, NULL, NULL)) { return; }
  char **lines = g_strsplit(data, "\n", -1);
  for (char **l = lines; *l; l++) {
    g_strstrip(*l);
    if (**l && **l != '#' && strlen(*l) >= 3 && strlen(*l) <= 10 &&
        skim_callsign_is_valid(*l)) {
      g_ptr_array_add(g_scp_calls, g_ascii_strup(*l, -1));
    }
  }
  g_strfreev(lines);
  g_free(data);
}

static void pick_call(char *out, gboolean in_scp, GRand *rng) {
  if (in_scp) {
    const char *c = g_ptr_array_index(g_scp_calls,
                                      g_rand_int_range(rng, 0, g_scp_calls->len));
    g_strlcpy(out, c, 16);
    return;
  }
  for (;;) {                              /* a valid call the SCP lacks      */
    GString *s = g_string_new(PREFIXES[g_rand_int_range(rng, 0,
                                                      G_N_ELEMENTS(PREFIXES))]);
    g_string_append_c(s, (char)('0' + g_rand_int_range(rng, 1, 10)));
    const int n = g_rand_int_range(rng, 2, 4);
    for (int i = 0; i < n; i++) {
      g_string_append_c(s, (char)('A' + g_rand_int_range(rng, 0, 26)));
    }
    gboolean ok = skim_callsign_is_valid(s->str) &&
                  !skim_callsign_dict_has(s->str);
    if (ok) { g_strlcpy(out, s->str, 16); }
    g_string_free(s, TRUE);
    if (ok) { return; }
  }
}

/* --- building a run ------------------------------------------------------------- */

static double amp_for(double snr500) {
  /* A² / (2σ² · 500 / RATE) = 10^(snr/10) */
  return sqrt(pow(10.0, snr500 / 10.0) * 2.0 * SIGMA * SIGMA * 500.0 / RATE);
}

/* A CQing station: keys its CQ until CQ_S is over, listening 3–8 s between. */
static void build_keying(Station *s, gboolean contest, const SynthFist *fist,
                         GRand *rng) {
  s->env = g_array_new(FALSE, FALSE, sizeof(float));
  synth_silence(s->env, g_rand_double_range(rng, 0.5, 8.0), ENV_RATE);
  char *over = contest ? g_strdup_printf("TEST %s", s->call)
                       : g_strdup_printf("CQ CQ DE %s %s K", s->call, s->call);
  while (s->env->len < (CQ_S - 4.0) * ENV_RATE) {
    synth_key(s->env, over, fist, ENV_RATE, rng);
    synth_silence(s->env, contest ? g_rand_double_range(rng, 2.0, 4.0)
                                  : g_rand_double_range(rng, 4.0, 8.0), ENV_RATE);
  }
  g_free(over);
  synth_shape(s->env, ENV_RATE, 0.005);
}

static void build_run(Run *r) {
  GRand *rng = g_rand_new_with_seed(r->seed);
  r->secs = (r->sc == SC_NOISE ? r->secs : CQ_S) + TAIL_S;
  if (r->sc == SC_NOISE) { g_rand_free(rng); return; }
  const double a = amp_for(r->snr);
  for (guint i = 0; i < NSTA; i++) {
    Station *s = &r->st[r->nst++];
    s->in_scp = (i & 1) == 0;
    pick_call(s->call, s->in_scp, rng);
    const double ch = (-168.0 + 28.0 * i) * CHAN_HZ;
    double off;
    switch (r->sc) {
    case SC_CENTRE: off = g_rand_double_range(rng, -15.0, 15.0); break;
    case SC_EDGE:   off = g_rand_double_range(rng, 50.0, 62.5) *
                          (g_rand_boolean(rng) ? 1 : -1); break;
    default:        off = g_rand_double_range(rng, -62.5, 62.5); break;
    }
    s->f = ch + off;
    s->amp = a;
    SynthFist fist = { .wpm = (double)g_rand_int_range(rng, 16, 33),
                       .jitter = r->sc == SC_HAND
                                 ? g_rand_double_range(rng, 0.05, 0.15) : 0.0 };
    if (r->sc == SC_DRIFT) { s->drift = g_rand_boolean(rng) ? 1.0 : -1.0; }
    if (r->sc == SC_CHIRP) { s->chirp = g_rand_double_range(rng, 20.0, 40.0); }
    build_keying(s, i % 4 >= 2, &fist, rng);
    if (r->sc == SC_QSB) {
      s->nfade = (guint)(r->secs * FADE_RATE) + 2;
      s->fre = g_new(float, s->nfade);
      s->fim = g_new(float, s->nfade);
      synth_fading(s->fre, s->fim, s->nfade, FADE_RATE,
                   g_rand_double_range(rng, 0.1, 1.0), rng);
    }
    s->text = g_string_new(NULL);
    if (r->sc == SC_NB10 || r->sc == SC_NB20) {
      Station *n = &r->st[r->nst++];
      static const int DF[3] = { 50, 100, 200 };
      s->sub = DF[i % 3];
      n->neighbour = TRUE;
      n->sub = s->sub;
      n->in_scp = TRUE;
      do { pick_call(n->call, TRUE, rng); } while (strcmp(n->call, s->call) == 0);
      n->f = s->f + s->sub * (g_rand_boolean(rng) ? 1 : -1);
      n->amp = a * pow(10.0, (r->sc == SC_NB10 ? 10.0 : 20.0) / 20.0);
      SynthFist nf = { .wpm = 28.0, .jitter = 0.0 };
      build_keying(n, TRUE, &nf, rng);
      n->text = g_string_new(NULL);
    }
  }
  g_rand_free(rng);
}

/* One block of the band: every station's carrier + AWGN. */
typedef struct { double ph, age; } Osc;

static void synth_block(Run *r, Osc *osc, guint64 at, guint n, float *iq,
                        GRand *rng) {
  for (guint k = 0; k < n; k++) {
    iq[2 * k]     = (float)(SIGMA * synth_gauss(rng));
    iq[2 * k + 1] = (float)(SIGMA * synth_gauss(rng));
  }
  for (guint j = 0; j < r->nst; j++) {
    Station *s = &r->st[j];
    Osc *o = &osc[j];
    for (guint k = 0; k < n; k++) {
      const double t = (double)(at + k) / RATE;
      const double x = t * ENV_RATE;
      const guint  xi = (guint)x;
      double e = 0.0;
      if (xi + 1 < s->env->len) {
        const float *ev = (const float *)s->env->data;
        e = ev[xi] + (ev[xi + 1] - ev[xi]) * (x - xi);
      }
      o->age = e > 0.5 ? o->age + 1.0 / RATE : 0.0;
      const double f = s->f + s->drift * t + s->chirp * exp(-o->age / 0.005);
      o->ph += 2.0 * G_PI * f / RATE;
      if (o->ph > G_PI) { o->ph -= 2.0 * G_PI; }
      if (e <= 0.0) { continue; }
      double gr = 1.0, gi = 0.0;
      if (s->fre) {
        const double y = t * FADE_RATE;
        const guint yi = MIN((guint)y, s->nfade - 2);
        gr = s->fre[yi] + (s->fre[yi + 1] - s->fre[yi]) * (y - yi);
        gi = s->fim[yi] + (s->fim[yi + 1] - s->fim[yi]) * (y - yi);
      }
      const double c = cos(o->ph), sn = sin(o->ph), v = s->amp * e;
      iq[2 * k]     += (float)(v * (c * gr - sn * gi));
      iq[2 * k + 1] += (float)(v * (sn * gr + c * gi));
    }
  }
}

/* --- callbacks ------------------------------------------------------------------ */

/* Where a station is at the end (drift moves it). */
static double sta_hz(const Station *s, double t) {
  return CENTER + s->f + s->drift * t;
}

static Station *near_call(Run *r, const char *call, double hz, double tol) {
  for (guint j = 0; j < r->nst; j++) {
    Station *s = &r->st[j];
    if (strcmp(s->call, call) != 0) { continue; }
    if (fabs(hz - sta_hz(s, 0)) <= tol || fabs(hz - sta_hz(s, CQ_S)) <= tol ||
        (s->drift != 0 && (hz - sta_hz(s, 0)) * (hz - sta_hz(s, CQ_S)) <= 0)) {
      return s;
    }
  }
  return NULL;
}

static void on_text(double hz, const char *text, gpointer user) {
  Run *r = user;
  for (guint j = 0; j < r->nst; j++) {
    Station *s = &r->st[j];
    const double lo = MIN(sta_hz(s, 0), sta_hz(s, CQ_S)) - 70.0;
    const double hi = MAX(sta_hz(s, 0), sta_hz(s, CQ_S)) + 70.0;
    if (hz >= lo && hz <= hi) { g_string_append(s->text, text); }
  }
}

static void on_station(const SkimStation *st, gpointer user) {
  Run *r = user;
  Station *s = near_call(r, st->call, st->freq_hz, 100.0);
  if (s) { s->got_table = TRUE; } else { r->bad_table++; }
}

static void on_feed(const char *call, double hz, double snr, double wpm,
                    gpointer user) {
  (void)snr; (void)wpm;
  Run *r = user;
  r->feed_lines++;
  Station *s = near_call(r, call, hz, 150.0);
  if (s) {
    s->got_feed = TRUE;
    return;
  }
  r->bad_feed++;
  Station *best = NULL;                   /* blame the nearest own station   */
  for (guint j = 0; j < r->nst; j++) {
    Station *t = &r->st[j];
    if (!t->neighbour &&
        (!best || fabs(hz - sta_hz(t, 0)) < fabs(hz - sta_hz(best, 0)))) {
      best = t;
    }
  }
  if (best) { best->bad++; }
}

/* --- one run -------------------------------------------------------------------- */

static void run_one(gpointer data, gpointer user) {
  (void)user;
  Run *r = data;
  build_run(r);
  SkimPipelineConfig cfg = {
    .chan_bw_hz = CHAN_HZ,
    .mode = SKIM_PIPELINE_MODE_CW,
    .cw_engine = SKIM_CW_ENGINE_V2,
    .rbn_cb = on_feed,
    .rbn_user = r,
  };
  SkimPipeline *p = skim_pipeline_new(&cfg);
  skim_pipeline_set_text_cb(p, on_text, r);
  skim_pipeline_set_station_cb(p, on_station, r);
  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    g_printerr("offline pipeline: %s\n", err ? err->message : "?");
    exit(2);
  }
  GRand *rng = g_rand_new_with_seed(r->seed ^ 0x9e3779b9u);
  Osc *osc = g_new0(Osc, MAX(r->nst, 1));
  float *iq = g_new(float, 2 * BLK);
  const guint64 total = (guint64)(r->secs * RATE);
  for (guint64 at = 0; at < total; at += BLK) {
    const guint n = (guint)MIN((guint64)BLK, total - at);
    synth_block(r, osc, at, n, iq, rng);
    skim_pipeline_feed(p, iq, n, RATE, CENTER);
  }
  skim_pipeline_stop(p);
  skim_pipeline_free(p);
  for (guint j = 0; j < r->nst; j++) {
    Station *s = &r->st[j];
    s->got_text = s->text && strstr(s->text->str, s->call) != NULL;
  }
  g_free(iq);
  g_free(osc);
  g_rand_free(rng);
}

static void run_free(Run *r) {
  for (guint j = 0; j < r->nst; j++) {
    Station *s = &r->st[j];
    if (s->env) { g_array_free(s->env, TRUE); }
    if (s->text) { g_string_free(s->text, TRUE); }
    g_free(s->fre);
    g_free(s->fim);
  }
}

/* --- report --------------------------------------------------------------------- */

typedef struct {
  Scenario sc;
  int      sub;
  double   snr;
  guint    n, text, table, feed, bad, lines;
  double   noise_h;
} Row;

static Row *row_for(GArray *rows, Scenario sc, int sub, double snr) {
  for (guint i = 0; i < rows->len; i++) {
    Row *w = &g_array_index(rows, Row, i);
    if (w->sc == sc && w->sub == sub && fabs(w->snr - snr) < 1e-6) { return w; }
  }
  Row z = { .sc = sc, .sub = sub, .snr = snr };
  g_array_append_val(rows, z);
  return &g_array_index(rows, Row, rows->len - 1);
}

static int row_cmp(gconstpointer a, gconstpointer b) {
  const Row *x = a, *y = b;
  if (x->sc != y->sc) { return x->sc - y->sc; }
  if (x->sub != y->sub) { return x->sub - y->sub; }
  return x->snr < y->snr ? -1 : x->snr > y->snr;
}

static double pc(guint k, guint n) { return n ? 100.0 * k / n : 0.0; }

/* The SNR where the feed recall crosses 50 %, interpolated (NAN if never). */
static double snr50(GArray *rows, guint from, guint to) {
  for (guint i = from; i < to; i++) {
    const Row *w = &g_array_index(rows, Row, i);
    const double y = pc(w->feed, w->n);
    if (y >= 50.0) {
      if (i == from) { return w->snr; }
      const Row *v = &g_array_index(rows, Row, i - 1);
      const double y0 = pc(v->feed, v->n);
      return v->snr + (w->snr - v->snr) * (50.0 - y0) / (y - y0);
    }
  }
  return NAN;
}

/* --- main ----------------------------------------------------------------------- */

typedef struct { Scenario sc; double snr; double recall_min; } Floor;

int main(int argc, char **argv) {
  gboolean gate = FALSE;
  char *scen = NULL, *snr_s = NULL, *scp = NULL, *csv = NULL;
  gint reps = 2, threads = 0;
  double noise_s = 600.0;
  GOptionEntry opts[] = {
    { "gate", 0, 0, G_OPTION_ARG_NONE, &gate, "quick regression gate", NULL },
    { "scenarios", 0, 0, G_OPTION_ARG_STRING, &scen, "comma list", "a,b" },
    { "snr", 0, 0, G_OPTION_ARG_STRING, &snr_s, "SNR500 range", "lo:hi:step" },
    { "reps", 0, 0, G_OPTION_ARG_INT, &reps, "runs (×12 stations) per point", "N" },
    { "threads", 0, 0, G_OPTION_ARG_INT, &threads, "worker threads", "N" },
    { "scp", 0, 0, G_OPTION_ARG_FILENAME, &scp, "MASTER.SCP", "PATH" },
    { "csv", 0, 0, G_OPTION_ARG_FILENAME, &csv, "write rows as CSV", "PATH" },
    { "noise-s", 0, 0, G_OPTION_ARG_DOUBLE, &noise_s, "noise-only run length", "S" },
    { NULL },
  };
  GOptionContext *oc = g_option_context_new("— weak-signal bench");
  g_option_context_add_main_entries(oc, opts, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return 2;
  }
  g_option_context_free(oc);

  /* The SCP: the gate (and a missing user file) gets the fixed pool. */
  char *tmp_scp = NULL;
  char *user_scp = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                    "master.scp", NULL);
  if (!scp && !gate && g_file_test(user_scp, G_FILE_TEST_EXISTS)) {
    scp = g_strdup(user_scp);
  }
  if (!scp) {
    GString *s = g_string_new("# skimmer-sweep fixture\n");
    for (guint i = 0; i < G_N_ELEMENTS(SCP_POOL); i++) {
      g_string_append_printf(s, "%s\n", SCP_POOL[i]);
    }
    int fd = g_file_open_tmp("skimmer-sweep-XXXXXX.scp", &tmp_scp, NULL);
    if (fd >= 0) { close(fd); }
    g_file_set_contents(tmp_scp, s->str, -1, NULL);
    g_string_free(s, TRUE);
    scp = g_strdup(tmp_scp);
  }
  g_free(user_scp);
  if (!skim_callsign_dict_load(scp, &err)) {
    g_printerr("SCP %s: %s\n", scp, err->message);
    return 2;
  }
  load_scp_list(scp);
  if (g_scp_calls->len < 8) {
    g_printerr("SCP %s: too few calls\n", scp);
    return 2;
  }

  /* What to run. */
  gboolean want[SC_COUNT] = { 0 };
  if (gate && !scen) { scen = g_strdup("centre,edge,noise"); }
  if (!scen) { scen = g_strdup("centre,edge,qsb,hand,drift,chirp,nb10,nb20,noise"); }
  char **names = g_strsplit(scen, ",", -1);
  for (char **n = names; *n; n++) {
    guint k;
    for (k = 0; k < SC_COUNT && g_strcmp0(*n, SC_NAME[k]) != 0; k++) {}
    if (k == SC_COUNT) { g_printerr("unknown scenario %s\n", *n); return 2; }
    want[k] = TRUE;
  }
  g_strfreev(names);
  double lo = -6, hi = 20, step = 1;
  if (gate) { lo = 4; hi = 20; step = 4; reps = 1; noise_s = 120; }
  if (snr_s && sscanf(snr_s, "%lf:%lf:%lf", &lo, &hi, &step) != 3) {
    g_printerr("--snr lo:hi:step\n");
    return 2;
  }
  if (threads <= 0) { threads = (gint)g_get_num_processors(); }

  GPtrArray *runs = g_ptr_array_new();
  for (guint sc = 0; sc < SC_COUNT; sc++) {
    if (!want[sc]) { continue; }
    if (sc == SC_NOISE) {
      Run *r = g_new0(Run, 1);
      r->sc = sc;
      r->secs = noise_s;
      r->seed = 0xA5A5u;
      g_ptr_array_add(runs, r);
      continue;
    }
    for (double snr = lo; snr <= hi + 1e-9; snr += step) {
      for (gint k = 0; k < reps; k++) {
        Run *r = g_new0(Run, 1);
        r->sc = sc;
        r->snr = snr;
        r->rep = (guint)k;
        r->seed = (guint32)(sc * 1000003u + (guint)(snr * 16 + 4096) * 7919u + k * 104729u);
        g_ptr_array_add(runs, r);
      }
    }
  }
  printf("=== skimmer-sweep: %u runs on %d threads, SCP %s (%u calls) ===\n",
         runs->len, threads, scp, g_scp_calls->len);
  fflush(stdout);
  const gint64 t0 = g_get_monotonic_time();
  GThreadPool *pool = g_thread_pool_new(run_one, NULL, threads, TRUE, NULL);
  for (guint i = 0; i < runs->len; i++) {
    g_thread_pool_push(pool, g_ptr_array_index(runs, i), NULL);
  }
  g_thread_pool_free(pool, FALSE, TRUE);

  /* Aggregate. */
  GArray *rows = g_array_new(FALSE, FALSE, sizeof(Row));
  for (guint i = 0; i < runs->len; i++) {
    Run *r = g_ptr_array_index(runs, i);
    if (r->sc == SC_NOISE) {
      Row *w = row_for(rows, r->sc, 0, 0);
      w->bad += r->bad_feed;
      w->lines += r->feed_lines;
      w->noise_h += r->secs / 3600.0;
      continue;
    }
    for (guint j = 0; j < r->nst; j++) {
      Station *s = &r->st[j];
      if (s->neighbour) { continue; }
      Row *w = row_for(rows, r->sc, s->sub, r->snr);
      w->n++;
      w->text += s->got_text;
      w->table += s->got_table;
      w->feed += s->got_feed;
      w->bad += s->bad;
    }
  }
  g_array_sort(rows, row_cmp);

  FILE *cf = csv ? fopen(csv, "w") : NULL;
  if (cf) { fprintf(cf, "scenario,sub,snr500,n,text,table,feed,bad,lines,noise_h\n"); }
  printf("%-7s %4s %6s %4s %6s %6s %6s %5s\n", "scen", "sub", "SNR500", "n",
         "text%", "table%", "feed%", "bad");
  for (guint i = 0; i < rows->len; i++) {
    const Row *w = &g_array_index(rows, Row, i);
    if (cf) {
      fprintf(cf, "%s,%d,%.1f,%u,%u,%u,%u,%u,%u,%.3f\n", SC_NAME[w->sc], w->sub,
              w->snr, w->n, w->text, w->table, w->feed, w->bad, w->lines, w->noise_h);
    }
    if (w->sc == SC_NOISE) {
      printf("%-7s %4s %6s %4s %6s %6s %6s %5u  (%.2f h of noise → %.1f bad/h)\n",
             "noise", "-", "-", "-", "-", "-", "-", w->bad, w->noise_h,
             w->bad / MAX(w->noise_h, 1e-9));
      continue;
    }
    printf("%-7s %4d %6.1f %4u %6.0f %6.0f %6.0f %5u\n", SC_NAME[w->sc], w->sub,
           w->snr, w->n, pc(w->text, w->n), pc(w->table, w->n), pc(w->feed, w->n),
           w->bad);
  }
  if (cf) { fclose(cf); }

  /* Figure of merit: feed recall 50 % point per scenario (lower = better). */
  printf("--- SNR500 at 50 %% feed recall\n");
  for (guint i = 0; i < rows->len;) {
    const Row *w = &g_array_index(rows, Row, i);
    guint j = i;
    while (j < rows->len && g_array_index(rows, Row, j).sc == w->sc &&
           g_array_index(rows, Row, j).sub == w->sub) { j++; }
    if (w->sc != SC_NOISE) {
      const double x = snr50(rows, i, j);
      if (isnan(x)) {
        printf("  %-7s %4d   never\n", SC_NAME[w->sc], w->sub);
      } else {
        printf("  %-7s %4d   %5.1f dB\n", SC_NAME[w->sc], w->sub, x);
      }
    }
    i = j;
  }
  printf("(%.0f s)\n", (g_get_monotonic_time() - t0) / 1e6);

  int fails = 0;
  if (gate) {
    /* Regression floor — the v2 baseline of 2026-10-02 less a margin; raise
     * it as the weak-signal work lands. */
    static const Floor FLOORS[] = {
      { SC_CENTRE,  8, 90.0 }, { SC_CENTRE, 20, 95.0 },  /* baseline 100, 100 */
      { SC_EDGE,   12, 75.0 }, { SC_EDGE,   20, 90.0 },  /* baseline  83, 100 */
    };
    printf("=== gate ===\n");
    for (guint k = 0; k < G_N_ELEMENTS(FLOORS); k++) {
      const Floor *f = &FLOORS[k];
      const Row *w = row_for(rows, f->sc, 0, f->snr);
      const double y = pc(w->feed, w->n);
      const gboolean ok = w->n && y >= f->recall_min;
      fails += !ok;
      printf("  %-4s %s at %.0f dB: feed %.0f %% ≥ %.0f %%\n", ok ? "ok" : "FAIL",
             SC_NAME[f->sc], f->snr, y, f->recall_min);
    }
    const Row *nz = row_for(rows, SC_NOISE, 0, 0);
    const gboolean ok = nz->bad == 0;
    fails += !ok;
    printf("  %-4s pure noise: %u bad feed lines\n", ok ? "ok" : "FAIL", nz->bad);
  }

  for (guint i = 0; i < runs->len; i++) {
    run_free(g_ptr_array_index(runs, i));
    g_free(g_ptr_array_index(runs, i));
  }
  g_ptr_array_free(runs, TRUE);
  g_array_free(rows, TRUE);
  if (tmp_scp) { g_unlink(tmp_scp); g_free(tmp_scp); }
  g_free(scp);
  return fails ? 1 : 0;
}
