/* feed_fold.h — the RBN feed's memory of what it sent: a busted twin of a
 * call it spotted lately on the same frequency stays off the wire.
 *
 * skimmer-compare, 2026-10-10 (reserve 10-06 … 10-10, 4 385 episodes): half
 * of the feed's misread busts are a call's BOUNDARY gone wrong — the next
 * letter glued on (F6FXX → F6FXXK) or the call cut (RK3DJW → RK3D) — and in
 * 225 of 477 the feed had already spotted the right call there. The station
 * table folds such twins (station.c clip/glue) only while the record is
 * fresh; once it was silent past the TTL, the twin is a new station.
 *
 * The memo keeps every call sent within window_us: its frequency, the most
 * hearings it was sent with, whether the dictionary knows it. A call about
 * to start a new run of lines (not sent itself within self_us) is FOLDED —
 * held off the wire — when a remembered call within df_hz is alike
 * (skim_feed_gate_similar() ≤ 2: an edit distance of two, or one call the
 * head or tail of the other) and better attested: the dictionary knows it
 * and not this one and it was heard at least as often, or both alike in the
 * dictionary and it was heard more. A twin that is better than the call it
 * resembles goes out — the feed often sends the garble first.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimFeedFold SkimFeedFold;

SkimFeedFold *skim_feed_fold_new(gint64 window_us, gint64 self_us, double df_hz);
void          skim_feed_fold_free(SkimFeedFold *f);

/* A line for call went out at freq_hz. */
void skim_feed_fold_sent(SkimFeedFold *f, const char *call, double freq_hz,
                         gint64 now_us, guint hearings, gboolean dict);

/* May call start a run of lines? NULL = yes; otherwise the remembered call
 * that folds it (borrowed, valid until the next call on f). */
const char *skim_feed_fold_check(SkimFeedFold *f, const char *call,
                                 double freq_hz, gint64 now_us, guint hearings,
                                 gboolean dict);

G_END_DECLS
