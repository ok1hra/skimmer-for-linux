/* rp_stream_test.c — gate `rp-stream`: SKRS (src/engine/rp_stream.c) from the
 * skimmer's agent (rp/agent/skimmer-rp.c, built for this machine) on the
 * skimmer's own bitstream, simulated (--sim-skrf: docs/FPGA.md):
 *   - 16 streams at once, every rate (48/96/192/384 kHz), IN1 and IN2: the
 *     discovery names the slots and the port; each stream at its rate, its
 *     tone where the simulation puts it (the slot's (s mod 8)+1 kHz above
 *     the centre, true orientation after the client's conjugate) at its
 *     input's level, no mirror;
 *   - a pair (one rate, IN1 and IN2): the same index is the same instant
 *     (IN2/IN1 = 0.2, phase 0);
 *   - SKRS holds instance A's slots: A answers busy and ignores a P1 start;
 *   - the stop frees them; a client that vanishes is stopped after 5 s;
 *   - a second SKRS client is refused while one streams; too many streams
 *     are refused.
 * Part of skimmer-for-linux. GPL-3.0-or-later. */
#include <arpa/inet.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "engine/hpsdr_p1.h"
#include "engine/rp_stream.h"

#define ADDR_A "127.0.0.4"
#define ADDR_B "127.0.0.5"
#define NS 16

static int fails;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (cond) { printf("  ok   "); } else { printf("  FAIL "); fails++; } \
    printf(__VA_ARGS__);                                    \
    printf("\n");                                           \
  } while (0)

typedef struct {
  GMutex   lock;
  double   rate, centre;
  guint64  frames;
  gint64   t_first, t_last;
  GArray  *iq;                     /* the first ~0.5 s, for the tone          */
  GHashTable *at;                  /* idx → I/Q (a pair's streams)            */
} Rx;

static Rx rx[NS];
static gboolean keep_index;

static void cb(guint id, const float *iq, guint n, double rate, double centre, guint32 epoch,
               guint64 idx, gpointer user) {
  (void)user;
  (void)epoch;
  if (id >= NS) { return; }
  Rx *r = &rx[id];
  g_mutex_lock(&r->lock);
  const gint64 now = g_get_monotonic_time();
  if (!r->frames) { r->t_first = now; }
  r->t_last = now;
  r->frames += n;
  r->rate = rate;
  r->centre = centre;
  if (r->iq->len < 2 * (guint)(rate / 2)) { g_array_append_vals(r->iq, iq, 2 * n); }
  if (keep_index && (id == 0 || id == 8)) {
    for (guint k = 0; k < n; k++) {
      gint64 *key = g_new(gint64, 1);
      *key = (gint64)(idx + k);
      float *v = g_new(float, 2);
      v[0] = iq[2 * k];
      v[1] = iq[2 * k + 1];
      g_hash_table_replace(r->at, key, v);
    }
  }
  g_mutex_unlock(&r->lock);
}

/* |the tone| at offset_hz in r's first samples (a DFT bin, Hann). */
static double tone(Rx *r, double offset_hz) {
  g_mutex_lock(&r->lock);
  const guint n = r->iq->len / 2;
  const float *x = (const float *)r->iq->data;
  double re = 0, im = 0, wsum = 0;
  for (guint k = 0; k < n; k++) {
    const double w = 0.5 - 0.5 * cos(2 * G_PI * k / n);
    const double ph = -2 * G_PI * offset_hz * k / r->rate;
    re += w * (x[2 * k] * cos(ph) - x[2 * k + 1] * sin(ph));
    im += w * (x[2 * k] * sin(ph) + x[2 * k + 1] * cos(ph));
    wsum += w;
  }
  g_mutex_unlock(&r->lock);
  return wsum > 0 ? sqrt(re * re + im * im) / wsum : 0;
}

static char *ctl_path;
static char *ctl(const char *cmd) {
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un a = { .sun_family = AF_UNIX };
  g_strlcpy(a.sun_path, ctl_path, sizeof(a.sun_path));
  if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return g_strdup(""); }
  char *line = g_strdup_printf("%s\n", cmd);
  if (write(s, line, strlen(line)) < 0) { /* the read below says */ }
  g_free(line);
  shutdown(s, SHUT_WR);
  GString *out = g_string_new(NULL);
  char buf[4096];
  ssize_t n;
  while ((n = read(s, buf, sizeof(buf))) > 0) { g_string_append_len(out, buf, n); }
  close(s);
  return g_string_free(out, FALSE);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-rp-stream-test PATH/TO/skimmer-rp-host\n");
    return 2;
  }
  const guint16 port = (guint16)(20000 + getpid() % 20000);
  char *dir = g_dir_make_tmp("rp-stream-XXXXXX", NULL);
  ctl_path = g_build_filename(dir, "ctl.sock", NULL);
  char *bind = g_strdup_printf(ADDR_A ":%u," ADDR_B ":%u", port, port);
  GError *err = NULL;
  GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &err, argv[1], "serve", "--sim-skrf",
                                       "--bind", bind, "--ctl", ctl_path, NULL);
  if (!proc) {
    printf("FAIL cannot start %s: %s\n", argv[1], err->message);
    return 1;
  }
  g_usleep(300000);
  for (int i = 0; i < NS; i++) {
    g_mutex_init(&rx[i].lock);
    rx[i].iq = g_array_new(FALSE, FALSE, sizeof(float));
    rx[i].at = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
  }

  printf("discovery:\n");
  SkimHpsdrInfo info;
  gboolean ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, &err);
  CHECK(ok && info.agent && info.agent_slots == 24 && info.agent_skrs_port == port + 1,
        "the agent on the skimmer's bitstream: %u slots, SKRS on port %u", info.agent_slots,
        info.agent_skrs_port);
  g_clear_error(&err);

  printf("%d streams at once:\n", NS);
  static const guint rates[4] = { 48000, 96000, 192000, 384000 };
  SkimRpStreamSpec spec[NS];
  for (int i = 0; i < NS; i++) {
    /* 0 and 8 (slots 16 and 0: the simulation's same tone): a pair — 96 k,
       IN1 and IN2, one centre; the rest every rate */
    spec[i].centre_hz = 1000000.0 * (2 + i);
    spec[i].rate = i == 0 || i == 8 ? 96000 : rates[i % 4];
    spec[i].input = i == 8 || i % 5 == 4 ? 2 : 1;
  }
  spec[8].centre_hz = spec[0].centre_hz;
  keep_index = TRUE;
  SkimRpStream *st = skim_rp_stream_new(ADDR_A, info.agent_skrs_port);
  skim_rp_stream_set_cb(st, cb, NULL);
  ok = skim_rp_stream_start(st, spec, NS, 0, &err);
  CHECK(ok, "SKRS: %d streams started%s%s", NS, err ? ": " : "", err ? err->message : "");
  g_clear_error(&err);
  g_usleep(1500000);
  char *status = ctl("status");
  /* the slots the agent gave: 16 … 23, then instance A's 0 … 7 (idle), then B's */
  for (int i = 0; i < NS; i++) {
    const int slot = i < 8 ? 16 + i : i - 8;
    char want[64];
    g_snprintf(want, sizeof(want), "{\"id\":%d,\"slot\":%d,", i, slot);
    Rx *r = &rx[i];
    g_mutex_lock(&r->lock);
    const double secs = (r->t_last - r->t_first) / 1e6;
    const double got = secs > 0.5 ? (double)r->frames / secs : 0;
    g_mutex_unlock(&r->lock);
    const double on = tone(r, 1000.0 * (slot % 8 + 1)), mirror = tone(r, -1000.0 * (slot % 8 + 1));
    const double amp = spec[i].input == 2 ? 0.05 : 0.25;
    CHECK(strstr(status, want) && fabs(got / spec[i].rate - 1) < 0.15 && fabs(on - amp) < 0.03 * amp / 0.25 &&
              mirror < 0.01 && r->centre == spec[i].centre_hz,
          "stream %2d: slot %2d, %3.0f k (%.3f of it), IN%u tone %.3f, mirror %.4f, lost %" G_GUINT64_FORMAT,
          i, slot, spec[i].rate / 1e3, got / spec[i].rate, spec[i].input, on, mirror,
          skim_rp_stream_lost_frames(st, i));
  }
  {
    double re = 0, im = 0, pa = 0, pb = 0;
    guint n = 0;
    g_mutex_lock(&rx[0].lock);
    g_mutex_lock(&rx[8].lock);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, rx[0].at);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      const float *x = v, *y = g_hash_table_lookup(rx[8].at, k);
      if (!y) { continue; }
      re += x[0] * y[0] + x[1] * y[1];
      im += x[1] * y[0] - x[0] * y[1];
      pa += x[0] * x[0] + x[1] * x[1];
      pb += y[0] * y[0] + y[1] * y[1];
      n++;
    }
    g_mutex_unlock(&rx[8].lock);
    g_mutex_unlock(&rx[0].lock);
    const double ratio = pa > 0 ? sqrt(pb / pa) : 0, deg = atan2(im, re) * 180 / G_PI;
    CHECK(n > 50000 && fabs(ratio - 0.2) < 0.01 && fabs(deg) < 0.5,
          "the pair (slots 16 and 0): the same index is the same instant — IN2/IN1 %.4f (0.2), "
          "phase %+.3f° (a sample off = 3.75°), %u common samples",
          ratio, deg, n);
  }
  keep_index = FALSE;

  printf("beside P1:\n");
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL);
  CHECK(ok && info.busy, "instance A answers busy: SKRS holds its slots");
  ok = skim_hpsdr_discover(ADDR_B, port, 500, &info, NULL);
  CHECK(ok && !info.busy, "instance B idle (its slots free)");
  SkimRpStream *other = skim_rp_stream_new(ADDR_A, info.agent_skrs_port ? info.agent_skrs_port : port + 1);
  SkimRpStreamSpec one = { 7030000, 48000, 1 };
  ok = skim_rp_stream_start(other, &one, 1, 0, &err);
  CHECK(!ok, "a second SKRS client gets nothing while one streams%s%s", err ? ": " : "",
        err ? err->message : "");
  g_clear_error(&err);
  skim_rp_stream_free(other);
  g_free(status);

  printf("the stop, and a client that vanishes:\n");
  skim_rp_stream_stop(st);
  g_usleep(300000);
  ok = skim_hpsdr_discover(ADDR_A, port, 500, &info, NULL);
  CHECK(ok && !info.busy, "the stop frees instance A's slots");
  status = ctl("status");
  CHECK(strstr(status, "\"skrs\":{\"streaming\":false") != NULL, "status: SKRS not streaming");
  g_free(status);
  SkimRpStreamSpec many[25];
  for (int i = 0; i < 25; i++) { many[i] = one; }
  ok = skim_rp_stream_start(st, many, 25, 0, &err);
  CHECK(!ok, "25 streams on 24 slots refused%s%s", err ? ": " : "", err ? err->message : "");
  g_clear_error(&err);
  /* a client that starts and goes away without a stop: raw datagrams */
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons((guint16)(port + 1)) };
  inet_pton(AF_INET, ADDR_A, &to.sin_addr);
  guint8 start[14] = { 'S', 'K', 'R', 'C', 1, 1, 1, 0, 0x00, 0x6B, 0x45, 0xF0, 0, 1 };   /* 7030000 Hz, 48 k */
  sendto(s, start, sizeof(start), 0, (struct sockaddr *)&to, sizeof(to));
  g_usleep(500000);
  status = ctl("status");
  CHECK(strstr(status, "\"skrs\":{\"streaming\":true") != NULL, "a raw client streams");
  g_free(status);
  close(s);
  gint64 t0 = g_get_monotonic_time();
  gboolean stopped = FALSE;
  while (!stopped && g_get_monotonic_time() - t0 < 9 * G_USEC_PER_SEC) {
    g_usleep(250000);
    status = ctl("status");
    stopped = strstr(status, "\"skrs\":{\"streaming\":false") != NULL && strstr(status, "\"timeouts\":1");
    g_free(status);
  }
  CHECK(stopped, "… it sent no keep-alive: stopped after %.1f s (5 s)", (g_get_monotonic_time() - t0) / 1e6 + 0.5);

  skim_rp_stream_free(st);
  g_subprocess_force_exit(proc);
  g_subprocess_wait(proc, NULL, NULL);
  g_object_unref(proc);
  g_unlink(ctl_path);
  g_rmdir(dir);
  printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
