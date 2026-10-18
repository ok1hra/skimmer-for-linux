/*
 * skimmer-hpsdr-test — offline gate for the HPSDR Protocol 1 client. No
 * radio, no GUI.
 *
 * Runs an in-process mock of Pavel Demin's Red Pitaya sdr-receiver-hpsdr
 * server on 127.0.0.1 (UDP) with the real server's semantics — discovery
 * answers 2 + streaming, EP2 control is taken from ANY sender, a start
 * redirects the stream to whoever sent it, a stop from anyone ends it — and
 * drives skim_hpsdr_client against it:
 *   - discovery: name / RX count / idle→busy flag,
 *   - EP2 bytes: rate code, ONE receiver, the RX1 frequency word with the
 *     clock correction applied (center / (1 + ppm·1e-6)),
 *   - ORIENTATION: the mock sends a +12 kHz tone RF-INVERTED, the way the
 *     real receiver does (live-measured 2026-09-29); after ingest it must
 *     sit at +12 kHz with the −12 kHz image down > 40 dB,
 *   - a sequence gap of 3 packets is zero-filled exactly (3 × 126 frames),
 *   - a busy radio refuses a start without take_over; with take_over the
 *     stream moves, the first client's watchdog fires, and from then on it
 *     sends NOTHING (no EP2 refresh, no stop on free) — the new owner's
 *     stream keeps going,
 *   - stop sends EF FE 04 00 and no IQ callback fires after it,
 *   - a mock that goes silent fires the closed callback once,
 *   - bad rate and an unanswered discovery fail with the right codes,
 *   - SIX receivers (the multi-band layout): every RX's frequency word
 *     reaches its own C0 address, the sample slots demultiplex — RX r's
 *     tone arrives on RX r's callback only — and more receivers than the
 *     radio has are refused.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "engine/hpsdr_p1.h"

#define TONE_HZ  12000.0
#define TONE_AMP 0.5
#define FFT_N    4096

static int fails;
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (cond) { printf("  ok   "); } else { printf("  FAIL "); fails++; }  \
    printf(__VA_ARGS__);                                                   \
    printf("\n");                                                          \
  } while (0)

/* ---- mock sdr-receiver-hpsdr --------------------------------------------- */

static int      m_fd = -1;
static guint16  m_port;
static GMutex   m_lock;
static volatile gint m_run = 1;
static gboolean m_active;               /* streaming (discovery says busy)   */
static gboolean m_mute;                 /* active but sends nothing          */
static struct sockaddr_in m_dst;        /* EP6 target (last start's sender)  */
static guint    m_rate_code = 0, m_nrx = 1;
static guint32  m_freq;                 /* RX1                                */
static guint32  m_rx_freq[8];
static guint    m_skip;                 /* packets to swallow (seq gap)      */
static guint    m_ep2_from[2];          /* EP2 packets per watched port      */
static guint16  m_watch_port[2];
static guint    m_stops;
static guint16  m_last_stop_port;

static guint rate_of(guint code) { return code == 1 ? 96000 : code == 2 ? 192000 : 48000; }

static void mock_ep2_frame(const guint8 *f) {
  switch (f[0]) {
    case 0: case 1:
      m_nrx = ((f[4] >> 3) & 7) + 1;
      m_rate_code = f[1] & 3;
      break;
    default: {
      /* RX1..RX7 at C0 0x04..0x10 (MOX bit ignored), RX8 at 0x24 */
      const guint a = f[0] & 0xFE;
      const int rx = a >= 0x04 && a <= 0x10 ? (int)(a - 0x04) / 2 : a == 0x24 ? 7 : -1;
      if (rx >= 0) {
        guint32 v;
        memcpy(&v, f + 1, 4);
        m_rx_freq[rx] = ntohl(v);
        if (rx == 0) { m_freq = m_rx_freq[0]; }
      }
      break;
    }
  }
}

static void put24(guint8 *p, double v) {
  gint32 x = (gint32)lrint(CLAMP(v, -1.0, 1.0) * 8388607.0);
  p[0] = (guint8)(x >> 16);
  p[1] = (guint8)(x >> 8);
  p[2] = (guint8)x;
}

static gpointer mock_serve(gpointer user) {
  (void)user;
  guint32 seq = 0;
  double  phase = 0;
  gint64  t0 = 0;
  guint64 sent_pkts = 0;                 /* incl. skipped, since start       */
  while (g_atomic_int_get(&m_run)) {
    struct pollfd pfd = { .fd = m_fd, .events = POLLIN };
    if (poll(&pfd, 1, 2) > 0) {
      guint8 buf[1100];
      struct sockaddr_in from;
      socklen_t flen = sizeof(from);
      const gssize n = recvfrom(m_fd, buf, sizeof(buf), 0,
                                (struct sockaddr *)&from, &flen);
      if (n >= 4 && buf[0] == 0xEF && buf[1] == 0xFE) {
        g_mutex_lock(&m_lock);
        if (buf[2] == 0x02) {                           /* discovery          */
          guint8 r[60] = { 0xEF, 0xFE, (guint8)(2 + m_active), 0, 0x26, 0x32,
                           0xF0, 0x89, 0x08, 25, 1, 'R', '_', 'P', 'I', 'T',
                           'A', 'Y', 'A', 8 };
          sendto(m_fd, r, sizeof(r), 0, (struct sockaddr *)&from, flen);
        } else if (buf[2] == 0x01 && buf[3] == 0x02 && n == 1032) {   /* EP2 */
          mock_ep2_frame(buf + 11);
          mock_ep2_frame(buf + 523);
          for (int w = 0; w < 2; w++) {
            if (ntohs(from.sin_port) == m_watch_port[w]) { m_ep2_from[w]++; }
          }
        } else if (buf[2] == 0x04) {
          if (buf[3] == 0x00) {                          /* stop               */
            m_active = FALSE;
            m_stops++;
            m_last_stop_port = ntohs(from.sin_port);
          } else {                                       /* start: redirect    */
            m_dst = from;
            m_active = TRUE;
            seq = 0;
            sent_pkts = 0;
            t0 = g_get_monotonic_time();
          }
        }
        g_mutex_unlock(&m_lock);
      }
    }

    g_mutex_lock(&m_lock);
    if (m_active) {
      const guint rate = rate_of(m_rate_code);
      const guint slot = 6 * m_nrx + 2, per_frame = 504 / slot;
      const guint64 due = (guint64)((g_get_monotonic_time() - t0) * 1e-6 *
                                    rate / (2.0 * per_frame)) + 4;
      while (sent_pkts < due) {
        guint8 pkt[1032] = { 0xEF, 0xFE, 0x01, 0x06 };
        const guint32 s = htonl(seq++);
        memcpy(pkt + 4, &s, 4);
        for (int f = 0; f < 2; f++) {
          guint8 *fr = pkt + 8 + f * 512;
          fr[0] = fr[1] = fr[2] = 0x7F;
          guint8 *d = fr + 8;
          for (guint k = 0; k < per_frame; k++, d += slot) {
            /* RF-inverted like the real DDC: a +f tone as I + j·(−Q).
             * RX r carries TONE_HZ − r·2 kHz, so each slot is tellable. */
            for (guint r = 0; r < m_nrx; r++) {
              const double ph = phase * (TONE_HZ - 2000.0 * r) / TONE_HZ;
              put24(d + 6 * r, TONE_AMP * cos(ph));
              put24(d + 6 * r + 3, -TONE_AMP * sin(ph));
            }
            phase += 2 * G_PI * TONE_HZ / rate;
          }
        }
        phase = fmod(phase, 2 * G_PI * 6);      /* 6 = TONE_HZ/2 kHz: every
                                                  * RX's phase wraps cleanly */
        sent_pkts++;
        if (m_skip) { m_skip--; continue; }            /* lost on the "wire" */
        if (!m_mute) {
          sendto(m_fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&m_dst,
                 sizeof(m_dst));
        }
      }
    }
    g_mutex_unlock(&m_lock);
  }
  return NULL;
}

/* ---- client side ------------------------------------------------------------ */

typedef struct {
  GMutex   lock;
  guint64  frames;
  float    buf[FFT_N * 2];
  guint    fill;
  guint64  skip;                         /* frames to ignore before capture  */
  gint     closed;
} Sink;

static void sink_iq(const float *iq, guint n, double rate, double center, gpointer u) {
  (void)rate; (void)center;
  Sink *s = u;
  g_mutex_lock(&s->lock);
  for (guint i = 0; i < n; i++, s->frames++) {
    if (s->frames >= s->skip && s->fill < FFT_N) {
      s->buf[2 * s->fill]     = iq[2 * i];
      s->buf[2 * s->fill + 1] = iq[2 * i + 1];
      s->fill++;
    }
  }
  g_mutex_unlock(&s->lock);
}

static void sink_closed(gpointer u) { g_atomic_int_inc(&((Sink *)u)->closed); }

static guint64 sink_frames(Sink *s) {
  g_mutex_lock(&s->lock);
  const guint64 v = s->frames;
  g_mutex_unlock(&s->lock);
  return v;
}

/* Power (dB) of a single DFT bin at hz — no FFT needed for two probes. */
static double bin_db(const float *iq, guint n, double rate, double hz) {
  double re = 0, im = 0;
  for (guint k = 0; k < n; k++) {
    const double w = 0.5 * (1 - cos(2 * G_PI * k / (n - 1)));
    const double a = -2 * G_PI * hz * k / rate;
    re += w * (iq[2 * k] * cos(a) - iq[2 * k + 1] * sin(a));
    im += w * (iq[2 * k] * sin(a) + iq[2 * k + 1] * cos(a));
  }
  return 10 * log10(re * re + im * im + 1e-30);
}

static gboolean wait_for(gboolean (*pred)(gpointer), gpointer d, int ms) {
  for (int t = 0; t < ms; t += 10) {
    if (pred(d)) { return TRUE; }
    g_usleep(10000);
  }
  return pred(d);
}
static gboolean pred_fill(gpointer d) {
  Sink *s = d;
  g_mutex_lock(&s->lock);
  const gboolean v = s->fill == FFT_N;
  g_mutex_unlock(&s->lock);
  return v;
}
static gboolean pred_closed(gpointer d) { return g_atomic_int_get(&((Sink *)d)->closed) > 0; }

static guint16 client_port_guess(void) {
  /* The client's ephemeral port is private; the mock learns it from the
   * start (m_dst) — read it right after a successful start. */
  g_mutex_lock(&m_lock);
  const guint16 p = ntohs(m_dst.sin_port);
  g_mutex_unlock(&m_lock);
  return p;
}

int main(void) {
  printf("=== skimmer-hpsdr-test — mock sdr-receiver-hpsdr on 127.0.0.1 ===\n");
  m_fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = { .sin_family = AF_INET,
                           .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
  bind(m_fd, (struct sockaddr *)&a, sizeof(a));
  socklen_t al = sizeof(a);
  getsockname(m_fd, (struct sockaddr *)&a, &al);
  m_port = ntohs(a.sin_port);
  GThread *mt = g_thread_new("mock", mock_serve, NULL);
  GError *err = NULL;

  printf("discovery:\n");
  SkimHpsdrInfo info;
  gboolean ok = skim_hpsdr_discover("127.0.0.1", m_port, 500, &info, &err);
  CHECK(ok && strcmp(info.name, "R_PITAYA") == 0 && info.nrx == 8 && !info.busy,
        "idle R_PITAYA, 8 RX (got %s \"%s\" %u %s)", ok ? "reply" : "none",
        ok ? info.name : "", ok ? info.nrx : 0, ok && info.busy ? "busy" : "idle");
  g_clear_error(&err);

  printf("argument / reachability errors:\n");
  SkimHpsdrClient *bad = skim_hpsdr_client_new("127.0.0.1", m_port);
  ok = skim_hpsdr_client_start(bad, 384000, 14e6, 0, FALSE, &err);
  CHECK(!ok && g_error_matches(err, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_FAILED),
        "384 kHz refused (%s)", err ? err->message : "started!");
  g_clear_error(&err);
  skim_hpsdr_client_free(bad);
  bad = skim_hpsdr_client_new("127.0.0.1", (guint16)(m_port + 1));
  ok = skim_hpsdr_client_start(bad, 48000, 14e6, 0, FALSE, &err);
  CHECK(!ok && g_error_matches(err, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_NO_REPLY),
        "no server → NO_REPLY (%s)", err ? err->message : "started!");
  g_clear_error(&err);
  skim_hpsdr_client_free(bad);

  printf("client A — start, EP2, orientation, gap fill:\n");
  Sink sa = { .skip = 4096 };
  g_mutex_init(&sa.lock);
  SkimHpsdrClient *ca = skim_hpsdr_client_new("127.0.0.1", m_port);
  skim_hpsdr_client_set_iq_cb(ca, sink_iq, &sa);
  skim_hpsdr_client_set_closed_cb(ca, sink_closed, &sa);
  ok = skim_hpsdr_client_start(ca, 48000, 14080000, 2.5, FALSE, &err);
  CHECK(ok, "start (%s)", err ? err->message : "streaming");
  g_clear_error(&err);
  const guint32 want = (guint32)llround(14080000 / (1 + 2.5e-6));
  g_mutex_lock(&m_lock);
  const guint rc = m_rate_code, nrx = m_nrx;
  const guint32 fw = m_freq;
  m_watch_port[0] = ntohs(m_dst.sin_port);
  g_mutex_unlock(&m_lock);
  CHECK(rc == 0 && nrx == 1, "EP2: rate code %u (48 kHz = 0), %u receiver(s)", rc, nrx);
  CHECK(fw == want && skim_hpsdr_client_freq_word(ca) == want,
        "EP2: RX1 word %u Hz = 14080000 / (1 + 2.5 ppm) = %u", fw, want);
  ok = skim_hpsdr_discover("127.0.0.1", m_port, 500, &info, NULL);
  CHECK(ok && info.busy, "discovery now says busy");

  CHECK(wait_for(pred_fill, &sa, 3000), "captured %u frames", sa.fill);
  const double p_tone = bin_db(sa.buf, FFT_N, 48000, TONE_HZ);
  const double p_img  = bin_db(sa.buf, FFT_N, 48000, -TONE_HZ);
  CHECK(p_tone - p_img > 40, "tone at +12 kHz, image %.1f dB down (RF-inverted "
        "wire conjugated on ingest)", p_tone - p_img);

  const guint64 lost0 = skim_hpsdr_client_lost_frames(ca);
  g_mutex_lock(&m_lock);
  m_skip = 3;
  g_mutex_unlock(&m_lock);
  g_usleep(300000);
  const guint64 lost = skim_hpsdr_client_lost_frames(ca) - lost0;
  CHECK(lost == 3 * 126, "3 lost packets zero-filled as %" G_GUINT64_FORMAT
        " frames (want 378)", lost);

  printf("client B — busy radio, take-over:\n");
  Sink sb = { .skip = 0 };
  g_mutex_init(&sb.lock);
  SkimHpsdrClient *cb = skim_hpsdr_client_new("127.0.0.1", m_port);
  skim_hpsdr_client_set_iq_cb(cb, sink_iq, &sb);
  ok = skim_hpsdr_client_start(cb, 48000, 7050000, 0, FALSE, &err);
  CHECK(!ok && g_error_matches(err, SKIM_HPSDR_ERROR, SKIM_HPSDR_ERROR_BUSY),
        "busy → BUSY without take_over (%s)", err ? err->message : "started!");
  g_clear_error(&err);
  g_mutex_lock(&m_lock);
  const guint32 fw_after = m_freq;
  g_mutex_unlock(&m_lock);
  CHECK(fw_after == want, "the refused start sent no EP2 (word still %u)", fw_after);

  ok = skim_hpsdr_client_start(cb, 48000, 7050000, 0, TRUE, &err);
  CHECK(ok, "take_over start (%s)", err ? err->message : "streaming");
  g_clear_error(&err);
  m_watch_port[1] = client_port_guess();
  CHECK(wait_for(pred_closed, &sa, 3500), "client A's watchdog fired (stream gone)");
  g_mutex_lock(&m_lock);
  const guint ep2_a = m_ep2_from[0];
  g_mutex_unlock(&m_lock);
  g_usleep(1500000);                             /* > one refresh period     */
  g_mutex_lock(&m_lock);
  const guint ep2_a2 = m_ep2_from[0], ep2_b = m_ep2_from[1];
  const guint32 fw_b = m_freq;
  const guint stops0 = m_stops;
  g_mutex_unlock(&m_lock);
  CHECK(ep2_a2 == ep2_a && ep2_b > 0 && fw_b == 7050000,
        "after losing the stream A sends no EP2 (%u→%u), B refreshes (%u), "
        "word %u", ep2_a, ep2_a2, ep2_b, fw_b);
  skim_hpsdr_client_free(ca);
  g_usleep(100000);
  g_mutex_lock(&m_lock);
  const guint stops1 = m_stops;
  const gboolean still = m_active;
  g_mutex_unlock(&m_lock);
  CHECK(stops1 == stops0 && still, "freeing A sent no stop — B's stream goes on");
  CHECK(g_atomic_int_get(&sa.closed) == 1, "A's closed callback fired exactly once");

  printf("client B — stop:\n");
  const guint64 fb0 = sink_frames(&sb);
  CHECK(fb0 > 0, "B receives IQ (%" G_GUINT64_FORMAT " frames)", fb0);
  skim_hpsdr_client_stop(cb);
  const guint64 fb1 = sink_frames(&sb);
  g_usleep(300000);
  g_mutex_lock(&m_lock);
  const gboolean idle = !m_active;
  const guint16 sp = m_last_stop_port;
  g_mutex_unlock(&m_lock);
  CHECK(idle && sp == m_watch_port[1], "EF FE 04 00 from B — radio idle");
  CHECK(sink_frames(&sb) == fb1, "no IQ callback after stop");
  skim_hpsdr_client_free(cb);

  printf("client C — the radio goes silent:\n");
  Sink sc = { .skip = 0 };
  g_mutex_init(&sc.lock);
  SkimHpsdrClient *cc = skim_hpsdr_client_new("127.0.0.1", m_port);
  skim_hpsdr_client_set_iq_cb(cc, sink_iq, &sc);
  skim_hpsdr_client_set_closed_cb(cc, sink_closed, &sc);
  ok = skim_hpsdr_client_start(cc, 96000, 3550000, 0, FALSE, &err);
  CHECK(ok, "start at 96 kHz (%s)", err ? err->message : "streaming");
  g_clear_error(&err);
  g_mutex_lock(&m_lock);
  CHECK(m_rate_code == 1, "EP2: rate code %u (96 kHz = 1)", m_rate_code);
  m_mute = TRUE;
  g_mutex_unlock(&m_lock);
  CHECK(wait_for(pred_closed, &sc, 3500), "closed callback after 2 s of silence");
  skim_hpsdr_client_free(cc);

  printf("client D — six receivers (the multi-band layout):\n");
  g_mutex_lock(&m_lock);
  m_active = FALSE;                            /* C was freed without a stop  */
  m_mute = FALSE;
  g_mutex_unlock(&m_lock);
  Sink sd[6];
  SkimHpsdrClient *cd = skim_hpsdr_client_new("127.0.0.1", m_port);
  const double centres[6] = { 1840000, 3540000, 7040000, 10125000, 14040000, 21040000 };
  for (int r = 0; r < 6; r++) {
    memset(&sd[r], 0, sizeof(sd[r]));
    g_mutex_init(&sd[r].lock);
    sd[r].skip = 4096;
    skim_hpsdr_client_set_rx_iq_cb(cd, r, sink_iq, &sd[r]);
  }
  ok = skim_hpsdr_client_start_multi(cd, 48000, centres, 6, 3.81, FALSE, &err);
  CHECK(ok, "start 6 RX (%s)", err ? err->message : "streaming");
  g_clear_error(&err);
  g_mutex_lock(&m_lock);
  gboolean words_ok = m_nrx == 6;
  for (int r = 0; r < 6; r++) {
    words_ok &= m_rx_freq[r] == (guint32)llround(centres[r] / (1 + 3.81e-6));
  }
  const guint mnrx = m_nrx;
  g_mutex_unlock(&m_lock);
  CHECK(words_ok, "EP2: %u receivers, each RX word at its own C0 address", mnrx);
  gboolean demux_ok = TRUE;
  for (int r = 0; r < 6; r++) {
    if (!wait_for(pred_fill, &sd[r], 3000)) { demux_ok = FALSE; continue; }
    const double own = bin_db(sd[r].buf, FFT_N, 48000, TONE_HZ - 2000.0 * r);
    const double other = bin_db(sd[r].buf, FFT_N, 48000, TONE_HZ - 2000.0 * ((r + 1) % 6));
    const double img = bin_db(sd[r].buf, FFT_N, 48000, -(TONE_HZ - 2000.0 * r));
    if (own - other < 40 || own - img < 40) {
      printf("       RX%d: own %.1f, neighbour's %.1f, image %.1f dB\n", r + 1, own, other, img);
      demux_ok = FALSE;
    }
  }
  CHECK(demux_ok, "each RX gets only its own tone, true orientation");
  skim_hpsdr_client_free(cd);
  SkimHpsdrClient *ce = skim_hpsdr_client_new("127.0.0.1", m_port);
  const double nine[9] = { 0 };
  ok = skim_hpsdr_client_start_multi(ce, 48000, nine, 9, 0, FALSE, &err);
  CHECK(!ok, "9 receivers refused (%s)", err ? err->message : "started!");
  g_clear_error(&err);
  skim_hpsdr_client_free(ce);

  g_atomic_int_set(&m_run, 0);
  g_thread_join(mt);
  close(m_fd);
  printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
