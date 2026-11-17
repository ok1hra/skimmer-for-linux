/* source_hub.c — a worker's IQ: the bands its hub passes on ([radio]
 * type=hub; headless/hub.h has the protocol).
 *
 * The start opens the link first (skim_hub_link_open): the hub's answer —
 * the fragment with the bands assigned to this worker's [hub] name — is
 * merged into the worker's config before its pipelines exist. The source
 * then reads the IQ frames on its own thread and hands each block on as
 * floats (n / 2^23 — the samples the hub's radio sent). A gap in a band's
 * seq counts as lost frames.
 *
 * When the link drops — the hub restarts on every settings change — the
 * source connects again every RETRY_S, for ever. A hub that now answers
 * with another fragment (other bands, another policy, or bands where there
 * were none) asks the owner for a restart (status.restart): the pipelines
 * are built from the fragment at the start.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "source.h"

#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discovery.h"
#include "hub.h"

#define RETRY_S        5
#define IQ_SILENCE_S   15             /* bands assigned and no frame that long */
#define MAX_FRAMES     65536

struct _SkimHubLink {
  GSocketConnection *conn;
  GSocket           *sock;
  char               desc[128];      /* "central (192.168.1.201:7380)"       */
};

/* ---- the link -------------------------------------------------------------------- */

/* One field of a flat discovery answer ("" when absent; numbers as text). */
static char *field(const char *obj, const char *key) {
  char *k = g_strdup_printf("\"%s\":", key);
  const char *p = strstr(obj, k);
  g_free(k);
  if (!p) { return g_strdup(""); }
  p = strchr(p, ':') + 1;
  if (*p == '"') {
    const char *e = strchr(p + 1, '"');
    return e ? g_strndup(p + 1, (gsize)(e - p - 1)) : g_strdup("");
  }
  const char *e = p;
  while (*e && *e != ',' && *e != '}') { e++; }
  return g_strndup(p, (gsize)(e - p));
}

/* host=auto: the hub that names this worker in its bands, else the first
 * instance that runs a hub server and is no worker itself. */
static gboolean find_hub(const char *name, char **host, guint16 *port, char **hub_name) {
  char *found = skim_hd_discovery_find(1000);
  char **objs = g_strsplit(found, "{\"addr\":", -1);
  char *mine = g_strdup_printf(" @%s\"", name);
  int best = -1, any = -1;
  for (int i = 1; objs[i]; i++) {
    char *obj = g_strconcat("{\"addr\":", objs[i], NULL);
    char *hp = field(obj, "hub_port"), *role = field(obj, "role");
    if (atoi(hp) > 0 && strcmp(role, "worker") != 0) {
      if (best < 0 && strstr(obj, mine)) { best = i; }
      if (any < 0) { any = i; }
    }
    g_free(hp);
    g_free(role);
    g_free(obj);
  }
  const int pick = best >= 0 ? best : any;
  if (pick >= 0) {
    char *obj = g_strconcat("{\"addr\":", objs[pick], NULL);
    char *hp = field(obj, "hub_port");
    *host = field(obj, "addr");
    *port = (guint16)atoi(hp);
    *hub_name = field(obj, "name");
    g_free(hp);
    g_free(obj);
  }
  g_free(mine);
  g_strfreev(objs);
  g_free(found);
  return pick >= 0;
}

static char *read_line(GSocket *s) {
  GString *l = g_string_new(NULL);
  char c;
  for (;;) {
    const gssize n = g_socket_receive(s, &c, 1, NULL, NULL);
    if (n <= 0) {
      g_string_free(l, TRUE);
      return NULL;
    }
    if (c == '\n') { break; }
    if (l->len > 512) {
      g_string_free(l, TRUE);
      return NULL;
    }
    g_string_append_c(l, c);
  }
  return g_string_free(l, FALSE);
}

static gboolean read_n(GSocket *s, void *buf, gsize n) {
  char *p = buf;
  while (n) {
    const gssize r = g_socket_receive(s, p, n, NULL, NULL);
    if (r <= 0) { return FALSE; }
    p += r;
    n -= (gsize)r;
  }
  return TRUE;
}

/* One try: find, connect, HELLO, the answer. */
static SkimHubLink *link_try(const SkimHdConfig *cfg, char **fragment, GError **error) {
  const char *spec = cfg->hosts && cfg->hosts[0] ? cfg->hosts[0] : SKIM_HD_HOST_AUTO;
  char *host = NULL, *hub_name = NULL;
  guint16 port = SKIM_HD_HUB_PORT;
  if (g_ascii_strcasecmp(spec, SKIM_HD_HOST_AUTO) == 0) {
    if (!find_hub(cfg->hub_name, &host, &port, &hub_name)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "no hub answered on this LAN ([radio] host=auto)");
      return NULL;
    }
  } else {
    const char *colon = strrchr(spec, ':');
    guint64 p = 0;
    if (colon && strchr(spec, ':') == colon &&
        g_ascii_string_to_unsigned(colon + 1, 10, 1, 65535, &p, NULL)) {
      host = g_strndup(spec, (gsize)(colon - spec));
      port = (guint16)p;
    } else {
      host = g_strdup(spec);
    }
  }
  GSocketClient *sc = g_socket_client_new();
  g_socket_client_set_timeout(sc, 5);
  GSocketConnection *conn = g_socket_client_connect_to_host(sc, host, port, NULL, error);
  g_object_unref(sc);
  SkimHubLink *l = NULL;
  if (conn) {
    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_timeout(sock, 5);
    char *hello = g_strdup_printf(SKIM_HD_HUB_PROTO " HELLO name=%s version=%s\n",
                                  cfg->hub_name, SKIMMER_VERSION);
    const gboolean sent = g_socket_send(sock, hello, strlen(hello), NULL, error) > 0;
    g_free(hello);
    char *ans = sent ? read_line(sock) : NULL;
    gsize n = 0;
    if (ans && sscanf(ans, SKIM_HD_HUB_PROTO " CONFIG %zu", &n) == 1 && n < 65536) {
      char *frag = g_malloc0(n + 1);
      if (read_n(sock, frag, n)) {
        *fragment = frag;
        l = g_new0(SkimHubLink, 1);
      } else {
        g_free(frag);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the hub's CONFIG was cut short");
      }
    } else if (ans && strcmp(ans, SKIM_HD_HUB_PROTO " NONE") == 0) {
      *fragment = NULL;
      l = g_new0(SkimHubLink, 1);
    } else if (ans && g_str_has_prefix(ans, SKIM_HD_HUB_PROTO " REFUSED")) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "the hub refused: %s",
                  ans + strlen(SKIM_HD_HUB_PROTO " REFUSED "));
    } else if (sent) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s:%u is no hub", host, port);
    }
    g_free(ans);
    if (l) {
      l->conn = conn;
      l->sock = sock;
      g_socket_set_timeout(sock, 0);
      g_socket_set_keepalive(sock, TRUE);
      g_snprintf(l->desc, sizeof(l->desc), "%s%s%s:%u%s", hub_name ? hub_name : "",
                 hub_name ? " (" : "", host, port, hub_name ? ")" : "");
    } else {
      g_object_unref(conn);
    }
  }
  if (!l && error && *error) { g_prefix_error(error, "hub %s:%u: ", host, port); }
  g_free(host);
  g_free(hub_name);
  return l;
}

SkimHubLink *skim_hub_link_open(const SkimHdConfig *cfg, guint wait_s, char **fragment,
                                GError **error) {
  const gint64 end = g_get_monotonic_time() + (gint64)wait_s * G_USEC_PER_SEC;
  for (;;) {
    GError *e = NULL;
    SkimHubLink *l = link_try(cfg, fragment, &e);
    if (l || g_get_monotonic_time() >= end ||
        g_error_matches(e, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
      if (!l) { g_propagate_error(error, e); }
      return l;
    }
    g_clear_error(&e);
    g_usleep(2 * G_USEC_PER_SEC);
  }
}

void skim_hub_link_close(SkimHubLink *l) {
  if (!l) { return; }
  g_io_stream_close(G_IO_STREAM(l->conn), NULL, NULL);
  g_object_unref(l->conn);
  g_free(l);
}

const char *skim_hub_link_desc(const SkimHubLink *l) { return l ? l->desc : ""; }

/* ---- the source ------------------------------------------------------------------ */

typedef struct {
  SkimIqSource  base;
  GThread      *th;
  volatile gint stop;
  GMutex        lock;               /* link (writes), status fields          */
  SkimHubLink  *link;
  char         *running;            /* the fragment the start built on       */
  guint         nb;
  gboolean      restart;
  char          state[160], desc[128];
  guint64       packets, lost;
  gint64        last_frame_us;
  guint64       next_seq[SKIM_HD_MAX_BANDS];
  gboolean      seen[SKIM_HD_MAX_BANDS];
} Hub;

static gboolean same_frag(const char *a, const char *b) {
  return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static void hub_state(Hub *h, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void hub_state(Hub *h, const char *fmt, ...) {
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  g_mutex_lock(&h->lock);
  const gboolean changed = strcmp(s, h->state) != 0;
  g_strlcpy(h->state, s, sizeof(h->state));
  g_mutex_unlock(&h->lock);
  if (changed) { g_message("hub link: %s", s); }
}

/* Read frames until the link breaks. */
static void read_frames(Hub *h, SkimHubLink *l) {
  float *buf = g_new(float, 2 * MAX_FRAMES);
  guint8 *raw = g_malloc(6 * MAX_FRAMES);
  memset(h->seen, 0, sizeof(h->seen));
  if (h->nb) { g_socket_set_timeout(l->sock, IQ_SILENCE_S); }
  while (!g_atomic_int_get(&h->stop)) {
    guint8 hd[SKIM_HD_HUB_HDR];
    if (!read_n(l->sock, hd, sizeof(hd))) { break; }
    guint32 magic, n, rate;
    guint64 seq, cu;
    memcpy(&magic, hd, 4);
    memcpy(&n, hd + 8, 4);
    memcpy(&rate, hd + 12, 4);
    memcpy(&seq, hd + 16, 8);
    memcpy(&cu, hd + 24, 8);
    magic = GUINT32_FROM_LE(magic);
    n = GUINT32_FROM_LE(n);
    rate = GUINT32_FROM_LE(rate);
    seq = GUINT64_FROM_LE(seq);
    union { guint64 u; double d; } c = { .u = GUINT64_FROM_LE(cu) };
    const guint band = hd[4];
    if (magic != SKIM_HD_HUB_MAGIC || n > MAX_FRAMES || !read_n(l->sock, raw, (gsize)n * 6)) {
      break;
    }
    for (guint i = 0; i < 2 * n; i++) {
      const guint8 *p = raw + 3 * i;
      const gint32 x = (gint32)((guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16);
      buf[i] = (float)((x ^ 0x800000) - 0x800000) / SKIM_HD_HUB_FULLSCALE;
    }
    if (band >= h->nb) { continue; }
    g_mutex_lock(&h->lock);
    if (h->seen[band] && seq > h->next_seq[band]) { h->lost += seq - h->next_seq[band]; }
    h->seen[band] = TRUE;
    h->next_seq[band] = seq + n;
    h->packets++;
    h->last_frame_us = g_get_monotonic_time();
    g_mutex_unlock(&h->lock);
    h->base.iq_cb(band, buf, n, rate, c.d, h->base.iq_user);
  }
  g_free(raw);
  g_free(buf);
}

static gpointer hub_thread(gpointer data) {
  Hub *h = data;
  for (;;) {
    SkimHubLink *l = h->link;
    if (l) {
      if (h->nb) {
        hub_state(h, "streaming from the hub %s: %u band%s", l->desc, h->nb, h->nb == 1 ? "" : "s");
        read_frames(h, l);
      } else {
        hub_state(h, "connected to the hub %s — no band assigned to %s yet", l->desc,
                  h->base.cfg->hub_name);
        char c;                                  /* idle: wait for it to close */
        while (!g_atomic_int_get(&h->stop) && g_socket_receive(l->sock, &c, 1, NULL, NULL) > 0) {}
      }
      g_mutex_lock(&h->lock);
      h->link = NULL;
      g_mutex_unlock(&h->lock);
      skim_hub_link_close(l);
      if (g_atomic_int_get(&h->stop)) { break; }
      hub_state(h, "the hub %s dropped the link — again in %d s", h->desc, RETRY_S);
    }
    for (int i = 0; i < RETRY_S * 10 && !g_atomic_int_get(&h->stop); i++) { g_usleep(100000); }
    if (g_atomic_int_get(&h->stop)) { break; }
    char *frag = NULL;
    GError *err = NULL;
    l = skim_hub_link_open(h->base.cfg, 0, &frag, &err);
    if (!l) {
      hub_state(h, "%s — again in %d s", err->message, RETRY_S);
      g_clear_error(&err);
      continue;
    }
    g_strlcpy(h->desc, l->desc, sizeof(h->desc));
    if (!same_frag(frag, h->running)) {          /* other bands: rebuild      */
      hub_state(h, "the hub %s assigns other bands now — restarting", l->desc);
      g_mutex_lock(&h->lock);
      h->restart = TRUE;
      h->link = l;                               /* keep it until the restart */
      g_mutex_unlock(&h->lock);
      g_free(frag);
      while (!g_atomic_int_get(&h->stop)) { g_usleep(100000); }
      break;
    }
    g_free(frag);
    g_mutex_lock(&h->lock);
    h->link = l;
    g_mutex_unlock(&h->lock);
  }
  return NULL;
}

static void hub_start(SkimIqSource *s) {
  Hub *h = (Hub *)s;
  h->th = g_thread_new("skim-hub-link", hub_thread, h);
}

static void hub_tick(SkimIqSource *s) { (void)s; }

static void hub_status(SkimIqSource *s, SkimIqSourceStatus *o) {
  Hub *h = (Hub *)s;
  memset(o, 0, sizeof(*o));
  g_mutex_lock(&h->lock);
  o->streaming = h->link && h->nb &&
                 g_get_monotonic_time() - h->last_frame_us < 2 * G_USEC_PER_SEC;
  o->restart = h->restart;
  g_strlcpy(o->state, h->state, sizeof(o->state));
  g_strlcpy(o->host, h->link ? h->link->desc : h->desc, sizeof(o->host));
  o->packets = h->packets;
  o->lost_frames = h->lost;
  g_mutex_unlock(&h->lock);
}

static void hub_free(SkimIqSource *s) {
  Hub *h = (Hub *)s;
  g_atomic_int_set(&h->stop, 1);
  g_mutex_lock(&h->lock);
  if (h->link) { g_socket_shutdown(h->link->sock, TRUE, TRUE, NULL); }   /* wake the reader */
  g_mutex_unlock(&h->lock);
  if (h->th) { g_thread_join(h->th); }
  skim_hub_link_close(h->link);
  g_mutex_clear(&h->lock);
  g_free(h->running);
  g_free(h);
}

static const SkimIqSourceVtable HUB_VT = {
  .start = hub_start, .tick = hub_tick, .status = hub_status, .free = hub_free,
};

SkimIqSource *skim_iq_source_hub_new(const SkimHdConfig *cfg, SkimHubLink *link,
                                     const char *fragment) {
  Hub *h = g_new0(Hub, 1);
  h->base.vt = &HUB_VT;
  g_mutex_init(&h->lock);
  h->link = link;
  h->running = g_strdup(fragment);
  h->nb = cfg->nb;
  if (link) { g_strlcpy(h->desc, link->desc, sizeof(h->desc)); }
  g_strlcpy(h->state, link ? "connected to the hub" : "looking for the hub", sizeof(h->state));
  return &h->base;
}

void skim_iq_source_hub_send(SkimIqSource *s, const char *line) {
  Hub *h = (Hub *)s;
  g_mutex_lock(&h->lock);
  if (h->link) {
    gsize n = strlen(line);
    const char *p = line;
    while (n) {
      const gssize w = g_socket_send(h->link->sock, p, n, NULL, NULL);
      if (w <= 0) { break; }                     /* the reader sees the break */
      p += w;
      n -= (gsize)w;
    }
  }
  g_mutex_unlock(&h->lock);
}
