/* digi_rx.c — one dial's 12 kHz USB audio cut into UTC periods (digi_rx.h).
 * Lifted out of ft8.c unchanged for one period (FT8's output stays byte for
 * byte), so JS8's four periods share one front end.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "digi_rx.h"

#include <math.h>
#include <string.h>

/* WDSP (vendor/wdsp): the resampler, as wspr.c and waterfall.c use it */
extern void *create_resample(int run, int size, double *in, double *out, int in_rate,
                             int out_rate, double fc, int ncoef, double gain);
extern void  destroy_resample(void *a);
extern int   xresample(void *a);

#define TAPS1   240                     /* rate → 12 k: only ±12 k multiples fold */
#define FC1     4500.0
#define TAPS2   800                     /* 12 k → 12 k: the window, ~120 Hz edges */
#define EDGE2   60.0                    /* the sharp filter's cut beyond the window */
#define PCM_RMS 1500.0                  /* the WAV's level (the decoders normalise) */

typedef struct {
  gboolean capturing;
  gint64   cap_cycle, done_cycle;       /* in periods since the epoch            */
  double   period_us;
  float   *cap;
  guint    cap_n, cap_len;
} Period;

struct _SkimDigiRx {
  double  rate, offset, mid;            /* dial + mid − centre; mid above the dial */
  void   *r1, *r2;
  double *b1, *m1, *m2;
  guint   blk_n, fill, mid_n;
  double  ph_re, ph_im, st_re, st_im;   /* the shift's phasor and its step       */
  double  up_re, up_im, us_re, us_im;   /* … and the one back up (12 kHz)        */
  double  delay_us;                     /* the two filters' group delay          */
  gboolean anchored;                    /* the clock, as wspr.c                  */
  double  anchor_us;
  guint64 frames;
  Period  per[SKIM_DIGI_MAX_PERIODS];
  guint   nper;
  SkimDigiCycleCb cb;
  gpointer user;
};

SkimDigiRx *skim_digi_rx_new(double rate, double centre, double dial_hz, double lo_hz,
                             double hi_hz, const guint *period_ms, guint nperiods,
                             SkimDigiCycleCb cb, gpointer user) {
  SkimDigiRx *rx = g_new0(SkimDigiRx, 1);
  rx->rate = rate;
  rx->mid = (lo_hz + hi_hz) / 2;
  rx->offset = dial_hz + rx->mid - centre;
  rx->blk_n = (guint)(rate / 100);                       /* 10 ms                */
  rx->mid_n = SKIM_DIGI_RATE / 100;                      /* 120                  */
  rx->b1 = g_new0(double, 2 * rx->blk_n);
  rx->m1 = g_new0(double, 2 * (rx->mid_n + 8));
  rx->m2 = g_new0(double, 2 * (rx->mid_n + 8));
  rx->r1 = create_resample(1, (int)rx->blk_n, rx->b1, rx->m1, (int)rate, SKIM_DIGI_RATE, FC1,
                           TAPS1, 1.0);
  /* flat over the window, gone before the dial (whose mirror it would be) */
  rx->r2 = create_resample(1, (int)rx->mid_n, rx->m1, rx->m2, SKIM_DIGI_RATE, SKIM_DIGI_RATE,
                           (hi_hz - lo_hz) / 2 + EDGE2, TAPS2, 1.0);
  rx->ph_re = 1;
  const double w = -2 * G_PI * rx->offset / rate;
  rx->st_re = cos(w);
  rx->st_im = sin(w);
  rx->up_re = 1;
  const double u = 2 * G_PI * rx->mid / SKIM_DIGI_RATE;
  rx->us_re = cos(u);
  rx->us_im = sin(u);
  rx->delay_us = 1e6 * ((TAPS1 / 2.0) / rate + (TAPS2 / 2.0) / SKIM_DIGI_RATE);
  rx->nper = MIN(nperiods, SKIM_DIGI_MAX_PERIODS);
  for (guint k = 0; k < rx->nper; k++) {
    Period *p = &rx->per[k];
    p->done_cycle = G_MININT64;
    p->period_us = period_ms[k] * 1000.0;
    p->cap_len = period_ms[k] * (SKIM_DIGI_RATE / 1000);
    p->cap = g_new0(float, p->cap_len);
  }
  rx->cb = cb;
  rx->user = user;
  return rx;
}

void skim_digi_rx_free(SkimDigiRx *rx) {
  if (!rx) { return; }
  destroy_resample(rx->r1);
  destroy_resample(rx->r2);
  g_free(rx->b1);
  g_free(rx->m1);
  g_free(rx->m2);
  for (guint k = 0; k < rx->nper; k++) { g_free(rx->per[k].cap); }
  g_free(rx);
}

static void emit(SkimDigiRx *rx, guint k) {
  Period *p = &rx->per[k];
  double e = 0;
  for (guint i = 0; i < p->cap_n; i++) { e += (double)p->cap[i] * p->cap[i]; }
  const double rms = p->cap_n ? sqrt(e / p->cap_n) : 0;
  const double g = rms > 0 ? PCM_RMS / rms : 1.0;
  gint16 *pcm = g_new(gint16, p->cap_len);
  for (guint i = 0; i < p->cap_len; i++) {
    pcm[i] = (gint16)CLAMP(lround(p->cap[i] * g), -32767, 32767);
  }
  p->done_cycle = p->cap_cycle;
  p->capturing = FALSE;
  rx->cb(k, (gint64)llround(p->cap_cycle * p->period_us / 1000.0), pcm, p->cap_len, rx->user);
}

static void take(SkimDigiRx *rx, double t_us, double a) {
  for (guint k = 0; k < rx->nper; k++) {
    Period *p = &rx->per[k];
    const gint64 cyc = (gint64)floor(t_us / p->period_us);
    if (!p->capturing) {
      const double pos = t_us - (double)cyc * p->period_us;
      if (cyc == p->done_cycle || pos < 0 || pos >= 0.5e6) { continue; }
      p->capturing = TRUE;
      p->cap_cycle = cyc;
      p->cap_n = (guint)lround(pos * SKIM_DIGI_RATE / 1e6);   /* zeros: the late start */
      memset(p->cap, 0, sizeof(float) * p->cap_len);
    }
    p->cap[p->cap_n] = (float)a;
    if (++p->cap_n >= p->cap_len) { emit(rx, k); }
  }
}

void skim_digi_rx_feed(SkimDigiRx *rx, const float *iq, guint n, gint64 t_end_us) {
  if (!n) { return; }
  /* the clock exactly as wspr.c's: a gap of a second or more starts it
   * again (and drops the captures), else the anchor follows the earliest
   * arrivals fast and the late ones slowly */
  const double t_meas = (double)t_end_us - (n - 1) * 1e6 / rx->rate;
  const double t_pred = rx->anchor_us + rx->frames * 1e6 / rx->rate;
  if (!rx->anchored || fabs(t_meas - t_pred) > 1e6) {
    rx->anchored = TRUE;
    rx->anchor_us = t_meas;
    rx->frames = 0;
    for (guint k = 0; k < rx->nper; k++) { rx->per[k].capturing = FALSE; }
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
    double norm = 1.0 / hypot(rx->ph_re, rx->ph_im);       /* the phasor stays on the circle */
    rx->ph_re *= norm;
    rx->ph_im *= norm;
    if (xresample(rx->r1) != (int)rx->mid_n) { continue; }
    const int m = xresample(rx->r2);
    const double t_blk = rx->anchor_us + (rx->frames - 1) * 1e6 / rx->rate - rx->delay_us;
    for (int j = 0; j < m; j++) {
      /* back up by the window's middle: the real part is the USB audio */
      const double a = rx->m2[2 * j] * rx->up_re - rx->m2[2 * j + 1] * rx->up_im;
      const double ur = rx->up_re * rx->us_re - rx->up_im * rx->us_im;
      rx->up_im = rx->up_re * rx->us_im + rx->up_im * rx->us_re;
      rx->up_re = ur;
      take(rx, t_blk - (m - 1 - j) * 1e6 / SKIM_DIGI_RATE, a);
    }
    norm = 1.0 / hypot(rx->up_re, rx->up_im);
    rx->up_re *= norm;
    rx->up_im *= norm;
  }
}

char *skim_digi_write_wav(const char *dir, gint64 start_ms, const gint16 *pcm, guint n,
                          GError **error) {
  GDateTime *dt = g_date_time_new_from_unix_utc(start_ms / 1000);
  char *name = g_date_time_format(dt, "%y%m%d_%H%M%S.wav");
  g_date_time_unref(dt);
  char *path = g_build_filename(dir, name, NULL);
  g_free(name);
  const guint32 data = n * 2, riff = 36 + data, fmt = 16, rate = SKIM_DIGI_RATE,
                byte_rate = SKIM_DIGI_RATE * 2;
  const guint16 pcm_fmt = 1, ch = 1, align = 2, bits = 16;
  const gsize len = 44 + data;
  guint8 *buf = g_malloc(len), *p = buf;
#define PUT(src, k) do { memcpy(p, (src), (k)); p += (k); } while (0)
  PUT("RIFF", 4); PUT(&riff, 4); PUT("WAVEfmt ", 8); PUT(&fmt, 4); PUT(&pcm_fmt, 2);
  PUT(&ch, 2); PUT(&rate, 4); PUT(&byte_rate, 4); PUT(&align, 2); PUT(&bits, 2);
  PUT("data", 4); PUT(&data, 4); PUT(pcm, data);
#undef PUT
  const gboolean ok = g_file_set_contents(path, (const char *)buf, (gssize)len, error);
  g_free(buf);
  if (!ok) { g_clear_pointer(&path, g_free); }
  return path;
}
