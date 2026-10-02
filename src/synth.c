/*
 * synth — synthetic CW for the offline benches (see synth.h). The keying
 * follows src/cw_test.c; the fist jitter and the fading are new.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "synth.h"

#include <math.h>
#include <string.h>

static const struct { char c; const char *m; } MORSE[] = {
  {'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
  {'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
  {'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
  {'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
  {'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
  {'Z',"--.."},
  {'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
  {'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
  {'/',"-..-."},
};

static const char *morse_of(char c) {
  for (guint i = 0; i < G_N_ELEMENTS(MORSE); i++) {
    if (MORSE[i].c == c) { return MORSE[i].m; }
  }
  return NULL;
}

double synth_gauss(GRand *rng) {
  double u1 = 1.0 - g_rand_double(rng), u2 = g_rand_double(rng);
  return sqrt(-2.0 * log(u1)) * cos(2.0 * G_PI * u2);
}

static void run(GArray *env, double samps, float on) {
  guint n = (guint)(samps + 0.5);
  for (guint i = 0; i < n; i++) { g_array_append_val(env, on); }
}

void synth_silence(GArray *env, double secs, double env_rate) {
  run(env, secs * env_rate, 0.0f);
}

/* One element or gap of `units` dits, stretched by the fist's jitter (never
 * below a third of its nominal length — a fist is sloppy, not broken). */
static double span(double units, double dit, const SynthFist *f, GRand *rng) {
  double k = 1.0 + (f->jitter > 0 ? f->jitter * synth_gauss(rng) : 0.0);
  return units * dit * MAX(k, 0.33);
}

void synth_key(GArray *env, const char *text, const SynthFist *f,
               double env_rate, GRand *rng) {
  const double dit = 1.2 / f->wpm * env_rate;
  for (const char *p = text; *p; p++) {
    if (*p == ' ') {
      run(env, span(4, dit, f, rng), 0.0f);    /* + the 3 after the char = 7 */
      continue;
    }
    const char *m = morse_of(*p);
    if (!m) { continue; }
    for (const char *e = m; *e; e++) {
      run(env, span(*e == '-' ? 3 : 1, dit, f, rng), 1.0f);
      if (e[1]) { run(env, span(1, dit, f, rng), 0.0f); }
    }
    run(env, span(3, dit, f, rng), 0.0f);
  }
}

void synth_shape(GArray *env, double env_rate, double rise_s) {
  guint L = (guint)(env_rate * rise_s);
  if (L < 3 || env->len == 0) { return; }
  float *w = g_new(float, L);
  double sum = 0.0;
  for (guint i = 0; i < L; i++) {
    w[i] = 0.5f - 0.5f * (float)cos(2.0 * G_PI * i / (L - 1));
    sum += w[i];
  }
  for (guint i = 0; i < L; i++) { w[i] = (float)(w[i] / sum); }
  float *src = (float *)env->data;
  float *dst = g_new0(float, env->len);
  for (guint n = 0; n < env->len; n++) {
    float acc = 0.0f;
    guint kmax = MIN(L, n + 1);
    for (guint k = 0; k < kmax; k++) { acc += w[k] * src[n - k]; }
    dst[n] = acc;
  }
  memcpy(src, dst, env->len * sizeof(float));
  g_free(dst);
  g_free(w);
}

/* White complex Gaussian through two cascaded one-pole low-passes at fd:
 * a smooth fade whose amplitude is Rayleigh. Unit mean power over the
 * block, so a sweep point's SNR is its MEAN SNR. */
void synth_fading(float *re, float *im, guint n, double gain_rate,
                  double fd_hz, GRand *rng) {
  const double a = exp(-2.0 * G_PI * fd_hz / gain_rate);
  double r1 = 0, i1 = 0, r2 = 0, i2 = 0, pw = 0;
  const guint warm = (guint)(5.0 * gain_rate / fd_hz);
  for (guint k = 0; k < warm + n; k++) {
    r1 = a * r1 + (1 - a) * synth_gauss(rng);
    i1 = a * i1 + (1 - a) * synth_gauss(rng);
    r2 = a * r2 + (1 - a) * r1;
    i2 = a * i2 + (1 - a) * i1;
    if (k >= warm) {
      re[k - warm] = (float)r2;
      im[k - warm] = (float)i2;
      pw += r2 * r2 + i2 * i2;
    }
  }
  const double s = pw > 0 ? 1.0 / sqrt(pw / n) : 1.0;
  for (guint k = 0; k < n; k++) {
    re[k] = (float)(re[k] * s);
    im[k] = (float)(im[k] * s);
  }
}
