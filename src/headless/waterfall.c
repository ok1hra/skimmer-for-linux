/* waterfall.c — see waterfall.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "waterfall.h"

#include <fftw3.h>
#include <math.h>
#include <string.h>

#include "engine/fftw_lock.h"
#include "json.h"

#define RING 32                        /* frames kept a band (~1.4 s)        */
#define HIST_ROWS 1200                 /* the band's history: 4 min …         */
#define HIST_S    0.2                  /* … a row every 0.2 s                */

/* WDSP (vendor/wdsp): its headers need the GNU dialect, so the calls used
 * are declared here (RESAMPLE is a pointer to its struct). */
extern void *create_resample(int run, int size, double *in, double *out, int in_rate,
                             int out_rate, double fc, int ncoef, double gain);
extern void  destroy_resample(void *a);
extern int   xresample(void *a);

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

/* The same from a history row: n bytes, display order (lowest first). */
void skim_hd_wf_render_bytes(const guint8 *disp, guint n, double rate, double centre, double lo,
                             double hi, guint w, guint8 *out) {
  const double bw = rate / n, px = (hi - lo) / w;
  for (guint p = 0; p < w; p++) {
    const double f0 = lo + p * px, f1 = f0 + px;
    long d0 = lround(floor((f0 - centre) / bw)) + (long)n / 2;
    long d1 = lround(ceil((f1 - centre) / bw)) + (long)n / 2 - 1;
    if (d1 < d0) { d1 = d0; }
    if (d1 < 0 || d0 >= (long)n) { out[p] = 0; continue; }
    d0 = MAX(d0, 0);
    d1 = MIN(d1, (long)n - 1);
    guint8 m = 0;
    for (long d = d0; d <= d1; d++) { m = MAX(m, disp[d]); }
    out[p] = m;
  }
}

/* ---- the bands and the viewers ------------------------------------------------ */

/* ---- the audio --------------------------------------------------------------- */

/* One listener's demodulator (the source thread runs it, under the band's
 * lock): the band shifted so the tuned frequency sits at 0, WDSP's resampler
 * down to 8 kHz, a second one at 8 kHz as the narrow filter (±bw/2), the
 * BFO at SKIM_WF_AUDIO_BFO, an AGC, G.711 µ-law out. */
struct _SkimWfAudio {
  void   *dec, *nar;                   /* WDSP RESAMPLE: rate → 8 k, the filter */
  double *blk, *mid, *out;             /* 10 ms: rate/100 in, 80 at 8 kHz       */
  guint   blk_n, fill;
  double  offset, rate;                /* tuned − centre (Hz), the IQ rate      */
  double  nco, bfo;                    /* phases (rad)                          */
  double  agc;                         /* the level followed                    */
  double  pmax;                        /* the loudest 10 ms since the last take  */
  GByteArray *pcm;                     /* µ-law waiting for the main loop       */
};
typedef SkimWfAudio Audio;

static guint8 ulaw(double x) {                     /* G.711 µ-law, x in ±1   */
  int s = (int)CLAMP(x * 32767.0, -32767, 32767);
  const int sign = s < 0 ? 0x80 : 0;
  if (s < 0) { s = -s; }
  s = MIN(s + 0x84, 0x7fff);
  int exp = 7;
  for (int m = 0x4000; !(s & m) && exp > 0; m >>= 1) { exp--; }
  const int mant = (s >> (exp + 3)) & 0x0f;
  return (guint8)~(sign | (exp << 4) | mant);
}

static Audio *audio_new(double rate, double offset, double bw) {
  Audio *a = g_new0(Audio, 1);
  a->rate = rate;
  a->offset = offset;
  a->blk_n = (guint)(rate / 100);
  a->blk = g_new0(double, 2 * a->blk_n);
  a->mid = g_new0(double, 2 * 80);
  a->out = g_new0(double, 2 * 80);
  /* to 8 kHz: only what would fold into the passband (around ±8 kHz) must
   * go, so a short filter does (240 taps: the stopband from ~6 kHz) */
  a->dec = create_resample(1, (int)a->blk_n, a->blk, a->mid, (int)rate, SKIM_WF_AUDIO_RATE, 3500, 240, 1.0);
  /* the narrow filter: a transition about half the passband wide */
  const int taps = CLAMP((int)(13.2 * SKIM_WF_AUDIO_RATE / bw), 64, 2400);
  a->nar = create_resample(1, 80, a->mid, a->out, SKIM_WF_AUDIO_RATE, SKIM_WF_AUDIO_RATE, bw / 2, taps, 1.0);
  a->agc = 1e-4;
  a->pcm = g_byte_array_new();
  return a;
}

static void audio_free(Audio *a) {
  if (!a) { return; }
  destroy_resample(a->dec);
  destroy_resample(a->nar);
  g_free(a->blk);
  g_free(a->mid);
  g_free(a->out);
  g_byte_array_unref(a->pcm);
  g_free(a);
}

static void audio_feed(Audio *a, const float *iq, guint n) {
  const double step = -2 * G_PI * a->offset / a->rate;
  const double bstep = 2 * G_PI * SKIM_WF_AUDIO_BFO / SKIM_WF_AUDIO_RATE;
  for (guint i = 0; i < n; i++) {
    const double c = cos(a->nco), s = sin(a->nco);
    const double re = iq[2 * i], im = iq[2 * i + 1];
    a->blk[2 * a->fill] = re * c - im * s;
    a->blk[2 * a->fill + 1] = re * s + im * c;
    a->nco = fmod(a->nco + step, 2 * G_PI);
    if (++a->fill < a->blk_n) { continue; }
    a->fill = 0;
    const int m = xresample(a->dec);
    if (m != 80) { continue; }
    xresample(a->nar);
    guint8 u[80];
    double p = 0;
    for (int k = 0; k < 80; k++) {
      const double I = a->out[2 * k], Q = a->out[2 * k + 1];
      p += I * I + Q * Q;
      const double y = I * cos(a->bfo) - Q * sin(a->bfo);   /* up to the BFO tone */
      a->bfo = fmod(a->bfo + bstep, 2 * G_PI);
      const double mag = sqrt(I * I + Q * Q);
      /* AGC: fast up (2 ms), slow down (~1.5 s), the floor keeps noise quiet */
      a->agc = mag > a->agc ? a->agc + (mag - a->agc) * 0.06 : a->agc * 0.99992;
      a->agc = MAX(a->agc, 1e-7);
      u[k] = ulaw(0.35 * y / a->agc);
    }
    a->pmax = MAX(a->pmax, p / 80);                 /* the S-meter: before the AGC */
    if (a->pcm->len < SKIM_WF_AUDIO_RATE * 2) { g_byte_array_append(a->pcm, u, 80); }  /* ≤ 2 s */
  }
}

SkimWfAudio *skim_wf_audio_new(double rate, double offset, double bw) {
  return audio_new(rate, offset, bw);
}
void skim_wf_audio_free(SkimWfAudio *a) { audio_free(a); }
void skim_wf_audio_feed(SkimWfAudio *a, const float *iq, guint n) { audio_feed(a, iq, n); }
static double level_take(Audio *a) {
  const double db = a->pmax > 1e-20 ? 10 * log10(a->pmax) : -200;
  a->pmax = 0;
  return db;
}
double skim_wf_audio_level(SkimWfAudio *a) { return level_take(a); }
GByteArray *skim_wf_audio_take(SkimWfAudio *a) {
  GByteArray *p = a->pcm;
  a->pcm = g_byte_array_new();
  return p;
}

typedef struct {
  GMutex          lock;
  GPtrArray      *audio;               /* Audio* of its listeners            */
  gint            nsubs;               /* atomic: viewers on it              */
  SkimWfAnalyser *an;                  /* while watched                      */
  guint           n;
  float          *ring;                /* RING × n                           */
  guint64         seq;                 /* frames made                        */
  double          centre;
  /* the history: a full-resolution row every HIST_S (bytes as a row's,
   * display order), so a viewer that zooms or moves gets its picture again
   * sharp at the new window instead of a stretched one */
  guint8         *hist;                /* HIST_ROWS × n                      */
  double         *hist_t;              /* unix s of each                     */
  guint64         hist_seq;
  float          *hacc;                /* the frames summed toward the next  */
  guint           hacc_n, hacc_need;
} WfBand;

struct _SkimHdWaterfall {
  guint      nb;
  char     **names;
  double    *centres;
  double     rate;
  WfBand    *band;
  GPtrArray *viewers;                  /* Viewer*                            */
  SkimWfViewCb view_cb;                /* what a signed-in visitor watches   */
  gpointer   view_user;
};

typedef struct {
  SkimHdWaterfall         *w;
  SoupWebsocketConnection *c;
  char                    *peer;
  char                    *visitor;   /* its sign-in token; NULL: free       */
  int                      band;      /* -1: none yet                        */
  double                   lo, hi;
  guint                    width, rate, timer;
  guint64                  frame_seq; /* the band's frames taken             */
  guint32                  row_seq, acked;
  float                   *avg;
  guint                    avg_n;
  guint8                  *row;
  Audio                   *au;        /* listening (in its band's list)      */
  double                   af, bw;    /* the tuned frequency, the passband   */
  guint                    audio_timer;
  guint32                  aseq;
} Viewer;

static void frame_cb(const float *power, guint n, gpointer user) {
  WfBand *b = user;                                   /* under b->lock      */
  memcpy(b->ring + (b->seq % RING) * n, power, n * sizeof(float));
  b->seq++;
  for (guint k = 0; k < n; k++) { b->hacc[k] += power[k]; }
  if (++b->hacc_n < b->hacc_need) { return; }
  guint8 *row = b->hist + (b->hist_seq % HIST_ROWS) * n;
  for (guint d = 0; d < n; d++) {                     /* display order      */
    const float v = b->hacc[(d + n / 2) % n] / b->hacc_n;
    const double db = v > 0 ? 10 * log10(v) : -300;
    row[d] = (guint8)CLAMP(lround((db - SKIM_WF_DB_FLOOR) * 2), 0, 255);
  }
  b->hist_t[b->hist_seq % HIST_ROWS] = g_get_real_time() / 1e6;
  b->hist_seq++;
  memset(b->hacc, 0, n * sizeof(float));
  b->hacc_n = 0;
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
  for (guint k = 0; b->audio && k < b->audio->len; k++) { audio_feed(b->audio->pdata[k], iq, n); }
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
    b->hist = g_new0(guint8, (gsize)HIST_ROWS * b->n);
    b->hist_t = g_new0(double, HIST_ROWS);
    b->hist_seq = 0;
    b->hacc = g_new0(float, b->n);
    b->hacc_n = 0;
    /* frames come every n/2 samples */
    b->hacc_need = MAX(1u, (guint)lround(HIST_S * v->w->rate / (b->n / 2)));
  }
  v->frame_seq = b->seq;
  g_atomic_int_inc(&b->nsubs);
  g_mutex_unlock(&b->lock);
  v->band = bi;
}

static guint listeners(SkimHdWaterfall *w);

/* The viewer's audio off: out of its band's list, freed. */
static void audio_stop(Viewer *v) {
  if (v->audio_timer) { g_source_remove(v->audio_timer); v->audio_timer = 0; }
  if (!v->au) { return; }
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  g_ptr_array_remove(b->audio, v->au);
  g_mutex_unlock(&b->lock);
  audio_free(v->au);
  v->au = NULL;
}

/* What was demodulated since the last time, to the viewer (every 100 ms):
 * u8 version (2), u8 band, u16 n, u32 seq, then n µ-law bytes at 8 kHz. */
static gboolean audio_tick(gpointer user) {
  Viewer *v = user;
  if (!v->au) { return G_SOURCE_CONTINUE; }
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  GByteArray *pcm = v->au->pcm;
  v->au->pcm = g_byte_array_new();
  const float lv = (float)level_take(v->au);
  g_mutex_unlock(&b->lock);
  if (pcm->len) {
    const guint n = MIN(pcm->len, 65535u);
    guint8 *msg = g_malloc(8 + n + 4);
    const guint16 nl = GUINT16_TO_LE((guint16)n);
    const guint32 sq = GUINT32_TO_LE(++v->aseq);
    msg[0] = 2;
    msg[1] = (guint8)v->band;
    memcpy(msg + 2, &nl, 2);
    memcpy(msg + 4, &sq, 4);
    memcpy(msg + 8, pcm->data + pcm->len - n, n);
    guint32 lb;
    memcpy(&lb, &lv, 4);
    lb = GUINT32_TO_LE(lb);
    memcpy(msg + 8 + n, &lb, 4);
    soup_websocket_connection_send_binary(v->c, msg, 8 + n + 4);
    g_free(msg);
  }
  g_byte_array_unref(pcm);
  return G_SOURCE_CONTINUE;
}

/* The viewer listens at v->af with passband v->bw (a new tuning restarts it). */
static void audio_start(Viewer *v) {
  audio_stop(v);
  if (v->band < 0) { return; }
  if (listeners(v->w) >= SKIM_WF_MAX_LISTENERS) {
    soup_websocket_connection_send_text(v->c, "{\"type\":\"audio\",\"error\":\"too many listeners — try again later\"}");
    return;
  }
  WfBand *b = &v->w->band[v->band];
  v->au = audio_new(v->w->rate, v->af - v->w->centres[v->band], v->bw);
  g_mutex_lock(&b->lock);
  if (!b->audio) { b->audio = g_ptr_array_new(); }
  g_ptr_array_add(b->audio, v->au);
  g_mutex_unlock(&b->lock);
  v->audio_timer = g_timeout_add(100, audio_tick, v);
}

static void detach(Viewer *v) {
  if (v->band < 0) { return; }
  audio_stop(v);
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  if (g_atomic_int_dec_and_test(&b->nsubs)) {         /* the last one: stop */
    g_clear_pointer(&b->an, skim_wf_analyser_free);
    g_clear_pointer(&b->ring, g_free);
    g_clear_pointer(&b->hist, g_free);
    g_clear_pointer(&b->hist_t, g_free);
    g_clear_pointer(&b->hacc, g_free);
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

/* The band's history (up to `rows` rows of the viewer's, newest first) at
 * its window and width, in one message: u8 version (3), u8 band, u16 w,
 * u32 count, f64 lo, f64 hi, then count × (f64 unix time, w bytes). */
static void send_history(Viewer *v, guint rows) {
  if (v->band < 0 || !v->width || !rows) { return; }
  WfBand *b = &v->w->band[v->band];
  g_mutex_lock(&b->lock);
  if (!b->hist || !b->hist_seq) { g_mutex_unlock(&b->lock); return; }
  /* the time the viewer's rows cover, in history rows */
  const guint want = (guint)MIN((guint64)ceil(rows / (double)v->rate / HIST_S) + 1, (guint64)HIST_ROWS);
  const guint count = (guint)MIN((guint64)want, b->hist_seq);
  const guint n = b->n;
  guint8 *copy = g_malloc((gsize)count * n);
  double *ts = g_new(double, count);
  for (guint i = 0; i < count; i++) {                 /* newest first       */
    const guint64 s = b->hist_seq - 1 - i;
    memcpy(copy + (gsize)i * n, b->hist + (s % HIST_ROWS) * n, n);
    ts[i] = b->hist_t[s % HIST_ROWS];
  }
  const double centre = b->centre;
  g_mutex_unlock(&b->lock);
  const gsize len = 24 + (gsize)count * (8 + v->width);
  guint8 *msg = g_malloc(len);
  const guint16 wl = GUINT16_TO_LE((guint16)v->width);
  const guint32 cl = GUINT32_TO_LE(count);
  msg[0] = 3;
  msg[1] = (guint8)v->band;
  memcpy(msg + 2, &wl, 2);
  memcpy(msg + 4, &cl, 4);
  memcpy(msg + 8, &v->lo, 8);
  memcpy(msg + 16, &v->hi, 8);
  for (guint i = 0; i < count; i++) {
    guint8 *r = msg + 24 + (gsize)i * (8 + v->width);
    memcpy(r, &ts[i], 8);
    skim_hd_wf_render_bytes(copy + (gsize)i * n, n, v->w->rate, centre, v->lo, v->hi, v->width, r + 8);
  }
  soup_websocket_connection_send_binary(v->c, msg, len);
  g_free(msg);
  g_free(ts);
  g_free(copy);
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
  /* audio=1&af=HZ&bw=HZ: listen there (in the band, 50–500 Hz); audio=0: stop */
  const char *sa = g_hash_table_lookup(f, "audio");
  if (sa && v->band >= 0) {
    const char *saf = g_hash_table_lookup(f, "af"), *sbw = g_hash_table_lookup(f, "bw");
    const double c = v->w->centres[v->band], half = v->w->rate / 2;
    const double af = saf ? g_ascii_strtod(saf, NULL) : v->af;
    const double bw = sbw ? CLAMP(g_ascii_strtod(sbw, NULL), 50, 500) : (v->bw ? v->bw : 250);
    if (strcmp(sa, "1") == 0 && af > c - half && af < c + half) {
      if (!v->au || af != v->af || bw != v->bw) {
        v->af = af;
        v->bw = bw;
        audio_start(v);
      }
    } else {
      audio_stop(v);
    }
  }
  if (v->band >= 0 && v->hi <= v->lo) {               /* the whole band     */
    v->lo = v->w->centres[v->band] - v->w->rate / 2;
    v->hi = v->w->centres[v->band] + v->w->rate / 2;
  }
  v->acked = v->row_seq;                              /* a fresh start      */
  if (v->visitor && v->w->view_cb && v->band >= 0) {   /* for the visitors' log */
    v->w->view_cb(v->visitor, v, v->w->names[v->band], v->lo, v->hi, v->au != NULL, v->af,
                  v->w->view_user);
  }
  /* hist=ROWS: the picture so far, sharp at the new window */
  const char *sh2 = g_hash_table_lookup(f, "hist");
  if (sh2) { send_history(v, (guint)CLAMP(g_ascii_strtoll(sh2, NULL, 10), 0, 4000)); }
  g_hash_table_unref(f);
}

static void viewer_free(Viewer *v) {
  detach(v);                                          /* the audio with it  */
  if (v->timer) { g_source_remove(v->timer); }
  g_signal_handlers_disconnect_by_data(v->c, v);
  g_object_unref(v->c);
  g_free(v->avg);
  g_free(v->row);
  g_free(v->peer);
  g_free(v->visitor);
  g_free(v);
}

static void on_closed(SoupWebsocketConnection *c, gpointer user) {
  (void)c;
  Viewer *v = user;
  g_message("waterfall: %s left", v->peer);
  if (v->visitor && v->w->view_cb) { v->w->view_cb(v->visitor, v, NULL, 0, 0, FALSE, 0, v->w->view_user); }
  g_ptr_array_remove(v->w->viewers, v);               /* frees it           */
}

void skim_hd_waterfall_set_view_cb(SkimHdWaterfall *w, SkimWfViewCb cb, gpointer user) {
  w->view_cb = cb;
  w->view_user = user;
}

void skim_hd_waterfall_add(SkimHdWaterfall *w, SoupWebsocketConnection *c, const char *peer,
                           const char *visitor) {
  if (w->viewers->len >= SKIM_WF_MAX_VIEWERS) {
    soup_websocket_connection_close(c, 1013, "too many waterfall viewers — try again later");
    return;
  }
  Viewer *v = g_new0(Viewer, 1);
  v->w = w;
  v->c = g_object_ref(c);
  v->peer = g_strdup(peer ? peer : "a viewer");
  v->visitor = g_strdup(visitor);
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

static guint listeners(SkimHdWaterfall *w) {
  guint n = 0;
  for (guint i = 0; i < w->viewers->len; i++) { n += ((Viewer *)w->viewers->pdata[i])->au != NULL; }
  return n;
}
guint skim_hd_waterfall_listeners(SkimHdWaterfall *w) { return w ? listeners(w) : 0; }

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
    g_free(w->band[i].hist);
    g_free(w->band[i].hist_t);
    g_free(w->band[i].hacc);
    if (w->band[i].audio) { g_ptr_array_unref(w->band[i].audio); }
    g_mutex_clear(&w->band[i].lock);
  }
  g_strfreev(w->names);
  g_free(w->centres);
  g_free(w->band);
  g_free(w);
}
