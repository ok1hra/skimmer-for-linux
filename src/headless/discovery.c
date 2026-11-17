/* discovery.c — see discovery.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST  "SKIMMER? 1"
#define ANSWER   "SKIMMER 1 "
#define MAX_JSON 8000

struct _SkimHdDiscovery {
  int                 fd;
  guint               watch;
  SkimHdDiscoveryInfo info;
  gpointer            user;
};

guint16 skim_hd_discovery_port(void) {
  const char *e = g_getenv("SKIM_DISCOVERY_PORT");
  guint64 v = 0;
  if (e && g_ascii_string_to_unsigned(e, 10, 1, 65535, &v, NULL)) { return (guint16)v; }
  return SKIM_HD_DISCOVERY_PORT;
}

static int udp_socket(GError **error) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "discovery socket: %s",
                g_strerror(errno));
    return -1;
  }
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
  return fd;
}

static gboolean on_request(gint fd, GIOCondition cond, gpointer data) {
  (void)cond;
  SkimHdDiscovery *d = data;
  char buf[64];
  struct sockaddr_in from;
  socklen_t flen = sizeof(from);
  const gssize n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
  if (n <= 0) { return G_SOURCE_CONTINUE; }
  buf[n] = '\0';
  if (!g_str_has_prefix(buf, REQUEST)) { return G_SOURCE_CONTINUE; }
  char *json = d->info(d->user);
  char *ans = g_strconcat(ANSWER, json, NULL);
  if (strlen(ans) <= MAX_JSON) {
    sendto(fd, ans, strlen(ans), 0, (struct sockaddr *)&from, flen);
  }
  g_free(ans);
  g_free(json);
  return G_SOURCE_CONTINUE;
}

SkimHdDiscovery *skim_hd_discovery_new(SkimHdDiscoveryInfo info, gpointer user,
                                       GError **error) {
  int fd = udp_socket(error);
  if (fd < 0) { return NULL; }
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(skim_hd_discovery_port()),
                           .sin_addr.s_addr = htonl(INADDR_ANY) };
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "discovery port %u: %s",
                skim_hd_discovery_port(), g_strerror(errno));
    close(fd);
    return NULL;
  }
  SkimHdDiscovery *d = g_new0(SkimHdDiscovery, 1);
  d->fd = fd;
  d->info = info;
  d->user = user;
  d->watch = g_unix_fd_add(fd, G_IO_IN, on_request, d);
  return d;
}

void skim_hd_discovery_free(SkimHdDiscovery *d) {
  if (!d) { return; }
  g_source_remove(d->watch);
  close(d->fd);
  g_free(d);
}

char *skim_hd_discovery_find(guint timeout_ms) {
  GString *o = g_string_new("[");
  int fd = udp_socket(NULL);
  if (fd < 0) { return g_string_free(g_string_append_c(o, ']'), FALSE); }
  const guint16 port = skim_hd_discovery_port();
  /* the LAN, and this machine (a broadcast does not always loop back) */
  static const char *const DST[] = { "255.255.255.255", "127.0.0.1" };
  for (guint i = 0; i < G_N_ELEMENTS(DST); i++) {
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, DST[i], &a.sin_addr);
    sendto(fd, REQUEST, strlen(REQUEST), 0, (struct sockaddr *)&a, sizeof(a));
  }
  /* one answer per instance (the same one heard via the LAN and loopback):
   * its JSON → the sender, a LAN address preferred over 127.0.0.1 */
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GPtrArray *order = g_ptr_array_new();
  const gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  for (;;) {
    const gint64 left = end - g_get_monotonic_time();
    if (left <= 0) { break; }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, (int)((left + 999) / 1000)) <= 0) { continue; }
    char buf[MAX_JSON + 1];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    const gssize r = recvfrom(fd, buf, MAX_JSON, 0, (struct sockaddr *)&from, &flen);
    if (r <= 0) { continue; }
    buf[r] = '\0';
    const char *json = g_str_has_prefix(buf, ANSWER) ? buf + strlen(ANSWER) : NULL;
    if (!json || json[0] != '{' || buf[r - 1] != '}') { continue; }
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
    const char *had = g_hash_table_lookup(seen, json);
    if (!had) {
      char *key = g_strdup(json);
      g_hash_table_insert(seen, key, g_strdup(addr));
      g_ptr_array_add(order, key);
    } else if (g_str_has_prefix(had, "127.") && !g_str_has_prefix(addr, "127.")) {
      /* insert keeps the old key (the one order holds) and frees ours */
      g_hash_table_insert(seen, g_strdup(json), g_strdup(addr));
    }
  }
  for (guint i = 0; i < order->len; i++) {
    const char *json = order->pdata[i];
    g_string_append_printf(o, "%s{\"addr\":\"%s\"%s%s", i ? "," : "",
                           (const char *)g_hash_table_lookup(seen, json),
                           json[1] == '}' ? "" : ",", json + 1);
  }
  g_ptr_array_free(order, TRUE);
  g_hash_table_unref(seen);
  close(fd);
  g_string_append_c(o, ']');
  return g_string_free(o, FALSE);
}
