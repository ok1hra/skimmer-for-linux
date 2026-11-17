/* hub.c — see hub.h. Part of skimmer-for-linux. GPL-3.0-or-later.
 *
 * The accept side is a GThreadedSocketService on the hub's own context
 * thread; each connection runs in a service thread of its own (link_run):
 * the handshake, then a loop that writes the queued IQ blocks and reads the
 * worker's lines. push() converts a block to int24 once and queues it on
 * the link that has the band; it never blocks.
 */
#include "hub.h"

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "web.h"

#define WORKER_QUEUE_S  2.0          /* the most IQ a link may hold back     */
#define HELLO_WAIT_S    5
#define SEND_WAIT_S     5            /* a worker that takes nothing that long is gone */

typedef struct {
  SkimHdHub    *hub;
  char          name[40];
  char          addr[64];
  GSocket      *sock;                /* borrowed from the connection         */
  gboolean      assigned;            /* got a CONFIG                         */
  int           k_of[SKIM_HD_MAX_BANDS];   /* hub band → fragment index, -1  */
  GMutex        lock;                /* queue, counters, status              */
  GCond         cond;
  GQueue        queue;               /* GBytes                               */
  gsize         queued, cap;
  guint64       seq[SKIM_HD_MAX_BANDS];
  guint64       dropped, sent_bytes, spots;
  guint64       sent_prev;
  gint64        sent_prev_us;
  double        mbps;
  char         *status_json;
  int           active[SKIM_HD_MAX_BANDS][2];
  gboolean      dead;
} Link;

struct _SkimHdHub {
  GMainContext    *ctx;
  GMainLoop       *loop;
  GThread         *thread;
  GSocketService  *svc;
  guint16          port;
  char           **allow;
  char             band_worker[SKIM_HD_MAX_BANDS][32];
  guint            nb;
  SkimHdHubAssign  assign;
  SkimHdHubSpot    spot;
  gpointer         user;
  GMutex           lock;             /* links                                */
  GPtrArray       *links;            /* Link*                                */
  volatile gint    stopping;
  volatile gint    running;          /* link threads inside link_run         */
};

/* ---- the link's thread ----------------------------------------------------------- */

static gboolean send_all(GSocket *s, const char *p, gsize n) {
  while (n) {
    const gssize w = g_socket_send(s, p, n, NULL, NULL);
    if (w <= 0) { return FALSE; }
    p += w;
    n -= (gsize)w;
  }
  return TRUE;
}

static gboolean send_str(GSocket *s, const char *str) { return send_all(s, str, strlen(str)); }

/* A line from the socket (blocking with the socket's timeout); NULL on EOF. */
static char *read_line(GSocket *s, GString *buf) {
  for (;;) {
    char *nl = strchr(buf->str, '\n');
    if (nl) {
      char *line = g_strndup(buf->str, (gsize)(nl - buf->str));
      g_string_erase(buf, 0, (gssize)(nl - buf->str) + 1);
      g_strchomp(line);
      return line;
    }
    if (buf->len > 1 << 20) { return NULL; }             /* no line that long */
    char tmp[4096];
    const gssize n = g_socket_receive(s, tmp, sizeof(tmp), NULL, NULL);
    if (n <= 0) { return NULL; }
    g_string_append_len(buf, tmp, n);
  }
}

/* A worker's line (its thread). */
static void link_line(Link *l, const char *line, char **names) {
  SkimHdHub *h = l->hub;
  if (g_str_has_prefix(line, "SPOT ")) {
    char call[24], mode[12];
    double f, snr, speed;
    if (sscanf(line + 5, "%23s %11s %lf %lf %lf", call, mode, &f, &snr, &speed) == 5) {
      g_mutex_lock(&l->lock);
      l->spots++;
      g_mutex_unlock(&l->lock);
      h->spot(l->name, call, mode, f, snr, speed, h->user);
    }
  } else if (g_str_has_prefix(line, "STAT ")) {
    char band[24], mode[8];
    unsigned dec, act, st, sp;
    if (sscanf(line + 5, "%23s %7s %u %u %u %u", band, mode, &dec, &act, &st, &sp) == 6) {
      for (guint b = 0; b < h->nb; b++) {
        if (l->k_of[b] >= 0 && names[b] && strcmp(names[b], band) == 0) {
          g_mutex_lock(&l->lock);
          l->active[b][strcmp(mode, "rtty") == 0] = (int)act;
          g_mutex_unlock(&l->lock);
        }
      }
    }
  } else if (g_str_has_prefix(line, "STATUS {")) {
    g_mutex_lock(&l->lock);
    g_free(l->status_json);
    l->status_json = g_strdup(line + 7);
    g_mutex_unlock(&l->lock);
  }
}

static void link_free(Link *l) {
  GBytes *b;
  while ((b = g_queue_pop_head(&l->queue))) { g_bytes_unref(b); }
  g_mutex_clear(&l->lock);
  g_cond_clear(&l->cond);
  g_free(l->status_json);
  g_free(l);
}

static gboolean link_run(GThreadedSocketService *svc, GSocketConnection *conn,
                         GObject *src, gpointer data) {
  (void)svc; (void)src;
  SkimHdHub *h = data;
  g_atomic_int_inc(&h->running);
  GSocket *sock = g_socket_connection_get_socket(conn);
  GSocketAddress *ra = g_socket_connection_get_remote_address(conn, NULL);
  GInetAddress *ia = ra && G_IS_INET_SOCKET_ADDRESS(ra)
                         ? g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(ra)) : NULL;
  char *addr = ia ? g_inet_address_to_string(ia) : g_strdup("?");
  const gboolean ok_addr = ia && skim_hd_web_addr_allowed((const char *const *)h->allow, ia);
  g_clear_object(&ra);
  g_socket_set_timeout(sock, HELLO_WAIT_S);
  GString *rx = g_string_new(NULL);
  char *hello = ok_addr ? read_line(sock, rx) : NULL;
  char name[40] = "";
  const char *np = hello ? strstr(hello, " name=") : NULL;
  if (np) { sscanf(np + 6, "%39[A-Za-z0-9._-]", name); }
  if (!ok_addr) {
    send_str(sock, SKIM_HD_HUB_PROTO " REFUSED this address may not connect ([hub] allow)\n");
    g_message("hub: %s refused ([hub] allow)", addr);
  } else if (!hello || !g_str_has_prefix(hello, SKIM_HD_HUB_PROTO " HELLO") || !name[0]) {
    send_str(sock, SKIM_HD_HUB_PROTO " REFUSED no HELLO name=…\n");
  } else {
    g_mutex_lock(&h->lock);
    gboolean dup = FALSE;
    for (guint i = 0; i < h->links->len && !dup; i++) {
      const Link *o = h->links->pdata[i];
      dup = !o->dead && strcmp(o->name, name) == 0;
    }
    g_mutex_unlock(&h->lock);
    char *frag = dup ? NULL : h->assign(name, h->user);
    if (dup) {
      send_str(sock, SKIM_HD_HUB_PROTO " REFUSED a worker of that name is connected already\n");
      g_message("hub: %s from %s refused — that name is connected already", name, addr);
    } else {
      Link *l = g_new0(Link, 1);
      l->hub = h;
      g_strlcpy(l->name, name, sizeof(l->name));
      g_strlcpy(l->addr, addr, sizeof(l->addr));
      l->sock = sock;
      g_mutex_init(&l->lock);
      g_cond_init(&l->cond);
      g_queue_init(&l->queue);
      for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) {
        l->k_of[b] = -1;
        l->active[b][0] = l->active[b][1] = -1;
      }
      char *names[SKIM_HD_MAX_BANDS] = { NULL };
      guint k = 0;
      double rate = 96000;
      /* the fragment lists the bands assigned to the name in hub order: the
       * k-th of them is the worker's band k (and its name, for STAT lines) */
      if (frag) {
        SkimHdConfig fc;
        if (skim_hd_config_parse(&fc, frag, strlen(frag), "fragment", NULL)) {
          rate = fc.rate;
          for (guint b = 0; b < h->nb && k < fc.nb; b++) {
            if (strcmp(h->band_worker[b], name) == 0) {
              names[b] = g_strdup(fc.band[k].name);
              l->k_of[b] = (int)k++;
            }
          }
          skim_hd_config_clear(&fc);
        }
        l->assigned = k > 0;
      }
      /* the queue holds WORKER_QUEUE_S of its bands: frames × 6 bytes + headers */
      l->cap = (gsize)(WORKER_QUEUE_S * rate * 6.0 * MAX(k, 1u)) + 64 * 1024;
      char *head = frag ? g_strdup_printf(SKIM_HD_HUB_PROTO " CONFIG %zu\n", strlen(frag))
                        : g_strdup(SKIM_HD_HUB_PROTO " NONE\n");
      gboolean alive = send_str(sock, head) && (!frag || send_str(sock, frag));
      g_free(head);
      g_message("hub: worker %s from %s %s", name, addr,
                !alive ? "lost at once" : k ? "connected" : "connected, nothing assigned to it");
      g_mutex_lock(&h->lock);
      g_ptr_array_add(h->links, l);
      g_mutex_unlock(&h->lock);
      g_socket_set_timeout(sock, SEND_WAIT_S);
      l->sent_prev_us = g_get_monotonic_time();
      while (alive && !g_atomic_int_get(&h->stopping)) {
        g_mutex_lock(&l->lock);
        if (!l->queue.length) {
          g_cond_wait_until(&l->cond, &l->lock, g_get_monotonic_time() + 100 * 1000);
        }
        GQueue out = l->queue;               /* take them all, write unlocked */
        g_queue_init(&l->queue);
        l->queued = 0;
        g_mutex_unlock(&l->lock);
        GBytes *b;
        while ((b = g_queue_pop_head(&out))) {
          gsize n = 0;
          const char *p = g_bytes_get_data(b, &n);
          if (alive && !send_all(sock, p, n)) { alive = FALSE; }
          if (alive) {
            g_mutex_lock(&l->lock);
            l->sent_bytes += n;
            g_mutex_unlock(&l->lock);
          }
          g_bytes_unref(b);
        }
        /* the worker's lines, as far as they are here */
        while (alive && g_socket_condition_check(sock, G_IO_IN | G_IO_HUP | G_IO_ERR)) {
          char tmp[4096];
          GError *e = NULL;
          const gssize n = g_socket_receive_with_blocking(sock, tmp, sizeof(tmp), FALSE, NULL, &e);
          if (n <= 0) {
            if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) { alive = FALSE; }
            g_clear_error(&e);
            break;
          }
          g_string_append_len(rx, tmp, n);
          char *nl;
          while ((nl = strchr(rx->str, '\n'))) {
            *nl = '\0';
            char *line = g_strdup(rx->str);
            g_string_erase(rx, 0, (gssize)(nl - rx->str) + 1);
            g_strchomp(line);
            link_line(l, line, names);
            g_free(line);
          }
          if (rx->len > 1 << 20) { alive = FALSE; }
        }
      }
      if (!g_atomic_int_get(&h->stopping)) { g_message("hub: worker %s (%s) gone", name, addr); }
      g_mutex_lock(&h->lock);
      g_ptr_array_remove(h->links, l);
      g_mutex_unlock(&h->lock);
      for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) { g_free(names[b]); }
      link_free(l);
    }
    g_free(frag);
  }
  g_free(hello);
  g_string_free(rx, TRUE);
  g_free(addr);
  g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
  g_atomic_int_add(&h->running, -1);
  return TRUE;
}

/* ---- push (the source's thread) ------------------------------------------------- */

static inline void put24(guint8 *p, float v) {
  float x = v * SKIM_HD_HUB_FULLSCALE;
  x = x > 8388607.0f ? 8388607.0f : x < -8388608.0f ? -8388608.0f : x;
  const gint32 i = (gint32)lrintf(x);
  p[0] = (guint8)(i & 0xff);
  p[1] = (guint8)((i >> 8) & 0xff);
  p[2] = (guint8)((i >> 16) & 0xff);
}

void skim_hd_hub_push(SkimHdHub *h, guint band, const float *iq, guint n, double rate,
                      double centre_hz) {
  if (band >= h->nb || !h->band_worker[band][0]) { return; }
  g_mutex_lock(&h->lock);
  for (guint i = 0; i < h->links->len; i++) {
    Link *l = h->links->pdata[i];
    const int k = l->k_of[band];
    if (k < 0) { continue; }
    const gsize len = SKIM_HD_HUB_HDR + (gsize)n * 6;
    g_mutex_lock(&l->lock);
    const guint64 seq = l->seq[band];
    l->seq[band] += n;
    if (l->queued + len > l->cap) {          /* the worker lags: drop, count  */
      l->dropped += n;
      g_mutex_unlock(&l->lock);
      continue;
    }
    g_mutex_unlock(&l->lock);
    guint8 *buf = g_malloc(len);
    const guint32 magic = GUINT32_TO_LE(SKIM_HD_HUB_MAGIC), fr = GUINT32_TO_LE(n),
                  rt = GUINT32_TO_LE((guint32)rate);
    const guint64 sq = GUINT64_TO_LE(seq);
    union { double d; guint64 u; } c = { .d = centre_hz };
    const guint64 cu = GUINT64_TO_LE(c.u);
    memset(buf, 0, SKIM_HD_HUB_HDR);
    memcpy(buf, &magic, 4);
    buf[4] = (guint8)k;
    memcpy(buf + 8, &fr, 4);
    memcpy(buf + 12, &rt, 4);
    memcpy(buf + 16, &sq, 8);
    memcpy(buf + 24, &cu, 8);
    guint8 *p = buf + SKIM_HD_HUB_HDR;
    for (guint j = 0; j < 2 * n; j++, p += 3) { put24(p, iq[j]); }
    g_mutex_lock(&l->lock);
    g_queue_push_tail(&l->queue, g_bytes_new_take(buf, len));
    l->queued += len;
    g_cond_signal(&l->cond);
    g_mutex_unlock(&l->lock);
  }
  g_mutex_unlock(&h->lock);
}

/* ---- the server ------------------------------------------------------------------ */

static gpointer hub_thread(gpointer data) {
  SkimHdHub *h = data;
  g_main_context_push_thread_default(h->ctx);
  g_main_loop_run(h->loop);
  g_main_context_pop_thread_default(h->ctx);
  return NULL;
}

SkimHdHub *skim_hd_hub_new(guint16 port, const char *const *allow,
                           const char *const *band_worker, guint nb, SkimHdHubAssign assign,
                           SkimHdHubSpot spot, gpointer user, GError **error) {
  SkimHdHub *h = g_new0(SkimHdHub, 1);
  h->allow = g_strdupv((char **)allow);
  h->nb = MIN(nb, (guint)SKIM_HD_MAX_BANDS);
  for (guint b = 0; b < h->nb; b++) {
    g_strlcpy(h->band_worker[b], band_worker[b] ? band_worker[b] : "", 32);
  }
  h->assign = assign;
  h->spot = spot;
  h->user = user;
  g_mutex_init(&h->lock);
  h->links = g_ptr_array_new();
  h->ctx = g_main_context_new();
  h->loop = g_main_loop_new(h->ctx, FALSE);
  g_main_context_push_thread_default(h->ctx);
  h->svc = g_threaded_socket_service_new(SKIM_HD_MAX_BANDS + 4);
  gboolean ok;
  if (port == 0) {
    h->port = g_socket_listener_add_any_inet_port(G_SOCKET_LISTENER(h->svc), NULL, error);
    ok = h->port != 0;
  } else {
    ok = g_socket_listener_add_inet_port(G_SOCKET_LISTENER(h->svc), port, NULL, error);
    h->port = port;
  }
  g_signal_connect(h->svc, "run", G_CALLBACK(link_run), h);
  g_main_context_pop_thread_default(h->ctx);
  if (!ok) {
    g_object_unref(h->svc);
    g_main_loop_unref(h->loop);
    g_main_context_unref(h->ctx);
    g_ptr_array_unref(h->links);
    g_strfreev(h->allow);
    g_free(h);
    return NULL;
  }
  h->thread = g_thread_new("skim-hub", hub_thread, h);
  return h;
}

static gboolean stop_cb(gpointer data) {
  SkimHdHub *h = data;
  g_socket_service_stop(h->svc);
  g_socket_listener_close(G_SOCKET_LISTENER(h->svc));
  g_main_loop_quit(h->loop);
  return G_SOURCE_REMOVE;
}

void skim_hd_hub_free(SkimHdHub *h) {
  if (!h) { return; }
  g_atomic_int_set(&h->stopping, 1);
  g_mutex_lock(&h->lock);
  for (guint i = 0; i < h->links->len; i++) {        /* wake the link threads */
    Link *l = h->links->pdata[i];
    g_socket_shutdown(l->sock, TRUE, TRUE, NULL);
    g_mutex_lock(&l->lock);
    g_cond_signal(&l->cond);
    g_mutex_unlock(&l->lock);
  }
  g_mutex_unlock(&h->lock);
  g_main_context_invoke(h->ctx, stop_cb, h);
  g_thread_join(h->thread);
  /* the link threads end — one still waiting for a HELLO within HELLO_WAIT_S */
  for (int t = 0; t < (HELLO_WAIT_S + 2) * 100 && g_atomic_int_get(&h->running); t++) {
    g_mutex_lock(&h->lock);
    for (guint i = 0; i < h->links->len; i++) {
      g_socket_shutdown(((Link *)h->links->pdata[i])->sock, TRUE, TRUE, NULL);
    }
    g_mutex_unlock(&h->lock);
    g_usleep(10 * 1000);
  }
  g_object_unref(h->svc);                            /* joins its threads     */
  g_main_loop_unref(h->loop);
  g_main_context_unref(h->ctx);
  g_ptr_array_unref(h->links);
  g_mutex_clear(&h->lock);
  g_strfreev(h->allow);
  g_free(h);
}

guint16 skim_hd_hub_port(const SkimHdHub *h) { return h->port; }

char *skim_hd_hub_json(SkimHdHub *h) {
  GString *o = g_string_new(NULL);
  g_string_append_printf(o, "{\"port\":%u,\"workers\":[", h->port);
  const gint64 now = g_get_monotonic_time();
  g_mutex_lock(&h->lock);
  for (guint i = 0; i < h->links->len; i++) {
    Link *l = h->links->pdata[i];
    g_mutex_lock(&l->lock);
    const double dt = (now - l->sent_prev_us) / 1e6;
    if (dt >= 0.5) {
      l->mbps = 8.0 * (double)(l->sent_bytes - l->sent_prev) / dt / 1e6;
      l->sent_prev = l->sent_bytes;
      l->sent_prev_us = now;
    }
    g_string_append_printf(o, "%s{\"name\":\"", i ? "," : "");
    skim_json_append(o, l->name);
    g_string_append(o, "\",\"addr\":\"");
    skim_json_append(o, l->addr);
    g_string_append_printf(o, "\",\"assigned\":%s,\"bands\":[", l->assigned ? "true" : "false");
    guint n = 0;
    for (guint b = 0; b < h->nb; b++) {
      if (l->k_of[b] >= 0) { g_string_append_printf(o, "%s%u", n++ ? "," : "", b); }
    }
    g_string_append_printf(o, "],\"mbps\":%.2f,\"dropped\":%" G_GUINT64_FORMAT
                           ",\"spots\":%" G_GUINT64_FORMAT ",\"status\":%s}",
                           l->mbps, l->dropped, l->spots, l->status_json ? l->status_json : "null");
    g_mutex_unlock(&l->lock);
  }
  g_mutex_unlock(&h->lock);
  g_string_append(o, "]}");
  return g_string_free(o, FALSE);
}

int skim_hd_hub_band_active(SkimHdHub *h, guint band, guint mode) {
  int v = -1;
  if (band >= h->nb || mode > 1) { return -1; }
  g_mutex_lock(&h->lock);
  for (guint i = 0; i < h->links->len && v < 0; i++) {
    Link *l = h->links->pdata[i];
    if (l->k_of[band] < 0) { continue; }
    g_mutex_lock(&l->lock);
    v = l->active[band][mode];
    g_mutex_unlock(&l->lock);
  }
  g_mutex_unlock(&h->lock);
  return v;
}

char **skim_hd_hub_idle_workers(SkimHdHub *h) {
  GPtrArray *o = g_ptr_array_new();
  g_mutex_lock(&h->lock);
  for (guint i = 0; i < h->links->len; i++) {
    const Link *l = h->links->pdata[i];
    if (!l->assigned) { g_ptr_array_add(o, g_strdup(l->name)); }
  }
  g_mutex_unlock(&h->lock);
  g_ptr_array_add(o, NULL);
  return (char **)g_ptr_array_free(o, FALSE);
}
