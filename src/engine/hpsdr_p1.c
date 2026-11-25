/* hpsdr_p1.c — HPSDR Protocol 1 receive-only IQ client (Red Pitaya).
 *
 * Wire (openHPSDR Metis/USB-over-IP, as Pavel Demin's sdr-receiver-hpsdr.c
 * implements it — docs/HPSDR-P1.md):
 *   discovery  EF FE 02 + 60×0        → EF FE 02|03 mac[6] code board name…
 *   start/stop EF FE 04 01 / EF FE 04 00 (64 bytes) — the server streams EP6
 *              to the address AND PORT the start came from, so one socket
 *              sends everything and receives the stream.
 *   EP2 (to)   EF FE 01 02 seq[4] + 2 × (7F 7F 7F C0 C1 C2 C3 C4 + 504 B)
 *              C0=0x00: C1[1:0] rate 0/1/2/3 = 48/96/192/384 kHz, C4[5:3] nrx−1
 *              C0=0x04, 0x06 … 0x10, 0x24: RX1 … RX8 frequency, Hz,
 *              big-endian (the server turns it into a phase increment
 *              against its nominal ADC clock: 122.88 or 125 MHz)
 *              C0=0x1C (only to the skimmer's agent, rp/agent): the ADC of
 *              each receiver, 2 bits each — C1 RX1..RX4, C2 RX5..RX8 (RX8
 *              beyond Protocol 1's seven); the start then carries "SKRP"
 *              in bytes 4–7, and only such a client's word is taken
 *   EP6 (from) EF FE 01 06 seq[4] + 2 × (7F 7F 7F C0..C4 + 504 B); per
 *              sample and receiver I[3] Q[3] (24-bit BE, signed), then a
 *              16-bit mic word: 504 / (6·nrx + 2) samples per frame.
 *
 * The server has no watchdog and takes EP2 and stop from ANY address, and
 * its receiver count, rate and DDC frequencies are simply whatever the LAST
 * EP2 frame from anyone said. So nothing goes to the radio unless the
 * stream is flowing to us right now — judged by the kernel's arrival time
 * of the newest EP6 packet, never by when we got round to processing it
 * (the socket can hold a second of backlog):
 *   - the control words are repeated (1 Hz, against UDP loss) only in the
 *     first REFRESH_WINDOW_US after the start, never later. A client that
 *     takes the radio over — CW Skimmer Server's HermesIntf sends its
 *     configuration ONCE, at start — must not get it overwritten by our
 *     refresh while our 2 s watchdog has not yet noticed the take-over.
 *     Live 2026-11-17/18: one such refresh left SkimSrv parsing our 7-RX
 *     frames as its 8-RX layout for a day (every call on every band, WPM
 *     ×1.1, all of it sent to RBN);
 *   - the layout is checked instead: the packet rate (sequence numbers over
 *     kernel arrival time) must match our rate and receiver count. A stream
 *     that still comes to us in another layout means someone else's EP2
 *     reached our receiver: we say so and send our control words again;
 *   - the final stop goes out only when the newest packet in the socket is
 *     fresh. A stop sent after a take-over would end the new owner's
 *     stream.
 * After the watchdog fires we go quiet for good.
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
#include <sys/uio.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PKT_BYTES        1032
#define FRAME_BYTES      512
#define FRAME_PAYLOAD    504
#define BLOCK_FRAMES     2048     /* per IQ callback — the TCI block size     */
#define POLL_MS          200
#define REFRESH_US       (1 * G_USEC_PER_SEC)
#define REFRESH_WINDOW_US (3 * G_USEC_PER_SEC) /* control repeats only so long */
#define FRESH_US         (50 * 1000)  /* newest packet younger: stream is ours  */
#define LAYOUT_CHECK_US  (2 * G_USEC_PER_SEC)
#define LAYOUT_TOL       0.03         /* 7 vs 8 RX differ by 10 %, 6 vs 7 by 9 % */
#define LAYOUT_LOG_US    (60 * G_USEC_PER_SEC)
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

typedef struct {
  SkimHpsdrIqCb cb;
  gpointer      user;
  double        center_hz;
  guint32       freq_word;
  float         blk[BLOCK_FRAMES * 2];     /* receive thread only           */
  guint         fill;
} Rx;

struct _SkimHpsdrClient {
  char    *host;
  guint16  port;
  char     device[96];

  Rx                rx[SKIM_HPSDR_MAX_RX];
  SkimHpsdrClosedCb closed_cb;
  gpointer          closed_cb_data;

  int                fd;
  struct sockaddr_in dst;
  GThread           *thread;
  volatile gint      run;

  /* Session parameters (fixed while running). */
  guint    rate;
  guint8   rate_code;
  guint    nrx;
  guint8   adc_mask;             /* bit r = receiver r on IN2 (agent only)   */
  gboolean agent;                /* the board runs the skimmer's agent       */

  /* Receive thread only. */
  guint32  ep2_seq;
  guint32  next_seq;
  gboolean have_seq;
  gint64   last_refresh_us;
  gint64   started_us;           /* monotonic: the start command went out    */
  gint64   last_lost_log_us;
  guint64  lost_since_log;
  gint64   arrival_us;           /* real time: kernel arrival, newest EP6    */
  gboolean chk_have;             /* layout check: window start below is set  */
  guint32  chk_seq;
  gint64   chk_arrival_us;
  gint64   last_layout_log_us;
  guint64  layout_resends;

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
  /* Kernel arrival times: "is the stream still ours?" must not be fooled
   * by a backlog we are still working through. */
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
  return fd;
}

/* One datagram and its kernel arrival time (real time, µs; the time of the
 * call when the kernel gave none). */
static gssize recv_stamped(int fd, guint8 *buf, gsize len, int flags,
                           struct sockaddr_in *from, gint64 *arrival_us) {
  struct iovec iov = { .iov_base = buf, .iov_len = len };
  union {
    char           b[CMSG_SPACE(sizeof(struct timespec))];
    struct cmsghdr align;
  } ctl;
  struct msghdr mh = {
    .msg_name = from, .msg_namelen = sizeof(*from),
    .msg_iov = &iov, .msg_iovlen = 1,
    .msg_control = ctl.b, .msg_controllen = sizeof(ctl.b),
  };
  const gssize n = recvmsg(fd, &mh, flags);
  if (n < 0) { return n; }
  *arrival_us = 0;
  for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMPNS) {
      struct timespec ts;
      memcpy(&ts, CMSG_DATA(cm), sizeof(ts));
      *arrival_us = (gint64)ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
    }
  }
  if (*arrival_us == 0) { *arrival_us = g_get_real_time(); }
  return n;
}

static void send_cmd(int fd, const struct sockaddr_in *dst, guint8 b2, guint8 b3) {
  guint8 pkt[64] = { 0xEF, 0xFE, b2, b3 };
  sendto(fd, pkt, sizeof(pkt), 0, (const struct sockaddr *)dst, sizeof(*dst));
}

/* The start to the skimmer's agent: "SKRP" in the padding marks the client
 * whose ADC word (C0=0x1C) the agent takes. */
static void send_start_marked(int fd, const struct sockaddr_in *dst) {
  guint8 pkt[64] = { 0xEF, 0xFE, 0x04, 0x01, 'S', 'K', 'R', 'P' };
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
  /* The skimmer's own agent on the board (rp/agent) answers a request
   * whose padding says SKRP (bytes 4..7 — byte 3 is part of the command,
   * so Pavel Demin's server answers it as a plain discovery) with its marker
   * after the standard 20 bytes; anything else leaves them zero. */
  if (n >= 0x1D && memcmp(d + 0x14, "SKRP", 4) == 0) {
    out->agent = TRUE;
    out->agent_major = d[0x18];
    out->agent_minor = d[0x19];
    out->agent_mode = d[0x1A];
    out->agent_sel = d[0x1B];
    out->agent_inst = d[0x1C];
  }
  return TRUE;
}

/* Discovery on an open socket: send, wait for the reply from dst. */
static gboolean discover_fd(int fd, const struct sockaddr_in *dst,
                            guint timeout_ms, SkimHpsdrInfo *out) {
  guint8 req[63] = { 0xEF, 0xFE, 0x02, 0x00, 'S', 'K', 'R', 'P' };   /* SKRP: see parse_discovery */
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

static gint found_cmp(gconstpointer a, gconstpointer b) {
  const guint32 x = ntohl(inet_addr(((const SkimHpsdrFound *)a)->host));
  const guint32 y = ntohl(inet_addr(((const SkimHpsdrFound *)b)->host));
  return x < y ? -1 : x > y;
}

GArray *skim_hpsdr_discover_all(const char *bcast, guint16 port, guint timeout_ms,
                                GError **error) {
  struct sockaddr_in dst;
  if (!resolve(bcast ? bcast : "255.255.255.255", port, &dst, error)) { return NULL; }
  int fd = open_socket(error);
  if (fd < 0) { return NULL; }
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
  GArray *out = g_array_new(FALSE, TRUE, sizeof(SkimHpsdrFound));
  guint8 req[63] = { 0xEF, 0xFE, 0x02, 0x00, 'S', 'K', 'R', 'P' };   /* SKRP: see parse_discovery */
  if (sendto(fd, req, sizeof(req), 0, (const struct sockaddr *)&dst, sizeof(dst)) < 0) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "discovery broadcast to %s: %s", bcast ? bcast : "255.255.255.255",
                g_strerror(errno));
    close(fd);
    g_array_unref(out);
    return NULL;
  }
  const gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
  for (;;) {
    const gint64 left = end - g_get_monotonic_time();
    if (left <= 0) { break; }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, (int)((left + 999) / 1000)) <= 0) { continue; }
    guint8 buf[PKT_BYTES];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    const gssize n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
    SkimHpsdrFound f;
    memset(&f, 0, sizeof(f));
    if (n <= 0 || !parse_discovery(buf, n, &f.info)) { continue; }
    inet_ntop(AF_INET, &from.sin_addr, f.host, sizeof(f.host));
    gboolean seen = FALSE;                       /* one reply per radio       */
    for (guint i = 0; i < out->len && !seen; i++) {
      seen = strcmp(g_array_index(out, SkimHpsdrFound, i).host, f.host) == 0;
    }
    if (!seen) { g_array_append_val(out, f); }
  }
  close(fd);
  g_array_sort(out, found_cmp);
  return out;
}

/* ---- EP2 control ------------------------------------------------------------ */

/* C0 address of receiver rx's frequency (0-based): RX1..RX7 sit at
 * 0x04..0x10, RX8 at 0x24 (the Hermes map sdr-receiver-hpsdr.c follows). */
static guint8 rx_freq_c0(guint rx) { return rx < 7 ? (guint8)(0x04 + 2 * rx) : 0x24; }

/* Control word k of the session: 0 = rate + receiver count, 1..nrx = the
 * receivers' frequencies, nrx + 1 = the receivers' ADCs (the agent only). */
static void put_ctrl(const SkimHpsdrClient *c, guint k, guint8 *fr) {
  fr[0] = fr[1] = fr[2] = 0x7F;
  if (k == 0) {
    fr[3] = 0x00;
    fr[4] = c->rate_code;
    fr[7] = (guint8)(((c->nrx - 1) & 7) << 3);
  } else if (k == c->nrx + 1) {
    fr[3] = 0x1C;
    for (guint r = 0; r < 8; r++) {
      if (c->adc_mask >> r & 1) { fr[4 + r / 4] |= (guint8)(1u << (2 * (r % 4))); }
    }
  } else {
    fr[3] = rx_freq_c0(k - 1);
    const guint32 hz = htonl(c->rx[k - 1].freq_word);
    memcpy(fr + 4, &hz, 4);
  }
}

/* All nrx + 1 control words, two per packet (an odd count repeats the
 * rate word). */
static void send_ep2(SkimHpsdrClient *c) {
  const guint words = c->nrx + 1 + (c->agent ? 1 : 0);
  for (guint k = 0; k < words; k += 2) {
    guint8 pkt[PKT_BYTES] = { 0xEF, 0xFE, 0x01, 0x02 };
    const guint32 seq = htonl(c->ep2_seq++);
    memcpy(pkt + 4, &seq, 4);
    put_ctrl(c, k, pkt + 8);
    put_ctrl(c, k + 1 < words ? k + 1 : 0, pkt + 8 + FRAME_BYTES);
    sendto(c->fd, pkt, sizeof(pkt), 0, (const struct sockaddr *)&c->dst,
           sizeof(c->dst));
  }
}

/* ---- EP6 ingest ------------------------------------------------------------- */

static void push_frame(SkimHpsdrClient *c, Rx *r, float re, float im) {
  r->blk[2 * r->fill]     = re;
  r->blk[2 * r->fill + 1] = im;
  if (++r->fill == BLOCK_FRAMES) {
    r->fill = 0;
    if (r->cb) { r->cb(r->blk, BLOCK_FRAMES, c->rate, r->center_hz, r->user); }
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
  const guint slot = 6 * c->nrx + 2;
  const guint per_frame = FRAME_PAYLOAD / slot;
  guint64 filled = 0;

  if (c->have_seq && seq != c->next_seq) {
    const guint32 gap = seq - c->next_seq;   /* mod 2^32                       */
    if (gap > 0x80000000u) {
      return;                                /* late/duplicate packet: drop   */
    }
    if (gap <= MAX_FILL_PKTS) {              /* keep stream time continuous    */
      filled = (guint64)gap * 2 * per_frame;
      for (guint k = 0; k < c->nrx; k++) {
        for (guint64 i = 0; i < filled; i++) { push_frame(c, &c->rx[k], 0.0f, 0.0f); }
      }
    }
  }
  c->have_seq = TRUE;
  c->next_seq = seq + 1;

  for (int f = 0; f < 2; f++) {
    const guint8 *fr = pkt + 8 + f * FRAME_BYTES;
    if (fr[0] != 0x7F || fr[1] != 0x7F || fr[2] != 0x7F) { continue; }
    const guint8 *s = fr + 8;
    for (guint k = 0; k < per_frame; k++, s += slot) {
      for (guint r = 0; r < c->nrx; r++) {             /* RX1 … RXn in order  */
        const float i = (float)be24(s + 6 * r) * SCALE_24;
        const float q = (float)be24(s + 6 * r + 3) * SCALE_24;
        push_frame(c, &c->rx[r], i, IQ_CONJ ? -q : q);
      }
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

static gboolean is_ep6(const SkimHpsdrClient *c, const guint8 *buf, gssize n,
                       const struct sockaddr_in *from) {
  return n == PKT_BYTES && from->sin_addr.s_addr == c->dst.sin_addr.s_addr &&
         buf[0] == 0xEF && buf[1] == 0xFE && buf[2] == 0x01 && buf[3] == 0x06;
}

/* Packets per second the server sends for our rate and receiver count. */
static double expected_pps(const SkimHpsdrClient *c) {
  const guint per_frame = FRAME_PAYLOAD / (6 * c->nrx + 2);
  return (double)c->rate / (2.0 * per_frame);
}

/* The stream still comes to us, but in another layout: someone else's EP2
 * reached our receiver (the server takes it from anyone). Measured over
 * kernel arrival times, so a backlog cannot fake it. */
static void check_layout(SkimHpsdrClient *c, gint64 now, gboolean fresh) {
  if (!c->have_seq) { return; }
  if (!c->chk_have) {
    c->chk_have = TRUE;
    c->chk_seq = c->next_seq;
    c->chk_arrival_us = c->arrival_us;
    return;
  }
  const gint64 dt = c->arrival_us - c->chk_arrival_us;
  if (dt < LAYOUT_CHECK_US) { return; }
  const double got = (double)(guint32)(c->next_seq - c->chk_seq) *
                     G_USEC_PER_SEC / (double)dt;
  const double want = expected_pps(c);
  c->chk_seq = c->next_seq;
  c->chk_arrival_us = c->arrival_us;
  if (!fresh || fabs(got / want - 1.0) <= LAYOUT_TOL) { return; }
  c->layout_resends++;
  if (now - c->last_layout_log_us > LAYOUT_LOG_US) {
    g_message("hpsdr: %s — %.0f packets/s where %u RX × %u Hz send %.0f: "
              "another client reconfigured our receiver; sending our control "
              "words again (%" G_GUINT64_FORMAT " time(s) so far)", c->device,
              got, c->nrx, c->rate, want, c->layout_resends);
    c->last_layout_log_us = now;
  }
  send_ep2(c);
  c->last_refresh_us = now;
}

static gpointer rx_thread(gpointer data) {
  SkimHpsdrClient *c = data;
  guint8 buf[PKT_BYTES + 16];
  while (g_atomic_int_get(&c->run)) {
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    if (poll(&pfd, 1, POLL_MS) > 0) {
      struct sockaddr_in from;
      gint64 arrival;
      const gssize n = recv_stamped(c->fd, buf, sizeof(buf), 0, &from, &arrival);
      if (is_ep6(c, buf, n, &from)) {
        handle_ep6(c, buf);
        c->arrival_us = arrival;
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
      continue;
    }
    if (!ours || c->arrival_us == 0) { continue; }
    const gboolean fresh = g_get_real_time() - c->arrival_us < FRESH_US;
    if (fresh && now - c->started_us < REFRESH_WINDOW_US &&
        now - c->last_refresh_us > REFRESH_US) {
      send_ep2(c);
      c->last_refresh_us = now;
    }
    check_layout(c, now, fresh);
  }
  return NULL;
}

/* After the receive thread has stopped: does the stream still come to us?
 * Drain what the socket holds and judge by the newest packet's kernel
 * arrival time — old means another client has the stream now. */
static gboolean stream_still_ours(SkimHpsdrClient *c) {
  gint64 newest = c->arrival_us;
  guint8 buf[PKT_BYTES + 16];
  for (;;) {
    struct sockaddr_in from;
    gint64 arrival;
    const gssize n = recv_stamped(c->fd, buf, sizeof(buf), MSG_DONTWAIT,
                                  &from, &arrival);
    if (n < 0) { break; }
    if (is_ep6(c, buf, n, &from) && arrival > newest) { newest = arrival; }
  }
  return newest != 0 && g_get_real_time() - newest < FRESH_US;
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
  skim_hpsdr_client_set_rx_iq_cb(c, 0, cb, user_data);
}

void skim_hpsdr_client_set_rx_iq_cb(SkimHpsdrClient *c, guint rx, SkimHpsdrIqCb cb,
                                    gpointer user_data) {
  g_return_if_fail(rx < SKIM_HPSDR_MAX_RX);
  c->rx[rx].cb = cb;
  c->rx[rx].user = user_data;
}

void skim_hpsdr_client_set_closed_cb(SkimHpsdrClient *c, SkimHpsdrClosedCb cb,
                                     gpointer user_data) {
  c->closed_cb = cb;
  c->closed_cb_data = user_data;
}

gboolean skim_hpsdr_client_start(SkimHpsdrClient *c, guint rate, double center_hz,
                                 double clock_ppm, gboolean take_over,
                                 GError **error) {
  return skim_hpsdr_client_start_multi(c, rate, &center_hz, 1, clock_ppm,
                                       take_over, error);
}

gboolean skim_hpsdr_client_start_multi(SkimHpsdrClient *c, guint rate,
                                       const double *centers_hz, guint nrx,
                                       double clock_ppm, gboolean take_over,
                                       GError **error) {
  g_return_val_if_fail(c->fd < 0, FALSE);
  if (nrx < 1 || nrx > SKIM_HPSDR_MAX_RX) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "%u receivers requested (1…%d)", nrx, SKIM_HPSDR_MAX_RX);
    return FALSE;
  }
  if (rate == 0) { rate = 192000; }
  switch (rate) {
    case 48000:  c->rate_code = 0; break;
    case 96000:  c->rate_code = 1; break;
    case 192000: c->rate_code = 2; break;
    case 384000: c->rate_code = 3; break;    /* the 122.88 MHz boards' servers */
    default:
      g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                  "HPSDR P1 sample rate %u not supported (48/96/192/384 kHz)", rate);
      return FALSE;
  }
  for (guint r = 0; r < nrx; r++) {
    const double word = centers_hz[r] / (1.0 + clock_ppm * 1e-6);
    if (!(word > 0 && word < 4294967295.0)) {
      g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                  "bad centre frequency %.0f Hz (RX%u)", centers_hz[r], r + 1);
      return FALSE;
    }
    c->rx[r].center_hz = centers_hz[r];
    c->rx[r].freq_word = (guint32)llround(word);
    c->rx[r].fill = 0;
  }
  c->rate = rate;
  c->nrx = nrx;

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
  if (info.nrx > 0 && nrx > info.nrx) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED,
                "%s has %u receivers, %u requested", c->device, info.nrx, nrx);
    goto fail;
  }
  if (info.busy && !take_over) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_BUSY,
                "%s is in use by another client", c->device);
    goto fail;
  }

  c->ep2_seq = 0;
  c->have_seq = FALSE;
  c->lost_since_log = 0;
  c->last_lost_log_us = 0;
  c->arrival_us = 0;
  c->chk_have = FALSE;
  c->last_layout_log_us = 0;
  c->layout_resends = 0;
  g_mutex_lock(&c->lock);
  c->got_iq = FALSE;
  c->ours = TRUE;
  c->packets = 0;
  c->lost_frames = 0;
  g_mutex_unlock(&c->lock);

  /* Configure first, so the first EP6 packet already has our layout. */
  c->agent = info.agent;
  send_ep2(c);
  send_ep2(c);
  if (c->agent) {
    send_start_marked(c->fd, &c->dst);
  } else {
    send_cmd(c->fd, &c->dst, 0x04, 0x01);
  }
  c->last_refresh_us = c->started_us = g_get_monotonic_time();

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
  GString *cs = g_string_new(NULL);
  for (guint r = 0; r < nrx; r++) {
    g_string_append_printf(cs, "%s%.0f", r ? ", " : "", c->rx[r].center_hz);
  }
  g_message("hpsdr: %s — code %u, board %u, %u RX; streaming %u × %u Hz @ "
            "%s Hz (%+.2f ppm)", c->device, info.code_version, info.board_id,
            info.nrx, nrx, rate, cs->str, clock_ppm);
  g_string_free(cs, TRUE);
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
  if (ours) {
    if (stream_still_ours(c)) {
      send_cmd(c->fd, &c->dst, 0x04, 0x00);
    } else {
      g_message("hpsdr: %s — the stream no longer comes to us (taken over?): "
                "no stop sent", c->device);
    }
  }
  close(c->fd);
  c->fd = -1;
}

const char *skim_hpsdr_client_device(SkimHpsdrClient *c) { return c->device; }
double      skim_hpsdr_client_center_hz(SkimHpsdrClient *c) { return c->rx[0].center_hz; }
guint       skim_hpsdr_client_rate(SkimHpsdrClient *c) { return c->rate; }
guint32     skim_hpsdr_client_freq_word(SkimHpsdrClient *c) { return c->rx[0].freq_word; }
guint       skim_hpsdr_client_nrx(SkimHpsdrClient *c) { return c->nrx; }

guint32 skim_hpsdr_client_rx_freq_word(SkimHpsdrClient *c, guint rx) {
  return rx < c->nrx ? c->rx[rx].freq_word : 0;
}

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

void skim_hpsdr_client_set_inputs(SkimHpsdrClient *c, guint8 mask) { c->adc_mask = mask; }

gboolean skim_hpsdr_client_agent(SkimHpsdrClient *c) { return c->agent; }

guint16 skim_hpsdr_client_local_port(SkimHpsdrClient *c) {
  if (c->fd < 0) { return 0; }
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  if (getsockname(c->fd, (struct sockaddr *)&a, &al) != 0) { return 0; }
  return ntohs(a.sin_port);
}

gboolean skim_hpsdr_stop_orphan(const char *host, guint16 port, guint16 local_port,
                                guint listen_ms, GError **error) {
  struct sockaddr_in dst;
  if (!local_port) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED, "no local port");
    return FALSE;
  }
  if (!resolve(host, port ? port : SKIM_HPSDR_DEFAULT_PORT, &dst, error)) { return FALSE; }
  int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED, "socket: %s",
                g_strerror(errno));
    return FALSE;
  }
  /* No SO_REUSEADDR / SO_REUSEPORT: the bind must fail while anything on
   * this machine still holds the port. */
  struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(local_port),
                            .sin_addr.s_addr = htonl(INADDR_ANY) };
  if (bind(fd, (struct sockaddr *)&me, sizeof(me)) != 0) {
    const int e = errno;
    close(fd);
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_BUSY,
                "port %u of this machine is held by a socket (%s) — a live client",
                local_port, g_strerror(e));
    return FALSE;
  }
  guint ep6 = 0;
  const gint64 end = g_get_monotonic_time() + (gint64)listen_ms * 1000;
  guint8 buf[PKT_BYTES + 16];
  while (ep6 < 3) {
    const gint64 left = end - g_get_monotonic_time();
    if (left <= 0) { break; }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, (int)MAX(left / 1000, 1)) <= 0) { continue; }
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const gssize n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n == PKT_BYTES && from.sin_addr.s_addr == dst.sin_addr.s_addr &&
        from.sin_port == dst.sin_port &&
        buf[0] == 0xEF && buf[1] == 0xFE && buf[2] == 0x01 && buf[3] == 0x06) {
      ep6++;
    }
  }
  if (ep6 < 3) {
    close(fd);
    g_set_error(error, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_NO_IQ,
                "no stream from %s comes to port %u of this machine", host, local_port);
    return FALSE;
  }
  send_cmd(fd, &dst, 0x04, 0x00);
  send_cmd(fd, &dst, 0x04, 0x00);                  /* against UDP loss          */
  close(fd);
  return TRUE;
}
