/* source_mock.c — synthetic CW (and RTTY) on every band, in real time
 * ([radio] type=mock). Not a radio: the headless gate runs the whole
 * skimmer with it — config, pipelines, feed, status — on a machine with no
 * Red Pitaya.
 *
 * Each band carries one keyed tone MOCK_OFFSET_HZ above its centre: "CQ CQ
 * DE OK1MCK OK1MCK K" at MOCK_WPM, a pause, again — over a little noise. A
 * band that decodes RTTY also gets "CQ CQ DE OK1RTY OK1RTY K" in 45.45 Bd
 * FSK, 170 Hz shift, MOCK_RTTY_OFFSET_HZ from its centre (mark high).
 * Blocks leave at the wall-clock pace of the configured rate, so the feed's
 * settle timers (monotonic clock) see the same time as on air.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "source.h"

#include <math.h>
#include <string.h>

#define MOCK_CALL      "OK1MCK"
#define MOCK_TEXT      "CQ CQ DE " MOCK_CALL " " MOCK_CALL " K"
#define MOCK_WPM       25.0
#define MOCK_OFFSET_HZ 1000.0
#define MOCK_PAUSE_S   3.0
#define MOCK_AMP       0.05f
#define MOCK_NOISE     0.004
#define MOCK_BLOCK     1024             /* frames per block                  */
#define MOCK_RTTY_CALL "OK1RTY"
#define MOCK_RTTY_TEXT "CQ CQ DE " MOCK_RTTY_CALL " " MOCK_RTTY_CALL " K"
#define MOCK_RTTY_OFFSET_HZ (-10000.0)
#define MOCK_BAUD      45.45
#define MOCK_SHIFT_HZ  170.0

typedef struct {
  SkimIqSource  base;
  GThread      *th;
  volatile gint stop;
  gint          blocks;                 /* blocks sent (atomic)              */
  float        *env;                    /* one keyed message + pause         */
  guint         env_n;
  float        *fsk;                    /* RTTY: one message + pause, IQ     */
  guint         fsk_n;                  /* frames                            */
  char          state[160];
} Mock;

static const struct { char ch; const char *m; } MORSE[] = {
  { 'A', ".-" }, { 'B', "-..." }, { 'C', "-.-." }, { 'D', "-.." }, { 'E', "." },
  { 'F', "..-." }, { 'G', "--." }, { 'H', "...." }, { 'I', ".." }, { 'J', ".---" },
  { 'K', "-.-" }, { 'L', ".-.." }, { 'M', "--" }, { 'N', "-." }, { 'O', "---" },
  { 'P', ".--." }, { 'Q', "--.-" }, { 'R', ".-." }, { 'S', "..." }, { 'T', "-" },
  { 'U', "..-" }, { 'V', "...-" }, { 'W', ".--" }, { 'X', "-..-" }, { 'Y', "-.--" },
  { 'Z', "--.." }, { '0', "-----" }, { '1', ".----" }, { '2', "..---" },
  { '3', "...--" }, { '4', "....-" }, { '5', "....." }, { '6', "-...." },
  { '7', "--..." }, { '8', "---.." }, { '9', "----." },
};

static void key(GArray *env, double secs, double rate, float on) {
  const guint n = (guint)(secs * rate + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}

/* The message keyed at MOCK_WPM (PARIS timing), a pause after it. */
static float *keyed(double rate, guint *n_out) {
  const double dit = 1.2 / MOCK_WPM;
  GArray *env = g_array_new(FALSE, FALSE, sizeof(float));
  for (const char *c = MOCK_TEXT; *c; c++) {
    if (*c == ' ') { key(env, 4 * dit, rate, 0); continue; }   /* + 3 after char = 7 */
    for (guint k = 0; k < G_N_ELEMENTS(MORSE); k++) {
      if (MORSE[k].ch != *c) { continue; }
      for (const char *e = MORSE[k].m; *e; e++) {
        key(env, (*e == '-' ? 3 : 1) * dit, rate, 1);
        key(env, dit, rate, 0);
      }
    }
    key(env, 2 * dit, rate, 0);
  }
  key(env, MOCK_PAUSE_S, rate, 0);
  /* a 4 ms moving average rounds the keying edges (no clicks) */
  const float *e = (const float *)env->data;
  const guint ramp = MAX(1u, (guint)(0.004 * rate));
  float *out = g_new0(float, env->len);
  double acc = 0;
  for (guint i = 0; i < env->len; i++) {
    acc += e[i] - (i >= ramp ? e[i - ramp] : 0.0f);
    out[i] = (float)(acc / ramp);
  }
  *n_out = env->len;
  g_array_free(env, TRUE);
  return out;
}

/* ITA2 letters (the figure shift only for digits) — mirrors decode_rtty.c */
static const char ITA2_LTRS[32] = {
  0,   'E', '\n', 'A', ' ', 'S', 'I', 'U', 0,   'D', 'R', 'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L',  'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0,   'M', 'X', 'V', 0,
};
static const char ITA2_FIGS[32] = {
  0,   '3', '\n', '-', ' ', 0,   '8', '7', 0,   '$', '4', '\'', ',', '!', ':', '(',
  '5', '"', ')',  '2', '#', '6', '0', '1', '9', '?', '&', 0,   '.', '/', ';', 0,
};
#define ITA2_FIGS_CODE 0x1B
#define ITA2_LTRS_CODE 0x1F

/* One character: start (space), 5 bits LSB first, 1.5 stop bits (mark). */
static void fsk_code(GArray *bits, guint code) {
  const gint8 start = 0, stop = 1;
  g_array_append_val(bits, start);
  for (guint k = 0; k < 5; k++) {
    const gint8 b = (gint8)((code >> k) & 1u);
    g_array_append_val(bits, b);
  }
  g_array_append_val(bits, stop);            /* 1.5: a half bit more, below  */
  const gint8 half = 2;
  g_array_append_val(bits, half);
}

/* The message in 45.45 Bd FSK as complex baseband at the offset: LTRS
 * diddles first (the idle an op keys; both tones, so the pair finder locks),
 * then the text, then MOCK_PAUSE_S of nothing. Continuous phase. */
static float *fsk_iq(double rate, guint *n_out) {
  GArray *bits = g_array_new(FALSE, FALSE, sizeof(gint8));
  for (guint i = 0; i < 8; i++) { fsk_code(bits, ITA2_LTRS_CODE); }
  gboolean figs = FALSE;
  for (const char *c = MOCK_RTTY_TEXT; *c; c++) {
    for (guint k = 1; k < 32; k++) {
      const gboolean in_l = ITA2_LTRS[k] == *c, in_f = !in_l && ITA2_FIGS[k] == *c;
      if (!in_l && !in_f) { continue; }
      if (in_f && !figs) { fsk_code(bits, ITA2_FIGS_CODE); figs = TRUE; }
      if (in_l && figs && *c != ' ') { fsk_code(bits, ITA2_LTRS_CODE); figs = FALSE; }
      fsk_code(bits, k);
      if (*c == ' ') { figs = FALSE; }            /* unshift on space         */
      break;
    }
  }
  for (guint i = 0; i < 3; i++) { fsk_code(bits, ITA2_LTRS_CODE); }
  const double spb = rate / MOCK_BAUD;
  GArray *iq = g_array_new(FALSE, FALSE, sizeof(float));
  double phase = 0, t = 0;
  guint n = 0;
  gint8 last = 1;
  for (guint i = 0; i < bits->len; i++) {
    gint8 b = g_array_index(bits, gint8, i);
    double len = spb;
    if (b == 2) { b = last; len = spb / 2; }       /* the half stop bit        */
    last = b;
    const double f = MOCK_RTTY_OFFSET_HZ + (b ? 0.5 : -0.5) * MOCK_SHIFT_HZ;
    const double dphi = 2.0 * G_PI * f / rate;
    for (t += len; n < (guint)t; n++) {
      const float re = (float)(MOCK_AMP * cos(phase)), im = (float)(MOCK_AMP * sin(phase));
      g_array_append_val(iq, re);
      g_array_append_val(iq, im);
      phase = fmod(phase + dphi, 2.0 * G_PI);
    }
  }
  const guint pause = (guint)(MOCK_PAUSE_S * rate);
  const float zero = 0;
  for (guint i = 0; i < 2 * pause; i++) { g_array_append_val(iq, zero); }
  g_array_free(bits, TRUE);
  *n_out = iq->len / 2;
  return (float *)g_array_free(iq, FALSE);
}

static double gauss(GRand *r) {
  const double u = g_rand_double_range(r, 1e-12, 1.0), v = g_rand_double(r);
  return sqrt(-2.0 * log(u)) * cos(2.0 * G_PI * v);
}

static gpointer run(gpointer data) {
  Mock *m = data;
  const SkimHdConfig *c = m->base.cfg;
  const double rate = c->rate;
  GRand *rng = g_rand_new_with_seed(1);
  float *iq = g_new(float, 2 * MOCK_BLOCK);
  double phase[SKIM_HD_MAX_BANDS] = { 0 };
  const double dphi = 2.0 * G_PI * MOCK_OFFSET_HZ / rate;
  guint pos = 0, fpos = 0;
  const gint64 t0 = g_get_monotonic_time();
  guint64 sent = 0;
  while (!g_atomic_int_get(&m->stop)) {
    for (guint b = 0; b < c->nb; b++) {
      guint p = pos;
      for (guint i = 0; i < MOCK_BLOCK; i++) {
        const float a = MOCK_AMP * m->env[p];
        iq[2 * i] = a * (float)cos(phase[b]) + (float)(MOCK_NOISE * gauss(rng));
        iq[2 * i + 1] = a * (float)sin(phase[b]) + (float)(MOCK_NOISE * gauss(rng));
        phase[b] = fmod(phase[b] + dphi, 2.0 * G_PI);
        if (++p == m->env_n) { p = 0; }
      }
      if (c->band[b].rtty) {
        guint q = fpos;
        for (guint i = 0; i < MOCK_BLOCK; i++) {
          iq[2 * i] += m->fsk[2 * q];
          iq[2 * i + 1] += m->fsk[2 * q + 1];
          if (++q == m->fsk_n) { q = 0; }
        }
      }
      m->base.iq_cb(b, iq, MOCK_BLOCK, rate, c->band[b].centre_hz, m->base.iq_user);
    }
    pos = (guint)(((guint64)pos + MOCK_BLOCK) % m->env_n);
    fpos = (guint)(((guint64)fpos + MOCK_BLOCK) % m->fsk_n);
    sent += MOCK_BLOCK;
    g_atomic_int_inc(&m->blocks);
    const gint64 due = t0 + (gint64)((double)sent / rate * G_USEC_PER_SEC);
    const gint64 wait = due - g_get_monotonic_time();
    if (wait > 0) { g_usleep((gulong)wait); }
  }
  g_free(iq);
  g_rand_free(rng);
  return NULL;
}

static void mock_start(SkimIqSource *s) {
  Mock *m = (Mock *)s;
  gboolean rtty = FALSE;
  for (guint b = 0; b < s->cfg->nb; b++) { rtty |= s->cfg->band[b].rtty; }
  g_snprintf(m->state, sizeof(m->state),
             "streaming — mock: %u band%s of synthetic CW (%s, %.0f wpm)%s",
             s->cfg->nb, s->cfg->nb == 1 ? "" : "s", MOCK_CALL, MOCK_WPM,
             rtty ? " + RTTY (" MOCK_RTTY_CALL ")" : "");
  g_message("radio: %s", m->state);
  m->th = g_thread_new("mock-iq", run, m);
}

static void mock_tick(SkimIqSource *s) { (void)s; }

static void mock_status(SkimIqSource *s, SkimIqSourceStatus *o) {
  Mock *m = (Mock *)s;
  memset(o, 0, sizeof(*o));
  o->streaming = m->th != NULL;
  g_strlcpy(o->state, m->state, sizeof(o->state));
  g_strlcpy(o->host, "mock", sizeof(o->host));
  o->packets = (guint64)g_atomic_int_get(&m->blocks);
}

static void mock_free(SkimIqSource *s) {
  Mock *m = (Mock *)s;
  if (m->th) {
    g_atomic_int_set(&m->stop, 1);
    g_thread_join(m->th);
  }
  g_free(m->env);
  g_free(m->fsk);
  g_free(m);
}

static const SkimIqSourceVtable MOCK_VT = { mock_start, mock_tick, mock_status, mock_free };

SkimIqSource *skim_iq_source_mock_new(const SkimHdConfig *cfg) {
  Mock *m = g_new0(Mock, 1);
  m->base.vt = &MOCK_VT;
  m->env = keyed(cfg->rate, &m->env_n);
  m->fsk = fsk_iq(cfg->rate, &m->fsk_n);
  return &m->base;
}
