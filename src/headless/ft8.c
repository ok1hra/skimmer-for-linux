/* ft8.c — FT8 and FT4 beside the skimmer (ft8.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "ft8.h"
#include "digi_rx.h"
#include "helper_run.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "callsign.h"
#include "json.h"

static void jstr(GString *s, const char *v) {   /* a quoted JSON string */
  g_string_append_c(s, '"');
  skim_json_append(s, v);
  g_string_append_c(s, '"');
}

const char *skim_ft8_mode_name(SkimFt8Mode m) { return m == SKIM_FT8_MODE_FT4 ? "FT4" : "FT8"; }
guint skim_ft8_period_ms(SkimFt8Mode m) { return m == SKIM_FT8_MODE_FT4 ? 7500 : 15000; }

/* WSJT-X's default FT8 and FT4 dial frequencies (its frequency list, IARU
 * region 1 where the regions differ). */
const SkimFt8Dial SKIM_FT8_DIALS[] = {
  { "160m", SKIM_FT8_MODE_FT8, 1840000 },  { "80m", SKIM_FT8_MODE_FT8, 3573000 },
  { "80m", SKIM_FT8_MODE_FT4, 3575000 },   { "60m", SKIM_FT8_MODE_FT8, 5357000 },
  { "40m", SKIM_FT8_MODE_FT8, 7074000 },   { "40m", SKIM_FT8_MODE_FT4, 7047500 },
  { "30m", SKIM_FT8_MODE_FT8, 10136000 },  { "30m", SKIM_FT8_MODE_FT4, 10140000 },
  { "20m", SKIM_FT8_MODE_FT8, 14074000 },  { "20m", SKIM_FT8_MODE_FT4, 14080000 },
  { "17m", SKIM_FT8_MODE_FT8, 18100000 },  { "17m", SKIM_FT8_MODE_FT4, 18104000 },
  { "15m", SKIM_FT8_MODE_FT8, 21074000 },  { "15m", SKIM_FT8_MODE_FT4, 21140000 },
  { "12m", SKIM_FT8_MODE_FT8, 24915000 },  { "12m", SKIM_FT8_MODE_FT4, 24919000 },
  { "10m", SKIM_FT8_MODE_FT8, 28074000 },  { "10m", SKIM_FT8_MODE_FT4, 28180000 },
  { "6m", SKIM_FT8_MODE_FT8, 50313000 },   { "6m", SKIM_FT8_MODE_FT4, 50318000 },
  { NULL, 0, 0 },
};

gboolean skim_ft8_covers(double centre, double rate, double dial_hz, double lo_hz, double hi_hz) {
  const double edge = 0.45 * rate;
  return dial_hz + lo_hz >= centre - edge && dial_hz + hi_hz <= centre + edge;
}

/* ---- the DSP: digi_rx.c, one period ------------------------------------------------ */

struct _SkimFt8Rx {
  SkimDigiRx    *rx;
  SkimFt8CycleCb cb;
  gpointer       user;
};

static void rx_cycle(guint period, gint64 start_ms, gint16 *pcm, guint n, gpointer user) {
  (void)period;
  SkimFt8Rx *rx = user;
  rx->cb(start_ms, pcm, n, rx->user);
}

SkimFt8Rx *skim_ft8_rx_new(double rate, double centre, double dial_hz, SkimFt8Mode mode,
                           double lo_hz, double hi_hz, SkimFt8CycleCb cb, gpointer user) {
  SkimFt8Rx *rx = g_new0(SkimFt8Rx, 1);
  const guint period = skim_ft8_period_ms(mode);
  rx->cb = cb;
  rx->user = user;
  rx->rx = skim_digi_rx_new(rate, centre, dial_hz, lo_hz, hi_hz, &period, 1, rx_cycle, rx);
  return rx;
}

void skim_ft8_rx_free(SkimFt8Rx *rx) {
  if (!rx) { return; }
  skim_digi_rx_free(rx->rx);
  g_free(rx);
}

void skim_ft8_rx_feed(SkimFt8Rx *rx, const float *iq, guint n, gint64 t_end_us) {
  skim_digi_rx_feed(rx->rx, iq, n, t_end_us);
}

char *skim_ft8_write_wav(const char *dir, gint64 start_ms, const gint16 *pcm, guint n,
                         GError **error) {
  return skim_digi_write_wav(dir, start_ms, pcm, n, error);
}

/* ---- a decode -------------------------------------------------------------------- */

/* A CQ's modifier (DX, POTA, NA, TEST, a 3-digit kHz …), not the caller. */
static gboolean cq_modifier(const char *t) {
  const gsize n = strlen(t);
  gboolean alpha = n >= 1 && n <= 4, digit = n == 3;
  for (const char *c = t; *c; c++) {
    alpha &= g_ascii_isupper(*c);
    digit &= g_ascii_isdigit(*c);
  }
  return alpha || digit;
}

static gboolean plain_call(const char *t, char *call, gsize size) {
  if (!t || !*t || strchr(t, '<') || strchr(t, '>') || strlen(t) >= size) { return FALSE; }
  if (!skim_callsign_is_valid(t)) { return FALSE; }
  g_strlcpy(call, t, size);
  return TRUE;
}

gboolean skim_ft8_sender(const char *msg, char *call, gsize call_size, gboolean *cq) {
  gboolean is_cq = FALSE, ok = FALSE;
  char **tok = g_strsplit_set(msg ? msg : "", " ", -1);
  GPtrArray *w = g_ptr_array_new();
  for (char **t = tok; *t; t++) {
    if (**t) { g_ptr_array_add(w, *t); }
  }
  guint i = 0;
  if (w->len && strcmp(w->pdata[0], "TU;") == 0) { i = 1; }   /* RTTY Roundup: "TU; C1 C2 …" */
  if (i < w->len && (strcmp(w->pdata[i], "CQ") == 0 || strcmp(w->pdata[i], "QRZ") == 0)) {
    is_cq = TRUE;
    for (guint k = i + 1; k < w->len && !ok; k++) {
      const char *t = w->pdata[k];
      if (strchr(t, '<')) { break; }                          /* a hashed caller   */
      if (plain_call(t, call, call_size)) { ok = TRUE; break; }
      if (!cq_modifier(t)) { break; }
    }
  } else if (i < w->len && strcmp(w->pdata[i], "DE") == 0) {
    ok = i + 1 < w->len && plain_call(w->pdata[i + 1], call, call_size);
  } else if (i + 1 < w->len) {
    /* "CALL1 CALL2 …": CALL1 a call or a hash (the one called), CALL2 sends */
    const char *c1 = w->pdata[i];
    char tmp[32];
    const gboolean c1_ok = c1[0] == '<' || plain_call(c1, tmp, sizeof tmp);
    ok = c1_ok && plain_call(w->pdata[i + 1], call, call_size);
  }
  g_ptr_array_free(w, TRUE);
  g_strfreev(tok);
  if (!ok && call_size) { call[0] = '\0'; }
  if (cq) { *cq = ok && is_cq; }
  return ok;
}

static gboolean grid4(const char *g) {
  return strlen(g) == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' &&
         g_ascii_isdigit(g[2]) && g_ascii_isdigit(g[3]) && strcmp(g, "RR73") != 0;
}

void skim_ft8_sender_grid(const char *msg, char grid[8]) {
  grid[0] = '\0';
  char call[16];
  gboolean cq = FALSE;
  if (!skim_ft8_sender(msg, call, sizeof call, &cq)) { return; }
  char **tok = g_strsplit(msg, " ", -1);
  for (guint i = 0; tok[i]; i++) {               /* the word right after the sender */
    if (strcmp(tok[i], call) == 0 && tok[i + 1] && grid4(tok[i + 1])) {
      g_strlcpy(grid, tok[i + 1], 8);
      break;
    }
  }
  g_strfreev(tok);
}

gboolean skim_ft8_parse_line(const char *line, SkimFt8Decode *out) {
  char hhmmss[8], m = 0;
  int snr, freq, used = 0;
  double dt;
  if (!line || sscanf(line, "%7s %d %lf %d %c%n", hhmmss, &snr, &dt, &freq, &m, &used) != 5 ||
      strlen(hhmmss) != 6 || (m != '~' && m != '+') || freq < 0 || freq > 6000) {
    return FALSE;
  }
  for (const char *c = hhmmss; *c; c++) {
    if (!g_ascii_isdigit(*c)) { return FALSE; }
  }
  /* then two spaces, the 37-character message (its last character '?' for a
   * doubtful AP decode), a space and the AP mark ("a1"…, or blanks) */
  const char *rest = line + used;
  while (*rest == ' ') { rest++; }
  char msg[40] = "";
  const gsize rl = strlen(rest);
  memcpy(msg, rest, MIN(rl, 37));
  msg[MIN(rl, 37)] = '\0';
  const gboolean doubt = rl >= 37 && rest[36] == '?';
  if (doubt) { msg[36] = ' '; }
  g_strstrip(msg);
  char annot[8] = "";
  if (rl > 37) {
    g_strlcpy(annot, rest + 37, sizeof annot);
    g_strstrip(annot);
  }
  if (!msg[0]) { return FALSE; }
  memset(out, 0, sizeof *out);
  out->snr = snr;
  out->dt = dt;
  out->df = freq;
  out->ap = annot[0] == 'a';
  out->doubt = doubt;
  g_strlcpy(out->msg, msg, sizeof out->msg);
  skim_ft8_sender(out->msg, out->call, sizeof out->call, &out->cq);
  skim_ft8_sender_grid(out->msg, out->grid);
  return TRUE;
}

/* ---- the decoder program ---------------------------------------------------------- */

char *skim_ft8_find_decoder(void) {
  char *self = g_file_read_link("/proc/self/exe", NULL);
  if (self) {
    char *dir = g_path_get_dirname(self);
    const char *const tries[] = { "skimmer-ft8d", "vendor/wsjtx-ft/skimmer-ft8d",
                                  "../vendor/wsjtx-ft/skimmer-ft8d", NULL };
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
  return g_find_program_in_path("skimmer-ft8d");
}

gboolean skim_ft8_decode_file(const char *decoder, const char *dir, const char *wav,
                              SkimFt8Mode mode, guint depth, double lo_hz, double hi_hz,
                              gint64 t_ms, guint slot, GArray *decodes, GCancellable *cancel,
                              GError **error) {
  char d[8], lo[16], hi[16];
  g_snprintf(d, sizeof d, "%u", CLAMP(depth, 1u, 3u));
  g_snprintf(lo, sizeof lo, "%.0f", lo_hz);
  g_snprintf(hi, sizeof hi, "%.0f", hi_hz);
  char *prog = g_canonicalize_filename(decoder, NULL);   /* it runs in dir */
  const char *argv[] = { prog, mode == SKIM_FT8_MODE_FT4 ? "-4" : "-8", "-d", d, "-L", lo,
                         "-H", hi, wav, NULL };
  char *out = NULL;
  const gboolean ok = skim_hd_helper_run(argv, dir, decoder, cancel, &out, error);
  g_free(prog);
  if (ok) {
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (char **ln = lines; *ln; ln++) {
      SkimFt8Decode x;
      if (!skim_ft8_parse_line(*ln, &x)) { continue; }
      x.t_ms = t_ms;
      x.slot = slot;
      g_array_append_val(decodes, x);
    }
    g_strfreev(lines);
  }
  g_free(out);
  return ok;
}

/* ---- the whole ------------------------------------------------------------------- */

typedef struct _Job Job;

typedef struct {
  SkimHdFt8  *f;
  SkimFt8Rx  *rx;
  guint       band;                     /* the receiver                          */
  const char *name;                     /* SKIM_FT8_DIALS' band                  */
  char        rx_name[16];              /* the receiver's band name (its row)    */
  SkimFt8Mode mode;
  double      dial_hz;
  char       *work;                     /* its own work dir (threads run side by side) */
  char       *keep;                     /* … or where its WAVs stay (keep_wav)   */
  Job        *waiting;                  /* its cycle not yet started (f->lock)   */
} Slot;

struct _Job {
  gint64  start_ms;
  guint   slot;
  gint16 *pcm;
  guint   n;
  gboolean skip;                        /* a newer cycle of the slot came first  */
};

struct _SkimHdFt8 {
  char    *dir, *work, *decoder;
  gboolean ft8, ft4;
  guint    depth;
  double   lo_hz, hi_hz;
  SkimHdFt8SpotCb spot;
  gpointer user;
  Slot     slot[SKIM_FT8_MAX_SLOTS];
  guint    nslots;
  GPtrArray *by_band[16];               /* receiver → its slots (Slot *)         */
  GThreadPool *pool;
  GCancellable *cancel;
  GMutex   lock;                        /* below                                  */
  GCond    idle;                        /* pending reached 0                     */
  guint    pending;                     /* jobs handed over, not finished        */
  GArray  *decodes;                     /* SkimFt8Decode, the last 15 min, oldest first */
  GHashTable *fed;                      /* "CALL|band|mode" → gint64 *: last spot (ms) */
  gint64   newest_ms;                   /* the newest cycle decoded              */
  /* the minute's history, per receiver (skim_hd_ft8_minute) */
  struct {
    guint spots, appeared;
    GHashTable *heard;                  /* the minute's senders (owned keys)     */
    GArray *snr;                        /* float, every decode                   */
  } min[16];
  GHashTable *seen;                     /* "CALL|rx" → gint64 *: last decoded (ms) */
  GHashTable *grids;                    /* call → its locator, from any decode   */
  guint64  cycles, ndecodes, skipped, spots;
  gint64   last_cycle;
  guint    last_decodes;
  char    *error;
};

static void set_error(SkimHdFt8 *f, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_error(SkimHdFt8 *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *m = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_mutex_lock(&f->lock);
  const gboolean same = g_strcmp0(f->error, m) == 0;
  g_free(f->error);
  f->error = m;
  g_mutex_unlock(&f->lock);
  if (!same) { g_warning("ft8: %s", m); }
}

static void job_done(SkimHdFt8 *f) {          /* lock held */
  if (f->pending && --f->pending == 0) { g_cond_broadcast(&f->idle); }
}

static void cycle_cb(gint64 start_ms, gint16 *pcm, guint n, gpointer user) {
  Slot *s = user;
  SkimHdFt8 *f = s->f;
  Job *j = g_new0(Job, 1);
  j->start_ms = start_ms;
  j->slot = (guint)(s - f->slot);
  j->pcm = pcm;
  j->n = n;
  g_mutex_lock(&f->lock);
  if (s->waiting) {                             /* still queued: the machine is behind */
    s->waiting->skip = TRUE;
    f->skipped++;
  }
  s->waiting = j;
  f->pending++;
  g_mutex_unlock(&f->lock);
  g_thread_pool_push(f->pool, j, NULL);
}

static void expire(SkimHdFt8 *f) {             /* lock held */
  const gint64 keep = f->newest_ms - (gint64)SKIM_FT8_KEEP_S * 1000;
  guint k = 0;
  while (k < f->decodes->len && g_array_index(f->decodes, SkimFt8Decode, k).t_ms < keep) { k++; }
  if (k) { g_array_remove_range(f->decodes, 0, k); }
  for (int pass = 0; pass < 2; pass++) {       /* the fold's and the appearances' old entries */
    GHashTable *ht = pass ? f->seen : f->fed;
    if (g_hash_table_size(ht) <= 4096) { continue; }
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, ht);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
      if (*(gint64 *)v < f->newest_ms - (gint64)SKIM_FT8_FOLD_S * 1000) { g_hash_table_iter_remove(&it); }
    }
  }
}

typedef struct {
  char   call[16], grid[8];
  double freq_hz;
  int    snr;
  gboolean cq;
  gint64 t;
} Out;

static void decode_job(gpointer data, gpointer user) {
  Job *j = data;
  SkimHdFt8 *f = user;
  g_mutex_lock(&f->lock);
  const gboolean skip = j->skip || g_cancellable_is_cancelled(f->cancel);
  Slot *s = &f->slot[j->slot];
  if (s->waiting == j) { s->waiting = NULL; }
  if (skip) {
    job_done(f);
    g_mutex_unlock(&f->lock);
    g_free(j->pcm);
    g_free(j);
    return;
  }
  g_mutex_unlock(&f->lock);

  GError *e = NULL;
  GArray *got = g_array_new(FALSE, TRUE, sizeof(SkimFt8Decode));
  gboolean ok = FALSE;
  char *wav = NULL;
  if (!f->decoder) {
    set_error(f, "no decoder (skimmer-ft8d) found — nothing is decoded");
  } else if (!(wav = skim_ft8_write_wav(s->keep ? s->keep : s->work, j->start_ms, j->pcm, j->n,
                                        &e))) {
    set_error(f, "%s", e->message);
    g_clear_error(&e);
  } else {
    ok = skim_ft8_decode_file(f->decoder, s->work, wav, s->mode, f->depth, f->lo_hz, f->hi_hz,
                              j->start_ms, j->slot, got, f->cancel, &e);
    if (!s->keep) { g_unlink(wav); }
    if (!ok) {
      if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        set_error(f, "%s %s: %s", s->name, skim_ft8_mode_name(s->mode), e->message);
      }
      g_clear_error(&e);
    }
  }
  g_free(wav);

  GArray *out = g_array_new(FALSE, TRUE, sizeof(Out));
  g_mutex_lock(&f->lock);
  if (ok) {
    if (j->start_ms > f->newest_ms) { f->newest_ms = j->start_ms; }
    for (guint i = 0; i < got->len; i++) {
      const SkimFt8Decode *d = &g_array_index(got, SkimFt8Decode, i);
      g_array_append_val(f->decodes, *d);
      /* the minute's history: every decode's SNR, each sender heard, and
       * whether it appeared (none from this receiver in 10 min) */
      const float snr = (float)d->snr;
      g_array_append_val(f->min[s->band].snr, snr);
      if (d->call[0] && d->grid[0]) {           /* its locator, for its later spots */
        if (g_hash_table_size(f->grids) > 50000) { g_hash_table_remove_all(f->grids); }
        g_hash_table_replace(f->grids, g_strdup(d->call), g_strdup(d->grid));
      }
      if (d->call[0]) {
        g_hash_table_add(f->min[s->band].heard, g_strdup(d->call));
        char *sk = g_strdup_printf("%s|%u", d->call, s->band);
        gint64 *at = g_hash_table_lookup(f->seen, sk);
        if (!at || d->t_ms - *at >= (gint64)SKIM_FT8_FOLD_S * 1000) { f->min[s->band].appeared++; }
        if (!at) {
          at = g_new(gint64, 1);
          g_hash_table_insert(f->seen, sk, at);
        } else {
          g_free(sk);
        }
        *at = MAX(*at, d->t_ms);
      }
      /* a spot: a valid sender, no AP decode, once a band and mode in 10 min */
      if (!d->call[0] || d->ap || d->doubt || !f->spot) { continue; }
      char *key = g_strdup_printf("%s|%s|%s", d->call, s->name, skim_ft8_mode_name(s->mode));
      gint64 *last = g_hash_table_lookup(f->fed, key);
      if (last && d->t_ms - *last < (gint64)SKIM_FT8_FOLD_S * 1000 && d->t_ms >= *last) {
        g_free(key);
        continue;
      }
      if (!last) {
        last = g_new(gint64, 1);
        g_hash_table_insert(f->fed, key, last);
      } else {
        g_free(key);
      }
      *last = d->t_ms;
      f->min[s->band].spots++;
      Out o = { .freq_hz = s->dial_hz + d->df, .snr = d->snr, .cq = d->cq, .t = d->t_ms / 1000 };
      g_strlcpy(o.call, d->call, sizeof o.call);
      const char *g = d->grid[0] ? d->grid : g_hash_table_lookup(f->grids, d->call);
      g_strlcpy(o.grid, g ? g : "", sizeof o.grid);
      g_array_append_val(out, o);
    }
    expire(f);
    f->cycles++;
    f->ndecodes += got->len;
    if (j->start_ms > f->last_cycle) {           /* the newest cycle: its dials' sum */
      f->last_cycle = j->start_ms;
      f->last_decodes = got->len;
    } else if (j->start_ms == f->last_cycle) {
      f->last_decodes += got->len;
    }
    f->spots += out->len;
    if (f->error && f->decoder) { g_clear_pointer(&f->error, g_free); }
  }
  g_mutex_unlock(&f->lock);
  for (guint i = 0; i < out->len; i++) {        /* outside the lock: the feed's own */
    const Out *o = &g_array_index(out, Out, i);
    f->spot(o->call, skim_ft8_mode_name(s->mode), o->freq_hz, o->snr, o->cq, s->name, o->grid, o->t,
            f->user);
  }
  g_array_unref(out);
  g_array_unref(got);
  g_mutex_lock(&f->lock);
  job_done(f);
  g_mutex_unlock(&f->lock);
  g_free(j->pcm);
  g_free(j);
}

/* ---- life ------------------------------------------------------------------------ */

SkimHdFt8 *skim_hd_ft8_new(const SkimHdFt8Config *cfg, guint nb, const char *const *names,
                           const double *centres, double rate) {
  SkimHdFt8 *f = g_new0(SkimHdFt8, 1);
  g_mutex_init(&f->lock);
  g_cond_init(&f->idle);
  f->ft8 = cfg->ft8;
  f->ft4 = cfg->ft4;
  f->depth = CLAMP(cfg->depth, 1u, 3u);
  f->lo_hz = cfg->lo_hz;
  f->hi_hz = cfg->hi_hz;
  f->spot = cfg->spot;
  f->user = cfg->user;
  f->dir = g_strdup(cfg->dir);
  f->work = g_build_filename(cfg->dir, "ft8", NULL);
  f->decoder = cfg->decoder ? g_strdup(cfg->decoder) : skim_ft8_find_decoder();
  f->decodes = g_array_new(FALSE, TRUE, sizeof(SkimFt8Decode));
  f->fed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  f->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  f->grids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  for (guint b = 0; b < G_N_ELEMENTS(f->min); b++) {
    f->min[b].heard = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    f->min[b].snr = g_array_new(FALSE, FALSE, sizeof(float));
  }
  f->cancel = g_cancellable_new();
  for (guint b = 0; b < nb && b < G_N_ELEMENTS(f->by_band); b++) {
    f->by_band[b] = g_ptr_array_new();
    gboolean want = !cfg->bands;                       /* [ft8] bands: this receiver? */
    for (const char *const *x = cfg->bands; x && *x && !want; x++) {
      want = names && names[b] && strcmp(names[b], *x) == 0;
    }
    for (const SkimFt8Dial *d = SKIM_FT8_DIALS; want && d->band; d++) {
      if ((d->mode == SKIM_FT8_MODE_FT8 && !f->ft8) || (d->mode == SKIM_FT8_MODE_FT4 && !f->ft4) ||
          !skim_ft8_covers(centres[b], rate, d->dial_hz, f->lo_hz, f->hi_hz) ||
          f->nslots >= SKIM_FT8_MAX_SLOTS) {
        continue;
      }
      gboolean dup = FALSE;                            /* another receiver has it */
      for (guint k = 0; k < f->nslots; k++) { dup |= f->slot[k].dial_hz == d->dial_hz; }
      if (dup) { continue; }
      Slot *s = &f->slot[f->nslots];
      s->f = f;
      s->band = b;
      s->name = d->band;
      s->mode = d->mode;
      g_strlcpy(s->rx_name, names && names[b] ? names[b] : d->band, sizeof s->rx_name);
      s->dial_hz = d->dial_hz;
      char sub[16];
      g_snprintf(sub, sizeof sub, "%u", f->nslots);
      s->work = g_build_filename(f->work, sub, NULL);
      if (g_mkdir_with_parents(s->work, 0755) != 0) {
        set_error(f, "%s: %s", s->work, g_strerror(errno));
      }
      if (cfg->keep_wav) {
        char *kd = g_strdup_printf("%s-%s", d->band, skim_ft8_mode_name(d->mode));
        s->keep = g_build_filename(cfg->keep_wav, kd, NULL);
        g_free(kd);
        g_mkdir_with_parents(s->keep, 0755);
      }
      s->rx = skim_ft8_rx_new(rate, centres[b], d->dial_hz, d->mode, f->lo_hz, f->hi_hz,
                              cycle_cb, s);
      g_ptr_array_add(f->by_band[b], s);
      f->nslots++;
    }
  }
  if (!f->decoder) { set_error(f, "no decoder (skimmer-ft8d) found — nothing is decoded"); }
  f->pool = g_thread_pool_new(decode_job, f, (gint)MAX(cfg->threads, 1u), FALSE, NULL);
  return f;
}

static int float_cmp(gconstpointer a, gconstpointer b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

gboolean skim_hd_ft8_minute(SkimHdFt8 *f, guint b, SkimHdFt8Minute *out) {
  memset(out, 0, sizeof *out);
  out->snr = NAN;
  if (!f || b >= G_N_ELEMENTS(f->by_band) || !f->by_band[b] || !f->by_band[b]->len) { return FALSE; }
  g_mutex_lock(&f->lock);
  out->spots = f->min[b].spots;
  out->appeared = f->min[b].appeared;
  out->heard = g_hash_table_size(f->min[b].heard);
  GArray *a = f->min[b].snr;
  if (a->len) {
    g_array_sort(a, float_cmp);
    out->snr = g_array_index(a, float, a->len / 2);
  }
  f->min[b].spots = f->min[b].appeared = 0;
  g_hash_table_remove_all(f->min[b].heard);
  g_array_set_size(a, 0);
  g_mutex_unlock(&f->lock);
  return TRUE;
}

void skim_hd_ft8_drain(SkimHdFt8 *f) {
  g_mutex_lock(&f->lock);
  while (f->pending) { g_cond_wait(&f->idle, &f->lock); }
  g_mutex_unlock(&f->lock);
}

void skim_hd_ft8_free(SkimHdFt8 *f) {
  if (!f) { return; }
  g_cancellable_cancel(f->cancel);
  g_thread_pool_free(f->pool, FALSE, TRUE);   /* queued jobs run and see the cancel */
  for (guint k = 0; k < f->nslots; k++) {
    skim_ft8_rx_free(f->slot[k].rx);
    g_free(f->slot[k].work);
    g_free(f->slot[k].keep);
  }
  for (guint b = 0; b < G_N_ELEMENTS(f->by_band); b++) {
    if (f->by_band[b]) { g_ptr_array_free(f->by_band[b], TRUE); }
  }
  g_object_unref(f->cancel);
  g_array_unref(f->decodes);
  g_hash_table_unref(f->fed);
  g_hash_table_unref(f->seen);
  g_hash_table_unref(f->grids);
  for (guint b = 0; b < G_N_ELEMENTS(f->min); b++) {
    g_hash_table_unref(f->min[b].heard);
    g_array_unref(f->min[b].snr);
  }
  g_free(f->dir); g_free(f->work); g_free(f->decoder); g_free(f->error);
  g_cond_clear(&f->idle);
  g_mutex_clear(&f->lock);
  g_free(f);
}

void skim_hd_ft8_push(SkimHdFt8 *f, guint b, const float *iq, guint n) {
  if (!f || b >= G_N_ELEMENTS(f->by_band) || !f->by_band[b] || !f->by_band[b]->len) { return; }
  skim_hd_ft8_push_at(f, b, iq, n, g_get_real_time());
}

void skim_hd_ft8_push_at(SkimHdFt8 *f, guint b, const float *iq, guint n, gint64 now) {
  if (!f || b >= G_N_ELEMENTS(f->by_band) || !f->by_band[b]) { return; }
  for (guint k = 0; k < f->by_band[b]->len; k++) {
    skim_ft8_rx_feed(((Slot *)f->by_band[b]->pdata[k])->rx, iq, n, now);
  }
}

char *skim_hd_ft8_dials_text(SkimHdFt8 *f) {
  GString *s = g_string_new(NULL);
  for (guint k = 0; k < f->nslots; k++) {
    g_string_append_printf(s, "%s%s %s %.1f", k ? ", " : "", f->slot[k].name,
                           skim_ft8_mode_name(f->slot[k].mode), f->slot[k].dial_hz / 1e3);
  }
  return g_string_free(s, FALSE);
}

char *skim_hd_ft8_status_json(SkimHdFt8 *f) {
  GString *s = g_string_new("{\"on\":true,\"decoder\":");
  g_string_append(s, f->decoder ? "true" : "false");
  g_string_append_printf(s, ",\"ft8\":%s,\"ft4\":%s,\"depth\":%u,\"lo_hz\":%.0f,\"hi_hz\":%.0f",
                         f->ft8 ? "true" : "false", f->ft4 ? "true" : "false", f->depth,
                         f->lo_hz, f->hi_hz);
  g_string_append(s, ",\"dials\":[");
  for (guint k = 0; k < f->nslots; k++) {
    const Slot *x = &f->slot[k];
    g_string_append_printf(s, "%s{\"band\":", k ? "," : "");
    jstr(s, x->name);
    g_string_append(s, ",\"mode\":");
    jstr(s, skim_ft8_mode_name(x->mode));
    g_string_append_printf(s, ",\"dial_hz\":%.0f,\"rx\":%u,\"lo_hz\":%.0f,\"hi_hz\":%.0f}",
                           x->dial_hz, x->band, x->dial_hz + f->lo_hz, x->dial_hz + f->hi_hz);
  }
  g_mutex_lock(&f->lock);
  /* per receiver (a row of the page's band table): the senders decoded on
   * its dials in the last 30 min, and the newest one */
  g_string_append(s, "],\"rx\":[");
  const gint64 since = g_get_real_time() / 1000 - (gint64)SKIM_FT8_RECENT_S * 1000;
  /* a cycle's decodes come after it ends: the last minute of cycles is the
   * four (FT8) before the newest decoded one, give or take one */
  const gint64 active_since = g_get_real_time() / 1000 - 75 * 1000;
  guint nrx = 0;
  for (guint k = 0; k < f->nslots; k++) {
    gboolean first = TRUE;                            /* the receiver's first dial */
    for (guint j = 0; j < k; j++) { first &= f->slot[j].band != f->slot[k].band; }
    if (!first) { continue; }
    GHashTable *calls = g_hash_table_new(g_str_hash, g_str_equal);
    /* … and as "active" the senders of the last minute, FT8 and FT4 apart
     * (the Statistics' measure) */
    GHashTable *now_ft8 = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *now_ft4 = g_hash_table_new(g_str_hash, g_str_equal);
    const char *last = "";
    for (guint i = 0; i < f->decodes->len; i++) {
      const SkimFt8Decode *d = &g_array_index(f->decodes, SkimFt8Decode, i);
      if (d->t_ms < since || !d->call[0] || f->slot[d->slot].band != f->slot[k].band) { continue; }
      g_hash_table_add(calls, (gpointer)d->call);
      if (d->t_ms >= active_since) {
        g_hash_table_add(f->slot[d->slot].mode == SKIM_FT8_MODE_FT4 ? now_ft4 : now_ft8, (gpointer)d->call);
      }
      last = d->call;
    }
    g_string_append_printf(s, "%s{\"band\":", nrx++ ? "," : "");
    jstr(s, f->slot[k].rx_name);
    g_string_append_printf(s, ",\"stations\":%u,\"active_ft8\":%u,\"active_ft4\":%u,\"last\":",
                           g_hash_table_size(calls), g_hash_table_size(now_ft8), g_hash_table_size(now_ft4));
    jstr(s, last);
    g_string_append_c(s, '}');
    g_hash_table_unref(calls);
    g_hash_table_unref(now_ft8);
    g_hash_table_unref(now_ft4);
  }
  g_string_append_printf(s, "],\"recent_s\":%d,\"cycles\":%" G_GUINT64_FORMAT ",\"decodes\":%"
                         G_GUINT64_FORMAT ",\"last_cycle\":%" G_GINT64_FORMAT ",\"last_decodes\":%u"
                         ",\"skipped\":%" G_GUINT64_FORMAT ",\"spots\":%" G_GUINT64_FORMAT
                         ",\"error\":",
                         SKIM_FT8_RECENT_S, f->cycles, f->ndecodes, f->last_cycle,
                         f->last_decodes, f->skipped, f->spots);
  if (f->error) {
    jstr(s, f->error);
  } else {
    g_string_append(s, "null");
  }
  g_mutex_unlock(&f->lock);
  g_string_append_c(s, '}');
  return g_string_free(s, FALSE);
}

char *skim_hd_ft8_decodes_json(SkimHdFt8 *f, double dial_hz, gint64 since_ms) {
  GString *s = g_string_new(NULL);
  g_string_append_printf(s, "{\"now\":%" G_GINT64_FORMAT ",\"decodes\":[", g_get_real_time() / 1000);
  g_mutex_lock(&f->lock);
  guint n = 0;
  for (guint i = 0; i < f->decodes->len; i++) {
    const SkimFt8Decode *d = &g_array_index(f->decodes, SkimFt8Decode, i);
    const Slot *x = &f->slot[d->slot];
    if (d->t_ms < since_ms || (dial_hz > 0 && x->dial_hz != dial_hz)) { continue; }
    char dt[16];
    g_ascii_formatd(dt, sizeof dt, "%.1f", d->dt);
    g_string_append_printf(s, "%s{\"t\":%" G_GINT64_FORMAT ",\"dial\":%.0f,\"mode\":\"%s\",\"band\":",
                           n++ ? "," : "", d->t_ms, x->dial_hz, skim_ft8_mode_name(x->mode));
    jstr(s, x->name);
    g_string_append_printf(s, ",\"snr\":%d,\"dt\":%s,\"df\":%d,\"msg\":", d->snr, dt, d->df);
    jstr(s, d->msg);
    g_string_append(s, ",\"call\":");
    jstr(s, d->call);
    g_string_append_printf(s, ",\"cq\":%s,\"ap\":%s}", d->cq ? "true" : "false",
                           d->ap ? "true" : "false");
  }
  g_mutex_unlock(&f->lock);
  g_string_append(s, "]}");
  return g_string_free(s, FALSE);
}
