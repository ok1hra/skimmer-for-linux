/* rbn_feed.h — RBN telnet feed: a CW-Skimmer-compatible telnet server.
 *
 * The Reverse Beacon Network does not take spots from skimmers directly:
 * the operator runs the RBN Aggregator, which connects TO the skimmer's
 * telnet server (the CW Skimmer convention, default port 7300), logs in
 * and relays the stream to the network. This module is that server. Any
 * number of clients may connect; each validated spot is broadcast as a
 * classic DX-cluster line:
 *
 *   DX de OK1BR-#:   7032.0  DL1ABC       CW    25 dB  22 WPM  CQ      1234Z
 *
 * Transport only — WHAT may be spotted is the pipeline's RBN policy
 * (always CQ-only, a stricter callsign-score gate than the panadapter,
 * its own dedup and rate budget). The server runs its own GMainContext
 * thread; skim_rbn_feed_spot() may be called from any thread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_RBN_FEED_H
#define SKIMMER_RBN_FEED_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimRbnFeed SkimRbnFeed;

/* Start the telnet server. mycall names the spotter ("MYCALL-#"). port 0
 * binds an ephemeral port (gates) — skim_rbn_feed_port() tells which. */
SkimRbnFeed *skim_rbn_feed_new(const char *mycall, guint16 port, GError **error);
void         skim_rbn_feed_free(SkimRbnFeed *f);

/* A feed that listens nowhere: its spots go only to its tap and its relay
 * (a worker hands them to its hub this way). Port 0 in the answers. */
SkimRbnFeed *skim_rbn_feed_new_sink(const char *mycall);

/* Every spot the feed sends, on the spotting thread, before any delay. */
typedef void (*SkimRbnFeedTap)(const char *call, const char *mode, double freq_hz,
                               double snr_db, double speed, gpointer user);
void skim_rbn_feed_set_tap(SkimRbnFeed *f, SkimRbnFeedTap tap, gpointer user);

guint16 skim_rbn_feed_port(const SkimRbnFeed *f);

/* Hold every spot delay_s seconds (0 = none, the default; at most 99)
 * before it goes out. The line keeps the time it was spotted. Set it
 * before the first spot. */
#define SKIM_RBN_FEED_MAX_DELAY_S 99
void skim_rbn_feed_set_delay(SkimRbnFeed *f, guint delay_s);
guint skim_rbn_feed_delay(const SkimRbnFeed *f);

/* Every spot f sends goes to relay too (with relay's own delay) — the
 * RBN's port beside the local feed. relay is borrowed: it must outlive f's
 * spotting. NULL ends it. Set it before the first spot. */
void skim_rbn_feed_set_relay(SkimRbnFeed *f, SkimRbnFeed *relay);

/* Broadcast one spot to every logged-in client (after the delay). Thread-safe. */
void skim_rbn_feed_spot(SkimRbnFeed *f, const char *call, const char *mode,
                        double freq_hz, double snr_db, double speed);

/* A spot another skimmer made (a peer's feed, a hub's worker): sent like
 * skim_rbn_feed_spot() unless this feed sent the call within 300 Hz in the
 * last 600 s, whoever spotted it — then dropped (FALSE) and counted. Every
 * spot, local or not, is noted for this, and a local spot of a call another
 * skimmer's spot already sent is dropped and counted too. Thread-safe. */
gboolean skim_rbn_feed_spot_external(SkimRbnFeed *f, const char *call, const char *mode,
                                     double freq_hz, double snr_db, double speed);
guint64  skim_rbn_feed_dup_dropped(const SkimRbnFeed *f);

/* Logged-in clients right now / spot lines actually written in total /
 * spots waiting out the delay. */
guint   skim_rbn_feed_clients(const SkimRbnFeed *f);
guint64 skim_rbn_feed_lines(const SkimRbnFeed *f);
guint   skim_rbn_feed_waiting(const SkimRbnFeed *f);

G_END_DECLS

#endif /* SKIMMER_RBN_FEED_H */
