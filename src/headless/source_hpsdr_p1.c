/* source_hpsdr_p1.c — a Red Pitaya running sdr_receiver_hpsdr (HPSDR
 * Protocol 1): one receiver per band, hardware DDCs, the RF-inverted wire
 * conjugated by the engine's client (docs/HPSDR-P1.md).
 *
 * The radio is never taken from another client unless take_over is given
 * (one-shot: for the first start only — it then takes the first busy radio
 * of the list). The source streams from the first IDLE radio of [radio]
 * host, at start and again whenever the stream is lost. "auto" in the list
 * stands for every radio a broadcast discovery finds (in address order) that
 * the list does not name; they run at clock_ppm. When every radio
 * answers busy, or none is free within RADIO_SEARCH_S, it gives up — waiting
 * on a busy radio once left the skimmer silent for 15 h unnoticed
 * (2026-10-07: CW Skimmer Server took .21 when its own radio dropped out).
 *
 * One busy radio is ours to free: our own orphan. The server has no
 * watchdog, so a skimmer that dies without its stop (a crash) leaves the
 * radio streaming to a port of this machine that nothing listens on — busy
 * for good, and every restart gave up (hra2 2026-11-22/23: 11 h, until a
 * stop was sent by hand). Each stream we run is noted ("host port", a line,
 * <state>/hpsdr-streams) and its line goes when the client is freed; a busy
 * radio whose line is still there gets skim_hpsdr_stop_orphan() — the stop
 * goes out only when that radio's stream really comes to that port and no
 * socket of this machine holds it. Anyone else's stream goes to their own
 * address, so it is never touched. A stream lost while we run keeps its
 * line too: it may be ours still, only unread for a while (drop()). 
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "source.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <glib/gstdio.h>

#include "engine/hpsdr_p1.h"

/* How long the radios may stay silent (no discovery reply — the network not
 * up yet after a boot, a radio rebooting) before the search gives up. A busy
 * reply is final at once. */
#define RADIO_SEARCH_S 30

G_STATIC_ASSERT(SKIM_HD_MAX_BANDS <= SKIM_HPSDR_MAX_RX);

typedef struct {
  SkimIqSource *s;
  guint         band;
} Rx;

typedef struct {
  SkimIqSource     base;
  gboolean         take_over;
  SkimHpsdrClient *rp;
  gboolean         streaming, gave_up;
  gint64           search_since_us;    /* 0 = not searching                  */
  volatile gint    closed;             /* set by the client's thread         */
  guint            ticks;
  char             host[64];           /* the radio streaming now (or first) */
  double           ppm_now;
  char             state[160];
  Rx               rx[SKIM_HD_MAX_BANDS];
} P1;

static void p1_state(P1 *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void p1_state(P1 *p, const char *fmt, ...) {
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  if (strcmp(s, p->state) != 0) {              /* log changes, not repeats    */
    g_strlcpy(p->state, s, sizeof(p->state));
    g_message("radio: %s", s);
  }
}

/* The radio port: SKIM_HPSDR_DISCOVER's port when it names one (a gate's
 * fake radio), else HPSDR's 1024. */
static guint16 radio_port(void) {
  const char *env = g_getenv("SKIM_HPSDR_DISCOVER");
  const char *colon = env ? strrchr(env, ':') : NULL;
  const guint64 v = colon ? g_ascii_strtoull(colon + 1, NULL, 10) : 0;
  return v > 0 && v < 65536 ? (guint16)v : SKIM_HPSDR_DEFAULT_PORT;
}

/* ---- the streams we run (an orphan's trace) ---------------------------------- */

static char *streams_path(void) {
  return g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "hpsdr-streams", NULL);
}

/* host's noted local port, 0 = none. */
static guint16 streams_get(const char *host) {
  char *path = streams_path(), *text = NULL;
  guint16 port = 0;
  if (g_file_get_contents(path, &text, NULL, NULL)) {
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l && !port; l++) {
      char h[64];
      unsigned v;
      if (sscanf(*l, "%63s %u", h, &v) == 2 && strcmp(h, host) == 0 && v > 0 && v < 65536) {
        port = (guint16)v;
      }
    }
    g_strfreev(lines);
  }
  g_free(text);
  g_free(path);
  return port;
}

/* Note host's stream at port (0 = remove its line). */
static void streams_set(const char *host, guint16 port) {
  char *path = streams_path(), *text = NULL;
  GString *out = g_string_new(NULL);
  if (g_file_get_contents(path, &text, NULL, NULL)) {
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
      char h[64];
      if (!**l || (sscanf(*l, "%63s", h) == 1 && strcmp(h, host) == 0)) { continue; }
      g_string_append_printf(out, "%s\n", *l);
    }
    g_strfreev(lines);
  }
  if (port) { g_string_append_printf(out, "%s %u\n", host, port); }
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  GError *e = NULL;
  if (out->len ? !g_file_set_contents(path, out->str, (gssize)out->len, &e)
               : (g_unlink(path) != 0 && errno != ENOENT)) {
    g_warning("radio: %s: %s", path, e ? e->message : g_strerror(errno));
  }
  g_clear_error(&e);
  g_free(dir);
  g_free(text);
  g_free(path);
  g_string_free(out, TRUE);
}

/* The client is done with (freed): its line goes. */
static void free_client(P1 *p) {
  if (!p->rp) { return; }
  g_clear_pointer(&p->rp, skim_hpsdr_client_free);
  streams_set(p->host, 0);
}

/* host answered busy: TRUE when it was our own orphan, now stopped and
 * idle again. */
static gboolean stop_orphan(P1 *p, const char *host) {
  const guint16 port = streams_get(host);
  if (!port) { return FALSE; }
  GError *err = NULL;
  const gboolean stopped = skim_hpsdr_stop_orphan(host, radio_port(), port, 1500, &err);
  streams_set(host, 0);                          /* judged: never again       */
  if (!stopped) {
    g_message("radio: %s busy, not our orphan: %s", host, err->message);
    g_clear_error(&err);
    return FALSE;
  }
  g_message("radio: %s still streamed to port %u of this machine, which nothing "
            "listens on (our last run ended without its stop) — that orphan "
            "stream is stopped", host, port);
  for (int i = 0; i < 10; i++) {                 /* the server takes a moment  */
    g_usleep(200000);
    SkimHpsdrInfo info;
    if (skim_hpsdr_discover(host, radio_port(), 500, &info, NULL) && !info.busy) { return TRUE; }
  }
  p1_state(p, "%s: still busy after the orphan's stop", host);
  return FALSE;
}

static void rx_iq_cb(const float *iq, guint n, double rate, double centre, gpointer user) {
  Rx *rx = user;
  rx->s->iq_cb(rx->band, iq, n, rate, centre, rx->s->iq_user);
}

static void closed_cb(gpointer user) { g_atomic_int_set(&((P1 *)user)->closed, 1); }

/* Start streaming from host at ppm; FALSE (state set) when the start fails. */
static gboolean start_on(P1 *p, const char *host, double ppm, gboolean took) {
  const SkimHdConfig *c = p->base.cfg;
  GError *err = NULL;
  double centres[SKIM_HD_MAX_BANDS];
  p->take_over = FALSE;                         /* one-shot: the first start   */
  p->rp = skim_hpsdr_client_new(host, radio_port());
  for (guint b = 0; b < c->nb; b++) {
    centres[b] = c->band[b].centre_hz;
    skim_hpsdr_client_set_rx_iq_cb(p->rp, b, rx_iq_cb, &p->rx[b]);
  }
  skim_hpsdr_client_set_closed_cb(p->rp, closed_cb, p);
  g_atomic_int_set(&p->closed, 0);
  if (!skim_hpsdr_client_start_multi(p->rp, c->rate, centres, c->nb, ppm, took, &err)) {
    p1_state(p, "%s: start failed: %s", host, err->message);
    g_clear_error(&err);
    g_clear_pointer(&p->rp, skim_hpsdr_client_free);
    return FALSE;
  }
  g_strlcpy(p->host, host, sizeof(p->host));
  streams_set(host, skim_hpsdr_client_local_port(p->rp));
  p->ppm_now = ppm;
  p->streaming = TRUE;
  p->search_since_us = 0;
  p1_state(p, "streaming — %s, %u RX × %u kHz, clock %+.2f ppm%s",
           skim_hpsdr_client_device(p->rp), c->nb, c->rate / 1000, ppm,
           took ? " (taken over)" : "");
  return TRUE;
}

/* Where the broadcast of host=auto goes: SKIM_HPSDR_DISCOVER=addr[:port]
 * (a gate's fake radio), else the LAN of the default route. */
GArray *skim_iq_source_hpsdr_discover(void) {
  const char *env = g_getenv("SKIM_HPSDR_DISCOVER");
  char *addr = NULL;
  guint16 port = SKIM_HPSDR_DEFAULT_PORT;
  if (env && env[0]) {
    addr = g_strdup(env);
    char *colon = strrchr(addr, ':');
    if (colon) {
      port = (guint16)g_ascii_strtoull(colon + 1, NULL, 10);
      *colon = '\0';
    }
  }
  GError *err = NULL;
  GArray *found = skim_hpsdr_discover_all(addr, port, 1000, &err);
  if (!found) {
    g_warning("radio: discovery broadcast: %s", err->message);
    g_clear_error(&err);
    found = g_array_new(FALSE, TRUE, sizeof(SkimHpsdrFound));
  }
  g_free(addr);
  return found;
}

static gboolean listed(const SkimHdConfig *c, const char *host) {
  for (guint i = 0; c->hosts[i]; i++) {
    if (strcmp(c->hosts[i], host) == 0) { return TRUE; }
  }
  return FALSE;
}

typedef struct {
  GString *why;
  char     first_busy[64];
  double   busy_ppm;
  gboolean all_answered;
} Round;

/* One radio of the round: TRUE when it is streaming now. */
static gboolean try_one(P1 *p, Round *r, const char *host, double ppm, const SkimHpsdrInfo *info) {
  if (r->why->len) { g_string_append(r->why, ", "); }
  if (!info) {
    g_string_append_printf(r->why, "%s no reply", host);
    r->all_answered = FALSE;
  } else if (info->busy && stop_orphan(p, host) && start_on(p, host, ppm, FALSE)) {
    return TRUE;
  } else if (info->busy) {
    g_string_append_printf(r->why, "%s busy", host);
    if (!r->first_busy[0]) {
      g_strlcpy(r->first_busy, host, sizeof(r->first_busy));
      r->busy_ppm = ppm;
    }
  } else if (start_on(p, host, ppm, FALSE)) {
    return TRUE;
  } else {
    g_string_append_printf(r->why, "%s start failed", host);
    r->all_answered = FALSE;                      /* may work on the next round  */
  }
  return FALSE;
}

/* One search round over [radio] host: the first idle radio wins. None idle —
 * every one busy, or the rest silent past RADIO_SEARCH_S — gives up. */
static void try_start(P1 *p) {
  const SkimHdConfig *c = p->base.cfg;
  const gint64 now = g_get_monotonic_time();
  if (!p->search_since_us) { p->search_since_us = now; }
  const gboolean take = p->take_over;
  Round r = { g_string_new(NULL), "", 0, TRUE };
  gboolean on = FALSE;
  for (guint i = 0; c->hosts[i] && !on; i++) {
    if (strcmp(c->hosts[i], SKIM_HD_HOST_AUTO) == 0) {
      GArray *found = skim_iq_source_hpsdr_discover();
      guint tried = 0;
      for (guint k = 0; k < found->len && !on; k++) {
        const SkimHpsdrFound *f = &g_array_index(found, SkimHpsdrFound, k);
        if (listed(c, f->host)) { continue; }   /* tried (or to be) by name   */
        tried++;
        on = try_one(p, &r, f->host, c->host_ppm[i], &f->info);
      }
      if (!tried && !on) {
        if (r.why->len) { g_string_append(r.why, ", "); }
        g_string_append(r.why, "auto: no Red Pitaya answered on the LAN");
        r.all_answered = FALSE;
      }
      g_array_unref(found);
      continue;
    }
    SkimHpsdrInfo info;
    GError *err = NULL;
    const gboolean ans = skim_hpsdr_discover(c->hosts[i], radio_port(), 1000, &info, &err);
    g_clear_error(&err);
    on = try_one(p, &r, c->hosts[i], c->host_ppm[i], ans ? &info : NULL);
  }
  if (!on && take && r.first_busy[0]) { on = start_on(p, r.first_busy, r.busy_ppm, TRUE); }
  if (on) {
    g_string_free(r.why, TRUE);
    return;
  }
  if (r.all_answered || now - p->search_since_us >= RADIO_SEARCH_S * G_USEC_PER_SEC) {
    p1_state(p, "no free radio (%s) — stopping", r.why->str);
    g_printerr("skimmer-headless: no free radio (%s)%s\n", r.why->str,
               r.first_busy[0] ? " — another client streams from it; "
                                 "--take-over grabs the first busy one" : "");
    p->gave_up = TRUE;
  } else {
    p1_state(p, "searching for a free radio… (%s)", r.why->str);
  }
  g_string_free(r.why, TRUE);
}

static void drop(P1 *p) {
  /* The client saw no packet for its watchdog's 2 s and judged the radio
   * taken — but a stall of this process reads the same (hra2 2026-11-24
   * 07:37: a skimmer out of memory), and the radio streamed on to the port
   * we just closed: busy, our own orphan, and with its note gone every
   * restart gave up (exit 3, for 36 min). So the note stays and the search
   * judges it: stop_orphan() stops the stream only when it still comes to
   * that port; another client's goes to its own address, left alone. */
  const guint16 port = p->rp ? skim_hpsdr_client_local_port(p->rp) : 0;
  free_client(p);                                     /* sends no stop: not ours */
  if (port) { streams_set(p->host, port); }
  p->streaming = FALSE;
  p1_state(p, "stream lost from %s (another client took the radio, or it left "
              "the network) — searching %s", p->host, p->base.cfg->hosts_text);
}

static void p1_start(SkimIqSource *s) {
  P1 *p = (P1 *)s;
  try_start(p);
  p->ticks = 1;
}

static void p1_tick(SkimIqSource *s) {
  P1 *p = (P1 *)s;
  if (p->gave_up) { return; }
  if (p->streaming && g_atomic_int_get(&p->closed)) { drop(p); }
  if (!p->streaming && p->ticks % 3 == 0) { try_start(p); }
  p->ticks++;
}

static void p1_status(SkimIqSource *s, SkimIqSourceStatus *o) {
  P1 *p = (P1 *)s;
  memset(o, 0, sizeof(*o));
  o->streaming = p->streaming;
  o->gave_up = p->gave_up;
  g_strlcpy(o->state, p->state, sizeof(o->state));
  g_strlcpy(o->host, p->host, sizeof(o->host));
  o->ppm = p->ppm_now;
  o->packets = p->rp ? skim_hpsdr_client_packets(p->rp) : 0;
  o->lost_frames = p->rp ? skim_hpsdr_client_lost_frames(p->rp) : 0;
}

static void p1_free(SkimIqSource *s) {
  P1 *p = (P1 *)s;
  free_client(p);                                     /* stops — if still ours */
  g_free(p);
}

static const SkimIqSourceVtable P1_VT = { p1_start, p1_tick, p1_status, p1_free };

SkimIqSource *skim_iq_source_hpsdr_p1_new(const SkimHdConfig *cfg, gboolean take_over) {
  P1 *p = g_new0(P1, 1);
  p->base.vt = &P1_VT;
  p->take_over = take_over;
  g_strlcpy(p->host, cfg->hosts[0], sizeof(p->host));
  p->ppm_now = cfg->host_ppm[0];
  for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) {
    p->rx[b].s = &p->base;
    p->rx[b].band = b;
  }
  return &p->base;
}
