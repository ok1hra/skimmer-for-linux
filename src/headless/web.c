/* web.c — see web.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "web.h"

#include <libsoup/soup.h>
#include <string.h>

#define WEB_RES "/skimmer-for-linux/web"

struct _SkimHdWeb {
  SoupServer    *srv;
  GPtrArray     *nets;                 /* GInetAddressMask                   */
  SkimHdWebHooks hooks;
};

/* An IPv4 client on a dual-stack socket shows as ::ffff:a.b.c.d. */
static GInetAddress *unmapped(GInetAddress *a) {
  if (g_inet_address_get_family(a) != G_SOCKET_FAMILY_IPV6) { return g_object_ref(a); }
  const guint8 *b = g_inet_address_to_bytes(a);
  static const guint8 PFX[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
  if (memcmp(b, PFX, 12) != 0) { return g_object_ref(a); }
  return g_inet_address_new_from_bytes(b + 12, G_SOCKET_FAMILY_IPV4);
}

static GPtrArray *nets_parse(const char *const *list) {
  GPtrArray *nets = g_ptr_array_new_with_free_func(g_object_unref);
  for (const char *const *n = list; n && *n; n++) {
    GError *err = NULL;
    GInetAddressMask *m = g_inet_address_mask_new_from_string(*n, &err);
    if (m) {
      g_ptr_array_add(nets, m);
    } else {
      g_warning("web: settings_from \"%s\": %s — ignored", *n, err->message);
      g_clear_error(&err);
    }
  }
  return nets;
}

static gboolean allowed(GPtrArray *nets, GInetAddress *addr) {
  if (!addr) { return FALSE; }
  GInetAddress *a = unmapped(addr);
  gboolean ok = FALSE;
  for (guint i = 0; i < nets->len && !ok; i++) {
    ok = g_inet_address_mask_matches(nets->pdata[i], a);
  }
  g_object_unref(a);
  return ok;
}

gboolean skim_hd_web_addr_allowed(const char *const *nets, GInetAddress *addr) {
  GPtrArray *n = nets_parse(nets);
  const gboolean ok = allowed(n, addr);
  g_ptr_array_unref(n);
  return ok;
}

static void reply(SoupServerMessage *msg, guint status, const char *type, char *body) {
  SoupMessageHeaders *h = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(h, "Cache-Control", "no-store");
  soup_server_message_set_status(msg, status, NULL);
  if (!body) { body = g_strdup(""); }
  soup_server_message_set_response(msg, type, SOUP_MEMORY_TAKE, body, strlen(body));
}

static void reply_error(SoupServerMessage *msg, guint status, const char *what) {
  char *esc = g_strescape(what, NULL);
  reply(msg, status, "application/json", g_strdup_printf("{\"error\":\"%s\"}", esc));
  g_free(esc);
}

static const char *type_of(const char *path) {
  if (g_str_has_suffix(path, ".js")) { return "text/javascript; charset=utf-8"; }
  if (g_str_has_suffix(path, ".css")) { return "text/css; charset=utf-8"; }
  if (g_str_has_suffix(path, ".svg")) { return "image/svg+xml"; }
  if (g_str_has_suffix(path, ".ttf")) { return "font/ttf"; }
  return "text/html; charset=utf-8";
}

/* The page: index.html's <!--admin-->…<!--/admin--> parts (Settings, Log)
 * go only to a client that may change settings — the others never get them,
 * not even hidden. */
static gboolean serve_static(SoupServerMessage *msg, const char *path, gboolean admin) {
  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) { path = "/index.html"; }
  if (strstr(path, "..")) { return FALSE; }
  char *res = g_strconcat(WEB_RES, path, NULL);
  GBytes *b = g_resources_lookup_data(res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  g_free(res);
  if (!b) { return FALSE; }
  soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
  if (strcmp(path, "/index.html") == 0 && !admin) {
    GString *o = g_string_new_len(g_bytes_get_data(b, NULL), (gssize)g_bytes_get_size(b));
    for (char *a; (a = strstr(o->str, "<!--admin-->"));) {
      char *e = strstr(a, "<!--/admin-->");
      const gsize from = (gsize)(a - o->str);
      g_string_erase(o, (gssize)from, e ? (gssize)(e - a) + 13 : -1);
    }
    const gsize len = o->len;
    soup_server_message_set_response(msg, type_of(path), SOUP_MEMORY_TAKE, g_string_free(o, FALSE), len);
  } else {
    soup_server_message_set_response(msg, type_of(path), SOUP_MEMORY_COPY,
                                      g_bytes_get_data(b, NULL), g_bytes_get_size(b));
  }
  g_bytes_unref(b);
  return TRUE;
}

/* A request a proxy forwarded: its address is the proxy's (127.0.0.1 for one
 * on this machine), not the client's — never trusted as a local one. */
static gboolean via_proxy(SoupServerMessage *msg) {
  SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
  return soup_message_headers_get_one(h, "X-Forwarded-For") ||
         soup_message_headers_get_one(h, "Forwarded") ||
         soup_message_headers_get_one(h, "X-Real-IP") || soup_message_headers_get_one(h, "Via");
}

/* The Host the browser asked for is this machine — an address, localhost or
 * its own name — and not some domain a page made point here (DNS rebinding:
 * a site that resolves its name to 127.0.0.1 would otherwise be "this page"
 * to the Origin check, with the client address local). */
static gboolean host_ok(SoupServerMessage *msg) {
  const char *host = soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Host");
  if (!host || !host[0]) { return TRUE; }                       /* HTTP/1.0, curl -0 */
  char *h = g_strdup(host);
  if (h[0] == '[') {                                            /* [::1]:8073        */
    char *e = strchr(h, ']');
    if (e) { *e = '\0'; memmove(h, h + 1, strlen(h)); }
  } else {
    char *c = strrchr(h, ':');
    if (c && strchr(h, ':') == c) { *c = '\0'; }
  }
  const char *me = g_get_host_name();
  gboolean ok = g_hostname_is_ip_address(h) || g_ascii_strcasecmp(h, "localhost") == 0 ||
                g_ascii_strcasecmp(h, me) == 0;
  static const char *const SFX[] = { ".local", ".lan", ".home", ".localdomain", ".home.arpa", NULL };
  for (const char *const *x = SFX; !ok && *x; x++) {
    char *full = g_strconcat(me, *x, NULL);
    ok = g_ascii_strcasecmp(h, full) == 0;
    g_free(full);
  }
  g_free(h);
  return ok;
}

/* May this client manage the skimmer (Settings, Log, the config, POSTs)?
 * Its address in settings_from, not through a proxy, and asking for this
 * machine by an address or its name. */
static gboolean client_allowed(SkimHdWeb *w, SoupServerMessage *msg) {
  GSocketAddress *sa = soup_server_message_get_remote_address(msg);
  GInetAddress *a = G_IS_INET_SOCKET_ADDRESS(sa)
                        ? g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sa))
                        : NULL;
  return allowed(w->nets, a) && !via_proxy(msg) && host_ok(msg);
}

/* What every answer carries: no sniffing, no framing (the settings page
 * must not be clickjacked), nothing loaded from elsewhere. */
static void harden(SoupServerMessage *msg) {
  SoupMessageHeaders *h = soup_server_message_get_response_headers(msg);
  soup_message_headers_replace(h, "X-Content-Type-Options", "nosniff");
  soup_message_headers_replace(h, "X-Frame-Options", "DENY");
  soup_message_headers_replace(h, "Referrer-Policy", "no-referrer");
  soup_message_headers_replace(h, "Content-Security-Policy",
      "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
      "img-src 'self' data: blob:; connect-src 'self' ws: wss:; font-src 'self'; "
      "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}

/* May this POST change the skimmer? The client's address must be in
 * settings_from, and a browser's Origin — when it sends one — this page. */
static const char *post_refused(SkimHdWeb *w, SoupServerMessage *msg) {
  if (!client_allowed(w, msg)) {
    return "settings are read-only from this address ([web] settings_from; not through a "
           "proxy; the page asked for by this machine's address or name)";
  }
  SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
  const char *origin = soup_message_headers_get_one(h, "Origin");
  const char *host = soup_message_headers_get_one(h, "Host");
  if (origin) {
    char *want = host ? g_strconcat("http://", host, NULL) : NULL;
    char *want_s = host ? g_strconcat("https://", host, NULL) : NULL;
    const gboolean same = want && (g_ascii_strcasecmp(origin, want) == 0 ||
                                   g_ascii_strcasecmp(origin, want_s) == 0);
    g_free(want);
    g_free(want_s);
    if (!same) { return "a page from another origin may not change the settings"; }
  }
  return NULL;
}

static void handler(SoupServer *srv, SoupServerMessage *msg, const char *path,
                    GHashTable *query, gpointer user) {
  (void)srv;
  SkimHdWeb *w = user;
  const char *method = soup_server_message_get_method(msg);
  const gboolean get = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0;
  const gboolean post = strcmp(method, "POST") == 0;
  const SkimHdWebHooks *k = &w->hooks;

  /* libsoup 3.2 (Debian 12, the AppImage's) never closes a kept-alive
   * connection the client has hung up: each one stayed in CLOSE-WAIT until
   * the 1024 fds ran out (~60 h of status polls). Close after every reply. */
  soup_message_headers_replace(soup_server_message_get_response_headers(msg),
                               "Connection", "close");
  harden(msg);
  const gboolean admin = client_allowed(w, msg);

  if (get) {
    /* the config, the log and the LAN search (0.8 s on the main loop) are
     * for those who may manage the skimmer only */
    if (!admin && (strcmp(path, "/api/config") == 0 || strcmp(path, "/api/log") == 0 ||
                   strcmp(path, "/api/skimmers") == 0)) {
      reply_error(msg, SOUP_STATUS_FORBIDDEN, "only from an address that may change the settings");
      return;
    }
    SkimHdWebGet hook = strcmp(path, "/status.json") == 0 ? k->status
                      : strcmp(path, "/stats.json") == 0  ? k->stats
                      : strcmp(path, "/api/log") == 0     ? k->log
                      : strcmp(path, "/api/config") == 0  ? k->config
                      : strcmp(path, "/api/skimmers") == 0 ? k->skimmers
                      : strcmp(path, "/api/spots") == 0    ? k->spots
                                                          : NULL;
    if (hook) {
      guint status = SOUP_STATUS_OK;
      const char *type = "application/json";
      char *body = hook(query, admin, &status, &type, k->user);
      reply(msg, status, type, body);
      return;
    }
    if (!serve_static(msg, path, admin)) { reply_error(msg, SOUP_STATUS_NOT_FOUND, "not found"); }
    return;
  }
  if (post) {
    SkimHdWebPost hook = strcmp(path, "/api/config") == 0   ? k->config_post
                       : strcmp(path, "/api/config/import") == 0 ? k->config_import
                       : strcmp(path, "/api/restart") == 0  ? k->restart
                       : strcmp(path, "/api/rollback") == 0 ? k->rollback
                       : strcmp(path, "/api/deepcw") == 0   ? k->deepcw
                                                            : NULL;
    if (!hook) {
      reply_error(msg, SOUP_STATUS_NOT_FOUND, "not found");
      return;
    }
    const char *why = post_refused(w, msg);
    if (why) {
      reply_error(msg, SOUP_STATUS_FORBIDDEN, why);
      return;
    }
    SoupMessageBody *b = soup_server_message_get_request_body(msg);
    GBytes *bytes = soup_message_body_flatten(b);
    char *text = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
    g_bytes_unref(bytes);
    GHashTable *form = text[0] ? soup_form_decode(text)
                               : g_hash_table_new(g_str_hash, g_str_equal);
    g_free(text);
    guint status = SOUP_STATUS_OK;
    char *body = hook(form, &status, k->user);
    g_hash_table_unref(form);
    reply(msg, status, "application/json", body);
    return;
  }
  reply_error(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, "method not allowed");
}

static void ws_waterfall(SoupServer *srv, SoupServerMessage *msg, const char *path,
                         SoupWebsocketConnection *conn, gpointer user) {
  (void)srv; (void)path;
  SkimHdWeb *w = user;
  /* a page from elsewhere may not open it (read-only, but it costs CPU) */
  SoupMessageHeaders *rh = soup_server_message_get_request_headers(msg);
  const char *origin = soup_message_headers_get_one(rh, "Origin");
  const char *host = soup_message_headers_get_one(rh, "Host");
  if (origin && host) {
    const char *o = strstr(origin, "://");
    if (!o || g_ascii_strcasecmp(o + 3, host) != 0) {
      soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "another origin");
      return;
    }
  }
  GSocketAddress *sa = soup_server_message_get_remote_address(msg);
  char *peer = G_IS_INET_SOCKET_ADDRESS(sa)
      ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sa)))
      : g_strdup("a viewer");
  if (w->hooks.waterfall) {
    w->hooks.waterfall(conn, peer, w->hooks.user);
  } else {
    soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, "no waterfall");
  }
  g_free(peer);
}

SkimHdWeb *skim_hd_web_new(guint16 port, const char *const *settings_from,
                           const SkimHdWebHooks *hooks, GError **error) {
  SkimHdWeb *w = g_new0(SkimHdWeb, 1);
  w->hooks = *hooks;
  w->nets = nets_parse(settings_from);
  w->srv = soup_server_new("server-header", "skimmer-headless", NULL);
  soup_server_add_handler(w->srv, NULL, handler, w, NULL);
  /* the waterfall's rows; permessage-deflate is on by default */
  soup_server_add_websocket_handler(w->srv, "/ws/waterfall", NULL, NULL, ws_waterfall, w, NULL);
  if (!soup_server_listen_all(w->srv, port, 0, error)) {
    skim_hd_web_free(w);
    return NULL;
  }
  return w;
}

void skim_hd_web_free(SkimHdWeb *w) {
  if (!w) { return; }
  if (w->srv) {
    soup_server_disconnect(w->srv);
    g_object_unref(w->srv);
  }
  g_ptr_array_unref(w->nets);
  g_free(w);
}
