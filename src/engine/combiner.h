/* combiner.h — two antennas' channels into one, per channel (docs/PHASING.md).
 *
 * The band's IQ from antenna A (IN1) and antenna B (IN2) goes through two
 * identical channelizers; in every channel k the combiner forms
 *   y(n) = c_a(k)·a(n) + c_b(k)·b(n)
 * with weights estimated from the data, so a constant offset between the
 * inputs (cables, the NCOs' phase, the antennas' own delays) needs no
 * calibration — it is part of what the estimates hold.
 *
 * Estimates (2×2 covariances, R = E[x xᴴ], x = [a, b]ᵀ):
 *   R_n  the noise and interference around a channel: per group of
 *        `group` channels (32 = 4 kHz at 125 Hz), the quietest
 *        `quiet_frac` of them averaged each update, exponentially
 *        averaged over tc_s, interpolated between the groups' centres;
 *   R_s  the channel's own covariance while it carries a signal (the
 *        samples whose |a|²+|b|² stand gate_db over R_n's trace), held
 *        through key-up, dropped after hold_s without one.
 * Weights, w ∝ R_n⁻¹·v (max SINR; diagonal loading 0.1 % of the trace):
 *   v = the principal eigenvector of R_s − R_n where the channel carries a
 *   station (layer 1 of PHASING.md, "steered"), else e_A: the group's
 *   interference canceller, which keeps what antenna A hears and takes out
 *   what dominates the quiet channels. MRC uses diag(R_n) instead of R_n.
 * Continuity: w is turned so that its A component is real and positive
 * (the output's phase follows antenna A's), scaled so that the output's
 * noise is A's noise (s = √(R_n,aa / wᴴR_n w): the decoders' thresholds and
 * the band floor stay where they were with one antenna), and moved
 * linearly over one update interval, never in a step.
 *
 * Channel order is the channelizer's (index 0 = the stream centre, 1..M/2
 * above, M/2+1..M-1 below), so cyclic index neighbours are frequency
 * neighbours; groups are taken in frequency order.
 *
 * Threading: single-threaded (the pipeline's engine thread).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_COMBINER_H
#define SKIMMER_COMBINER_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKIM_PHASE_MVDR = 0,     /* max SINR: nulls + enhancement (the default)    */
  SKIM_PHASE_MRC,          /* maximal-ratio combining, noise taken as
                            * independent between the antennas              */
  SKIM_PHASE_FIXED,        /* one manual weight for the whole band (Linrad)  */
  SKIM_PHASE_A,            /* antenna A alone: y = a, bit for bit            */
  SKIM_PHASE_B,            /* antenna B alone, scaled to A's noise floor     */
} SkimPhaseMode;

typedef struct {
  SkimPhaseMode mode;
  double tc_s;             /* averaging time constant (1 s)                   */
  double gate_db;          /* a signal sample: this much over R_n (6 dB)      */
  double hold_s;           /* R_s forgotten after this long without one (30)  */
  guint  group;            /* channels a noise group (32)                     */
  double quiet_frac;       /* the quietest share of a group = its noise (0.3) */
  double fixed_gain_db;    /* FIXED: B's gain against A, dB                   */
  double fixed_phase_deg;  /* FIXED: B's phase against A, degrees             */
  gboolean layer2;         /* the decoders' marks steer their own channels
                            * (skim_combiner_mark; default TRUE)             */
  double tc2_s;            /* layer 2's time constant (5 s)                  */
} SkimCombinerConfig;

/* The defaults above, mode MVDR. */
void skim_combiner_config_default(SkimCombinerConfig *cfg);

/* Parse "mvdr" | "mrc" | "fixed" | "a" | "b"; FALSE when unknown. */
gboolean skim_phase_mode_parse(const char *s, SkimPhaseMode *out);
const char *skim_phase_mode_name(SkimPhaseMode m);

typedef struct _SkimCombiner SkimCombiner;

/* nchan channels (the channelizer's count), each sampled at rate Hz. NULL
 * on a bad geometry. */
SkimCombiner *skim_combiner_new(guint nchan, double rate,
                                const SkimCombinerConfig *cfg);
void          skim_combiner_free(SkimCombiner *c);

/* Live changes (the phasing panel): the new weights take effect at the next
 * update, ramped as always. */
void skim_combiner_set_mode(SkimCombiner *c, SkimPhaseMode mode);
void skim_combiner_set_fixed(SkimCombiner *c, double gain_db, double phase_deg);
SkimPhaseMode skim_combiner_mode(const SkimCombiner *c);

/* Hold the weights where they are ("adapt, then fix"): updates keep
 * measuring (the meters' numbers stay live) but no longer move a weight. */
void skim_combiner_hold(SkimCombiner *c, gboolean hold);

/* One block of channel chan: a and b (interleaved I/Q, n frames each, the
 * same instants) in, y out. y may be a or b (each sample is read before it
 * is written). */
void skim_combiner_process(SkimCombiner *c, guint chan, const float *a,
                           const float *b, float *y, guint n);

/* Layer 2 (docs/PHASING.md): after the channel's decoder ran on the
 * block, the same n samples of A and B and the decoder's mark probability
 * of each (decode.h mark_prob: 0..1, -1 = no keyed station held; they
 * describe the samples delay frames before, ≤ 64 — the combiner keeps that
 * much of A and B to pair them with). Marks
 * (p ≥ 0.75) teach the station's covariance, spaces (p ≤ 0.25) what
 * disturbs it in its own channel; integrated over seconds, R_mark − R_space
 * gives its signature even where no power gate could, and R_space⁻¹·v
 * its max-SINR weight. It replaces the layer-1 weight only while it
 * promises a better SINR on the same estimates (by 0.5 dB), and lets go
 * hold_s after the decoder lost the station. */
void skim_combiner_mark(SkimCombiner *c, guint chan, const float *a, const float *b,
                        const float *p, guint n, guint delay);

/* Re-estimate every channel's weight from what process() saw since the
 * last update. The owner calls it about once a second (stream time). */
void skim_combiner_update(SkimCombiner *c);

/* Forget every estimate (a retune, a new epoch of the paired stream): the
 * output is antenna A until the next updates have learnt again. */
void skim_combiner_reset(SkimCombiner *c);

/* What the meters and the page read about one channel. */
typedef struct {
  float    ca[2], cb[2];   /* the coefficients the ramp is heading for:
                            * y = ca·a + cb·b ({re, im})                     */
  gboolean steered;        /* the channel carries a station (R_s valid): the
                            * adaptive weight follows it                     */
  double   drop_db;        /* the floor this weight takes off antenna A's,
                            * with A's own signals kept at their gain: on the
                            * last update's noise covariance (NAN: the weight
                            * barely uses A, or nothing learnt yet)          */
  double   share_b;        /* B's part of the output noise, 0..1             */
  /* What the adaptive weight (MVDR; MRC in mode MRC) gives, whatever the
   * mode applies — in modes A, B and Fixed "what phasing would give": */
  double   drop_est_db;    /* its floor drop, as drop_db                     */
  double   gain_est_db;    /* its SINR over the better antenna's, from the
                            * estimates (λ·v·vᴴ the station, R_n the rest);
                            * NAN where no station steers                    */
  gboolean layer2;         /* the weight follows the decoder's marks         */
  double   align;          /* |vᴴ·u_n|²: the station's signature against the
                            * noise's principal direction, 0..1; near 1 = it
                            * comes as the interference does (B a copy of A,
                            * or the interferer's own direction): nothing can
                            * separate them. NAN: no station                 */
} SkimCombinerChanInfo;

gboolean skim_combiner_chan_info(const SkimCombiner *c, guint chan,
                                 SkimCombinerChanInfo *out);

/* The band-level coefficients of a channel (y = ca·a + cb·b, as chan_info):
 * the group's interference canceller with no station steering (MVDR towards
 * e_A), so they vary smoothly across the band — what band_combine.h applies
 * to the band's own IQ (the waterfall, FT8/JS8/WSPR). ca = 1: A's own
 * signals keep their gain and the interference leaves the floor. Modes A
 * and MRC: A; B: B scaled to A's floor; Fixed: the manual weight. */
gboolean skim_combiner_band_coef(const SkimCombiner *c, guint chan, float ca[2], float cb[2]);

/* Noise groups: their number, a channel's group, and a group's coherence
 * |γ|² = |R_ab|² / (R_aa·R_bb) of its quiet channels (0..1; NAN before the
 * first update). Near 1 = one source both antennas hear alike: it can be
 * nulled, as deep as D_max = −10·log10(1 − |γ|²) dB. */
guint  skim_combiner_groups(const SkimCombiner *c);
guint  skim_combiner_group_of(const SkimCombiner *c, guint chan);
double skim_combiner_coherence(const SkimCombiner *c, guint group);

/* Updates done since creation / the last reset. */
guint64 skim_combiner_updates(const SkimCombiner *c);

G_END_DECLS

#endif /* SKIMMER_COMBINER_H */
