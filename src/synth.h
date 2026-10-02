/*
 * synth — synthetic CW for the offline benches: keyed envelopes with a
 * human fist, keying edges, slow fading. Test-side only (not the engine).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIM_SYNTH_H
#define SKIM_SYNTH_H

#include <glib.h>

typedef struct {
  double wpm;      /* PARIS speed: dit = 1.2 / wpm s                        */
  double jitter;   /* sd of every element and gap, as a fraction (0 = keyer) */
} SynthFist;

/* Standard normal deviate (Box–Muller). */
double synth_gauss(GRand *rng);

/* Append `secs` of key-up to env (float, 0/1) at env_rate. */
void synth_silence(GArray *env, double secs, double env_rate);

/* Append the keyed text (A–Z, 0–9, '/', ' ') to env at env_rate, followed by
 * the 3-dit gap of its last character. Unknown characters are skipped. */
void synth_key(GArray *env, const char *text, const SynthFist *fist,
               double env_rate, GRand *rng);

/* Raised-cosine keying edges of rise_s (a real transmitter's shaping). */
void synth_shape(GArray *env, double env_rate, double rise_s);

/* n samples at gain_rate of a slow complex Gaussian fade (Rayleigh
 * amplitude) with Doppler spread fd_hz, scaled to unit mean power. */
void synth_fading(float *re, float *im, guint n, double gain_rate,
                  double fd_hz, GRand *rng);

#endif
