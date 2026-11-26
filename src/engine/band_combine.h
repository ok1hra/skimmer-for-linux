/* band_combine.h — a phased band's own IQ from both antennas
 * (docs/PHASING.md, "Band-level output").
 *
 * The decoders get the combination per channel (combiner.h); the consumers
 * that read the band's IQ — the waterfall, FT8/FT4, JS8, WSPR — get it here:
 * y(f) = ca(f)·A(f) + cb(f)·B(f) on a short-time FFT of the band (weighted
 * overlap-add: √Hann analysis and synthesis, half overlapping, so a unit
 * weight gives A back exactly, delayed). The weights come per channel from
 * the combiner (skim_combiner_band_coef: the group's interference canceller,
 * smooth across the band) and are interpolated between the channels'
 * centres to every bin.
 *
 * The output is as long as the input, nfft frames late (the first nfft/2 of
 * them silence). Threading: one thread feeds it; set_weights from the same
 * thread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_BAND_COMBINE_H
#define SKIMMER_BAND_COMBINE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimBandCombine SkimBandCombine;

/* A band at rate Hz, FFT of nfft (even; 0 = 2048 at ≤ 96 kHz, rate/48 bins
 * of ~47 Hz above). */
SkimBandCombine *skim_band_combine_new(double rate, guint nfft);
void             skim_band_combine_free(SkimBandCombine *bc);

/* The channels' coefficients in frequency order ({re, im} pairs, n of
 * each): channel i sits at first_hz + i·spacing_hz from the band centre. */
void skim_band_combine_set_weights(SkimBandCombine *bc, const float *ca, const float *cb,
                                   guint n, double first_hz, double spacing_hz);

/* n frames of A and of B in, n frames of the combination out (may be a). */
void skim_band_combine_process(SkimBandCombine *bc, const float *a, const float *b,
                               float *out, guint n);

/* Forget the frames in hand (silence again), keep the weights. */
void skim_band_combine_reset(SkimBandCombine *bc);

G_END_DECLS

#endif /* SKIMMER_BAND_COMBINE_H */
