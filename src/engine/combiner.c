/* combiner.c — see combiner.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "combiner.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Diagonal loading, a share of R_n's trace: keeps a near-singular R_n (one
 * overwhelming interferer, or B a copy of A) from blowing the weight up.
 * 0.1 %: at 30 dB INR it costs ~1 dB of null depth (1 % cost ~4 dB). */
#define LOADING        1e-3
/* A tick's gated samples must add up to this before R_s learns from it. */
#define MIN_GATED_S    0.1
/* R_s − R_n's principal eigenvalue must reach this share of the noise per
 * antenna before its eigenvector steers (else: layer 1, e_A). */
#define MIN_SIG_REL    0.1
/* The weight is "mostly A" while |w_a|² holds this share of |w|²: below it
 * drop_db is not defined (the output is B, turned). */
#define MIN_A_SHARE    0.05

typedef double complex cplx;

typedef struct {
  double r11, r22;
  cplx   r12;              /* E[a·b*]                                       */
} Cov;

typedef struct {
  /* this tick */
  double sa, sb;
  cplx   sab;
  guint  n;
  double ga, gb;
  cplx   gab;
  guint  gn;
  /* estimates */
  Cov      rs;
  gboolean rs_ok;
  double   rs_idle_s;
  Cov      inst;           /* the last tick's covariance, all samples        */
  double   thr;            /* gate on |a|²+|b|²; 0 = not yet                 */
  /* weights: the ramp from c0 to c1 */
  cplx     c0[2], c1[2];
  guint    ramp_pos, ramp_len;
  gboolean steered;
  double   drop_db, share_b;
  double   drop_est_db, gain_est_db, align;
  /* layer 2: the decoder's marks and spaces in this channel */
  double   ma, mb, sa2, sb2;
  cplx     mab, sab2;
  guint    mn, sn;
  Cov      rm2, rs2;           /* exponentially averaged                   */
  double   l2_s;               /* seconds of marks learnt                  */
  double   l2_idle_s;
  gboolean l2_ok, layer2;
  float   *hist;               /* the last HIST frames of A and B (a, b)   */
  guint    hpos;
  cplx     bw[2];          /* the band-level weight (no station steering),
                            * turned and scaled: c = conj(w)               */
} Ch;

typedef struct {
  double p;
  guint  ch;
} Quiet;

struct _SkimCombiner {
  guint   nchan, ngroup;
  double  rate;
  SkimCombinerConfig cfg;
  gboolean hold;
  Ch     *ch;
  Cov    *grp;             /* R_n per group (exponentially averaged)        */
  Cov    *grp_inst;        /* the last tick's, unaveraged                   */
  gboolean grp_ok;
  double *coh;             /* per group                                     */
  Quiet  *qs;              /* scratch: a group's channels by power          */
  guint64 updates;
};

void skim_combiner_config_default(SkimCombinerConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->mode       = SKIM_PHASE_MVDR;
  cfg->tc_s       = 1.0;
  cfg->gate_db    = 6.0;
  cfg->hold_s     = 30.0;
  cfg->group      = 32;
  cfg->quiet_frac = 0.3;
  cfg->layer2     = TRUE;
  cfg->tc2_s      = 5.0;
}

static const char *const mode_names[] = { "mvdr", "mrc", "fixed", "a", "b" };

gboolean skim_phase_mode_parse(const char *s, SkimPhaseMode *out) {
  for (guint i = 0; s && i < G_N_ELEMENTS(mode_names); i++) {
    if (g_ascii_strcasecmp(s, mode_names[i]) == 0) {
      *out = (SkimPhaseMode)i;
      return TRUE;
    }
  }
  return FALSE;
}

const char *skim_phase_mode_name(SkimPhaseMode m) {
  return (guint)m < G_N_ELEMENTS(mode_names) ? mode_names[m] : "?";
}

/* A channel's place in frequency order: the lowest offset first. */
static guint pos_of(const SkimCombiner *c, guint chan) {
  return (chan + c->nchan / 2) % c->nchan;
}

static guint chan_at(const SkimCombiner *c, guint pos) {
  return (pos + c->nchan - c->nchan / 2) % c->nchan;
}

static void ch_reset(Ch *h) {
  memset(h, 0, sizeof(*h));
  h->c0[0] = h->c1[0] = 1.0;
  h->drop_db = h->drop_est_db = h->gain_est_db = h->align = NAN;
  h->share_b = 0.0;
  h->bw[0] = 1.0;
  h->bw[1] = 0.0;
}

SkimCombiner *skim_combiner_new(guint nchan, double rate,
                                const SkimCombinerConfig *cfg) {
  if (nchan < 8 || rate <= 0)
    return NULL;
  SkimCombiner *c = g_new0(SkimCombiner, 1);
  c->nchan = nchan;
  c->rate  = rate;
  if (cfg) { c->cfg = *cfg; } else { skim_combiner_config_default(&c->cfg); }
  if (c->cfg.group < 4) { c->cfg.group = 4; }
  if (c->cfg.group > nchan) { c->cfg.group = nchan; }
  if (c->cfg.quiet_frac <= 0 || c->cfg.quiet_frac > 1) { c->cfg.quiet_frac = 0.3; }
  if (c->cfg.tc_s <= 0) { c->cfg.tc_s = 1.0; }
  if (c->cfg.hold_s <= 0) { c->cfg.hold_s = 30.0; }
  if (c->cfg.tc2_s <= 0) { c->cfg.tc2_s = 5.0; }
  c->ngroup = (nchan + c->cfg.group - 1) / c->cfg.group;
  c->ch = g_new0(Ch, nchan);                   /* hist NULL: reset keeps it */
  c->grp = g_new0(Cov, c->ngroup);
  c->grp_inst = g_new0(Cov, c->ngroup);
  c->coh = g_new(double, c->ngroup);
  c->qs = g_new(Quiet, c->cfg.group);
  skim_combiner_reset(c);
  return c;
}

void skim_combiner_free(SkimCombiner *c) {
  if (!c)
    return;
  for (guint k = 0; k < c->nchan; k++) { g_free(c->ch[k].hist); }
  g_free(c->ch);
  g_free(c->grp);
  g_free(c->grp_inst);
  g_free(c->coh);
  g_free(c->qs);
  g_free(c);
}

void skim_combiner_reset(SkimCombiner *c) {
  for (guint k = 0; k < c->nchan; k++) {
    float *hist = c->ch[k].hist;               /* kept: the history buffer */
    ch_reset(&c->ch[k]);
    c->ch[k].hist = hist;
  }
  memset(c->grp, 0, c->ngroup * sizeof(Cov));
  memset(c->grp_inst, 0, c->ngroup * sizeof(Cov));
  for (guint g = 0; g < c->ngroup; g++) { c->coh[g] = NAN; }
  c->grp_ok = FALSE;
  c->updates = 0;
}

void skim_combiner_set_mode(SkimCombiner *c, SkimPhaseMode mode) { c->cfg.mode = mode; }
SkimPhaseMode skim_combiner_mode(const SkimCombiner *c) { return c->cfg.mode; }
void skim_combiner_hold(SkimCombiner *c, gboolean hold) { c->hold = hold; }
guint64 skim_combiner_updates(const SkimCombiner *c) { return c->updates; }

void skim_combiner_set_fixed(SkimCombiner *c, double gain_db, double phase_deg) {
  c->cfg.fixed_gain_db = gain_db;
  c->cfg.fixed_phase_deg = phase_deg;
}

void skim_combiner_process(SkimCombiner *c, guint chan, const float *a,
                           const float *b, float *y, guint n) {
  if (chan >= c->nchan)
    return;
  Ch *h = &c->ch[chan];
  /* antenna A alone, the ramp done: the output is a, bit for bit */
  const gboolean copy_a = h->ramp_pos >= h->ramp_len && h->c1[0] == 1.0 &&
                          h->c1[1] == 0.0;
  double sa = 0, sb = 0, ga = 0, gb = 0;
  cplx sab = 0, gab = 0;
  guint gn = 0;
  const double thr = h->thr;
  for (guint i = 0; i < n; i++) {
    const float ar = a[2 * i], ai = a[2 * i + 1];
    const float br = b[2 * i], bi = b[2 * i + 1];
    const double pa = (double)ar * ar + (double)ai * ai;
    const double pb = (double)br * br + (double)bi * bi;
    const cplx ab = ((double)ar * br + (double)ai * bi) +
                    I * ((double)ai * br - (double)ar * bi);
    sa += pa;
    sb += pb;
    sab += ab;
    if (thr > 0 && pa + pb > thr) {
      ga += pa;
      gb += pb;
      gab += ab;
      gn++;
    }
    if (copy_a) {
      y[2 * i] = ar;
      y[2 * i + 1] = ai;
      continue;
    }
    cplx ca = h->c1[0], cb = h->c1[1];
    if (h->ramp_pos < h->ramp_len) {
      const double t = (double)(++h->ramp_pos) / h->ramp_len;
      ca = h->c0[0] + t * (h->c1[0] - h->c0[0]);
      cb = h->c0[1] + t * (h->c1[1] - h->c0[1]);
    }
    const cplx out = ca * (ar + I * ai) + cb * (br + I * bi);
    y[2 * i] = (float)creal(out);
    y[2 * i + 1] = (float)cimag(out);
  }
  h->sa += sa;
  h->sb += sb;
  h->sab += sab;
  h->n += n;
  h->ga += ga;
  h->gb += gb;
  h->gab += gab;
  h->gn += gn;
}

#define HIST 128

void skim_combiner_mark(SkimCombiner *c, guint chan, const float *a, const float *b,
                        const float *p, guint n, guint delay) {
  if (!c || chan >= c->nchan || !c->cfg.layer2 || !p || delay > 64)
    return;
  if (n > 64) {                                /* the history holds 64 + 64 */
    for (guint o = 0; o < n; o += 64) {
      const guint m = MIN(64u, n - o);
      skim_combiner_mark(c, chan, a + 2 * o, b + 2 * o, p + o, m, delay);
    }
    return;
  }
  Ch *h = &c->ch[chan];
  if (!h->hist) { h->hist = g_new0(float, 4 * HIST); }
  const guint start = h->hpos;                 /* the block's first frame  */
  for (guint i = 0; i < n; i++) {              /* A and B of this block in */
    float *x = h->hist + 4 * ((start + i) % HIST);
    x[0] = a[2 * i];
    x[1] = a[2 * i + 1];
    x[2] = b[2 * i];
    x[3] = b[2 * i + 1];
  }
  h->hpos = (start + n) % HIST;
  for (guint i = 0; i < n; i++) {
    if (p[i] < 0)
      continue;
    const float *x = h->hist + 4 * ((start + i + HIST - delay % HIST) % HIST);
    const float ar = x[0], ai = x[1], br = x[2], bi = x[3];
    const double pa = (double)ar * ar + (double)ai * ai;
    const double pb = (double)br * br + (double)bi * bi;
    const cplx ab = ((double)ar * br + (double)ai * bi) + I * ((double)ai * br - (double)ar * bi);
    if (p[i] >= 0.75f) {
      h->ma += pa;
      h->mb += pb;
      h->mab += ab;
      h->mn++;
    } else if (p[i] <= 0.25f) {
      h->sa2 += pa;
      h->sb2 += pb;
      h->sab2 += ab;
      h->sn++;
    }
  }
}

/* SINR of weight w for a station λ·v·vᴴ in noise r. */
static double sinr_of(const Cov *r, double lam, const cplx v[2], const cplx w[2]) {
  const cplx wv = conj(w[0]) * v[0] + conj(w[1]) * v[1];
  const double q = creal(conj(w[0]) * w[0]) * r->r11 + creal(conj(w[1]) * w[1]) * r->r22 +
                   2.0 * creal(conj(w[0]) * r->r12 * w[1]);
  return q > 0 ? lam * creal(wv * conj(wv)) / q : 0;
}

static int cmp_quiet(const void *x, const void *y) {
  const Quiet *a = x, *b = y;
  return a->p < b->p ? -1 : a->p > b->p ? 1 : (a->ch > b->ch) - (a->ch < b->ch);
}

static void cov_ema(Cov *r, const Cov *x, double alpha) {
  r->r11 += alpha * (x->r11 - r->r11);
  r->r22 += alpha * (x->r22 - r->r22);
  r->r12 += alpha * (x->r12 - r->r12);
}

/* R_n at a frequency position: linear between the groups' centres. */
static Cov rn_at(const SkimCombiner *c, const Cov *grp, guint pos) {
  const double G = c->cfg.group;
  double x = ((double)pos + 0.5) / G - 0.5;  /* in units of group centres */
  if (x < 0) { x = 0; }
  if (x > c->ngroup - 1) { x = c->ngroup - 1; }
  const guint g0 = (guint)floor(x);
  const guint g1 = MIN(g0 + 1, c->ngroup - 1);
  const double t = x - g0;
  Cov r = grp[g0];
  if (g1 != g0 && t > 0) {
    r.r11 += t * (grp[g1].r11 - r.r11);
    r.r22 += t * (grp[g1].r22 - r.r22);
    r.r12 += t * (grp[g1].r12 - r.r12);
  }
  return r;
}

/* wᴴ R w */
static double quad(const Cov *r, const cplx w[2]) {
  return creal(conj(w[0]) * w[0]) * r->r11 + creal(conj(w[1]) * w[1]) * r->r22 +
         2.0 * creal(conj(w[0]) * r->r12 * w[1]);
}

/* w = R⁻¹ v for a 2×2 Hermitian R (loaded); FALSE when singular. */
static gboolean solve(const Cov *r, const cplx v[2], cplx w[2]) {
  const double det = r->r11 * r->r22 - creal(r->r12 * conj(r->r12));
  if (!(det > 0))
    return FALSE;
  w[0] = (r->r22 * v[0] - r->r12 * v[1]) / det;
  w[1] = (-conj(r->r12) * v[0] + r->r11 * v[1]) / det;
  return TRUE;
}

/* The principal eigenvector of a 2×2 Hermitian matrix; its eigenvalue. */
static double principal(const Cov *r, cplx v[2]) {
  const double m = 0.5 * (r->r11 + r->r22);
  const double d = 0.5 * (r->r11 - r->r22);
  const double lam = m + sqrt(d * d + creal(r->r12 * conj(r->r12)));
  const cplx u0[2] = { r->r12, lam - r->r11 };
  const cplx u1[2] = { lam - r->r22, conj(r->r12) };
  const double n0 = cabs(u0[0]) + cabs(u0[1]), n1 = cabs(u1[0]) + cabs(u1[1]);
  const cplx *u = n0 >= n1 ? u0 : u1;
  const double nn = sqrt(creal(u[0] * conj(u[0])) + creal(u[1] * conj(u[1])));
  if (!(nn > 0)) {
    v[0] = 1.0;
    v[1] = 0.0;
  } else {
    v[0] = u[0] / nn;
    v[1] = u[1] / nn;
  }
  return lam;
}

/* The adaptive weight of one channel (w: y = wᴴx; MVDR, or MRC in mode
 * MRC), whatever the mode applies: the estimates ("what phasing would
 * give") come from it in modes A, B and Fixed too. Sets h->steered, the
 * station's signature v and its eigenvalue. */
static void adaptive(const SkimCombiner *c, Ch *h, const Cov *rn, cplx w[2],
                     cplx v[2], double *lam_out) {
  h->steered = FALSE;
  *lam_out = 0;
  Cov r = *rn;
  if (c->cfg.mode == SKIM_PHASE_MRC) { r.r12 = 0; }
  const double load = LOADING * (rn->r11 + rn->r22);
  r.r11 += load;
  r.r22 += load;
  v[0] = 1.0;
  v[1] = 0.0;
  if (h->rs_ok) {
    Cov sig = { h->rs.r11 - rn->r11, h->rs.r22 - rn->r22, h->rs.r12 - rn->r12 };
    cplx u[2];
    const double lam = principal(&sig, u);
    if (lam > MIN_SIG_REL * 0.5 * (rn->r11 + rn->r22)) {
      v[0] = u[0];
      v[1] = u[1];
      h->steered = TRUE;
      *lam_out = lam;
    }
  }
  if (!solve(&r, v, w)) {
    w[0] = 1.0;
    w[1] = 0.0;
  }
}

/* The weight the mode applies (before turning and scaling). */
static void applied(const SkimCombiner *c, const cplx ad[2], cplx w[2]) {
  switch (c->cfg.mode) {
  case SKIM_PHASE_A:
    w[0] = 1.0;
    w[1] = 0.0;
    return;
  case SKIM_PHASE_B:
    w[0] = 0.0;
    w[1] = 1.0;
    return;
  case SKIM_PHASE_FIXED: {
    const double g = pow(10.0, c->cfg.fixed_gain_db / 20.0);
    const double ph = c->cfg.fixed_phase_deg * G_PI / 180.0;
    w[0] = 1.0;
    w[1] = conj(g * cexp(I * ph));   /* y = a + g·e^{jφ}·b */
    return;
  }
  case SKIM_PHASE_MVDR:
  case SKIM_PHASE_MRC:
    break;
  }
  w[0] = ad[0];
  w[1] = ad[1];
}

/* The floor a weight takes off antenna A's, A's signals kept at their gain,
 * on the noise covariance r; NAN where the weight barely uses A. */
static double drop_of(const Cov *r, const cplx w[2]) {
  const double pa2 = creal(w[0] * conj(w[0])), pb2 = creal(w[1] * conj(w[1]));
  const double q = quad(r, w);
  return (pa2 >= MIN_A_SHARE * (pa2 + pb2) && q > 0 && r->r11 > 0)
             ? 10.0 * log10(r->r11 * pa2 / q) : NAN;
}

/* w turned (A's component real, positive) and scaled to A's noise on rn;
 * the coefficients c = conj(w) out. */
static void turn_scale(const Cov *rn, cplx w[2], cplx out[2]) {
  const double wa2 = creal(w[0] * conj(w[0])), wb2 = creal(w[1] * conj(w[1]));
  if (wa2 > 1e-12 * (wa2 + wb2)) {
    const cplx rot = conj(w[0]) / sqrt(wa2);
    w[0] = creal(w[0] * rot);
    w[1] *= rot;
  } else if (wb2 > 0) {
    const cplx rot = conj(w[1]) / sqrt(wb2);
    w[1] = creal(w[1] * rot);
    w[0] = 0;
  }
  const double q = quad(rn, w);
  const double s = q > 0 && rn->r11 > 0 ? sqrt(rn->r11 / q) : 1.0;
  out[0] = conj(w[0] * s);
  out[1] = conj(w[1] * s);
}

/* The band-level weight of a channel: the group's interference canceller
 * (MVDR towards e_A, no station steering — smooth across the band), or the
 * mode's fixed one; scaled so that A's own signals keep their gain. */
static void band_weight(const SkimCombiner *c, const Cov *rn, cplx out[2]) {
  cplx w[2] = { 1.0, 0.0 };
  switch (c->cfg.mode) {
  case SKIM_PHASE_A:
  case SKIM_PHASE_MRC:                     /* no preference, no null: A    */
    out[0] = 1.0;
    out[1] = 0.0;
    return;
  case SKIM_PHASE_B:
    w[0] = 0.0;
    w[1] = 1.0;
    break;
  case SKIM_PHASE_FIXED: {
    const double g = pow(10.0, c->cfg.fixed_gain_db / 20.0);
    w[1] = conj(g * cexp(I * c->cfg.fixed_phase_deg * G_PI / 180.0));
    break;
  }
  case SKIM_PHASE_MVDR: {
    Cov r = *rn;
    const double load = LOADING * (rn->r11 + rn->r22);
    r.r11 += load;
    r.r22 += load;
    const cplx e[2] = { 1.0, 0.0 };
    if (!solve(&r, e, w)) {
      w[0] = 1.0;
      w[1] = 0.0;
    }
    break;
  }
  }
  /* A's signals kept at their gain (w_A = 1): the interference leaves the
   * floor, the stations stay — what a waterfall shows plainly. B alone (no
   * A in it) is scaled to A's floor instead. */
  const double wa = cabs(w[0]);
  if (c->cfg.mode != SKIM_PHASE_B && wa > 1e-9 * (wa + cabs(w[1]))) {
    out[0] = 1.0;
    out[1] = conj(w[1] / w[0]);
    return;
  }
  turn_scale(rn, w, out);
}

void skim_combiner_update(SkimCombiner *c) {
  /* 1. every channel's tick covariance */
  double tick_s = 0;
  for (guint k = 0; k < c->nchan; k++) {
    Ch *h = &c->ch[k];
    if (h->n > 0) {
      h->inst.r11 = h->sa / h->n;
      h->inst.r22 = h->sb / h->n;
      h->inst.r12 = h->sab / h->n;
      tick_s = MAX(tick_s, h->n / c->rate);
    }
  }
  if (tick_s <= 0)
    return;
  const double alpha = 1.0 - exp(-tick_s / c->cfg.tc_s);

  /* 2. R_n per group: the quietest channels' covariances, averaged */
  for (guint g = 0; g < c->ngroup; g++) {
    guint m = 0;
    for (guint p = g * c->cfg.group; p < MIN((g + 1) * c->cfg.group, c->nchan); p++) {
      const guint k = chan_at(c, p);
      const Ch *h = &c->ch[k];
      if (h->n == 0)
        continue;
      c->qs[m].p = h->inst.r11 + h->inst.r22;
      c->qs[m].ch = k;
      m++;
    }
    if (m == 0)
      continue;
    qsort(c->qs, m, sizeof(Quiet), cmp_quiet);
    const guint q = MAX(1u, (guint)ceil(c->cfg.quiet_frac * m));
    Cov x = { 0 };
    for (guint s = 0; s < q; s++) {
      const Cov *r = &c->ch[c->qs[s].ch].inst;
      x.r11 += r->r11 / q;
      x.r22 += r->r22 / q;
      x.r12 += r->r12 / q;
    }
    c->grp_inst[g] = x;
    if (!c->grp_ok) { c->grp[g] = x; } else { cov_ema(&c->grp[g], &x, alpha); }
    const Cov *r = &c->grp[g];
    c->coh[g] = r->r11 > 0 && r->r22 > 0
                    ? creal(r->r12 * conj(r->r12)) / (r->r11 * r->r22) : NAN;
  }
  c->grp_ok = TRUE;

  /* 3. every channel: R_s, the weight, its ramp */
  const double gate = pow(10.0, c->cfg.gate_db / 10.0);
  for (guint k = 0; k < c->nchan; k++) {
    Ch *h = &c->ch[k];
    const guint pos = pos_of(c, k);
    const Cov rn = rn_at(c, c->grp, pos);
    const Cov rn_inst = rn_at(c, c->grp_inst, pos);
    const double min_gated = MIN_GATED_S * c->rate;
    if (h->gn >= min_gated) {
      Cov x = { h->ga / h->gn, h->gb / h->gn, h->gab / h->gn };
      if (!h->rs_ok) { h->rs = x; } else { cov_ema(&h->rs, &x, alpha); }
      h->rs_ok = TRUE;
      h->rs_idle_s = 0;
    } else if (h->rs_ok && (h->rs_idle_s += tick_s) > c->cfg.hold_s) {
      h->rs_ok = FALSE;
    }
    h->thr = gate * (rn.r11 + rn.r22);

    cplx ad[2], v[2], w[2];
    double lam;
    adaptive(c, h, &rn, ad, v, &lam);
    /* layer 2: the decoder's marks and spaces of this tick */
    h->layer2 = FALSE;
    if (c->cfg.layer2) {
      const double min_n = MIN_GATED_S * c->rate;
      if (h->mn >= min_n && h->sn >= min_n) {
        const double a2 = 1.0 - exp(-tick_s / c->cfg.tc2_s);
        const Cov xm = { h->ma / h->mn, h->mb / h->mn, h->mab / h->mn };
        const Cov xs = { h->sa2 / h->sn, h->sb2 / h->sn, h->sab2 / h->sn };
        if (h->l2_s <= 0) {
          h->rm2 = xm;
          h->rs2 = xs;
        } else {
          cov_ema(&h->rm2, &xm, a2);
          cov_ema(&h->rs2, &xs, a2);
        }
        h->l2_s += h->mn / c->rate;
        h->l2_idle_s = 0;
      } else if (h->l2_s > 0 && (h->l2_idle_s += tick_s) > c->cfg.hold_s) {
        h->l2_s = 0;                           /* the decoder let go of it */
      }
      h->l2_ok = h->l2_s >= 3.0;
      if (h->l2_ok && (c->cfg.mode == SKIM_PHASE_MVDR || c->cfg.mode == SKIM_PHASE_MRC)) {
        Cov sig = { h->rm2.r11 - h->rs2.r11, h->rm2.r22 - h->rs2.r22, h->rm2.r12 - h->rs2.r12 };
        cplx v2[2];
        const double lam2 = principal(&sig, v2);
        Cov r2 = h->rs2;
        if (c->cfg.mode == SKIM_PHASE_MRC) { r2.r12 = 0; }
        const double load = LOADING * (h->rs2.r11 + h->rs2.r22);
        r2.r11 += load;
        r2.r22 += load;
        cplx w2[2];
        if (lam2 > 0.02 * 0.5 * (h->rs2.r11 + h->rs2.r22) && solve(&r2, v2, w2)) {
          /* on the same estimates, only where it is better than layer 1 */
          const double s2 = sinr_of(&h->rs2, lam2, v2, w2), s1 = sinr_of(&h->rs2, lam2, v2, ad);
          if (s2 > 1.122 * s1) {
            ad[0] = w2[0];
            ad[1] = w2[1];
            v[0] = v2[0];
            v[1] = v2[1];
            lam = lam2;
            h->layer2 = TRUE;
            h->steered = TRUE;
          }
        }
      }
      h->ma = h->mb = h->sa2 = h->sb2 = 0;
      h->mab = h->sab2 = 0;
      h->mn = h->sn = 0;
    }
    applied(c, ad, w);
    /* what the adaptive weight gives, whatever the mode applies */
    h->drop_est_db = drop_of(&rn_inst, ad);
    h->gain_est_db = NAN;
    h->align = NAN;
    if (h->steered) {
      /* the station alone: λ·v·vᴴ; the SINR of A, B and the weight */
      const double na = rn.r11, nb = rn.r22;
      const double sa = lam * creal(v[0] * conj(v[0])) / na;
      const double sb = lam * creal(v[1] * conj(v[1])) / nb;
      const cplx wv = conj(ad[0]) * v[0] + conj(ad[1]) * v[1];
      const double qy = quad(&rn, ad);
      const double sy = qy > 0 ? lam * creal(wv * conj(wv)) / qy : 0;
      const double best = MAX(sa, sb);
      if (best > 0 && sy > 0) { h->gain_est_db = 10.0 * log10(sy / best); }
      /* the station's signature against the noise's own direction */
      cplx un[2];
      principal(&rn, un);
      const cplx d = conj(un[0]) * v[0] + conj(un[1]) * v[1];
      h->align = creal(d * conj(d));
    }
    /* the phase reference: A's component real and positive */
    const double wa2 = creal(w[0] * conj(w[0])), wb2 = creal(w[1] * conj(w[1]));
    if (wa2 > 1e-12 * (wa2 + wb2)) {
      const cplx rot = conj(w[0]) / sqrt(wa2);
      w[0] *= rot;
      w[1] *= rot;
      w[0] = creal(w[0]);
    } else if (wb2 > 0) {
      const cplx rot = conj(w[1]) / sqrt(wb2);
      w[1] = creal(w[1] * rot);
      w[0] = 0;
    }
    /* the output's noise = A's noise */
    const double q = quad(&rn, w);
    const double s = q > 0 && rn.r11 > 0 ? sqrt(rn.r11 / q) : 1.0;
    if (c->cfg.mode != SKIM_PHASE_A) {
      w[0] *= s;
      w[1] *= s;
    }
    if (!c->hold) { band_weight(c, &rn, h->bw); }
    if (!c->hold) {
      /* the ramp starts where the output is now */
      cplx now0 = h->c1[0], now1 = h->c1[1];
      if (h->ramp_pos < h->ramp_len) {
        const double t = (double)h->ramp_pos / h->ramp_len;
        now0 = h->c0[0] + t * (h->c1[0] - h->c0[0]);
        now1 = h->c0[1] + t * (h->c1[1] - h->c0[1]);
      }
      h->c0[0] = now0;
      h->c0[1] = now1;
      if (c->cfg.mode == SKIM_PHASE_A) {
        h->c1[0] = 1.0;
        h->c1[1] = 0.0;
      } else {
        h->c1[0] = conj(w[0]);
        h->c1[1] = conj(w[1]);
      }
      h->ramp_pos = 0;
      h->ramp_len = h->n > 0 ? h->n : (guint)(c->rate * tick_s);
      if (h->c0[0] == h->c1[0] && h->c0[1] == h->c1[1]) { h->ramp_len = 0; }
    }
    /* the numbers of the weight the output is heading for (held or new) */
    const cplx wa[2] = { conj(h->c1[0]), conj(h->c1[1]) };
    const double pa2 = creal(wa[0] * conj(wa[0])), pb2 = creal(wa[1] * conj(wa[1]));
    h->drop_db = drop_of(&rn_inst, wa);
    const double na = pa2 * rn.r11, nb = pb2 * rn.r22;
    h->share_b = na + nb > 0 ? nb / (na + nb) : 0.0;
    h->sa = h->sb = h->ga = h->gb = 0;
    h->sab = h->gab = 0;
    h->n = h->gn = 0;
  }
  c->updates++;
}

gboolean skim_combiner_chan_info(const SkimCombiner *c, guint chan,
                                 SkimCombinerChanInfo *out) {
  if (!c || chan >= c->nchan)
    return FALSE;
  const Ch *h = &c->ch[chan];
  out->ca[0] = (float)creal(h->c1[0]);
  out->ca[1] = (float)cimag(h->c1[0]);
  out->cb[0] = (float)creal(h->c1[1]);
  out->cb[1] = (float)cimag(h->c1[1]);
  out->steered = h->steered;
  out->drop_db = c->updates ? h->drop_db : NAN;
  out->share_b = h->share_b;
  out->drop_est_db = c->updates ? h->drop_est_db : NAN;
  out->gain_est_db = h->gain_est_db;
  out->align = h->align;
  out->layer2 = h->layer2;
  return TRUE;
}

gboolean skim_combiner_band_coef(const SkimCombiner *c, guint chan, float ca[2], float cb[2]) {
  if (!c || chan >= c->nchan)
    return FALSE;
  const Ch *h = &c->ch[chan];
  ca[0] = (float)creal(h->bw[0]);
  ca[1] = (float)cimag(h->bw[0]);
  cb[0] = (float)creal(h->bw[1]);
  cb[1] = (float)cimag(h->bw[1]);
  return TRUE;
}

guint skim_combiner_groups(const SkimCombiner *c) { return c->ngroup; }

guint skim_combiner_group_of(const SkimCombiner *c, guint chan) {
  return chan < c->nchan ? pos_of(c, chan) / c->cfg.group : 0;
}

double skim_combiner_coherence(const SkimCombiner *c, guint group) {
  return group < c->ngroup ? c->coh[group] : NAN;
}
