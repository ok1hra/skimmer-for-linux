/* rtty_pairs.h — the RTTY stations a band's RTTY pipeline is copying now,
 * for its CW pipeline to stay off.
 *
 * An FSK pair is two carriers 170 Hz apart keyed alternately: to a CW
 * decoder each tone is a carrier keyed at ~54 wpm, so CW channels on an
 * RTTY station read junk text and breed junk candidates. The band's RTTY
 * pipeline publishes the pairs it HOLDS — acquired and with its squelch
 * open (anti-correlated tones, valid framing): a copied RTTY signal, never
 * two unrelated carriers — and the CW pipeline, when told to, drops the
 * decodes whose carrier lies on one (centre ± 85 Hz ± SKIM_RTTY_PAIR_EDGE_HZ),
 * not the whole 125 Hz channel.
 *
 * A pair stays published SKIM_RTTY_PAIR_LINGER_US after its last sighting:
 * a CW decoder's text leaves seconds after the audio. Both sides use their
 * own pipeline clock — live both monotonic, offline both the stream time of
 * the one IQ they are fed in lockstep. Thread-safe.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_RTTY_PAIR_HALF_HZ   85.0
#define SKIM_RTTY_PAIR_EDGE_HZ   30.0
#define SKIM_RTTY_PAIR_LINGER_US (5 * G_USEC_PER_SEC)

typedef struct _SkimRttyPairs SkimRttyPairs;

SkimRttyPairs *skim_rtty_pairs_new(void);
SkimRttyPairs *skim_rtty_pairs_ref(SkimRttyPairs *b);
void           skim_rtty_pairs_unref(SkimRttyPairs *b);

/* The pairs held now (absolute centres, Hz): each is (re)published until
 * now + linger; one within 30 Hz of a published pair is the same pair. */
void skim_rtty_pairs_publish(SkimRttyPairs *b, gint64 now_us, const double *centre_hz,
                             guint n);

/* Does a carrier at hz lie on a pair published at now_us? */
gboolean skim_rtty_pairs_covers(SkimRttyPairs *b, gint64 now_us, double hz);

/* Pairs published at now_us (status). */
guint skim_rtty_pairs_count(SkimRttyPairs *b, gint64 now_us);

G_END_DECLS
