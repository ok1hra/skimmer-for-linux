/* gatelog.h — the feed-gate learning logs.
 *
 * The RBN feed gate is to be LEARNED from what skimmer-compare measures
 * (Dan, 2026-10-03: L caught 24 % more than VE3NEA's CW Skimmer Server but
 * busted ten times as often). The compare logs hold only the spots that
 * went out, so they can teach what to suppress, never what the hand gate
 * threw away. Two logs close that gap; either may be off:
 *
 *  tap   what the post-decode path EATS: every decode that survived ghost
 *        arbitration (its extractor slot, the locked frequency, speed, SNR,
 *        confidence, the contested flag and the text) and every extractor
 *        reset. skimmer-tap-replay feeds it back through the same
 *        extractor + station table + feed gate offline, so features can be
 *        redefined and recomputed over the whole history.
 *
 *          # skimmer tap v1 engine=cw-v2 nslot=3
 *          T <t_us> <wall_us> <ix> <hz> <wpm> <snr> <conf> <contested> |text|
 *          R <t_us> <wall_us> <ix>          one extractor reset (ix < 0: all)
 *          C <t_us> <wall_us>               the clock, once a second: time
 *                                           passes without decodes too, and
 *                                           the settle hold and the station
 *                                           prune run on it
 *
 *  rows  JSON lines as the gate sees things: one "c" row per decode while
 *        the extractor holds a candidate — the candidate threshold aside
 *        (callsign.h SkimCallsignCand), the station record it updated and
 *        the hand gate's verdict on it — and the feed's events (hold, send,
 *        drop, spot = a line on the wire, gone). "read" is how long ago the
 *        call was last actually decoded: a candidate outlives its station,
 *        and noise on a quiet channel reports it again (the ghosts).
 *
 * t_us is the engine clock (monotonic live, stream time offline); wall_us
 * is UTC, what compare's logs are stamped with. Engine thread only (or the
 * offline caller's thread). GLib-only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_GATELOG_H
#define SKIMMER_GATELOG_H

#include <glib.h>

#include "callsign.h"
#include "station.h"

G_BEGIN_DECLS

typedef struct _SkimGateLog SkimGateLog;

/* Opens (appends to) whichever paths are not NULL. NULL when both are NULL
 * or neither could be opened (a warning says which). */
SkimGateLog *skim_gatelog_open(const char *tap_path, const char *rows_path,
                               const char *engine, guint nslot);
void         skim_gatelog_close(SkimGateLog *g);

void skim_gatelog_tap_text(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                           guint ix, double hz, double wpm, double snr_db,
                           double confidence, gboolean contested,
                           const char *text);
/* The engine clock, for the replay to tick the hold and the prune on. */
void skim_gatelog_tap_clock(SkimGateLog *g, gint64 t_us, gint64 wall_us);
/* ix < 0: every extractor of the pipeline. */
void skim_gatelog_tap_reset(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                            gint ix);

/* One "c" row. cand NULL = the extractor holds nothing (no row). st: the
 * station record this decode updated, NULL when it updated none. gate: the
 * hand gate's verdict on st (rbn_gate). */
void skim_gatelog_cand(SkimGateLog *g, gint64 t_us, gint64 wall_us, guint ix,
                       double hz, double wpm, double snr_db, double confidence,
                       gboolean contested, const SkimCallsignCand *cand,
                       const SkimStation *st, gboolean gate);

/* A feed event: "hold", "send", "drop", "spot", "gone". */
void skim_gatelog_event(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                        const char *ev, const char *call, double hz,
                        double snr_db, double wpm);

/* A "spot" event scored by the learned gate in shadow (feed_gate.h): the
 * same line as skim_gatelog_event(…, "spot", …) plus "lr" (its p) and "x"
 * (the feature vector, feed_gate names order) — the offline parity check
 * against learn.py reads both. */
void skim_gatelog_spot(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                       const char *call, double hz, double snr_db, double wpm,
                       double lr, const double *x, guint nx);

G_END_DECLS

#endif /* SKIMMER_GATELOG_H */
