/* feed_gate.h — the learned RBN feed gate, shadow mode.
 *
 * skimmer-compare/gate/learn.py fits a logistic regression on what the
 * pipeline knew when it sent a spot: the station record, the newest
 * candidate row of the call, how the call was read in the last win_s, and
 * the other calls heard within near_khz (a similar one = a misread pair).
 * The weights, the feature means/sds and the threshold come in an ini:
 *
 *   [gate]
 *   bias = 1.95          threshold = 0.15
 *   win_s = 120          near_khz = 0.150
 *   [weights]
 *   <feature> = <weight> <mean> <sd>     (names: skim_feed_gate_names())
 *
 *   p = 1 / (1 + exp(-(bias + sum(weight * (x - mean) / sd))))
 *
 * In SHADOW mode the pipeline computes p for every line the RBN feed sends
 * and only reports it (gatelog "spot" rows, the headless status): the hand
 * gate still decides. The features mirror learn.py's band_features() row
 * for row — the same rounding as the gatelog prints — so an offline replay
 * with the ini reproduces the p the model was judged by.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

#include "callsign.h"
#include "station.h"

G_BEGIN_DECLS

#define SKIM_FEED_GATE_NFEAT 35

typedef struct _SkimFeedGate SkimFeedGate;

/* The feature names in vector order (SKIM_FEED_GATE_NFEAT of them). */
const char *const *skim_feed_gate_names(void);

/* NULL + error on a missing file, a missing [gate] bias/threshold, an
 * unknown feature, a weight line that is not three numbers, or sd <= 0.
 * Features the ini leaves out weigh nothing. */
SkimFeedGate *skim_feed_gate_load(const char *path, GError **error);
void          skim_feed_gate_free(SkimFeedGate *g);

double      skim_feed_gate_threshold(const SkimFeedGate *g);
double      skim_feed_gate_win_s(const SkimFeedGate *g);
double      skim_feed_gate_near_khz(const SkimFeedGate *g);
const char *skim_feed_gate_id(const SkimFeedGate *g);   /* the ini's basename */

/* p for a feature vector of SKIM_FEED_GATE_NFEAT values. */
double skim_feed_gate_p(const SkimFeedGate *g, const double *x);

/* ---- the features: one tracker per pipeline (one band) ------------------- */

typedef struct _SkimFeedGateTrack SkimFeedGateTrack;

SkimFeedGateTrack *skim_feed_gate_track_new(double win_s, double near_khz);
void               skim_feed_gate_track_free(SkimFeedGateTrack *t);

/* One gatelog "c" row: the extractor's top candidate after a decode at
 * hz, and the station record that decode updated (NULL: none). t_us on
 * the pipeline clock. */
void skim_feed_gate_track_row(SkimFeedGateTrack *t, gint64 t_us, double hz,
                              double wpm, double snr_db, double confidence,
                              const SkimCallsignCand *cand,
                              const SkimStation *st);

/* The feature vector for a spot of call at spot_hz going out now (t_us
 * pipeline clock, wall_us for the hour of day); band as headless names it
 * ("40m"; NULL/unknown = no band feature). */
void skim_feed_gate_track_features(SkimFeedGateTrack *t, gint64 t_us,
                                   gint64 wall_us, const char *call,
                                   double spot_hz, const char *band,
                                   double *x);

/* Edit-distance likeness of two calls, as skimcmp.match.similar(): -1 when
 * they are equal, shorter than 3, or further apart than k. */
gint skim_feed_gate_similar(const char *a, const char *b, gint k);

G_END_DECLS
