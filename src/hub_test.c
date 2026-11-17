/*
 * skimmer-hub-test — the hub's side of the hub ↔ worker link (headless/hub.h).
 *
 * A raw socket plays the worker: HELLO → the fragment for its name; the IQ
 * of its band arrives as int24 frames, exactly the samples pushed (a float
 * n / 2^23 comes back as n), nothing of a band it does not have; its SPOT,
 * STAT and STATUS lines reach the hub. A second link under the same name is
 * refused, an unknown name gets NONE and is listed idle, an address outside
 * allow is refused. A worker that stops reading loses blocks — counted, and
 * the seq shows the gap — while push() never waits. The hub is freed with a
 * worker still on the link.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "headless/hub.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static const char FRAG[] = "[radio]\nrate=48000\n[bands]\n20m=14045500 cw\n";

static char *assign(const char *worker, gpointer user) {
  (void)user;
  return strcmp(worker, "pc2") == 0 ? g_strdup(FRAG) : NULL;
}

static GMutex lock;
static GString *spots;
static void on_spot(const char *worker, const char *call, const char *mode, double f, double snr,
                    double speed, gpointer user) {
  (void)user;
  g_mutex_lock(&lock);
  g_string_append_printf(spots, "%s %s %s %.0f %.0f %.0f;", worker, call, mode, f, snr, speed);
  g_mutex_unlock(&lock);
}

static GSocket *connect_to(guint16 port) {
  GSocketClient *sc = g_socket_client_new();
  GSocketConnection *c = g_socket_client_connect_to_host(sc, "127.0.0.1", port, NULL, NULL);
  g_object_unref(sc);
  if (!c) { return NULL; }
  GSocket *s = g_object_ref(g_socket_connection_get_socket(c));
  g_object_set_data_full(G_OBJECT(s), "conn", c, g_object_unref);
  g_socket_set_timeout(s, 5);
  return s;
}

static gboolean read_n(GSocket *s, void *buf, gsize n) {
  guint8 *p = buf;
  while (n) {
    const gssize r = g_socket_receive(s, (char *)p, n, NULL, NULL);
    if (r <= 0) { return FALSE; }
    p += r;
    n -= (gsize)r;
  }
  return TRUE;
}

static char *read_line(GSocket *s) {
  GString *l = g_string_new(NULL);
  char c;
  while (read_n(s, &c, 1) && c != '\n') { g_string_append_c(l, c); }
  return g_string_free(l, FALSE);
}

static void say(GSocket *s, const char *line) { g_socket_send(s, line, strlen(line), NULL, NULL); }

typedef struct {
  guint8  band;
  guint32 frames, rate;
  guint64 seq;
  double  centre;
  gint32 *v;                         /* frames × 2                          */
} Frame;

static gboolean read_frame(GSocket *s, Frame *f) {
  guint8 h[SKIM_HD_HUB_HDR];
  if (!read_n(s, h, sizeof(h))) { return FALSE; }
  guint32 magic;
  memcpy(&magic, h, 4);
  if (GUINT32_FROM_LE(magic) != SKIM_HD_HUB_MAGIC) { return FALSE; }
  f->band = h[4];
  memcpy(&f->frames, h + 8, 4);
  f->frames = GUINT32_FROM_LE(f->frames);
  memcpy(&f->rate, h + 12, 4);
  f->rate = GUINT32_FROM_LE(f->rate);
  memcpy(&f->seq, h + 16, 8);
  f->seq = GUINT64_FROM_LE(f->seq);
  union { guint64 u; double d; } c;
  memcpy(&c.u, h + 24, 8);
  c.u = GUINT64_FROM_LE(c.u);
  f->centre = c.d;
  guint8 *raw = g_malloc((gsize)f->frames * 6);
  const gboolean ok = read_n(s, raw, (gsize)f->frames * 6);
  f->v = g_new(gint32, 2 * f->frames);
  for (guint i = 0; i < 2 * f->frames; i++) {
    const guint8 *p = raw + 3 * i;
    const gint32 x = (gint32)((guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16);
    f->v[i] = (x ^ 0x800000) - 0x800000;
  }
  g_free(raw);
  return ok;
}

int main(void) {
  spots = g_string_new(NULL);
  const char *allow[] = { "127.0.0.0/8", NULL };
  const char *bw[] = { "", "pc2" };
  GError *err = NULL;
  SkimHdHub *hub = skim_hd_hub_new(0, allow, bw, 2, assign, on_spot, NULL, &err);
  check("the hub listens", hub && skim_hd_hub_port(hub));
  const guint16 port = skim_hd_hub_port(hub);

  printf("== handshake\n");
  GSocket *w = connect_to(port);
  say(w, "SKIMHUB 1 HELLO name=pc2 version=test\n");
  char *head = read_line(w);
  gsize n = 0;
  check("CONFIG with its length", sscanf(head, "SKIMHUB 1 CONFIG %zu", &n) == 1 && n == strlen(FRAG));
  char *frag = g_malloc0(n + 1);
  check("… and pc2's fragment", read_n(w, frag, n) && strcmp(frag, FRAG) == 0);
  g_free(head);
  g_free(frag);
  for (int t = 0; t < 100; t++) {                    /* registered on the hub */
    char *j = skim_hd_hub_json(hub);
    const gboolean in = strstr(j, "\"name\":\"pc2\"") != NULL;
    g_free(j);
    if (in) { break; }
    g_usleep(10 * 1000);
  }

  printf("== IQ\n");
  float iq[2 * 64];
  for (int i = 0; i < 2 * 64; i++) {
    iq[i] = (float)((i % 2 ? -1 : 1) * (i * 65537 % 8388607)) / SKIM_HD_HUB_FULLSCALE;
  }
  iq[0] = -1.0f;                                     /* full scale            */
  skim_hd_hub_push(hub, 0, iq, 64, 48000, 7045500);  /* a band pc2 has not   */
  skim_hd_hub_push(hub, 1, iq, 64, 48000, 14045500);
  Frame f = { 0 };
  const gboolean got = read_frame(w, &f);
  check("a frame of pc2's band: its band 0, 64 frames, 48 kHz, seq 0, the centre",
        got && f.band == 0 && f.frames == 64 && f.rate == 48000 && f.seq == 0 &&
        f.centre == 14045500);
  gboolean exact = got;
  for (int i = 0; got && i < 2 * 64; i++) {
    exact = exact && f.v[i] == (gint32)(iq[i] * SKIM_HD_HUB_FULLSCALE);
  }
  check("… every sample exact (int24, -1.0 = -2^23)", exact && f.v[0] == -8388608);
  g_free(f.v);
  skim_hd_hub_push(hub, 1, iq, 64, 48000, 14045500);
  check("the next frame: seq 64", read_frame(w, &f) && f.seq == 64);
  g_free(f.v);

  printf("== the worker's lines\n");
  say(w, "SPOT DL1ABC CW 14025000 20 25\nSTAT 20m cw 768 5 3 1\nSTATUS {\"version\":\"w\"}\n");
  gboolean sp = FALSE;
  for (int t = 0; t < 200 && !sp; t++) {
    g_mutex_lock(&lock);
    sp = strstr(spots->str, "pc2 DL1ABC CW 14025000 20 25;") != NULL;
    g_mutex_unlock(&lock);
    g_usleep(10 * 1000);
  }
  check("SPOT reaches the hub's callback, the worker named", sp);
  for (int t = 0; t < 200 && skim_hd_hub_band_active(hub, 1, 0) != 5; t++) { g_usleep(10 * 1000); }
  check("STAT: 5 active CW decoders on hub band 1, nothing on band 0",
        skim_hd_hub_band_active(hub, 1, 0) == 5 && skim_hd_hub_band_active(hub, 0, 0) == -1);
  char *j = skim_hd_hub_json(hub);
  check("STATUS: the worker's status in the hub's json, its bands [1]",
        strstr(j, "\"status\":{\"version\":\"w\"}") && strstr(j, "\"bands\":[1]") &&
        strstr(j, "\"assigned\":true"));
  g_free(j);

  printf("== other links\n");
  GSocket *dup = connect_to(port);
  say(dup, "SKIMHUB 1 HELLO name=pc2 version=test\n");
  char *r = read_line(dup);
  check("a second pc2 is refused", g_str_has_prefix(r, "SKIMHUB 1 REFUSED"));
  g_free(r);
  g_object_unref(dup);
  GSocket *idle = connect_to(port);
  say(idle, "SKIMHUB 1 HELLO name=pc9 version=test\n");
  r = read_line(idle);
  check("pc9 (nothing assigned): NONE", strcmp(r, "SKIMHUB 1 NONE") == 0);
  g_free(r);
  char **iw = NULL;
  for (int t = 0; t < 100; t++) {
    g_strfreev(iw);
    iw = skim_hd_hub_idle_workers(hub);
    if (iw[0]) { break; }
    g_usleep(10 * 1000);
  }
  check("… and listed as an idle worker", iw[0] && strcmp(iw[0], "pc9") == 0 && !iw[1]);
  g_strfreev(iw);
  const char *none[] = { "10.9.9.9/32", NULL };
  SkimHdHub *closed = skim_hd_hub_new(0, none, bw, 2, assign, on_spot, NULL, &err);
  GSocket *out = connect_to(skim_hd_hub_port(closed));
  say(out, "SKIMHUB 1 HELLO name=pc2 version=test\n");
  r = read_line(out);
  check("an address outside allow is refused", g_str_has_prefix(r, "SKIMHUB 1 REFUSED") &&
        strstr(r, "allow"));
  g_free(r);
  g_object_unref(out);
  skim_hd_hub_free(closed);

  printf("== a worker that stops reading\n");
  const gint64 t0 = g_get_monotonic_time();
  static float big[2 * 4800];
  for (int i = 0; i < 3000; i++) { skim_hd_hub_push(hub, 1, big, 4800, 48000, 14045500); }
  const double secs = (g_get_monotonic_time() - t0) / 1e6;
  j = skim_hd_hub_json(hub);
  guint64 dropped = 0;
  const char *d = strstr(j, "\"dropped\":");
  if (d) { dropped = g_ascii_strtoull(d + 10, NULL, 10); }
  printf("       300 s of IQ pushed in %.2f s, %" G_GUINT64_FORMAT " frames dropped\n", secs, dropped);
  check("push() never waits for it; the blocks that do not fit are dropped, counted",
        secs < 5 && dropped > 0);
  g_free(j);
  /* read what was queued, then one more block: its seq jumps the dropped ones */
  const guint64 queued = 3000ull * 4800 - dropped;
  guint64 expect = 128;
  gboolean contiguous = TRUE;
  while (expect < 128 + queued && read_frame(w, &f)) {
    contiguous = contiguous && f.seq == expect;
    expect = f.seq + f.frames;
    g_free(f.v);
  }
  skim_hd_hub_push(hub, 1, big, 4800, 48000, 14045500);
  const gboolean more = read_frame(w, &f);
  check("… the queued ones arrive in order, the next one's seq shows the gap",
        contiguous && expect == 128 + queued && more && f.seq == 128 + 3000ull * 4800);
  if (more) { g_free(f.v); }

  skim_hd_hub_free(hub);
  check("the hub freed with two workers on their links", TRUE);
  g_object_unref(w);
  g_object_unref(idle);
  g_string_free(spots, TRUE);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
