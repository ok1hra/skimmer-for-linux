/* peers.h — other skimmers' telnet feeds, aggregated ([peers] hosts=).
 *
 * The hub logs in to each peer's spot feed (the cluster dialect our own
 * feed and CW Skimmer Server speak: "DX de X-#: 14025.0 DL1ABC CW 25 dB
 * 22 WPM CQ 1234Z") with its own call, reads the CQ spots and hands each one
 * to the owner — which sends it on through its own feed
 * (skim_rbn_feed_spot_external: deduplicated against everything the feed
 * already sent). A peer that drops or refuses is tried again, 5 s after
 * the first failure, backing off to a minute.
 *
 * A peer's call must pass skim_callsign_is_valid(); lines that are no CQ
 * spot (talk, announcements, DX/BEACON types) are skipped.
 *
 * Threading: the sockets live on the module's own thread; the spot callback
 * fires there. new/free/json from any one thread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HD_PEER_PORT 7300          /* a feed's port when none is given */

typedef struct _SkimHdPeers SkimHdPeers;

typedef void (*SkimHdPeerSpot)(const char *peer, const char *call, const char *mode,
                               double freq_hz, double snr_db, double speed, gpointer user);

/* Connect to hosts ("host" or "host:port", NULL-terminated), logging in as
 * login. cb gets every CQ spot (peer = the host as given). */
SkimHdPeers *skim_hd_peers_new(const char *const *hosts, const char *login,
                               SkimHdPeerSpot cb, gpointer user);
void         skim_hd_peers_free(SkimHdPeers *p);

/* [{"host":…, "state":"connecting|logged in|retrying …", "connected":bool,
 *   "spots":N, "lines":N}, …] */
char *skim_hd_peers_json(SkimHdPeers *p);

/* One cluster line → a CQ spot (TRUE) — exposed for the gate. freq in Hz;
 * snr / speed 0 when the line has none. */
gboolean skim_hd_peer_parse(const char *line, char *call, gsize call_size, char *mode,
                            gsize mode_size, double *freq_hz, double *snr_db,
                            double *speed);

G_END_DECLS
