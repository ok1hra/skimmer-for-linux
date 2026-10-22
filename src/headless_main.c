/*
 * skimmer-headless — multi-band skimmer without a GUI.
 *
 *   skimmer-headless [--config FILE] [--take-over] [--status-every S]
 *                    [--http-port N] [--feed-gate FILE]
 *                    [--feed-gate-mode off|shadow|on] [--check-config]
 *                    [--list-switches]
 *
 * An IQ source (headless/source.h — a Red Pitaya running sdr_receiver_hpsdr,
 * up to 8 receivers at once) feeds one engine pipeline per band and mode
 * (SOURCE_EXTERNAL): CW, and RTTY beside it on the same IQ where the band
 * asks for it ([bands] 20m=14045500 cw,rtty). Every pipeline's validated CQ
 * spots go to ONE shared telnet feed (cluster dialect — the CW Skimmer
 * Server model); [feed] rtty=false keeps RTTY off it. Status: a
 * table on stdout every few seconds and a small web page (+ /status.json).
 *
 * Config: ~/.config/skimmer-for-linux/headless.ini by default, written with
 * defaults when missing — headless/config.h has the keys. A config that
 * does not parse stops the start (exit 2).
 *
 * The SKIM_* environment switches the start runs with are logged and listed
 * in /status.json ("switches"); --list-switches prints the known ones.
 *
 * The learned feed gate is built in (weights/feed-gate.ini — a new model is
 * a new commit) and runs in SHADOW unless told otherwise: it scores every
 * line the hand gate lets through and only reports. --feed-gate FILE (or
 * [feed] gate=) replaces the model for this start; it must name every
 * feature. --feed-gate-mode (or [feed] gate_mode=) off drops it, on lets it
 * hold back the lines it scores below its threshold. The gate's id —
 * builtin:… or file:…, with the first 8 hex of its sha256 — is in the log
 * and in /status.json, so a measurement can tell its periods apart.
 *
 * The radio is never taken from another client unless --take-over is given
 * (headless/source_hpsdr_p1.c). When the source gives up — every radio busy,
 * or none free within its search time, also after a lost stream — the
 * skimmer exits with code 3 and its supervisor retries.
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
#include <errno.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "headless/config.h"
#include "headless/deepcw_model.h"
#include "headless/json.h"
#include "headless/logring.h"
#include "headless/record.h"
#include "headless/scp_update.h"
#include "headless/source.h"
#include "headless/stats.h"
#include "headless/switches.h"
#include "headless/web.h"
#include "engine/callsign.h"
#include "engine/decode_deepcw.h"
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

#define EXIT_CONFIG      2
#define EXIT_NO_RADIO    3
#define EXIT_RESTART     75            /* the settings page asked for one     */

/* The modes a band decodes, each its own pipeline on the band's IQ; the
 * index is the stats mode (stats.h). */
enum { M_CW = 0, M_RTTY = 1, M_N = SKIM_STATS_MODES };
static const char *const MODE_KEY[M_N] = { "cw", "rtty" };

typedef struct {
  const char   *band;          /* the band's name                             */
  guint         mode;          /* M_CW / M_RTTY                               */
  double        bin_hz;        /* channel spacing: the active-decoder key     */
  SkimPipeline *p;             /* NULL = the band does not decode this mode   */
  GMutex        lock;          /* engine-thread callbacks ↔ main loop         */
  GHashTable   *active;        /* channel key → Act                           */
  GHashTable   *stations;      /* call → SkimStation copy                     */
  /* the minute's history (stats.h): under lock, from the engine thread */
  guint         appeared;      /* stations new in the table this minute       */
  GArray       *snr;           /* SNR of the minute's feed lines (float)      */
  /* main loop: the cumulative counters at the last minute's end */
  guint64       spots_prev, folded_prev, below_prev;
} Dec;

typedef struct {
  char          name[16];
  double        centre_hz;
  char          label[96];
  Dec           dec[M_N];
  SkimRttyPairs *pairs;        /* CW and RTTY: the pairs RTTY copies (or NULL) */
  guint64       frames_prev;   /* main loop: for kS/s                         */
  double        ksps;
  Rec          *rec;           /* [record]: this band's IQ tap, or NULL       */
} Band;

/* A pipeline of the band — every one of them sees the same IQ. */
static SkimPipeline *band_p(const Band *b) {
  return b->dec[M_CW].p ? b->dec[M_CW].p : b->dec[M_RTTY].p;
}

typedef struct {
  SkimHdConfig cfg;
  char    *cfg_path;
  /* start switches: the command line wins over the config */
  int      console_s;          /* -1 = the config's (10)                      */
  int      http_port;          /* -1 = the config's (8073)                    */
  gboolean take_over;          /* --take-over: the first start only           */
  char    *feed_gate;          /* learned gate ini; NULL = the built-in one   */
  char    *feed_gate_mode;     /* off | shadow | on                           */
  char    *cli_gate;           /* --feed-gate: wins over the config           */
  char    *cli_gate_mode;      /* --feed-gate-mode                            */
  SkimFeedGate *gate;          /* loaded once, shared by the pipelines        */
  char    *gate_id;            /* builtin:… / file:…, #sha8                   */
  char    *switches_json;      /* the SKIM_* switches set at start            */
  char   **argv;               /* as started, for a restart without a supervisor */
  gint64   started_us;         /* real time of this start: tells starts apart */

  Band     band[SKIM_HD_MAX_BANDS];
  guint    nb;

  SkimIqSource      *src;
  SkimIqSourceStatus src_st;   /* main loop: refreshed every tick             */
  guint64   packets_prev;
  double    pps;
  int       exit_code;         /* != 0: stop the main loop with it            */
  SkimRbnFeed    *feed;
  SkimScpUpdater *scp;
  GMainLoop      *loop;
  gint64          t_start;
  guint64         cpu_prev;    /* clock ticks, utime + stime                  */
  gint64          cpu_prev_us;
  double          cpu_pct;
  guint           ticks;

  SkimStats *stats;            /* the 7-day history (stats.h)                 */
  gint64    minute;            /* the unix minute being collected             */
  guint64   min_frames_prev, min_lost_prev;
  guint     minutes;           /* minutes recorded since start                */

  GMutex   snap_lock;          /* the web page's copy of the status           */
  char    *snap_json;

  /* DeepCW: the model on disk, why a band that asks for it runs v2, and a
   * fetch the web page started (on its own thread, under dcw_lock) */
  gboolean      dcw_wanted;    /* a band names deepcw                         */
  char         *dcw_model;     /* where the engine looks for it               */
  char         *dcw_base_url;  /* $SKIM_DEEPCW_URL, NULL = upstream           */
  char         *dcw_ca;        /* TLS trust store (AppImage)                  */
  char         *dcw_fallback;  /* why the bands asking for it run v2          */
  GMutex        dcw_lock;
  gboolean      dcw_model_ok;
  char         *dcw_model_err;
  const char   *dcw_state;     /* idle | running | done | failed              */
  guint64       dcw_got, dcw_total;
  char         *dcw_err;
  GThread      *dcw_thread;
  GCancellable *dcw_cancel;
} Hd;

/* ---- pipeline callbacks (engine threads) ----------------------------------------- */

/* ---- the learned feed gate ------------------------------------------------------- */

#define GATE_BUILTIN "/skimmer-for-linux/weights/feed-gate.ini"

static gboolean gate_on(const Hd *h) { return g_strcmp0(h->feed_gate_mode, "on") == 0; }

/* The model this start runs: --feed-gate, else [feed] gate=, else the
 * built-in one — strict either way (every feature named). Mode off loads
 * none. */
static gboolean gate_load(Hd *h, GError **error) {
  if (h->cli_gate) { g_free(h->feed_gate); h->feed_gate = g_strdup(h->cli_gate); }
  if (h->cli_gate_mode) {
    g_free(h->feed_gate_mode);
    h->feed_gate_mode = g_strdup(h->cli_gate_mode);
  }
  if (!h->feed_gate_mode || !h->feed_gate_mode[0]) {
    g_free(h->feed_gate_mode);
    h->feed_gate_mode = g_strdup("shadow");
  }
  if (g_strcmp0(h->feed_gate_mode, "off") && g_strcmp0(h->feed_gate_mode, "shadow") &&
      !gate_on(h)) {
    g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                "feed gate mode \"%s\": off, shadow or on", h->feed_gate_mode);
    return FALSE;
  }
  if (g_strcmp0(h->feed_gate_mode, "off") == 0) { return TRUE; }
  GBytes *data = NULL;
  char *src;
  if (h->feed_gate) {
    char *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents(h->feed_gate, &text, &len, error)) { return FALSE; }
    data = g_bytes_new_take(text, len);
    char *base = g_path_get_basename(h->feed_gate);
    src = g_strdup_printf("file:%s", base);
    g_free(base);
  } else {
    data = g_resources_lookup_data(GATE_BUILTIN, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
    if (!data) { return FALSE; }
    src = g_strdup("builtin:feed-gate.ini");
  }
  gsize len = 0;
  const char *text = g_bytes_get_data(data, &len);
  char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)text, len);
  h->gate_id = g_strdup_printf("%s#%.8s", src, sha);
  h->gate = skim_feed_gate_load_data(text, len, h->gate_id, TRUE, error);
  g_free(sha);
  g_free(src);
  g_bytes_unref(data);
  return h->gate != NULL;
}

static void text_cb(double freq_hz, const char *text, gpointer user) {
  Dec *b = user;
  guint chars = 0;
  for (const char *c = text; *c; c++) {
    if (g_ascii_isalnum(*c) || *c == '/' || *c == '?') { chars++; }
  }
  if (!chars) { return; }
  const gint64 now = g_get_monotonic_time();
  const gpointer key = GINT_TO_POINTER((gint)(freq_hz / b->bin_hz + 0.5));
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
  Dec *b = user;
  g_mutex_lock(&b->lock);
  SkimStation *copy = g_hash_table_lookup(b->stations, st->call);
  if (!copy) {
    copy = g_new0(SkimStation, 1);
    g_hash_table_insert(b->stations, g_strdup(st->call), copy);
    b->appeared++;
  }
  *copy = *st;
  g_mutex_unlock(&b->lock);
}

/* Every line the feed puts out (engine thread): its SNR for the minute. */
static void feed_cb(const SkimStation *st, gpointer user) {
  Dec *b = user;
  const float v = (float)st->snr_db;
  g_mutex_lock(&b->lock);
  g_array_append_val(b->snr, v);
  g_mutex_unlock(&b->lock);
}

static void gone_cb(const SkimStation *st, gpointer user) {
  Dec *b = user;
  g_mutex_lock(&b->lock);
  g_hash_table_remove(b->stations, st->call);
  g_mutex_unlock(&b->lock);
}

static void state_cb(gboolean connected, const char *detail, gpointer user) {
  Dec *b = user;
  if (connected && detail && strstr(detail, "channels")) {
    g_message("%s %s: %s", b->band, MODE_KEY[b->mode], detail);  /* bank sized */
  }
}

/* Source thread: one block of band i to its pipeline (and its recording). */
static void rx_iq_cb(guint i, const float *iq, guint n, double rate, double centre,
                     gpointer user) {
  Band *b = &((Hd *)user)->band[i];
  if (b->rec) { skim_rec_push(b->rec, iq, n, rate, centre); }
  for (guint k = 0; k < M_N; k++) {
    if (b->dec[k].p) { skim_pipeline_push(b->dec[k].p, iq, n, rate, centre); }
  }
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

static void band_stat(Dec *b, BandStat *s) {
  memset(s, 0, sizeof(*s));
  if (!b->p) { return; }
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
static GPtrArray *band_recent(Dec *b, guint max) {
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

/* The pipeline the feed policy is read from: any that feeds (one policy
 * for all). */
static SkimPipeline *feed_pipeline(const Hd *h) {
  if (!h->feed) { return NULL; }
  for (guint i = 0; i < h->nb; i++) {
    if (h->band[i].dec[M_CW].p) { return h->band[i].dec[M_CW].p; }
  }
  return h->cfg.feed_rtty && h->nb ? h->band[0].dec[M_RTTY].p : NULL;
}

/* One mode of a band as JSON fields (no braces): what decodes it, its
 * counters and its newest stations. A mode not decoded gives zeros. */
static void dec_json(GString *json, Dec *d, gint64 now) {
  BandStat s;
  band_stat(d, &s);
  g_string_append_printf(json,
      "\"engine\":\"%s\",\"decoders\":%u,\"active\":%u,\"stations\":%u,"
      "\"spots\":%" G_GUINT64_FORMAT ",\"drops\":%" G_GUINT64_FORMAT ",\"recent\":[",
      d->p ? skim_pipeline_cw_engine_name(d->p) : "", s.channels, s.active, s.stations,
      s.spots, s.drops);
  GPtrArray *rec = d->p ? band_recent(d, RECENT_STATIONS) : g_ptr_array_new();
  for (guint k = 0; k < rec->len; k++) {
    const SkimStation *st = rec->pdata[k];
    char *jcall = skim_json_str(st->call);
    /* wpm: the CW speed, or the RTTY baud rate */
    g_string_append_printf(json,
        "%s{\"call\":\"%s\",\"freq_hz\":%.0f,\"wpm\":%.0f,\"snr_db\":%.0f,"
        "\"cq\":%s,\"age_s\":%" G_GINT64_FORMAT "}",
        k ? "," : "", jcall, st->freq_hz, st->speed, st->snr_db,
        st->cq ? "true" : "false", (now - st->last_heard) / G_USEC_PER_SEC);
    g_free(jcall);
  }
  g_string_append_c(json, ']');
  g_ptr_array_unref(rec);
}

static const char *engine_key(SkimCwEngine e) {
  return e == SKIM_CW_ENGINE_DEEPCW ? "deepcw" : e == SKIM_CW_ENGINE_V1 ? "v1" : "v2";
}

static void json_opt_str(GString *o, const char *v) {
  if (!v) {
    g_string_append(o, "null");
    return;
  }
  g_string_append_c(o, '"');
  skim_json_append(o, v);
  g_string_append_c(o, '"');
}

/* "deepcw": the model, the runtime, why a band asking for it runs v2, and
 * the fetch the web page can start. */
static void dcw_json(Hd *h, GString *o) {
  g_mutex_lock(&h->dcw_lock);
  g_string_append_printf(o, "{\"wanted\":%s,\"model\":", h->dcw_wanted ? "true" : "false");
  json_opt_str(o, h->dcw_model);
  g_string_append_printf(o, ",\"model_ok\":%s,\"model_error\":",
                         h->dcw_model_ok ? "true" : "false");
  json_opt_str(o, h->dcw_model_ok ? NULL : h->dcw_model_err);
  g_string_append(o, ",\"runtime\":");
  json_opt_str(o, skim_decode_deepcw_runtime_info());
  g_string_append(o, ",\"fallback\":");
  json_opt_str(o, h->dcw_fallback);
  g_string_append_printf(o, ",\"commit\":\"%s\",\"fetch\":{\"state\":\"%s\",\"got\":%"
                         G_GUINT64_FORMAT ",\"total\":%" G_GUINT64_FORMAT ",\"error\":",
                         SKIM_DEEPCW_COMMIT, h->dcw_state, h->dcw_got, h->dcw_total);
  json_opt_str(o, h->dcw_err);
  g_string_append(o, "}}");
  g_mutex_unlock(&h->dcw_lock);
}

static void status_build(Hd *h, gboolean print) {
  const gint64 now = g_get_monotonic_time();
  const SkimIqSourceStatus *src = &h->src_st;
  const guint64 lost = src->lost_frames;
  guint64 frames0 = h->nb ? skim_pipeline_frames(band_p(&h->band[0])) : 0;
  const double lost_pct = frames0 ? 100.0 * (double)lost / (double)frames0 : 0;
  const guint fclients = h->feed ? skim_rbn_feed_clients(h->feed) : 0;
  const guint64 flines = h->feed ? skim_rbn_feed_lines(h->feed) : 0;
  const gint64 up_s = (now - h->t_start) / G_USEC_PER_SEC;

  GString *con = g_string_new(NULL);
  GString *json = g_string_new(NULL);
  GDateTime *dt = g_date_time_new_now_local();
  char *clock = g_date_time_format(dt, "%H:%M:%S");
  g_date_time_unref(dt);

  g_string_append_printf(con,
      "── %s · %s · lost %.2f %% · %.0f pkt/s · CPU %.0f %% · feed :%d %u client%s, "
      "%" G_GUINT64_FORMAT " lines · dict %u · up %" G_GINT64_FORMAT ":%02d\n"
      "   band  mode  centre kHz   span kHz        decoders  active  stations  spots  drops  kS/s\n",
      clock, src->state, lost_pct, h->pps, h->cpu_pct, h->feed ? h->cfg.feed_port : 0,
      fclients, fclients == 1 ? "" : "s", flines, (guint)skim_callsign_dict_size(),
      up_s / 3600, (int)(up_s / 60 % 60));


  char *jstate = skim_json_str(src->state);
  char *jhost = skim_json_str(src->host);
  /* the decoder that actually runs (deepcw falls back to cw-v2 without its
   * runtime or model) — a feed comparison needs to know which one spotted */
  const char *engine = "";
  for (guint i = 0; i < h->nb && !engine[0]; i++) {
    if (h->band[i].dec[M_CW].p) { engine = skim_pipeline_cw_engine_name(h->band[i].dec[M_CW].p); }
  }
  /* the feed policy that let the spots out — a comparison before and after
   * a policy change must be able to tell the two apart */
  double pol_score = 0, pol_settle = 0, pol_fresh = 0, pol_fold = 0, pol_nd = 0;
  guint pol_hear = 0;
  guint64 folded = 0;
  SkimPipeline *fp = feed_pipeline(h);
  if (fp) {
    skim_pipeline_rbn_policy(fp, &pol_score, &pol_hear, &pol_settle, &pol_fresh);
    skim_pipeline_rbn_fold(fp, &pol_fold, NULL);
    for (guint i = 0; i < h->nb; i++) {
      for (guint k = 0; k < M_N; k++) {
        guint64 n = 0;
        if (h->band[i].dec[k].p) { skim_pipeline_rbn_fold(h->band[i].dec[k].p, NULL, &n); }
        folded += n;
      }
    }
    pol_nd = skim_pipeline_rbn_settle_nodict(fp);
  }
  /* the learned gate: what it held back (on) or would have (shadow) */
  const char *fg_id = NULL;
  double fg_thr = 0;
  guint64 fg_spots = 0, fg_below = 0;
  for (guint i = 0; i < h->nb; i++) {
    guint64 n = 0, k = 0;
    SkimPipeline *cw = h->band[i].dec[M_CW].p;          /* RTTY has no gate */
    const char *id = cw ? skim_pipeline_feed_gate_stats(cw, &fg_thr, &n, &k) : NULL;
    if (id) { fg_id = id; }
    fg_spots += n;
    fg_below += k;
  }
  char *jgid = fg_id ? skim_json_str(fg_id) : NULL;
  char *jgate = fg_id
      ? g_strdup_printf("{\"id\":\"%s\",\"mode\":\"%s\",\"threshold\":%.2f,"
                        "\"spots\":%" G_GUINT64_FORMAT ",\"below\":%" G_GUINT64_FORMAT "}",
                        jgid, gate_on(h) ? "on" : "shadow", fg_thr, fg_spots, fg_below)
      : g_strdup("null");
  g_free(jgid);
  g_string_append_printf(json,
      "{\"version\":\"%s\",\"started_us\":%" G_GINT64_FORMAT ",\"time\":\"%s\",\"radio\":\"%s\",\"streaming\":%s,\"host\":\"%s\","
      "\"clock_ppm\":%.2f,\"rate\":%u,\"lost_pct\":%.3f,\"packets_per_s\":%.0f,\"cpu_pct\":%.1f,"
      "\"engine\":\"%s\","
      "\"feed_policy\":{\"min_score\":%.2f,\"min_hearings\":%u,\"settle_s\":%.1f,"
      "\"settle_nodict_s\":%.1f,"
      "\"fresh_s\":%.1f,\"fold_s\":%.1f,\"folded\":%" G_GUINT64_FORMAT "},"
      "\"feed_gate\":%s,\"switches\":%s,"
      "\"suppress_cw_on_rtty\":%s,\"feed_port\":%d,\"feed_rtty\":%s,\"feed_clients\":%u,\"feed_lines\":%" G_GUINT64_FORMAT ","
      "\"dict_calls\":%u,\"uptime_s\":%" G_GINT64_FORMAT ",\"bands\":[",
      SKIMMER_VERSION, h->started_us, clock, jstate, src->streaming ? "true" : "false", jhost, src->ppm, h->cfg.rate, lost_pct,
      h->pps, h->cpu_pct, engine, pol_score, pol_hear, pol_settle, pol_nd, pol_fresh,
      pol_fold, folded, jgate, h->switches_json,
      h->cfg.suppress_cw_on_rtty ? "true" : "false",
      h->feed ? h->cfg.feed_port : 0, h->cfg.feed_rtty ? "true" : "false", fclients, flines,
      (guint)skim_callsign_dict_size(), up_s);
  g_free(jstate);
  g_free(jgate);
  g_free(jhost);

  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    const double lo = (b->centre_hz - h->cfg.rate / 2.0) / 1000.0;
    const double hi = (b->centre_hz + h->cfg.rate / 2.0) / 1000.0;
    for (guint k = 0; k < M_N; k++) {
      Dec *d = &b->dec[k];
      if (!d->p) { continue; }
      BandStat s;
      band_stat(d, &s);
      g_string_append_printf(con,
          "   %-5s %-4s %11.1f   %8.0f–%-8.0f %6u  %6u  %8u  %5" G_GUINT64_FORMAT
          "  %5" G_GUINT64_FORMAT "  %4.0f\n",
          b->name, MODE_KEY[k], b->centre_hz / 1000.0, lo, hi, s.channels, s.active,
          s.stations, s.spots, s.drops, b->ksps);
    }
    /* The band's CW at the top level (what skimmer-compare and older pages
     * read; zeros when the band decodes RTTY only), its RTTY in "rtty". */
    char *jname = skim_json_str(b->name);
    /* rtty_suppressed: CW decodes dropped on an RTTY pair (0 while off) */
    /* engine_wanted: what the config asks for — "deepcw" beside an
     * engine "cw-v2" is a band that fell back (the reason: "deepcw") */
    g_string_append_printf(json, "%s{\"band\":\"%s\",\"centre_hz\":%.0f,\"ksps\":%.1f,"
                           "\"cw\":%s,\"engine_wanted\":\"%s\","
                           "\"rtty_suppressed\":%" G_GUINT64_FORMAT ",",
                           i ? "," : "", jname, b->centre_hz, b->ksps,
                           b->dec[M_CW].p ? "true" : "false",
                           b->dec[M_CW].p ? engine_key(h->cfg.band[i].cw_engine) : "",
                           b->dec[M_CW].p ? skim_pipeline_rtty_suppressed(b->dec[M_CW].p) : 0);
    g_free(jname);
    dec_json(json, &b->dec[M_CW], now);
    g_string_append(json, ",\"rtty\":");
    if (b->dec[M_RTTY].p) {
      g_string_append_c(json, '{');
      dec_json(json, &b->dec[M_RTTY], now);
      /* the pairs it copies now (published for the band's CW) */
      g_string_append_printf(json, ",\"pairs\":%u}",
                             b->pairs ? skim_rtty_pairs_count(b->pairs, now) : 0);
    } else {
      g_string_append(json, "null");
    }
    g_string_append_c(json, '}');
  }
  g_string_append(json, "],\"deepcw\":");
  dcw_json(h, json);
  g_string_append_c(json, '}');
  g_free(clock);

  g_mutex_lock(&h->snap_lock);
  g_free(h->snap_json);
  h->snap_json = g_string_free(json, FALSE);
  g_mutex_unlock(&h->snap_lock);

  if (print) {
    fputs(con->str, stdout);
    fflush(stdout);
  }
  g_string_free(con, TRUE);
}

/* ---- history (headless/stats.h) ------------------------------------------------- */

static gint float_cmp(gconstpointer a, gconstpointer b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

/* Close the minute h->minute: what each band did in it, and the machine. */
static void stats_minute(Hd *h) {
  SkimStatsMinute m;
  memset(&m, 0, sizeof(m));
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    for (guint k = 0; k < SKIM_STATS_MODES; k++) {
      for (guint j = 0; j < SKIM_SM_N; j++) { m.band[b][k][j] = NAN; }
    }
  }
  for (guint i = 0; i < h->nb && i < SKIM_STATS_BANDS; i++) {
    for (guint k = 0; k < M_N; k++) {
      Dec *b = &h->band[i].dec[k];
      if (!b->p) { continue; }                 /* stays NaN: not decoded      */
      BandStat bs;
      band_stat(b, &bs);
      guint64 folded = 0, gspots = 0, below = 0;
      skim_pipeline_rbn_fold(b->p, NULL, &folded);
      skim_pipeline_feed_gate_stats(b->p, NULL, &gspots, &below);
      float *v = m.band[i][k];
      v[SKIM_SM_SPOTS] = (float)(bs.spots - b->spots_prev);
      v[SKIM_SM_FOLDED] = (float)(folded - b->folded_prev);
      v[SKIM_SM_GATE_BELOW] = (float)(below - b->below_prev);
      v[SKIM_SM_ACTIVE] = (float)bs.active;
      b->spots_prev = bs.spots;
      b->folded_prev = folded;
      b->below_prev = below;
      g_mutex_lock(&b->lock);
      v[SKIM_SM_STATIONS] = (float)b->appeared;
      b->appeared = 0;
      if (b->snr->len) {
        g_array_sort(b->snr, float_cmp);
        v[SKIM_SM_SNR] = g_array_index(b->snr, float, b->snr->len / 2);
        g_array_set_size(b->snr, 0);
      }
      g_mutex_unlock(&b->lock);
    }
  }
  const SkimIqSourceStatus *src = &h->src_st;
  const guint64 frames = h->nb ? skim_pipeline_frames(band_p(&h->band[0])) : 0;
  const guint64 df = frames - h->min_frames_prev, dl = src->lost_frames - h->min_lost_prev;
  m.glob[SKIM_SG_CPU] = (float)h->cpu_pct;
  m.glob[SKIM_SG_LOST] = df + dl && src->lost_frames >= h->min_lost_prev
                             ? (float)(100.0 * (double)dl / (double)(df + dl)) : 0;
  m.glob[SKIM_SG_CLIENTS] = h->feed ? (float)skim_rbn_feed_clients(h->feed) : 0;
  m.glob[SKIM_SG_STREAMING] = src->streaming ? 1 : 0;
  m.glob[SKIM_SG_RADIO] = 0;
  for (guint i = 0; src->streaming && h->cfg.hosts[i]; i++) {
    if (strcmp(h->cfg.hosts[i], src->host) == 0) { m.glob[SKIM_SG_RADIO] = (float)(i + 1); }
  }
  h->min_frames_prev = frames;
  h->min_lost_prev = src->lost_frames;
  /* the first minute after a start is partial and its deltas start from
   * zero — not a record */
  if (h->minutes++) { skim_stats_put(h->stats, h->minute, &m); }
  if (h->minutes % 5 == 0) {
    GError *err = NULL;
    if (!skim_stats_save(h->stats, &err)) {
      g_warning("stats: %s", err->message);
      g_clear_error(&err);
    }
  }
}

/* 1 Hz: link supervision, rates, status. */
static gboolean tick(gpointer data) {
  Hd *h = data;
  skim_iq_source_tick(h->src);
  skim_iq_source_status(h->src, &h->src_st);
  if (h->src_st.gave_up) {
    h->exit_code = EXIT_NO_RADIO;
    g_main_loop_quit(h->loop);
    return G_SOURCE_REMOVE;
  }

  const guint64 pk = h->src_st.packets;
  h->pps = h->src_st.streaming && h->packets_prev && pk >= h->packets_prev
               ? (double)(pk - h->packets_prev) : 0;
  h->packets_prev = pk;
  for (guint i = 0; i < h->nb; i++) {
    const guint64 fr = skim_pipeline_frames(band_p(&h->band[i]));
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

  const gint64 minute = g_get_real_time() / (60 * G_USEC_PER_SEC);
  if (minute != h->minute) {
    if (h->minute) { stats_minute(h); }
    h->minute = minute;
  }

  h->ticks++;
  const gboolean print = h->console_s > 0 && h->ticks % (guint)h->console_s == 0;
  status_build(h, print);
  return G_SOURCE_CONTINUE;
}

/* ---- web page (headless/web.h) --------------------------------------------------- */

static char *web_status(GHashTable *q, gboolean may, guint *st, const char **type,
                        gpointer user) {
  (void)q; (void)may; (void)st; (void)type;
  Hd *h = user;
  g_mutex_lock(&h->snap_lock);
  char *body = g_strdup(h->snap_json ? h->snap_json : "{}");
  g_mutex_unlock(&h->snap_lock);
  return body;
}

static char *web_stats(GHashTable *q, gboolean may, guint *st, const char **type,
                       gpointer user) {
  (void)may; (void)st; (void)type;
  Hd *h = user;
  const char *hv = q ? g_hash_table_lookup(q, "hours") : NULL;
  const char *sv = q ? g_hash_table_lookup(q, "step") : NULL;
  return skim_stats_json(h->stats, g_get_real_time() / (60 * G_USEC_PER_SEC),
                         hv ? (guint)g_ascii_strtoull(hv, NULL, 10) : 24,
                         sv ? (guint)g_ascii_strtoull(sv, NULL, 10) : 0);
}

/* ---- settings (POST from settings_from only — web.c checks) ---------------------- */

static gboolean quit_soon(gpointer user) {
  g_main_loop_quit(((Hd *)user)->loop);
  return G_SOURCE_REMOVE;
}

/* Restart once the answer is out: under a supervisor (SKIM_SUPERVISED=1,
 * the systemd unit sets it) exit EXIT_RESTART and let it start us again;
 * alone, main execs itself after the teardown. */
static void restart_soon(Hd *h, const char *why) {
  g_message("restart: %s", why);
  h->exit_code = EXIT_RESTART;
  g_timeout_add(400, quit_soon, h);
}

static char *json_error(guint *st, guint code, const char *msg) {
  *st = code;
  char *e = skim_json_str(msg);
  char *body = g_strdup_printf("{\"error\":\"%s\"}", e);
  g_free(e);
  return body;
}

static void json_kv(GString *o, const char *k, const char *v, gboolean comma) {
  g_string_append_printf(o, "%s\"", comma ? "," : "");
  skim_json_append(o, k);
  g_string_append(o, "\":\"");
  skim_json_append(o, v);
  g_string_append_c(o, '"');
}

/* GET /api/config: the file on disk (what a restart would run) — its
 * editable values, its bands, the rest read-only, the backups. */
static char *web_config(GHashTable *q, gboolean may, guint *st, const char **type,
                        gpointer user) {
  (void)q; (void)type;
  Hd *h = user;
  char *text = NULL;
  gsize len = 0;
  GError *err = NULL;
  if (!g_file_get_contents(h->cfg_path, &text, &len, &err)) {
    char *b = json_error(st, 500, err->message);
    g_error_free(err);
    return b;
  }
  GString *o = g_string_new("{");
  json_kv(o, "path", h->cfg_path, FALSE);
  g_string_append_printf(o, ",\"may_change\":%s,\"settings_from\":[", may ? "true" : "false");
  for (guint i = 0; h->cfg.settings_from[i]; i++) {
    g_string_append_printf(o, "%s\"", i ? "," : "");
    skim_json_append(o, h->cfg.settings_from[i]);
    g_string_append_c(o, '"');
  }
  g_string_append(o, "],\"values\":{");
  GKeyFile *kf = g_key_file_new();
  g_key_file_load_from_data(kf, text, len, G_KEY_FILE_NONE, NULL);
  gboolean first = TRUE;
  for (const char *const *k = SKIM_HD_EDITABLE; *k; k++) {
    if (strcmp(*k, "bands") == 0) { continue; }
    char *group = g_strndup(*k, (gsize)(strchr(*k, '.') - *k));
    char *v = g_key_file_get_value(kf, group, strchr(*k, '.') + 1, NULL);
    json_kv(o, *k, v ? g_strstrip(v) : "", !first);
    first = FALSE;
    g_free(v);
    g_free(group);
  }
  g_string_append(o, "},\"readonly\":{");
  first = TRUE;
  static const char *const RO_GROUPS[] = { "feed", "decode", "web", "radio", "status", NULL };
  for (const char *const *g = RO_GROUPS; *g; g++) {
    gsize n = 0;
    char **keys = g_key_file_get_keys(kf, *g, &n, NULL);
    for (gsize i = 0; keys && i < n; i++) {
      char *full = g_strconcat(*g, ".", keys[i], NULL);
      gboolean ed = FALSE;
      for (const char *const *k = SKIM_HD_EDITABLE; *k && !ed; k++) { ed = strcmp(*k, full) == 0; }
      if (!ed) {
        char *v = g_key_file_get_value(kf, *g, keys[i], NULL);
        json_kv(o, full, v ? g_strstrip(v) : "", !first);
        first = FALSE;
        g_free(v);
      }
      g_free(full);
    }
    g_strfreev(keys);
  }
  g_string_append_c(o, '}');
  g_key_file_free(kf);
  SkimHdConfig c;
  if (skim_hd_config_parse(&c, text, len, h->cfg_path, &err)) {
    g_string_append(o, ",\"file_error\":null,\"bands\":[");
    for (guint i = 0; i < c.nb; i++) {
      g_string_append_printf(o, "%s{", i ? "," : "");
      json_kv(o, "name", c.band[i].name, FALSE);
      g_string_append_printf(o, ",\"centre_hz\":%.0f,\"cw\":%s,\"engine\":\"%s\",\"rtty\":%s}",
                             c.band[i].centre_hz, c.band[i].cw ? "true" : "false",
                             engine_key(c.band[i].cw_engine), c.band[i].rtty ? "true" : "false");
    }
    g_string_append_c(o, ']');
    skim_hd_config_clear(&c);
  } else {
    g_string_append(o, ",\"bands\":[],\"file_error\":\"");
    skim_json_append(o, err->message);
    g_string_append_c(o, '"');
    g_clear_error(&err);
  }
  g_string_append(o, ",\"backups\":[");
  char **baks = skim_hd_config_backups(h->cfg_path);
  char *dir = g_path_get_dirname(h->cfg_path);
  for (guint i = 0; baks[i]; i++) {
    char *p = g_build_filename(dir, baks[i], NULL);
    GStatBuf sb;
    const gint64 mt = g_stat(p, &sb) == 0 ? (gint64)sb.st_mtime : 0;
    g_string_append_printf(o, "%s{", i ? "," : "");
    json_kv(o, "name", baks[i], FALSE);
    g_string_append_printf(o, ",\"time\":%" G_GINT64_FORMAT "}", mt);
    g_free(p);
  }
  g_free(dir);
  g_strfreev(baks);
  g_string_append(o, "],");
  json_kv(o, "ini", text, FALSE);
  g_string_append_printf(o, ",\"started\":%" G_GINT64_FORMAT "}",
                         (g_get_real_time() - (g_get_monotonic_time() - h->t_start)) /
                             G_USEC_PER_SEC);
  g_free(text);
  return g_string_free(o, FALSE);
}

/* POST /api/config: the form applied to the file, checked by the start's
 * own parser, saved (the old file backed up), and a restart. */
static char *web_config_post(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  char *text = NULL;
  gsize len = 0;
  GError *err = NULL;
  char *body = NULL, *out = NULL, *bak = NULL;
  SkimHdConfig c;
  if (!g_file_get_contents(h->cfg_path, &text, &len, &err) ||
      !(out = skim_hd_config_apply_form(text, len, form, &err))) {
    body = json_error(st, 400, err->message);
  } else if (!skim_hd_config_parse(&c, out, strlen(out), h->cfg_path, &err)) {
    body = json_error(st, 400, err->message);         /* nothing written     */
  } else {
    skim_hd_config_clear(&c);
    if (strcmp(out, text) == 0) {
      body = g_strdup("{\"ok\":true,\"changed\":false}");
    } else if (!skim_hd_config_save(h->cfg_path, out, &bak, &err)) {
      body = json_error(st, 500, err->message);
    } else {
      g_message("config: %s saved from the web page (previous: %s)", h->cfg_path, bak);
      GString *o = g_string_new("{\"ok\":true,\"changed\":true");
      json_kv(o, "backup", bak, TRUE);
      g_string_append(o, ",\"restart\":true}");
      body = g_string_free(o, FALSE);
      restart_soon(h, "the config changed");
    }
  }
  g_clear_error(&err);
  g_free(bak);
  g_free(out);
  g_free(text);
  return body;
}

static char *web_restart(GHashTable *form, guint *st, gpointer user) {
  (void)form; (void)st;
  restart_soon(user, "asked for on the web page");
  return g_strdup("{\"ok\":true,\"restart\":true}");
}

/* POST /api/rollback name=<backup>: that backup back as the config. */
static char *web_rollback(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  const char *name = g_hash_table_lookup(form, "name");
  GError *err = NULL;
  if (!name) { return json_error(st, 400, "which backup? (name=)"); }
  char *dir = g_path_get_dirname(h->cfg_path);
  char *p = g_build_filename(dir, name, NULL);
  g_free(dir);
  char *text = NULL;
  gsize len = 0;
  SkimHdConfig c;
  char *body;
  /* a backup that would not start is not restored */
  if (strchr(name, '/') || !g_file_get_contents(p, &text, &len, &err) ||
      !skim_hd_config_parse(&c, text, len, h->cfg_path, &err)) {
    body = json_error(st, 400, err ? err->message : "no such backup");
  } else {
    skim_hd_config_clear(&c);
    if (!skim_hd_config_restore(h->cfg_path, name, &err)) {
      body = json_error(st, 400, err->message);
    } else {
      g_message("config: %s restored from %s on the web page", h->cfg_path, name);
      body = g_strdup("{\"ok\":true,\"restart\":true}");
      restart_soon(h, "a backup of the config was restored");
    }
  }
  g_clear_error(&err);
  g_free(text);
  g_free(p);
  return body;
}

/* ---- the DeepCW model, fetched on request ------------------------------------- */

static void dcw_progress(guint64 got, guint64 total, gpointer user) {
  Hd *h = user;
  g_mutex_lock(&h->dcw_lock);
  h->dcw_got = got;
  h->dcw_total = total;
  g_mutex_unlock(&h->dcw_lock);
}

static gpointer dcw_fetch_thread(gpointer user) {
  Hd *h = user;
  const SkimDeepcwFetch f = {
    .model_path = h->dcw_model, .base_url = h->dcw_base_url, .ca_path = h->dcw_ca,
    .cancel = h->dcw_cancel, .progress = dcw_progress, .user = h,
  };
  GError *err = NULL;
  gboolean ok = skim_deepcw_model_fetch(&f, &err);
  GError *cerr = NULL;
  const gboolean model_ok = ok && skim_deepcw_model_check(h->dcw_model, &cerr);
  g_mutex_lock(&h->dcw_lock);
  h->dcw_state = model_ok ? "done" : "failed";
  h->dcw_model_ok = model_ok;
  g_clear_pointer(&h->dcw_err, g_free);
  if (!model_ok) { h->dcw_err = g_strdup(err ? err->message : cerr->message); }
  g_mutex_unlock(&h->dcw_lock);
  if (model_ok) {
    g_message("deepcw: model %s installed (commit %.7s)%s", h->dcw_model, SKIM_DEEPCW_COMMIT,
              h->dcw_wanted ? " — a restart puts it to work" : "");
  } else {
    g_warning("deepcw: model fetch failed: %s", err ? err->message : cerr->message);
  }
  g_clear_error(&err);
  g_clear_error(&cerr);
  return NULL;
}

/* POST /api/deepcw: fetch the model (once at a time; a model already in
 * place is not fetched again). The answer says what is going on. */
static char *web_deepcw(GHashTable *form, guint *st, gpointer user) {
  (void)form; (void)st;
  Hd *h = user;
  g_mutex_lock(&h->dcw_lock);
  const gboolean start = !h->dcw_model_ok && g_strcmp0(h->dcw_state, "running") != 0;
  if (start) {
    h->dcw_state = "running";
    h->dcw_got = h->dcw_total = 0;
    g_clear_pointer(&h->dcw_err, g_free);
  }
  char *body = g_strdup_printf("{\"ok\":true,\"state\":\"%s\"}",
                               h->dcw_model_ok ? "done" : h->dcw_state);
  g_mutex_unlock(&h->dcw_lock);
  if (start) {
    g_message("deepcw: fetching the model (commit %.7s) into %s, asked for on the web page",
              SKIM_DEEPCW_COMMIT, h->dcw_model);
    if (h->dcw_thread) { g_thread_join(h->dcw_thread); }
    h->dcw_thread = g_thread_new("deepcw-fetch", dcw_fetch_thread, h);
  }
  return body;
}

/* --fetch-deepcw-model: the same, in the foreground. */
static void cli_progress(guint64 got, guint64 total, gpointer user) {
  guint *last = user;
  const guint pct = total ? (guint)(100 * got / total) : 0;
  if (pct / 10 != *last / 10) {
    g_printerr("  %3u %%  (%.1f of %.1f MB)\n", pct, got / 1e6, total / 1e6);
    *last = pct;
  }
}

static int fetch_model_cli(void) {
  char *model = skim_decode_deepcw_model_path();
  GError *err = NULL;
  int rc = 0;
  if (skim_deepcw_model_check(model, NULL)) {
    printf("%s is the pinned DeepCW model (commit %.7s) — nothing to fetch\n", model,
           SKIM_DEEPCW_COMMIT);
  } else {
    char *base = skim_deepcw_env_base_url();
    char *ca = skim_scp_env_ca_path();
    guint last = 0;
    const SkimDeepcwFetch f = {
      .model_path = model, .base_url = base, .ca_path = ca, .progress = cli_progress,
      .user = &last,
    };
    g_printerr("DeepCW model by e04 (AGPL-3.0-only), commit %.7s of %s\n  into %s\n",
               SKIM_DEEPCW_COMMIT, base ? base : SKIM_DEEPCW_UPSTREAM, model);
    if (skim_deepcw_model_fetch(&f, &err)) {
      printf("%s installed (sha256 %.8s…, LICENSE and NOTICE beside it)\n", model,
             SKIM_DEEPCW_MODEL_SHA);
    } else {
      g_printerr("fetch failed: %s\n", err->message);
      g_clear_error(&err);
      rc = 1;
    }
    g_free(base);
    g_free(ca);
  }
  g_free(model);
  return rc;
}

static char *web_log(GHashTable *q, gboolean may, guint *st, const char **type,
                     gpointer user) {
  (void)q; (void)may; (void)st; (void)type; (void)user;
  return skim_log_ring_json();
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

/* Start one mode of a band: its pipeline on the band's IQ, its logs (the
 * CW ones keep their old names, RTTY's carry "-rtty"). learndir NULL = no
 * tap. RTTY spots the feed only with [feed] rtty, and never through the
 * learned gate — its model knows CW. */
static gboolean dec_start(Hd *h, Band *b, guint k, const SkimHdBand *cb, const char *logdir,
                          const char *day, const char *learndir, const char *start,
                          GError **error) {
  const SkimHdConfig *c = &h->cfg;
  Dec *d = &b->dec[k];
  const gboolean rtty = k == M_RTTY;
  const char *sfx = rtty ? "-rtty" : "";
  d->band = b->name;
  d->mode = k;
  d->bin_hz = rtty ? 250.0 : 125.0;
  g_mutex_init(&d->lock);
  d->active = g_hash_table_new_full(NULL, NULL, NULL, g_free);
  d->stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  d->snr = g_array_new(FALSE, FALSE, sizeof(float));
  char *dlog = c->decode_log ? g_strdup_printf("%s/decodes-%s%s-%s.log", logdir, b->name,
                                               sfx, day)
                             : NULL;
  char *tap = learndir ? g_strdup_printf("%s/tap-%s%s-%s.log", learndir, b->name, sfx, start)
                       : NULL;
  SkimPipelineConfig pc = {
    .source = SKIM_PIPELINE_SOURCE_EXTERNAL,
    .host = b->label,
    .iq_rate = c->rate,
    .center_hz = b->centre_hz,
    .mode = rtty ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .cw_engine = cb->cw_engine,
    .decode_log_path = dlog,
    .rbn = rtty && !c->feed_rtty ? NULL : h->feed,
    .rbn_min_score = c->feed_min_score,
    .rbn_min_hearings = c->feed_min_hearings,
    .rbn_settle_s = c->feed_settle_s,
    .rbn_settle_nodict_s = c->feed_settle_nodict_s,
    .rbn_fresh_s = c->feed_fresh_s,
    .rbn_fold_s = c->feed_fold_s,
    .tap_path = tap,
    .feed_gate = rtty ? NULL : h->gate,
    .feed_gate_on = gate_on(h),
    .band = b->name,
    /* RTTY publishes what it copies; CW stays off it only when told to */
    .rtty_pairs = rtty || c->suppress_cw_on_rtty ? b->pairs : NULL,
  };
  d->p = skim_pipeline_new(&pc);
  g_free(dlog);
  g_free(tap);
  skim_pipeline_set_text_cb(d->p, text_cb, d);
  skim_pipeline_set_station_cb(d->p, station_cb, d);
  skim_pipeline_set_station_gone_cb(d->p, gone_cb, d);
  skim_pipeline_set_feed_cb(d->p, feed_cb, d);
  skim_pipeline_set_state_cb(d->p, state_cb, d);
  return skim_pipeline_start(d->p, error);
}

int main(int argc, char **argv) {
  skim_log_ring_install();
  Hd *h = g_new0(Hd, 1);
  h->console_s = -1;
  h->http_port = -1;
  char *cfg = NULL;
  gboolean list_switches = FALSE, check_config = FALSE, fetch_model = FALSE;
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
    { "feed-gate", 0, 0, G_OPTION_ARG_FILENAME, &h->cli_gate,
      "Learned feed gate model for this start instead of the built-in one "
      "(must name every feature; overrides the config)", "FILE" },
    { "feed-gate-mode", 0, 0, G_OPTION_ARG_STRING, &h->cli_gate_mode,
      "off | shadow (default: score and report) | on (hold back the spots "
      "below the threshold); overrides the config", "MODE" },
    { "check-config", 0, 0, G_OPTION_ARG_NONE, &check_config,
      "Read and check the config and the feed gate, print what would run, and "
      "exit (0 = fine, 2 = not) — no radio is touched", NULL },
    { "fetch-deepcw-model", 0, 0, G_OPTION_ARG_NONE, &fetch_model,
      "Fetch the DeepCW model (e04/deepcw-engine, AGPL-3.0-only, ~15 MB) at the "
      "commit this version pins, check its sha256, install it where the "
      "engine looks, and exit (0 = in place)", NULL },
    { "list-switches", 0, 0, G_OPTION_ARG_NONE, &list_switches,
      "Print the SKIM_* environment switches the engine reads, and exit", NULL },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  /* for a restart without a supervisor: the command line as given, less
   * --take-over (one-shot: the first start only) */
  GPtrArray *av = g_ptr_array_new();
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--take-over") != 0) { g_ptr_array_add(av, g_strdup(argv[i])); }
  }
  g_ptr_array_add(av, NULL);
  h->argv = (char **)g_ptr_array_free(av, FALSE);
  GOptionContext *oc = g_option_context_new("— multi-band CW skimmer on a Red Pitaya, no GUI");
  g_option_context_add_main_entries(oc, opts, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return EXIT_CONFIG;
  }
  g_option_context_free(oc);
  if (list_switches) {
    skim_switches_print_known();
    return 0;
  }
  if (fetch_model) { return fetch_model_cli(); }
  h->cfg_path = cfg ? cfg : g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                             "headless.ini", NULL);
  if (!g_file_test(h->cfg_path, G_FILE_TEST_EXISTS) && !check_config) {
    if (!skim_hd_config_write_default(h->cfg_path, &err)) {
      g_printerr("config: %s\n", err->message);
      return EXIT_CONFIG;
    }
    g_message("config: %s did not exist — written with defaults", h->cfg_path);
  }
  SkimHdConfig *c = &h->cfg;
  if (!skim_hd_config_load(c, h->cfg_path, &err)) {
    g_printerr("config: %s\n", err->message);
    return EXIT_CONFIG;
  }
  if (h->console_s < 0) { h->console_s = c->console_s >= 0 ? c->console_s : 10; }
  if (h->http_port < 0) { h->http_port = c->http_port >= 0 ? c->http_port : 8073; }
  h->feed_gate = g_strdup(c->feed_gate);
  h->feed_gate_mode = g_strdup(c->feed_gate_mode);
  if (!gate_load(h, &err)) {
    g_printerr("feed gate: %s\n", err->message);
    return EXIT_CONFIG;
  }
  if (check_config) {
    printf("%s: fine\n  radio  %s, %u kHz\n", h->cfg_path,
           c->source == SKIM_HD_SOURCE_MOCK ? "mock (no radio)" : c->hosts_text,
           c->rate / 1000);
    for (guint i = 0; i < c->nb; i++) {
      char *m = skim_hd_band_modes(&c->band[i], c->engine);
      printf("  RX%u    %-5s %.1f kHz  %s\n", i + 1, c->band[i].name,
             c->band[i].centre_hz / 1000.0, m);
      g_free(m);
    }
    printf("  feed   %s port %d, %s\n  gate   %s, %s (CW)\n",
           c->feed_call && c->feed_call[0] ? c->feed_call : "(no call)", c->feed_port,
           c->feed_rtty ? "CW and RTTY" : "CW only",
           h->gate_id ? h->gate_id : "none", h->feed_gate_mode);
    gboolean dcw = FALSE;
    for (guint i = 0; i < c->nb; i++) {
      dcw |= c->band[i].cw && c->band[i].cw_engine == SKIM_CW_ENGINE_DEEPCW;
    }
    if (dcw) {
      /* the bands asking for DeepCW start anyway — on v2 when it is not here */
      char *model = skim_decode_deepcw_model_path();
      if (!skim_deepcw_model_check(model, &err)) {
        printf("  deepcw NOT AVAILABLE, those bands run v2: %s\n", err->message);
      } else if (!skim_decode_deepcw_available(&err)) {
        printf("  deepcw NOT AVAILABLE, those bands run v2: %s\n", err->message);
      } else {
        printf("  deepcw %s, ONNX Runtime %s\n", model, skim_decode_deepcw_runtime_info());
      }
      g_clear_error(&err);
      g_free(model);
    }
    return 0;
  }
  g_mutex_init(&h->snap_lock);
  g_mutex_init(&h->dcw_lock);
  h->dcw_state = "idle";
  h->dcw_model = skim_decode_deepcw_model_path();
  h->dcw_base_url = skim_deepcw_env_base_url();
  h->dcw_ca = skim_scp_env_ca_path();
  h->dcw_cancel = g_cancellable_new();
  {
    GError *e = NULL;
    h->dcw_model_ok = skim_deepcw_model_check(h->dcw_model, &e);
    if (e) { h->dcw_model_err = g_strdup(e->message); }
    g_clear_error(&e);
  }
  h->t_start = g_get_monotonic_time();
  h->started_us = g_get_real_time();
  g_message("skimmer-headless %s — %s %s, %u band%s × %u kHz, config %s", SKIMMER_VERSION,
            c->source == SKIM_HD_SOURCE_MOCK ? "mock source" : "HPSDR P1",
            c->source == SKIM_HD_SOURCE_MOCK ? "(no radio)" : c->hosts_text, c->nb,
            c->nb == 1 ? "" : "s", c->rate / 1000, h->cfg_path);
  h->switches_json = skim_switches_scan();

  /* Callsign dictionary: loaded once for every pipeline, kept current. */
  char *scp = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux", "master.scp", NULL);
  if (g_file_test(scp, G_FILE_TEST_EXISTS) && !skim_callsign_dict_load(scp, &err)) {
    g_warning("MASTER.SCP %s not loaded: %s", scp, err->message);
    g_clear_error(&err);
  }
  h->scp = skim_scp_updater_new(scp, on_scp, h);
  skim_scp_updater_start(h->scp, 15);
  g_free(scp);

  if (c->feed_port > 0) {
    if (!c->feed_call || !c->feed_call[0]) {
      g_warning("feed: [feed] call is empty — telnet feed off");
    } else {
      h->feed = skim_rbn_feed_new(c->feed_call, (guint16)c->feed_port, &err);
      if (!h->feed) {
        g_warning("feed: port %d: %s — telnet feed off", c->feed_port, err->message);
        g_clear_error(&err);
      } else {
        g_message("feed: telnet spots on port %d as %s", c->feed_port, c->feed_call);
      }
    }
  }

  char *logdir = g_build_filename(g_get_user_data_dir(), "skimmer-for-linux", "headless", NULL);
  GDateTime *now = g_date_time_new_now_local();
  char *day = g_date_time_format(now, "%Y-%m-%d");
  g_date_time_unref(now);
  if (c->decode_log) { g_mkdir_with_parents(logdir, 0755); }
  /* Learning log: a decode tap per band and START (UTC) — a restart begins
   * a fresh tap, as the extractors and the station table begin fresh. Only
   * the tap: the gate rows are four times its size live (2026-10-03, 20 m:
   * 16 MB in 32 min), and skimmer-tap-replay writes them byte for byte from
   * the tap — under any policy, with any features added since. */
  char *learndir = g_build_filename(logdir, "learn", NULL);
  GDateTime *utc = g_date_time_new_now_utc();
  char *start = g_date_time_format(utc, "%Y%m%d-%H%M%S");
  g_date_time_unref(utc);
  if (c->feed_learn) { g_mkdir_with_parents(learndir, 0755); }
  h->nb = c->nb;
  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    const SkimHdBand *cb = &c->band[i];
    g_strlcpy(b->name, cb->name, sizeof(b->name));
    b->centre_hz = cb->centre_hz;
    g_snprintf(b->label, sizeof(b->label), "RX%u %s", i + 1, b->name);
    if (cb->cw && cb->rtty) { b->pairs = skim_rtty_pairs_new(); }
    for (guint k = 0; k < M_N; k++) {
      if (k == M_CW ? !cb->cw : !cb->rtty) { continue; }
      if (!dec_start(h, b, k, cb, logdir, day, c->feed_learn ? learndir : NULL, start,
                     &err)) {
        g_printerr("pipeline %s %s: %s\n", b->name, MODE_KEY[k], err->message);
        return 1;
      }
    }
  }
  if (c->feed_learn) { g_message("learn: decode tap in %s", learndir); }
  if (!h->gate) {
    g_message("feed gate: off");
  } else if (!h->feed) {
    g_message("feed gate: %s — no telnet feed, not used", h->gate_id);
  } else {
    g_message("feed gate: %s, threshold %.2f, %s", h->gate_id,
              skim_feed_gate_threshold(h->gate),
              gate_on(h) ? "ON (holds back the spots below it)" : "SHADOW (logged only)");
  }
  g_clear_pointer(&h->gate, skim_feed_gate_free);   /* the pipelines hold theirs */
  g_free(learndir);
  g_free(start);
  g_free(logdir);
  g_free(day);
  for (char **x = c->rec_bands; x && *x; x++) {
    Band *b = NULL;
    for (guint i = 0; i < h->nb; i++) {
      if (strcmp(h->band[i].name, *x) == 0) { b = &h->band[i]; }
    }
    if (!b) {
      g_warning("record: no band %s in [bands] — not recorded", *x);
      continue;
    }
    g_mkdir_with_parents(c->rec_dir, 0755);
    b->rec = skim_rec_new(c->rec_dir, b->name, b->label, c->ppm, c->rec_at_us,
                          c->rec_minutes);
    if (c->rec_at_us) {
      GDateTime *t = g_date_time_new_from_unix_utc(c->rec_at_us / G_USEC_PER_SEC);
      char *ts = g_date_time_format(t, "%Y-%m-%d %H:%M UTC");
      g_message("record: %s for %.0f min from %s into %s", b->name, c->rec_minutes,
                ts, c->rec_dir);
      g_free(ts);
      g_date_time_unref(t);
    }
  }
  for (guint i = 0; i < h->nb; i++) {
    const Band *b = &h->band[i];
    g_message("decode: %s%s%s%s%s", b->name, b->dec[M_CW].p ? " CW, engine " : "",
              b->dec[M_CW].p ? skim_pipeline_cw_engine_name(b->dec[M_CW].p) : "",
              b->dec[M_RTTY].p ? " + RTTY" : "",
              b->dec[M_RTTY].p && h->feed && !c->feed_rtty ? " (not on the feed)" : "");
    if (b->pairs && c->suppress_cw_on_rtty) {
      g_message("decode: %s CW stays off the RTTY stations RTTY copies", b->name);
    }
  }
  /* A band that asks for DeepCW and runs v2: why, for the log and the page
   * (the pipeline has tried and cached the answer). */
  for (guint i = 0; i < h->nb; i++) {
    const Band *b = &h->band[i];
    if (!b->dec[M_CW].p || c->band[i].cw_engine != SKIM_CW_ENGINE_DEEPCW) { continue; }
    h->dcw_wanted = TRUE;
    if (strcmp(skim_pipeline_cw_engine_name(b->dec[M_CW].p), "deepcw") != 0 && !h->dcw_fallback) {
      GError *e = NULL;
      if (!h->dcw_model_ok) {
        h->dcw_fallback = g_strdup(h->dcw_model_err);
      } else if (!skim_decode_deepcw_available(&e)) {
        h->dcw_fallback = g_strdup(e->message);
      } else {
        h->dcw_fallback = g_strdup("the engine chose v2 (SKIM_CW_ENGINE?)");
      }
      g_clear_error(&e);
      g_warning("decode: DeepCW asked for, not available (%s) — those bands run cw-v2",
                h->dcw_fallback);
    }
  }
  SkimPipeline *fp = feed_pipeline(h);
  if (fp) {
    double fs, fset, ffr, ffo;
    guint fh;
    skim_pipeline_rbn_policy(fp, &fs, &fh, &fset, &ffr);
    skim_pipeline_rbn_fold(fp, &ffo, NULL);
    g_message("feed: spots a call at score >= %.2f, read %u times (or in "
              "MASTER.SCP), after %.0f s settled, last read <= %.0f s ago "
              "(0 = any), twins of a call sent <= %.0f s ago held back "
              "(0 = off)", fs, fh, fset, ffr, ffo);
  }

  const char *names[SKIM_HD_MAX_BANDS];
  for (guint i = 0; i < h->nb; i++) { names[i] = h->band[i].name; }
  char *stats_path = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux",
                                      "stats.bin", NULL);
  h->stats = skim_stats_open(stats_path, names, h->nb);
  g_message("stats: %s (7 days, a record a minute)", stats_path);
  g_free(stats_path);

  SkimHdWeb *web = NULL;
  if (h->http_port > 0) {
    const SkimHdWebHooks hooks = {
      .status = web_status, .stats = web_stats, .log = web_log, .config = web_config,
      .config_post = web_config_post, .restart = web_restart, .rollback = web_rollback,
      .deepcw = web_deepcw,
      .user = h,
    };
    web = skim_hd_web_new((guint16)h->http_port, (const char *const *)c->settings_from,
                          &hooks, &err);
    if (!web) {
      g_warning("web: port %d: %s — web page off", h->http_port, err->message);
      g_clear_error(&err);
    } else {
      char *from = g_strjoinv(", ", c->settings_from);
      g_message("web: http://<this machine>:%d/ (JSON: /status.json); settings "
                "from %s", h->http_port, from);
      g_free(from);
    }
  }

  h->loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGINT, on_signal, h);
  g_unix_signal_add(SIGTERM, on_signal, h);
  h->src = skim_iq_source_new(c, h->take_over, rx_iq_cb, h);
  skim_iq_source_start(h->src);                /* first start attempt now     */
  tick(h);
  if (!h->exit_code) {                         /* a quit before run is lost   */
    g_timeout_add_seconds(1, tick, h);
    g_main_loop_run(h->loop);
  }

  /* Teardown: the radio first (stop — only if the stream is still ours),
   * then the engines, then the feed they spot into. */
  g_clear_pointer(&web, skim_hd_web_free);
  g_cancellable_cancel(h->dcw_cancel);         /* a fetch in flight stops      */
  if (h->dcw_thread) { g_thread_join(h->dcw_thread); }
  g_clear_pointer(&h->src, skim_iq_source_free);
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) { skim_pipeline_free(h->band[i].dec[k].p); }
    g_clear_pointer(&h->band[i].pairs, skim_rtty_pairs_unref);
    skim_rec_free(h->band[i].rec);
  }
  g_clear_pointer(&h->stats, skim_stats_free);  /* saves                       */
  g_clear_pointer(&h->feed, skim_rbn_feed_free);
  g_clear_pointer(&h->scp, skim_scp_updater_free);
  if (h->exit_code == EXIT_RESTART && g_strcmp0(g_getenv("SKIM_SUPERVISED"), "1") != 0) {
    g_message("restart: starting again");
    fflush(stdout);
    execv("/proc/self/exe", h->argv);
    g_printerr("restart: exec failed: %s — exiting %d\n", g_strerror(errno), EXIT_RESTART);
  }
  g_message("stopped");
  return h->exit_code;
}
