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
  CHECK(ok && info.agent && info.agent_inst == 0 && info.agent_mode == 0 && !info.busy,
        "the engine's discovery sees the agent (v%u.%u, instance %u)", info.agent_major,
        info.agent_minor, info.agent_inst);
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

  g_subprocess_force_exit(proc);
  g_subprocess_wait(proc, NULL, NULL);
  g_object_unref(proc);
  g_unlink(ctl_path);
  g_rmdir(dir);
  printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
