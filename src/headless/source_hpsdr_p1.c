/* source_hpsdr_p1.c — a Red Pitaya running sdr_receiver_hpsdr (HPSDR
 * Protocol 1): one receiver per band, hardware DDCs, the RF-inverted wire
 * conjugated by the engine's client (docs/HPSDR-P1.md).
 *
 * The radio is never taken from another client unless take_over is given
 * (one-shot: for the first start only — it then takes the first busy radio
 * of the list). The source streams from the first IDLE radio of [radio]
 * host, at start and again whenever the stream is lost. When every radio
 * answers busy, or none is free within RADIO_SEARCH_S, it gives up — waiting
 * on a busy radio once left the skimmer silent for 15 h unnoticed
 * (2026-10-07: CW Skimmer Server took .21 when its own radio dropped out).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "source.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
  const char      *host;               /* the radio streaming now (or first) */
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

static void rx_iq_cb(const float *iq, guint n, double rate, double centre, gpointer user) {
  Rx *rx = user;
  rx->s->iq_cb(rx->band, iq, n, rate, centre, rx->s->iq_user);
}

static void closed_cb(gpointer user) { g_atomic_int_set(&((P1 *)user)->closed, 1); }

/* Start streaming from hosts[i] at its clock_ppm; FALSE (state set) when the
 * start fails. */
static gboolean start_on(P1 *p, guint i, gboolean took) {
  const SkimHdConfig *c = p->base.cfg;
  const char *host = c->hosts[i];
  const double ppm = c->host_ppm[i];
  GError *err = NULL;
  double centres[SKIM_HD_MAX_BANDS];
  p->take_over = FALSE;                         /* one-shot: the first start   */
  p->rp = skim_hpsdr_client_new(host, SKIM_HPSDR_DEFAULT_PORT);
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
  p->host = host;
  p->ppm_now = ppm;
  p->streaming = TRUE;
  p->search_since_us = 0;
  p1_state(p, "streaming — %s, %u RX × %u kHz, clock %+.2f ppm%s",
           skim_hpsdr_client_device(p->rp), c->nb, c->rate / 1000, ppm,
           took ? " (taken over)" : "");
  return TRUE;
}

/* One search round over [radio] host: the first idle radio wins. None idle —
 * every one busy, or the rest silent past RADIO_SEARCH_S — gives up. */
static void try_start(P1 *p) {
  const SkimHdConfig *c = p->base.cfg;
  const gint64 now = g_get_monotonic_time();
  if (!p->search_since_us) { p->search_since_us = now; }
  const gboolean take = p->take_over;
  GString *why = g_string_new(NULL);
  int first_busy = -1;
  gboolean all_answered = TRUE;
  for (guint i = 0; c->hosts[i]; i++) {
    const char *hp = c->hosts[i];
    SkimHpsdrInfo info;
    GError *err = NULL;
    if (why->len) { g_string_append(why, ", "); }
    if (!skim_hpsdr_discover(hp, SKIM_HPSDR_DEFAULT_PORT, 1000, &info, &err)) {
      g_string_append_printf(why, "%s no reply", hp);
      g_clear_error(&err);
      all_answered = FALSE;
    } else if (info.busy) {
      g_string_append_printf(why, "%s busy", hp);
      if (first_busy < 0) { first_busy = (int)i; }
    } else if (start_on(p, i, FALSE)) {
      g_string_free(why, TRUE);
      return;
    } else {
      g_string_append_printf(why, "%s start failed", hp);
      all_answered = FALSE;                     /* may work on the next round  */
    }
  }
  if (take && first_busy >= 0 && start_on(p, (guint)first_busy, TRUE)) {
    g_string_free(why, TRUE);
    return;
  }
  if (all_answered || now - p->search_since_us >= RADIO_SEARCH_S * G_USEC_PER_SEC) {
    p1_state(p, "no free radio (%s) — stopping", why->str);
    g_printerr("skimmer-headless: no free radio (%s)%s\n", why->str,
               first_busy >= 0 ? " — another client streams from it; "
                                 "--take-over grabs the first busy one" : "");
    p->gave_up = TRUE;
  } else {
    p1_state(p, "searching for a free radio… (%s)", why->str);
  }
  g_string_free(why, TRUE);
}

static void drop(P1 *p) {
  g_clear_pointer(&p->rp, skim_hpsdr_client_free);   /* sends no stop: not ours */
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
  g_clear_pointer(&p->rp, skim_hpsdr_client_free);   /* stops — if still ours */
  g_free(p);
}

static const SkimIqSourceVtable P1_VT = { p1_start, p1_tick, p1_status, p1_free };

SkimIqSource *skim_iq_source_hpsdr_p1_new(const SkimHdConfig *cfg, gboolean take_over) {
  P1 *p = g_new0(P1, 1);
  p->base.vt = &P1_VT;
  p->take_over = take_over;
  p->host = cfg->hosts[0];
  p->ppm_now = cfg->host_ppm[0];
  for (guint b = 0; b < SKIM_HD_MAX_BANDS; b++) {
    p->rx[b].s = &p->base;
    p->rx[b].band = b;
  }
  return &p->base;
}
