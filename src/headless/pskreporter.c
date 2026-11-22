/* pskreporter.c — the FT8/FT4 stations heard, to PSK Reporter (pskreporter.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "pskreporter.h"

#include <gio/gio.h>
#include <string.h>

#include "json.h"

/* ---- the datagram (https://pskreporter.info/pskdev.html, byte for byte) ---------- */

/* Receiver options template "R1": receiverCallsign, receiverLocator,
 * decodingSoftware (enterprise 30351, variable length) — template 0x9992. */
static const guint8 TMPL_RX[] = {
  0x00, 0x03, 0x00, 0x24, 0x99, 0x92, 0x00, 0x03, 0x00, 0x01,
  0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x00, 0x00,
};
/* Sender template "sNR, iMD … senderLocator": senderCallsign, frequency (4),
 * sNR (1), iMD (1), mode, informationSource (1), senderLocator,
 * flowStartSeconds (4) — template 0x9993. */
static const guint8 TMPL_TX[] = {
  0x00, 0x02, 0x00, 0x44, 0x99, 0x93, 0x00, 0x08,
  0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x07, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
  0x80, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
  0x00, 0x96, 0x00, 0x04,
};
G_STATIC_ASSERT(sizeof(TMPL_RX) == 0x24);
G_STATIC_ASSERT(sizeof(TMPL_TX) == 0x44);

static void put16(GByteArray *b, guint v) {
  const guint8 x[2] = { (guint8)(v >> 8), (guint8)v };
  g_byte_array_append(b, x, 2);
}
static void put32(GByteArray *b, guint32 v) {
  const guint8 x[4] = { (guint8)(v >> 24), (guint8)(v >> 16), (guint8)(v >> 8), (guint8)v };
  g_byte_array_append(b, x, 4);
}
static void put8(GByteArray *b, guint v) {
  const guint8 x = (guint8)v;
  g_byte_array_append(b, &x, 1);
}
/* a string: one length byte, then its bytes (254 at most) */
static void putstr(GByteArray *b, const char *s) {
  const gsize n = MIN(strlen(s ? s : ""), 254u);
  put8(b, (guint)n);
  if (n) { g_byte_array_append(b, (const guint8 *)s, (guint)n); }
}
static void pad4(GByteArray *b, guint from) {
  while ((b->len - from) % 4) { put8(b, 0); }
}
static void set16(GByteArray *b, guint at, guint v) {
  b->data[at] = (guint8)(v >> 8);
  b->data[at + 1] = (guint8)v;
}
static gsize report_len(const SkimPskrReport *r) {
  return 1 + MIN(strlen(r->call), 254u) + 4 + 1 + 1 + 1 + MIN(strlen(r->mode), 254u) + 1 + 1 +
         MIN(strlen(r->grid), 254u) + 4;
}

GBytes *skim_pskr_encode(const char *rcall, const char *rgrid, const char *software,
                         guint32 seq, guint32 odid, guint32 now, gboolean templates,
                         const SkimPskrReport *r, guint n, gsize max_bytes, guint *used) {
  GByteArray *b = g_byte_array_new();
  put16(b, 0x000A);                                 /* IPFIX                   */
  put16(b, 0);                                      /* the length, below       */
  put32(b, now);
  put32(b, seq);
  put32(b, odid);
  if (templates) {
    g_byte_array_append(b, TMPL_RX, sizeof TMPL_RX);
    g_byte_array_append(b, TMPL_TX, sizeof TMPL_TX);
  }
  /* the receiver */
  guint at = b->len;
  put16(b, 0x9992);
  put16(b, 0);
  putstr(b, rcall);
  putstr(b, rgrid);
  putstr(b, software);
  pad4(b, at);
  set16(b, at + 2, b->len - at);
  /* the senders that fit */
  guint k = 0;
  if (n) {
    at = b->len;
    put16(b, 0x9993);
    put16(b, 0);
    for (; k < n; k++) {
      const gsize need = report_len(&r[k]);
      if (k && b->len + need + 3 > max_bytes) { break; }   /* +3: the padding */
      putstr(b, r[k].call);
      put32(b, (guint32)(r[k].freq_hz + 0.5));
      put8(b, (guint)(gint8)CLAMP(r[k].snr, -128, 127));
      put8(b, 0);                                   /* iMD: none (not PSK)     */
      putstr(b, r[k].mode);
      put8(b, 1);                                   /* automatically extracted */
      putstr(b, r[k].grid);
      put32(b, (guint32)r[k].t);
    }
    pad4(b, at);
    set16(b, at + 2, b->len - at);
  }
  set16(b, 2, b->len);
  if (used) { *used = k; }
  return g_byte_array_free_to_bytes(b);
}

/* ---- the uploader ---------------------------------------------------------------- */

#define QUEUE_MAX   20000
#define GRIDS_MAX   50000

struct _SkimPskr {
  char    *call, *locator, *software, *host;
  guint16  port;
  guint    interval_s;
  GMutex   lock;
  GCond    cond;
  gboolean stop;
  GArray  *queue;                       /* SkimPskrReport, oldest first        */
  gsize    queued_bytes;
  GHashTable *sent;                     /* "CALL|band|mode" → gint64 *: when   */
  GHashTable *grids;                    /* call → its locator (owned)          */
  GThread *thread;
  /* the thread's */
  GSocket *sock;
  guint32  seq, odid;
  guint    datagrams_tmpl;              /* datagrams with the templates so far */
  gint64   tmpl_at;                     /* when the templates went last (s)    */
  /* the status (lock) */
  guint64  nsent, ndatagrams;
  gint64   last;
  char    *error;
};

static void set_error(SkimPskr *p, char *m) {     /* takes m; NULL = fine */
  g_mutex_lock(&p->lock);
  const gboolean same = g_strcmp0(p->error, m) == 0;
  g_free(p->error);
  p->error = m;
  g_mutex_unlock(&p->lock);
  if (m && !same) { g_warning("pskreporter: %s", m); }
}

static GSocketAddress *resolve(SkimPskr *p, GError **error) {
  GResolver *res = g_resolver_get_default();
  GList *list = g_resolver_lookup_by_name(res, p->host, NULL, error);
  g_object_unref(res);
  if (!list) { return NULL; }
  GInetAddress *pick = NULL;                       /* IPv4 first, else any    */
  for (GList *l = list; l && !pick; l = l->next) {
    if (g_inet_address_get_family(l->data) == G_SOCKET_FAMILY_IPV4) { pick = l->data; }
  }
  if (!pick) { pick = list->data; }
  GSocketAddress *a = g_inet_socket_address_new(pick, p->port);
  g_resolver_free_addresses(list);
  return a;
}

/* Everything that waits, in as many datagrams as it takes (the thread). */
static void flush(SkimPskr *p) {
  g_mutex_lock(&p->lock);
  GArray *q = p->queue;
  p->queue = g_array_new(FALSE, FALSE, sizeof(SkimPskrReport));
  p->queued_bytes = 0;
  g_mutex_unlock(&p->lock);
  if (!q->len) {
    g_array_unref(q);
    return;
  }
  GError *e = NULL;
  GSocketAddress *to = resolve(p, &e);
  if (to && !p->sock) {                            /* one socket: one source port */
    p->sock = g_socket_new(g_socket_address_get_family(to), G_SOCKET_TYPE_DATAGRAM,
                           G_SOCKET_PROTOCOL_UDP, &e);
  }
  guint done = 0;
  while (to && p->sock && done < q->len) {
    const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    const gboolean tmpl = p->datagrams_tmpl < 3 || now - p->tmpl_at >= 3600;
    guint used = 0;
    GBytes *d = skim_pskr_encode(p->call, p->locator, p->software, p->seq, p->odid, (guint32)now,
                                 tmpl, &g_array_index(q, SkimPskrReport, done), q->len - done,
                                 SKIM_PSKR_MAX_BYTES, &used);
    gsize len = 0;
    const char *data = g_bytes_get_data(d, &len);
    const gssize w = g_socket_send_to(p->sock, to, data, len, NULL, &e);
    g_bytes_unref(d);
    if (w < 0) { break; }
    if (tmpl) {
      p->datagrams_tmpl++;
      p->tmpl_at = now;
    }
    p->seq += used;
    done += used;
    g_mutex_lock(&p->lock);
    p->nsent += used;
    p->ndatagrams++;
    p->last = now;
    g_mutex_unlock(&p->lock);
  }
  if (done < q->len) {                              /* back to the head, for the next */
    g_mutex_lock(&p->lock);
    const gint64 old = g_get_real_time() / G_USEC_PER_SEC - SKIM_PSKR_REPEAT_S;
    GArray *rest = g_array_new(FALSE, FALSE, sizeof(SkimPskrReport));
    for (guint i = done; i < q->len; i++) {
      const SkimPskrReport *r = &g_array_index(q, SkimPskrReport, i);
      if (r->t >= old) { g_array_append_val(rest, *r); }   /* an hour old: gone */
    }
    for (guint i = 0; i < p->queue->len && rest->len < QUEUE_MAX; i++) {
      g_array_append_val(rest, g_array_index(p->queue, SkimPskrReport, i));
    }
    g_array_unref(p->queue);
    p->queue = rest;
    p->queued_bytes = 0;                            /* counted anew: the old and the new */
    for (guint i = 0; i < rest->len; i++) { p->queued_bytes += report_len(&g_array_index(rest, SkimPskrReport, i)); }
    g_mutex_unlock(&p->lock);
    set_error(p, g_strdup_printf("%s:%u: %s", p->host, p->port, e ? e->message : "not sent"));
  } else {
    set_error(p, NULL);
  }
  g_clear_error(&e);
  g_clear_object(&to);
  g_array_unref(q);
}

static gpointer uploader(gpointer data) {
  SkimPskr *p = data;
  /* the first datagram an interval after the start, a few s of random on
   * top: never on the clock's minute, never in step with other stations */
  gint64 next = g_get_monotonic_time() + (gint64)p->interval_s * G_USEC_PER_SEC +
                g_random_int_range(0, (gint32)MAX(1u, p->interval_s / 20)) * G_USEC_PER_SEC;
  g_mutex_lock(&p->lock);
  while (!p->stop) {
    const gboolean full = p->queued_bytes + 120 >= SKIM_PSKR_MAX_BYTES;
    if (!full && g_get_monotonic_time() < next) {
      g_cond_wait_until(&p->cond, &p->lock, next);
      continue;
    }
    g_mutex_unlock(&p->lock);
    flush(p);
    next = g_get_monotonic_time() + (gint64)p->interval_s * G_USEC_PER_SEC;
    g_mutex_lock(&p->lock);
  }
  g_mutex_unlock(&p->lock);
  flush(p);                                         /* what waits, once         */
  return NULL;
}

SkimPskr *skim_pskr_new(const SkimPskrConfig *cfg) {
  SkimPskr *p = g_new0(SkimPskr, 1);
  g_mutex_init(&p->lock);
  g_cond_init(&p->cond);
  p->call = g_strdup(cfg->call ? cfg->call : "");
  p->locator = g_strdup(cfg->locator ? cfg->locator : "");
  p->software = g_strdup(cfg->software ? cfg->software : "skimmer-for-linux");
  p->host = g_strdup(cfg->host && *cfg->host ? cfg->host : SKIM_PSKR_HOST);
  p->port = cfg->port ? cfg->port : SKIM_PSKR_PORT;
  p->interval_s = cfg->interval_s ? cfg->interval_s : SKIM_PSKR_INTERVAL_S;
  p->queue = g_array_new(FALSE, FALSE, sizeof(SkimPskrReport));
  p->sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  p->grids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  p->odid = g_random_int();
  p->thread = g_thread_new("pskreporter", uploader, p);
  return p;
}

void skim_pskr_free(SkimPskr *p) {
  if (!p) { return; }
  g_mutex_lock(&p->lock);
  p->stop = TRUE;
  g_cond_signal(&p->cond);
  g_mutex_unlock(&p->lock);
  g_thread_join(p->thread);
  g_clear_object(&p->sock);
  g_array_unref(p->queue);
  g_hash_table_unref(p->sent);
  g_hash_table_unref(p->grids);
  g_free(p->call); g_free(p->locator); g_free(p->software); g_free(p->host); g_free(p->error);
  g_cond_clear(&p->cond);
  g_mutex_clear(&p->lock);
  g_free(p);
}

void skim_pskr_add(SkimPskr *p, const char *call, const char *grid, double freq_hz, int snr,
                   const char *mode, const char *band, gint64 t) {
  if (!p || !call || !*call) { return; }
  g_mutex_lock(&p->lock);
  if (grid && *grid) {
    if (g_hash_table_size(p->grids) > GRIDS_MAX) { g_hash_table_remove_all(p->grids); }
    g_hash_table_replace(p->grids, g_strdup(call), g_strdup(grid));
  }
  char *key = g_strdup_printf("%s|%s|%s", call, band ? band : "", mode ? mode : "");
  gint64 *at = g_hash_table_lookup(p->sent, key);
  if (at && t - *at < SKIM_PSKR_REPEAT_S && t >= *at) {   /* within the hour */
    g_free(key);
    g_mutex_unlock(&p->lock);
    return;
  }
  if (g_hash_table_size(p->sent) > GRIDS_MAX) { g_hash_table_remove_all(p->sent); }
  if (!at) {
    at = g_new(gint64, 1);
    g_hash_table_insert(p->sent, key, at);
  } else {
    g_free(key);
  }
  *at = t;
  if (p->queue->len < QUEUE_MAX) {
    SkimPskrReport r = { .freq_hz = freq_hz, .snr = snr, .t = t };
    g_strlcpy(r.call, call, sizeof r.call);
    g_strlcpy(r.mode, mode ? mode : "", sizeof r.mode);
    const char *g = grid && *grid ? grid : g_hash_table_lookup(p->grids, call);
    g_strlcpy(r.grid, g ? g : "", sizeof r.grid);
    g_array_append_val(p->queue, r);
    p->queued_bytes += report_len(&r);
    if (p->queued_bytes + 120 >= SKIM_PSKR_MAX_BYTES) { g_cond_signal(&p->cond); }   /* full: now */
  }
  g_mutex_unlock(&p->lock);
}

char *skim_pskr_status_json(SkimPskr *p) {
  GString *s = g_string_new("{\"on\":true,\"call\":\"");
  skim_json_append(s, p->call);
  char sq[5];
  g_strlcpy(sq, p->locator, sizeof sq);             /* its square only: the page is public */
  g_string_append(s, "\",\"locator\":\"");
  skim_json_append(s, sq);
  g_mutex_lock(&p->lock);
  g_string_append_printf(s, "\",\"sent\":%" G_GUINT64_FORMAT ",\"datagrams\":%" G_GUINT64_FORMAT
                         ",\"queued\":%u,\"last\":%" G_GINT64_FORMAT ",\"error\":",
                         p->nsent, p->ndatagrams, p->queue->len, p->last);
  if (p->error) {
    g_string_append_c(s, '"');
    skim_json_append(s, p->error);
    g_string_append_c(s, '"');
  } else {
    g_string_append(s, "null");
  }
  g_mutex_unlock(&p->lock);
  g_string_append_c(s, '}');
  return g_string_free(s, FALSE);
}
