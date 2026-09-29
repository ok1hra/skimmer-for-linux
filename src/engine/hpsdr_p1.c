/* hpsdr_p1.c — HPSDR Protocol 1 receive-only IQ client (Red Pitaya).
 *
 * Wire (openHPSDR Metis/USB-over-IP, as Pavel Demin's sdr-receiver-hpsdr.c
 * implements it — docs/HPSDR-P1.md):
 *   discovery  EF FE 02 + 60×0        → EF FE 02|03 mac[6] code board name…
 *   start/stop EF FE 04 01 / EF FE 04 00 (64 bytes) — the server streams EP6
 *              to the address AND PORT the start came from, so one socket
 *              sends everything and receives the stream.
 *   EP2 (to)   EF FE 01 02 seq[4] + 2 × (7F 7F 7F C0 C1 C2 C3 C4 + 504 B)
 *              C0=0x00: C1[1:0] rate 0/1/2 = 48/96/192 kHz, C4[5:3] nrx−1
 *              C0=0x04: RX1 frequency, Hz, big-endian (the server turns it
 *              into a phase increment against its nominal 125 MHz)
 *   EP6 (from) EF FE 01 06 seq[4] + 2 × (7F 7F 7F C0..C4 + 504 B); per
 *              sample and receiver I[3] Q[3] (24-bit BE, signed), then a
 *              16-bit mic word: 504 / (6·nrx + 2) samples per frame.
 *
 * The server has no watchdog and takes EP2 and stop from ANY address. So
 * the control refresh (1 Hz, against UDP loss) and the final stop go out
 * only while the stream is still ours; after a take-over by another client
 * we go quiet.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "hpsdr_p1.h"

#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define NRX              1        /* receivers requested (RX1 is ours)        */
#define PKT_BYTES        1032
#define FRAME_BYTES      512
#define FRAME_PAYLOAD    504
#define BLOCK_FRAMES     2048     /* per IQ callback — the TCI block size     */
#define POLL_MS          200
#define REFRESH_US       (1 * G_USEC_PER_SEC)
#define WATCHDOG_US      (2 * G_USEC_PER_SEC)
#define FIRST_IQ_US      (2 * G_USEC_PER_SEC)
#define LOST_LOG_US      (10 * G_USEC_PER_SEC)
#define MAX_FILL_PKTS    2000     /* a larger seq jump resyncs without fill   */
#define SCALE_24         (1.0f / 8388608.0f)

/* The EP6 IQ is RF-INVERTED (like every raw HPSDR DDC feed): taken as
 * I + jQ, a station above the centre lands below it. Ingest conjugates, so
 * the callback sees the TRUE spectrum the rest of the engine expects
 * (TCI's wire convention). Live-measured on the Red Pitaya 2026-09-29: with
 * I + jQ, a 5 kHz centre step moved every carrier by 10 kHz
 * (docs/HPSDR-P1.md). */
#define IQ_CONJ          1

struct _SkimHpsdrClient {
  char    *host;
  guint16  port;
  char     device[96];

  SkimHpsdrIqCb     iq_cb;
  gpointer          iq_cb_data;
  SkimHpsdrClosedCb closed_cb;
  gpointer          closed_cb_data;

  int                fd;
  struct sockaddr_in dst;
  GThread           *thread;
  volatile gint      run;

  /* Session parameters (fixed while running). */
  guint    rate;
  guint8   rate_code;
  double   center_hz;
  guint32  freq_word;

  /* Receive thread only. */
  guint32  ep2_seq;
  guint32  next_seq;
  gboolean have_seq;
  float    blk[BLOCK_FRAMES * 2];
  guint    fill;
  gint64   last_refresh_us;
  gint64   last_lost_log_us;
  guint64  lost_since_log;

  GMutex   lock;                 /* guards everything below                  */
  GCond    cond;
  gboolean got_iq;               /* first EP6 packet arrived                 */
  gboolean ours;                 /* the stream is (still) ours               */
  gint64   last_rx_us;
  guint64  packets;
  guint64  lost_frames;
};

/* ---- sockets ---------------------------------------------------------------- */

static gboolean resolve(const char *host, guint16 port, struct sockaddr_in *out,
                        GError **error) {
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons(port);
  if (inet_pton(AF_INET, host, &out->sin_addr) == 1) { return TRUE; }
  struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
  struct addrinfo *res = NULL;
  if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "cannot resolve %s", host);
    return FALSE;
  }
  out->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return TRUE;
}

static int open_socket(GError **error) {
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "socket: %s", g_strerror(errno));
    return -1;
  }
  /* ~1500 packets/s at 192 kHz: a deep buffer rides out scheduling hiccups. */
  int rcvbuf = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  return fd;
}

static void send_cmd(int fd, const struct sockaddr_in *dst, guint8 b2, guint8 b3) {
  guint8 pkt[64] = { 0xEF, 0xFE, b2, b3 };
  sendto(fd, pkt, sizeof(pkt), 0, (const struct sockaddr *)dst, sizeof(*dst));
}

/* ---- discovery -------------------------------------------------------------- */

static gboolean parse_discovery(const guint8 *d, gssize n, SkimHpsdrInfo *out) {
  if (n < 20 || d[0] != 0xEF || d[1] != 0xFE || (d[2] != 2 && d[2] != 3)) {
    return FALSE;
  }
  memset(out, 0, sizeof(*out));
  out->busy = d[2] == 3;
  memcpy(out->mac, d + 3, 6);
  out->code_version = d[9];
  out->board_id = d[10];
  for (int i = 0; i < 8; i++) {
    const guint8 ch = d[11 + i];
    out->name[i] = g_ascii_isprint(ch) ? (char)ch : '\0';
  }
  out->name[8] = '\0';
  out->nrx = d[0x13];
  return TRUE;
}

/* Discovery on an open socket: send, wait for the reply from dst. */
static gboolean discover_fd(int fd, const struct sockaddr_in *dst,
                            guint timeout_ms, SkimHpsdrInfo *out) {
  guint8 req[63] = { 0xEF, 0xFE, 0x02 };
  sendto(fd, req, sizeof(req), 0, (const struct sockaddr *)dst, sizeof(*dst));
  const gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  for (;;) {
    const gint64 left = end - g_get_monotonic_time();
    if (left <= 0) { return FALSE; }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, (int)((left + 999) / 1000)) <= 0) { continue; }
    guint8 buf[PKT_BYTES];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    const gssize n = recvfrom(fd, buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &flen);
    if (n > 0 && from.sin_addr.s_addr == dst->sin_addr.s_addr &&
        parse_discovery(buf, n, out)) {
      return TRUE;
    }
  }
}

gboolean skim_hpsdr_discover(const char *host, guint16 port, guint timeout_ms,
                             SkimHpsdrInfo *out, GError **error) {
  struct sockaddr_in dst;
  if (!resolve(host, port, &dst, error)) { return FALSE; }
  int fd = open_socket(error);
  if (fd < 0) { return FALSE; }
  const gboolean ok = discover_fd(fd, &dst, timeout_ms, out);
  close(fd);
  if (!ok) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_NO_REPLY,
                "no HPSDR discovery reply from %s:%u", host, port);
  }
  return ok;
}

/* ---- EP2 control ------------------------------------------------------------ */

static void send_ep2(SkimHpsdrClient *c) {
  guint8 pkt[PKT_BYTES] = { 0xEF, 0xFE, 0x01, 0x02 };
  const guint32 seq = htonl(c->ep2_seq++);
  memcpy(pkt + 4, &seq, 4);
  for (int f = 0; f < 2; f++) {
    guint8 *fr = pkt + 8 + f * FRAME_BYTES;
    fr[0] = fr[1] = fr[2] = 0x7F;
    if (f == 0) {                            /* C0=0: rate + receiver count   */
      fr[3] = 0x00;
      fr[4] = c->rate_code;
      fr[7] = (guint8)(((NRX - 1) & 7) << 3);
    } else {                                 /* C0=0x04: RX1 frequency, BE    */
      fr[3] = 0x04;
      const guint32 hz = htonl(c->freq_word);
      memcpy(fr + 4, &hz, 4);
    }
  }
  sendto(c->fd, pkt, sizeof(pkt), 0, (const struct sockaddr *)&c->dst,
         sizeof(c->dst));
}

/* ---- EP6 ingest ------------------------------------------------------------- */

static void push_frame(SkimHpsdrClient *c, float re, float im) {
  c->blk[2 * c->fill]     = re;
  c->blk[2 * c->fill + 1] = im;
  if (++c->fill == BLOCK_FRAMES) {
    c->fill = 0;
    if (c->iq_cb) {
      c->iq_cb(c->blk, BLOCK_FRAMES, c->rate, c->center_hz, c->iq_cb_data);
    }
  }
}

static inline gint32 be24(const guint8 *p) {
  gint32 v = (gint32)((guint32)p[0] << 16 | (guint32)p[1] << 8 | p[2]);
  return (v ^ 0x800000) - 0x800000;          /* sign-extend 24 → 32 bits      */
}

static void handle_ep6(SkimHpsdrClient *c, const guint8 *pkt) {
  guint32 seq;
  memcpy(&seq, pkt + 4, 4);
  seq = ntohl(seq);
  const guint slot = 6 * NRX + 2;
  const guint per_frame = FRAME_PAYLOAD / slot;
  guint64 filled = 0;

  if (c->have_seq && seq != c->next_seq) {
    const guint32 gap = seq - c->next_seq;   /* mod 2^32                       */
    if (gap > 0x80000000u) {
      return;                                /* late/duplicate packet: drop   */
    }
    if (gap <= MAX_FILL_PKTS) {              /* keep stream time continuous    */
      filled = (guint64)gap * 2 * per_frame;
      for (guint64 i = 0; i < filled; i++) { push_frame(c, 0.0f, 0.0f); }
    }
  }
  c->have_seq = TRUE;
  c->next_seq = seq + 1;

  for (int f = 0; f < 2; f++) {
    const guint8 *fr = pkt + 8 + f * FRAME_BYTES;
    if (fr[0] != 0x7F || fr[1] != 0x7F || fr[2] != 0x7F) { continue; }
    const guint8 *s = fr + 8;
    for (guint k = 0; k < per_frame; k++, s += slot) {
      const float i = (float)be24(s) * SCALE_24;       /* RX1 = first slot    */
      const float q = (float)be24(s + 3) * SCALE_24;
      push_frame(c, i, IQ_CONJ ? -q : q);
    }
  }

  g_mutex_lock(&c->lock);
  c->packets++;
  c->lost_frames += filled;
  c->last_rx_us = g_get_monotonic_time();
  if (!c->got_iq) {
    c->got_iq = TRUE;
    g_cond_broadcast(&c->cond);
  }
  g_mutex_unlock(&c->lock);

  if (filled) {
    c->lost_since_log += filled;
    const gint64 now = g_get_monotonic_time();
    if (now - c->last_lost_log_us > LOST_LOG_US) {
      g_message("hpsdr: %s — %" G_GUINT64_FORMAT " frames lost (UDP), "
                "zero-filled", c->device, c->lost_since_log);
      c->lost_since_log = 0;
      c->last_lost_log_us = now;
    }
  }
}

static gpointer rx_thread(gpointer data) {
  SkimHpsdrClient *c = data;
  guint8 buf[PKT_BYTES + 16];
  while (g_atomic_int_get(&c->run)) {
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    if (poll(&pfd, 1, POLL_MS) > 0) {
      struct sockaddr_in from;
      socklen_t flen = sizeof(from);
      const gssize n = recvfrom(c->fd, buf, sizeof(buf), 0,
                                (struct sockaddr *)&from, &flen);
      if (n == PKT_BYTES && from.sin_addr.s_addr == c->dst.sin_addr.s_addr &&
          buf[0] == 0xEF && buf[1] == 0xFE && buf[2] == 0x01 && buf[3] == 0x06) {
        handle_ep6(c, buf);
      }
    }

    const gint64 now = g_get_monotonic_time();
    g_mutex_lock(&c->lock);
    gboolean fire = FALSE;
    if (c->ours && c->got_iq && now - c->last_rx_us > WATCHDOG_US) {
      c->ours = FALSE;                       /* taken over or gone: go quiet  */
      fire = TRUE;
    }
    const gboolean ours = c->ours;
    g_mutex_unlock(&c->lock);

    if (fire) {
      g_message("hpsdr: %s — stream stopped (another client, or the radio "
                "left the network)", c->device);
      if (c->closed_cb) { c->closed_cb(c->closed_cb_data); }
    } else if (ours && now - c->last_refresh_us > REFRESH_US) {
      send_ep2(c);
      c->last_refresh_us = now;
    }
  }
  return NULL;
}

/* ---- public API ------------------------------------------------------------- */

SkimHpsdrClient *skim_hpsdr_client_new(const char *host, guint16 port) {
  SkimHpsdrClient *c = g_new0(SkimHpsdrClient, 1);
  c->host = g_strdup(host && *host ? host : "192.168.1.21");
  c->port = port ? port : SKIM_HPSDR_DEFAULT_PORT;
  c->fd = -1;
  g_snprintf(c->device, sizeof(c->device), "HPSDR %s", c->host);
  g_mutex_init(&c->lock);
  g_cond_init(&c->cond);
  return c;
}

void skim_hpsdr_client_free(SkimHpsdrClient *c) {
  if (!c) { return; }
  skim_hpsdr_client_stop(c);
  g_mutex_clear(&c->lock);
  g_cond_clear(&c->cond);
  g_free(c->host);
  g_free(c);
}

void skim_hpsdr_client_set_iq_cb(SkimHpsdrClient *c, SkimHpsdrIqCb cb, gpointer user_data) {
  c->iq_cb = cb;
  c->iq_cb_data = user_data;
}

void skim_hpsdr_client_set_closed_cb(SkimHpsdrClient *c, SkimHpsdrClosedCb cb,
                                     gpointer user_data) {
  c->closed_cb = cb;
  c->closed_cb_data = user_data;
}

gboolean skim_hpsdr_client_start(SkimHpsdrClient *c, guint rate, double center_hz,
                                 double clock_ppm, gboolean take_over,
                                 GError **error) {
  g_return_val_if_fail(c->fd < 0, FALSE);
  if (rate == 0) { rate = 192000; }
  switch (rate) {
    case 48000:  c->rate_code = 0; break;
    case 96000:  c->rate_code = 1; break;
    case 192000: c->rate_code = 2; break;
    default:
      g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                  "HPSDR P1 sample rate %u not supported (48/96/192 kHz)", rate);
      return FALSE;
  }
  const double word = center_hz / (1.0 + clock_ppm * 1e-6);
  if (!(word > 0 && word < 4294967295.0)) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "bad centre frequency %.0f Hz", center_hz);
    return FALSE;
  }
  c->rate = rate;
  c->center_hz = center_hz;
  c->freq_word = (guint32)llround(word);

  if (!resolve(c->host, c->port, &c->dst, error)) { return FALSE; }
  c->fd = open_socket(error);
  if (c->fd < 0) { return FALSE; }

  SkimHpsdrInfo info;
  if (!discover_fd(c->fd, &c->dst, 1000, &info)) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_NO_REPLY,
                "no HPSDR discovery reply from %s:%u", c->host, c->port);
    goto fail;
  }
  const gboolean rp = strcmp(info.name, "R_PITAYA") == 0;
  g_snprintf(c->device, sizeof(c->device), "%s %s (HPSDR P1)",
             rp ? "Red Pitaya" : "HPSDR", c->host);
  if (info.busy && !take_over) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_BUSY,
                "%s is in use by another client", c->device);
    goto fail;
  }

  c->ep2_seq = 0;
  c->have_seq = FALSE;
  c->fill = 0;
  c->lost_since_log = 0;
  c->last_lost_log_us = 0;
  g_mutex_lock(&c->lock);
  c->got_iq = FALSE;
  c->ours = TRUE;
  c->packets = 0;
  c->lost_frames = 0;
  g_mutex_unlock(&c->lock);

  /* Configure first, so the first EP6 packet already has our layout. */
  send_ep2(c);
  send_ep2(c);
  send_cmd(c->fd, &c->dst, 0x04, 0x01);
  c->last_refresh_us = g_get_monotonic_time();

  g_atomic_int_set(&c->run, 1);
  c->thread = g_thread_new("skim-hpsdr", rx_thread, c);

  const gint64 end = g_get_monotonic_time() + FIRST_IQ_US;
  g_mutex_lock(&c->lock);
  while (!c->got_iq) {
    if (!g_cond_wait_until(&c->cond, &c->lock, end)) { break; }
  }
  const gboolean got = c->got_iq;
  g_mutex_unlock(&c->lock);
  if (!got) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_NO_IQ,
                "%s answered but sent no IQ", c->device);
    skim_hpsdr_client_stop(c);
    return FALSE;
  }
  g_message("hpsdr: %s — code %u, board %u, %u RX; streaming %u Hz @ "
            "%.0f Hz (word %u, %+.2f ppm)", c->device, info.code_version,
            info.board_id, info.nrx, rate, center_hz, c->freq_word, clock_ppm);
  return TRUE;

fail:
  close(c->fd);
  c->fd = -1;
  return FALSE;
}

void skim_hpsdr_client_stop(SkimHpsdrClient *c) {
  if (c->fd < 0) { return; }
  g_atomic_int_set(&c->run, 0);
  if (c->thread) {
    g_thread_join(c->thread);
    c->thread = NULL;
  }
  g_mutex_lock(&c->lock);
  const gboolean ours = c->ours;
  c->ours = FALSE;
  g_mutex_unlock(&c->lock);
  if (ours) { send_cmd(c->fd, &c->dst, 0x04, 0x00); }
  close(c->fd);
  c->fd = -1;
}

const char *skim_hpsdr_client_device(SkimHpsdrClient *c) { return c->device; }
double      skim_hpsdr_client_center_hz(SkimHpsdrClient *c) { return c->center_hz; }
guint       skim_hpsdr_client_rate(SkimHpsdrClient *c) { return c->rate; }
guint32     skim_hpsdr_client_freq_word(SkimHpsdrClient *c) { return c->freq_word; }

guint64 skim_hpsdr_client_packets(SkimHpsdrClient *c) {
  g_mutex_lock(&c->lock);
  const guint64 v = c->packets;
  g_mutex_unlock(&c->lock);
  return v;
}

guint64 skim_hpsdr_client_lost_frames(SkimHpsdrClient *c) {
  g_mutex_lock(&c->lock);
  const guint64 v = c->lost_frames;
  g_mutex_unlock(&c->lock);
  return v;
}
