/* snr_meter.c — see snr_meter.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "snr_meter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A channel's noise: the median of its 32 ms mean powers over the last
 * 4 s. Not the mean: a strong neighbour's keying sidebands splash into the
 * channel at every edge, and a busy band then has no clean channel left by
 * mean power (snr_test: 22 keyed +30 dB neighbours lifted it 14 dB). */
#define WIN_S        0.032
#define RING         128    /* windows: 4 s                                */
#define RING_READY   32     /* windows before a channel counts: 1 s        */
/* median / mean of a noise-only window ring (snr_test, white noise: -0.4 dB) */
#define MEDIAN_BIAS  0.912
#define PEAK_TAU_S   3.0    /* peak hold decay                             */
#define MARK_TAU_S   0.25   /* mark power, over mark samples only          */
#define MARK_REL     0.25   /* a mark sample: within 6 dB of the peak      */
#define FLOOR_HALF   32     /* neighbours each side: ±4 kHz at 125 Hz      */
/* The floor's percentile among them: at least a fifth of the neighbours
 * must be clean — one in four channels free between strong keyed stations
 * is (snr_test); a station every 375 Hz leaves none, and the floor rises. */
#define FLOOR_RANK   0.2

typedef struct {
  float  ma[3];             /* 3-tap MA of |x|² (the decoders' smoothing)  */
  guint  ma_n;
  double peak, mark;
  guint64 seen;
  double win;               /* the window being summed                     */
  guint  win_n;
  float  ring[RING];        /* window mean powers                          */
  guint  ring_n;            /* windows written (ring index = ring_n % RING) */
} SnrCh;

struct _SkimSnrMeter {
  guint   nchan;
  double  to_2500_db;       /* channel noise bandwidth → 2500 Hz           */
  double  a_peak, a_mark;
  guint   win;              /* samples per window                          */
  SnrCh  *ch;
  float  *med;              /* one channel's ring, sorted                  */
  double *scratch;
  double *medv;             /* refresh: every channel's noise              */
  double *floor;            /* refresh: every channel's band floor         */
};

SkimSnrMeter *skim_snr_meter_new(guint nchan, double rate, double enbw_hz) {
  if (nchan == 0 || rate <= 0 || enbw_hz <= 0)
    return NULL;
  SkimSnrMeter *m = g_new0(SkimSnrMeter, 1);
  m->nchan  = nchan;
  m->to_2500_db = 10.0 * log10(enbw_hz / 2500.0);
  m->win    = MAX(1u, (guint)lround(WIN_S * rate));
  m->a_peak = 1.0 / (PEAK_TAU_S * rate);
  m->a_mark = 1.0 / (MARK_TAU_S * rate);
  m->ch = g_new0(SnrCh, nchan);
  m->med = g_new(float, RING);
  m->scratch = g_new(double, 2 * FLOOR_HALF + 1);
  m->medv = g_new(double, nchan);
  m->floor = g_new(double, nchan);
  for (guint c = 0; c < nchan; c++) { m->floor[c] = NAN; }
  return m;
}

void skim_snr_meter_free(SkimSnrMeter *m) {
  if (!m)
    return;
  g_free(m->ch);
  g_free(m->med);
  g_free(m->scratch);
  g_free(m->medv);
  g_free(m->floor);
  g_free(m);
}

void skim_snr_meter_push(SkimSnrMeter *m, guint chan, const float *iq,
                         guint nframes) {
  if (!m || chan >= m->nchan)
    return;
  SnrCh *c = &m->ch[chan];
  for (guint n = 0; n < nframes; n++) {
    const float i = iq[2 * n], q = iq[2 * n + 1];
    c->ma[c->ma_n % 3] = i * i + q * q;
    c->ma_n++;
    const double p = (c->ma[0] + c->ma[1] + c->ma[2]) / (double)MIN(c->ma_n, 3);
    c->win += i * i + q * q;
    if (++c->win_n == m->win) {
      c->ring[c->ring_n++ % RING] = (float)(c->win / m->win);
      c->win = 0;
      c->win_n = 0;
    }
    if (c->seen++ == 0) {
      c->peak = c->mark = p;
      continue;
    }
    c->peak = p > c->peak ? p : c->peak + m->a_peak * (p - c->peak);
    if (p > MARK_REL * c->peak) { c->mark += m->a_mark * (p - c->mark); }
  }
}

static double sel_d(double *v, guint n, guint k) {
  guint lo = 0, hi = n - 1;
  while (lo < hi) {
    const double piv = v[(lo + hi) / 2];
    guint i = lo, j = hi;
    while (i <= j) {
      while (v[i] < piv) { i++; }
      while (v[j] > piv) { j--; }
      if (i <= j) {
        const double x = v[i];
        v[i] = v[j];
        v[j] = x;
        i++;
        if (j == 0) { break; }
        j--;
      }
    }
    if (k <= j) { hi = j; } else if (k >= i) { lo = i; } else { break; }
  }
  return v[k];
}

static float sel_f(float *v, guint n, guint k) {
  guint lo = 0, hi = n - 1;
  while (lo < hi) {
    const float piv = v[(lo + hi) / 2];
    guint i = lo, j = hi;
    while (i <= j) {
      while (v[i] < piv) { i++; }
      while (v[j] > piv) { j--; }
      if (i <= j) {
        const float x = v[i];
        v[i] = v[j];
        v[j] = x;
        i++;
        if (j == 0) { break; }
        j--;
      }
    }
    if (k <= j) { hi = j; } else if (k >= i) { lo = i; } else { break; }
  }
  return v[k];
}

static int cmp_double(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* One channel's noise power: its window median, unbiased. */
static double chan_noise(SkimSnrMeter *m, const SnrCh *c) {
  const guint n = MIN(c->ring_n, RING);
  memcpy(m->med, c->ring, n * sizeof(float));
  return sel_f(m->med, n, n / 2) / MEDIAN_BIAS;
}

double skim_snr_meter_db(SkimSnrMeter *m, guint chan) {
  if (!m || chan >= m->nchan || m->ch[chan].ring_n < RING_READY)
    return NAN;
  /* refreshed floors (a set_floor backend asked for them) are at most a
   * second old: no median over the neighbours per decode event */
  if (!isnan(m->floor[chan]) && m->floor[chan] > 0) {
    const double noise = m->floor[chan];
    const double sig = m->ch[chan].mark - noise;
    return 10.0 * log10(MAX(sig, 1e-3 * noise) / noise) + m->to_2500_db;
  }
  const gint M = (gint)m->nchan;
  const gint half = MIN(FLOOR_HALF, (M - 1) / 2);
  guint k = 0;
  for (gint d = -half; d <= half; d++) {
    const SnrCh *c = &m->ch[((gint)chan + d % M + M) % M];
    if (c->ring_n >= RING_READY) { m->scratch[k++] = chan_noise(m, c); }
  }
  if (k == 0)
    return NAN;
  qsort(m->scratch, k, sizeof(double), cmp_double);
  const double noise = m->scratch[(guint)(FLOOR_RANK * (k - 1))];
  if (noise <= 0)
    return NAN;
  const double sig = m->ch[chan].mark - noise;
  return 10.0 * log10(MAX(sig, 1e-3 * noise) / noise) + m->to_2500_db;
}

void skim_snr_meter_refresh(SkimSnrMeter *m) {
  if (!m)
    return;
  for (guint c = 0; c < m->nchan; c++) {
    m->medv[c] = m->ch[c].ring_n >= RING_READY ? chan_noise(m, &m->ch[c]) : NAN;
  }
  const gint M = (gint)m->nchan;
  const gint half = MIN(FLOOR_HALF, (M - 1) / 2);
  for (guint c = 0; c < m->nchan; c++) {
    guint k = 0;
    for (gint d = -half; d <= half; d++) {
      const double v = m->medv[((gint)c + d % M + M) % M];
      if (!isnan(v)) { m->scratch[k++] = v; }
    }
    if (k == 0) {
      m->floor[c] = NAN;
      continue;
    }
    m->floor[c] = sel_d(m->scratch, k, (guint)(FLOOR_RANK * (k - 1)));
  }
}

double skim_snr_meter_noise(const SkimSnrMeter *m, guint chan) {
  return m && chan < m->nchan ? m->floor[chan] : NAN;
}

double skim_snr_esn0_dot(double snr2500_db, double wpm) {
  return wpm > 0 ? snr2500_db + 10.0 * log10(2500.0 * 1.2 / wpm) : NAN;
}
