/*
 * skimmer-phase-test — the two-antenna combiner (engine/combiner.h) and its
 * meters (engine/phase_meter.h) against the truth (docs/PHASING.md, P3).
 *
 * A synthetic pair: stations with their own amplitude and phase on each
 * antenna, independent receiver noise on A and B, and interferers that both
 * antennas hear with a fixed signature. Every component goes through a
 * channelizer of its own, and the antennas' channels are their sums
 * (the channelizer is linear), so once the combiner's weights are held
 * the truth of every channel is known exactly: the signal, the noise and
 * the interference each antenna and the output carry. The gate checks
 *   the combiner: MRC gains ≥ 2.5 dB with no interferer; one interferer
 *     30 dB over the noise is nulled ≥ 20 dB; two interferers in two halves
 *     of the band are each nulled where they are; B a copy of A gives no
 *     gain and no blow-up; an empty IN2 costs nothing; mode A is antenna A
 *     bit for bit; a Fixed weight set against a known signature nulls it;
 *   the meters: the per-channel gain G = SNR_Y − max(SNR_A, SNR_B), the
 *     floor drop and the coherence's D_max read what the truth says.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/channelizer.h"
#include "engine/combiner.h"
#include "engine/phase_meter.h"
#include "engine/pipeline.h"
#include "engine/band_combine.h"

#define FS       48000.0
#define SPACING  125.0
#define BLK      4800                    /* 0.1 s                          */
#define SECS     20
#define HOLD_AT  10                      /* s: the weights held from here  */
#define TRUTH_AT 12                      /* s: the truth summed from here  */
#define FIR_TAPS 255
#define RAMP_S   0.005

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

typedef struct {
  double off_hz;           /* from the band centre                          */
  double snr_a;            /* SNR_2500 on antenna A, dB (against its noise)  */
  double b_rel_db;         /* B's amplitude against A's                      */
  double b_ph_deg;         /* B's phase against A's                          */
  gboolean keyed;
} Station;

typedef struct {
  double inr_db;           /* over the receiver noise, same bandwidth        */
  double h_mag, h_ph_deg;  /* B = h · A                                      */
  double lo_hz, hi_hz;     /* band-limited when lo < hi                      */
} Interf;

typedef struct {
  const char *name;
  Station st[8];
  guint   nst;
  Interf  in[2];
  guint   nin;
  double  nb_db;           /* B's receiver noise against A's (0 = equal)     */
  gboolean b_copy;         /* B = A, every component                         */
  gboolean b_dead;         /* no station or interferer reaches B             */
  SkimPhaseMode mode;
  double  fixed_gain_db, fixed_phase_deg;
} Scenario;

/* What a run leaves for the checks. */
typedef struct {
  guint   nchan;
  double *g_true, *g_meter;      /* per station                             */
  double *g_est;                 /* the combiner's estimate, per station    */
  guint  *st_chan;
  double *drop_true, *drop_meter;/* per channel (NAN: not a quiet channel)  */
  double  dmax_true;             /* analytic, one wideband interferer       */
  double  dmax_meter;            /* the meter's median                      */
  SkimPhaseSummary sum;
  gboolean a_exact;              /* mode A: y == a bit for bit, all along   */
  guint   group_of[16384];
} Result;

/* --- signal sources ------------------------------------------------------ */

typedef struct { GRand *r; guint left; gboolean down; guint nel; double lin; } Keyer;

static double keyer_step(Keyer *k, guint dot) {
  if (k->left == 0) {
    if (k->down) {
      k->down = FALSE;
      k->left = (++k->nel % 4 == 0) ? 3 * dot : dot;
    } else {
      k->down = TRUE;
      k->left = g_rand_boolean(k->r) ? dot : 3 * dot;
    }
  }
  k->left--;
  const double step = 1.0 / (RAMP_S * FS);
  k->lin = k->down ? MIN(1.0, k->lin + step) : MAX(0.0, k->lin - step);
  const double e = sin(G_PI / 2 * k->lin);
  return e * e;
}

static double complex gauss(GRand *r, double power) {
  const double u1 = g_rand_double(r) + 1e-300, u2 = g_rand_double(r);
  const double g = sqrt(-log(u1) * power);   /* E|x|² = power */
  return g * cexp(I * 2 * G_PI * u2);
}

/* A band-limited noise: white through a complex band-pass FIR (unit gain
 * in its passband), so its density in band is the white input's. */
typedef struct {
  double complex taps[FIR_TAPS];
  double complex hist[FIR_TAPS];
  guint pos;
  gboolean on;
} Fir;

static void fir_init(Fir *f, double lo, double hi) {
  memset(f, 0, sizeof(*f));
  if (!(lo < hi))
    return;
  f->on = TRUE;
  const double fc = 0.5 * (hi - lo) / FS, f0 = 0.5 * (hi + lo) / FS;
  const int mid = FIR_TAPS / 2;
  for (int n = 0; n < FIR_TAPS; n++) {
    const int m = n - mid;
    const double s = m == 0 ? 2 * fc : sin(2 * G_PI * fc * m) / (G_PI * m);
    const double w = 0.42 - 0.5 * cos(2 * G_PI * n / (FIR_TAPS - 1)) +
                     0.08 * cos(4 * G_PI * n / (FIR_TAPS - 1));
    f->taps[n] = s * w * cexp(I * 2 * G_PI * f0 * m);
  }
}

static double complex fir_step(Fir *f, double complex x) {
  if (!f->on)
    return x;
  f->hist[f->pos] = x;
  double complex y = 0;
  guint j = f->pos;
  for (int n = 0; n < FIR_TAPS; n++) {
    y += f->taps[n] * f->hist[j];
    j = j == 0 ? FIR_TAPS - 1 : j - 1;
  }
  f->pos = (f->pos + 1) % FIR_TAPS;
  return y;
}

/* --- one run -------------------------------------------------------------- */

enum { C_SA, C_SB, C_NA, C_NB, C_IA, C_IB, C_N };

/* The pair's components, sample by sample: v = { S_A, S_B, N_A, N_B, I_A,
 * I_B } (stations, receiver noise, interference on each antenna). */
typedef struct {
  const Scenario *sc;
  GRand  *r;
  double  nb_pow;
  guint   dot;
  Keyer   key[8];
  double  amp[8], ph[8];
  double complex bsig[8];
  Fir     fir[2];
  double  ipow[2];
  double complex ih[2];
} Gen;

static void gen_init(Gen *g, const Scenario *sc) {
  memset(g, 0, sizeof(*g));
  g->sc = sc;
  g->r = g_rand_new_with_seed(11);
  const double n0 = 1.0 / FS;                  /* A's noise: unit power     */
  g->nb_pow = pow(10.0, sc->nb_db / 10.0);
  g->dot = (guint)(1.2 / 22 * FS);
  for (guint s = 0; s < sc->nst; s++) {
    g->key[s] = (Keyer){ .r = g_rand_new_with_seed(200 + s) };
    g->amp[s] = sqrt(pow(10.0, sc->st[s].snr_a / 10.0) * n0 * 2500.0);
    g->bsig[s] = pow(10.0, sc->st[s].b_rel_db / 20.0) *
                 cexp(I * sc->st[s].b_ph_deg * G_PI / 180.0);
  }
  for (guint j = 0; j < sc->nin; j++) {
    fir_init(&g->fir[j], sc->in[j].lo_hz, sc->in[j].hi_hz);
    g->ipow[j] = pow(10.0, sc->in[j].inr_db / 10.0);
    g->ih[j] = sc->in[j].h_mag * cexp(I * sc->in[j].h_ph_deg * G_PI / 180.0);
  }
}

static void gen_step(Gen *g, double complex v[C_N]) {
  const Scenario *sc = g->sc;
  double complex sa = 0, sb = 0, ia = 0, ib = 0;
  for (guint s = 0; s < sc->nst; s++) {
    const double e = (sc->st[s].keyed ? keyer_step(&g->key[s], g->dot) : 1.0) * g->amp[s];
    const double complex x = e * cexp(I * g->ph[s]);
    g->ph[s] = remainder(g->ph[s] + 2 * G_PI * sc->st[s].off_hz / FS, 2 * G_PI);
    sa += x;
    sb += x * g->bsig[s];
  }
  for (guint j = 0; j < sc->nin; j++) {
    const double complex x = fir_step(&g->fir[j], gauss(g->r, g->ipow[j]));
    ia += x;
    ib += x * g->ih[j];
  }
  const double complex na = gauss(g->r, 1.0);
  double complex nb = gauss(g->r, g->nb_pow);
  if (sc->b_copy) {
    sb = sa;
    ib = ia;
    nb = na;
  }
  if (sc->b_dead) {
    sb = 0;
    ib = 0;
  }
  v[C_SA] = sa;
  v[C_SB] = sb;
  v[C_NA] = na;
  v[C_NB] = nb;
  v[C_IA] = ia;
  v[C_IB] = ib;
}

static void gen_clear(Gen *g) {
  for (guint s = 0; s < g->sc->nst; s++) { g_rand_free(g->key[s].r); }
  g_rand_free(g->r);
}

static guint chan_of(guint nchan, double off_hz) {
  const long k = lround(off_hz / SPACING);
  return (guint)((k % (long)nchan + (long)nchan) % (long)nchan);
}

static double pw(const float *x, guint n) {
  double s = 0;
  for (guint i = 0; i < 2 * n; i++) { s += (double)x[i] * x[i]; }
  return s;
}

static void mix(const float c[2][2], const float *a, const float *b, float *y, guint n) {
  const double complex ca = c[0][0] + I * c[0][1], cb = c[1][0] + I * c[1][1];
  for (guint i = 0; i < n; i++) {
    const double complex o = ca * (a[2 * i] + I * a[2 * i + 1]) +
                             cb * (b[2 * i] + I * b[2 * i + 1]);
    y[2 * i] = (float)creal(o);
    y[2 * i + 1] = (float)cimag(o);
  }
}

static void run(const Scenario *sc, Result *res) {
  SkimChannelizer *bank[C_N];
  for (guint i = 0; i < C_N; i++) { bank[i] = skim_channelizer_new(FS, SPACING); }
  const guint nchan = skim_channelizer_count(bank[0]);
  const double rate = skim_channelizer_out_rate(bank[0]);
  SkimCombinerConfig cfg;
  skim_combiner_config_default(&cfg);
  cfg.mode = sc->mode;
  cfg.fixed_gain_db = sc->fixed_gain_db;
  cfg.fixed_phase_deg = sc->fixed_phase_deg;
  SkimCombiner *comb = skim_combiner_new(nchan, rate, &cfg);
  SkimPhaseMeter *met = skim_phase_meter_new(nchan, rate, SKIM_SNR_CW_ENBW * SPACING);

  Gen gen;
  gen_init(&gen, sc);

  float *iq[C_N];
  for (guint i = 0; i < C_N; i++) { iq[i] = g_new(float, 2 * BLK); }
  float ch[C_N][2 * 64], a[2 * 64], b[2 * 64], y[2 * 64], t[2 * 64];
  /* truth: per channel the powers of each component on A, B and Y */
  enum { T_SA, T_SB, T_SY, T_NA, T_NB, T_NY, T_N };
  typedef double Truth[T_N];
  Truth *tr = g_new0(Truth, nchan);
  res->a_exact = TRUE;
  const guint nblk = (guint)(SECS * FS / BLK);
  const guint upd_every = (guint)(FS / BLK);   /* once a second */
  for (guint blk = 0; blk < nblk; blk++) {
    const double tsec = (double)blk * BLK / FS;
    for (guint n = 0; n < BLK; n++) {
      double complex v[C_N];
      gen_step(&gen, v);
      for (guint i = 0; i < C_N; i++) {
        iq[i][2 * n] = (float)creal(v[i]);
        iq[i][2 * n + 1] = (float)cimag(v[i]);
      }
    }
    for (guint i = 0; i < C_N; i++) { skim_channelizer_push(bank[i], iq[i], BLK); }

    for (guint k = 0; k < nchan; k++) {
      for (;;) {
        guint got = skim_channelizer_read(bank[0], k, ch[0], 64);
        for (guint i = 1; i < C_N; i++) {
          const guint g2 = skim_channelizer_read(bank[i], k, ch[i], 64);
          g_assert(g2 == got);
        }
        if (got == 0)
          break;
        for (guint j = 0; j < 2 * got; j++) {
          a[j] = ch[C_SA][j] + ch[C_NA][j] + ch[C_IA][j];
          b[j] = ch[C_SB][j] + ch[C_NB][j] + ch[C_IB][j];
        }
        skim_combiner_process(comb, k, a, b, y, got);
        if (sc->mode == SKIM_PHASE_A && memcmp(a, y, 2 * got * sizeof(float)) != 0) {
          res->a_exact = FALSE;
        }
        skim_phase_meter_push(met, k, a, b, y, got);
        if (tsec >= TRUTH_AT) {
          SkimCombinerChanInfo info;
          skim_combiner_chan_info(comb, k, &info);
          const float c[2][2] = { { info.ca[0], info.ca[1] }, { info.cb[0], info.cb[1] } };
          tr[k][T_SA] += pw(ch[C_SA], got);
          tr[k][T_SB] += pw(ch[C_SB], got);
          mix(c, ch[C_SA], ch[C_SB], t, got);
          tr[k][T_SY] += pw(t, got);
          /* noise + interference together */
          float na_[2 * 64], nb_[2 * 64];
          for (guint j = 0; j < 2 * got; j++) {
            na_[j] = ch[C_NA][j] + ch[C_IA][j];
            nb_[j] = ch[C_NB][j] + ch[C_IB][j];
          }
          tr[k][T_NA] += pw(na_, got);
          tr[k][T_NB] += pw(nb_, got);
          mix(c, na_, nb_, t, got);
          tr[k][T_NY] += pw(t, got);
        }
      }
    }
    if ((blk + 1) % upd_every == 0) {
      skim_combiner_update(comb);
      skim_phase_meter_refresh(met, comb);
      if ((blk + 1) / upd_every == HOLD_AT) { skim_combiner_hold(comb, TRUE); }
    }
  }

  res->nchan = nchan;
  res->g_true = g_new(double, sc->nst);
  res->g_meter = g_new(double, sc->nst);
  res->g_est = g_new(double, sc->nst);
  res->st_chan = g_new(guint, sc->nst);
  for (guint s = 0; s < sc->nst; s++) {
    const guint k = chan_of(nchan, sc->st[s].off_hz);
    res->st_chan[s] = k;
    const double snr_a = tr[k][T_SA] / tr[k][T_NA];
    const double snr_b = tr[k][T_NB] > 0 ? tr[k][T_SB] / tr[k][T_NB] : 0;
    const double snr_y = tr[k][T_SY] / tr[k][T_NY];
    res->g_true[s] = 10.0 * log10(snr_y / MAX(snr_a, snr_b));
    res->g_meter[s] = skim_phase_meter_gain(met, k);
    res->g_est[s] = skim_phase_meter_gain_est(met, k);
  }
  res->drop_true = g_new(double, nchan);
  res->drop_meter = g_new(double, nchan);
  for (guint k = 0; k < nchan; k++) {
    SkimCombinerChanInfo info;
    skim_combiner_chan_info(comb, k, &info);
    res->group_of[k] = skim_combiner_group_of(comb, k);
    res->drop_meter[k] = skim_phase_meter_drop(met, k);
    const double ca2 = (double)info.ca[0] * info.ca[0] + (double)info.ca[1] * info.ca[1];
    res->drop_true[k] = isnan(res->drop_meter[k]) || ca2 <= 0
                            ? NAN : 10.0 * log10(tr[k][T_NA] * ca2 / tr[k][T_NY]);
  }
  skim_phase_meter_summary(met, &res->sum);
  res->dmax_meter = res->sum.dmax_db;
  res->dmax_true = NAN;
  if (sc->nin == 1 && !(sc->in[0].lo_hz < sc->in[0].hi_hz)) {
    const double I0 = gen.ipow[0], h2 = sc->in[0].h_mag * sc->in[0].h_mag;
    const double g2 = h2 * I0 * I0 / ((I0 + 1.0) * (h2 * I0 + gen.nb_pow));
    res->dmax_true = -10.0 * log10(1.0 - g2);
  }

  gen_clear(&gen);
  for (guint i = 0; i < C_N; i++) {
    g_free(iq[i]);
    skim_channelizer_free(bank[i]);
  }
  g_free(tr);
  skim_phase_meter_free(met);
  skim_combiner_free(comb);
}

/* --- the whole pipeline ------------------------------------------------------ */

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},{'B',"-..."},{'C',"-.-."},{'D',"-.."},{'E',"."},{'F',"..-."},
  {'G',"--."},{'H',"...."},{'I',".."},{'J',".---"},{'K',"-.-"},{'L',".-.."},
  {'M',"--"},{'N',"-."},{'O',"---"},{'P',".--."},{'Q',"--.-"},{'R',".-."},
  {'S',"..."},{'T',"-"},{'U',"..-"},{'V',"...-"},{'W',".--"},{'X',"-..-"},
  {'Y',"-.--"},{'Z',"--.."},{'0',"-----"},{'1',".----"},{'2',"..---"},
  {'3',"...--"},{'4',"....-"},{'5',"....."},{'6',"-...."},{'7',"--..."},
  {'8',"---.."},{'9',"----."},
};

/* The keying envelope of text at wpm, edges shaped (5 ms raised cosine). */
static float *cw_env(const char *text, double wpm, guint *len) {
  GArray *e = g_array_new(FALSE, FALSE, sizeof(float));
  const double dit = 1.2 / wpm * FS;
  const float one = 1.0f, zero = 0.0f;
#define RUN(n, v) for (guint i_ = 0; i_ < (guint)((n) + 0.5); i_++) g_array_append_val(e, v)
  RUN(3 * dit, zero);
  for (const char *p = text; *p; p++) {
    if (*p == ' ') { RUN(4 * dit, zero); continue; }
    const char *m = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
      if (MORSE[i].c == *p) { m = MORSE[i].m; }
    }
    if (!m) { continue; }
    for (const char *q = m; *q; q++) {
      RUN(*q == '-' ? 3 * dit : dit, one);
      RUN(dit, zero);
    }
    RUN(2 * dit, zero);
  }
#undef RUN
  const guint L = (guint)(FS * RAMP_S);
  float *src = (float *)e->data, *out = g_new0(float, e->len);
  for (guint n = 0; n < e->len; n++) {
    double acc = 0, ws = 0;
    for (guint k = 0; k < L && k <= n; k++) {
      const double w = 0.5 - 0.5 * cos(2 * G_PI * (k + 0.5) / L);
      acc += w * src[n - k];
      ws += w;
    }
    out[n] = (float)(acc / ws);
  }
  *len = e->len;
  g_array_free(e, TRUE);
  return out;
}

typedef struct {
  char   calls[32][16];
  guint  n;
  gboolean phased;
  double gain, snr_a, snr_b, snr_y;
} Seen;

static void seen_cb(const SkimStation *st, gpointer user) {
  Seen *s = user;
  if (strcmp(st->call, "OK1PHS") == 0) {
    s->phased = st->phased;
    s->gain = st->phase_gain_db;
    s->snr_a = st->snr2_a_db;
    s->snr_b = st->snr2_b_db;
    s->snr_y = st->snr2_db;
  }
  for (guint i = 0; i < s->n; i++) {
    if (strcmp(s->calls[i], st->call) == 0)
      return;
  }
  if (s->n < G_N_ELEMENTS(s->calls)) { g_strlcpy(s->calls[s->n++], st->call, 16); }
}

static gboolean seen_has(const Seen *s, const char *call) {
  for (guint i = 0; i < s->n; i++) {
    if (strcmp(s->calls[i], call) == 0) { return TRUE; }
  }
  return FALSE;
}

/* The band of the pipeline checks: OK1PHS calling at SNR_2500 21 dB over
 * the receiver noise, under a wideband interferer 30 dB over it that both
 * antennas hear (B = 0.8∠57° · A), the station 0 dB ∠170° on B. */
static void pipe_band_ex(float **pa, float **pb, guint *frames, double snr_db,
                         double complex bs, double ipow_db, double complex h);

static void pipe_band(float **pa, float **pb, guint *frames) {
  pipe_band_ex(pa, pb, frames, 21, cexp(I * 170 * G_PI / 180), 30, 0.8 * cexp(I * 57 * G_PI / 180));
}

/* The same band: OK1PHS at snr_db, its signature bs on B, an interferer
 * ipow_db over the noise with signature h. */
static void pipe_band_ex(float **pa, float **pb, guint *frames, double snr_db,
                         double complex bs, double ipow_db, double complex h) {
  guint len;
  char *text = g_strdup("CQ TEST OK1PHS OK1PHS CQ TEST OK1PHS OK1PHS K "
                        "CQ TEST OK1PHS OK1PHS CQ TEST OK1PHS OK1PHS K");
  float *env = cw_env(text, 24, &len);
  g_free(text);
  const guint total = len + (guint)(2 * FS);
  float *a = g_new(float, 2 * (gsize)total), *b = g_new(float, 2 * (gsize)total);
  GRand *r = g_rand_new_with_seed(77);
  const double amp = sqrt(pow(10.0, snr_db / 10.0) / FS * 2500.0);
  const double ipow = pow(10.0, ipow_db / 10.0), off = 7005.0;
  double ph = 0;
  for (guint n = 0; n < total; n++) {
    const double e = n < len ? env[n] * amp : 0;
    const double complex x = e * cexp(I * ph);
    ph = remainder(ph + 2 * G_PI * off / FS, 2 * G_PI);
    const double complex i0 = gauss(r, ipow);
    const double complex va = x + i0 + gauss(r, 1.0);
    const double complex vb = x * bs + i0 * h + gauss(r, 1.0);
    a[2 * n] = (float)creal(va);
    a[2 * n + 1] = (float)cimag(va);
    b[2 * n] = (float)creal(vb);
    b[2 * n + 1] = (float)cimag(vb);
  }
  g_rand_free(r);
  g_free(env);
  *pa = a;
  *pb = b;
  *frames = total;
}

/* The band-level coefficients the last pipe_run's pipeline published. */
static float  last_bca[2048], last_bcb[2048];
static guint  last_bcn;
static double last_first, last_spacing;

/* The band-level combination of the whole pair with the last coefficients:
 * its power against A's, dB (≥ 0: the floor went down). */
static double band_drop(const float *a, const float *b, guint frames) {
  SkimBandCombine *bc = skim_band_combine_new(FS, 0);
  skim_band_combine_set_weights(bc, last_bca, last_bcb, last_bcn, last_first, last_spacing);
  float *y = g_new(float, 2 * (gsize)frames);
  for (guint at = 0; at < frames; at += BLK) {
    const guint n = MIN((guint)BLK, frames - at);
    skim_band_combine_process(bc, a + 2 * (gsize)at, b + 2 * (gsize)at, y + 2 * (gsize)at, n);
  }
  const guint skip = (guint)(2 * FS);          /* past the start            */
  const double pa = pw(a + 2 * (gsize)skip, frames - skip - 4096);
  const double py = pw(y + 2 * (gsize)(skip + 4096), frames - skip - 4096);
  g_free(y);
  skim_band_combine_free(bc);
  return 10.0 * log10(pa / py);
}

/* Unit weights give A back exactly, delayed by a fixed number of frames. */
static void check_band_unit(void) {
  const guint frames = 48000;
  float *a = g_new(float, 2 * frames), *b = g_new(float, 2 * frames), *y = g_new(float, 2 * frames);
  GRand *r = g_rand_new_with_seed(5);
  for (guint i = 0; i < 2 * frames; i++) {
    a[i] = (float)g_rand_double_range(r, -1, 1);
    b[i] = (float)g_rand_double_range(r, -1, 1);
  }
  g_rand_free(r);
  SkimBandCombine *bc = skim_band_combine_new(FS, 0);
  const float one[2] = { 1, 0 }, zero[2] = { 0, 0 };
  skim_band_combine_set_weights(bc, one, zero, 1, 0, SPACING);
  for (guint at = 0; at < frames; at += 1000) {     /* odd chunks: across hops */
    const guint n = MIN(1000u, frames - at);
    skim_band_combine_process(bc, a + 2 * at, b + 2 * at, y + 2 * at, n);
  }
  skim_band_combine_free(bc);
  guint best = 0;
  double best_err = G_MAXDOUBLE;
  for (guint d = 0; d < 8192; d++) {
    double e = 0;
    for (guint i = 10000; i < 20000; i++) {
      const double dr = y[2 * (i + d)] - a[2 * i], di = y[2 * (i + d) + 1] - a[2 * i + 1];
      e = MAX(e, fabs(dr) + fabs(di));
    }
    if (e < best_err) { best_err = e; best = d; }
  }
  char what[160];
  g_snprintf(what, sizeof(what), "band level, unit weights: A back exactly (error %.1e), %u frames late",
             best_err, best);
  check(what, best_err < 1e-5);
  g_free(a);
  g_free(b);
  g_free(y);
}

/* One offline pipeline over the band: phased with B (b given), phased with
 * B missing all along (b NULL, phased), or a plain pipeline on one input. */
static gboolean run_layer2 = TRUE;
static SkimCwEngine run_engine = SKIM_CW_ENGINE_V2;

static void pipe_run(const float *in, const float *b, gboolean phased, guint frames,
                     Seen *seen, SkimPipelinePhase *ph, gint8 *rows, SkimPhaseMode mode) {
  SkimPipelineConfig cfg = { 0 };
  cfg.chan_bw_hz = SPACING;
  cfg.cw_engine = run_engine;
  cfg.phased = phased;
  SkimCombinerConfig cc;
  skim_combiner_config_default(&cc);
  cc.mode = mode;
  cc.layer2 = run_layer2;
  cfg.phase = &cc;
  SkimPipeline *p = skim_pipeline_new(&cfg);
  memset(seen, 0, sizeof(*seen));
  seen->gain = seen->snr_a = seen->snr_b = seen->snr_y = NAN;
  skim_pipeline_set_station_cb(p, seen_cb, seen);
  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    printf("  FAIL offline pipeline: %s\n", err ? err->message : "?");
    g_clear_error(&err);
  }
  for (guint at = 0; at < frames; at += BLK) {
    const guint n = MIN((guint)BLK, frames - at);
    skim_pipeline_feed2(p, in + 2 * (gsize)at, b ? b + 2 * (gsize)at : NULL, n, FS, 14050000.0);
  }
  if (ph) { skim_pipeline_phase_snapshot(p, ph, rows, rows ? rows + 1024 : NULL, NULL, 1024); }
  last_bcn = skim_pipeline_phase_band_coef(p, last_bca, last_bcb, 1024, &last_first, &last_spacing);
  skim_pipeline_stop(p);
  skim_pipeline_free(p);
}

/* Layer 2: a station 12° from a strong interferer's signature. The group
 * canceller (layer 1) nulls the interferer and most of the station with it
 * (|1 − e^{-j12°}| = −13.6 dB), and the station is too weak beside the
 * interferer for layer 1's power gate to steer on it; the decoder still
 * reads it, and its marks teach layer 2 the station's own signature. */
static void check_layer2_engine(SkimCwEngine eng, const char *name);
static void check_layer2(void) {
  check_layer2_engine(SKIM_CW_ENGINE_V2, "CW v2");
  check_layer2_engine(SKIM_CW_ENGINE_V3, "CW v3");
  check_layer2_engine(SKIM_CW_ENGINE_V4, "CW v4");
}

static void check_layer2_engine(SkimCwEngine eng, const char *name) {
  char what[220];
  run_engine = eng;
  printf("== layer 2 (%s): OK1PHS 26 dB, 12° from an interferer 40 dB over the noise\n", name);
  float *a, *b;
  guint frames;
  const double complex h = 0.8 * cexp(I * 57 * G_PI / 180);
  pipe_band_ex(&a, &b, &frames, 26, 0.8 * cexp(I * 69 * G_PI / 180), 40, h);
  Seen s1, s2;
  SkimPipelinePhase p1, p2;
  run_layer2 = FALSE;
  pipe_run(a, b, TRUE, frames, &s1, &p1, NULL, SKIM_PHASE_MVDR);
  run_layer2 = TRUE;
  pipe_run(a, b, TRUE, frames, &s2, &p2, NULL, SKIM_PHASE_MVDR);
  printf("    layer 1 only: %s, SNR_2500 combined %+.1f (A %+.1f, B %+.1f), %u channels on layer 2\n",
         seen_has(&s1, "OK1PHS") ? "OK1PHS spotted" : "no OK1PHS", s1.snr_y, s1.snr_a, s1.snr_b,
         p1.sum.layer2);
  printf("    layer 2:      %s, SNR_2500 combined %+.1f, gain %+.1f dB, %u channels on layer 2\n",
         seen_has(&s2, "OK1PHS") ? "OK1PHS spotted" : "no OK1PHS", s2.snr_y, s2.gain, p2.sum.layer2);
  check("layer 2 engages on the station's channel (and only with it on)",
        p2.sum.layer2 >= 1 && p1.sum.layer2 == 0);
  /* one strong interferer: nulling it fixes the weight's direction (two
   * antennas, one null), so layer 1 is already near the optimum — layer 2
   * must not do worse (the in-channel case below is where it gains) */
  g_snprintf(what, sizeof(what), "it decodes, no worse than layer 1 (%+.1f dB)", s2.snr_y - s1.snr_y);
  check(what, seen_has(&s2, "OK1PHS") && s2.snr_y - s1.snr_y >= -0.5);
  gboolean bogus = FALSE;
  for (guint i = 0; i < s2.n; i++) { bogus = bogus || strcmp(s2.calls[i], "OK1PHS") != 0; }
  check("… and no other call comes out", !bogus);
  run_layer2 = TRUE;
  run_engine = SKIM_CW_ENGINE_V2;
  g_free(a);
  g_free(b);
}

/* Layer 2's own case, on the combiner alone with the true marks: an
 * interferer in the station's channel only (as loud as the station, its
 * own signature), which the group's quiet channels never see. Layer 1
 * steers on the mix of both; layer 2 learns the station from the marks and
 * the interferer from the spaces, and nulls it. The SINR of each final
 * weight measured on the components. */
static double l2_sinr(gboolean layer2) {
  const guint NC = 64, K = 10, RATE = 250, SECS2 = 40;
  SkimCombinerConfig cfg;
  skim_combiner_config_default(&cfg);
  cfg.layer2 = layer2;
  SkimCombiner *c = skim_combiner_new(NC, RATE, &cfg);
  GRand *r = g_rand_new_with_seed(99);
  const double complex s = cexp(I * 100 * G_PI / 180), h = 0.9 * cexp(I * -60 * G_PI / 180);
  const double ps = 100.0, pi = 100.0;       /* station and interferer: 20 dB over the noise */
  float a[2 * RATE], b[2 * RATE], y[2 * RATE], pm[RATE];
  double sig_y = 0, noi_y = 0;
  guint key_t = 0;
  for (guint sec = 0; sec < SECS2; sec++) {
    if (sec == SECS2 - 10) { skim_combiner_hold(c, TRUE); }
    for (guint k = 0; k < NC; k++) {
      for (guint n = 0; n < RATE; n++) {
        const double complex na = gauss(r, 1.0), nb = gauss(r, 1.0);
        double complex xa = na, xb = nb, st = 0, it = 0;
        if (k == K) {
          const gboolean down = ((key_t + n) / 12) % 3 != 2;   /* 2/3 key-down */
          st = down ? sqrt(ps) * cexp(I * 0.3 * (key_t + n)) : 0;
          it = gauss(r, pi);
          xa += st + it;
          xb += st * s + it * h;
          pm[n] = down ? 1.0f : 0.0f;
        }
        a[2 * n] = (float)creal(xa); a[2 * n + 1] = (float)cimag(xa);
        b[2 * n] = (float)creal(xb); b[2 * n + 1] = (float)cimag(xb);
        if (k == K && sec >= SECS2 - 8) {      /* the truth, weights held */
          SkimCombinerChanInfo info;
          skim_combiner_chan_info(c, K, &info);
          const double complex ca = info.ca[0] + I * info.ca[1], cb = info.cb[0] + I * info.cb[1];
          const double complex so = ca * st + cb * st * s, no = ca * (na + it) + cb * (nb + it * h);
          sig_y += creal(so * conj(so));
          noi_y += creal(no * conj(no));
        }
      }
      skim_combiner_process(c, k, a, b, y, RATE);
      skim_combiner_mark(c, k, a, b, k == K ? pm : NULL, RATE, 0);
    }
    key_t += RATE;
    skim_combiner_update(c);
  }
  g_rand_free(r);
  skim_combiner_free(c);
  return 10.0 * log10(sig_y / noi_y);
}

static void check_layer2_inchannel(void) {
  char what[200];
  printf("== layer 2 on its own case: an interferer in the station's channel only\n");
  const double l1 = l2_sinr(FALSE), l2 = l2_sinr(TRUE);
  g_snprintf(what, sizeof(what), "the station's SINR: layer 1 %+.1f dB, layer 2 %+.1f dB (≥ 10 dB more)", l1, l2);
  check(what, l2 - l1 >= 10.0);
}

static void check_pipeline(void) {
  char what[200];
  float *a, *b;
  guint frames;
  pipe_band(&a, &b, &frames);
  Seen sa, sb, sp, sm, s_a;
  SkimPipelinePhase ph, phm, pha;
  gint8 rows[2048];
  pipe_run(a, NULL, FALSE, frames, &sa, NULL, NULL, SKIM_PHASE_MVDR);
  pipe_run(b, NULL, FALSE, frames, &sb, NULL, NULL, SKIM_PHASE_MVDR);
  pipe_run(a, b, TRUE, frames, &sp, &ph, rows, SKIM_PHASE_MVDR);
  pipe_run(a, NULL, TRUE, frames, &sm, &phm, NULL, SKIM_PHASE_MVDR);
  pipe_run(a, b, TRUE, frames, &s_a, &pha, NULL, SKIM_PHASE_A);
  printf("    A alone: %u calls%s; B alone: %u calls%s; phased: %u calls%s\n",
         sa.n, seen_has(&sa, "OK1PHS") ? " (OK1PHS)" : "",
         sb.n, seen_has(&sb, "OK1PHS") ? " (OK1PHS)" : "",
         sp.n, seen_has(&sp, "OK1PHS") ? " (OK1PHS)" : "");
  printf("    phased OK1PHS: SNR_2500 A %+.1f, B %+.1f, combined %+.1f, gain %+.1f dB\n",
         sp.snr_a, sp.snr_b, sp.snr_y, sp.gain);
  printf("    snapshot: %llu updates, G %+.1f, drop %+.1f (D_max %.1f), %u stations\n",
         (unsigned long long)ph.updates, ph.sum.gain_db, ph.sum.drop_db, ph.sum.dmax_db,
         ph.sum.stations);
  check("phased, the pipeline spots OK1PHS under the interferer", seen_has(&sp, "OK1PHS"));
  check("not worse than either antenna alone (every call they spot, it spots)",
        (!seen_has(&sa, "OK1PHS") || seen_has(&sp, "OK1PHS")) &&
        (!seen_has(&sb, "OK1PHS") || seen_has(&sp, "OK1PHS")));
  check("… and it hears what neither antenna alone does: A and B miss OK1PHS",
        !seen_has(&sa, "OK1PHS") && !seen_has(&sb, "OK1PHS"));
  gboolean bogus = FALSE;
  for (guint i = 0; i < sp.n; i++) { bogus = bogus || strcmp(sp.calls[i], "OK1PHS") != 0; }
  check("no other call comes out of the phased band", !bogus);
  g_snprintf(what, sizeof(what), "the spot carries its numbers: phased, gain %+.1f dB ≥ 10",
             sp.gain);
  check(what, sp.phased && sp.gain >= 10.0);
  g_snprintf(what, sizeof(what), "the snapshot: floor drop %+.1f dB ≥ 20, the station's "
             "gain %+.1f dB ≥ 10, rows filled", ph.sum.drop_db, ph.sum.gain_db);
  gboolean rows_ok = FALSE;
  for (guint i = 0; i < MIN(ph.nchan, 1024u); i++) {
    rows_ok = rows_ok || rows[1024 + i] != SKIM_PHASE_NONE;
  }
  check(what, ph.phased && !ph.b_missing && ph.sum.drop_db >= 20.0 && rows_ok &&
              ph.sum.stations >= 1 && ph.sum.gain_db >= 10.0 &&
              ph.nchan == 384 && fabs(ph.first_hz + 23875.0) < 1);
  check("B missing all along: the band decodes A, the snapshot says so",
        phm.phased && phm.b_missing && seen_has(&sm, "OK1PHS") == seen_has(&sa, "OK1PHS"));
  g_snprintf(what, sizeof(what), "mode A: estimates (est, drop %+.1f dB ≥ 20) while A decodes alone",
             pha.sum.drop_est_db);
  check(what, pha.est && pha.mode == SKIM_PHASE_A && pha.sum.drop_est_db >= 20.0 &&
              seen_has(&s_a, "OK1PHS") == seen_has(&sa, "OK1PHS"));
  /* the band's own IQ (waterfall, FT8): mode A → A; MVDR → the floor drops */
  const double off = band_drop(a, b, frames);
  pipe_run(a, b, TRUE, frames, &sp, &ph, rows, SKIM_PHASE_MVDR);
  const double on = band_drop(a, b, frames);
  g_snprintf(what, sizeof(what), "band level: mode A changes nothing (%+.2f dB), MVDR takes the "
             "interferer off the floor (%+.1f dB ≥ 15)", off, on);
  check(what, fabs(off) < 0.05 && on >= 15.0);
  check_band_unit();
  g_free(a);
  g_free(b);
  a = b = NULL;
  check_layer2();
  check_layer2_inchannel();
  g_free(a);
  g_free(b);
}

static void result_free(Result *r) {
  g_free(r->g_true);
  g_free(r->g_meter);
  g_free(r->g_est);
  g_free(r->st_chan);
  g_free(r->drop_true);
  g_free(r->drop_meter);
}

/* --- checks ---------------------------------------------------------------- */

static void print_stations(const Scenario *sc, const Result *r) {
  for (guint s = 0; s < sc->nst; s++) {
    printf("    station %+7.0f Hz  A %+5.1f dB  B %+4.0f dB %+5.0f°  G true %+6.2f  meter %+6.2f  est %+6.2f\n",
           sc->st[s].off_hz, sc->st[s].snr_a, sc->st[s].b_rel_db, sc->st[s].b_ph_deg,
           r->g_true[s], r->g_meter[s], r->g_est[s]);
  }
  printf("    summary: G median %+.2f (A %+.2f), drop median %+.2f p90 %+.2f, "
         "groups ≥3 dB %.2f, |γ|² %.3f, D_max %.1f, share B %.2f, %u stations, %u quiet\n"
         "             est: G %+.2f, drop %+.2f; same direction %.2f\n",
         r->sum.gain_db, r->sum.gain_a_db, r->sum.drop_db, r->sum.drop_p90_db,
         r->sum.drop_share, r->sum.coherence, r->sum.dmax_db, r->sum.share_b,
         r->sum.stations, r->sum.quiet, r->sum.gain_est_db, r->sum.drop_est_db,
         r->sum.same_dir);
}

/* The meter's gain against the truth, every station. */
static void check_gain_meter(const Scenario *sc, const Result *r, double tol) {
  double worst = 0;
  gboolean all = TRUE;
  for (guint s = 0; s < sc->nst; s++) {
    if (isnan(r->g_meter[s])) { all = FALSE; continue; }
    worst = MAX(worst, fabs(r->g_meter[s] - r->g_true[s]));
  }
  char what[160];
  g_snprintf(what, sizeof(what), "the meter reads every station's gain within %.1f dB "
             "of the truth (worst %.2f)", tol, worst);
  check(what, all && worst <= tol);
}

/* The combiner's own estimate of the gain against the truth. */
static void check_gain_est(const Scenario *sc, const Result *r, double tol) {
  double worst = 0;
  gboolean all = TRUE;
  for (guint s = 0; s < sc->nst; s++) {
    if (isnan(r->g_est[s])) { all = FALSE; continue; }
    worst = MAX(worst, fabs(r->g_est[s] - r->g_true[s]));
  }
  char what[160];
  g_snprintf(what, sizeof(what), "the estimate of every station's gain within %.1f dB "
             "of the truth (worst %.2f)", tol, worst);
  check(what, all && worst <= tol);
}

/* The meter's floor drop against the truth over the quiet channels of the
 * groups [g0, g1]: their median error, and the true median drop there. */
static double check_drop_meter(const Result *r, guint g0, guint g1, double tol,
                               const char *where) {
  double *err = g_new(double, r->nchan), *tru = g_new(double, r->nchan);
  guint n = 0;
  for (guint k = 0; k < r->nchan; k++) {
    if (r->group_of[k] < g0 || r->group_of[k] > g1)
      continue;
    if (isnan(r->drop_meter[k]) || isnan(r->drop_true[k]))
      continue;
    err[n] = fabs(r->drop_meter[k] - r->drop_true[k]);
    tru[n] = r->drop_true[k];
    n++;
  }
  double med_err = NAN, med_true = NAN;
  if (n) {
    for (guint i = 1; i < n; i++)          /* insertion sorts: n ≤ 16384 */
      for (guint j = i; j > 0 && err[j - 1] > err[j]; j--) {
        const double x = err[j]; err[j] = err[j - 1]; err[j - 1] = x;
      }
    for (guint i = 1; i < n; i++)
      for (guint j = i; j > 0 && tru[j - 1] > tru[j]; j--) {
        const double x = tru[j]; tru[j] = tru[j - 1]; tru[j - 1] = x;
      }
    med_err = err[n / 2];
    med_true = tru[n / 2];
  }
  char what[200];
  g_snprintf(what, sizeof(what), "%s: the meter's floor drop within %.1f dB of the truth "
             "(median error %.2f over %u quiet channels, true drop %.1f dB)",
             where, tol, med_err, n, med_true);
  check(what, n > 0 && med_err <= tol);
  g_free(err);
  g_free(tru);
  return med_true;
}

/* The bench's pair (tests/phase-bench.sh): a wideband interferer 30 dB over
 * the noise, four stations, as two cf32 files with a [record] .meta. */
static const Scenario BENCH = { "bench", {
    { -17000 + 5, 35, 0, -60, TRUE }, { -5000 + 5, 35, -3, 100, FALSE },
    { 7000 + 5, 35, 0, 170, TRUE },   { 15000 + 5, 38, 0, 20, FALSE }, }, 4,
    { { 30, 0.8, 57, 0, 0 } }, 1, 0.0, FALSE, FALSE, SKIM_PHASE_MVDR, 0, 0 };

static int write_pair(const char *pa, const char *pb, double secs) {
  FILE *f[2] = { fopen(pa, "wb"), fopen(pb, "wb") };
  if (!f[0] || !f[1]) {
    perror(!f[0] ? pa : pb);
    return 2;
  }
  Gen gen;
  gen_init(&gen, &BENCH);
  const guint64 total = (guint64)(secs * FS);
  float buf[2][2 * 1024];
  for (guint64 done = 0; done < total;) {
    const guint n = (guint)MIN((guint64)1024, total - done);
    for (guint i = 0; i < n; i++) {
      double complex v[C_N];
      gen_step(&gen, v);
      const double complex a = v[C_SA] + v[C_NA] + v[C_IA];
      const double complex b = v[C_SB] + v[C_NB] + v[C_IB];
      buf[0][2 * i] = (float)creal(a);
      buf[0][2 * i + 1] = (float)cimag(a);
      buf[1][2 * i] = (float)creal(b);
      buf[1][2 * i + 1] = (float)cimag(b);
    }
    for (guint k = 0; k < 2; k++) { fwrite(buf[k], 2 * sizeof(float), n, f[k]); }
    done += n;
  }
  gen_clear(&gen);
  for (guint k = 0; k < 2; k++) {
    fclose(f[k]);
    char *mp = g_strdup_printf("%s.meta", k ? pb : pa);
    char *meta = g_strdup_printf("source: skimmer-phase-test --write-pair (%s)\n"
                                 "format: cf32 interleaved I,Q — TRUE spectrum orientation\n"
                                 "center_hz: 14000000\nrate_hz: %.0f\nduration_s: %.1f\n---\n",
                                 k ? "antenna B" : "antenna A", FS, secs);
    g_file_set_contents(mp, meta, -1, NULL);
    g_free(meta);
    g_free(mp);
  }
  return 0;
}

int main(int argc, char **argv) {
  char what[200];
  g_setenv("SKIM_DCW4_SYNC", "1", FALSE);      /* CW v4 inline: deterministic */
  if (argc >= 4 && strcmp(argv[1], "--write-pair") == 0) {
    return write_pair(argv[2], argv[3], argc > 4 ? g_ascii_strtod(argv[4], NULL) : 30.0);
  }

  printf("== MRC: no interferer, equal noise on A and B\n");
  {
    const Scenario sc = { "mrc", {
        { -17000 + 5, 15, 0, 40, TRUE },  { -9000 + 5, 15, 0, -120, FALSE },
        { 3000 + 5, 15, 0, 170, TRUE },   { 11000 + 5, 12, -6, 75, FALSE },
        { 19000 + 5, 10, +6, -30, TRUE }, }, 5, { { .inr_db = 0 } }, 0, 0.0, FALSE, FALSE,
        SKIM_PHASE_MVDR, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    gboolean eq = TRUE;
    for (guint s = 0; s < 3; s++) { eq = eq && r.g_true[s] >= 2.5; }
    check("equal antennas: the combiner gains ≥ 2.5 dB on every station", eq);
    check("B 6 dB weaker: still ≥ 0.6 dB over A (MRC: +1.0)", r.g_true[3] >= 0.6);
    check("B 6 dB stronger: ≥ 0.6 dB over B, the better one", r.g_true[4] >= 0.6);
    check_gain_meter(&sc, &r, 1.0);
    check_gain_est(&sc, &r, 1.0);
    g_snprintf(what, sizeof(what), "no interferer: the floor drop is ~0 (median %+.2f)",
               r.sum.drop_db);
    check(what, fabs(r.sum.drop_db) <= 0.5);
    g_snprintf(what, sizeof(what), "no interferer: |γ|² ~0 (median %.3f)", r.sum.coherence);
    check(what, r.sum.coherence < 0.05);
    result_free(&r);
  }

  printf("== one wideband interferer 30 dB over the noise, B = 0.8∠57° · A\n");
  {
    const Scenario sc = { "null", {
        { -17000 + 5, 35, 0, -60, TRUE }, { -5000 + 5, 35, -3, 100, FALSE },
        { 7000 + 5, 35, 0, 170, TRUE },   { 15000 + 5, 38, 0, 20, FALSE }, }, 4,
        { { 30, 0.8, 57, 0, 0 } }, 1, 0.0, FALSE, FALSE, SKIM_PHASE_MVDR, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    const double drop = check_drop_meter(&r, 0, 1000, 1.0, "whole band");
    g_snprintf(what, sizeof(what), "the interferer is nulled ≥ 20 dB (true drop %.1f dB)", drop);
    check(what, drop >= 20.0);
    g_snprintf(what, sizeof(what), "D_max from the coherence %.1f dB, analytic %.1f (±1)",
               r.dmax_meter, r.dmax_true);
    check(what, fabs(r.dmax_meter - r.dmax_true) <= 1.0);
    gboolean big = TRUE;
    for (guint s = 0; s < sc.nst; s++) { big = big && r.g_true[s] >= 15.0; }
    check("every station gains ≥ 15 dB over the better antenna", big);
    check_gain_meter(&sc, &r, 1.5);
    /* the estimate sees R_n through the quiet channels of a group and the
     * station through gated samples: at +20..30 dB of gain it reads up to
     * ~2 dB low — the metered gain is the number that counts on air */
    check_gain_est(&sc, &r, 2.0);
    result_free(&r);
  }

  printf("== two interferers in two halves of the band, different signatures\n");
  {
    const Scenario sc = { "two", {
        { -13000 + 5, 30, 0, 30, TRUE }, { 13000 + 5, 30, 0, -150, TRUE }, }, 2,
        { { 25, 0.8, 57, -23000, -1000 }, { 25, 1.2, -100, 1000, 23000 } }, 2,
        0.0, FALSE, FALSE, SKIM_PHASE_MVDR, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    /* 48 kHz: 12 groups of 4 kHz from -24 kHz; groups 1-4 lie inside the
     * lower interferer, 7-10 inside the upper */
    const double lo = check_drop_meter(&r, 1, 4, 1.0, "lower half");
    const double hi = check_drop_meter(&r, 7, 10, 1.0, "upper half");
    g_snprintf(what, sizeof(what), "both nulled where they are: ≥ 15 dB (%.1f / %.1f dB)", lo, hi);
    check(what, lo >= 15.0 && hi >= 15.0);
    check_gain_meter(&sc, &r, 1.5);
    result_free(&r);
  }

  printf("== B a copy of A\n");
  {
    const Scenario sc = { "copy", {
        { -9000 + 5, 15, 0, 0, TRUE }, { 9000 + 5, 20, 0, 0, FALSE }, }, 2, { { .inr_db = 0 } }, 0,
        0.0, TRUE, FALSE, SKIM_PHASE_MVDR, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    gboolean ok = TRUE;
    for (guint s = 0; s < sc.nst; s++) {
      ok = ok && isfinite(r.g_true[s]) && fabs(r.g_true[s]) <= 0.5 &&
           isfinite(r.g_meter[s]) && fabs(r.g_meter[s]) <= 0.5;
    }
    check("no gain, no blow-up: |G| ≤ 0.5 dB, true and metered", ok);
    g_snprintf(what, sizeof(what), "every station comes as the noise does (same direction %.2f): "
               "the page can say B hears what A hears", r.sum.same_dir);
    check(what, r.sum.same_dir >= 0.99);
    result_free(&r);
  }

  printf("== IN2 empty: B hears only its own noise, 20 dB lower\n");
  {
    const Scenario sc = { "empty", {
        { -9000 + 5, 15, 0, 0, TRUE }, { 5000 + 5, 5, 0, 0, FALSE }, { 17000 + 5, 25, 0, 0, TRUE }, },
        3, { { .inr_db = 0 } }, 0, -20.0, FALSE, TRUE, SKIM_PHASE_MVDR, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    gboolean ok = TRUE;
    for (guint s = 0; s < sc.nst; s++) {
      ok = ok && r.g_true[s] >= -0.5 && (isnan(r.g_meter[s]) || r.g_meter[s] >= -0.5);
    }
    check("it costs nothing: G ≥ -0.5 dB, true and metered", ok);
    g_snprintf(what, sizeof(what), "|γ|² ~0 (median %.3f): the page can say IN2 is empty",
               r.sum.coherence);
    check(what, r.sum.coherence < 0.05);
    result_free(&r);
  }

  printf("== mode A\n");
  {
    const Scenario sc = { "a", { { 3000 + 5, 35, 0, 90, TRUE }, }, 1,
        { { 30, 0.8, 57, 0, 0 } }, 1, 0.0, FALSE, FALSE, SKIM_PHASE_A, 0, 0 };
    Result r = { 0 };
    run(&sc, &r);
    check("the output is antenna A bit for bit, from the first sample", r.a_exact);
    g_snprintf(what, sizeof(what), "and its floor drop is 0 (median %+.2f)", r.sum.drop_db);
    check(what, fabs(r.sum.drop_db) < 0.05);
    print_stations(&sc, &r);
    g_snprintf(what, sizeof(what), "what phasing would give shows meanwhile: est. gain %+.1f dB "
               "(≥ 15), est. drop %+.1f dB (≥ 20)", r.g_est[0], r.sum.drop_est_db);
    check(what, r.g_est[0] >= 15.0 && r.sum.drop_est_db >= 20.0);
    result_free(&r);
  }

  printf("== Fixed: a manual weight against the known signature\n");
  {
    /* y = a + g·e^{jφ}·b nulls B = h·A when g·e^{jφ} = −1/h */
    const double hm = 0.8, hp = 57;
    const Scenario sc = { "fixed", { { 7000 + 5, 35, 0, 170, TRUE }, }, 1,
        { { 30, hm, hp, 0, 0 } }, 1, 0.0, FALSE, FALSE, SKIM_PHASE_FIXED,
        -20.0 * log10(hm), 180.0 - hp };
    Result r = { 0 };
    run(&sc, &r);
    print_stations(&sc, &r);
    const double drop = check_drop_meter(&r, 0, 1000, 1.0, "whole band");
    g_snprintf(what, sizeof(what), "the Fixed weight nulls it ≥ 20 dB (true drop %.1f dB)", drop);
    check(what, drop >= 20.0);
    result_free(&r);
  }

  printf("== the whole pipeline: OK1PHS 21 dB over the noise under a 30 dB interferer\n");
  check_pipeline();

  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
