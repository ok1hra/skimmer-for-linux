/*
 * skimmer-headless — multi-band skimmer without a GUI.
 *
 *   skimmer-headless [--config FILE] [--take-over] [--status-every S]
 *                    [--http-port N]
 *
 * One HPSDR Protocol 1 receiver (Red Pitaya sdr_receiver_hpsdr) streams up
 * to 8 receivers at once; each receiver feeds its own engine pipeline (one
 * band each, SOURCE_EXTERNAL). Every pipeline's validated CQ spots go to ONE
 * shared telnet feed (cluster dialect — the CW Skimmer Server model). Status:
 * a table on stdout every few seconds and a small web page (+ /status.json).
 *
 * Config (default ~/.config/skimmer-for-linux/headless.ini, written with
 * defaults when missing):
 *   [radio]  host — one radio or a list (comma separated, tried in order),
 *            each optionally host@ppm: that radio's own clock error;
 *            rate (48000/96000/192000 — one rate for all receivers),
 *            clock_ppm (the radios without @ppm)
 *   [bands]  <name>=<centre Hz>, in receiver order (RX1 first), ≤ 8
 *   [decode] engine=v2|deepcw, log=true|false (per-band decode logs)
 *   [feed]   call, port (0 = no telnet feed)
 *   [status] console_s (0 = quiet), http_port (0 = no web page)
 *
 * The radio is never taken from another client unless --take-over is given
 * (one-shot: for the first start only — it then takes the first busy radio of
 * the list). The scanner streams from the first IDLE radio of the list, at
 * start and again whenever the stream is lost. When every radio answers busy,
 * or none is free within RADIO_SEARCH_S, it reports which and exits (3) —
 * waiting on a busy radio once left it silent for 15 h unnoticed
 * (2026-10-07: CW Skimmer Server took .21 when its own radio dropped out).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app/scp_update.h"
#include "engine/callsign.h"
#include "engine/hpsdr_p1.h"
#include "engine/pipeline.h"
#include "engine/rbn_feed.h"

/* "Active decoder": a channel decoding running text, not noise. The decoders
 * emit a one-character fragment on noise now and then ("·", "E", "I" —
 * 160 m at night: ~0.1–0.2 per second per channel); keyed CW gives 2–3
 * characters a second. A per-channel character score decaying with
 * ACTIVE_TAU_S, active at ACTIVE_SCORE, tells the two apart. */
#define ACTIVE_TAU_S     10.0
#define ACTIVE_SCORE     8.0

typedef struct {
  gint64 last_us;
  double score;              /* characters, decayed to last_us              */
} Act;

static double act_now(const Act *a, gint64 now) {
  return a->score * exp(-(double)(now - a->last_us) / (ACTIVE_TAU_S * G_USEC_PER_SEC));
}
#define RECENT_STATIONS  10                      /* per band on the web page */

/* How long the radios may stay silent (no discovery reply — the network not
 * up yet after a boot, a radio rebooting) before the search gives up. A busy
 * reply is final at once. */
#define RADIO_SEARCH_S   30
#define EXIT_NO_RADIO    3

typedef struct {
  char          name[16];
  double        centre_hz;
  char          label[96];
  SkimPipeline *p;
  GMutex        lock;          /* engine-thread callbacks ↔ main loop         */
  GHashTable   *active;        /* channel key → last text µs (gint64 *)       */
  GHashTable   *stations;      /* call → SkimStation copy                     */
  guint64       frames_prev;   /* main loop: for kS/s                         */
  double        ksps;
} Band;

typedef struct {
  /* config */
  char    *cfg_path;
  char   **hosts;              /* [radio] host, in search order               */
  double  *host_ppm;           /* each radio's clock_ppm (host@ppm)           */
  const char *host;            /* the radio streaming now (or hosts[0])       */
  double   ppm_now;            /* … and its clock correction                  */
  char    *hosts_text;         /* "a, b" for messages                         */
  guint    rate;
  double   ppm;
  char    *engine;
  gboolean decode_log;
  char    *feed_call;
  int      feed_port;
  double   feed_min_score;     /* 0 = the pipeline's default                  */
  guint    feed_min_hearings;  /* 0 = default; 1 = no hearings gate          */
  double   feed_settle_s;      /* 0 = default; < 0 = send at once            */
  double   feed_fresh_s;       /* 0 = default (off); < 0 = off               */
  gboolean feed_learn;         /* decode tap (gatelog.h)                      */
  char    *feed_gate;          /* learned gate ini, shadow (feed_gate.h)      */
  int      console_s;
  int      http_port;
  gboolean take_over;          /* --take-over: the first start only           */

  Band     band[SKIM_HPSDR_MAX_RX];
  guint    nb;

  SkimHpsdrClient *rp;
  gboolean  streaming;
  gint64    search_since_us;   /* 0 = not searching                           */
  int       exit_code;         /* != 0: stop the main loop with it            */
  volatile gint closed;        /* set by the client's thread                 */
  char      rp_state[160];
  guint64   packets_prev;
  double    pps;
  SkimRbnFeed    *feed;
  SkimScpUpdater *scp;
  GMainLoop      *loop;
  gint64          t_start;
  guint64         cpu_prev;    /* clock ticks, utime + stime                  */
  gint64          cpu_prev_us;
  double          cpu_pct;
  guint           ticks;

  GMutex   snap_lock;          /* the web page's copy of the status           */
  char    *snap_html;
  char    *snap_json;
} Hd;

/* ---- config --------------------------------------------------------------------- */

static const char *DEFAULT_CONFIG =
  "# skimmer-headless — one Red Pitaya, one pipeline per band.\n"
  "# Bands are receivers in order (RX1 first), at most 8, all at one rate.\n"
  "# 6 x 96 kHz is ~30 Mb/s of UDP: use a wired link.\n"
  "\n[radio]\n# one radio, or a list tried in order: the first idle one is used;\n"
  "# host@ppm gives that radio its own clock_ppm (192.168.1.21@3.81,192.168.1.71)\n"
  "host=192.168.1.21\nrate=96000\n"
  "# sampling-clock error: (true - shown) / true * 1e6, measured with 0 here;\n"
  "# for the radios without @ppm\n"
  "clock_ppm=0\n"
  "\n[bands]\n"
  "# name=centre Hz; with 96 kHz each band spans centre +-48 kHz\n"
  "160m=1840000\n80m=3540000\n40m=7040000\n30m=10125000\n20m=14040000\n"
  "15m=21040000\n"
  "\n[decode]\n# v2 (classical) or deepcw (needs ONNX Runtime + model)\n"
  "engine=v2\n# per-band raw decode logs in ~/.local/share/skimmer-for-linux/headless\n"
  "log=false\n"
  "\n[feed]\n# telnet spot feed (validated CQ spots only); port 0 = off\n"
  "call=\nport=7300\n"
  "# a call is spotted once it scored min_score, was read min_hearings times\n"
  "# (or MASTER.SCP knows it) and stayed settle_s seconds in the station table;\n"
  "# min_hearings=1 and settle_s=0 switch those two off\n"
  "#min_score=0.85\n#min_hearings=2\n#settle_s=8\n"
  "# fresh_s=N also wants the call READ within the last N s: a stale candidate\n"
  "# on a quiet channel is not a station (off unless set; 120 = the station TTL)\n"
  "#fresh_s=120\n"
  "# learn=true records what the feed gate saw, for learning a better one: per\n"
  "# band a decode tap in ~/.local/share/skimmer-for-linux/headless/learn —\n"
  "# skimmer-tap-replay turns it into the gate's rows (~80 MB/day/band raw)\n"
  "#learn=false\n"
  "# gate=FILE scores every spot with the learned feed gate (skimmer-compare\n"
  "# gate/learn.py) — SHADOW: reported in the status, the hand gate decides\n"
  "#gate=feed-gate.ini\n"
  "\n[status]\n# console table every N s (0 = off); web page port (0 = off)\n"
  "console_s=10\nhttp_port=8073\n";

static gboolean config_load(Hd *h, GError **error) {
  if (!g_file_test(h->cfg_path, G_FILE_TEST_EXISTS)) {
    char *dir = g_path_get_dirname(h->cfg_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    if (!g_file_set_contents(h->cfg_path, DEFAULT_CONFIG, -1, error)) { return FALSE; }
    g_message("config: %s did not exist — written with defaults", h->cfg_path);
  }
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, h->cfg_path, G_KEY_FILE_NONE, error)) {
    g_key_file_free(kf);
    return FALSE;
  }
  h->ppm = g_key_file_has_key(kf, "radio", "clock_ppm", NULL)
               ? g_key_file_get_double(kf, "radio", "clock_ppm", NULL) : 0;
  /* host[@ppm], …: each radio has its own sampling clock */
  char *hv = g_key_file_get_string(kf, "radio", "host", NULL);
  GPtrArray *hl = g_ptr_array_new();
  GArray *pl = g_array_new(FALSE, FALSE, sizeof(double));
  char **parts = g_strsplit_set(hv ? hv : "", ",; \t", -1);
  gboolean bad = FALSE;
  for (char **q = parts; *q && !bad; q++) {
    if (!**q) { continue; }
    char *at = strchr(*q, '@');
    double ppm = h->ppm;
    if (at) {
      char *end = NULL;
      ppm = g_ascii_strtod(at + 1, &end);
      if (end == at + 1 || *end || at == *q) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "%s: [radio] host \"%s\" (host or host@ppm)", h->cfg_path, *q);
        bad = TRUE;
        break;
      }
      *at = '\0';
    }
    g_ptr_array_add(hl, g_strdup(*q));
    g_array_append_val(pl, ppm);
  }
  g_strfreev(parts);
  g_free(hv);
  if (bad) {
    g_ptr_array_free(hl, TRUE);
    g_array_free(pl, TRUE);
    g_key_file_free(kf);
    return FALSE;
  }
  if (hl->len == 0) {
    g_ptr_array_add(hl, g_strdup("192.168.1.21"));
    g_array_append_val(pl, h->ppm);
  }
  g_ptr_array_add(hl, NULL);
  h->hosts = (char **)g_ptr_array_free(hl, FALSE);
  h->host_ppm = (double *)g_array_free(pl, FALSE);
  h->host = h->hosts[0];
  h->ppm_now = h->host_ppm[0];
  h->hosts_text = g_strjoinv(", ", h->hosts);
  h->rate = 96000;
  if (g_key_file_has_key(kf, "radio", "rate", NULL)) {
    h->rate = (guint)g_key_file_get_integer(kf, "radio", "rate", NULL);
  }
  h->engine = g_key_file_get_string(kf, "decode", "engine", NULL);
  h->decode_log = g_key_file_has_key(kf, "decode", "log", NULL) &&
                  g_key_file_get_boolean(kf, "decode", "log", NULL);
  h->feed_call = g_key_file_get_string(kf, "feed", "call", NULL);
  h->feed_port = g_key_file_has_key(kf, "feed", "port", NULL)
                     ? g_key_file_get_integer(kf, "feed", "port", NULL) : 7300;
  h->feed_min_score = g_key_file_has_key(kf, "feed", "min_score", NULL)
                          ? g_key_file_get_double(kf, "feed", "min_score", NULL) : 0;
  h->feed_min_hearings = g_key_file_has_key(kf, "feed", "min_hearings", NULL)
      ? (guint)MAX(1, g_key_file_get_integer(kf, "feed", "min_hearings", NULL)) : 0;
  h->feed_settle_s = 0;
  if (g_key_file_has_key(kf, "feed", "settle_s", NULL)) {
    const double v = g_key_file_get_double(kf, "feed", "settle_s", NULL);
    h->feed_settle_s = v > 0 ? v : -1;          /* 0 in the file = off        */
  }
  h->feed_fresh_s = 0;
  if (g_key_file_has_key(kf, "feed", "fresh_s", NULL)) {
    const double v = g_key_file_get_double(kf, "feed", "fresh_s", NULL);
    h->feed_fresh_s = v > 0 ? v : -1;           /* 0 in the file = off        */
  }
  h->feed_learn = g_key_file_has_key(kf, "feed", "learn", NULL) &&
                  g_key_file_get_boolean(kf, "feed", "learn", NULL);
  g_clear_pointer(&h->feed_gate, g_free);
  char *gate = g_key_file_get_string(kf, "feed", "gate", NULL);
  if (gate && gate[0]) {
    char *dir = g_path_get_dirname(h->cfg_path);   /* relative: next to the ini */
    char *x = gate[0] == '~' ? g_build_filename(g_get_home_dir(), gate + 1, NULL)
                             : g_strdup(gate);
    h->feed_gate = g_path_is_absolute(x) ? g_strdup(x) : g_build_filename(dir, x, NULL);
    g_free(x);
    g_free(dir);
  }
  g_free(gate);
  if (h->console_s < 0) {
    h->console_s = g_key_file_has_key(kf, "status", "console_s", NULL)
                       ? g_key_file_get_integer(kf, "status", "console_s", NULL) : 10;
  }
  if (h->http_port < 0) {
    h->http_port = g_key_file_has_key(kf, "status", "http_port", NULL)
                       ? g_key_file_get_integer(kf, "status", "http_port", NULL) : 8073;
  }

  gsize n = 0;
  char **keys = g_key_file_get_keys(kf, "bands", &n, NULL);   /* file order */
  for (gsize i = 0; keys && i < n; i++) {
    const double hz = g_key_file_get_double(kf, "bands", keys[i], NULL);
    if (hz < 100000 || hz > 62000000) {
      g_warning("config: band %s=%s ignored (not a centre in Hz)", keys[i],
                g_key_file_get_value(kf, "bands", keys[i], NULL));
      continue;
    }
    if (h->nb == SKIM_HPSDR_MAX_RX) {
      g_warning("config: band %s ignored — %d receivers at most", keys[i],
                SKIM_HPSDR_MAX_RX);
      continue;
    }
    Band *b = &h->band[h->nb++];
    g_strlcpy(b->name, keys[i], sizeof(b->name));
    b->centre_hz = hz;
  }
  g_strfreev(keys);
  g_key_file_free(kf);
  if (h->nb == 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s: no [bands] entries", h->cfg_path);
    return FALSE;
  }
  if (h->rate != 48000 && h->rate != 96000 && h->rate != 192000) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "%s: rate %u (48000, 96000 or 192000)", h->cfg_path, h->rate);
    return FALSE;
  }
  return TRUE;
}

/* ---- pipeline callbacks (engine threads) ----------------------------------------- */

static void text_cb(double freq_hz, const char *text, gpointer user) {
  Band *b = user;
  guint chars = 0;
  for (const char *c = text; *c; c++) {
    if (g_ascii_isalnum(*c) || *c == '/' || *c == '?') { chars++; }
  }
  if (!chars) { return; }
  const gint64 now = g_get_monotonic_time();
  const gpointer key = GINT_TO_POINTER((gint)(freq_hz / 125.0 + 0.5));
  g_mutex_lock(&b->lock);
  Act *a = g_hash_table_lookup(b->active, key);
  if (!a) {
    a = g_new0(Act, 1);
    a->last_us = now;
    g_hash_table_insert(b->active, key, a);
  }
  a->score = act_now(a, now) + chars;
  a->last_us = now;
  g_mutex_unlock(&b->lock);
}

static void station_cb(const SkimStation *st, gpointer user) {
  Band *b = user;
  g_mutex_lock(&b->lock);
  SkimStation *copy = g_hash_table_lookup(b->stations, st->call);
  if (!copy) {
    copy = g_new0(SkimStation, 1);
    g_hash_table_insert(b->stations, g_strdup(st->call), copy);
  }
  *copy = *st;
  g_mutex_unlock(&b->lock);
}

static void gone_cb(const SkimStation *st, gpointer user) {
  Band *b = user;
  g_mutex_lock(&b->lock);
  g_hash_table_remove(b->stations, st->call);
  g_mutex_unlock(&b->lock);
}

static void state_cb(gboolean connected, const char *detail, gpointer user) {
  Band *b = user;
  if (connected && detail && strstr(detail, "channels")) {
    g_message("%s: %s", b->name, detail);      /* the bank got sized          */
  }
}

/* ---- the radio link -------------------------------------------------------------- */

static void rx_iq_cb(const float *iq, guint n, double rate, double centre, gpointer user) {
  skim_pipeline_push(((Band *)user)->p, iq, n, rate, centre);
}

static void rp_closed_cb(gpointer user) { g_atomic_int_set(&((Hd *)user)->closed, 1); }

static void rp_state(Hd *h, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void rp_state(Hd *h, const char *fmt, ...) {
  char s[160];
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(s, sizeof(s), fmt, ap);
  va_end(ap);
  if (strcmp(s, h->rp_state) != 0) {           /* log changes, not repeats    */
    g_strlcpy(h->rp_state, s, sizeof(h->rp_state));
    g_message("radio: %s", s);
  }
}

/* Start streaming from hosts[i] at its clock_ppm; FALSE (state set) when the
 * start fails. */
static gboolean rp_start_on(Hd *h, guint i, gboolean took) {
  const char *host = h->hosts[i];
  const double ppm = h->host_ppm[i];
  GError *err = NULL;
  double centres[SKIM_HPSDR_MAX_RX];
  h->take_over = FALSE;                         /* one-shot: the first start   */
  h->rp = skim_hpsdr_client_new(host, SKIM_HPSDR_DEFAULT_PORT);
  for (guint b = 0; b < h->nb; b++) {
    centres[b] = h->band[b].centre_hz;
    skim_hpsdr_client_set_rx_iq_cb(h->rp, b, rx_iq_cb, &h->band[b]);
  }
  skim_hpsdr_client_set_closed_cb(h->rp, rp_closed_cb, h);
  g_atomic_int_set(&h->closed, 0);
  if (!skim_hpsdr_client_start_multi(h->rp, h->rate, centres, h->nb, ppm, took,
                                     &err)) {
    rp_state(h, "%s: start failed: %s", host, err->message);
    g_clear_error(&err);
    g_clear_pointer(&h->rp, skim_hpsdr_client_free);
    return FALSE;
  }
  h->host = host;
  h->ppm_now = ppm;
  h->streaming = TRUE;
  h->packets_prev = 0;
  h->search_since_us = 0;
  rp_state(h, "streaming — %s, %u RX × %u kHz, clock %+.2f ppm%s",
           skim_hpsdr_client_device(h->rp), h->nb, h->rate / 1000, ppm,
           took ? " (taken over)" : "");
  return TRUE;
}

/* One search round over [radio] host: the first idle radio wins. None idle —
 * every one busy, or the rest silent past RADIO_SEARCH_S — stops the scanner
 * with EXIT_NO_RADIO. */
static void rp_try_start(Hd *h) {
  const gint64 now = g_get_monotonic_time();
  if (!h->search_since_us) { h->search_since_us = now; }
  const gboolean take = h->take_over;
  GString *why = g_string_new(NULL);
  int first_busy = -1;
  gboolean all_answered = TRUE;
  for (guint i = 0; h->hosts[i]; i++) {
    const char *hp = h->hosts[i];
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
    } else if (rp_start_on(h, i, FALSE)) {
      g_string_free(why, TRUE);
      return;
    } else {
      g_string_append_printf(why, "%s start failed", hp);
      all_answered = FALSE;                     /* may work on the next round  */
    }
  }
  if (take && first_busy >= 0 && rp_start_on(h, (guint)first_busy, TRUE)) {
    g_string_free(why, TRUE);
    return;
  }
  if (all_answered || now - h->search_since_us >= RADIO_SEARCH_S * G_USEC_PER_SEC) {
    rp_state(h, "no free radio (%s) — stopping", why->str);
    g_printerr("skimmer-headless: no free radio (%s)%s\n", why->str,
               first_busy >= 0 ? " — another client streams from it; "
                            "--take-over grabs the first busy one" : "");
    h->exit_code = EXIT_NO_RADIO;
    if (h->loop) { g_main_loop_quit(h->loop); }
  } else {
    rp_state(h, "searching for a free radio… (%s)", why->str);
  }
  g_string_free(why, TRUE);
}

static void rp_drop(Hd *h) {
  g_clear_pointer(&h->rp, skim_hpsdr_client_free);   /* sends no stop: not ours */
  h->streaming = FALSE;
  rp_state(h, "stream lost from %s (another client took the radio, or it left "
              "the network) — searching %s", h->host, h->hosts_text);
}

/* ---- status ---------------------------------------------------------------------- */

static guint64 cpu_ticks(void) {
  char buf[1024];
  FILE *f = fopen("/proc/self/stat", "r");
  if (!f) { return 0; }
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  const char *p = strrchr(buf, ')');           /* comm may contain spaces      */
  unsigned long ut = 0, st = 0;
  if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                   &ut, &st) != 2) {
    return 0;
  }
  return (guint64)ut + st;
}

typedef struct {
  guint   channels, active, stations;
  guint64 spots, drops;
} BandStat;

static void band_stat(Band *b, BandStat *s) {
  const gint64 now = g_get_monotonic_time();
  s->channels = skim_pipeline_channels(b->p);
  s->spots = skim_pipeline_rbn_spots(b->p);
  s->drops = skim_pipeline_dropped_blocks(b->p);
  s->active = 0;
  g_mutex_lock(&b->lock);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, b->active);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    const double sc = act_now(v, now);
    if (sc < 0.1) {
      g_hash_table_iter_remove(&it);
    } else if (sc >= ACTIVE_SCORE) {
      s->active++;
    }
  }
  s->stations = g_hash_table_size(b->stations);
  g_mutex_unlock(&b->lock);
}

static gint station_newest_first(gconstpointer a, gconstpointer b) {
  const SkimStation *x = *(SkimStation *const *)a, *y = *(SkimStation *const *)b;
  return x->last_heard < y->last_heard ? 1 : x->last_heard > y->last_heard ? -1 : 0;
}

/* The band's stations newest first (copies; caller frees the array). */
static GPtrArray *band_recent(Band *b, guint max) {
  GPtrArray *all = g_ptr_array_new_with_free_func(g_free);
  g_mutex_lock(&b->lock);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, b->stations);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    g_ptr_array_add(all, g_memdup2(v, sizeof(SkimStation)));
  }
  g_mutex_unlock(&b->lock);
  g_ptr_array_sort(all, station_newest_first);
  if (all->len > max) { g_ptr_array_set_size(all, max); }
  return all;
}

/* A JSON string body: quote, backslash and control characters escaped,
 * UTF-8 passed through (g_strescape octal-escapes it — not JSON). */
static char *json_str(const char *in) {
  GString *o = g_string_new(NULL);
  for (const guchar *p = (const guchar *)in; *p; p++) {
    if (*p == '"' || *p == '\\') { g_string_append_c(o, '\\'); g_string_append_c(o, (char)*p); }
    else if (*p < 0x20) { g_string_append_printf(o, "\\u%04x", *p); }
    else { g_string_append_c(o, (char)*p); }
  }
  return g_string_free(o, FALSE);
}

static void fmt_age(char *out, gsize n, gint64 us) {
  const gint64 s = us / G_USEC_PER_SEC;
  if (s < 60) { g_snprintf(out, n, "%" G_GINT64_FORMAT " s", s); }
  else { g_snprintf(out, n, "%" G_GINT64_FORMAT " min", s / 60); }
}

static void status_build(Hd *h, gboolean print) {
  const gint64 now = g_get_monotonic_time();
  const guint64 lost = h->rp ? skim_hpsdr_client_lost_frames(h->rp) : 0;
  guint64 frames0 = h->nb ? skim_pipeline_frames(h->band[0].p) : 0;
  const double lost_pct = frames0 ? 100.0 * (double)lost / (double)frames0 : 0;
  const guint fclients = h->feed ? skim_rbn_feed_clients(h->feed) : 0;
  const guint64 flines = h->feed ? skim_rbn_feed_lines(h->feed) : 0;
  const gint64 up_s = (now - h->t_start) / G_USEC_PER_SEC;

  GString *con = g_string_new(NULL);
  GString *html = g_string_new(NULL);
  GString *json = g_string_new(NULL);
  GDateTime *dt = g_date_time_new_now_local();
  char *clock = g_date_time_format(dt, "%H:%M:%S");
  g_date_time_unref(dt);

  g_string_append_printf(con,
      "── %s · %s · lost %.2f %% · %.0f pkt/s · CPU %.0f %% · feed :%d %u client%s, "
      "%" G_GUINT64_FORMAT " lines · dict %u · up %" G_GINT64_FORMAT ":%02d\n"
      "   band     centre kHz   span kHz        decoders  active  stations  spots  drops  kS/s\n",
      clock, h->rp_state, lost_pct, h->pps, h->cpu_pct, h->feed ? h->feed_port : 0,
      fclients, fclients == 1 ? "" : "s", flines, (guint)skim_callsign_dict_size(),
      up_s / 3600, (int)(up_s / 60 % 60));

  char *esc_state = g_markup_escape_text(h->rp_state, -1);
  g_string_append_printf(html,
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<meta http-equiv=refresh content=5><title>Skimmer status</title><style>"
      ":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fafafa;--mut:#6b6b70;--ln:#ddd;--ok:#1a7f37;--cq:#0b62c4}"
      "@media(prefers-color-scheme:dark){:root{--fg:#e6e6e6;--bg:#18181b;--mut:#9a9aa2;--ln:#333;--ok:#4ac26b;--cq:#5aa9ff}}"
      "body{font:14px/1.4 system-ui,sans-serif;margin:16px;color:var(--fg);background:var(--bg)}"
      "h1{font-size:18px;margin:0 0 4px}.mut{color:var(--mut)}"
      "table{border-collapse:collapse;margin:12px 0;width:100%%;max-width:900px}"
      "th,td{padding:4px 8px;border-bottom:1px solid var(--ln);text-align:right;white-space:nowrap}"
      "th:first-child,td:first-child{text-align:left}th{color:var(--mut);font-weight:500}"
      ".bands{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px;max-width:1200px}"
      ".b{border:1px solid var(--ln);border-radius:8px;padding:8px 10px}.b h2{font-size:15px;margin:0}"
      ".b table{margin:6px 0 0}.cq{color:var(--cq);font-weight:600}.on{color:var(--ok)}"
      ".wrap{overflow-x:auto}</style></head><body>"
      "<h1>Skimmer for Linux — headless</h1><div class=mut>%s · <span class=%s>%s</span><br>"
      "lost %.2f %% · %.0f packets/s · CPU %.0f %% · telnet feed :%d (%u client%s, %"
      G_GUINT64_FORMAT " lines) · MASTER.SCP %u calls · up %" G_GINT64_FORMAT ":%02d</div>"
      "<div class=wrap><table><tr><th>band</th><th>centre kHz</th><th>span kHz</th>"
      "<th>decoders</th><th>active</th><th>stations</th><th>spots</th><th>drops</th><th>kS/s</th></tr>",
      clock, h->streaming ? "on" : "mut", esc_state, lost_pct, h->pps, h->cpu_pct,
      h->feed ? h->feed_port : 0, fclients, fclients == 1 ? "" : "s", flines,
      (guint)skim_callsign_dict_size(), up_s / 3600, (int)(up_s / 60 % 60));
  g_free(esc_state);

  char *jstate = json_str(h->rp_state);
  char *jhost = json_str(h->host);
  /* the decoder that actually runs (deepcw falls back to cw-v2 without its
   * runtime or model) — a feed comparison needs to know which one spotted */
  const char *engine = h->nb && h->band[0].p ? skim_pipeline_cw_engine_name(h->band[0].p) : "";
  /* the feed policy that let the spots out — a comparison before and after
   * a policy change must be able to tell the two apart */
  double pol_score = 0, pol_settle = 0, pol_fresh = 0;
  guint pol_hear = 0;
  if (h->feed && h->nb && h->band[0].p) {
    skim_pipeline_rbn_policy(h->band[0].p, &pol_score, &pol_hear, &pol_settle,
                             &pol_fresh);
  }
  /* the learned gate in shadow: what it would have held back, all bands */
  const char *fg_id = NULL;
  double fg_thr = 0;
  guint64 fg_spots = 0, fg_below = 0;
  for (guint i = 0; i < h->nb; i++) {
    guint64 n = 0, k = 0;
    const char *id = h->band[i].p ? skim_pipeline_feed_gate_stats(h->band[i].p, &fg_thr,
                                                                  &n, &k) : NULL;
    if (id) { fg_id = id; }
    fg_spots += n;
    fg_below += k;
  }
  char *jgid = fg_id ? json_str(fg_id) : NULL;
  char *jgate = fg_id
      ? g_strdup_printf("{\"id\":\"%s\",\"mode\":\"shadow\",\"threshold\":%.2f,"
                        "\"spots\":%" G_GUINT64_FORMAT ",\"below\":%" G_GUINT64_FORMAT "}",
                        jgid, fg_thr, fg_spots, fg_below)
      : g_strdup("null");
  g_free(jgid);
  g_string_append_printf(json,
      "{\"time\":\"%s\",\"radio\":\"%s\",\"streaming\":%s,\"host\":\"%s\","
      "\"clock_ppm\":%.2f,\"rate\":%u,\"lost_pct\":%.3f,\"packets_per_s\":%.0f,\"cpu_pct\":%.1f,"
      "\"engine\":\"%s\","
      "\"feed_policy\":{\"min_score\":%.2f,\"min_hearings\":%u,\"settle_s\":%.1f,"
      "\"fresh_s\":%.1f},\"feed_gate\":%s,"
      "\"feed_port\":%d,\"feed_clients\":%u,\"feed_lines\":%" G_GUINT64_FORMAT ","
      "\"dict_calls\":%u,\"uptime_s\":%" G_GINT64_FORMAT ",\"bands\":[",
      clock, jstate, h->streaming ? "true" : "false", jhost, h->ppm_now, h->rate, lost_pct,
      h->pps, h->cpu_pct, engine, pol_score, pol_hear, pol_settle, pol_fresh, jgate,
      h->feed ? h->feed_port : 0, fclients, flines,
      (guint)skim_callsign_dict_size(), up_s);
  g_free(jstate);
  g_free(jgate);
  g_free(jhost);

  GString *cards = g_string_new("<div class=bands>");
  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    BandStat s;
    band_stat(b, &s);
    const double lo = (b->centre_hz - h->rate / 2.0) / 1000.0;
    const double hi = (b->centre_hz + h->rate / 2.0) / 1000.0;
    g_string_append_printf(con,
        "   %-6s %11.1f   %8.0f–%-8.0f %6u  %6u  %8u  %5" G_GUINT64_FORMAT
        "  %5" G_GUINT64_FORMAT "  %4.0f\n",
        b->name, b->centre_hz / 1000.0, lo, hi, s.channels, s.active, s.stations,
        s.spots, s.drops, b->ksps);
    g_string_append_printf(html,
        "<tr><td>%s</td><td>%.1f</td><td>%.0f–%.0f</td><td>%u</td><td>%u</td>"
        "<td>%u</td><td>%" G_GUINT64_FORMAT "</td><td>%" G_GUINT64_FORMAT
        "</td><td>%.0f</td></tr>",
        b->name, b->centre_hz / 1000.0, lo, hi, s.channels, s.active, s.stations,
        s.spots, s.drops, b->ksps);
    char *jname = json_str(b->name);
    g_string_append_printf(json,
        "%s{\"band\":\"%s\",\"centre_hz\":%.0f,\"decoders\":%u,\"active\":%u,"
        "\"stations\":%u,\"spots\":%" G_GUINT64_FORMAT ",\"drops\":%" G_GUINT64_FORMAT
        ",\"ksps\":%.1f,\"recent\":[",
        i ? "," : "", jname, b->centre_hz, s.channels, s.active, s.stations,
        s.spots, s.drops, b->ksps);
    g_free(jname);

    GPtrArray *rec = band_recent(b, RECENT_STATIONS);
    g_string_append_printf(cards, "<div class=b><h2>%s <span class=mut>· %u stations</span></h2>"
                           "<table><tr><th>call</th><th>kHz</th><th>wpm</th><th>dB</th>"
                           "<th>heard</th></tr>", b->name, s.stations);
    for (guint k = 0; k < rec->len; k++) {
      const SkimStation *st = rec->pdata[k];
      char age[24];
      fmt_age(age, sizeof(age), now - st->last_heard);
      char *call = g_markup_escape_text(st->call, -1);
      char *jcall = json_str(st->call);
      g_string_append_printf(cards,
          "<tr><td%s>%s</td><td>%.1f</td><td>%.0f</td><td>%.0f</td><td>%s</td></tr>",
          st->cq ? " class=cq" : "", call, st->freq_hz / 1000.0, st->speed,
          st->snr_db, age);
      g_string_append_printf(json,
          "%s{\"call\":\"%s\",\"freq_hz\":%.0f,\"wpm\":%.0f,\"snr_db\":%.0f,"
          "\"cq\":%s,\"age_s\":%" G_GINT64_FORMAT "}",
          k ? "," : "", jcall, st->freq_hz, st->speed, st->snr_db,
          st->cq ? "true" : "false", (now - st->last_heard) / G_USEC_PER_SEC);
      g_free(call);
      g_free(jcall);
    }
    if (!rec->len) {
      g_string_append(cards, "<tr><td colspan=5 class=mut>nothing yet</td></tr>");
    }
    g_string_append(cards, "</table></div>");
    g_string_append(json, "]}");
    g_ptr_array_unref(rec);
  }
  g_string_append(cards, "</div>");
  g_string_append(html, "</table></div><div class=mut>decoders = channel "
                  "decoders in the band · active = decoding running text now (noise fragments don't count) · "
                  "<span class=cq>blue</span> = heard calling CQ · refreshes every 5 s · "
                  "<a href=status.json>status.json</a></div>");
  g_string_append(html, cards->str);
  g_string_append(html, "</body></html>");
  g_string_append(json, "]}");
  g_string_free(cards, TRUE);
  g_free(clock);

  g_mutex_lock(&h->snap_lock);
  g_free(h->snap_html);
  g_free(h->snap_json);
  h->snap_html = g_string_free(html, FALSE);
  h->snap_json = g_string_free(json, FALSE);
  g_mutex_unlock(&h->snap_lock);

  if (print) {
    fputs(con->str, stdout);
    fflush(stdout);
  }
  g_string_free(con, TRUE);
}

/* 1 Hz: link supervision, rates, status. */
static gboolean tick(gpointer data) {
  Hd *h = data;
  if (h->streaming && g_atomic_int_get(&h->closed)) { rp_drop(h); }
  if (!h->streaming && h->ticks % 3 == 0) { rp_try_start(h); }
  if (h->exit_code) { return G_SOURCE_REMOVE; }

  const guint64 pk = h->rp ? skim_hpsdr_client_packets(h->rp) : 0;
  h->pps = h->streaming && h->packets_prev ? (double)(pk - h->packets_prev) : 0;
  h->packets_prev = pk;
  for (guint i = 0; i < h->nb; i++) {
    const guint64 fr = skim_pipeline_frames(h->band[i].p);
    h->band[i].ksps = h->band[i].frames_prev ? (fr - h->band[i].frames_prev) / 1000.0 : 0;
    h->band[i].frames_prev = fr;
  }
  const gint64 now = g_get_monotonic_time();
  const guint64 cpu = cpu_ticks();
  if (h->cpu_prev_us) {
    const double wall = (now - h->cpu_prev_us) / 1e6;
    h->cpu_pct = wall > 0 ? 100.0 * (double)(cpu - h->cpu_prev) /
                                (double)sysconf(_SC_CLK_TCK) / wall : 0;
  }
  h->cpu_prev = cpu;
  h->cpu_prev_us = now;

  h->ticks++;
  const gboolean print = h->console_s > 0 && h->ticks % (guint)h->console_s == 0;
  status_build(h, print);
  return G_SOURCE_CONTINUE;
}

/* ---- web page -------------------------------------------------------------------- */

static gboolean http_run(GThreadedSocketService *svc, GSocketConnection *conn,
                         GObject *src, gpointer user) {
  (void)svc; (void)src;
  Hd *h = user;
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  g_socket_set_timeout(g_socket_connection_get_socket(conn), 5);
  char req[2048];
  const gssize n = g_input_stream_read(in, req, sizeof(req) - 1, NULL, NULL);
  if (n <= 0) { return TRUE; }
  req[n] = '\0';
  const gboolean want_json = g_str_has_prefix(req, "GET /status.json");
  const gboolean root = g_str_has_prefix(req, "GET / ") || g_str_has_prefix(req, "GET /index");
  g_mutex_lock(&h->snap_lock);
  char *body = g_strdup(want_json ? h->snap_json : root ? h->snap_html : NULL);
  g_mutex_unlock(&h->snap_lock);
  char *head;
  if (!body) {
    body = g_strdup("not found\n");
    head = g_strdup_printf("HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\n"
                           "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
  } else {
    head = g_strdup_printf("HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
                           "Cache-Control: no-store\r\nContent-Length: %zu\r\n"
                           "Connection: close\r\n\r\n",
                           want_json ? "application/json" : "text/html; charset=utf-8",
                           strlen(body));
  }
  g_output_stream_write_all(out, head, strlen(head), NULL, NULL, NULL);
  g_output_stream_write_all(out, body, strlen(body), NULL, NULL, NULL);
  g_free(head);
  g_free(body);
  return TRUE;
}

/* ---- main ------------------------------------------------------------------------ */

static void on_scp(const SkimScpOutcome *out, gpointer user) {
  (void)user;
  g_message("MASTER.SCP: %s (%u calls loaded)", out->detail,
            (guint)skim_callsign_dict_size());
}

static gboolean on_signal(gpointer user) {
  g_message("signal — stopping");
  g_main_loop_quit(((Hd *)user)->loop);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  Hd *h = g_new0(Hd, 1);
  h->console_s = -1;
  h->http_port = -1;
  char *cfg = NULL;
  GOptionEntry opts[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &cfg,
      "Config file (default ~/.config/skimmer-for-linux/headless.ini)", "FILE" },
    { "take-over", 0, 0, G_OPTION_ARG_NONE, &h->take_over,
      "Start even if another client streams from the radio — the first busy one "
      "of [radio] host, when none is idle (first start only)", NULL },
    { "status-every", 's', 0, G_OPTION_ARG_INT, &h->console_s,
      "Console status every N s (0 = off; overrides the config)", "N" },
    { "http-port", 'p', 0, G_OPTION_ARG_INT, &h->http_port,
      "Web status port (0 = off; overrides the config)", "PORT" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *oc = g_option_context_new("— multi-band CW skimmer on a Red Pitaya, no GUI");
  g_option_context_add_main_entries(oc, opts, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return 2;
  }
  g_option_context_free(oc);
  h->cfg_path = cfg ? cfg : g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                             "headless.ini", NULL);
  if (!config_load(h, &err)) {
    g_printerr("config: %s\n", err->message);
    return 2;
  }
  g_mutex_init(&h->snap_lock);
  h->t_start = g_get_monotonic_time();
  g_message("skimmer-headless %s — %s, %u band%s × %u kHz, config %s", SKIMMER_VERSION,
            h->hosts_text, h->nb, h->nb == 1 ? "" : "s", h->rate / 1000, h->cfg_path);

  /* Callsign dictionary: loaded once for every pipeline, kept current. */
  char *scp = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux", "master.scp", NULL);
  if (g_file_test(scp, G_FILE_TEST_EXISTS) && !skim_callsign_dict_load(scp, &err)) {
    g_warning("MASTER.SCP %s not loaded: %s", scp, err->message);
    g_clear_error(&err);
  }
  h->scp = skim_scp_updater_new(scp, on_scp, h);
  skim_scp_updater_start(h->scp, 15);
  g_free(scp);

  if (h->feed_port > 0) {
    if (!h->feed_call || !h->feed_call[0]) {
      g_warning("feed: [feed] call is empty — telnet feed off");
    } else {
      h->feed = skim_rbn_feed_new(h->feed_call, (guint16)h->feed_port, &err);
      if (!h->feed) {
        g_warning("feed: port %d: %s — telnet feed off", h->feed_port, err->message);
        g_clear_error(&err);
      } else {
        g_message("feed: telnet spots on port %d as %s", h->feed_port, h->feed_call);
      }
    }
  }

  char *logdir = g_build_filename(g_get_user_data_dir(), "skimmer-for-linux", "headless", NULL);
  GDateTime *now = g_date_time_new_now_local();
  char *day = g_date_time_format(now, "%Y-%m-%d");
  g_date_time_unref(now);
  if (h->decode_log) { g_mkdir_with_parents(logdir, 0755); }
  /* Learning log: a decode tap per band and START (UTC) — a restart begins
   * a fresh tap, as the extractors and the station table begin fresh. Only
   * the tap: the gate rows are four times its size live (2026-10-03, 20 m:
   * 16 MB in 32 min), and skimmer-tap-replay writes them byte for byte from
   * the tap — under any policy, with any features added since. */
  char *learndir = g_build_filename(logdir, "learn", NULL);
  GDateTime *utc = g_date_time_new_now_utc();
  char *start = g_date_time_format(utc, "%Y%m%d-%H%M%S");
  g_date_time_unref(utc);
  if (h->feed_learn) { g_mkdir_with_parents(learndir, 0755); }
  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    g_mutex_init(&b->lock);
    b->active = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    b->stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_snprintf(b->label, sizeof(b->label), "RX%u %s", i + 1, b->name);
    char *dlog = h->decode_log ? g_strdup_printf("%s/decodes-%s-%s.log", logdir, b->name, day)
                               : NULL;
    char *tap = h->feed_learn ? g_strdup_printf("%s/tap-%s-%s.log", learndir, b->name, start)
                              : NULL;
    SkimPipelineConfig pc = {
      .source = SKIM_PIPELINE_SOURCE_EXTERNAL,
      .host = b->label,
      .iq_rate = h->rate,
      .center_hz = b->centre_hz,
      .mode = SKIM_PIPELINE_MODE_CW,
      .cw_engine = g_strcmp0(h->engine, "deepcw") == 0 ? SKIM_CW_ENGINE_DEEPCW
                                                       : SKIM_CW_ENGINE_V2,
      .decode_log_path = dlog,
      .rbn = h->feed,
      .rbn_min_score = h->feed_min_score,
      .rbn_min_hearings = h->feed_min_hearings,
      .rbn_settle_s = h->feed_settle_s,
      .rbn_fresh_s = h->feed_fresh_s,
      .tap_path = tap,
      .feed_gate_path = h->feed_gate,
      .band = b->name,
    };
    b->p = skim_pipeline_new(&pc);
    g_free(dlog);
    g_free(tap);
    skim_pipeline_set_text_cb(b->p, text_cb, b);
    skim_pipeline_set_station_cb(b->p, station_cb, b);
    skim_pipeline_set_station_gone_cb(b->p, gone_cb, b);
    skim_pipeline_set_state_cb(b->p, state_cb, b);
    if (!skim_pipeline_start(b->p, &err)) {
      g_printerr("pipeline %s: %s\n", b->name, err->message);
      return 1;
    }
  }
  if (h->feed_learn) { g_message("learn: decode tap in %s", learndir); }
  if (h->feed_gate) {
    const char *id = h->nb ? skim_pipeline_feed_gate_stats(h->band[0].p, NULL, NULL, NULL)
                           : NULL;
    if (id) { g_message("feed gate: %s in SHADOW (logged only)", h->feed_gate); }
  }
  g_free(learndir);
  g_free(start);
  g_free(logdir);
  g_free(day);
  g_message("decode: %u pipelines, engine %s", h->nb,
            skim_pipeline_cw_engine_name(h->band[0].p));
  if (h->feed) {
    double fs, fset, ffr;
    guint fh;
    skim_pipeline_rbn_policy(h->band[0].p, &fs, &fh, &fset, &ffr);
    g_message("feed: spots a call at score >= %.2f, read %u times (or in "
              "MASTER.SCP), after %.0f s settled, last read <= %.0f s ago "
              "(0 = any)", fs, fh, fset, ffr);
  }

  GSocketService *web = NULL;
  if (h->http_port > 0) {
    web = g_threaded_socket_service_new(4);
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(web), (guint16)h->http_port,
                                         NULL, &err)) {
      g_warning("web: port %d: %s — web status off", h->http_port, err->message);
      g_clear_error(&err);
      g_clear_object(&web);
    } else {
      g_signal_connect(web, "run", G_CALLBACK(http_run), h);
      g_socket_service_start(web);
      g_message("web: status on http://<this machine>:%d/ (JSON: /status.json)",
                h->http_port);
    }
  }

  h->loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGINT, on_signal, h);
  g_unix_signal_add(SIGTERM, on_signal, h);
  tick(h);                                     /* first start attempt now     */
  if (!h->exit_code) {                         /* a quit before run is lost   */
    g_timeout_add_seconds(1, tick, h);
    g_main_loop_run(h->loop);
  }

  /* Teardown: the radio first (stop — only if the stream is still ours),
   * then the engines, then the feed they spot into. */
  if (web) {
    g_socket_service_stop(web);
    g_object_unref(web);
  }
  g_clear_pointer(&h->rp, skim_hpsdr_client_free);
  for (guint i = 0; i < h->nb; i++) {
    skim_pipeline_free(h->band[i].p);
  }
  g_clear_pointer(&h->feed, skim_rbn_feed_free);
  g_clear_pointer(&h->scp, skim_scp_updater_free);
  g_message("stopped");
  return h->exit_code;
}
