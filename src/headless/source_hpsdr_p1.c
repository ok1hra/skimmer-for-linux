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
 * The board's second IP (bands with ip=b, config.h): the skimmer's agent
 * serves DDC 8–15 on mvl0's address. That stream follows the first: once
 * IP A streams, IP B ([rp] b_host, else the address the agent's discovery
 * names) starts with the ip=b bands at [rp] b_rate, the same clock
 * correction — and is retried every few seconds while it is busy (another
 * client, never taken over unless take_over was given for this start) or
 * silent. Losing it never stops IP A, nor the skimmer. Each receiver's
 * input (in=2) goes in its stream's start (hpsdr_p1.h; the agent only).
 *
 * A phased band (ip=ab, docs/PHASING.md) takes a receiver on IP A (IN1) and
 * one on IP B (IN2), both streams started paired: the agent then resets
 * both FIFOs together and stamps each packet with an epoch and a sample
 * index. A pairer per band lines the two up by (epoch, index) and hands
 * them to iq2_cb as one block of A and one of B. While B is missing — not
 * streaming, no stamps (Pavel's server, an older agent), another epoch, or
 * late by more than PAIR_HOLD_S — A goes on alone (b = NULL): the band
 * never stops because of B.
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

G_STATIC_ASSERT(SKIM_HD_IP_RX <= SKIM_HPSDR_MAX_RX);
G_STATIC_ASSERT(SKIM_HD_MAX_BANDS <= 2 * SKIM_HD_IP_RX);

typedef struct {
  SkimIqSource *s;
  guint         band;
} Rx;

/* A phased band's pairer (one per band, both clients' threads feed it). */
#define PAIR_HOLD_S  0.25       /* A waits this long for B's same samples   */
#define PAIR_LIVE_US (1 * G_USEC_PER_SEC)   /* B's last block: B is there   */

typedef struct {
  SkimIqSource *s;
  guint         band;
  GMutex        lock;              /* the queues                            */
  GMutex        emit;              /* the order blocks go out in            */
  float        *q[2];              /* A's and B's frames not yet sent       */
  guint         len[2], cap;
  gint          ep[2];             /* each side's current epoch (-1 none)   */
  guint64       i0[2];             /* the index of q[s][0]                   */
  gint64        b_last_us;         /* B's last block (monotonic)            */
  double        rate, centre;
  guint64       paired, alone;     /* frames sent with B / without it       */
  gint64        alone_us;          /* the last time A went alone            */
} Pair;

typedef struct {
  Pair *pair;
  guint side;                      /* 0 = A (IN1), 1 = B (IN2)              */
} PairSide;

/* IP B: the board's second stream (ip=b bands). */
typedef struct {
  SkimHpsdrClient *cl;
  gboolean         streaming;
  volatile gint    closed;
  gboolean         take_over;          /* one-shot, as IP A's               */
  char             host[64];           /* where it streams (or was tried)    */
  char             peer[16];           /* the agent's word for it            */
  char             state[96];
} IpB;

typedef struct {
  SkimIqSource     base;
  gboolean         take_over;
  SkimHpsdrClient *rp;
  IpB              b;
  guint            nrx_ip[2];          /* bands on each IP                   */
  guint            band_of[2][SKIM_HD_IP_RX];  /* receiver → band            */
  guint8           inputs[2];          /* bit r = receiver r on IN2          */
  Pair            *pair[SKIM_HD_MAX_BANDS];   /* phased bands (ip=ab)        */
  PairSide         side[2][SKIM_HD_IP_RX];    /* receiver → its band's side  */
  gboolean         phased_rx[2][SKIM_HD_IP_RX];
  guint            nphased;
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

/* ---- phased bands: A and B lined up by the agent's (epoch, index) -------------- */

static Pair *pair_new(SkimIqSource *s, guint band, guint rate) {
  Pair *a = g_new0(Pair, 1);
  a->s = s;
  a->band = band;
  g_mutex_init(&a->lock);
  g_mutex_init(&a->emit);
  a->cap = MAX(rate, 48000u);                  /* a second each side          */
  for (guint k = 0; k < 2; k++) {
    a->q[k] = g_new(float, 2 * (gsize)a->cap);
    a->ep[k] = -1;
  }
  return a;
}

static void pair_free(Pair *a) {
  if (!a) { return; }
  g_free(a->q[0]);
  g_free(a->q[1]);
  g_mutex_clear(&a->lock);
  g_mutex_clear(&a->emit);
  g_free(a);
}

static void q_drop(Pair *a, guint k, guint n) {
  n = MIN(n, a->len[k]);
  memmove(a->q[k], a->q[k] + 2 * (gsize)n, 2 * (gsize)(a->len[k] - n) * sizeof(float));
  a->len[k] -= n;
  a->i0[k] += n;
}

/* Called with a->lock held: what can go out now — n frames of A, and of B
 * too when with_b — copied out; FALSE when nothing. */
static gboolean pair_take(Pair *a, float **oa, float **ob, guint *n) {
  const gboolean b_live = a->ep[1] >= 0 && a->ep[0] >= 0 &&
                          g_get_monotonic_time() - a->b_last_us < PAIR_LIVE_US;
  *ob = NULL;
  if (b_live && a->ep[0] == a->ep[1] && a->len[0] && a->len[1]) {
    /* the same epoch: what only A has at the front goes out alone (A, the
     * band's own input, never skips), what only B has is dropped */
    if (a->i0[0] < a->i0[1]) {
      const guint m = (guint)MIN(a->i0[1] - a->i0[0], (guint64)a->len[0]);
      *oa = g_memdup2(a->q[0], 2 * (gsize)m * sizeof(float));
      q_drop(a, 0, m);
      *n = m;
      a->alone += m;
      a->alone_us = g_get_monotonic_time();
      return TRUE;
    }
    if (a->i0[1] < a->i0[0]) { q_drop(a, 1, (guint)MIN(a->i0[0] - a->i0[1], (guint64)a->len[1])); }
    const guint m = MIN(a->len[0], a->len[1]);
    if (m && a->i0[0] == a->i0[1]) {
      *oa = g_memdup2(a->q[0], 2 * (gsize)m * sizeof(float));
      *ob = g_memdup2(a->q[1], 2 * (gsize)m * sizeof(float));
      q_drop(a, 0, m);
      q_drop(a, 1, m);
      *n = m;
      a->paired += m;
      return TRUE;
    }
  }
  /* B not there, or late past the hold: the front of A goes alone */
  const guint hold = b_live ? (guint)(PAIR_HOLD_S * a->rate) : 0;
  if (a->len[0] > hold) {
    const guint m = a->len[0] - hold;
    *oa = g_memdup2(a->q[0], 2 * (gsize)m * sizeof(float));
    q_drop(a, 0, m);
    *n = m;
    a->alone += m;
    a->alone_us = g_get_monotonic_time();
    return TRUE;
  }
  return FALSE;
}

/* A block of one side (any client's thread). */
static void pair_iq_cb(const float *iq, guint n, double rate, double centre, gint epoch,
                       guint64 idx, gpointer user) {
  PairSide *ps = user;
  Pair *a = ps->pair;
  const guint k = ps->side;
  g_mutex_lock(&a->lock);
  if (k == 0) {
    a->rate = rate;
    a->centre = centre;
  } else {
    a->b_last_us = g_get_monotonic_time();
  }
  /* a new epoch, or a gap the client could not fill: this side starts over */
  if (epoch != a->ep[k] || (a->len[k] && a->i0[k] + a->len[k] != idx)) {
    a->len[k] = 0;
    a->ep[k] = epoch;
  }
  if (a->len[k] == 0) { a->i0[k] = idx; }
  if (a->len[k] + n > a->cap) { q_drop(a, k, a->len[k] + n - a->cap); }   /* never more */
  n = MIN(n, a->cap);
  memcpy(a->q[k] + 2 * (gsize)a->len[k], iq, 2 * (gsize)n * sizeof(float));
  a->len[k] += n;
  for (;;) {
    float *oa = NULL, *ob = NULL;
    guint m = 0;
    if (!pair_take(a, &oa, &ob, &m)) { break; }
    g_mutex_lock(&a->emit);                    /* in order, outside the queues */
    g_mutex_unlock(&a->lock);
    SkimIqSource *s = a->s;
    if (s->iq2_cb) {
      s->iq2_cb(a->band, oa, ob, m, a->rate, a->centre, s->iq_user);
    } else {
      s->iq_cb(a->band, oa, m, a->rate, a->centre, s->iq_user);
    }
    g_mutex_unlock(&a->emit);
    g_free(oa);
    g_free(ob);
    g_mutex_lock(&a->lock);
  }
  g_mutex_unlock(&a->lock);
}

static void rx_iq_cb(const float *iq, guint n, double rate, double centre, gpointer user) {
  Rx *rx = user;
  rx->s->iq_cb(rx->band, iq, n, rate, centre, rx->s->iq_user);
}

static void closed_cb(gpointer user) { g_atomic_int_set(&((P1 *)user)->closed, 1); }
static void closed_b_cb(gpointer user) { g_atomic_int_set(&((P1 *)user)->b.closed, 1); }

/* A client for IP ip's bands: receivers, callbacks, inputs; centres out. */
static SkimHpsdrClient *client_for(P1 *p, guint ip, const char *host, double *centres) {
  const SkimHdConfig *c = p->base.cfg;
  SkimHpsdrClient *cl = skim_hpsdr_client_new(host, radio_port());
  for (guint r = 0; r < p->nrx_ip[ip]; r++) {
    const guint b = p->band_of[ip][r];
    centres[r] = c->band[b].centre_hz;
    if (p->phased_rx[ip][r]) {
      skim_hpsdr_client_set_rx_iq_ix_cb(cl, r, pair_iq_cb, &p->side[ip][r]);
    } else {
      skim_hpsdr_client_set_rx_iq_cb(cl, r, rx_iq_cb, &p->rx[b]);
    }
  }
  skim_hpsdr_client_set_inputs(cl, p->inputs[ip]);
  skim_hpsdr_client_set_paired(cl, p->nphased > 0);
  return cl;
}

/* Start streaming from host at ppm; FALSE (state set) when the start fails. */
static gboolean start_on(P1 *p, const char *host, double ppm, gboolean took) {
  const SkimHdConfig *c = p->base.cfg;
  GError *err = NULL;
  double centres[SKIM_HD_IP_RX];
  p->take_over = FALSE;                         /* one-shot: the first start   */
  p->rp = client_for(p, 0, host, centres);
  skim_hpsdr_client_set_closed_cb(p->rp, closed_cb, p);
  g_atomic_int_set(&p->closed, 0);
  if (!skim_hpsdr_client_start_multi(p->rp, c->rate, centres, p->nrx_ip[0], ppm, took, &err)) {
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
           skim_hpsdr_client_device(p->rp), p->nrx_ip[0], c->rate / 1000, ppm,
           took ? " (taken over)" : "");
  if (p->inputs[0] && !skim_hpsdr_client_agent(p->rp)) {
    g_warning("radio: %s runs no skimmer agent — in=2 bands take IN1 (Settings → Red Pitaya "
              "installs it)", host);
  }
  if (p->nrx_ip[1]) {                           /* the board names its second IP */
    SkimHpsdrInfo info;
    if (skim_hpsdr_discover(host, radio_port(), 500, &info, NULL) && info.agent) {
      g_strlcpy(p->b.peer, info.agent_peer, sizeof(p->b.peer));
    }
  }
  return TRUE;
}

/* ---- IP B ---------------------------------------------------------------------- */

static void b_state(P1 *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void b_state(P1 *p, const char *fmt, ...) {
  char s[96];
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  if (strcmp(s, p->b.state) != 0) {
    g_strlcpy(p->b.state, s, sizeof(p->b.state));
    g_message("radio: IP B %s", s);
  }
}

static void b_free(P1 *p) {
  if (!p->b.cl) { return; }
  g_clear_pointer(&p->b.cl, skim_hpsdr_client_free);
  streams_set(p->b.host, 0);
}

/* IP B's stream lost: as drop() — its note stays for the orphan check. */
static void b_drop(P1 *p) {
  const guint16 port = skim_hpsdr_client_local_port(p->b.cl);
  b_free(p);
  if (port) { streams_set(p->b.host, port); }
  p->b.streaming = FALSE;
  b_state(p, "%s: stream lost — trying again", p->b.host);
}

static void b_try(P1 *p) {
  const SkimHdConfig *c = p->base.cfg;
  if (!p->nrx_ip[1] || p->b.streaming || !p->streaming) { return; }
  if (!c->rp_b_host && !p->b.peer[0]) {        /* the agent may not have had it yet (DHCP) */
    SkimHpsdrInfo ia;
    if (skim_hpsdr_discover(p->host, radio_port(), 500, &ia, NULL) && ia.agent) {
      g_strlcpy(p->b.peer, ia.agent_peer, sizeof(p->b.peer));
    }
  }
  const char *host = c->rp_b_host ? c->rp_b_host : p->b.peer[0] ? p->b.peer : NULL;
  if (!host) {
    b_state(p, "has no address: the board runs no skimmer agent to name it — set [rp] b_host");
    return;
  }
  g_strlcpy(p->b.host, host, sizeof(p->b.host));
  SkimHpsdrInfo info;
  if (!skim_hpsdr_discover(host, radio_port(), 500, &info, NULL)) {
    b_state(p, "%s: no reply", host);
    return;
  }
  const gboolean took = info.busy && p->b.take_over;
  if (info.busy && !took && !stop_orphan(p, host)) {
    b_state(p, "%s: busy (another client) — trying again", host);
    return;
  }
  p->b.take_over = FALSE;
  double centres[SKIM_HD_IP_RX];
  p->b.cl = client_for(p, 1, host, centres);
  skim_hpsdr_client_set_closed_cb(p->b.cl, closed_b_cb, p);
  g_atomic_int_set(&p->b.closed, 0);
  GError *err = NULL;
  if (!skim_hpsdr_client_start_multi(p->b.cl, c->rp_b_rate, centres, p->nrx_ip[1], p->ppm_now,
                                     took, &err)) {
    b_state(p, "%s: start failed: %s", host, err->message);
    g_clear_error(&err);
    g_clear_pointer(&p->b.cl, skim_hpsdr_client_free);
    return;
  }
  streams_set(host, skim_hpsdr_client_local_port(p->b.cl));
  p->b.streaming = TRUE;
  b_state(p, "streaming — %s, %u RX × %u kHz%s", skim_hpsdr_client_device(p->b.cl),
          p->nrx_ip[1], c->rp_b_rate / 1000, took ? " (taken over)" : "");
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
  b_try(p);
  p->ticks = 1;
}

static void p1_tick(SkimIqSource *s) {
  P1 *p = (P1 *)s;
  if (p->gave_up) { return; }
  if (p->streaming && g_atomic_int_get(&p->closed)) { drop(p); }
  if (!p->streaming && p->ticks % 3 == 0) { try_start(p); }
  if (p->b.streaming && g_atomic_int_get(&p->b.closed)) { b_drop(p); }
  if (!p->b.streaming && p->ticks % 3 == 0) { b_try(p); }
  p->ticks++;
}

static void p1_status(SkimIqSource *s, SkimIqSourceStatus *o) {
  P1 *p = (P1 *)s;
  memset(o, 0, sizeof(*o));
  o->streaming = p->streaming;
  o->gave_up = p->gave_up;
  if (p->nrx_ip[1] && p->b.state[0]) {
    g_snprintf(o->state, sizeof(o->state), "%s; IP B %s", p->state, p->b.state);
  } else {
    g_strlcpy(o->state, p->state, sizeof(o->state));
  }
  /* a phased band whose B went missing in the last seconds */
  for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) {
    Pair *a = p->pair[b];
    if (!a) { continue; }
    g_mutex_lock(&a->lock);
    const gboolean alone = a->alone_us && g_get_monotonic_time() - a->alone_us < 3 * G_USEC_PER_SEC;
    const gboolean stamps = a->ep[0] >= 0;
    g_mutex_unlock(&a->lock);
    if (alone && strlen(o->state) + 64 < sizeof(o->state)) {
      g_strlcat(o->state, "; ", sizeof(o->state));
      g_strlcat(o->state, p->base.cfg->band[b].name, sizeof(o->state));
      g_strlcat(o->state, !stamps ? " phased: IN1 alone (no agent 0.4 stamps)"
                          : p->base.cfg->band[b].same_ip ? " phased: IN1 alone (IN2 missing)"
                                                         : " phased: IN1 alone (IP B missing)",
                sizeof(o->state));
    }
  }
  g_strlcpy(o->host, p->host, sizeof(o->host));
  o->ppm = p->ppm_now;
  o->packets = p->rp ? skim_hpsdr_client_packets(p->rp) : 0;
  o->lost_frames = p->rp ? skim_hpsdr_client_lost_frames(p->rp) : 0;
  if (p->b.cl) {
    o->packets += skim_hpsdr_client_packets(p->b.cl);
    o->lost_frames += skim_hpsdr_client_lost_frames(p->b.cl);
  }
}

static void p1_free(SkimIqSource *s) {
  P1 *p = (P1 *)s;
  b_free(p);                                          /* stops — if still ours */
  free_client(p);
  for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) { pair_free(p->pair[b]); }
  g_free(p);
}

static const SkimIqSourceVtable P1_VT = { p1_start, p1_tick, p1_status, p1_free };

SkimIqSource *skim_iq_source_hpsdr_p1_new(const SkimHdConfig *cfg, gboolean take_over) {
  P1 *p = g_new0(P1, 1);
  p->base.vt = &P1_VT;
  p->take_over = take_over;
  p->b.take_over = take_over;
  g_strlcpy(p->host, cfg->hosts[0], sizeof(p->host));
  p->ppm_now = cfg->host_ppm[0];
  for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) {
    p->rx[b].s = &p->base;
    p->rx[b].band = b;
  }
  for (guint b = 0; b < cfg->nb; b++) {        /* each IP's receivers, in band order */
    const guint ip = cfg->band[b].ip ? 1 : 0;
    const guint r = p->nrx_ip[ip]++;
    p->band_of[ip][r] = b;
    if (cfg->band[b].input) { p->inputs[ip] |= (guint8)(1u << r); }
  }
  for (guint b = 0; b < cfg->nb; b++) {        /* phased: the IN2 receivers after the IPs' own */
    if (!cfg->band[b].phased) { continue; }
    p->pair[b] = pair_new(&p->base, b, cfg->band[b].rate);
    p->nphased++;
    const guint ipa = cfg->band[b].same_ip && cfg->band[b].ip ? 1 : 0;
    for (guint r = 0; r < p->nrx_ip[ipa]; r++) {
      if (p->band_of[ipa][r] == b) {
        p->phased_rx[ipa][r] = TRUE;
        p->side[ipa][r] = (PairSide){ p->pair[b], 0 };
      }
    }
    /* in=12: the IN2 receiver on the same IP (one stream: aligned by
     * itself); ip=ab: on IP B */
    const guint ipb = cfg->band[b].same_ip ? ipa : 1;
    const guint rb = p->nrx_ip[ipb]++;
    p->band_of[ipb][rb] = b;
    p->inputs[ipb] |= (guint8)(1u << rb);        /* IN2 */
    p->phased_rx[ipb][rb] = TRUE;
    p->side[ipb][rb] = (PairSide){ p->pair[b], 1 };
  }
  return &p->base;
}
