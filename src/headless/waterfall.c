/* waterfall.c — see waterfall.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "waterfall.h"

#include <fftw3.h>
#include <math.h>
#include <string.h>

#include "engine/fftw_lock.h"
#include "json.h"

#define RING 32                        /* frames kept a band (~1.4 s)        */

/* ---- the analyser ------------------------------------------------------------- */

struct _SkimWfAnalyser {
  guint          n;
  fftwf_complex *in, *out;
  fftwf_plan     plan;
  float         *win;
  float         *buf;                  /* n frames, interleaved I/Q          */
  guint          fill;
  float          norm;
  float         *pow;
};

guint skim_hd_wf_fft_size(double rate) { return rate > 100000 ? 2 * SKIM_WF_FFT : SKIM_WF_FFT; }

SkimWfAnalyser *skim_wf_analyser_new(guint n) {
  SkimWfAnalyser *a = g_new0(SkimWfAnalyser, 1);
  a->n = n;
  a->in = fftwf_alloc_complex(n);
  a->out = fftwf_alloc_complex(n);
  skim_fftw_lock();
  a->plan = fftwf_plan_dft_1d((int)n, a->in, a->out, FFTW_FORWARD, FFTW_ESTIMATE);
  skim_fftw_unlock();
  a->win = g_new(float, n);
  a->buf = g_new0(float, 2 * n);
  a->pow = g_new(float, n);
  double sum = 0;
  for (guint k = 0; k < n; k++) {             /* 4-term Blackman-Harris     */
    const double x = 2 * G_PI * k / (n - 1);
    a->win[k] = (float)(0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x));
    sum += a->win[k];
  }
  a->norm = (float)(1.0 / (sum * sum));
  return a;
}

void skim_wf_analyser_free(SkimWfAnalyser *a) {
  if (!a) { return; }
  skim_fftw_lock();
  fftwf_destroy_plan(a->plan);
  skim_fftw_unlock();
  fftwf_free(a->in);
  fftwf_free(a->out);
  g_free(a->win);
  g_free(a->buf);
  g_free(a->pow);
  g_free(a);
}

void skim_wf_analyser_feed(SkimWfAnalyser *a, const float *iq, guint n, SkimWfFrameCb cb,
                           gpointer user) {
  const guint hop = a->n / 2;
  while (n) {
    const guint take = MIN(n, a->n - a->fill);
    memcpy(a->buf + 2 * a->fill, iq, (gsize)take * 2 * sizeof(float));
    a->fill += take;
    iq += 2 * take;
    n -= take;
    if (a->fill < a->n) { break; }
    for (guint k = 0; k < a->n; k++) {
      a->in[k][0] = a->buf[2 * k] * a->win[k];
      a->in[k][1] = a->buf[2 * k + 1] * a->win[k];
    }
    fftwf_execute(a->plan);
    for (guint k = 0; k < a->n; k++) {
      a->pow[k] = (a->out[k][0] * a->out[k][0] + a->out[k][1] * a->out[k][1]) * a->norm;
    }
    cb(a->pow, a->n, user);
    memmove(a->buf, a->buf + 2 * hop, (gsize)(a->n - hop) * 2 * sizeof(float));
    a->fill = a->n - hop;
  }
}

void skim_hd_wf_render(const float *power, guint n, double rate, double centre, double lo,
                       double hi, guint w, guint8 *out) {
  const double bw = rate / n, px = (hi - lo) / w;
  for (guint p = 0; p < w; p++) {
    /* display bins (lowest frequency first) under the pixel */
    const double f0 = lo + p * px, f1 = f0 + px;
    long d0 = lround(floor((f0 - centre) / bw)) + (long)n / 2;
    long d1 = lround(ceil((f1 - centre) / bw)) + (long)n / 2 - 1;
    if (d1 < d0) { d1 = d0; }
    if (d1 < 0 || d0 >= (long)n) { out[p] = 0; continue; }
    d0 = MAX(d0, 0);
    d1 = MIN(d1, (long)n - 1);
    float m = 0;
    for (long d = d0; d <= d1; d++) {
      const float v = power[(d + n / 2) % n];  /* display → FFT order       */
      if (v > m) { m = v; }
    }
    const double db = m > 0 ? 10 * log10(m) : -300;
    out[p] = (guint8)CLAMP(lround((db - SKIM_WF_DB_FLOOR) * 2), 0, 255);
  }
}

/* ---- the bands and the viewers ------------------------------------------------ */

typedef struct {
  GMutex          lock;
  gint            nsubs;               /* atomic: viewers on it              */
  SkimWfAnalyser *an;                  /* while watched                      */
  guint           n;
  float          *ring;                /* RING × n                           */
  guint64         seq;                 /* frames made                        */
  double          centre;
} WfBand;

struct _SkimHdWaterfall {
  guint      nb;
  char     **names;
  double    *centres;
  double     rate;
  WfBand    *band;
  GPtrArray *viewers;                  /* Viewer*                            */
};

typedef struct {
  SkimHdWaterfall         *w;
  SoupWebsocketConnection *c;
  char                    *peer;
  int                      band;      /* -1: none yet                        */
  double                   lo, hi;
  guint                    width, rate, timer;
  guint64                  frame_seq; /* the band's frames taken             */
  guint32                  row_seq, acked;
  float                   *avg;
  guint                    avg_n;
  guint8                  *row;
} Viewer;

static void frame_cb(const float *power, guint n, gpointer user) {
  WfBand *b = user;                                   /* under b->lock      */
  memcpy(b->ring + (b->seq % RING) * n, power, n * sizeof(float));
  b->seq++;
}

void skim_hd_waterfall_push(SkimHdWaterfall *w, guint bi, const float *iq, guint n,
                            double rate, double centre) {
  (void)rate;
  if (!w || bi >= w->nb) { return; }
  WfBand *b = &w->band[bi];
  if (!g_atomic_int_get(&b->nsubs)) { return; }       /* nobody watches     */
  g_mutex_lock(&b->lock);
  if (b->an) {
    b->centre = centre;
    skim_wf_analyser_feed(b->an, iq, n, frame_cb, b);
  }
  g_mutex_unlock(&b->lock);
}

static void attach(Viewer *v, int bi) {
  WfBand *b = &v->w->band[bi];
  g_mutex_lock(&b->lock);
  if (!b->an) {
    b->n = skim_hd_wf_fft_size(v->w->rate);
    b->an = skim_wf_analyser_new(b->n);
    b->ring = g_new0(float, (gsize)RING * b->n);
    b->seq = 0;
    b->centre = v->w->centres[bi];
  }
  v->frame_seq = b->seq;
  g_atomic_int_inc(&b->nsubs);
  g_mutex_unlock(&b->lock);
  v->band = bi;
}

static void detach(Viewer *v) {
  if (v->band < 0) { return; }
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  if (g_atomic_int_dec_and_test(&b->nsubs)) {         /* the last one: stop */
    g_clear_pointer(&b->an, skim_wf_analyser_free);
    g_clear_pointer(&b->ring, g_free);
  }
  g_mutex_unlock(&b->lock);
  v->band = -1;
}

/* One row to the viewer: the band's frames since its last one, averaged. */
static gboolean viewer_tick(gpointer user) {
  Viewer *v = user;
  if (v->band < 0 || !v->width) { return G_SOURCE_CONTINUE; }
  const guint32 window = MAX(4u, v->rate * 2);
  if (v->row_seq - v->acked >= window) { return G_SOURCE_CONTINUE; }   /* slow link */
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  const guint64 have = b->an ? b->seq - v->frame_seq : 0;
  if (!have) {
    g_mutex_unlock(&b->lock);
    return G_SOURCE_CONTINUE;
  }
  const guint n = b->n;
  if (v->avg_n != n) {
    g_free(v->avg);
    v->avg = g_new(float, n);
    v->avg_n = n;
  }
  const guint frames = (guint)MIN(have, (guint64)RING);
  memset(v->avg, 0, n * sizeof(float));
  for (guint64 s = b->seq - frames; s < b->seq; s++) {
    const float *f = b->ring + (s % RING) * n;
    for (guint k = 0; k < n; k++) { v->avg[k] += f[k]; }
  }
  for (guint k = 0; k < n; k++) { v->avg[k] /= frames; }
  v->frame_seq = b->seq;
  const double centre = b->centre;
  g_mutex_unlock(&b->lock);

  const gsize len = 32 + v->width;
  guint8 *msg = v->row;
  skim_hd_wf_render(v->avg, n, v->w->rate, centre, v->lo, v->hi, v->width, msg + 32);
  const guint16 wl = GUINT16_TO_LE((guint16)v->width);
  const guint32 sq = GUINT32_TO_LE(++v->row_seq);
  const double t = g_get_real_time() / 1e6;
  msg[0] = 1;
  msg[1] = (guint8)v->band;
  memcpy(msg + 2, &wl, 2);
  memcpy(msg + 4, &sq, 4);
  memcpy(msg + 8, &t, 8);                             /* x86/ARM: LE doubles */
  memcpy(msg + 16, &v->lo, 8);
  memcpy(msg + 24, &v->hi, 8);
  soup_websocket_connection_send_binary(v->c, msg, len);
  return G_SOURCE_CONTINUE;
}

static void viewer_set_rate(Viewer *v, guint rate) {
  if (rate == v->rate && v->timer) { return; }
  v->rate = rate;
  if (v->timer) { g_source_remove(v->timer); }
  v->timer = g_timeout_add(1000 / rate, viewer_tick, v);
}

static void on_message(SoupWebsocketConnection *c, gint type, GBytes *message, gpointer user) {
  (void)c;
  Viewer *v = user;
  if (type != SOUP_WEBSOCKET_DATA_TEXT) { return; }
  gsize len = 0;
  const char *data = g_bytes_get_data(message, &len);
  char *text = g_strndup(data, MIN(len, 512));
  if (g_str_has_prefix(text, "a=")) {                 /* a row arrived      */
    const guint32 a = (guint32)g_ascii_strtoull(text + 2, NULL, 10);
    if ((gint32)(a - v->acked) > 0 && (gint32)(v->row_seq - a) >= 0) { v->acked = a; }
    g_free(text);
    return;
  }
  GHashTable *f = soup_form_decode(text);
  g_free(text);
  const char *sb = g_hash_table_lookup(f, "band"), *sl = g_hash_table_lookup(f, "lo");
  const char *sh = g_hash_table_lookup(f, "hi"), *sw = g_hash_table_lookup(f, "w");
  const char *sr = g_hash_table_lookup(f, "rate");
  if (sb) {
    const gint64 bi = g_ascii_strtoll(sb, NULL, 10);
    if (bi >= 0 && bi < (gint64)v->w->nb && bi != v->band) {
      detach(v);
      attach(v, (int)bi);
      g_message("waterfall: %s watches %s", v->peer, v->w->names[bi]);
    }
  }
  if (sl && sh) {
    const double lo = g_ascii_strtod(sl, NULL), hi = g_ascii_strtod(sh, NULL);
    if (hi > lo && hi - lo >= 100) { v->lo = lo; v->hi = hi; }
  }
  if (sw) {
    const gint64 width = CLAMP(g_ascii_strtoll(sw, NULL, 10), 16, SKIM_WF_MAX_W);
    if ((guint)width != v->width) {
      v->width = (guint)width;
      g_free(v->row);
      v->row = g_malloc(32 + v->width);
    }
  }
  if (sr) { viewer_set_rate(v, (guint)CLAMP(g_ascii_strtoll(sr, NULL, 10), 1, 25)); }
  if (v->band >= 0 && v->hi <= v->lo) {               /* the whole band     */
    v->lo = v->w->centres[v->band] - v->w->rate / 2;
    v->hi = v->w->centres[v->band] + v->w->rate / 2;
  }
  v->acked = v->row_seq;                              /* a fresh start      */
  g_hash_table_unref(f);
}

static void viewer_free(Viewer *v) {
  detach(v);
  if (v->timer) { g_source_remove(v->timer); }
  g_signal_handlers_disconnect_by_data(v->c, v);
  g_object_unref(v->c);
  g_free(v->avg);
  g_free(v->row);
  g_free(v->peer);
  g_free(v);
}

static void on_closed(SoupWebsocketConnection *c, gpointer user) {
  (void)c;
  Viewer *v = user;
  g_message("waterfall: %s left", v->peer);
  g_ptr_array_remove(v->w->viewers, v);               /* frees it           */
}

void skim_hd_waterfall_add(SkimHdWaterfall *w, SoupWebsocketConnection *c, const char *peer) {
  if (w->viewers->len >= SKIM_WF_MAX_VIEWERS) {
    soup_websocket_connection_close(c, 1013, "too many waterfall viewers — try again later");
    return;
  }
  Viewer *v = g_new0(Viewer, 1);
  v->w = w;
  v->c = g_object_ref(c);
  v->peer = g_strdup(peer ? peer : "a viewer");
  v->band = -1;
  g_ptr_array_add(w->viewers, v);
  soup_websocket_connection_set_keepalive_interval(c, 30);   /* a dead link goes */
  g_signal_connect(c, "message", G_CALLBACK(on_message), v);
  g_signal_connect(c, "closed", G_CALLBACK(on_closed), v);
  viewer_set_rate(v, 5);
  GString *o = g_string_new(NULL);
  g_string_append_printf(o, "{\"type\":\"hello\",\"rate\":%.0f,\"fft\":%u,\"max_w\":%u,\"bands\":[",
                         w->rate, skim_hd_wf_fft_size(w->rate), SKIM_WF_MAX_W);
  for (guint i = 0; i < w->nb; i++) {
    g_string_append_printf(o, "%s{\"name\":\"", i ? "," : "");
    skim_json_append(o, w->names[i]);
    g_string_append_printf(o, "\",\"centre_hz\":%.0f}", w->centres[i]);
  }
  g_string_append(o, "]}");
  soup_websocket_connection_send_text(c, o->str);
  g_string_free(o, TRUE);
  g_message("waterfall: %s connected (%u watching)", v->peer, w->viewers->len);
}

guint skim_hd_waterfall_viewers(SkimHdWaterfall *w) { return w ? w->viewers->len : 0; }

SkimHdWaterfall *skim_hd_waterfall_new(guint nb, const char *const *names,
                                       const double *centres, double rate) {
  SkimHdWaterfall *w = g_new0(SkimHdWaterfall, 1);
  w->nb = nb;
  w->names = g_new0(char *, nb + 1);
  w->centres = g_new(double, nb);
  w->band = g_new0(WfBand, nb);
  for (guint i = 0; i < nb; i++) {
    w->names[i] = g_strdup(names[i]);
    w->centres[i] = centres[i];
    g_mutex_init(&w->band[i].lock);
  }
  w->rate = rate;
  w->viewers = g_ptr_array_new_with_free_func((GDestroyNotify)viewer_free);
  return w;
}

void skim_hd_waterfall_free(SkimHdWaterfall *w) {
  if (!w) { return; }
  g_ptr_array_unref(w->viewers);
  for (guint i = 0; i < w->nb; i++) {
    skim_wf_analyser_free(w->band[i].an);
    g_free(w->band[i].ring);
    g_mutex_clear(&w->band[i].lock);
  }
  g_strfreev(w->names);
  g_free(w->centres);
  g_free(w->band);
  g_free(w);
}
