/* web.h — skimmer-headless's web page (libsoup 3, on the main loop).
 *
 * The page itself is static (web/: HTML, JS, CSS built in as a GResource)
 * and draws in the browser from JSON:
 *
 *   GET  /, /app.js, /app.css   the page
 *   GET  /status.json           the status snapshot (skimmer-compare reads it)
 *   GET  /stats.json            per-minute history (?hours=N, default 24)
 *   GET  /api/log               the last log lines
 *   GET  /api/config            the config: the ini, what may be edited, backups
 *   POST /api/config            a new config (settings_from only)
 *   POST /api/restart           restart with the config on disk (settings_from)
 *   POST /api/rollback          the previous config back, restart (settings_from)
 *   POST /api/deepcw            fetch the DeepCW model (settings_from)
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

G_BEGIN_DECLS

typedef struct _SkimHdWeb SkimHdWeb;

/* A hook answers with a body and an HTTP status; *type is the content type
 * (static string; default application/json). may_change: the client may
 * POST (its address is in settings_from). A POST hook gets the form fields
 * of an application/x-www-form-urlencoded body (empty table for none). */
typedef char *(*SkimHdWebGet)(GHashTable *query, gboolean may_change, guint *status,
                              const char **type, gpointer user);
typedef char *(*SkimHdWebPost)(GHashTable *form, guint *status, gpointer user);

typedef struct {
  SkimHdWebGet  status, stats, log, config;
  SkimHdWebPost config_post, restart, rollback, deepcw;
  gpointer      user;
} SkimHdWebHooks;

/* Listen on port (all interfaces). settings_from: the networks that may
 * POST (NULL-terminated "a.b.c.d" / "a.b.c.d/n" / "::1" …). */
SkimHdWeb *skim_hd_web_new(guint16 port, const char *const *settings_from,
                           const SkimHdWebHooks *hooks, GError **error);
void       skim_hd_web_free(SkimHdWeb *w);

/* TRUE when addr (a client address) is inside one of nets. */
gboolean skim_hd_web_addr_allowed(const char *const *nets, GInetAddress *addr);

G_END_DECLS
