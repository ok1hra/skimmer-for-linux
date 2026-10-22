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
  return "text/html; charset=utf-8";
}

static gboolean serve_static(SoupServerMessage *msg, const char *path) {
  if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) { path = "/index.html"; }
  if (strstr(path, "..")) { return FALSE; }
  char *res = g_strconcat(WEB_RES, path, NULL);
  GBytes *b = g_resources_lookup_data(res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  g_free(res);
  if (!b) { return FALSE; }
  soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(msg, type_of(path), SOUP_MEMORY_COPY,
                                    g_bytes_get_data(b, NULL), g_bytes_get_size(b));
  g_bytes_unref(b);
  return TRUE;
}

/* May this POST change the skimmer? The client's address must be in
 * settings_from, and a browser's Origin — when it sends one — this page. */
static const char *post_refused(SkimHdWeb *w, SoupServerMessage *msg) {
  GSocketAddress *sa = soup_server_message_get_remote_address(msg);
  GInetAddress *a = G_IS_INET_SOCKET_ADDRESS(sa)
                        ? g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(sa))
                        : NULL;
  if (!allowed(w->nets, a)) {
    return "settings are read-only from this address ([web] settings_from)";
  }
  SoupMessageHeaders *h = soup_server_message_get_request_headers(msg);
  const char *origin = soup_message_headers_get_one(h, "Origin");
  const char *host = soup_message_headers_get_one(h, "Host");
  if (origin) {
    char *want = host ? g_strconcat("http://", host, NULL) : NULL;
    const gboolean same = want && g_ascii_strcasecmp(origin, want) == 0;
    g_free(want);
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

  if (get) {
    SkimHdWebGet hook = strcmp(path, "/status.json") == 0 ? k->status
                      : strcmp(path, "/stats.json") == 0  ? k->stats
                      : strcmp(path, "/api/log") == 0     ? k->log
                      : strcmp(path, "/api/config") == 0  ? k->config
                                                          : NULL;
    if (hook) {
      guint status = SOUP_STATUS_OK;
      const char *type = "application/json";
      char *body = hook(query, &status, &type, k->user);
      reply(msg, status, type, body);
      return;
    }
    if (!serve_static(msg, path)) { reply_error(msg, SOUP_STATUS_NOT_FOUND, "not found"); }
    return;
  }
  if (post) {
    SkimHdWebPost hook = strcmp(path, "/api/config") == 0   ? k->config_post
                       : strcmp(path, "/api/restart") == 0  ? k->restart
                       : strcmp(path, "/api/rollback") == 0 ? k->rollback
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
    gsize len = 0;
    const char *data = g_bytes_get_data(bytes, &len);
    guint status = SOUP_STATUS_OK;
    char *body = hook(data ? data : "", len, &status, k->user);
    g_bytes_unref(bytes);
    reply(msg, status, "application/json", body);
    return;
  }
  reply_error(msg, SOUP_STATUS_METHOD_NOT_ALLOWED, "method not allowed");
}

SkimHdWeb *skim_hd_web_new(guint16 port, const char *const *settings_from,
                           const SkimHdWebHooks *hooks, GError **error) {
  SkimHdWeb *w = g_new0(SkimHdWeb, 1);
  w->hooks = *hooks;
  w->nets = nets_parse(settings_from);
  w->srv = soup_server_new("server-header", "skimmer-headless", NULL);
  soup_server_add_handler(w->srv, NULL, handler, w, NULL);
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
