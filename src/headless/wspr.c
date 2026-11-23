/* wspr.c — WSPR beside the skimmer (wspr.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "wspr.h"
#include "helper_run.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "json.h"

static void jstr(GString *s, const char *v) {   /* a quoted JSON string */
  g_string_append_c(s, '"');
  skim_json_append(s, v);
  g_string_append_c(s, '"');
}

/* WDSP (vendor/wdsp): the resampler, as waterfall.c uses it */
extern void *create_resample(int run, int size, double *in, double *out, int in_rate,
                             int out_rate, double fc, int ncoef, double gain);
extern void  destroy_resample(void *a);
extern int   xresample(void *a);

/* WSJT-X's default WSPR-2 dial frequencies (its frequency list). */
const SkimWsprDial SKIM_WSPR_DIALS[] = {
  { "2200m", 136000 },  { "630m", 474200 },    { "160m", 1836600 },  { "80m", 3568600 },
  { "60m", 5287200 },   { "60m", 5364700 },    { "40m", 7038600 },   { "30m", 10138700 },
  { "20m", 14095600 },  { "17m", 18104600 },   { "15m", 21094600 },  { "12m", 24924600 },
  { "10m", 28124600 },  { "6m", 50293000 },    { "4m", 70091000 },   { "2m", 144489000 },
  { NULL, 0 },
};

gboolean skim_wspr_covers(double centre, double rate, double dial_hz) {
  const double mid = dial_hz + SKIM_WSPR_BFO, edge = 0.45 * rate;
  return mid - SKIM_WSPR_HALF_SPAN >= centre - edge && mid + SKIM_WSPR_HALF_SPAN <= centre + edge;
}

/* ---- the DSP --------------------------------------------------------------------- */

#define MID_RATE   12000                /* the first stage's output              */
#define TAPS1      240                  /* rate → 12 k: only ±12 k multiples fold */
#define TAPS2      1400                 /* 12 k → 375: flat to 150, gone by 225 */
#define FC2        190.0

struct _SkimWsprRx {
  double  rate, offset;                 /* dial + 1500 − centre (Hz)             */
  void   *r1, *r2;
  double *b1, *m1, *o2;
  guint   blk_n, fill, mid_n;
  double  ph_re, ph_im, st_re, st_im;   /* the shift's phasor and its step       */
  double  delay_us;                     /* the two filters' group delay          */
  /* the clock: frame 0 since the anchor was at anchor_us */
  gboolean anchored;
  double  anchor_us;
  guint64 frames;
  /* the capture */
  gboolean capturing;
  gint64  cap_cycle, done_cycle;        /* in 120 s units                        */
  float  *cap;
  guint   cap_n;
  SkimWsprCycleCb cb;
  gpointer user;
};

SkimWsprRx *skim_wspr_rx_new(double rate, double centre, double dial_hz, SkimWsprCycleCb cb,
                             gpointer user) {
  SkimWsprRx *rx = g_new0(SkimWsprRx, 1);
  rx->rate = rate;
  rx->offset = dial_hz + SKIM_WSPR_BFO - centre;
  rx->blk_n = (guint)(rate / 100);                       /* 10 ms                */
  rx->mid_n = MID_RATE / 100;                            /* 120                  */
  rx->b1 = g_new0(double, 2 * rx->blk_n);
  rx->m1 = g_new0(double, 2 * (rx->mid_n + 8));
  rx->o2 = g_new0(double, 2 * 16);
  rx->r1 = create_resample(1, (int)rx->blk_n, rx->b1, rx->m1, (int)rate, MID_RATE, 3000, TAPS1, 1.0);
  rx->r2 = create_resample(1, (int)rx->mid_n, rx->m1, rx->o2, MID_RATE, SKIM_WSPR_RATE, FC2, TAPS2, 1.0);
  rx->ph_re = 1;
  const double w = -2 * G_PI * rx->offset / rate;
  rx->st_re = cos(w);
  rx->st_im = sin(w);
  rx->delay_us = 1e6 * ((TAPS1 / 2.0) / rate + (TAPS2 / 2.0) / MID_RATE);
  rx->done_cycle = G_MININT64;
  rx->cap = g_new0(float, 2 * SKIM_WSPR_C2_N);
  rx->cb = cb;
  rx->user = user;
  return rx;
}

void skim_wspr_rx_free(SkimWsprRx *rx) {
  if (!rx) { return; }
  destroy_resample(rx->r1);
  destroy_resample(rx->r2);
  g_free(rx->b1);
  g_free(rx->m1);
  g_free(rx->o2);
  g_free(rx->cap);
  g_free(rx);
}

static void emit(SkimWsprRx *rx) {
  float *out = g_new0(float, 2 * SKIM_WSPR_C2_N);
  /* the level wsprd sees does not matter (it normalises), but keep it
   * in a float-friendly range: an RMS of 10 */
  double e = 0;
  for (guint i = 0; i < 2 * rx->cap_n; i++) { e += (double)rx->cap[i] * rx->cap[i]; }
  const double rms = rx->cap_n ? sqrt(e / rx->cap_n) : 0;
  const double g = rms > 0 ? 10.0 / rms : 1.0;
  for (guint i = 0; i < 2 * rx->cap_n; i++) { out[i] = (float)(rx->cap[i] * g); }
  rx->done_cycle = rx->cap_cycle;
  rx->capturing = FALSE;
  rx->cb(rx->cap_cycle * 120, out, rx->user);
}

static void take(SkimWsprRx *rx, double t_us, double I, double Q) {
  const gint64 cyc = (gint64)floor(t_us / 120e6);
  if (!rx->capturing) {
    const double pos = t_us - (double)cyc * 120e6;
    if (cyc == rx->done_cycle || pos < 0 || pos >= 0.5e6) { return; }
    rx->capturing = TRUE;
    rx->cap_cycle = cyc;
    rx->cap_n = (guint)lround(pos * SKIM_WSPR_RATE / 1e6);   /* zeros: the late start */
    memset(rx->cap, 0, sizeof(float) * 2 * SKIM_WSPR_C2_N);
  }
  rx->cap[2 * rx->cap_n] = (float)I;
  rx->cap[2 * rx->cap_n + 1] = (float)Q;
  if (++rx->cap_n >= SKIM_WSPR_CAPTURE_N) { emit(rx); }
}

void skim_wspr_rx_feed(SkimWsprRx *rx, const float *iq, guint n, gint64 t_end_us) {
  if (!n) { return; }
  /* the block's first frame: measured (its arrival) and predicted (the
   * frames counted); a gap of a second or more starts the clock again (and
   * drops the capture), else the anchor follows the earliest arrivals fast
   * and the late ones slowly — the delay that is always there, not jitter */
  const double t_meas = (double)t_end_us - (n - 1) * 1e6 / rx->rate;
  const double t_pred = rx->anchor_us + rx->frames * 1e6 / rx->rate;
  if (!rx->anchored || fabs(t_meas - t_pred) > 1e6) {
    rx->anchored = TRUE;
    rx->anchor_us = t_meas;
    rx->frames = 0;
    rx->capturing = FALSE;
  } else {
    const double e = t_meas - t_pred;
    rx->anchor_us += e < 0 ? 0.05 * e : 0.002 * e;
  }
  for (guint i = 0; i < n; i++) {
    const double re = iq[2 * i], im = iq[2 * i + 1];
    rx->b1[2 * rx->fill] = re * rx->ph_re - im * rx->ph_im;
    rx->b1[2 * rx->fill + 1] = re * rx->ph_im + im * rx->ph_re;
    const double pr = rx->ph_re * rx->st_re - rx->ph_im * rx->st_im;
    rx->ph_im = rx->ph_re * rx->st_im + rx->ph_im * rx->st_re;
    rx->ph_re = pr;
    rx->frames++;
    if (++rx->fill < rx->blk_n) { continue; }
    rx->fill = 0;
    const double norm = 1.0 / hypot(rx->ph_re, rx->ph_im);  /* the phasor stays on the circle */
    rx->ph_re *= norm;
    rx->ph_im *= norm;
    if (xresample(rx->r1) != (int)rx->mid_n) { continue; }
    const int m = xresample(rx->r2);
    const double t_blk = rx->anchor_us + (rx->frames - 1) * 1e6 / rx->rate - rx->delay_us;
    for (int j = 0; j < m; j++) {
      take(rx, t_blk - (m - 1 - j) * 1e6 / SKIM_WSPR_RATE, rx->o2[2 * j], rx->o2[2 * j + 1]);
    }
  }
}

char *skim_wspr_write_c2(const char *dir, gint64 start_s, double dial_hz, const float *iq,
                         GError **error) {
  GDateTime *dt = g_date_time_new_from_unix_utc(start_s);
  char *name = g_date_time_format(dt, "%y%m%d_%H%M.c2");         /* 14 characters */
  g_date_time_unref(dt);
  char *path = g_build_filename(dir, name, NULL);
  /* the header: the name (14 bytes), the T/R minutes (int), the dial (MHz,
   * double); then the samples with Q negated — wsprd's own convention
   * (writec2file / readc2file in wsprd.c) */
  const gsize hdr = 14 + sizeof(int) + sizeof(double);
  const gsize len = hdr + sizeof(float) * 2 * SKIM_WSPR_C2_N;
  char *buf = g_malloc0(len);
  memcpy(buf, name, MIN(strlen(name), 14));
  const int trmin = 2;
  const double mhz = dial_hz / 1e6;
  memcpy(buf + 14, &trmin, sizeof trmin);
  memcpy(buf + 14 + sizeof trmin, &mhz, sizeof mhz);
  float *s = (float *)(buf + hdr);
  for (guint i = 0; i < SKIM_WSPR_C2_N; i++) {
    s[2 * i] = iq[2 * i];
    s[2 * i + 1] = -iq[2 * i + 1];
  }
  const gboolean ok = g_file_set_contents(path, buf, (gssize)len, error);
  g_free(buf);
  g_free(name);
  if (!ok) { g_clear_pointer(&path, g_free); }
  return path;
}

/* ---- a decode -------------------------------------------------------------------- */

static gboolean grid_like(const char *g) {
  const gsize n = strlen(g);
  if (n != 4 && n != 6) { return FALSE; }
  if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !g_ascii_isdigit(g[2]) ||
      !g_ascii_isdigit(g[3])) {
    return FALSE;
  }
  return n == 4 || (g_ascii_isalpha(g[4]) && g_ascii_isalpha(g[5]));
}

gboolean skim_wspr_parse_line(const char *line, SkimWsprSpot *out) {
  char hhmm[8];
  int snr, drift, used = 0;
  double dt, mhz;
  if (sscanf(line, "%7s %d %lf %lf %d %n", hhmm, &snr, &dt, &mhz, &drift, &used) != 5 || !used ||
      strlen(hhmm) != 4 || !g_ascii_isdigit(hhmm[0])) {
    return FALSE;
  }
  char **tok = g_strsplit_set(g_strstrip(g_strdup(line + used)), " ", -1);
  GPtrArray *w = g_ptr_array_new();
  for (char **t = tok; *t; t++) {
    if (**t) { g_ptr_array_add(w, *t); }
  }
  gboolean ok = w->len == 2 || w->len == 3;
  char call[32] = "", grid[16] = "";
  int dbm = 0;
  if (ok) {
    g_strlcpy(call, w->pdata[0], sizeof call);
    if (w->len == 3) { g_strlcpy(grid, w->pdata[1], sizeof grid); }
    char *end = NULL;
    dbm = (int)strtol(w->pdata[w->len - 1], &end, 10);
    ok = end && !*end && dbm >= -30 && dbm <= 70;
  }
  g_ptr_array_free(w, TRUE);
  g_strfreev(tok);
  if (!ok) { return FALSE; }
  /* type 3: "<CALL> GRID6 dBm" — the call from the hash table; "<...>" = unknown */
  gsize n = strlen(call);
  if (call[0] == '<') {
    if (n < 3 || call[n - 1] != '>' || strcmp(call, "<...>") == 0) { return FALSE; }
    memmove(call, call + 1, n - 2);
    call[n - 2] = '\0';
  }
  n = strlen(call);
  if (n < 3 || n >= sizeof out->call || (grid[0] && !grid_like(grid))) { return FALSE; }
  for (const char *c = call; *c; c++) {
    if (!g_ascii_isalnum(*c) && *c != '/') { return FALSE; }
  }
  g_strlcpy(out->call, call, sizeof out->call);
  g_strlcpy(out->grid, grid, sizeof out->grid);
  out->snr = snr;
  out->dt = dt;
  out->freq_hz = mhz * 1e6;
  out->drift = drift;
  out->dbm = dbm;
  return TRUE;
}

char *skim_wspr_post_body(const SkimWsprSpot *s, const char *rcall, const char *rgrid,
                          const char *version) {
  GDateTime *d = g_date_time_new_from_unix_utc(s->t);
  char *date = g_date_time_format(d, "%y%m%d"), *time = g_date_time_format(d, "%H%M");
  g_date_time_unref(d);
  char *rc = g_uri_escape_string(rcall, NULL, FALSE), *rg = g_uri_escape_string(rgrid, NULL, FALSE);
  char *tc = g_uri_escape_string(s->call, NULL, FALSE), *tg = g_uri_escape_string(s->grid, NULL, FALSE);
  char *ver = g_uri_escape_string(version, NULL, FALSE);
  char rq[32], tq[32], dt[16];
  g_ascii_formatd(rq, sizeof rq, "%.6f", s->dial_hz / 1e6);
  g_ascii_formatd(tq, sizeof tq, "%.6f", s->freq_hz / 1e6);
  g_ascii_formatd(dt, sizeof dt, "%.1f", s->dt);
  char *body = g_strdup_printf("function=wspr&rcall=%s&rgrid=%s&rqrg=%s&date=%s&time=%s&sig=%d"
                               "&dt=%s&drift=%d&tqrg=%s&tcall=%s&tgrid=%s&dbm=%d&version=%s&mode=2",
                               rc, rg, rq, date, time, s->snr, dt, s->drift, tq, tc, tg, s->dbm, ver);
  g_free(date); g_free(time); g_free(rc); g_free(rg); g_free(tc); g_free(tg); g_free(ver);
  return body;
}

/* ---- the decoder program ---------------------------------------------------------- */

char *skim_wspr_find_decoder(void) {
  char *self = g_file_read_link("/proc/self/exe", NULL);
  if (self) {
    char *dir = g_path_get_dirname(self);
    const char *const tries[] = { "skimmer-wsprd", "vendor/wsprd/skimmer-wsprd",
                                  "../vendor/wsprd/skimmer-wsprd", NULL };
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
  char *p = g_find_program_in_path("skimmer-wsprd");
  return p ? p : g_find_program_in_path("wsprd");
}

gboolean skim_wspr_decode_file(const char *decoder, const char *dir, const char *c2,
                               gint64 t, const char *band, double dial_hz, guint flags,
                               GArray *spots, GCancellable *cancel, GError **error) {
  char mhz[32];
  g_ascii_formatd(mhz, sizeof mhz, "%.6f", dial_hz / 1e6);
  char *prog = g_canonicalize_filename(decoder, NULL);   /* it runs in dir */
  const char *argv[12];
  guint n = 0;
  argv[n++] = prog;
  argv[n++] = "-a"; argv[n++] = dir;
  argv[n++] = "-C"; argv[n++] = "500";
  argv[n++] = "-w";
  if (flags & SKIM_WSPR_DEEP) { argv[n++] = "-d"; }
  if (flags & SKIM_WSPR_NO_SUBTRACT) { argv[n++] = "-s"; }
  argv[n++] = "-f"; argv[n++] = mhz;
  argv[n++] = c2;
  argv[n] = NULL;
  char *out = NULL;
  const gboolean ok = skim_hd_helper_run(argv, dir, decoder, cancel, &out, error);
  g_free(prog);
  if (ok) {
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (char **ln = lines; *ln; ln++) {
      SkimWsprSpot s = { 0 };
      if (!skim_wspr_parse_line(*ln, &s)) { continue; }
      s.t = t;
      g_strlcpy(s.band, band, sizeof s.band);
      s.dial_hz = dial_hz;
      g_array_append_val(spots, s);
    }
    g_strfreev(lines);
  }
  g_free(out);
  return ok;
}

/* ---- the whole ------------------------------------------------------------------- */

typedef struct {
  SkimHdWspr *w;
  SkimWsprRx *rx;
  guint       band;                     /* the receiver                          */
  const char *name;                     /* SKIM_WSPR_DIALS' band                 */
  char        rx_name[16];              /* the receiver's band name (its row)    */
  double      dial_hz;
} Slot;

typedef struct {
  gint64 start_s;
  guint  slot;
  float *iq;
} Job;

static Job STOP;

#define KEEP_S       (24 * 3600)
#define QUEUE_MAX    20000
#define FILE_COMPACT 20000              /* lines appended before wspr.tsv is rewritten */

struct _SkimHdWspr {
  char   *call, *locator, *url, *dir, *work, *tsv, *decoder, *version;
  guint    flags;                       /* SKIM_WSPR_DEEP | SKIM_WSPR_NO_SUBTRACT */
  gboolean upload;
  Slot    slot[SKIM_WSPR_MAX_SLOTS];
  guint   nslots;
  GPtrArray *by_band[16];               /* receiver → its slots (Slot *)         */
  GAsyncQueue *jobs;
  GThread *thread;
  GCancellable *cancel;
  GMutex  lock;                         /* below: the decoder thread writes, any reads */
  GArray *spots;                        /* SkimWsprSpot, the last 24 h, oldest first */
  GArray *queue;                        /* … not yet on WSPRnet                  */
  guint64 cycles, decodes, uploaded, upload_failed;
  gint64  last_cycle;
  guint   last_decodes;
  char   *error;                        /* the last trouble; NULL = none         */
  guint   appended;
};

static void set_error(SkimHdWspr *w, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_error(SkimHdWspr *w, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *m = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_mutex_lock(&w->lock);
  const gboolean same = g_strcmp0(w->error, m) == 0;
  g_free(w->error);
  w->error = m;
  g_mutex_unlock(&w->lock);
  if (!same) { g_warning("wspr: %s", m); }
}

static void clear_error(SkimHdWspr *w) {
  g_mutex_lock(&w->lock);
  g_clear_pointer(&w->error, g_free);
  g_mutex_unlock(&w->lock);
}

static void cycle_cb(gint64 start_s, float *iq, gpointer user) {
  Slot *s = user;
  Job *j = g_new(Job, 1);
  j->start_s = start_s;
  j->slot = (guint)(s - s->w->slot);
  j->iq = iq;
  g_async_queue_push(s->w->jobs, j);
}

/* ---- wspr.tsv: t band dial freq snr dt drift call grid dbm -------------------------- */

static char *spot_line(const SkimWsprSpot *s) {
  char dt[16];
  g_ascii_formatd(dt, sizeof dt, "%.1f", s->dt);
  return g_strdup_printf("%" G_GINT64_FORMAT "\t%s\t%.0f\t%.0f\t%d\t%s\t%d\t%s\t%s\t%d\n", s->t,
                         s->band, s->dial_hz, s->freq_hz, s->snr, dt, s->drift, s->call,
                         s->grid, s->dbm);
}

static gboolean spot_parse(const char *line, SkimWsprSpot *s) {
  char **f = g_strsplit(line, "\t", -1);
  const gboolean ok = g_strv_length(f) == 10;
  if (ok) {
    memset(s, 0, sizeof *s);
    s->t = g_ascii_strtoll(f[0], NULL, 10);
    g_strlcpy(s->band, f[1], sizeof s->band);
    s->dial_hz = g_ascii_strtod(f[2], NULL);
    s->freq_hz = g_ascii_strtod(f[3], NULL);
    s->snr = atoi(f[4]);
    s->dt = g_ascii_strtod(f[5], NULL);
    s->drift = atoi(f[6]);
    g_strlcpy(s->call, f[7], sizeof s->call);
    g_strlcpy(s->grid, f[8], sizeof s->grid);
    s->dbm = atoi(f[9]);
  }
  g_strfreev(f);
  return ok && s->t > 0 && s->call[0];
}

/* Rewrite wspr.tsv with the spots held (lock held). */
static void tsv_rewrite(SkimHdWspr *w) {
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < w->spots->len; i++) {
    char *l = spot_line(&g_array_index(w->spots, SkimWsprSpot, i));
    g_string_append(s, l);
    g_free(l);
  }
  GError *e = NULL;
  if (!g_file_set_contents(w->tsv, s->str, (gssize)s->len, &e)) {
    g_warning("wspr: %s: %s", w->tsv, e->message);
    g_clear_error(&e);
  }
  g_string_free(s, TRUE);
  w->appended = 0;
}

static void tsv_load(SkimHdWspr *w, gint64 now) {
  char *text = NULL;
  if (!g_file_get_contents(w->tsv, &text, NULL, NULL)) { return; }
  char **lines = g_strsplit(text, "\n", -1);
  for (char **l = lines; *l; l++) {
    SkimWsprSpot s;
    if (spot_parse(*l, &s) && s.t >= now - KEEP_S) { g_array_append_val(w->spots, s); }
  }
  g_strfreev(lines);
  g_free(text);
  tsv_rewrite(w);
}

static void expire(SkimHdWspr *w, gint64 now) {   /* lock held */
  guint k = 0;
  while (k < w->spots->len && g_array_index(w->spots, SkimWsprSpot, k).t < now - KEEP_S) { k++; }
  if (k) { g_array_remove_range(w->spots, 0, k); }
  k = 0;
  while (k < w->queue->len && g_array_index(w->queue, SkimWsprSpot, k).t < now - KEEP_S) { k++; }
  if (k) { g_array_remove_range(w->queue, 0, k); }
}

/* ---- the decoder thread ---------------------------------------------------------- */

static void keep(SkimHdWspr *w, GArray *got) {
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  GString *add = g_string_new(NULL);
  g_mutex_lock(&w->lock);
  expire(w, now);
  for (guint i = 0; i < got->len; i++) {
    const SkimWsprSpot *s = &g_array_index(got, SkimWsprSpot, i);
    g_array_append_val(w->spots, *s);
    if (w->upload && w->queue->len < QUEUE_MAX) { g_array_append_val(w->queue, *s); }
    char *l = spot_line(s);
    g_string_append(add, l);
    g_free(l);
  }
  w->decodes += got->len;
  w->appended += got->len;
  if (w->appended > FILE_COMPACT) {
    tsv_rewrite(w);
  } else if (add->len) {
    FILE *f = fopen(w->tsv, "a");
    if (!f || fputs(add->str, f) < 0) { g_warning("wspr: %s: %s", w->tsv, g_strerror(errno)); }
    if (f) { fclose(f); }
  }
  g_mutex_unlock(&w->lock);
  g_string_free(add, TRUE);
}

static void decode_job(SkimHdWspr *w, Job *j) {
  const Slot *s = &w->slot[j->slot];
  if (!w->decoder) {
    set_error(w, "no decoder (skimmer-wsprd) found — nothing is decoded");
    return;
  }
  GError *e = NULL;
  char *c2 = skim_wspr_write_c2(w->work, j->start_s, s->dial_hz, j->iq, &e);
  if (!c2) {
    set_error(w, "%s", e->message);
    g_clear_error(&e);
    return;
  }
  GArray *got = g_array_new(FALSE, TRUE, sizeof(SkimWsprSpot));
  const gboolean ok = skim_wspr_decode_file(w->decoder, w->work, c2, j->start_s, s->name,
                                            s->dial_hz, w->flags, got, w->cancel, &e);
  g_unlink(c2);
  g_free(c2);
  char *all = g_build_filename(w->work, "ALL_WSPR.TXT", NULL);   /* wsprd's own log grows */
  g_unlink(all);
  g_free(all);
  if (!ok) {
    if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) { set_error(w, "%s: %s", s->name, e->message); }
    g_clear_error(&e);
  } else {
    keep(w, got);
    g_mutex_lock(&w->lock);
    w->cycles++;
    if (w->last_cycle != j->start_s) { w->last_decodes = 0; }
    w->last_cycle = j->start_s;
    w->last_decodes += got->len;
    g_mutex_unlock(&w->lock);
  }
  g_array_unref(got);
}

/* Post what waits, oldest first; the first failure ends the round (the
 * next cycle tries again). */
static void upload_round(SkimHdWspr *w, SoupSession *ss) {
  for (;;) {
    SkimWsprSpot s;
    g_mutex_lock(&w->lock);
    const gboolean any = w->queue->len > 0;
    if (any) { s = g_array_index(w->queue, SkimWsprSpot, 0); }
    g_mutex_unlock(&w->lock);
    if (!any || g_cancellable_is_cancelled(w->cancel)) { return; }
    char *body = skim_wspr_post_body(&s, w->call, w->locator, w->version);
    SoupMessage *m = soup_message_new("POST", w->url);
    if (!m) {
      g_free(body);
      set_error(w, "upload: \"%s\" is no URL", w->url);
      return;
    }
    GBytes *b = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(m, "application/x-www-form-urlencoded", b);
    g_bytes_unref(b);
    GError *e = NULL;
    GBytes *reply = soup_session_send_and_read(ss, m, w->cancel, &e);
    const guint st = soup_message_get_status(m);
    g_object_unref(m);
    if (reply) { g_bytes_unref(reply); }
    if (!reply || st < 200 || st >= 300) {
      if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&e);
        return;
      }
      g_mutex_lock(&w->lock);
      w->upload_failed++;
      /* refused (4xx): this spot will not do better — drop it; else keep it */
      if (reply && st >= 400 && st < 500 && w->queue->len) { g_array_remove_index(w->queue, 0); }
      g_mutex_unlock(&w->lock);
      if (e) {
        set_error(w, "upload: %s", e->message);
      } else {
        set_error(w, "upload: WSPRnet answered HTTP %u", st);
      }
      g_clear_error(&e);
      return;
    }
    g_mutex_lock(&w->lock);
    if (w->queue->len) { g_array_remove_index(w->queue, 0); }
    w->uploaded++;
    const gboolean was = w->error && g_str_has_prefix(w->error, "upload:");
    g_mutex_unlock(&w->lock);
    if (was) { clear_error(w); }
  }
}

static gpointer decoder_thread(gpointer user) {
  SkimHdWspr *w = user;
  GMainContext *ctx = g_main_context_new();            /* libsoup's sync calls run here */
  g_main_context_push_thread_default(ctx);
  SoupSession *ss = w->upload ? soup_session_new_with_options("timeout", 20, "idle-timeout", 60,
                                                              "user-agent", w->version, NULL)
                              : NULL;
  for (;;) {
    Job *j = g_async_queue_timeout_pop(w->jobs, 30 * G_USEC_PER_SEC);
    if (j == &STOP) { break; }
    if (j) {
      decode_job(w, j);
      g_free(j->iq);
      g_free(j);
      if (g_async_queue_length(w->jobs) > 0) { continue; }   /* the cycle's other bands first */
    }
    if (ss) { upload_round(w, ss); }
  }
  g_clear_object(&ss);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  return NULL;
}

/* ---- life ------------------------------------------------------------------------ */

SkimHdWspr *skim_hd_wspr_new(const SkimHdWsprConfig *cfg, guint nb, const char *const *names,
                             const double *centres, double rate) {
  SkimHdWspr *w = g_new0(SkimHdWspr, 1);
  g_mutex_init(&w->lock);
  w->call = g_strdup(cfg->call ? cfg->call : "");
  w->locator = g_strdup(cfg->locator ? cfg->locator : "");
  w->upload = cfg->upload && w->call[0] && w->locator[0];
  w->flags = cfg->flags;
  w->url = g_strdup(cfg->upload_url && *cfg->upload_url ? cfg->upload_url : SKIM_WSPR_URL);
  w->dir = g_strdup(cfg->dir);
  w->work = g_build_filename(cfg->dir, "wspr", NULL);
  w->tsv = g_build_filename(cfg->dir, "wspr.tsv", NULL);
  w->decoder = cfg->decoder ? g_strdup(cfg->decoder) : skim_wspr_find_decoder();
  w->version = g_strdup("skimmer-" SKIMMER_VERSION);
  w->spots = g_array_new(FALSE, TRUE, sizeof(SkimWsprSpot));
  w->queue = g_array_new(FALSE, TRUE, sizeof(SkimWsprSpot));
  w->jobs = g_async_queue_new();
  w->cancel = g_cancellable_new();
  w->last_cycle = 0;
  if (g_mkdir_with_parents(w->work, 0755) != 0) {
    set_error(w, "%s: %s", w->work, g_strerror(errno));
  }
  for (guint b = 0; b < nb && b < G_N_ELEMENTS(w->by_band); b++) {
    w->by_band[b] = g_ptr_array_new();
    for (const SkimWsprDial *d = SKIM_WSPR_DIALS; d->band; d++) {
      if (!skim_wspr_covers(centres[b], rate, d->dial_hz) || w->nslots >= SKIM_WSPR_MAX_SLOTS) {
        continue;
      }
      gboolean dup = FALSE;                            /* another receiver has it */
      for (guint k = 0; k < w->nslots; k++) { dup |= w->slot[k].dial_hz == d->dial_hz; }
      if (dup) { continue; }
      Slot *s = &w->slot[w->nslots++];
      s->w = w;
      s->band = b;
      s->name = d->band;
      g_strlcpy(s->rx_name, names && names[b] ? names[b] : d->band, sizeof s->rx_name);
      s->dial_hz = d->dial_hz;
      s->rx = skim_wspr_rx_new(rate, centres[b], d->dial_hz, cycle_cb, s);
      g_ptr_array_add(w->by_band[b], s);
    }
  }
  g_mutex_lock(&w->lock);
  tsv_load(w, g_get_real_time() / G_USEC_PER_SEC);
  g_mutex_unlock(&w->lock);
  if (!w->decoder) { set_error(w, "no decoder (skimmer-wsprd) found — nothing is decoded"); }
  if (w->nslots) { w->thread = g_thread_new("wspr", decoder_thread, w); }
  return w;
}

void skim_hd_wspr_free(SkimHdWspr *w) {
  if (!w) { return; }
  if (w->thread) {
    g_cancellable_cancel(w->cancel);
    g_async_queue_push_front(w->jobs, &STOP);
    g_thread_join(w->thread);
  }
  Job *j;
  while ((j = g_async_queue_try_pop(w->jobs))) {
    if (j != &STOP) {
      g_free(j->iq);
      g_free(j);
    }
  }
  g_async_queue_unref(w->jobs);
  for (guint k = 0; k < w->nslots; k++) { skim_wspr_rx_free(w->slot[k].rx); }
  for (guint b = 0; b < G_N_ELEMENTS(w->by_band); b++) {
    if (w->by_band[b]) { g_ptr_array_free(w->by_band[b], TRUE); }
  }
  g_object_unref(w->cancel);
  g_array_unref(w->spots);
  g_array_unref(w->queue);
  g_free(w->call); g_free(w->locator); g_free(w->url); g_free(w->dir); g_free(w->work);
  g_free(w->tsv); g_free(w->decoder); g_free(w->version); g_free(w->error);
  g_mutex_clear(&w->lock);
  g_free(w);
}

void skim_hd_wspr_push(SkimHdWspr *w, guint b, const float *iq, guint n) {
  if (!w || b >= G_N_ELEMENTS(w->by_band) || !w->by_band[b] || !w->by_band[b]->len) { return; }
  skim_hd_wspr_push_at(w, b, iq, n, g_get_real_time());
}

void skim_hd_wspr_push_at(SkimHdWspr *w, guint b, const float *iq, guint n, gint64 now) {
  if (!w || b >= G_N_ELEMENTS(w->by_band) || !w->by_band[b]) { return; }
  for (guint k = 0; k < w->by_band[b]->len; k++) {
    skim_wspr_rx_feed(((Slot *)w->by_band[b]->pdata[k])->rx, iq, n, now);
  }
}

char *skim_hd_wspr_dials_text(SkimHdWspr *w) {
  GString *s = g_string_new(NULL);
  for (guint k = 0; k < w->nslots; k++) {
    g_string_append_printf(s, "%s%s %.1f", k ? ", " : "", w->slot[k].name, w->slot[k].dial_hz / 1e3);
  }
  return g_string_free(s, FALSE);
}

char *skim_hd_wspr_status_json(SkimHdWspr *w) {
  GString *s = g_string_new("{\"on\":true,\"upload\":");
  g_string_append(s, w->upload ? "true" : "false");
  g_string_append(s, ",\"call\":");
  jstr(s, w->call);
  g_string_append(s, ",\"locator\":");        /* its square only: the page is public */
  char sq[5];
  g_strlcpy(sq, w->locator, sizeof sq);
  jstr(s, sq);
  g_string_append(s, ",\"decoder\":");
  g_string_append(s, w->decoder ? "true" : "false");
  g_string_append_printf(s, ",\"deep\":%s,\"subtract\":%s",
                         w->flags & SKIM_WSPR_DEEP ? "true" : "false",
                         w->flags & SKIM_WSPR_NO_SUBTRACT ? "false" : "true");
  g_string_append(s, ",\"dials\":[");
  for (guint k = 0; k < w->nslots; k++) {
    g_string_append_printf(s, "%s{\"band\":", k ? "," : "");
    jstr(s, w->slot[k].name);
    g_string_append_printf(s, ",\"dial_hz\":%.0f,\"rx\":%u,\"lo_hz\":%.0f,\"hi_hz\":%.0f}",
                           w->slot[k].dial_hz, w->slot[k].band,
                           w->slot[k].dial_hz + SKIM_WSPR_BFO - SKIM_WSPR_HALF_SPAN,
                           w->slot[k].dial_hz + SKIM_WSPR_BFO + SKIM_WSPR_HALF_SPAN);
  }
  g_mutex_lock(&w->lock);
  /* per receiver (a row of the page's band table): the stations heard on its
   * dials in the last 30 min, and the newest one */
  g_string_append(s, "],\"rx\":[");
  const gint64 since = g_get_real_time() / G_USEC_PER_SEC - SKIM_WSPR_RECENT_S;
  guint nrx = 0;
  for (guint k = 0; k < w->nslots; k++) {
    gboolean first = TRUE;                            /* the receiver's first dial */
    for (guint j = 0; j < k; j++) { first &= w->slot[j].band != w->slot[k].band; }
    if (!first) { continue; }
    GHashTable *calls = g_hash_table_new(g_str_hash, g_str_equal);
    const char *last = "";
    for (guint i = 0; i < w->spots->len; i++) {
      const SkimWsprSpot *p = &g_array_index(w->spots, SkimWsprSpot, i);
      if (p->t < since) { continue; }
      gboolean mine = FALSE;
      for (guint j = 0; j < w->nslots; j++) {
        mine |= w->slot[j].band == w->slot[k].band && w->slot[j].dial_hz == p->dial_hz;
      }
      if (!mine) { continue; }
      g_hash_table_add(calls, (gpointer)p->call);
      last = p->call;                                 /* oldest first: the last wins */
    }
    g_string_append_printf(s, "%s{\"band\":", nrx++ ? "," : "");
    jstr(s, w->slot[k].rx_name);
    g_string_append_printf(s, ",\"stations\":%u,\"last\":", g_hash_table_size(calls));
    jstr(s, last);
    g_string_append_c(s, '}');
    g_hash_table_unref(calls);
  }
  g_string_append_printf(s, "],\"recent_s\":%d,\"cycles\":%" G_GUINT64_FORMAT ",\"decodes\":%" G_GUINT64_FORMAT
                         ",\"last_cycle\":%" G_GINT64_FORMAT ",\"last_decodes\":%u,\"spots_24h\":%u"
                         ",\"uploaded\":%" G_GUINT64_FORMAT ",\"upload_failed\":%" G_GUINT64_FORMAT
                         ",\"queued\":%u,\"error\":",
                         SKIM_WSPR_RECENT_S, w->cycles, w->decodes, w->last_cycle, w->last_decodes, w->spots->len,
                         w->uploaded, w->upload_failed, w->queue->len);
  if (w->error) {
    jstr(s, w->error);
  } else {
    g_string_append(s, "null");
  }
  g_mutex_unlock(&w->lock);
  g_string_append_c(s, '}');
  return g_string_free(s, FALSE);
}

typedef struct {
  const SkimWsprSpot *last;             /* the newest spot of the call           */
  guint n;
  int   best;
  guint bands;                          /* bit k = SKIM_WSPR_DIALS[k]'s band name  */
  gint64 seen[32];                      /* [k]: its newest decode there, unix s  */
  char  grid[8];                        /* the longest locator heard             */
} Station;

static gint by_last(gconstpointer a, gconstpointer b) {
  const Station *x = *(Station *const *)a, *y = *(Station *const *)b;
  if (x->last->t != y->last->t) { return x->last->t > y->last->t ? -1 : 1; }
  return strcmp(x->last->call, y->last->call);
}

static int band_bit(const char *band) {
  for (int k = 0; SKIM_WSPR_DIALS[k].band; k++) {
    if (strcmp(SKIM_WSPR_DIALS[k].band, band) == 0) { return k; }
  }
  return -1;
}

char *skim_hd_wspr_stations_json(SkimHdWspr *w, guint hours, gint64 now) {
  GHashTable *by = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
  GString *s = g_string_new(NULL);
  g_mutex_lock(&w->lock);
  guint nspots = 0;
  for (guint i = 0; i < w->spots->len; i++) {
    const SkimWsprSpot *p = &g_array_index(w->spots, SkimWsprSpot, i);
    if (p->t < now - (gint64)hours * 3600) { continue; }
    nspots++;
    Station *st = g_hash_table_lookup(by, p->call);
    if (!st) {
      st = g_new0(Station, 1);
      st->best = p->snr;
      g_hash_table_insert(by, (gpointer)p->call, st);
    }
    st->last = p;                                       /* oldest first: the last wins */
    st->n++;
    st->best = MAX(st->best, p->snr);
    const int bit = band_bit(p->band);
    if (bit >= 0) { st->bands |= 1u << bit; st->seen[bit] = MAX(st->seen[bit], p->t); }
    if (strlen(p->grid) >= strlen(st->grid)) { g_strlcpy(st->grid, p->grid, sizeof st->grid); }
  }
  GPtrArray *list = g_ptr_array_new();
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init(&it, by);
  while (g_hash_table_iter_next(&it, NULL, &v)) { g_ptr_array_add(list, v); }
  g_ptr_array_sort(list, by_last);
  g_string_append_printf(s, "{\"hours\":%u,\"spots\":%u,\"stations\":[", hours, nspots);
  for (guint i = 0; i < list->len; i++) {
    const Station *st = list->pdata[i];
    const SkimWsprSpot *p = st->last;
    g_string_append(s, i ? ",{\"call\":" : "{\"call\":");
    jstr(s, p->call);
    g_string_append(s, ",\"grid\":");
    jstr(s, st->grid);
    GString *bands = g_string_new(NULL);
    for (int k = 0; SKIM_WSPR_DIALS[k].band; k++) {   /* low to high, each name once */
      if ((st->bands & (1u << k)) && !strstr(bands->str, SKIM_WSPR_DIALS[k].band)) {
        g_string_append_printf(bands, "%s%s", bands->len ? "," : "", SKIM_WSPR_DIALS[k].band);
      }
    }
    g_string_append(s, ",\"bands\":");
    jstr(s, bands->str);
    g_string_free(bands, TRUE);
    g_string_append(s, ",\"band\":");
    jstr(s, p->band);
    g_string_append_printf(s, ",\"dbm\":%d,\"n\":%u,\"snr\":%d,\"best\":%d,\"last\":%" G_GINT64_FORMAT
                           ",\"freq\":%.0f,\"drift\":%d,\"seen\":{",
                           p->dbm, st->n, p->snr, st->best, p->t, p->freq_hz, p->drift);
    /* each band's newest decode (the band's name once: 60m has two dials) */
    gboolean first = TRUE;
    for (int k = 0; SKIM_WSPR_DIALS[k].band; k++) {
      if (!(st->bands & (1u << k)) || band_bit(SKIM_WSPR_DIALS[k].band) != k) { continue; }
      g_string_append_printf(s, "%s\"%s\":%" G_GINT64_FORMAT, first ? "" : ",",
                             SKIM_WSPR_DIALS[k].band, st->seen[k]);
      first = FALSE;
    }
    g_string_append(s, "}}");
  }
  g_mutex_unlock(&w->lock);
  g_string_append(s, "]}");
  g_ptr_array_unref(list);
  g_hash_table_unref(by);
  return g_string_free(s, FALSE);
}
