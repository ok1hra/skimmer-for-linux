/* web.h — skimmer-headless's web page (libsoup 3, on the main loop).
 *
 * The page itself is static (web/: HTML, JS, CSS built in as a GResource)
 * and draws in the browser from JSON:
 *
 *   GET  /, /app.js, /app.css   the page
 *   GET  /status.json           the status snapshot (skimmer-compare reads it)
 *   GET  /stats.json            history (?hours=N, default 24; &end=<unix s>: a
 *                               window ending then instead of now)
 *   GET  /api/log               the last log lines
 *   GET  /api/config            the config: the ini, what may be edited, backups
 *   GET  /api/spots             the spots the feed sent (?minutes=N, default 30, ≤ 120)
 *   GET  /api/wspr              the WSPR stations heard in the last 24 h (wspr.h)
 *   GET  /api/ft8               the FT8/FT4 decodes of the last 15 min (?dial=Hz: one
 *                               dial; &since=<unix ms>: newer only — ft8.h)
 *   GET  /api/skimmers          the skimmer instances on the LAN (a 0.8 s search)
 *   GET  /api/update            the updates: running, latest, staged, kept (settings_from)
 *   POST /api/config            a new config (settings_from only)
 *   POST /api/config/import    an exported ini (ini=…) as the config (settings_from)
 *   POST /api/restart           restart with the config on disk (settings_from)
 *   POST /api/rollback          the previous config back, restart (settings_from)
 *   POST /api/deepcw            fetch the DeepCW model (settings_from)
 *   POST /api/update            action=check | install | rollback (settings_from; update.h)
 *   GET  /api/visitor           {"login","need","call","free"}: must this client sign in
 *   POST /api/visitor           call=… signs in (a cookie), logout=1 signs out — open
 *                               to all, same origin only (visitors.h)
 *   GET  /api/visitors          the visitors' log (settings_from)
 *   WS   /ws/waterfall          a band's spectrum, live (waterfall.h)
 *
 * With visitors on (skim_hd_web_set_visitors), a client that is not free —
 * not a manager, and not from [visitors] free_from without a proxy — gets
 * the page itself but no data (/status.json, /stats.json, /api/spots, /api/wspr,
 * /api/ft8, the waterfall: 401 {"error":"login"}) until it signs in with a callsign.
 *
 * Every body comes from the owner's hooks — this module only routes, checks
 * who may change things and answers. A POST is refused (403) unless the
 * client's address is in settings_from and its Origin, when sent, is this
 * very page (a page from elsewhere, open in a browser ON this machine, must
 * not be able to post to 127.0.0.1).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>
#include <libsoup/soup.h>

#include "visitors.h"

G_BEGIN_DECLS

typedef struct _SkimHdWeb SkimHdWeb;

/* Who asks: a client from outside (not a manager, not from free_from
 * without a proxy — it sees no machine readings: the CPU load, IQ lost;
 * the temperature it does), one from the LAN, or a manager (may POST: its address is in
 * settings_from). */
typedef enum {
  SKIM_HD_WEB_PUBLIC,
  SKIM_HD_WEB_LOCAL,
  SKIM_HD_WEB_MANAGER,
} SkimHdWebWho;

/* A hook answers with a body and an HTTP status; *type is the content type
 * (static string; default application/json). A POST hook gets the form
 * fields of an application/x-www-form-urlencoded body (empty table for
 * none). */
typedef char *(*SkimHdWebGet)(GHashTable *query, SkimHdWebWho who, guint *status,
                              const char **type, gpointer user);
typedef char *(*SkimHdWebPost)(GHashTable *form, guint *status, gpointer user);

typedef struct {
  SkimHdWebGet  status, stats, log, config, skimmers, spots, skimsrv, wspr, ft8, update;
  SkimHdWebPost config_post, config_import, restart, rollback, deepcw, update_post;
  /* a WebSocket viewer of the waterfall (peer: its address, for the log;
   * visitor: its sign-in token, NULL for a free client); the hook takes its
   * own reference */
  void        (*waterfall)(SoupWebsocketConnection *conn, const char *peer, const char *visitor,
                           gpointer user);
  gpointer      user;
} SkimHdWebHooks;

/* Listen on port (all interfaces). settings_from: the networks that may
 * POST (NULL-terminated "a.b.c.d" / "a.b.c.d/n" / "::1" …). */
SkimHdWeb *skim_hd_web_new(guint16 port, const char *const *settings_from,
                           const SkimHdWebHooks *hooks, GError **error);
void       skim_hd_web_free(SkimHdWeb *w);

/* Visitors from outside sign in (v: the log, kept by the caller; NULL = no
 * sign-in, everyone gets the data). free_from: the networks that need not
 * (NULL-terminated) — and that see the machine readings (SkimHdWebWho);
 * never set, every client that is not a manager counts as from outside. */
void       skim_hd_web_set_visitors(SkimHdWeb *w, SkimHdVisitors *v, const char *const *free_from);

/* TRUE when addr (a client address) is inside one of nets. */
gboolean skim_hd_web_addr_allowed(const char *const *nets, GInetAddress *addr);

G_END_DECLS
