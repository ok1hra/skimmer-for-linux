/* rp_stream.c — the SKRS client (rp_stream.h; the wire: rp/agent/skimmer-rp.c,
 * "SKRS"). Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "rp_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BLOCK      2048            /* frames a callback, as the P1 client's  */
#define HDR        22
#define MAX_GAP_S  1.0             /* a longer gap starts a new block run    */

typedef struct {
  double   rate, centre;
  guint64  next;                   /* the index the block in hand goes on at */
  guint32  epoch;
  gboolean have;
  float    block[2 * BLOCK];
  guint    nb;
  guint64  block_idx;              /* the index of block[0]                  */
  guint64  datagrams, lost;
} Stream;

struct _SkimRpStream {
  char               host[64];
  guint16            port;
  char               device[96];
  int                fd;
  struct sockaddr_in to;
  GThread           *thread;
  volatile gint      run;
  guint              n;
  Stream             st[SKIM_RP_STREAM_MAX];
  volatile gint      acked;        /* -1 none yet, 0 refused, 1 ok           */
  volatile guint     clips;
  SkimRpStreamCb     cb;
  gpointer           cb_user;
  SkimRpStreamClosedCb closed_cb;
  gpointer           closed_user;
  guint8             start_msg[7 + 7 * SKIM_RP_STREAM_MAX];
  gsize              start_len;
};

SkimRpStream *skim_rp_stream_new(const char *host, guint16 port) {
  SkimRpStream *s = g_new0(SkimRpStream, 1);
  g_strlcpy(s->host, host, sizeof(s->host));
  s->port = port;
  s->fd = -1;
  g_snprintf(s->device, sizeof(s->device), "Red Pitaya %s (SKRS)", host);
  return s;
}

void skim_rp_stream_set_cb(SkimRpStream *s, SkimRpStreamCb cb, gpointer user) {
  s->cb = cb;
  s->cb_user = user;
}

void skim_rp_stream_set_closed_cb(SkimRpStream *s, SkimRpStreamClosedCb cb, gpointer user) {
  s->closed_cb = cb;
  s->closed_user = user;
}

static void flush_block(SkimRpStream *s, guint id) {
  Stream *x = &s->st[id];
  if (x->nb && s->cb) {
    s->cb(id, x->block, x->nb, x->rate, x->centre, x->epoch, x->block_idx, s->cb_user);
  }
  x->nb = 0;
}

/* frames (n of them, or zeros when src NULL) at index idx into the block */
static void push(SkimRpStream *s, guint id, const guint8 *src, guint n, guint64 idx) {
  Stream *x = &s->st[id];
  for (guint k = 0; k < n; k++) {
    if (!x->nb) { x->block_idx = idx + k; }
    float i = 0, q = 0;
    if (src) {
      const guint8 *b = src + 6 * k;
      gint32 vi = (b[0] << 16) | (b[1] << 8) | b[2];
      gint32 vq = (b[3] << 16) | (b[4] << 8) | b[5];
      if (vi & 0x800000) { vi -= 0x1000000; }
      if (vq & 0x800000) { vq -= 0x1000000; }
      i = (float)vi / 8388608.0f;
      q = -(float)vq / 8388608.0f;              /* the wire is RF-inverted */
    }
    x->block[2 * x->nb] = i;
    x->block[2 * x->nb + 1] = q;
    if (++x->nb == BLOCK) { flush_block(s, id); }
  }
}

static void datagram(SkimRpStream *s, const guint8 *d, gssize len) {
  if (len >= 7 && memcmp(d, "SKRC", 4) == 0 && d[4] == 1 && d[5] == 0x81) {
    g_atomic_int_set(&s->acked, d[6] ? 1 : 0);
    return;
  }
  if (len < HDR || memcmp(d, "SKRS", 4) != 0 || d[4] != 1) { return; }
  const guint id = d[5];
  if (id >= s->n) { return; }
  const guint count = (guint)(d[20] << 8 | d[21]);
  if ((gsize)len < HDR + 6u * count) { return; }
  Stream *x = &s->st[id];
  s->clips |= (d[7] >> 1) & 3;
  const guint32 ep = (guint32)d[8] << 24 | (guint32)d[9] << 16 | (guint32)d[10] << 8 | d[11];
  guint64 idx = 0;
  for (int b = 0; b < 8; b++) { idx = idx << 8 | d[12 + b]; }
  x->datagrams++;
  if (x->have && ep == x->epoch && idx < x->next) { return; }          /* old, or a duplicate */
  if (x->have && ep == x->epoch && idx > x->next) {
    const guint64 gap = idx - x->next;
    if ((double)gap <= MAX_GAP_S * x->rate) {
      push(s, id, NULL, (guint)gap, x->next);                          /* lost: zeros */
      x->lost += gap;
    } else {
      flush_block(s, id);                                              /* a new run   */
      x->lost += gap;
    }
  } else if (x->have && ep != x->epoch) {
    flush_block(s, id);                                                /* a new epoch */
  }
  x->epoch = ep;
  x->have = TRUE;
  push(s, id, d + HDR, count, idx);
  x->next = idx + count;
}

static gpointer rx_thread(gpointer data) {
  SkimRpStream *s = data;
  gint64 last_ka = 0, last_rx = g_get_monotonic_time();
  gboolean closed = FALSE;
  guint8 buf[2048];
  while (g_atomic_int_get(&s->run)) {
    const gint64 now = g_get_monotonic_time();
    if (now - last_ka >= G_USEC_PER_SEC) {                 /* keep-alive, every second */
      const guint8 ka[6] = { 'S', 'K', 'R', 'C', 1, 3 };
      sendto(s->fd, ka, sizeof(ka), 0, (struct sockaddr *)&s->to, sizeof(s->to));
      last_ka = now;
    }
    struct pollfd p = { s->fd, POLLIN, 0 };
    if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
      for (;;) {
        const gssize n = recv(s->fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) { break; }
        datagram(s, buf, n);
        last_rx = g_get_monotonic_time();
      }
    }
    if (!closed && g_atomic_int_get(&s->acked) == 1 &&
        g_get_monotonic_time() - last_rx > 2 * G_USEC_PER_SEC) {
      closed = TRUE;
      if (s->closed_cb) { s->closed_cb(s->closed_user); }
    }
  }
  for (guint id = 0; id < s->n; id++) { flush_block(s, id); }
  return NULL;
}

gboolean skim_rp_stream_start(SkimRpStream *s, const SkimRpStreamSpec *specs, guint n,
                              double clock_ppm, GError **error) {
  skim_rp_stream_stop(s);
  if (n < 1 || n > SKIM_RP_STREAM_MAX) {
    g_set_error(error, SKIM_RP_STREAM_ERROR, SKIM_RP_STREAM_ERROR_FAILED, "%u streams (1–%d)", n,
                SKIM_RP_STREAM_MAX);
    return FALSE;
  }
  struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
  char ps[8];
  g_snprintf(ps, sizeof(ps), "%u", s->port);
  if (getaddrinfo(s->host, ps, &hints, &ai) != 0 || !ai) {
    g_set_error(error, SKIM_RP_STREAM_ERROR, SKIM_RP_STREAM_ERROR_FAILED, "cannot resolve %s", s->host);
    return FALSE;
  }
  memcpy(&s->to, ai->ai_addr, sizeof(s->to));
  freeaddrinfo(ai);
  s->fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (s->fd < 0) {
    g_set_error(error, SKIM_RP_STREAM_ERROR, SKIM_RP_STREAM_ERROR_FAILED, "socket: %s", g_strerror(errno));
    return FALSE;
  }
  int big = 8 << 20;
  setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  s->n = n;
  guint8 *m = s->start_msg;
  memcpy(m, "SKRC", 4);
  m[4] = 1;
  m[5] = 1;
  m[6] = (guint8)n;
  for (guint i = 0; i < n; i++) {
    Stream *x = &s->st[i];
    memset(x, 0, sizeof(*x));
    x->rate = specs[i].rate;
    x->centre = specs[i].centre_hz;
    const double word = specs[i].centre_hz / (1.0 + clock_ppm * 1e-6);
    const guint32 hz = (guint32)llround(word);
    guint8 *e = m + 7 + 7 * i;
    e[0] = (guint8)i;
    e[1] = (guint8)(hz >> 24); e[2] = (guint8)(hz >> 16); e[3] = (guint8)(hz >> 8); e[4] = (guint8)hz;
    e[5] = specs[i].rate >= 384000 ? 3 : specs[i].rate >= 192000 ? 2 : specs[i].rate >= 96000 ? 1 : 0;
    e[6] = specs[i].input == 2 ? 2 : 1;
  }
  s->start_len = 7 + 7 * n;
  g_atomic_int_set(&s->acked, -1);
  s->clips = 0;
  sendto(s->fd, s->start_msg, s->start_len, 0, (struct sockaddr *)&s->to, sizeof(s->to));
  g_atomic_int_set(&s->run, 1);
  s->thread = g_thread_new("rp-stream", rx_thread, s);
  /* the ack, then every stream's first datagram */
  const gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  gint64 resend = g_get_monotonic_time() + 300000;
  while (g_get_monotonic_time() < end) {
    const gint a = g_atomic_int_get(&s->acked);
    if (a == 0) {
      skim_rp_stream_stop(s);
      g_set_error(error, SKIM_RP_STREAM_ERROR, SKIM_RP_STREAM_ERROR_BUSY,
                  "%s refused %u streams (not enough free slots)", s->host, n);
      return FALSE;
    }
    if (a < 0 && g_get_monotonic_time() > resend) {     /* a lost start: once more */
      sendto(s->fd, s->start_msg, s->start_len, 0, (struct sockaddr *)&s->to, sizeof(s->to));
      resend = g_get_monotonic_time() + 300000;
    }
    if (a == 1) {
      guint got = 0;
      for (guint i = 0; i < n; i++) { got += s->st[i].datagrams > 0; }
      if (got == n) { return TRUE; }
    }
    g_usleep(10000);
  }
  const gboolean acked = g_atomic_int_get(&s->acked) == 1;
  skim_rp_stream_stop(s);
  g_set_error(error, SKIM_RP_STREAM_ERROR,
              acked ? SKIM_RP_STREAM_ERROR_NO_IQ : SKIM_RP_STREAM_ERROR_NO_REPLY,
              acked ? "%s: SKRS started, but not every stream came" : "%s: no SKRS answer", s->host);
  return FALSE;
}

void skim_rp_stream_stop(SkimRpStream *s) {
  if (!s->thread) {
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    return;
  }
  const guint8 stop[6] = { 'S', 'K', 'R', 'C', 1, 2 };
  sendto(s->fd, stop, sizeof(stop), 0, (struct sockaddr *)&s->to, sizeof(s->to));
  g_atomic_int_set(&s->run, 0);
  g_thread_join(s->thread);
  s->thread = NULL;
  close(s->fd);
  s->fd = -1;
}

void skim_rp_stream_free(SkimRpStream *s) {
  if (!s) { return; }
  skim_rp_stream_stop(s);
  g_free(s);
}

guint64 skim_rp_stream_datagrams(SkimRpStream *s, guint id) { return id < s->n ? s->st[id].datagrams : 0; }
guint64 skim_rp_stream_lost_frames(SkimRpStream *s, guint id) { return id < s->n ? s->st[id].lost : 0; }
guint skim_rp_stream_clips(SkimRpStream *s) { return s->clips; }
const char *skim_rp_stream_device(SkimRpStream *s) { return s->device; }
