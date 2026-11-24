/* js8.c — JS8 beside the skimmer (js8.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "js8.h"
#include "digi_rx.h"
#include "helper_run.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "callsign.h"
#include "json.h"

static void jstr(GString *s, const char *v) {   /* a quoted JSON string */
  g_string_append_c(s, '"');
  skim_json_append(s, v);
  g_string_append_c(s, '"');
}

/* ---- submodes, dials ------------------------------------------------------------- */

static const struct {
  int number;                         /* JS8's own (skimmer-js8d -m)          */
  const char *name;
  guint period_ms;
  double tone_hz;                     /* tone spacing                         */
} SUB[SKIM_JS8_NSUB] = {
  { 0, "normal", 15000, 6.25 },
  { 1, "fast", 10000, 10.0 },
  { 2, "turbo", 6000, 20.0 },
  { 4, "slow", 30000, 3.125 },
};

int skim_js8_sub_number(guint index) { return index < SKIM_JS8_NSUB ? SUB[index].number : -1; }
const char *skim_js8_sub_name(guint index) { return index < SKIM_JS8_NSUB ? SUB[index].name : "?"; }
guint skim_js8_sub_period_ms(guint index) { return index < SKIM_JS8_NSUB ? SUB[index].period_ms : 0; }

int skim_js8_sub_index(int number) {
  for (guint i = 0; i < SKIM_JS8_NSUB; i++) {
    if (SUB[i].number == number) { return (int)i; }
  }
  return -1;
}

gboolean skim_js8_parse_submodes(const char *text, guint *bits) {
  *bits = 0;
  char **w = g_strsplit_set(text ? text : "", ", ", -1);
  gboolean ok = TRUE;
  for (char **x = w; *x; x++) {
    if (!**x) { continue; }
    gboolean hit = FALSE;
    for (guint i = 0; i < SKIM_JS8_NSUB; i++) {
      if (g_ascii_strcasecmp(*x, SUB[i].name) == 0) { *bits |= 1u << i; hit = TRUE; }
    }
    ok &= hit;
  }
  g_strfreev(w);
  return ok && *bits;
}

/* JS8Call's default dial frequencies (its frequency list). */
const SkimJs8Dial SKIM_JS8_DIALS[] = {
  { "160m", 1842000 },  { "80m", 3578000 },   { "40m", 7078000 },   { "30m", 10130000 },
  { "20m", 14078000 },  { "17m", 18104000 },  { "15m", 21078000 },  { "12m", 24922000 },
  { "10m", 28078000 },  { "6m", 50318000 },
  { NULL, 0 },
};

gboolean skim_js8_covers(double centre, double rate, double dial_hz, double lo_hz, double hi_hz) {
  const double edge = 0.45 * rate;
  return dial_hz + lo_hz >= centre - edge && dial_hz + hi_hz <= centre + edge;
}

/* ---- a frame --------------------------------------------------------------------- */

gboolean skim_js8_parse_line(const char *line, SkimJs8Decode *out) {
  memset(out, 0, sizeof *out);
  char hhmmss[8], raw[16];
  int sub = -1, snr = 0, type = 0;
  double dt = 0, df = 0;
  float q = 0;
  if (!line || sscanf(line, "%7s %d %d %lf %lf %d %f %15s", hhmmss, &sub, &snr, &dt, &df, &type,
                      &q, raw) != 8) {
    return FALSE;
  }
  const int idx = skim_js8_sub_index(sub);
  if (idx < 0 || strlen(hhmmss) != 6 || strlen(raw) != 12 || df < 0 || df > 6000) { return FALSE; }
  out->sub = (guint)idx;
  out->snr = snr;
  out->dt = dt;
  out->df = df;
  out->type = type & 7;
  out->quality = q;
  g_strlcpy(out->raw, raw, sizeof out->raw);
  return skim_js8_unpack(out->raw, out->type, &out->f);
}

gboolean skim_js8_sender(const SkimJs8Frame *f, char *call, gsize size) {
  call[0] = 0;
  if (f->kind != SKIM_JS8_HEARTBEAT && f->kind != SKIM_JS8_COMPOUND && f->kind != SKIM_JS8_DIRECTED &&
      f->kind != SKIM_JS8_COMPOUND_DIRECTED) {
    return FALSE;
  }
  const char *c = f->from;
  if (!c[0] || c[0] == '@' || c[0] == '<' || !skim_callsign_is_valid(c)) { return FALSE; }
  g_strlcpy(call, c, size);
  return TRUE;
}

/* ---- the decoder program ---------------------------------------------------------- */

char *skim_js8_find_decoder(void) {
  char *self = g_file_read_link("/proc/self/exe", NULL);
  if (self) {
    char *dir = g_path_get_dirname(self);
    const char *const tries[] = { "skimmer-js8d", "vendor/js8call/skimmer-js8d",
                                  "../vendor/js8call/skimmer-js8d", NULL };
    for (const char *const *t = tries; *t; t++) {
      char *p = g_build_filename(dir, *t, NULL);
      if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE) && !g_file_test(p, G_FILE_TEST_IS_DIR)) {
        char *c = g_canonicalize_filename(p, NULL);
        g_free(p); g_free(dir); g_free(self);
        return c;
      }
      g_free(p);
    }
    g_free(dir);
    g_free(self);
  }
  return g_find_program_in_path("skimmer-js8d");
}

/* ---- the whole ------------------------------------------------------------------- */

typedef struct _Job Job;

typedef struct {
  SkimHdJs8  *j;
  SkimDigiRx *rx;
  guint       band;                     /* the receiver                          */
  const char *name;                     /* SKIM_JS8_DIALS' band                  */
  char        rx_name[16];              /* the receiver's band name (its row)    */
  double      dial_hz;
  guint       sub_of[SKIM_JS8_NSUB];    /* digi_rx period → submode index        */
  char       *work;                     /* its own work dir (the helper's cwd)   */
  char       *wdir[SKIM_JS8_NSUB];      /* … a subdir a submode: periods of two
                                         * submodes that start together have the
                                         * same YYMMDD_HHMMSS.wav name            */
  char       *keep[SKIM_JS8_NSUB];      /* … or where its WAVs stay (keep_wav)   */
  Job        *waiting[SKIM_JS8_NSUB];   /* a period not yet started (j->lock)    */
  SkimHdHelperServer *srv;              /* --serve: the dial's helper            */
  GMutex      srv_lock;                 /* one job at a time through it          */
  struct {                              /* the last compound sender (j->lock)    */
    gint64 t_ms;
    guint  sub;
    double df;
    char   call[20];
  } compound;
} Slot;

struct _Job {
  gint64  start_ms;
  guint   slot, sub;
  gint16 *pcm;
  guint   n;
  gboolean skip;                        /* a newer period of the slot came first */
};

typedef struct {
  gint64 last_ms;                       /* the newest decode's period            */
} Heard;

struct _SkimHdJs8 {
  char    *dir, *work, *decoder, *known_path;
  guint    submodes;
  double   lo_hz, hi_hz, clean_q;
  gboolean serve;
  SkimHdJs8SpotCb spot;
  gpointer user;
  Slot     slot[SKIM_JS8_MAX_SLOTS];
  guint    nslots;
  GPtrArray *by_band[16];               /* receiver → its slots (Slot *)         */
  GThreadPool *pool;
  GCancellable *cancel;
  GMutex   lock;                        /* below                                  */
  GCond    idle;
  guint    pending;
  GArray  *frames;                      /* SkimJs8Decode, the last 15 min, oldest first */
  GHashTable *fed_feed, *fed_page;      /* "CALL|band" → gint64 *: last spot (ms) */
  GHashTable *heard;                    /* call → Heard: the second-decode check */
  GHashTable *known;                    /* call → gint64 *: confirmed (unix s)   */
  gboolean known_dirty;
  gint64   known_saved;                 /* monotonic s                           */
  gint64   newest_ms;
  struct {
    guint spots, appeared;
    GHashTable *heard;
    GArray *snr;
  } min[16];
  GHashTable *seen;                     /* "CALL|rx" → gint64 *: last decoded (ms) */
  GHashTable *grids;                    /* call → its locator                    */
  guint64  cycles, ndecodes, ndeep, nconfirmed, skipped, spots;
  gint64   last_cycle;
  guint    last_decodes;
  char    *error;
};

static void set_error(SkimHdJs8 *j, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_error(SkimHdJs8 *j, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *m = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_mutex_lock(&j->lock);
  const gboolean same = g_strcmp0(j->error, m) == 0;
  g_free(j->error);
  j->error = m;
  g_mutex_unlock(&j->lock);
  if (!same) { g_warning("js8: %s", m); }
}

/* ---- the calls confirmed on JS8 (js8-known.tsv: CALL<TAB>unix) --------------------- */

static void known_load(SkimHdJs8 *j) {
  char *text = NULL;
  if (!j->known_path || !g_file_get_contents(j->known_path, &text, NULL, NULL)) { return; }
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  char **lines = g_strsplit(text, "\n", -1);
  for (char **ln = lines; *ln; ln++) {
    char call[20];
    long long t = 0;
    if (sscanf(*ln, "%19s %lld", call, &t) != 2 || now - t > (gint64)SKIM_JS8_KNOWN_DAYS * 86400 ||
        !skim_callsign_is_valid(call)) {
      continue;
    }
    gint64 *v = g_new(gint64, 1);
    *v = t;
    g_hash_table_replace(j->known, g_strdup(call), v);
  }
  g_strfreev(lines);
  g_free(text);
}

static char *known_text(SkimHdJs8 *j) {           /* lock held */
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  GString *s = g_string_new("# calls confirmed on JS8 (skimmer-headless), call<TAB>unix time\n");
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, j->known);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    if (now - *(gint64 *)v > (gint64)SKIM_JS8_KNOWN_DAYS * 86400) {
      g_hash_table_iter_remove(&it);
      continue;
    }
    g_string_append_printf(s, "%s\t%" G_GINT64_FORMAT "\n", (const char *)k, *(gint64 *)v);
  }
  j->known_dirty = FALSE;
  j->known_saved = g_get_monotonic_time() / G_USEC_PER_SEC;
  return g_string_free(s, FALSE);
}

static void known_write(SkimHdJs8 *j, char *text) {
  GError *e = NULL;
  if (text && j->known_path && !g_file_set_contents(j->known_path, text, -1, &e)) {
    g_warning("js8: %s", e->message);
    g_error_free(e);
  }
  g_free(text);
}

static gboolean scp_has(const char *call) {
  if (skim_callsign_dict_has(call)) { return TRUE; }
  const char *slash = strrchr(call, '/');                 /* OK1ABC/P → OK1ABC */
  if (slash && slash - call >= 3) {
    char base[20];
    g_strlcpy(base, call, MIN(sizeof base, (gsize)(slash - call) + 1));
    return skim_callsign_dict_has(base);
  }
  return FALSE;
}

/* ---- the jobs -------------------------------------------------------------------- */

static void job_done(SkimHdJs8 *j) {          /* lock held */
  if (j->pending && --j->pending == 0) { g_cond_broadcast(&j->idle); }
}

static void cycle_cb(guint period, gint64 start_ms, gint16 *pcm, guint n, gpointer user) {
  Slot *s = user;
  SkimHdJs8 *j = s->j;
  Job *b = g_new0(Job, 1);
  b->start_ms = start_ms;
  b->slot = (guint)(s - j->slot);
  b->sub = s->sub_of[period];
  b->pcm = pcm;
  b->n = n;
  g_mutex_lock(&j->lock);
  if (s->waiting[b->sub]) {                     /* still queued: the machine is behind */
    s->waiting[b->sub]->skip = TRUE;
    j->skipped++;
  }
  s->waiting[b->sub] = b;
  j->pending++;
  g_mutex_unlock(&j->lock);
  g_thread_pool_push(j->pool, b, NULL);
}

static void expire(SkimHdJs8 *j) {             /* lock held */
  const gint64 keep = j->newest_ms - (gint64)SKIM_JS8_KEEP_S * 1000;
  guint k = 0;
  while (k < j->frames->len && g_array_index(j->frames, SkimJs8Decode, k).t_ms < keep) { k++; }
  if (k) { g_array_remove_range(j->frames, 0, k); }
  GHashTable *const tabs[] = { j->fed_feed, j->fed_page, j->seen };
  for (guint t = 0; t < G_N_ELEMENTS(tabs); t++) {
    if (g_hash_table_size(tabs[t]) <= 4096) { continue; }
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, tabs[t]);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
      if (*(gint64 *)v < j->newest_ms - (gint64)SKIM_JS8_FOLD_S * 1000) { g_hash_table_iter_remove(&it); }
    }
  }
  if (g_hash_table_size(j->heard) > 4096) {
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, j->heard);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
      if (((Heard *)v)->last_ms < j->newest_ms - (gint64)SKIM_JS8_CONFIRM_S * 1000) {
        g_hash_table_iter_remove(&it);
      }
    }
  }
}

/* The helper over one WAV: its lines in *out. */
static gboolean run_decoder(SkimHdJs8 *j, Slot *s, guint sub, const char *wav, char **out,
                            GError **error) {
  char m[8], lo[16], hi[16];
  g_snprintf(m, sizeof m, "%d", SUB[sub].number);
  g_snprintf(lo, sizeof lo, "%.0f", j->lo_hz);
  g_snprintf(hi, sizeof hi, "%.0f", j->hi_hz);
  if (!j->serve) {
    char *prog = g_canonicalize_filename(j->decoder, NULL);   /* it runs in work */
    const char *argv[] = { prog, "-m", m, "-L", lo, "-H", hi, wav, NULL };
    const gboolean ok = skim_hd_helper_run(argv, s->work, j->decoder, j->cancel, out, error);
    g_free(prog);
    return ok;
  }
  char *line = g_strdup_printf("%s %s %s %s", m, lo, hi, wav);
  g_mutex_lock(&s->srv_lock);
  if (!s->srv) {
    char *prog = g_canonicalize_filename(j->decoder, NULL);
    const char *argv[] = { prog, "--serve", NULL };
    s->srv = skim_hd_helper_server_new(argv, s->work, j->decoder, j->cancel);
    g_free(prog);
  }
  gboolean ok = skim_hd_helper_server_ask(s->srv, line, "<DecodeFinished>", out, error);
  g_mutex_unlock(&s->srv_lock);
  g_free(line);
  if (ok && *out) {                               /* the helper refused the job */
    const char *err = strstr(*out, "<Error>");
    if (err) {
      const char *end = strchr(err, '\n');
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%.*s", end ? (int)(end - err) : (int)strlen(err),
                  err);
      g_clear_pointer(out, g_free);
      ok = FALSE;
    }
  }
  return ok;
}

typedef struct {
  char   call[20], grid[8];
  double freq_hz;
  int    snr;
  gboolean cq, confirmed;
  gint64 t;
} Out;

/* A frame's sender, its confirmation, its spot (lock held). */
static void take_frame(SkimHdJs8 *j, Slot *s, SkimJs8Decode *d, GArray *out) {
  /* a compound-directed frame's sender is the compound frame before it in
   * the same transmission: same submode, same frequency, the period before
   * or the same one */
  if (d->f.kind == SKIM_JS8_COMPOUND && d->f.from[0]) {
    s->compound.t_ms = d->t_ms;
    s->compound.sub = d->sub;
    s->compound.df = d->df;
    g_strlcpy(s->compound.call, d->f.from, sizeof s->compound.call);
  } else if (d->f.kind == SKIM_JS8_COMPOUND_DIRECTED && s->compound.call[0] &&
             s->compound.sub == d->sub && fabs(s->compound.df - d->df) <= 4 * SUB[d->sub].tone_hz &&
             d->t_ms >= s->compound.t_ms && d->t_ms - s->compound.t_ms <= (gint64)SUB[d->sub].period_ms) {
    g_strlcpy(d->f.from, s->compound.call, sizeof d->f.from);
  }
  d->deep = d->quality < j->clean_q;
  if (d->deep) { j->ndeep++; }
  const float snr = (float)d->snr;
  g_array_append_val(j->min[s->band].snr, snr);
  if (!skim_js8_sender(&d->f, d->call, sizeof d->call)) { return; }

  /* confirmed: a clean frame; a deep one decoded before in another period
   * within 30 min, in MASTER.SCP, or confirmed on JS8 before */
  Heard *h = g_hash_table_lookup(j->heard, d->call);
  if (!d->deep) {
    d->confirmed = TRUE;
  } else if (h && h->last_ms != d->t_ms && llabs(d->t_ms - h->last_ms) <= (gint64)SKIM_JS8_CONFIRM_S * 1000) {
    d->confirmed = TRUE;
  } else if (g_hash_table_contains(j->known, d->call) || scp_has(d->call)) {
    d->confirmed = TRUE;
  }
  if (!h) {
    h = g_new0(Heard, 1);
    g_hash_table_insert(j->heard, g_strdup(d->call), h);
  }
  h->last_ms = MAX(h->last_ms, d->t_ms);
  if (d->confirmed) {
    j->nconfirmed++;
    gint64 *k = g_hash_table_lookup(j->known, d->call);
    if (!k) {
      k = g_new(gint64, 1);
      g_hash_table_insert(j->known, g_strdup(d->call), k);
    }
    *k = d->t_ms / 1000;
    j->known_dirty = TRUE;
  }
  if (d->f.grid[0]) {
    if (g_hash_table_size(j->grids) > 50000) { g_hash_table_remove_all(j->grids); }
    g_hash_table_replace(j->grids, g_strdup(d->call), g_strdup(d->f.grid));
  }
  /* the minute's history: each sender heard, and whether it appeared */
  g_hash_table_add(j->min[s->band].heard, g_strdup(d->call));
  char *sk = g_strdup_printf("%s|%u", d->call, s->band);
  gint64 *at = g_hash_table_lookup(j->seen, sk);
  if (!at || d->t_ms - *at >= (gint64)SKIM_JS8_FOLD_S * 1000) { j->min[s->band].appeared++; }
  if (!at) {
    at = g_new(gint64, 1);
    *at = d->t_ms;
    g_hash_table_insert(j->seen, sk, at);
  } else {
    g_free(sk);
  }
  *at = MAX(*at, d->t_ms);

  /* a spot once a call and band in 10 min: to the feed once confirmed,
   * to the page only before */
  if (!j->spot) { return; }
  char *key = g_strdup_printf("%s|%s", d->call, s->name);
  gint64 *ff = g_hash_table_lookup(j->fed_feed, key), *fp = g_hash_table_lookup(j->fed_page, key);
  const gint64 fold = (gint64)SKIM_JS8_FOLD_S * 1000;
  const gboolean feed_fresh = ff && d->t_ms - *ff < fold && d->t_ms >= *ff;
  const gboolean page_fresh = fp && d->t_ms - *fp < fold && d->t_ms >= *fp;
  gboolean go = FALSE;
  if (d->confirmed && !feed_fresh) {
    go = TRUE;
    if (!ff) { ff = g_new(gint64, 1); g_hash_table_insert(j->fed_feed, g_strdup(key), ff); }
    *ff = d->t_ms;
  } else if (!d->confirmed && !feed_fresh && !page_fresh) {
    go = TRUE;
  }
  if (go) {
    if (!fp) { fp = g_new(gint64, 1); g_hash_table_insert(j->fed_page, g_strdup(key), fp); }
    *fp = d->t_ms;
    j->min[s->band].spots++;
    Out o = { .freq_hz = s->dial_hz + d->df, .snr = d->snr, .cq = d->f.kind == SKIM_JS8_HEARTBEAT,
              .confirmed = d->confirmed, .t = d->t_ms / 1000 };
    g_strlcpy(o.call, d->call, sizeof o.call);
    const char *g = d->f.grid[0] ? d->f.grid : g_hash_table_lookup(j->grids, d->call);
    g_strlcpy(o.grid, g ? g : "", sizeof o.grid);
    g_array_append_val(out, o);
  }
  g_free(key);
}

static void decode_job(gpointer data, gpointer user) {
  Job *b = data;
  SkimHdJs8 *j = user;
  g_mutex_lock(&j->lock);
  const gboolean skip = b->skip || g_cancellable_is_cancelled(j->cancel);
  Slot *s = &j->slot[b->slot];
  if (s->waiting[b->sub] == b) { s->waiting[b->sub] = NULL; }
  if (skip) {
    job_done(j);
    g_mutex_unlock(&j->lock);
    g_free(b->pcm);
    g_free(b);
    return;
  }
  g_mutex_unlock(&j->lock);

  GError *e = NULL;
  GArray *got = g_array_new(FALSE, TRUE, sizeof(SkimJs8Decode));
  gboolean ok = FALSE;
  char *wav = NULL, *text = NULL;
  const char *wdir = s->keep[b->sub] ? s->keep[b->sub] : s->wdir[b->sub];
  if (!j->decoder) {
    set_error(j, "no decoder (skimmer-js8d) found — nothing is decoded");
  } else if (!(wav = skim_digi_write_wav(wdir, b->start_ms, b->pcm, b->n, &e))) {
    set_error(j, "%s", e->message);
    g_clear_error(&e);
  } else {
    ok = run_decoder(j, s, b->sub, wav, &text, &e);
    if (!s->keep[b->sub]) { g_unlink(wav); }
    if (!ok) {
      if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        set_error(j, "%s %s: %s", s->name, SUB[b->sub].name, e->message);
      }
      g_clear_error(&e);
    } else {
      char **lines = g_strsplit(text ? text : "", "\n", -1);
      for (char **ln = lines; *ln; ln++) {
        SkimJs8Decode x;
        if (!skim_js8_parse_line(*ln, &x)) { continue; }
        x.t_ms = b->start_ms;
        x.slot = b->slot;
        g_array_append_val(got, x);
      }
      g_strfreev(lines);
    }
  }
  g_free(wav);
  g_free(text);

  GArray *out = g_array_new(FALSE, TRUE, sizeof(Out));
  char *save = NULL;
  g_mutex_lock(&j->lock);
  if (ok) {
    if (b->start_ms > j->newest_ms) { j->newest_ms = b->start_ms; }
    for (guint i = 0; i < got->len; i++) {
      SkimJs8Decode *d = &g_array_index(got, SkimJs8Decode, i);
      take_frame(j, s, d, out);
      g_array_append_val(j->frames, *d);
    }
    expire(j);
    j->cycles++;
    j->ndecodes += got->len;
    if (b->start_ms > j->last_cycle) {
      j->last_cycle = b->start_ms;
      j->last_decodes = got->len;
    } else if (b->start_ms == j->last_cycle) {
      j->last_decodes += got->len;
    }
    j->spots += out->len;
    if (j->error && j->decoder) { g_clear_pointer(&j->error, g_free); }
    if (j->known_dirty && j->known_path &&
        g_get_monotonic_time() / G_USEC_PER_SEC - j->known_saved >= 300) {
      save = known_text(j);
    }
  }
  g_mutex_unlock(&j->lock);
  known_write(j, save);
  for (guint i = 0; i < out->len; i++) {        /* outside the lock: the feed's own */
    const Out *o = &g_array_index(out, Out, i);
    j->spot(o->call, o->freq_hz, o->snr, o->cq, s->name, o->grid, o->t, o->confirmed, j->user);
  }
  g_array_unref(out);
  g_array_unref(got);
  g_mutex_lock(&j->lock);
  job_done(j);
  g_mutex_unlock(&j->lock);
  g_free(b->pcm);
  g_free(b);
}

/* ---- life ------------------------------------------------------------------------ */

SkimHdJs8 *skim_hd_js8_new(const SkimHdJs8Config *cfg, guint nb, const char *const *names,
                           const double *centres, double rate) {
  SkimHdJs8 *j = g_new0(SkimHdJs8, 1);
  g_mutex_init(&j->lock);
  g_cond_init(&j->idle);
  j->submodes = cfg->submodes & SKIM_JS8_M_ALL;
  j->lo_hz = cfg->lo_hz;
  j->hi_hz = cfg->hi_hz;
  j->serve = cfg->serve;
  j->clean_q = cfg->clean_quality;
  j->spot = cfg->spot;
  j->user = cfg->user;
  j->dir = g_strdup(cfg->dir);
  j->work = g_build_filename(cfg->dir, "js8", NULL);
  j->decoder = cfg->decoder ? g_strdup(cfg->decoder) : skim_js8_find_decoder();
  if (!cfg->no_known) { j->known_path = g_build_filename(cfg->dir, "js8-known.tsv", NULL); }
  j->frames = g_array_new(FALSE, TRUE, sizeof(SkimJs8Decode));
  j->fed_feed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->fed_page = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->heard = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->grids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  j->known_saved = g_get_monotonic_time() / G_USEC_PER_SEC;
  known_load(j);
  for (guint b = 0; b < G_N_ELEMENTS(j->min); b++) {
    j->min[b].heard = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    j->min[b].snr = g_array_new(FALSE, FALSE, sizeof(float));
  }
  j->cancel = g_cancellable_new();
  guint periods[SKIM_JS8_NSUB], np = 0, sub_of[SKIM_JS8_NSUB];
  for (guint i = 0; i < SKIM_JS8_NSUB; i++) {
    if (j->submodes & (1u << i)) {
      sub_of[np] = i;
      periods[np++] = SUB[i].period_ms;
    }
  }
  for (guint b = 0; b < nb && b < G_N_ELEMENTS(j->by_band); b++) {
    j->by_band[b] = g_ptr_array_new();
    gboolean want = !cfg->bands;                       /* [js8] bands: this receiver? */
    for (const char *const *x = cfg->bands; x && *x && !want; x++) {
      want = names && names[b] && strcmp(names[b], *x) == 0;
    }
    for (const SkimJs8Dial *d = SKIM_JS8_DIALS; want && np && d->band; d++) {
      if (!skim_js8_covers(centres[b], rate, d->dial_hz, j->lo_hz, j->hi_hz) ||
          j->nslots >= SKIM_JS8_MAX_SLOTS) {
        continue;
      }
      gboolean dup = FALSE;                            /* another receiver has it */
      for (guint k = 0; k < j->nslots; k++) { dup |= j->slot[k].dial_hz == d->dial_hz; }
      if (dup) { continue; }
      Slot *s = &j->slot[j->nslots];
      s->j = j;
      s->band = b;
      s->name = d->band;
      g_strlcpy(s->rx_name, names && names[b] ? names[b] : d->band, sizeof s->rx_name);
      s->dial_hz = d->dial_hz;
      memcpy(s->sub_of, sub_of, sizeof sub_of);
      g_mutex_init(&s->srv_lock);
      char sub[16];
      g_snprintf(sub, sizeof sub, "%u", j->nslots);
      s->work = g_build_filename(j->work, sub, NULL);
      if (g_mkdir_with_parents(s->work, 0755) != 0) {
        set_error(j, "%s: %s", s->work, g_strerror(errno));
      }
      for (guint k = 0; k < np; k++) {
        s->wdir[sub_of[k]] = g_build_filename(s->work, SUB[sub_of[k]].name, NULL);
        g_mkdir_with_parents(s->wdir[sub_of[k]], 0755);
      }
      for (guint k = 0; cfg->keep_wav && k < np; k++) {
        char *kd = g_strdup_printf("%s-%s", d->band, SUB[sub_of[k]].name);
        s->keep[sub_of[k]] = g_build_filename(cfg->keep_wav, kd, NULL);
        g_free(kd);
        g_mkdir_with_parents(s->keep[sub_of[k]], 0755);
      }
      s->rx = skim_digi_rx_new(rate, centres[b], d->dial_hz, j->lo_hz, j->hi_hz, periods, np,
                               cycle_cb, s);
      g_ptr_array_add(j->by_band[b], s);
      j->nslots++;
    }
  }
  if (!j->decoder) { set_error(j, "no decoder (skimmer-js8d) found — nothing is decoded"); }
  j->pool = g_thread_pool_new(decode_job, j, (gint)MAX(cfg->threads, 1u), FALSE, NULL);
  return j;
}

static int float_cmp(gconstpointer a, gconstpointer b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

gboolean skim_hd_js8_minute(SkimHdJs8 *j, guint b, SkimHdJs8Minute *out) {
  memset(out, 0, sizeof *out);
  out->snr = NAN;
  if (!j || b >= G_N_ELEMENTS(j->by_band) || !j->by_band[b] || !j->by_band[b]->len) { return FALSE; }
  g_mutex_lock(&j->lock);
  out->spots = j->min[b].spots;
  out->appeared = j->min[b].appeared;
  out->heard = g_hash_table_size(j->min[b].heard);
  GArray *a = j->min[b].snr;
  if (a->len) {
    g_array_sort(a, float_cmp);
    out->snr = g_array_index(a, float, a->len / 2);
  }
  j->min[b].spots = j->min[b].appeared = 0;
  g_hash_table_remove_all(j->min[b].heard);
  g_array_set_size(a, 0);
  g_mutex_unlock(&j->lock);
  return TRUE;
}

void skim_hd_js8_drain(SkimHdJs8 *j) {
  g_mutex_lock(&j->lock);
  while (j->pending) { g_cond_wait(&j->idle, &j->lock); }
  g_mutex_unlock(&j->lock);
}

void skim_hd_js8_free(SkimHdJs8 *j) {
  if (!j) { return; }
  g_cancellable_cancel(j->cancel);
  g_thread_pool_free(j->pool, FALSE, TRUE);   /* queued jobs run and see the cancel */
  g_mutex_lock(&j->lock);
  char *save = j->known_dirty ? known_text(j) : NULL;
  g_mutex_unlock(&j->lock);
  known_write(j, save);
  for (guint k = 0; k < j->nslots; k++) {
    Slot *s = &j->slot[k];
    skim_digi_rx_free(s->rx);
    skim_hd_helper_server_free(s->srv);
    g_mutex_clear(&s->srv_lock);
    g_free(s->work);
    for (guint i = 0; i < SKIM_JS8_NSUB; i++) { g_free(s->keep[i]); g_free(s->wdir[i]); }
  }
  for (guint b = 0; b < G_N_ELEMENTS(j->by_band); b++) {
    if (j->by_band[b]) { g_ptr_array_free(j->by_band[b], TRUE); }
  }
  g_object_unref(j->cancel);
  g_array_unref(j->frames);
  g_hash_table_unref(j->fed_feed);
  g_hash_table_unref(j->fed_page);
  g_hash_table_unref(j->heard);
  g_hash_table_unref(j->known);
  g_hash_table_unref(j->seen);
  g_hash_table_unref(j->grids);
  for (guint b = 0; b < G_N_ELEMENTS(j->min); b++) {
    g_hash_table_unref(j->min[b].heard);
    g_array_unref(j->min[b].snr);
  }
  g_free(j->dir); g_free(j->work); g_free(j->decoder); g_free(j->known_path); g_free(j->error);
  g_cond_clear(&j->idle);
  g_mutex_clear(&j->lock);
  g_free(j);
}

void skim_hd_js8_push(SkimHdJs8 *j, guint b, const float *iq, guint n) {
  if (!j || b >= G_N_ELEMENTS(j->by_band) || !j->by_band[b] || !j->by_band[b]->len) { return; }
  skim_hd_js8_push_at(j, b, iq, n, g_get_real_time());
}

void skim_hd_js8_push_at(SkimHdJs8 *j, guint b, const float *iq, guint n, gint64 now) {
  if (!j || b >= G_N_ELEMENTS(j->by_band) || !j->by_band[b]) { return; }
  for (guint k = 0; k < j->by_band[b]->len; k++) {
    skim_digi_rx_feed(((Slot *)j->by_band[b]->pdata[k])->rx, iq, n, now);
  }
}

char *skim_hd_js8_dials_text(SkimHdJs8 *j) {
  GString *s = g_string_new(NULL);
  for (guint k = 0; k < j->nslots; k++) {
    g_string_append_printf(s, "%s%s %.1f", k ? ", " : "", j->slot[k].name, j->slot[k].dial_hz / 1e3);
  }
  return g_string_free(s, FALSE);
}

char *skim_hd_js8_status_json(SkimHdJs8 *j) {
  GString *s = g_string_new("{\"on\":true,\"decoder\":");
  g_string_append(s, j->decoder ? "true" : "false");
  g_string_append_printf(s, ",\"serve\":%s,\"lo_hz\":%.0f,\"hi_hz\":%.0f,\"submodes\":[",
                         j->serve ? "true" : "false", j->lo_hz, j->hi_hz);
  guint ns = 0;
  for (guint i = 0; i < SKIM_JS8_NSUB; i++) {
    if (!(j->submodes & (1u << i))) { continue; }
    g_string_append_printf(s, "%s{\"name\":\"%s\",\"period_ms\":%u,\"tone_hz\":", ns++ ? "," : "",
                           SUB[i].name, SUB[i].period_ms);
    char t[16];
    g_ascii_formatd(t, sizeof t, "%.3f", SUB[i].tone_hz);
    g_string_append_printf(s, "%s}", t);
  }
  g_string_append(s, "],\"dials\":[");
  for (guint k = 0; k < j->nslots; k++) {
    const Slot *x = &j->slot[k];
    g_string_append_printf(s, "%s{\"band\":", k ? "," : "");
    jstr(s, x->name);
    g_string_append_printf(s, ",\"mode\":\"JS8\",\"dial_hz\":%.0f,\"rx\":%u,\"lo_hz\":%.0f,\"hi_hz\":%.0f}",
                           x->dial_hz, x->band, x->dial_hz + j->lo_hz, x->dial_hz + j->hi_hz);
  }
  g_mutex_lock(&j->lock);
  g_string_append(s, "],\"rx\":[");
  const gint64 now_ms = g_get_real_time() / 1000;
  const gint64 since = now_ms - (gint64)SKIM_JS8_RECENT_S * 1000;
  /* "active": the senders of the last minute (a period's frames come after
   * it ends — 75 s holds a minute of them whatever the submode) */
  const gint64 active_since = now_ms - 75 * 1000;
  guint nrx = 0;
  for (guint k = 0; k < j->nslots; k++) {
    gboolean first = TRUE;
    for (guint q = 0; q < k; q++) { first &= j->slot[q].band != j->slot[k].band; }
    if (!first) { continue; }
    GHashTable *calls = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *now = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *per[SKIM_JS8_NSUB];
    for (guint i = 0; i < SKIM_JS8_NSUB; i++) { per[i] = g_hash_table_new(g_str_hash, g_str_equal); }
    const char *last = "";
    guint deep = 0;
    for (guint i = 0; i < j->frames->len; i++) {
      const SkimJs8Decode *d = &g_array_index(j->frames, SkimJs8Decode, i);
      if (d->t_ms < since || !d->call[0] || j->slot[d->slot].band != j->slot[k].band) { continue; }
      g_hash_table_add(calls, (gpointer)d->call);
      if (d->t_ms >= active_since) {
        g_hash_table_add(now, (gpointer)d->call);
        g_hash_table_add(per[d->sub], (gpointer)d->call);
        deep += d->deep && !d->confirmed;
      }
      last = d->call;
    }
    g_string_append_printf(s, "%s{\"band\":", nrx++ ? "," : "");
    jstr(s, j->slot[k].rx_name);
    g_string_append_printf(s, ",\"stations\":%u,\"active_js8\":%u,\"unconfirmed\":%u,\"sub\":{",
                           g_hash_table_size(calls), g_hash_table_size(now), deep);
    for (guint i = 0; i < SKIM_JS8_NSUB; i++) {
      g_string_append_printf(s, "%s\"%s\":%u", i ? "," : "", SUB[i].name, g_hash_table_size(per[i]));
      g_hash_table_unref(per[i]);
    }
    g_string_append(s, "},\"last\":");
    jstr(s, last);
    g_string_append_c(s, '}');
    g_hash_table_unref(calls);
    g_hash_table_unref(now);
  }
  g_string_append_printf(s, "],\"recent_s\":%d,\"cycles\":%" G_GUINT64_FORMAT ",\"decodes\":%"
                         G_GUINT64_FORMAT ",\"deep\":%" G_GUINT64_FORMAT ",\"confirmed\":%"
                         G_GUINT64_FORMAT ",\"known\":%u,\"last_cycle\":%" G_GINT64_FORMAT
                         ",\"last_decodes\":%u,\"skipped\":%" G_GUINT64_FORMAT ",\"spots\":%"
                         G_GUINT64_FORMAT ",\"error\":",
                         SKIM_JS8_RECENT_S, j->cycles, j->ndecodes, j->ndeep, j->nconfirmed,
                         g_hash_table_size(j->known), j->last_cycle, j->last_decodes, j->skipped,
                         j->spots);
  if (j->error) {
    jstr(s, j->error);
  } else {
    g_string_append(s, "null");
  }
  g_mutex_unlock(&j->lock);
  g_string_append_c(s, '}');
  return g_string_free(s, FALSE);
}

char *skim_hd_js8_frames_json(SkimHdJs8 *j, double dial_hz, gint64 since_ms) {
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "{\"now\":%" G_GINT64_FORMAT ",\"frames\":[", g_get_real_time() / 1000);
  g_mutex_lock(&j->lock);
  guint n = 0;
  for (guint i = 0; i < j->frames->len; i++) {
    const SkimJs8Decode *d = &g_array_index(j->frames, SkimJs8Decode, i);
    const Slot *x = &j->slot[d->slot];
    if (d->t_ms < since_ms || (dial_hz > 0 && x->dial_hz != dial_hz)) { continue; }
    char dt[16], df[16], q[16];
    g_ascii_formatd(dt, sizeof dt, "%.2f", d->dt);
    g_ascii_formatd(df, sizeof df, "%.1f", d->df);
    g_ascii_formatd(q, sizeof q, "%.3f", d->quality);
    g_string_append_printf(s, "%s{\"t\":%" G_GINT64_FORMAT ",\"dial\":%.0f,\"band\":", n++ ? "," : "",
                           d->t_ms, x->dial_hz);
    jstr(s, x->name);
    g_string_append_printf(s, ",\"sub\":%d,\"snr\":%d,\"dt\":%s,\"df\":%s,\"type\":%d,\"q\":%s,\"raw\":",
                           SUB[d->sub].number, d->snr, dt, df, d->type, q);
    jstr(s, d->raw);
    g_string_append(s, ",\"call\":");
    jstr(s, d->call);
    g_string_append_printf(s, ",\"deep\":%s,\"ok\":%s,\"text\":", d->deep ? "true" : "false",
                           d->confirmed ? "true" : "false");
    jstr(s, d->f.text);
    g_string_append_c(s, '}');
  }
  g_mutex_unlock(&j->lock);
  g_string_append(s, "]}");
  return g_string_free(s, FALSE);
}
