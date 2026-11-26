/* phase_meter.c — see phase_meter.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "phase_meter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A station counts once the combination or the better antenna reads it
 * this far over its floor. The combination is enough: a station under the
 * noise on each antenna and clear on the output is the gain that matters
 * most — its single readings then sit a dB or two high (snr_test: mark
 * power picked from noise near -10 dB), so its G reads low, never high. */
#define MIN_SNR_DB  0.0

struct _SkimPhaseMeter {
  guint         nchan;
  SkimSnrMeter *snr[SKIM_PH_N];
  double       *gain, *gain_a, *drop, *gain_est, *drop_est;
  double       *scratch;
  SkimPhaseSummary sum;
};

SkimPhaseMeter *skim_phase_meter_new(guint nchan, double rate, double enbw_hz) {
  if (nchan == 0)
    return NULL;
  SkimPhaseMeter *m = g_new0(SkimPhaseMeter, 1);
  m->nchan = nchan;
  for (guint s = 0; s < SKIM_PH_N; s++) {
    m->snr[s] = skim_snr_meter_new(nchan, rate, enbw_hz);
    if (!m->snr[s]) {
      skim_phase_meter_free(m);
      return NULL;
    }
  }
  m->gain = g_new(double, nchan);
  m->gain_a = g_new(double, nchan);
  m->drop = g_new(double, nchan);
  m->gain_est = g_new(double, nchan);
  m->drop_est = g_new(double, nchan);
  m->scratch = g_new(double, nchan);
  for (guint k = 0; k < nchan; k++) {
    m->gain[k] = m->gain_a[k] = m->drop[k] = m->gain_est[k] = m->drop_est[k] = NAN;
  }
  memset(&m->sum, 0, sizeof(m->sum));
  m->sum.gain_db = m->sum.gain_a_db = m->sum.gain_p10_db = m->sum.gain_p90_db = NAN;
  m->sum.drop_db = m->sum.drop_p90_db = m->sum.drop_share = NAN;
  m->sum.coherence = m->sum.dmax_db = m->sum.share_b = NAN;
  m->sum.gain_est_db = m->sum.drop_est_db = m->sum.same_dir = NAN;
  return m;
}

void skim_phase_meter_free(SkimPhaseMeter *m) {
  if (!m)
    return;
  for (guint s = 0; s < SKIM_PH_N; s++) { skim_snr_meter_free(m->snr[s]); }
  g_free(m->gain);
  g_free(m->gain_a);
  g_free(m->drop);
  g_free(m->gain_est);
  g_free(m->drop_est);
  g_free(m->scratch);
  g_free(m);
}

void skim_phase_meter_push(SkimPhaseMeter *m, guint chan, const float *a,
                           const float *b, const float *y, guint n) {
  if (!m)
    return;
  skim_snr_meter_push(m->snr[SKIM_PH_A], chan, a, n);
  skim_snr_meter_push(m->snr[SKIM_PH_B], chan, b, n);
  skim_snr_meter_push(m->snr[SKIM_PH_Y], chan, y, n);
}

static int cmp_d(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* The q-quantile of v[0..n) (sorts it); NAN when empty. */
static double quantile(double *v, guint n, double q) {
  if (n == 0)
    return NAN;
  qsort(v, n, sizeof(double), cmp_d);
  const double x = q * (n - 1);
  const guint i = (guint)floor(x);
  const double t = x - i;
  return i + 1 < n ? v[i] + t * (v[i + 1] - v[i]) : v[i];
}

void skim_phase_meter_refresh(SkimPhaseMeter *m, const SkimCombiner *c) {
  if (!m || !c)
    return;
  for (guint s = 0; s < SKIM_PH_N; s++) { skim_snr_meter_refresh(m->snr[s]); }
  guint ns = 0, nb = 0, nsame = 0, nal = 0;
  for (guint k = 0; k < m->nchan; k++) {
    SkimCombinerChanInfo info;
    m->gain[k] = m->gain_a[k] = m->drop[k] = m->gain_est[k] = m->drop_est[k] = NAN;
    if (!skim_combiner_chan_info(c, k, &info))
      continue;
    if (info.steered) {
      m->gain_est[k] = info.gain_est_db;
      if (!isnan(info.align)) {
        nal++;
        if (info.align > 0.99) { nsame++; }
      }
    } else {
      m->drop_est[k] = info.drop_est_db;
    }
    const double sa = skim_snr_meter_db(m->snr[SKIM_PH_A], k);
    const double sb = skim_snr_meter_db(m->snr[SKIM_PH_B], k);
    const double sy = skim_snr_meter_db(m->snr[SKIM_PH_Y], k);
    const double best = isnan(sb) || sa >= sb ? sa : sb;
    if (info.steered && !isnan(sy) && !isnan(best) && MAX(sy, best) >= MIN_SNR_DB) {
      m->gain[k] = sy - best;
      m->gain_a[k] = isnan(sa) ? NAN : sy - sa;
      ns++;
      if (info.share_b > 0.5) { nb++; }
    } else {
      m->drop[k] = info.drop_db;
    }
  }

  SkimPhaseSummary *o = &m->sum;
  guint n = 0;
  for (guint k = 0; k < m->nchan; k++) {
    if (!isnan(m->gain[k])) { m->scratch[n++] = m->gain[k]; }
  }
  o->stations = n;
  o->gain_db = quantile(m->scratch, n, 0.5);
  o->gain_p10_db = n ? m->scratch[(guint)floor(0.1 * (n - 1))] : NAN;
  o->gain_p90_db = n ? m->scratch[(guint)ceil(0.9 * (n - 1))] : NAN;
  n = 0;
  for (guint k = 0; k < m->nchan; k++) {
    if (!isnan(m->gain_a[k])) { m->scratch[n++] = m->gain_a[k]; }
  }
  o->gain_a_db = quantile(m->scratch, n, 0.5);
  o->share_b = ns ? (double)nb / ns : NAN;
  o->same_dir = nal ? (double)nsame / nal : NAN;
  n = 0;
  for (guint k = 0; k < m->nchan; k++) {
    if (!isnan(m->gain_est[k])) { m->scratch[n++] = m->gain_est[k]; }
  }
  o->gain_est_db = quantile(m->scratch, n, 0.5);
  n = 0;
  for (guint k = 0; k < m->nchan; k++) {
    if (!isnan(m->drop_est[k])) { m->scratch[n++] = m->drop_est[k]; }
  }
  o->drop_est_db = quantile(m->scratch, n, 0.5);
  n = 0;
  for (guint k = 0; k < m->nchan; k++) {
    if (!isnan(m->drop[k])) { m->scratch[n++] = m->drop[k]; }
  }
  o->quiet = n;
  o->drop_db = quantile(m->scratch, n, 0.5);
  o->drop_p90_db = n ? m->scratch[(guint)ceil(0.9 * (n - 1))] : NAN;

  /* per group: the median drop (≥ 3 dB counts), |γ|², D_max */
  const guint ng = skim_combiner_groups(c);
  double *gd = g_new(double, ng), *coh = g_new(double, ng), *dmax = g_new(double, ng);
  guint ngd = 0, nc = 0, groups_open = 0;
  for (guint g = 0; g < ng; g++) {
    guint k2 = 0;
    for (guint k = 0; k < m->nchan; k++) {
      if (skim_combiner_group_of(c, k) == g && !isnan(m->drop[k])) {
        m->scratch[k2++] = m->drop[k];
      }
    }
    if (k2) {
      gd[ngd] = quantile(m->scratch, k2, 0.5);
      if (gd[ngd] >= 3.0) { groups_open++; }
      ngd++;
    }
    const double y = skim_combiner_coherence(c, g);
    if (!isnan(y)) {
      coh[nc] = y;
      dmax[nc] = -10.0 * log10(MAX(1e-6, 1.0 - y));
      nc++;
    }
  }
  o->drop_share = ngd ? (double)groups_open / ngd : NAN;
  o->coherence = quantile(coh, nc, 0.5);
  o->dmax_db = quantile(dmax, nc, 0.5);
  g_free(gd);
  g_free(coh);
  g_free(dmax);
}

double skim_phase_meter_gain(const SkimPhaseMeter *m, guint chan) {
  return m && chan < m->nchan ? m->gain[chan] : NAN;
}

double skim_phase_meter_gain_a(const SkimPhaseMeter *m, guint chan) {
  return m && chan < m->nchan ? m->gain_a[chan] : NAN;
}

double skim_phase_meter_drop(const SkimPhaseMeter *m, guint chan) {
  return m && chan < m->nchan ? m->drop[chan] : NAN;
}

double skim_phase_meter_gain_est(const SkimPhaseMeter *m, guint chan) {
  return m && chan < m->nchan ? m->gain_est[chan] : NAN;
}

double skim_phase_meter_drop_est(const SkimPhaseMeter *m, guint chan) {
  return m && chan < m->nchan ? m->drop_est[chan] : NAN;
}

double skim_phase_meter_snr(SkimPhaseMeter *m, SkimPhaseStream s, guint chan) {
  return m && s < SKIM_PH_N ? skim_snr_meter_db(m->snr[s], chan) : NAN;
}

void skim_phase_meter_summary(const SkimPhaseMeter *m, SkimPhaseSummary *out) {
  if (m) { *out = m->sum; }
}

SkimSnrMeter *skim_phase_meter_snr_meter(SkimPhaseMeter *m, SkimPhaseStream s) {
  return m && s < SKIM_PH_N ? m->snr[s] : NULL;
}

gint8 skim_phase_q8(double db) {
  if (isnan(db))
    return SKIM_PHASE_NONE;
  const long q = lround(db * 4.0);
  return (gint8)CLAMP(q, -127, 127);
}
