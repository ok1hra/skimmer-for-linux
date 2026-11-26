/* rp_agent_test.c — the rp-agent gate: the skimmer's Red Pitaya agent
 * (rp/agent/skimmer-rp.c, built for this machine) in --sim on two loopback
 * addresses, driven by the engine's own HPSDR P1 client (docs/REDPITAYA.md).
 *
 *   - a plain discovery is Pavel Demin's reply byte for byte (no marker), an
 *     SKRP one carries the agent's marker (version, mode, inputs, instance)
 *   - two instances stream at once (3 RX × 96 kHz, 2 RX × 384 kHz): each
 *     receiver's tone at its centre + (rx+1) kHz at TRUE RF — the wire is
 *     RF-inverted like the stock server's and the client conjugates it —
 *     at −12 dBFS (IN1)
 *   - `ctl sel` moves one receiver to IN2 live (−26 dBFS) and leaves the rest
 *   - `ctl status` reports both streams
 *   - a client that dies without a stop: the agent stops that orphan by itself
 *     (ICMP port unreachable for --orphan-s), and the other stream runs on
 *   - paired (docs/PHASING.md): one client starts A and B with the paired
 *     flag — both carry one epoch and a contiguous sample index in their C&C
 *     bytes, and at the same index A's and B's samples are the same instant
 *     (B = 0.2 · A, phase 0: one sample off would be 3.75°); an overflow on
 *     one side starts a new epoch on both, aligned again; an unmarked stream
 *     keeps Pavel's C&C, and a paired start beside it never resets it
 *
 * Usage: skimmer-rp-agent-test PATH/TO/skimmer-rp-host
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <arpa/inet.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine/hpsdr_p1.h"

static int fails;
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (cond) { printf("  ok   "); } else { printf("  FAIL "); fails++; }  \
    printf(__VA_ARGS__);                                                   \
    printf("\n");                                                          \
  } while (0)

#define ADDR_A "127.0.0.2"
#define ADDR_B "127.0.0.3"
#define NBUF   16384

/* The last NBUF frames of one receiver, and the rate/centre they came at. */
typedef struct {
  GMutex  lock;
  float   iq[2 * NBUF];
  guint   pos, have;
  double  rate, centre;
} Rx;

static void iq_cb(const float *iq, guint n, double rate, double centre, gpointer user) {
  Rx *r = user;
  g_mutex_lock(&r->lock);
  for (guint i = 0; i < n; i++) {
    r->iq[2 * r->pos] = iq[2 * i];
    r->iq[2 * r->pos + 1] = iq[2 * i + 1];
    r->pos = (r->pos + 1) % NBUF;
  }
  r->have += n;
  r->rate = rate;
  r->centre = centre;
  g_mutex_unlock(&r->lock);
}

/* Amplitude of the component at offset_hz (a single-bin DFT over the ring,
 * read from its oldest frame on — across the write point the phase jumps). */
static double tone_amp(Rx *r, double offset_hz) {
  g_mutex_lock(&r->lock);
  double re = 0, im = 0;
  const double w = -2 * G_PI * offset_hz / r->rate;
  for (guint k = 0; k < NBUF; k++) {               /* in time order: oldest first */
    const guint j = (r->pos + k) % NBUF;
    const double c = cos(w * k), s = sin(w * k);
    const double i = r->iq[2 * j], q = r->iq[2 * j + 1];
    re += i * c - q * s;
    im += i * s + q * c;
  }
  g_mutex_unlock(&r->lock);
  return hypot(re, im) / NBUF;
}

static void wait_frames(Rx *r, guint want) {
  for (int i = 0; i < 300; i++) {
    g_mutex_lock(&r->lock);
    const guint have = r->have;
    g_mutex_unlock(&r->lock);
    if (have >= want) { return; }
    g_usleep(10000);
  }
}

static void reset_rx(Rx *r) {
  g_mutex_lock(&r->lock);
  r->have = 0;
  g_mutex_unlock(&r->lock);
}

static guint16 port;
static const char *agent;
static char *ctl_path;

static char *ctl(const char *cmd) {
  char *out = NULL;
  char *argv_s = g_strdup_printf("%s ctl --ctl %s %s", agent, ctl_path, cmd);
  gchar **argv = NULL;
  g_shell_parse_argv(argv_s, NULL, &argv, NULL);
  g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, NULL, NULL, NULL);
  g_strfreev(argv);
  g_free(argv_s);
  return out ? out : g_strdup("");
}

/* A raw discovery to addr: the reply's 60 bytes (FALSE = none). */
static gboolean raw_discovery(const char *addr, gboolean skrp, guint8 out[60]) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
  inet_pton(AF_INET, addr, &to.sin_addr);
  guint8 req[63] = { 0xEF, 0xFE, 0x02 };
  if (skrp) { memcpy(req + 4, "SKRP", 4); }
  struct timeval tv = { 1, 0 };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sendto(s, req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to));
  const ssize_t n = recv(s, out, 60, 0);
  close(s);
  return n == 60;
}

static int count_of(const char *hay, const char *needle) {
  int n = 0;
  for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) { n++; }
  return n;
}

/* ---- a raw Protocol 1 client: the paired streams' C&C, sample by sample ---- */

typedef struct {
  int s;
  struct sockaddr_in to;
} Raw;

static void raw_open(Raw *r, const char *addr) {
  r->s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in me = { .sin_family = AF_INET };
  inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);
  bind(r->s, (struct sockaddr *)&me, sizeof(me));
  struct timeval tv = { 0, 200000 };
  setsockopt(r->s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int big = 4 << 20;
  setsockopt(r->s, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  memset(&r->to, 0, sizeof(r->to));
  r->to.sin_family = AF_INET;
  r->to.sin_port = htons(port);
  inet_pton(AF_INET, addr, &r->to.sin_addr);
}

/* One EP2 packet: two control frames (C0, C1..C4 each). */
static void raw_ep2(Raw *r, guint8 c0a, const guint8 ca[4], guint8 c0b, const guint8 cb[4]) {
  guint8 pkt[1032] = { 0xEF, 0xFE, 0x01, 0x02 };
  pkt[8] = pkt[9] = pkt[10] = 0x7F;
  pkt[11] = c0a;
  memcpy(pkt + 12, ca, 4);
  pkt[520] = pkt[521] = pkt[522] = 0x7F;
  pkt[523] = c0b;
  memcpy(pkt + 524, cb, 4);
  sendto(r->s, pkt, sizeof(pkt), 0, (struct sockaddr *)&r->to, sizeof(r->to));
}

/* 96 kHz, one receiver at hz; in2: RX1 on IN2 (the ADC word, marked only). */
static void raw_config(Raw *r, guint32 hz, gboolean in2) {
  const guint8 cfg[4] = { 1, 0, 0, 0 };            /* C1 rate 1 = 96 kHz, 1 RX */
  const guint8 f[4] = { hz >> 24, hz >> 16, hz >> 8, hz };
  raw_ep2(r, 0x00, cfg, 0x04, f);
  const guint8 adc[4] = { in2 ? 0x01 : 0x00, 0, 0, 0 };
  raw_ep2(r, 0x1C, adc, 0x04, f);
}

static void raw_cmd(Raw *r, guint8 b3, gboolean marked, gboolean paired) {
  guint8 pkt[64] = { 0xEF, 0xFE, 0x04, b3 };
  if (marked) { memcpy(pkt + 4, "SKRP", 4); }
  if (paired) { pkt[8] = 0x01; }
  sendto(r->s, pkt, sizeof(pkt), 0, (struct sockaddr *)&r->to, sizeof(r->to));
}

static void raw_drain(Raw *r) {
  guint8 b[1032];
  while (recv(r->s, b, sizeof(b), MSG_DONTWAIT) > 0) {}
}

/* What a run of packets showed. */
typedef struct {
  guint     packets;
  gboolean  marked_all, paired_all, contiguous;
  guint8    c0_plain;                 /* an unmarked frame's C0 (first packet) */
  guint8    epoch;                    /* the last packet's                     */
  GHashTable *at;                     /* idx → complex sample, last epoch only */
} Run;

static gint32 be24s(const guint8 *b) {
  gint32 v = (b[0] << 16) | (b[1] << 8) | b[2];
  return v & 0x800000 ? v - 0x1000000 : v;
}

static void run_init(Run *o) {
  memset(o, 0, sizeof(*o));
  o->marked_all = o->paired_all = o->contiguous = TRUE;
  o->at = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
}

/* One EP6 packet into o (its last epoch's samples by index). */
static void run_take(Run *o, const guint8 *b, guint64 *next, gboolean *have) {
  if (o->packets++ == 0) { o->c0_plain = b[11]; }
  const gboolean mk = b[11] == 0x78 && b[523] == 0x80;
  o->marked_all &= mk;
  if (!mk)
    return;
  o->paired_all &= (b[15] & 1) != 0;
  const guint8 ep = b[12];
  const guint64 idx = ((guint64)b[13] << 40) | ((guint64)b[14] << 32) | ((guint64)b[524] << 24) |
                      ((guint64)b[525] << 16) | ((guint64)b[526] << 8) | b[527];
  if (*have && ep == o->epoch && idx != *next) { o->contiguous = FALSE; }
  if (*have && ep != o->epoch) { g_hash_table_remove_all(o->at); }
  o->epoch = ep;
  *have = TRUE;
  *next = idx + 126;                                    /* 2 × 63 samples, 1 RX */
  for (int f = 0; f < 2; f++) {
    const guint8 *s = b + 16 + f * 512;
    for (int k = 0; k < 63; k++, s += 8) {
      gint64 *key = g_new(gint64, 1);
      *key = (gint64)(idx + (guint64)f * 63 + (guint64)k);
      double *v = g_new(double, 2);
      v[0] = be24s(s);
      v[1] = be24s(s + 3);
      g_hash_table_replace(o->at, key, v);
    }
  }
}

/* secs of both streams at once (rb NULL: one). */
static void run_collect(Raw *ra, Run *a, Raw *rb, Run *b, double secs) {
  run_init(a);
  if (rb) { run_init(b); }
  guint64 na = 0, nb = 0;
  gboolean ha = FALSE, hb = FALSE;
  const gint64 end = g_get_monotonic_time() + (gint64)(secs * G_USEC_PER_SEC);
  while (g_get_monotonic_time() < end) {
    struct pollfd p[2] = { { ra->s, POLLIN, 0 }, { rb ? rb->s : -1, POLLIN, 0 } };
    if (poll(p, rb ? 2 : 1, 100) <= 0)
      continue;
    for (int i = 0; i < 2; i++) {
      if (!(p[i].revents & POLLIN))
        continue;
      guint8 buf[1032];
      if (recv(p[i].fd, buf, sizeof(buf), MSG_DONTWAIT) != 1032 || buf[3] != 0x06)
        continue;
      if (i == 0) { run_take(a, buf, &na, &ha); } else { run_take(b, buf, &nb, &hb); }
    }
  }
}

/* A's and B's samples at the same index: the ratio |b|/|a| and the phase
 * of Σ a·b* (degrees), over n common samples. */
static void run_align(const Run *a, const Run *b, double *ratio, double *deg, guint *n) {
  double re = 0, im = 0, pa = 0, pb = 0;
  *n = 0;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, a->at);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    const double *x = v, *y = g_hash_table_lookup(b->at, k);
    if (!y)
      continue;
    re += x[0] * y[0] + x[1] * y[1];
    im += x[1] * y[0] - x[0] * y[1];
    pa += x[0] * x[0] + x[1] * x[1];
    pb += y[0] * y[0] + y[1] * y[1];
    (*n)++;
  }
  *ratio = pa > 0 ? sqrt(pb / pa) : 0;
  *deg = atan2(im, re) * 180 / G_PI;
}

static void run_free(Run *o) { g_clear_pointer(&o->at, g_hash_table_destroy); }

static void check_paired(void) {
  printf("paired (docs/PHASING.md):\n");
  Raw ra, rb;
  raw_open(&ra, ADDR_A);
  raw_open(&rb, ADDR_B);
  raw_config(&ra, 14030000, FALSE);
  raw_config(&rb, 14030000, TRUE);
  g_usleep(50000);
  raw_cmd(&ra, 0x01, TRUE, TRUE);
  g_usleep(150000);                       /* the second start, well after */
  raw_cmd(&rb, 0x01, TRUE, TRUE);
  g_usleep(300000);
  raw_drain(&ra);
  raw_drain(&rb);
  Run a, b;
  run_collect(&ra, &a, &rb, &b, 0.4);
  double ratio, deg;
  guint n;
  run_align(&a, &b, &ratio, &deg, &n);
  CHECK(a.packets > 100 && b.packets > 100 && a.marked_all && b.marked_all && a.paired_all &&
            b.paired_all && a.contiguous && b.contiguous,
        "both streams: the epoch and index in every packet, paired, contiguous (%u + %u packets)",
        a.packets, b.packets);
  CHECK(a.epoch == b.epoch, "one epoch on both (%u, %u): the second start reset both", a.epoch, b.epoch);
  CHECK(n > 10000 && fabs(ratio - 0.2) < 0.01 && fabs(deg) < 0.5,
        "the same index is the same instant: B/A %.4f (IN2/IN1 = 0.2), phase %+.3f° "
        "(one sample off = 3.75°), %u common samples", ratio, deg, n);
  const guint8 e0 = a.epoch;
  run_free(&a);
  run_free(&b);

  char *out = ctl("sim-overflow a");
  CHECK(g_str_has_prefix(out, "ok"), "ctl sim-overflow a: %s", g_strstrip(out));
  g_free(out);
  g_usleep(200000);
  raw_drain(&ra);
  raw_drain(&rb);
  run_collect(&ra, &a, &rb, &b, 0.4);
  run_align(&a, &b, &ratio, &deg, &n);
  CHECK(a.epoch == b.epoch && a.epoch != e0 && a.contiguous && b.contiguous,
        "an overflow on A: a new epoch on both (%u → %u, %u)", e0, a.epoch, b.epoch);
  CHECK(n > 10000 && fabs(ratio - 0.2) < 0.01 && fabs(deg) < 0.5,
        "… aligned again: B/A %.4f, phase %+.3f°, %u common samples", ratio, deg, n);
  run_free(&a);
  run_free(&b);
  char *st = ctl("status");
  CHECK(count_of(st, "\"paired\":true") == 2 && count_of(st, "\"pair_resets\":2") == 2,
        "status: both paired, two paired resets each (the second start, the overflow)");
  g_free(st);

  /* an unmarked client on A, a paired start on B beside it */
  raw_cmd(&ra, 0x00, FALSE, FALSE);
  raw_cmd(&rb, 0x00, FALSE, FALSE);
  g_usleep(100000);
  Raw rp;
  raw_open(&rp, ADDR_A);
  raw_config(&rp, 7030000, FALSE);
  raw_cmd(&rp, 0x01, FALSE, FALSE);
  g_usleep(200000);
  raw_drain(&rp);
  Run plain;
  run_collect(&rp, &plain, NULL, NULL, 0.1);
  CHECK(plain.packets > 10 && !plain.marked_all && plain.c0_plain % 8 == 0 && plain.c0_plain <= 32,
        "an unmarked stream keeps Pavel's C&C (C0 0x%02x)", plain.c0_plain);
  run_free(&plain);
  raw_cmd(&rb, 0x01, TRUE, TRUE);
  g_usleep(300000);
  st = ctl("status");
  const char *ib = strstr(st, "\"iface\":\"" ADDR_B);
  CHECK(ib && count_of(st, "\"pair_resets\":2") == 2 && strstr(ib, "\"paired\":true") &&
            count_of(st, "\"paired\":true") == 1,
        "a paired start on B beside it: no paired reset, A's FIFOs left alone");
  g_free(st);
  raw_cmd(&rp, 0x00, FALSE, FALSE);
  raw_cmd(&rb, 0x00, FALSE, FALSE);
  close(ra.s);
  close(rb.s);
  close(rp.s);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-rp-agent-test PATH/TO/skimmer-rp-host\n");
    return 2;
  }
  agent = argv[1];
  port = (guint16)(20000 + getpid() % 20000);
  char *dir = g_dir_make_tmp("rp-agent-XXXXXX", NULL);
  ctl_path = g_build_filename(dir, "ctl.sock", NULL);
  char *bind = g_strdup_printf(ADDR_A ":%u," ADDR_B ":%u", port, port);
  GError *err = NULL;
  GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, agent, "serve", "--sim",
                                       "--bind", bind, "--ctl", ctl_path, "--orphan-s", "2", NULL);
  if (!proc) {
    printf("FAIL cannot start %s: %s\n", agent, err->message);
    return 1;
  }
  g_usleep(300000);

  printf("discovery:\n");
  guint8 plain[60], marked[60];
  gboolean ok = raw_discovery(ADDR_A, FALSE, plain);
  gboolean zero = TRUE;
  for (int i = 20; i < 60; i++) { zero &= plain[i] == 0; }
  CHECK(ok && plain[2] == 2 && memcmp(plain + 11, "R_PITAYA", 8) == 0 && plain[0x13] == 8 && zero,
        "a plain discovery: idle, R_PITAYA, 8 RX, nothing after byte 20 (Pavel's reply)");
  ok = raw_discovery(ADDR_B, TRUE, marked);
  CHECK(ok && memcmp(marked + 0x14, "SKRP", 4) == 0 && marked[0x1C] == 1,
        "an SKRP discovery: the agent's marker, instance 1 on " ADDR_B);
  SkimHpsdrInfo info;
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, &err);
  CHECK(ok && info.agent && info.agent_inst == 0 && info.agent_mode == 0 && !info.busy &&
            strcmp(info.agent_peer, ADDR_B) == 0,
        "the engine's discovery sees the agent (v%u.%u, instance %u, the other IP %s)",
        info.agent_major, info.agent_minor, info.agent_inst, info.agent_peer);
  g_clear_error(&err);

  printf("two streams at once:\n");
  static Rx ra[3], rb[2];
  SkimHpsdrClient *a = skim_hpsdr_client_new(ADDR_A, port), *b = skim_hpsdr_client_new(ADDR_B, port);
  const double ca[3] = { 7030000, 14030000, 21030000 }, cb[2] = { 3530000, 28030000 };
  for (guint r = 0; r < 3; r++) { skim_hpsdr_client_set_rx_iq_cb(a, r, iq_cb, &ra[r]); }
  for (guint r = 0; r < 2; r++) { skim_hpsdr_client_set_rx_iq_cb(b, r, iq_cb, &rb[r]); }
  skim_hpsdr_client_set_inputs(a, 0x04);   /* RX3 on IN2, through Protocol 1 */
  ok = skim_hpsdr_client_start_multi(a, 96000, ca, 3, 0, FALSE, &err);
  CHECK(ok, "A: 3 RX × 96 kHz started%s%s", err ? ": " : "", err ? err->message : "");
  g_clear_error(&err);
  ok = skim_hpsdr_client_start_multi(b, 384000, cb, 2, 0, FALSE, &err);
  CHECK(ok, "B: 2 RX × 384 kHz started%s%s", err ? ": " : "", err ? err->message : "");
  g_clear_error(&err);
  for (guint r = 0; r < 3; r++) { wait_frames(&ra[r], 3 * NBUF); }
  for (guint r = 0; r < 2; r++) { wait_frames(&rb[r], 3 * NBUF); }
  CHECK(skim_hpsdr_client_agent(a) && skim_hpsdr_client_agent(b), "the client knows it talks to the agent");
  for (guint r = 0; r < 3; r++) {
    const double on = tone_amp(&ra[r], 1000.0 * (r + 1)), mirror = tone_amp(&ra[r], -1000.0 * (r + 1));
    const double want = r == 2 ? 0.05 : 0.25;     /* RX3: IN2, set by the client's ADC word */
    CHECK(fabs(on - want) < 0.03 * want / 0.25 && mirror < 0.01 && ra[r].rate == 96000 && ra[r].centre == ca[r],
          "A RX%u: tone at +%u kHz %.3f (%s), mirror %.4f, %.0f Hz at %.0f", r + 1,
          r + 1, on, r == 2 ? "IN2 by the client's ADC word: 0.05" : "IN1: 0.25", mirror, ra[r].rate,
          ra[r].centre);
  }
  for (guint r = 0; r < 2; r++) {
    const double on = tone_amp(&rb[r], 1000.0 * (r + 1));
    CHECK(fabs(on - 0.25) < 0.03 && rb[r].rate == 384000,
          "B RX%u: tone at +%u kHz %.3f at 384 kHz (lost frames %" G_GUINT64_FORMAT ", packets %" G_GUINT64_FORMAT ")",
          r + 1, r + 1, on, skim_hpsdr_client_lost_frames(b), skim_hpsdr_client_packets(b));
  }
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL);
  CHECK(ok && info.busy, "A answers busy while it streams");

  printf("inputs, live:\n");
  char *out = ctl("sel b 0x02");
  CHECK(g_str_has_prefix(out, "ok"), "ctl sel b 0x02: %s", g_strstrip(out));
  g_free(out);
  g_usleep(100000);
  reset_rx(&rb[0]);
  reset_rx(&rb[1]);
  wait_frames(&rb[0], 2 * NBUF);
  wait_frames(&rb[1], 2 * NBUF);
  const double b1 = tone_amp(&rb[0], 1000.0), b2 = tone_amp(&rb[1], 2000.0);
  CHECK(fabs(b1 - 0.25) < 0.03 && fabs(b2 - 0.05) < 0.01,
        "B RX1 stays on IN1 (%.3f), RX2 moved to IN2 (%.3f = −26 dBFS; lost frames %" G_GUINT64_FORMAT ")",
        b1, b2, skim_hpsdr_client_lost_frames(b));
  out = ctl("sel c 0x01");
  CHECK(g_str_has_prefix(out, "error"), "ctl sel c refused");
  g_free(out);

  printf("status:\n");
  out = ctl("status");
  CHECK(count_of(out, "\"streaming\":true") == 2 && strstr(out, "\"rate\":384000") &&
        strstr(out, "\"receivers\":3") && strstr(out, "\"sel\":2") && strstr(out, "\"board\":\"SIM\""),
        "both streams, A 3 RX, B 384 kHz with RX2 on IN2");
  g_free(out);

  printf("the stop, and an orphan:\n");
  skim_hpsdr_client_stop(a);
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL);
  CHECK(ok && !info.busy, "A idle after the client's stop");
  ok = raw_discovery(ADDR_A, TRUE, marked);
  CHECK(ok && marked[0x1B] == 0, "… its inputs back to the manager's (0x%02x)", marked[0x1B]);
  /* a client that starts A and dies without a stop */
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
  inet_pton(AF_INET, ADDR_A, &to.sin_addr);
  guint8 ep2[1032] = { 0xEF, 0xFE, 0x01, 0x02 };
  ep2[8] = ep2[9] = ep2[10] = 0x7F;
  ep2[11] = 0x1C;                               /* every receiver on IN2 … */
  ep2[12] = ep2[13] = 0x55;
  sendto(s, ep2, sizeof(ep2), 0, (struct sockaddr *)&to, sizeof(to));
  guint8 start[64] = { 0xEF, 0xFE, 0x04, 0x01 };
  sendto(s, start, sizeof(start), 0, (struct sockaddr *)&to, sizeof(to));
  g_usleep(300000);
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL);
  CHECK(ok && info.busy, "A busy: a client streams");
  ok = raw_discovery(ADDR_A, TRUE, marked);
  CHECK(ok && marked[0x1B] == 0, "… an unmarked client's ADC word changes nothing (0x%02x)", marked[0x1B]);
  close(s);
  gint64 t0 = g_get_monotonic_time();
  gboolean idle = FALSE;
  while (!idle && g_get_monotonic_time() - t0 < 8 * G_USEC_PER_SEC) {
    g_usleep(250000);
    idle = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL) && !info.busy;
  }
  CHECK(idle, "the client died: the agent stopped its orphan by itself in %.1f s (--orphan-s 2)",
        (g_get_monotonic_time() - t0) / 1e6);
  out = ctl("status");
  CHECK(strstr(out, "\"orphan_stops\":1") != NULL, "status counts the orphan stop");
  g_free(out);
  reset_rx(&rb[0]);
  wait_frames(&rb[0], NBUF);
  CHECK(rb[0].have >= NBUF && skim_hpsdr_discover(ADDR_B, port, 500, &info, NULL) && info.busy,
        "B streams on through all of it");
  skim_hpsdr_client_stop(b);
  skim_hpsdr_client_free(a);
  skim_hpsdr_client_free(b);
  g_usleep(100000);
  check_paired();

  g_subprocess_force_exit(proc);
  g_subprocess_wait(proc, NULL, NULL);
  g_object_unref(proc);
  g_unlink(ctl_path);
  g_rmdir(dir);
  printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
