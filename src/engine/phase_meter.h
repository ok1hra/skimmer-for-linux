/* phase_meter.h — how much the two-antenna combiner gives (docs/PHASING.md).
 *
 * The combiner has both antennas' channels and its own output at the same
 * instants, so A alone, B alone and the combination are compared on the
 * same stations at the same time — no second decoder. Three shadow SNR
 * meters (snr_meter.h: SNR_2500, each against its own band floor) read
 * A, B and the output Y; the combiner's estimates give the floor drop.
 *
 * Per channel (one pixel of the waterfall line and of the history):
 *   gain      G = SNR_Y − max(SNR_A, SNR_B), where the channel carries a
 *             station (the combiner steers it and the meters read it):
 *             the gain over the better antenna. NAN where there is none.
 *   gain_a    SNR_Y − SNR_A: the gain over antenna A alone (today's skimmer).
 *   drop      the floor the weight takes off antenna A's, A's own signals
 *             kept at their gain (combiner.h drop_db): the interference
 *             suppression. NAN where it is not defined.
 *   gain_est, drop_est  what the adaptive weight gives by the combiner's
 *             own estimates (combiner.h gain_est_db, drop_est_db) — in modes
 *             A, B and Fixed what phasing WOULD give: the waterfall line and
 *             the history show it while the operator compares by ear.
 * Per noise group: the coherence |γ|² and the deepest null it allows,
 * D_max = −10·log10(1 − |γ|²).
 * A band summary (the minute's statistics): medians of the above.
 *
 * Threading: single-threaded (the pipeline's engine thread).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_PHASE_METER_H
#define SKIMMER_PHASE_METER_H

#include <glib.h>

#include "combiner.h"
#include "snr_meter.h"

G_BEGIN_DECLS

typedef enum {
  SKIM_PH_A = 0,
  SKIM_PH_B,
  SKIM_PH_Y,
  SKIM_PH_N,
} SkimPhaseStream;

typedef struct {
  double gain_db;          /* median G over the channels with a station     */
  double gain_a_db;        /* median SNR_Y − SNR_A over them                */
  double gain_p10_db;      /* their 10th and 90th percentiles               */
  double gain_p90_db;
  double drop_db;          /* median floor drop over the quiet channels     */
  double drop_p90_db;      /* its 90th percentile                           */
  double drop_share;       /* share of the groups whose median drop ≥ 3 dB  */
  double coherence;        /* median |γ|² over the groups                   */
  double dmax_db;          /* median D_max over the groups                  */
  double share_b;          /* share of the stations B carried (> half of the
                            * output noise from B)                          */
  double gain_est_db;      /* median estimated gain of the adaptive weight   */
  double drop_est_db;      /* median estimated floor drop of the adaptive
                            * weight over the quiet channels                */
  double same_dir;         /* share of the stations whose signature is the
                            * noise's own (align > 0.99): near 1 with |γ|²
                            * near 1 = B hears what A hears (a splitter?)   */
  guint  layer2;           /* channels whose weight follows their decoder   */
  guint  stations;         /* channels with a station — any signal the
                            * combiner steers on, a band-limited interferer's
                            * edge too (same_dir tells them); the statistics
                            * of stations count the spots (their channels'
                            * numbers), not this                            */
  guint  quiet;            /* channels counted for the drop                 */
} SkimPhaseSummary;

typedef struct _SkimPhaseMeter SkimPhaseMeter;

/* nchan channels at rate Hz, noise bandwidth enbw_hz (as snr_meter.h). */
SkimPhaseMeter *skim_phase_meter_new(guint nchan, double rate, double enbw_hz);
void            skim_phase_meter_free(SkimPhaseMeter *m);

/* One block of channel chan: both antennas and the combiner's output. */
void skim_phase_meter_push(SkimPhaseMeter *m, guint chan, const float *a,
                           const float *b, const float *y, guint n);

/* About once a second, after skim_combiner_update(): the three meters'
 * floors and every channel's numbers. */
void skim_phase_meter_refresh(SkimPhaseMeter *m, const SkimCombiner *c);

/* The last refresh's numbers (NAN = none). */
double skim_phase_meter_gain(const SkimPhaseMeter *m, guint chan);
double skim_phase_meter_gain_a(const SkimPhaseMeter *m, guint chan);
double skim_phase_meter_drop(const SkimPhaseMeter *m, guint chan);
double skim_phase_meter_gain_est(const SkimPhaseMeter *m, guint chan);
double skim_phase_meter_drop_est(const SkimPhaseMeter *m, guint chan);
double skim_phase_meter_snr(SkimPhaseMeter *m, SkimPhaseStream s, guint chan);
void   skim_phase_meter_summary(const SkimPhaseMeter *m, SkimPhaseSummary *out);

/* The meter of one stream: Y's is the band's meter when it is phased (the
 * decoders' set_floor and the spots' snr2 come from it). */
SkimSnrMeter *skim_phase_meter_snr_meter(SkimPhaseMeter *m, SkimPhaseStream s);

/* A channel's number as one byte, 0.25 dB a step (−31.75..+31.75 dB),
 * SKIM_PHASE_NONE where there is none: the waterfall line and the history. */
#define SKIM_PHASE_NONE (-128)
gint8 skim_phase_q8(double db);

G_END_DECLS

#endif /* SKIMMER_PHASE_METER_H */
