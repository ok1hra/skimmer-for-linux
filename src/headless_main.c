/*
 * skimmer-headless — multi-band skimmer without a GUI.
 *
 *   skimmer-headless [--config FILE] [--take-over] [--status-every S]
 *                    [--http-port N] [--feed-gate FILE]
 *                    [--feed-gate-mode off|shadow|on] [--check-config]
 *                    [--list-switches] [--fetch-deepcw-model]
 *                    [--install-service] [--uninstall-service] [--find-radios]
 *                    [--find-skimmers]
 *                    [--redpitaya connect|probe|status|install|uninstall [HOST]]
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
 * Exit codes: 0 stopped (SIGTERM / Ctrl+C), 2 the config is wrong, 3 no
 * free radio / the stream was lost, 75 a restart for a supervisor
 * (SKIM_SUPERVISED=1). --install-service writes a systemd user unit built
 * on them (headless/service.h): 3 and crashes start again after a minute,
 * 2 stays down. A restart the web page asks for re-executes the program in
 * place (same PID — systemd sees nothing) unless SKIM_SUPERVISED=1 asks
 * for exit 75. Under systemd the console table is off (the journal is no
 * place for it; the web page and /status.json have it).
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
#include <termios.h>
#include <unistd.h>

#include "headless/archive.h"
#include "headless/spotlog.h"
#include "headless/config.h"
#include "headless/deepcw_model.h"
#include "headless/hub.h"
#include "headless/discovery.h"
#include "headless/governor.h"
#include "headless/json.h"
#include "headless/logring.h"
#include "headless/peers.h"
#include "headless/record.h"
#include "headless/scp_update.h"
#include "headless/update.h"
#include "headless/service.h"
#include "headless/source.h"
#include "engine/hpsdr_p1.h"
#include "headless/stats.h"
#include "headless/switches.h"
#include "headless/waterfall.h"
#include "headless/wspr.h"
#include "headless/ft8.h"
#include "headless/js8.h"
#include "headless/pskreporter.h"
#include "headless/skimsrv.h"
#include "headless/rp_manage.h"
#include "headless/visitors.h"
#include "headless/web.h"
#include "engine/callsign.h"
#include "engine/decode_deepcw.h"
#include "engine/decode_cw_v4.h"
#include "engine/dcw4_kernel.h"
#include "engine/pipeline.h"
#include "engine/band_combine.h"
#include "headless/phase_hist.h"
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
enum { M_CW = 0, M_RTTY = 1, M_N = 2 };     /* the pipelines; FT8 is SKIM_STATS_FT8 */
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
  volatile gint paused;        /* governor.h: no IQ pushed while set          */
} Dec;

typedef struct {
  char          name[16];
  double        centre_hz;
  char          label[96];
  Dec           dec[M_N];
  SkimRttyPairs *pairs;        /* CW and RTTY: the pairs RTTY copies (or NULL) */
  volatile guint64 frames;     /* source thread: frames received             */
  guint64       frames_prev;   /* main loop: for kS/s                         */
  double        ksps;
  Rec          *rec;           /* [record]: this band's IQ tap, or NULL       */
  const char   *worker;        /* "@name": decoded by that worker, or NULL    */
  volatile gint remote_spots[M_N]; /* … the spots it sent, CW / RTTY (link thread) */
  gint          remote_prev[M_N];   /* main loop: at the last minute's end    */
  guint         shed_s;        /* main loop: the minute's s the thermal guard had it off */
  volatile gint ft8_paused;    /* the thermal guard has the band off: no FT8 (nor JS8) either */
  /* a phased band's own IQ from both antennas (band_combine.h; source thread) */
  SkimBandCombine *bc;
  float        *bc_y;          /* the combination of the block in hand        */
  guint         bc_cap;
  float        *bc_ca, *bc_cb; /* the pipeline's coefficients, last copied    */
  gint64        bc_next_us;
  gboolean      bc_on;         /* not A alone: the combination goes out       */
  SkimPhaseHist *phist;        /* main loop: what phasing gave, kept (phase_hist.h) */
} Band;

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
  char    *cfg_text;           /* the config this start runs (what a worker's
                                * fragment is cut from)                      */

  Band     band[SKIM_HD_MAX_BANDS];
  guint    nb;

  SkimIqSource      *src;
  SkimIqSourceStatus src_st;   /* main loop: refreshed every tick             */
  guint64   packets_prev;
  double    pps;
  int       exit_code;         /* != 0: stop the main loop with it            */
  SkimRbnFeed    *feed;
  SkimRbnFeed    *rbn;         /* [rbn]: the Aggregator's port (delayed)      */
  SkimHdPeers    *peers;       /* [peers]: other skimmers' feeds, aggregated  */
  SkimHdDiscovery *disco;      /* answers the LAN's "who is there"            */
  SkimHdHub      *hub;         /* [hub]: bands passed to workers              */
  SkimHdSpotLog  *spot_log;    /* the spots the feed sent (the page's map, DXC) */
  char           *spot_path;   /* … kept there across a restart               */
  SkimScpUpdater *scp;
  SkimHdUpdater  *upd;         /* [update]: newer releases (update.h)         */
  SkimHdUpdateEnv upd_env;     /* … this process, as the updater sees it      */
  char           *upd_state;   /* … its state file                            */
  char           *exec_path;   /* a restart execs this (the updated AppImage)
                                * instead of /proc/self/exe                   */
  GMainLoop      *loop;
  gint64          t_start;
  guint64         cpu_prev;    /* clock ticks, utime + stime                  */
  gint64          cpu_prev_us;
  double          cpu_pct;
  guint64         cpu_mode_prev[M_N];   /* ns, the pipelines' engine threads */
  double          cpu_mode_pct[M_N];    /* CW, RTTY: % of one core           */
  GPtrArray      *temp_paths;  /* the CPU's temperature sensors (°C × 1000)  */
  double          temp_c;      /* the hottest of them now; NAN = none        */
  float           temp_peak;   /* … the minute's hottest (the history)       */
  guint           guard_s;     /* the minute's s the thermal guard watched   */
  guint           ticks;

  SkimStats *stats;            /* the 7-day history (stats.h)                 */
  SkimHdGovernor *gov;         /* adaptive RTTY: what pauses on its own       */
  SkimHdVisitors *vis;         /* [visitors]: sign-in and the visitors' log   */
  SkimHdArchive *archive;      /* [archive]: every spot, a file a day; NULL   */
  SkimHdWaterfall *wf;         /* the page's waterfall (idle while unwatched) */
  SkimHdWspr *wspr;            /* [wspr]: WSPR decoded beside; NULL = off     */
  SkimHdFt8  *ft8;             /* [ft8]: FT8/FT4 decoded beside; NULL = off   */
  SkimHdJs8  *js8;             /* [js8]: JS8 decoded beside; NULL = off       */
  SkimPskr   *pskr;            /* [ft8]/[js8] upload: to PSK Reporter; NULL = off */
  char      *archive_err;      /* the last flush error logged (no repeats)    */
  gint64    minute;            /* the unix minute being collected             */
  guint64   min_frames_prev, min_lost_prev;
  guint64   min_cpu_prev, min_mode_prev[M_N];  /* CPU at the last minute's end  */
  guint64   min_child_prev;                     /* … the helper programs'        */
  gint64    min_cpu_us;                         /* … and when (0 = not yet)      */
  guint     minutes;           /* minutes recorded since start                */

  GMutex   snap_lock;          /* the web page's copy of the status           */
  char    *snap_json;

  /* DeepCW: the model on disk, why a band that asks for it runs v2, and a
   * fetch the web page started (on its own thread, under dcw_lock) */
  gboolean      dcw_wanted;    /* a band names deepcw                         */
  gboolean      v4_wanted;     /* a band decodes with CW v4                   */
  /* CW v4's compute check (v4_bench_*): the measurement, cached per CPU,
   * encoder, threads and version in <state>/v4-bench.ini */
  GMutex        v4b_lock;
  const char   *v4b_state;     /* "none", "running", "done", "failed"         */
  double        v4b_wps;       /* windows a second on v4b_threads             */
  guint         v4b_threads;
  char          v4b_isa[16];
  gint64        v4b_at;        /* unix s                                      */
  GThread      *v4b_thread;
  float         v4_peak;       /* the week's most CW decoders active at once  */
  guint64       v4_ticks_prev, v4_skipped_prev;   /* the minute's late share    */
  gint64        v4_peak_minute;
  char         *dcw_model;     /* where the engine looks for it               */
  char         *dcw_base_url;  /* $SKIM_DEEPCW_URL, NULL = upstream           */
  char         *dcw_ca;        /* TLS trust store (AppImage)                  */
  char         *dcw_fallback;  /* why the bands asking for it run v3          */
  GMutex        dcw_lock;
  gboolean      dcw_model_ok;
  char         *dcw_model_err;
  const char   *dcw_state;     /* idle | running | done | failed              */
  guint64       dcw_got, dcw_total;
  char         *dcw_err;
  GThread      *dcw_thread;
  GCancellable *dcw_cancel;
  SkimHdRp     *rp;            /* the Red Pitaya over SSH (rp_manage.h)       */
  guint         rp_tick;       /* seconds since its last status read          */
  gboolean      rp_probed;
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

/* [filter] for the pipelines (engine threads): the config never changes
 * while they run. */
static gboolean call_pass(const char *call, gpointer user) {
  return skim_hd_config_call_pass(&((Hd *)user)->cfg, call);
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

/* A phased band's own IQ (the waterfall, FT8/JS8/WSPR): the band-level
 * combination of A and B with the coefficients the band's pipeline
 * published (once a second), or A itself while they are A alone (mode A or
 * MRC, B missing) — so switching phasing off shows A again at once. */
static const float *phase_band_iq(Band *b, const float *a, const float *bb, guint n,
                                  double rate) {
  SkimPipeline *p = b->dec[M_CW].p ? b->dec[M_CW].p : b->dec[M_RTTY].p;
  if (!p)
    return a;
  const gint64 now = g_get_monotonic_time();
  if (now >= b->bc_next_us) {
    b->bc_next_us = now + G_USEC_PER_SEC;
    if (!b->bc_ca) {
      b->bc_ca = g_new(float, 2 * 16384);
      b->bc_cb = g_new(float, 2 * 16384);
    }
    double first = 0, spacing = 0;
    const guint nc = skim_pipeline_phase_band_coef(p, b->bc_ca, b->bc_cb, 16384, &first, &spacing);
    gboolean only_a = TRUE;
    for (guint k = 0; k < nc && only_a; k++) {
      only_a = b->bc_ca[2 * k] == 1.0f && b->bc_ca[2 * k + 1] == 0.0f &&
               b->bc_cb[2 * k] == 0.0f && b->bc_cb[2 * k + 1] == 0.0f;
    }
    const gboolean on = nc > 0 && !only_a;
    if (on) {
      if (!b->bc) { b->bc = skim_band_combine_new(rate, 0); }
      skim_band_combine_set_weights(b->bc, b->bc_ca, b->bc_cb, nc, first, spacing);
    }
    if (on && !b->bc_on && b->bc) { skim_band_combine_reset(b->bc); }   /* fresh */
    b->bc_on = on;
  }
  if (!b->bc_on || !bb || !b->bc)
    return a;
  if (b->bc_cap < n) {
    g_free(b->bc_y);
    b->bc_y = g_new(float, 2 * (gsize)n);
    b->bc_cap = n;
  }
  skim_band_combine_process(b->bc, a, bb, b->bc_y, n);
  return b->bc_y;
}

/* Source thread: one block of band i to its pipeline (and its recording);
 * iq_b: a phased band's antenna B (docs/PHASING.md; NULL = none or missing)
 * — only the pipelines take it, the rest of the band runs on A for now. */
static void rx_iq2_cb(guint i, const float *iq, const float *iq_b, guint n, double rate,
                      double centre, gpointer user) {
  Hd *h = user;
  Band *b = &h->band[i];
  b->frames += n;
  if (b->rec) { skim_rec_push(b->rec, iq, n, rate, centre); }
  const float *band_iq = iq;                   /* what the band's own IQ is */
  if (h->cfg.band[i].phased) { band_iq = phase_band_iq(b, iq, iq_b, n, rate); }
  skim_hd_waterfall_push(h->wf, i, band_iq, n, rate, centre);   /* nothing unless watched */
  skim_hd_wspr_push(h->wspr, i, band_iq, n);   /* nothing without a WSPR dial */
  if (h->ft8 && !g_atomic_int_get(&b->ft8_paused)) {   /* … an FT8/FT4 dial; off with the */
    skim_hd_ft8_push(h->ft8, i, band_iq, n);            /* thermal guard's band          */
  }
  if (h->js8 && !g_atomic_int_get(&b->ft8_paused)) {   /* … a JS8 dial, the same       */
    skim_hd_js8_push(h->js8, i, band_iq, n);
  }
  if (b->worker) {                             /* decoded on another PC     */
    if (h->hub) { skim_hd_hub_push(h->hub, i, iq, n, rate, centre); }
    return;
  }
  const gboolean phased = h->cfg.band[i].phased;
  for (guint k = 0; k < M_N; k++) {
    if (b->dec[k].p && !g_atomic_int_get(&b->dec[k].paused)) {
      if (phased) {
        skim_pipeline_push2(b->dec[k].p, iq, iq_b, n, rate, centre);
      } else {
        skim_pipeline_push(b->dec[k].p, iq, n, rate, centre);
      }
    }
  }
}

static void rx_iq_cb(guint i, const float *iq, guint n, double rate, double centre,
                     gpointer user) {
  rx_iq2_cb(i, iq, NULL, n, rate, centre, user);
}

/* A phased band's state for status.json: "phase":{…} or null. */
static void phase_json(GString *json, Band *b) {
  SkimPipeline *p = b->dec[M_CW].p ? b->dec[M_CW].p : b->dec[M_RTTY].p;
  SkimPipelinePhase ph;
  if (!p || !skim_pipeline_phase_snapshot(p, &ph, NULL, NULL, NULL, 0)) {
    g_string_append(json, "null");
    return;
  }
  const SkimPhaseSummary *u = &ph.sum;
  const double v[] = { u->gain_db, u->gain_a_db, u->drop_db, u->drop_p90_db, u->drop_share,
                       u->coherence, u->dmax_db, u->share_b, u->same_dir, u->gain_est_db,
                       u->drop_est_db };
  static const char *const k[] = { "gain_db", "gain_a_db", "drop_db", "drop_p90_db", "drop_share",
                                   "coherence", "dmax_db", "share_b", "same_dir", "gain_est_db",
                                   "drop_est_db" };
  g_string_append_printf(json, "{\"mode\":\"%s\",\"b_missing\":%s,\"est\":%s,"
                         "\"updates\":%" G_GUINT64_FORMAT ",\"stations\":%u",
                         skim_phase_mode_name(ph.mode), ph.b_missing ? "true" : "false",
                         ph.est ? "true" : "false", ph.updates, u->stations);
  for (guint i = 0; i < G_N_ELEMENTS(v); i++) {
    if (isnan(v[i])) {
      g_string_append_printf(json, ",\"%s\":null", k[i]);
    } else {
      g_string_append_printf(json, ",\"%s\":%.2f", k[i], v[i]);
    }
  }
  g_string_append_c(json, '}');
}

/* Why the governor pauses mode k of band i now (NULL: it runs). */
static const char *paused_why(const Hd *h, guint i, guint k) {
  if (h->gov && skim_hd_governor_band_shed(h->gov, i)) { return "thermal"; }
  if (k == M_CW && h->gov && skim_hd_governor_band_load_shed(h->gov, i)) { return "v4-load"; }
  if (k == M_RTTY && h->gov && !skim_hd_governor_rtty_on(h->gov)) { return "rtty-adaptive"; }
  return NULL;
}

/* The pipelines' paused flags as the governor has them now. */
static void pause_apply(Hd *h) {
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) {
      if (h->band[i].dec[k].p) { g_atomic_int_set(&h->band[i].dec[k].paused, paused_why(h, i, k) != NULL); }
    }
    g_atomic_int_set(&h->band[i].ft8_paused, h->gov && skim_hd_governor_band_shed(h->gov, i));
  }
}

/* ---- status ---------------------------------------------------------------------- */

/* Clock ticks of this process (utime + stime), or with children = TRUE of
 * the children it waited for (cutime + cstime: the helper programs). */
static guint64 cpu_ticks_of(gboolean children) {
  char buf[1024];
  FILE *f = fopen("/proc/self/stat", "r");
  if (!f) { return 0; }
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  const char *p = strrchr(buf, ')');           /* comm may contain spaces      */
  unsigned long ut = 0, st = 0;
  long cut = 0, cst = 0;
  if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %ld %ld",
                   &ut, &st, &cut, &cst) != 4) {
    return 0;
  }
  return children ? (guint64)MAX(cut, 0) + (guint64)MAX(cst, 0) : (guint64)ut + st;
}
static guint64 cpu_ticks(void) { return cpu_ticks_of(FALSE); }

/* The CPU's temperature sensors, found once at start: the inputs of a CPU's
 * hwmon chip (coretemp, k10temp, zenpower, cpu_thermal …), else a CPU's
 * thermal zone (x86_pkg_temp, cpu…, soc…). Empty when the machine has none
 * — no temperature, not a wrong one (acpitz and the disks are not the CPU). */
static GPtrArray *temp_sensors(void) {
  static const char *const CHIPS[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal",
                                       "cpu-thermal", "soc_thermal", "cpu", NULL };
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  GDir *d = g_dir_open("/sys/class/hwmon", 0, NULL);
  for (const char *e; d && (e = g_dir_read_name(d));) {
    char *np = g_build_filename("/sys/class/hwmon", e, "name", NULL), *name = NULL;
    if (g_file_get_contents(np, &name, NULL, NULL) && g_strv_contains(CHIPS, g_strstrip(name))) {
      char *dir = g_build_filename("/sys/class/hwmon", e, NULL);
      GDir *hd = g_dir_open(dir, 0, NULL);
      for (const char *f; hd && (f = g_dir_read_name(hd));) {
        if (g_str_has_prefix(f, "temp") && g_str_has_suffix(f, "_input")) {
          g_ptr_array_add(out, g_build_filename(dir, f, NULL));
        }
      }
      if (hd) { g_dir_close(hd); }
      g_free(dir);
    }
    g_free(name);
    g_free(np);
  }
  if (d) { g_dir_close(d); }
  if (out->len) { return out; }
  d = g_dir_open("/sys/class/thermal", 0, NULL);
  for (const char *e; d && (e = g_dir_read_name(d));) {
    if (!g_str_has_prefix(e, "thermal_zone")) { continue; }
    char *tp = g_build_filename("/sys/class/thermal", e, "type", NULL), *type = NULL;
    if (g_file_get_contents(tp, &type, NULL, NULL)) {
      g_strstrip(type);
      if (strcmp(type, "x86_pkg_temp") == 0 || g_str_has_prefix(type, "cpu") ||
          g_str_has_prefix(type, "soc")) {
        g_ptr_array_add(out, g_build_filename("/sys/class/thermal", e, "temp", NULL));
      }
    }
    g_free(type);
    g_free(tp);
  }
  if (d) { g_dir_close(d); }
  return out;
}

static double read_milli_c(const char *path) {
  char *t = NULL;
  double v = NAN;
  if (g_file_get_contents(path, &t, NULL, NULL)) {
    char *end = NULL;
    const double x = g_ascii_strtod(t, &end) / 1000.0;
    if (end != t && x > 0 && x < 200) { v = x; }
  }
  g_free(t);
  return v;
}

/* The lowest critical temperature of those sensors, °C (NAN: none says):
 * a hwmon input's tempN_crit, a thermal zone's "critical" trip point. */
static double cpu_crit_c(const GPtrArray *paths) {
  double crit = NAN;
  for (guint i = 0; paths && i < paths->len; i++) {
    const char *p = paths->pdata[i];
    if (g_str_has_suffix(p, "_input")) {
      char *c = g_strndup(p, strlen(p) - 6);
      char *cp = g_strconcat(c, "_crit", NULL);
      const double v = read_milli_c(cp);
      if (!isnan(v) && !(v >= crit)) { crit = v; }
      g_free(cp);
      g_free(c);
      continue;
    }
    char *dir = g_path_get_dirname(p);                 /* a thermal zone       */
    for (int k = 0; k < 16; k++) {
      char *tp = g_strdup_printf("%s/trip_point_%d_type", dir, k), *type = NULL;
      const gboolean got = g_file_get_contents(tp, &type, NULL, NULL);
      if (got && strcmp(g_strstrip(type), "critical") == 0) {
        char *vp = g_strdup_printf("%s/trip_point_%d_temp", dir, k);
        const double v = read_milli_c(vp);
        if (!isnan(v) && !(v >= crit)) { crit = v; }
        g_free(vp);
      }
      g_free(type);
      g_free(tp);
      if (!got) { break; }
    }
    g_free(dir);
  }
  return crit;
}

/* The hottest of them now, °C (NAN: none, or none readable). */
static double cpu_temp_c(const GPtrArray *paths) {
  double hot = NAN;
  for (guint i = 0; paths && i < paths->len; i++) {
    char buf[32];
    FILE *f = fopen(paths->pdata[i], "r");
    if (!f) { continue; }
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *end = NULL;
    const double v = g_ascii_strtod(buf, &end) / 1000.0;
    if (end != buf && v > -50 && v < 200 && !(v <= hot)) { hot = v; }
  }
  return hot;
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
  if (!h->feed && !h->rbn) { return NULL; }
  for (guint i = 0; i < h->nb; i++) {
    if (h->band[i].dec[M_CW].p) { return h->band[i].dec[M_CW].p; }
  }
  return h->cfg.feed_rtty && h->nb ? h->band[0].dec[M_RTTY].p : NULL;
}

static void json_opt_str(GString *o, const char *v);

/* One mode of a band as JSON fields (no braces): what decodes it, its
 * counters and its newest stations. A mode not decoded gives zeros. */
static void dec_json(GString *json, Dec *d, const char *paused, gint64 now) {
  BandStat s;
  band_stat(d, &s);
  g_string_append(json, "\"paused\":");
  json_opt_str(json, paused);
  g_string_append_printf(json,
      ",\"engine\":\"%s\",\"decoders\":%u,\"active\":%u,\"stations\":%u,"
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
  return e == SKIM_CW_ENGINE_DEEPCW ? "deepcw" : e == SKIM_CW_ENGINE_V1 ? "v1"
         : e == SKIM_CW_ENGINE_V3 ? "v3" : e == SKIM_CW_ENGINE_V4 ? "v4" : "v2";
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

/* ---- CW v4's compute check --------------------------------------------------- */

static char *v4_bench_path(void) {
  return g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "v4-bench.ini", NULL);
}

/* what a measurement holds for: this CPU, encoder, thread count, build */
static char *v4_bench_key(guint threads) {
  char *cpu = NULL, *model = g_strdup("?");
  if (g_file_get_contents("/proc/cpuinfo", &cpu, NULL, NULL)) {
    char *m = strstr(cpu, "model name");
    char *colon = m ? strchr(m, ':') : NULL;
    char *e = colon ? strchr(colon, '\n') : NULL;
    if (colon && e) {
      g_free(model);
      model = g_strstrip(g_strndup(colon + 1, (gsize)(e - colon - 1)));
    }
    g_free(cpu);
  }
  const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
  char *key = g_strdup_printf("%s|%s|%u|%s%s", model,
                              net ? skim_dcw4_isa_name(skim_dcw4_net_isa(net)) : "none", threads,
                              SKIMMER_VERSION, SKIMMER_BUILD);
  g_free(model);
  return key;
}

/* the cached measurement, if it is this machine's; TRUE when it was */
static gboolean v4_bench_load(Hd *h) {
  char *path = v4_bench_path();
  GKeyFile *kf = g_key_file_new();
  gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
  const guint threads = skim_decode_cw_v4_workers();
  char *key = v4_bench_key(threads);
  char *have = ok ? g_key_file_get_string(kf, "bench", "key", NULL) : NULL;
  ok = ok && g_strcmp0(have, key) == 0;
  if (ok) {
    g_mutex_lock(&h->v4b_lock);
    h->v4b_wps = g_key_file_get_double(kf, "bench", "windows_per_s", NULL);
    h->v4b_threads = threads;
    char *isa = g_key_file_get_string(kf, "bench", "isa", NULL);
    g_strlcpy(h->v4b_isa, isa ? isa : "", sizeof h->v4b_isa);
    g_free(isa);
    h->v4b_at = g_key_file_get_int64(kf, "bench", "at", NULL);
    h->v4b_state = h->v4b_wps > 0 ? "done" : "none";
    ok = h->v4b_wps > 0;
    g_mutex_unlock(&h->v4b_lock);
  }
  g_free(have);
  g_free(key);
  g_key_file_free(kf);
  g_free(path);
  return ok;
}

static gpointer v4_bench_thread(gpointer user) {
  Hd *h = user;
  const guint threads = skim_decode_cw_v4_workers();
  const double wps = skim_decode_cw_v4_bench(threads, 2.0);
  const SkimDcw4Net *net = skim_dcw4_net_shared(NULL);
  const char *isa = net ? skim_dcw4_isa_name(skim_dcw4_net_isa(net)) : "none";
  const gint64 at = g_get_real_time() / G_USEC_PER_SEC;
  g_mutex_lock(&h->v4b_lock);
  h->v4b_wps = wps;
  h->v4b_threads = threads;
  g_strlcpy(h->v4b_isa, isa, sizeof h->v4b_isa);
  h->v4b_at = at;
  h->v4b_state = wps > 0 ? "done" : "failed";
  g_mutex_unlock(&h->v4b_lock);
  if (wps > 0) {
    GKeyFile *kf = g_key_file_new();
    char *key = v4_bench_key(threads);
    g_key_file_set_string(kf, "bench", "key", key);
    g_key_file_set_double(kf, "bench", "windows_per_s", wps);
    g_key_file_set_string(kf, "bench", "isa", isa);
    g_key_file_set_int64(kf, "bench", "at", at);
    char *path = v4_bench_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
      g_warning("cw-v4: the compute check not saved: %s", err->message);
      g_clear_error(&err);
    }
    g_free(dir);
    g_free(path);
    g_free(key);
    g_key_file_free(kf);
  }
  g_message("cw-v4: compute check — %.0f windows/s on %u thread%s (%s): %.0f keyed channels "
            "in real time", wps, threads, threads == 1 ? "" : "s", isa,
            wps * skim_decode_cw_v4_step_s());
  return NULL;
}

/* a measurement now, unless one runs; FALSE when one already did */
static gboolean v4_bench_start(Hd *h) {
  g_mutex_lock(&h->v4b_lock);
  const gboolean go = g_strcmp0(h->v4b_state, "running") != 0;
  if (go) { h->v4b_state = "running"; }
  g_mutex_unlock(&h->v4b_lock);
  if (!go) { return FALSE; }
  if (h->v4b_thread) { g_thread_join(h->v4b_thread); }
  h->v4b_thread = g_thread_new("cw-v4-check", v4_bench_thread, h);
  return TRUE;
}

/* "v4": CW v4's network, its load and the compute check, flat (the public
 * filter drops the object whole): what runs, the windows, the ticks a
 * channel skipped because its last window was still waiting (the load
 * signal), the workers' CPU; the measurement, the capacity it gives (keyed
 * channels in real time), the need (the week's most CW decoders active at
 * once × 1.5 — the gate opens on lines that never make text, too; with no
 * history 150 a CW band) and the verdict: ok under half the capacity,
 * tight up to it, short over it. */
static void v4_json(Hd *h, GString *o) {
  gboolean any = FALSE;
  guint cw_bands = 0;
  for (guint i = 0; i < h->nb; i++) {
    const Band *b = &h->band[i];
    any |= b->dec[M_CW].p && strcmp(skim_pipeline_cw_engine_name(b->dec[M_CW].p), "cw-v4") == 0;
    cw_bands += b->dec[M_CW].p != NULL;
  }
  GError *e = NULL;
  const gboolean ok = skim_decode_cw_v4_available(&e);
  char *rt = ok ? skim_decode_cw_v4_runtime_info() : NULL;
  SkimCwV4Stats s;
  skim_decode_cw_v4_stats(&s, FALSE);
  if (h->stats && h->v4_peak_minute != h->minute) {
    h->v4_peak = skim_stats_peak_sum(h->stats, h->minute, SKIM_STATS_MINUTES, 0, SKIM_SM_ACTIVE);
    h->v4_peak_minute = h->minute;
  }
  g_mutex_lock(&h->v4b_lock);
  const double cap = h->v4b_wps * skim_decode_cw_v4_step_s();
  const double need = isnan(h->v4_peak) ? 150.0 * cw_bands : ceil(h->v4_peak * 1.5);
  const char *verdict = !ok || h->v4b_wps <= 0 ? "unknown"
                        : need <= 0.5 * cap   ? "ok"
                        : need <= cap         ? "tight"
                                              : "short";
  g_string_append_printf(o, "{\"running\":%s,\"runtime\":", any ? "true" : "false");
  json_opt_str(o, rt);
  g_string_append(o, ",\"error\":");
  json_opt_str(o, ok ? NULL : e->message);
  g_string_append_printf(o, ",\"workers\":%u,\"windows\":%" G_GUINT64_FORMAT ",\"ticks\":%"
                         G_GUINT64_FORMAT ",\"skipped\":%" G_GUINT64_FORMAT ",\"queued\":%u,"
                         "\"cpu_s\":%.1f,\"bench_state\":\"%s\",\"bench_wps\":%.0f,"
                         "\"bench_threads\":%u,\"bench_isa\":\"%s\",\"bench_at\":%" G_GINT64_FORMAT
                         ",\"capacity\":%.0f,\"need\":%.0f,\"peak_active\":",
                         s.workers, s.windows, s.ticks, s.skipped, s.queued, s.cpu_ns / 1e9,
                         h->v4b_state, h->v4b_wps, h->v4b_threads, h->v4b_isa, h->v4b_at, cap,
                         need);
  if (isnan(h->v4_peak)) {
    g_string_append(o, "null");
  } else {
    g_string_append_printf(o, "%.0f", h->v4_peak);
  }
  g_string_append_printf(o, ",\"verdict\":\"%s\"}", verdict);
  g_mutex_unlock(&h->v4b_lock);
  g_clear_error(&e);
  g_free(rt);
}

/* GET /api/phase?band=: a phased band's line for the waterfall's spectrum —
 * the snapshot's rows (pipeline.h: gain, drop, dmax; one value a channel in
 * frequency order, 0.25 dB steps, -128 = none) and where they lie. No
 * addresses or machine readings: the same for everyone who sees the data. */
static char *web_phase_get(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                           gpointer user) {
  (void)who; (void)type;
  Hd *h = user;
  const char *band = q ? g_hash_table_lookup(q, "band") : NULL;
  for (guint i = 0; band && i < h->nb; i++) {
    if (strcmp(h->band[i].name, band) != 0)
      continue;
    Band *b = &h->band[i];
    SkimPipeline *p = b->dec[M_CW].p ? b->dec[M_CW].p : b->dec[M_RTTY].p;
    SkimPipelinePhase ph;
    gint8 *rows = g_new(gint8, 3 * 16384);
    if (!p || !skim_pipeline_phase_snapshot(p, &ph, rows, rows + 16384, rows + 32768, 16384)) {
      g_free(rows);
      return g_strdup("{\"phased\":false}");
    }
    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "{\"phased\":true,\"band\":\"%s\",\"mode\":\"%s\",\"est\":%s,"
                           "\"b_missing\":%s,\"centre_hz\":%.0f,\"first_hz\":%.1f,"
                           "\"spacing_hz\":%.1f", b->name, skim_phase_mode_name(ph.mode),
                           ph.est ? "true" : "false", ph.b_missing ? "true" : "false",
                           ph.centre_hz, ph.first_hz, ph.spacing_hz);
    static const char *const key[3] = { "gain", "drop", "dmax" };
    const guint n = MIN(ph.nchan, 16384u);
    for (guint r = 0; r < 3; r++) {
      g_string_append_printf(o, ",\"%s\":[", key[r]);
      for (guint k = 0; k < n; k++) {
        g_string_append_printf(o, "%s%d", k ? "," : "", rows[r * 16384 + k]);
      }
      g_string_append_c(o, ']');
    }
    g_string_append_c(o, '}');
    g_free(rows);
    return g_string_free(o, FALSE);
  }
  *st = 404;
  return g_strdup("{\"error\":\"no such band\"}");
}

/* GET /api/phase_hist?band=&hours=&end=: a phased band's coarse waterfall of
 * what phasing gave (phase_hist.h) for the Statistics page. */
static char *web_phase_hist(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                            gpointer user) {
  (void)who; (void)type;
  Hd *h = user;
  const char *band = q ? g_hash_table_lookup(q, "band") : NULL;
  const char *hs = q ? g_hash_table_lookup(q, "hours") : NULL;
  const char *es = q ? g_hash_table_lookup(q, "end") : NULL;
  const guint hours = hs ? (guint)CLAMP(g_ascii_strtoull(hs, NULL, 10), 1, 2160) : 24;
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  const gint64 end = es && *es ? MIN((gint64)g_ascii_strtoll(es, NULL, 10), now) : now;
  for (guint i = 0; band && i < h->nb; i++) {
    if (strcmp(h->band[i].name, band) == 0) {
      if (!h->band[i].phist) { return g_strdup("{\"rows\":0}"); }
      return skim_phase_hist_json(h->band[i].phist, end, hours, 720);
    }
  }
  *st = 404;
  return g_strdup("{\"error\":\"no such band\"}");
}

/* POST /api/phase (managers): a phased band's combiner, live — the mode and
 * the Fixed weight, for the band's CW and RTTY pipelines; not written to
 * the config (docs/PHASING.md: the panel's Save does that). */
static char *web_phase_post(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  const char *band = g_hash_table_lookup(form, "band");
  const char *mode = g_hash_table_lookup(form, "mode");
  SkimPhaseMode m;
  if (!mode || !skim_phase_mode_parse(mode, &m)) {
    *st = 400;
    return g_strdup("{\"error\":\"mode: mvdr, mrc, fixed, a or b\"}");
  }
  for (guint i = 0; band && i < h->nb; i++) {
    if (strcmp(h->band[i].name, band) != 0)
      continue;
    if (!h->cfg.band[i].phased) {
      *st = 400;
      return g_strdup("{\"error\":\"the band is not phased (ip=ab)\"}");
    }
    const char *g = g_hash_table_lookup(form, "gain_db"), *ph = g_hash_table_lookup(form, "phase_deg");
    const double gain = g ? g_ascii_strtod(g, NULL) : h->cfg.phase_fixed_gain_db;
    const double phase = ph ? g_ascii_strtod(ph, NULL) : h->cfg.phase_fixed_phase_deg;
    for (guint k = 0; k < M_N; k++) {
      if (h->band[i].dec[k].p) { skim_pipeline_phase_set(h->band[i].dec[k].p, m, gain, phase); }
    }
    g_message("phase: %s → %s (from the web page)", band, skim_phase_mode_name(m));
    return g_strdup_printf("{\"ok\":true,\"band\":\"%s\",\"mode\":\"%s\"}", h->band[i].name,
                           skim_phase_mode_name(m));
  }
  *st = 404;
  return g_strdup("{\"error\":\"no such band\"}");
}

static char *web_v4bench(GHashTable *form, guint *st, gpointer user) {
  (void)form; (void)st;
  Hd *h = user;
  const gboolean started = v4_bench_start(h);
  if (started) { g_message("cw-v4: compute check asked for on the web page"); }
  return g_strdup_printf("{\"ok\":true,\"state\":\"running\",\"started\":%s}",
                         started ? "true" : "false");
}

/* Who restarts this process: "systemd" (the user unit), "supervisor"
 * (SKIM_SUPERVISED=1: exit 75 for a restart) or null (nobody). */
static const char *supervisor_json(void) {
  return g_strcmp0(g_getenv("SKIM_SUPERVISED"), "1") == 0 ? "\"supervisor\""
         : skim_hd_service_under_systemd()                ? "\"systemd\""
                                                          : "null";
}

/* [archive] asked for (whether it could be opened or not). */
static gboolean c_archive_wanted(const Hd *h) { return h->cfg.archive_dir != NULL; }

static void status_build(Hd *h, gboolean print) {
  const gint64 now = g_get_monotonic_time();
  const SkimIqSourceStatus *src = &h->src_st;
  const guint64 lost = src->lost_frames;
  guint64 frames0 = h->nb ? h->band[0].frames : 0;
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


  char temp[G_ASCII_DTOSTR_BUF_SIZE] = "null";       /* the CPU temperature now */
  if (!isnan(h->temp_c)) { g_ascii_formatd(temp, sizeof(temp), "%.1f", h->temp_c); }
  char *jstate = skim_json_str(src->state);
  char *jcall = skim_json_str(h->cfg.feed_call ? h->cfg.feed_call : "");
  char *jhost = skim_json_str(src->host);
  /* the decoder that actually runs (deepcw falls back to cw-v3.1 without its
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
  /* [peers]: who is connected, what came in, what the dedup dropped */
  char *jpeers = h->peers ? skim_hd_peers_json(h->peers) : g_strdup("[]");
  const guint64 dups = h->feed ? skim_rbn_feed_dup_dropped(h->feed)
                     : h->rbn  ? skim_rbn_feed_dup_dropped(h->rbn) : 0;
  /* [rbn]: the Aggregator's port — who is on it, what went out, what waits */
  char *jrbn = h->rbn
      ? g_strdup_printf("{\"port\":%d,\"delay_s\":%u,\"clients\":%u,\"lines\":%"
                        G_GUINT64_FORMAT ",\"waiting\":%u}",
                        h->cfg.rbn_port, skim_rbn_feed_delay(h->rbn),
                        skim_rbn_feed_clients(h->rbn), skim_rbn_feed_lines(h->rbn),
                        skim_rbn_feed_waiting(h->rbn))
      : g_strdup("null");
  g_string_append_printf(json,
      "{\"version\":\"%s\",\"started_us\":%" G_GINT64_FORMAT ",\"time\":\"%s\",\"radio\":\"%s\",\"streaming\":%s,\"host\":\"%s\","
      "\"call\":\"%s\",\"locator\":\"%s\",\"clock_ppm\":%.2f,\"rate\":%u,\"lost_pct\":%.3f,\"packets_per_s\":%.0f,\"cpu_pct\":%.1f,"
      "\"cpu_cw_pct\":%.1f,\"cpu_rtty_pct\":%.1f,\"cpu_temp_c\":%s,"
      "\"engine\":\"%s\","
      "\"feed_policy\":{\"min_score\":%.2f,\"min_hearings\":%u,\"settle_s\":%.1f,"
      "\"settle_nodict_s\":%.1f,"
      "\"fresh_s\":%.1f,\"fold_s\":%.1f,\"folded\":%" G_GUINT64_FORMAT "},"
      "\"feed_gate\":%s,\"switches\":%s,\"supervisor\":%s,"
      "\"rbn\":%s,\"peers\":%s,\"external_dups\":%" G_GUINT64_FORMAT ","
      "\"suppress_cw_on_rtty\":%s,\"feed_port\":%d,\"feed_rtty\":%s,\"feed_clients\":%u,\"feed_lines\":%" G_GUINT64_FORMAT ","
      "\"dict_calls\":%u,\"uptime_s\":%" G_GINT64_FORMAT,
      SKIMMER_VERSION, h->started_us, clock, jstate, src->streaming ? "true" : "false", jhost, jcall, h->cfg.locator, src->ppm, h->cfg.rate, lost_pct,
      h->pps, h->cpu_pct, h->cpu_mode_pct[M_CW], h->cpu_mode_pct[M_RTTY], temp, engine, pol_score,
      pol_hear, pol_settle, pol_nd, pol_fresh,
      pol_fold, folded, jgate, h->switches_json, supervisor_json(), jrbn, jpeers, dups,
      h->cfg.suppress_cw_on_rtty ? "true" : "false",
      h->feed ? h->cfg.feed_port : 0, h->cfg.feed_rtty ? "true" : "false", fclients, flines,
      (guint)skim_callsign_dict_size(), up_s);
  if (h->gov) { skim_hd_governor_json(h->gov, json); }   /* "rtty_adaptive" */
  {                                            /* the managed Red Pitaya (rp_manage.h) */
    char *rpj = h->rp ? skim_hd_rp_status_block(h->rp) : NULL;
    g_string_append_printf(json, ",\"redpitaya\":%s", rpj ? rpj : "null");
    g_free(rpj);
  }
  g_string_append(json, ",\"bands\":[");
  g_free(jstate);
  g_free(jcall);
  g_free(jgate);
  g_free(jrbn);
  g_free(jpeers);
  g_free(jhost);

  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    const double lo = (b->centre_hz - h->cfg.band[i].rate / 2.0) / 1000.0;
    const double hi = (b->centre_hz + h->cfg.band[i].rate / 2.0) / 1000.0;
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
     * engine "cw-v3.1" is a band that fell back (the reason: "deepcw") */
    if (b->worker) {
      g_string_append_printf(con, "   %-5s %-9s %6.1f   → decoded by the worker %s, %d spots\n",
                             b->name, h->cfg.band[i].cw && h->cfg.band[i].rtty ? "cw,rtty"
                             : h->cfg.band[i].rtty ? "rtty" : "cw",
                             b->centre_hz / 1000.0, b->worker,
                             g_atomic_int_get(&b->remote_spots[M_CW]) +
                                 g_atomic_int_get(&b->remote_spots[M_RTTY]));
    }
    /* a band a worker decodes: who, which modes, the spots it sent — the
     * page takes the rest from that worker's own status (hub.workers) */
    char *jw = b->worker ? skim_json_str(b->worker) : NULL;
    if (jw) {
      g_string_append_printf(json, "%s{\"worker\":\"%s\",\"worker_cw\":%s,\"worker_rtty\":%s,"
                             "\"remote_spots\":%d,\"remote_spots_rtty\":%d,", i ? "," : "", jw,
                             h->cfg.band[i].cw ? "true" : "false",
                             h->cfg.band[i].rtty ? "true" : "false",
                             g_atomic_int_get(&b->remote_spots[M_CW]),
                             g_atomic_int_get(&b->remote_spots[M_RTTY]));
    }
    g_free(jw);
    g_string_append_printf(json, "%s\"band\":\"%s\",\"centre_hz\":%.0f,\"ksps\":%.1f,"
                           "\"cw\":%s,\"engine_wanted\":\"%s\","
                           "\"rtty_suppressed\":%" G_GUINT64_FORMAT ",\"phase\":",
                           b->worker ? "" : i ? ",{" : "{", jname, b->centre_hz, b->ksps,
                           b->dec[M_CW].p ? "true" : "false",
                           b->dec[M_CW].p ? engine_key(h->cfg.band[i].cw_engine) : "",
                           b->dec[M_CW].p ? skim_pipeline_rtty_suppressed(b->dec[M_CW].p) : 0);
    phase_json(json, b);
    g_string_append_c(json, ',');
    g_free(jname);
    dec_json(json, &b->dec[M_CW], b->dec[M_CW].p ? paused_why(h, i, M_CW) : NULL, now);
    g_string_append(json, ",\"rtty\":");
    if (b->dec[M_RTTY].p) {
      g_string_append_c(json, '{');
      dec_json(json, &b->dec[M_RTTY], paused_why(h, i, M_RTTY), now);
      /* the pairs it copies now (published for the band's CW) */
      g_string_append_printf(json, ",\"pairs\":%u}",
                             b->pairs ? skim_rtty_pairs_count(b->pairs, now) : 0);
    } else {
      g_string_append(json, "null");
    }
    g_string_append_c(json, '}');
  }
  g_string_append(json, "],\"hub\":");
  if (h->hub) {
    char *hj = skim_hd_hub_json(h->hub);
    g_string_append(json, hj);
    g_free(hj);
  } else {
    g_string_append(json, "null");
  }
  g_string_append(json, ",\"wspr\":");
  if (h->wspr) {
    char *wj = skim_hd_wspr_status_json(h->wspr);
    g_string_append(json, wj);
    g_free(wj);
  } else {
    g_string_append(json, "{\"on\":false}");
  }
  g_string_append(json, ",\"ft8\":");
  if (h->ft8) {
    char *fj = skim_hd_ft8_status_json(h->ft8);
    g_string_append(json, fj);
    g_free(fj);
  } else {
    g_string_append(json, "{\"on\":false}");
  }
  g_string_append(json, ",\"js8\":");
  if (h->js8) {
    char *jj = skim_hd_js8_status_json(h->js8);
    g_string_append(json, jj);
    g_free(jj);
  } else {
    g_string_append(json, "{\"on\":false}");
  }
  g_string_append(json, ",\"pskreporter\":");
  if (h->pskr) {
    char *pj = skim_pskr_status_json(h->pskr);
    g_string_append(json, pj);
    g_free(pj);
  } else {
    g_string_append(json, "{\"on\":false}");
  }
  g_string_append_printf(json, ",\"waterfall_viewers\":%u,\"waterfall_listeners\":%u",
                         skim_hd_waterfall_viewers(h->wf), skim_hd_waterfall_listeners(h->wf));
  g_string_append(json, ",\"archive\":");
  if (h->archive) {                            /* [archive]: where, today's file */
    char *adir = NULL, *today = NULL, *aerr = NULL;
    guint64 written = 0;
    guint waiting = 0;
    skim_hd_archive_status(h->archive, &adir, &today, &written, &waiting, &aerr);
    g_string_append(json, "{\"dir\":");
    json_opt_str(json, adir);
    g_string_append(json, ",\"file\":");
    json_opt_str(json, today);
    g_string_append_printf(json, ",\"written\":%" G_GUINT64_FORMAT ",\"waiting\":%u,\"error\":",
                           written, waiting);
    json_opt_str(json, aerr);
    g_string_append_c(json, '}');
    g_free(adir); g_free(today); g_free(aerr);
  } else {
    g_string_append(json, c_archive_wanted(h) ? "{\"error\":\"the directory could not be used — see the log\"}" : "null");
  }
  g_string_append(json, ",\"deepcw\":");
  dcw_json(h, json);
  g_string_append(json, ",\"v4\":");
  v4_json(h, json);
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

static void spot_log_save(Hd *h);

/* Close the minute h->minute: what each band did in it, and the machine. */
static void stats_minute(Hd *h) {
  SkimStatsMinute m;
  memset(&m, 0, sizeof(m));
  guint rtty_spots = 0;                        /* the minute's, every band here */
  guint band_spots[SKIM_HD_MAX_BANDS] = { 0 };  /* … each band's, CW + RTTY      */
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    for (guint k = 0; k < SKIM_STATS_MODES; k++) {
      for (guint j = 0; j < SKIM_SM_N; j++) { m.band[b][k][j] = NAN; }
    }
  }
  for (guint i = 0; i < h->nb && i < SKIM_STATS_BANDS; i++) {
    Band *rb = &h->band[i];
    for (guint k = 0; rb->worker && k < M_N; k++) {   /* decoded by a worker */
      if (k == M_CW ? !h->cfg.band[i].cw : !h->cfg.band[i].rtty) { continue; }
      const gint sp = g_atomic_int_get(&rb->remote_spots[k]);
      const int act = h->hub ? skim_hd_hub_band_active(h->hub, i, k) : -1;
      float *v = m.band[i][k];
      v[SKIM_SM_SPOTS] = (float)(sp - rb->remote_prev[k]);
      rb->remote_prev[k] = sp;
      if (act >= 0) { v[SKIM_SM_ACTIVE] = (float)act; }
    }
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
      if (k == M_RTTY) { rtty_spots += (guint)v[SKIM_SM_SPOTS]; }
      band_spots[i] += (guint)v[SKIM_SM_SPOTS];
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
      /* paused at the minute's end (adaptive RTTY changes only on a
       * minute's edge, the thermal guard any second): not decoded — a gap
       * in the history, not a quiet minute */
      if (g_atomic_int_get(&b->paused)) {
        for (guint j = 0; j < SKIM_SM_N; j++) { v[j] = NAN; }
      }
      /* … and how much of it the thermal guard had the band off (red in the
       * Statistics; NaN while the guard is off) */
      if (h->guard_s) { v[SKIM_SM_THERMAL] = (float)rb->shed_s / (float)h->guard_s; }
    }
    /* FT8 + FT4 ([ft8]): the spots, the senders that appeared, as "active"
     * the senders heard, the median SNR of the decodes; a band the thermal
     * guard had off at the minute's end decodes none — a gap */
    SkimHdFt8Minute fm;
    if (h->ft8 && skim_hd_ft8_minute(h->ft8, i, &fm) && !g_atomic_int_get(&rb->ft8_paused)) {
      float *v = m.band[i][SKIM_STATS_FT8];
      v[SKIM_SM_SPOTS] = (float)fm.spots;
      v[SKIM_SM_STATIONS] = (float)fm.appeared;
      v[SKIM_SM_ACTIVE] = (float)fm.heard;
      v[SKIM_SM_SNR] = fm.snr;
      if (h->guard_s) { v[SKIM_SM_THERMAL] = (float)rb->shed_s / (float)h->guard_s; }
    }
    /* JS8 ([js8], every submode): the same as FT8's — its spots (the
     * page's: confirmed or not), the senders that appeared, the senders
     * heard, the median SNR */
    SkimHdJs8Minute jm;
    if (h->js8 && skim_hd_js8_minute(h->js8, i, &jm) && !g_atomic_int_get(&rb->ft8_paused)) {
      float *v = m.band[i][SKIM_STATS_JS8];
      v[SKIM_SM_SPOTS] = (float)jm.spots;
      v[SKIM_SM_STATIONS] = (float)jm.appeared;
      v[SKIM_SM_ACTIVE] = (float)jm.heard;
      v[SKIM_SM_SNR] = jm.snr;
      if (h->guard_s) { v[SKIM_SM_THERMAL] = (float)rb->shed_s / (float)h->guard_s; }
    }
    rb->shed_s = 0;
  }
  h->guard_s = 0;
  if (h->gov && skim_hd_governor_minute(h->gov, h->minute, rtty_spots, band_spots)) { pause_apply(h); }
  {                                            /* CW v4: the minute's late share */
    SkimCwV4Stats vs;
    skim_decode_cw_v4_stats(&vs, TRUE);
    const guint64 dt = vs.ticks - h->v4_ticks_prev, ds = vs.skipped - h->v4_skipped_prev;
    h->v4_ticks_prev = vs.ticks;
    h->v4_skipped_prev = vs.skipped;
    if (h->gov && skim_hd_governor_load(h->gov, h->minute, dt ? (double)ds / dt : NAN)) {
      pause_apply(h);
    }
  }
  const SkimIqSourceStatus *src = &h->src_st;
  const guint64 frames = h->nb ? h->band[0].frames : 0;
  const guint64 df = frames - h->min_frames_prev, dl = src->lost_frames - h->min_lost_prev;
  /* CPU: the minute's mean — the whole process, and the CW and RTTY
   * pipelines' engine threads in it (the rest is IQ in, feed, web) */
  const gint64 now_us = g_get_monotonic_time();
  const guint64 cpu = cpu_ticks();
  guint64 mode_ns[M_N] = { 0 };
  gboolean has[M_N] = { FALSE };
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) {
      if (!h->band[i].dec[k].p) { continue; }
      has[k] = TRUE;
      mode_ns[k] += skim_pipeline_cpu_ns(h->band[i].dec[k].p);
    }
  }
  const double wall = h->min_cpu_us ? (now_us - h->min_cpu_us) / 1e6 : 0;
  m.glob[SKIM_SG_CPU] = wall > 0 ? (float)(100.0 * (double)(cpu - h->min_cpu_prev) /
                                           (double)sysconf(_SC_CLK_TCK) / wall)
                                 : (float)h->cpu_pct;
  m.glob[SKIM_SG_CPU_CW] = m.glob[SKIM_SG_CPU_RTTY] = NAN;
  for (guint k = 0; k < M_N && wall > 0; k++) {
    if (has[k] && mode_ns[k] >= h->min_mode_prev[k]) {
      m.glob[k == M_CW ? SKIM_SG_CPU_CW : SKIM_SG_CPU_RTTY] =
          (float)(100.0 * (double)(mode_ns[k] - h->min_mode_prev[k]) / 1e9 / wall);
    }
  }
  /* the helper programs (skimmer-ft8d, skimmer-js8d, skimmer-wsprd): their
   * CPU is not in this process's; NaN until one ran. A kept-running
   * skimmer-js8d reaches the children's times only once it ends: its CPU
   * so far is read from /proc */
  const guint64 child = cpu_ticks_of(TRUE) + (h->js8 ? skim_hd_js8_helper_ticks(h->js8) : 0);
  m.glob[SKIM_SG_CPU_HELPERS] = wall > 0 && h->min_cpu_us && (h->ft8 || h->js8 || h->wspr) &&
                                        child >= h->min_child_prev
                                    ? (float)(100.0 * (double)(child - h->min_child_prev) /
                                              (double)sysconf(_SC_CLK_TCK) / wall)
                                    : NAN;
  h->min_child_prev = child;
  h->min_cpu_prev = cpu;
  memcpy(h->min_mode_prev, mode_ns, sizeof(mode_ns));
  h->min_cpu_us = now_us;
  m.glob[SKIM_SG_LOST] = df + dl && src->lost_frames >= h->min_lost_prev
                             ? (float)(100.0 * (double)dl / (double)(df + dl)) : 0;
  m.glob[SKIM_SG_CLIENTS] = h->feed ? (float)skim_rbn_feed_clients(h->feed) : 0;
  m.glob[SKIM_SG_STREAMING] = src->streaming ? 1 : 0;
  m.glob[SKIM_SG_CPU_TEMP] = h->temp_peak;            /* NAN: no sensor        */
  {                                            /* the Red Pitaya's (NaN: no agent) */
    SkimHdRpReadings rd = { NAN, NAN, { NAN, NAN } };
    if (h->rp) { skim_hd_rp_readings(h->rp, &rd); }
    m.glob[SKIM_SG_RP_TEMP] = (float)rd.fpga_c;
    m.glob[SKIM_SG_RP_LOAD] = (float)rd.load1;
    m.glob[SKIM_SG_RP_MBPS_A] = (float)rd.mbps[0];
    m.glob[SKIM_SG_RP_MBPS_B] = (float)rd.mbps[1];
  }
  h->temp_peak = NAN;
  m.glob[SKIM_SG_RADIO] = 0;
  for (guint i = 0; src->streaming && h->cfg.hosts[i]; i++) {
    if (strcmp(h->cfg.hosts[i], src->host) == 0) { m.glob[SKIM_SG_RADIO] = (float)(i + 1); }
  }
  h->min_frames_prev = frames;
  h->min_lost_prev = src->lost_frames;
  /* the first minute after a start is partial and its deltas start from
   * zero — not a record */
  if (h->minutes++) { skim_stats_put(h->stats, h->minute, &m); }
  spot_log_save(h);                            /* a crash loses a minute at most */
  if (h->minutes % 5 == 0) {
    GError *err = NULL;
    if (!skim_stats_save(h->stats, &err)) {
      g_warning("stats: %s", err->message);
      g_clear_error(&err);
    }
  }
}

static void worker_report(Hd *h);
static void restart_soon(Hd *h, const char *why);

/* 1 Hz: link supervision, rates, status. */
static void rp_tick(Hd *h);

/* Once a second: each phased band's snapshot into its history (opened at
 * the first snapshot, when the channels are known). */
static void phase_hist_tick(Hd *h) {
  static gint8 rows[2 * 16384];
  for (guint i = 0; i < h->nb; i++) {
    if (!h->cfg.band[i].phased)
      continue;
    Band *b = &h->band[i];
    SkimPipeline *p = b->dec[M_CW].p ? b->dec[M_CW].p : b->dec[M_RTTY].p;
    SkimPipelinePhase ph;
    if (!p || !skim_pipeline_phase_snapshot(p, &ph, rows, rows + 16384, NULL, 16384) || !ph.updates)
      continue;
    if (!b->phist) {
      char *dir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
      b->phist = skim_phase_hist_open(dir, b->name, ph.nchan, ph.first_hz, ph.spacing_hz);
      g_free(dir);
      if (!b->phist)
        continue;
    }
    skim_phase_hist_add(b->phist, g_get_real_time() / G_USEC_PER_SEC, &ph, rows, rows + 16384);
  }
}

static gboolean tick(gpointer data) {
  Hd *h = data;
  skim_iq_source_tick(h->src);
  skim_iq_source_status(h->src, &h->src_st);
  rp_tick(h);
  phase_hist_tick(h);
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
    const guint64 fr = h->band[i].frames;
    h->band[i].ksps = h->band[i].frames_prev ? (fr - h->band[i].frames_prev) / 1000.0 : 0;
    h->band[i].frames_prev = fr;
  }
  const gint64 now = g_get_monotonic_time();
  const guint64 cpu = cpu_ticks();
  guint64 mode_ns[M_N] = { 0 };               /* the CW / RTTY pipelines' share */
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) {
      if (h->band[i].dec[k].p) { mode_ns[k] += skim_pipeline_cpu_ns(h->band[i].dec[k].p); }
    }
  }
  if (h->cpu_prev_us) {
    const double wall = (now - h->cpu_prev_us) / 1e6;
    h->cpu_pct = wall > 0 ? 100.0 * (double)(cpu - h->cpu_prev) /
                                (double)sysconf(_SC_CLK_TCK) / wall : 0;
    for (guint k = 0; k < M_N; k++) {
      h->cpu_mode_pct[k] = wall > 0 && mode_ns[k] >= h->cpu_mode_prev[k]
          ? 100.0 * (double)(mode_ns[k] - h->cpu_mode_prev[k]) / 1e9 / wall : 0;
    }
  }
  memcpy(h->cpu_mode_prev, mode_ns, sizeof(mode_ns));
  h->cpu_prev = cpu;
  h->cpu_prev_us = now;
  h->temp_c = cpu_temp_c(h->temp_paths);              /* and the minute's peak */
  if (!isnan(h->temp_c) && !(h->temp_c <= h->temp_peak)) { h->temp_peak = (float)h->temp_c; }
  if (h->gov && skim_hd_governor_temp(h->gov, g_get_real_time() / G_USEC_PER_SEC, h->temp_c)) {
    pause_apply(h);                                    /* the thermal guard     */
  }
  if (h->gov && h->cfg.thermal) {                      /* its share, a minute   */
    h->guard_s++;
    for (guint i = 0; i < h->nb; i++) { h->band[i].shed_s += skim_hd_governor_band_shed(h->gov, i); }
  }

  const gint64 minute = g_get_real_time() / (60 * G_USEC_PER_SEC);
  if (minute != h->minute) {
    if (h->minute) { stats_minute(h); }
    h->minute = minute;
  }

  h->ticks++;
  if (h->vis && h->ticks % 10 == 0) { skim_hd_visitors_tick(h->vis, g_get_real_time() / G_USEC_PER_SEC); }
  if (h->archive && h->ticks % 60 == 0) {      /* [archive]: a gzip member a minute */
    GError *err = NULL;
    if (!skim_hd_archive_flush(h->archive, &err)) {
      if (g_strcmp0(err->message, h->archive_err) != 0) {
        g_warning("archive: %s — the spots wait and are tried again each minute", err->message);
      }
      g_free(h->archive_err);
      h->archive_err = g_strdup(err->message);
      g_clear_error(&err);
    } else if (h->archive_err) {
      g_message("archive: writing again");
      g_clear_pointer(&h->archive_err, g_free);
    }
  }
  const gboolean print = h->console_s > 0 && h->ticks % (guint)h->console_s == 0;
  status_build(h, print);
  if (h->cfg.source == SKIM_HD_SOURCE_HUB) {
    if (h->nb && h->ticks % 10 == 0) { worker_report(h); }
    if (h->src_st.restart && !h->exit_code) { restart_soon(h, "the hub assigns other bands"); }
  }
  return G_SOURCE_CONTINUE;
}

/* ---- web page (headless/web.h) --------------------------------------------------- */

/* The status for everyone else (the page published on a public address):
 * the same, less what tells about the machine and the LAN — addresses
 * (the radio's, the peers', the workers'), file paths (with the user's
 * name), the SKIM_* switches' values. */
static char *status_public(const char *full, gboolean outside) {
  static const struct { const char *re, *to; } RULES[] = {
    /* from outside the LAN, no machine readings either: the CPU load and
     * IQ lost — here and in the workers' status the hub passes on. The
     * temperature (the thermal guard's too) goes to everyone: Dan,
     * 2026-11-21 — the visitors see why bands go OFF */
    { "\"(cpu_pct|cpu_cw_pct|cpu_rtty_pct|lost_pct|packets_per_s|rp_load|rp_mem_mb)\":-?[0-9.eE+-]+",
      "\"\\1\":null" },
    /* CW v4's load and the compute check: machine readings too */
    { "\"v4\":\\{[^{}]*\\}", "\"v4\":null" },
    { "\"(host|addr|model|dir)\":\"(?:[^\"\\\\]|\\\\.)*\"", "\"\\1\":\"\"" },
    { "\"switches\":\\{[^{}]*\\}", "\"switches\":{}" },
    /* the locator as its square only: six characters are nearly an address */
    { "\"locator\":\"([A-Z]{2}[0-9]{2})[A-Z]*\"", "\"locator\":\"\\1\"" },
    { "\\b(?:\\d{1,3}\\.){3}\\d{1,3}(?::\\d+)?\\b", "(address)" },
    { "(?<![\\w.])/(?:home|root|var|tmp|etc|usr|srv|mnt|opt|run|media)/[^\"\\s,;)]*", "(path)" },
  };
  char *out = g_strdup(full);
  for (guint i = outside ? 0 : 2; i < G_N_ELEMENTS(RULES); i++) {
    GRegex *re = g_regex_new(RULES[i].re, G_REGEX_OPTIMIZE, 0, NULL);
    char *next = re ? g_regex_replace(re, out, -1, 0, RULES[i].to, 0, NULL) : NULL;
    if (re) { g_regex_unref(re); }
    if (next) { g_free(out); out = next; }
  }
  return out;
}

static char *web_status(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                        gpointer user) {
  (void)q; (void)st; (void)type;
  Hd *h = user;
  g_mutex_lock(&h->snap_lock);
  char *body = g_strdup(h->snap_json ? h->snap_json : "{}");
  g_mutex_unlock(&h->snap_lock);
  if (who != SKIM_HD_WEB_MANAGER) {
    char *pub = status_public(body, who == SKIM_HD_WEB_PUBLIC);
    g_free(body);
    body = pub;
  }
  /* [feed] address: published on purpose — after the address rule */
  if (h->cfg.feed_address && body[0] == '{') {
    char *ja = skim_json_str(h->cfg.feed_address);
    char *with = g_strdup_printf("{\"feed_address\":\"%s\",%s", ja, body + 1);
    g_free(ja);
    g_free(body);
    body = with;
  }
  /* [rtty_palette]: the page's palette settings, for everyone who may see
   * the waterfall (nothing in them names the machine) */
  if (body[0] == '{') {
    char *rp = skim_hd_config_rtty_palette_json(&h->cfg);
    char *with = g_strdup_printf("{\"rtty_palette\":%s%s%s", rp, body[1] == '}' ? "" : ",", body + 1);
    g_free(rp);
    g_free(body);
    body = with;
  }
  return body;
}

/* The history without the machine readings (the CPU load, IQ lost) for a
 * client from outside: those series as null. The CPU temperature stays. */
static char *stats_public(const char *full) {
  static GRegex *re;
  if (!re) {
    re = g_regex_new("\"(cpu|cpu_cw|cpu_rtty|lost|rp_load)\":\\[[^\\]]*\\]", G_REGEX_OPTIMIZE,
                     0, NULL);
  }
  char *out = re ? g_regex_replace(re, full, -1, 0, "\"\\1\":null", 0, NULL) : NULL;
  return out ? out : g_strdup(full);
}

static char *web_stats(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                       gpointer user) {
  (void)st; (void)type;
  Hd *h = user;
  const char *hv = q ? g_hash_table_lookup(q, "hours") : NULL;
  const char *sv = q ? g_hash_table_lookup(q, "step") : NULL;
  const char *ev = q ? g_hash_table_lookup(q, "end") : NULL;    /* unix s: a past window */
  gint64 end = g_get_real_time() / (60 * G_USEC_PER_SEC);
  if (ev) { end = MIN(end, (gint64)g_ascii_strtoll(ev, NULL, 10) / 60); }
  const guint hours = hv ? (guint)g_ascii_strtoull(hv, NULL, 10) : 24;
  const guint step = sv ? (guint)g_ascii_strtoull(sv, NULL, 10) : 0;
  /* the same question within 20 s gets the same answer: many viewers (or
   * someone hammering a public page) cost one computation */
  static char *c_body;
  static gint64 c_end, c_at;
  static guint c_hours, c_step;
  const gint64 now = g_get_monotonic_time();
  if (!(c_body && c_end == end && c_hours == hours && c_step == step &&
        now - c_at < 20 * G_USEC_PER_SEC)) {
    g_free(c_body);
    c_body = skim_stats_json(h->stats, end, hours, step);
    c_end = end; c_hours = hours; c_step = step; c_at = now;
  }
  return who == SKIM_HD_WEB_PUBLIC ? stats_public(c_body) : g_strdup(c_body);
}

/* ---- settings (POST from settings_from only — web.c checks) ---------------------- */

static gboolean quit_soon(gpointer user) {
  g_main_loop_quit(((Hd *)user)->loop);
  return G_SOURCE_REMOVE;
}

/* Restart once the answer is out: under a supervisor that asks for it
 * (SKIM_SUPERVISED=1) exit EXIT_RESTART and let it start us again;
 * otherwise — alone, or as the systemd unit, whose RestartSec would keep
 * the page waiting a minute — main execs itself after the teardown. */
static void restart_soon(Hd *h, const char *why) {
  g_message("restart: %s", why);
  h->exit_code = EXIT_RESTART;
  g_timeout_add(400, quit_soon, h);
}

/* The updater swapped the AppImage's file: the restart runs the file, not
 * this process's image (which stays the old one inside its mount). */
static void upd_restart(const char *why, gpointer user) {
  Hd *h = user;
  g_free(h->exec_path);
  h->exec_path = g_strdup(h->upd_env.appimage);
  restart_soon(h, why);
}

static char *json_error(guint *st, guint code, const char *msg) {
  *st = code;
  char *e = skim_json_str(msg);
  char *body = g_strdup_printf("{\"error\":\"%s\"}", e);
  g_free(e);
  return body;
}

static char *web_update(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                        gpointer user) {
  (void)q;
  (void)who;
  (void)st;
  (void)type;
  return skim_hd_updater_json(((Hd *)user)->upd);
}

static char *web_update_post(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  char *why = skim_hd_updater_action(h->upd, g_hash_table_lookup(form, "action"));
  if (why) {
    char *body = json_error(st, SOUP_STATUS_CONFLICT, why);
    g_free(why);
    return body;
  }
  return g_strdup("{\"ok\":true}");
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
static char *web_config(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                        gpointer user) {
  (void)q; (void)type;
  Hd *h = user;
  const gboolean may = who == SKIM_HD_WEB_MANAGER;
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
    for (guint i = 0; i < c.nlisted; i++) {           /* the ones off too */
      const SkimHdBand *b = &c.listed[i];
      g_string_append_printf(o, "%s{", i ? "," : "");
      json_kv(o, "name", b->name, FALSE);
      g_string_append_printf(o, ",\"centre_hz\":%.0f,\"cw\":%s,\"engine\":\"%s\",\"rtty\":%s,"
                             "\"on\":%s,",
                             b->centre_hz, b->cw ? "true" : "false", engine_key(b->cw_engine),
                             b->rtty ? "true" : "false", b->off ? "false" : "true");
      json_kv(o, "worker", b->worker, FALSE);
      g_string_append_printf(o, ",\"ip\":\"%s\",\"input\":%u,\"phase\":\"%s\"}",
                             b->phased ? "ab" : b->ip ? "b" : "a", b->input + 1u,
                             b->phase_mode >= 0 ? skim_phase_mode_name((SkimPhaseMode)b->phase_mode) : "");
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

/* A new config text: checked by the start's own parser, saved (the old
 * file backed up) and a restart — or the answer why not. text: the file
 * now; what: for the log ("from the web page", "imported …"). */
static char *config_replace(Hd *h, const char *text, const char *out, const char *what,
                            guint *st) {
  GError *err = NULL;
  char *bak = NULL, *body;
  SkimHdConfig c;
  if (!skim_hd_config_parse(&c, out, strlen(out), h->cfg_path, &err)) {
    body = json_error(st, 400, err->message);         /* nothing written     */
  } else {
    skim_hd_config_clear(&c);
    if (strcmp(out, text) == 0) {
      body = g_strdup("{\"ok\":true,\"changed\":false}");
    } else if (!skim_hd_config_save(h->cfg_path, out, &bak, &err)) {
      body = json_error(st, 500, err->message);
    } else {
      g_message("config: %s saved %s (previous: %s)", h->cfg_path, what, bak);
      GString *o = g_string_new("{\"ok\":true,\"changed\":true");
      json_kv(o, "backup", bak, TRUE);
      g_string_append(o, ",\"restart\":true}");
      body = g_string_free(o, FALSE);
      restart_soon(h, "the config changed");
    }
  }
  g_clear_error(&err);
  g_free(bak);
  return body;
}

/* POST /api/config: the form applied to the file, then config_replace. */
static char *web_config_post(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  char *text = NULL, *out = NULL, *body;
  gsize len = 0;
  GError *err = NULL;
  if (!g_file_get_contents(h->cfg_path, &text, &len, &err) ||
      !(out = skim_hd_config_apply_form(text, len, form, &err))) {
    body = json_error(st, 400, err->message);
  } else {
    body = config_replace(h, text, out, "from the web page", st);
  }
  g_clear_error(&err);
  g_free(out);
  g_free(text);
  return body;
}

/* POST /api/config/import ini=<an exported file>: the whole file as the
 * config, this machine's [web] kept (skim_hd_config_import). */
static char *web_config_import(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  const char *ini = g_hash_table_lookup(form, "ini");
  char *text = NULL, *out = NULL, *body;
  gsize len = 0;
  GError *err = NULL;
  if (!ini || !ini[0]) {
    body = json_error(st, 400, "no file (ini=)");
  } else if (!g_file_get_contents(h->cfg_path, &text, &len, &err) ||
             !(out = skim_hd_config_import(text, len, ini, strlen(ini), &err))) {
    body = json_error(st, 400, err->message);
  } else {
    body = config_replace(h, text, out, "imported on the web page", st);
  }
  g_clear_error(&err);
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

/* ---- the radios on the LAN ------------------------------------------------------ */

/* One line per radio a broadcast finds, each starting with prefix; "" when
 * none answered. */
static char *radios_lines(const char *prefix) {
  GArray *found = skim_iq_source_hpsdr_discover();
  GString *o = g_string_new(NULL);
  for (guint i = 0; i < found->len; i++) {
    const SkimHpsdrFound *f = &g_array_index(found, SkimHpsdrFound, i);
    const guint8 *m = f->info.mac;
    g_string_append_printf(o, "%s%-15s  %s, %u RX, %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                           prefix, f->host, f->info.name[0] ? f->info.name : "HPSDR",
                           f->info.nrx, f->info.busy ? "busy (another client streams from it)" : "idle",
                           m[0], m[1], m[2], m[3], m[4], m[5]);
  }
  g_array_unref(found);
  return g_string_free(o, FALSE);
}

/* --find-radios */
/* --- the Red Pitaya over SSH (rp_manage.h) ------------------------------------- */

/* The board rp_manage talks to: the first [radio] host that is an address
 * (not auto) — eth0's IP of the managed board. */
static const char *rp_default_host(const SkimHdConfig *c) {
  for (char **p = c->hosts; p && *p; p++) {
    if (strcmp(*p, SKIM_HD_HOST_AUTO) != 0) { return *p; }
  }
  return NULL;
}

/* The board's state once at start (key only — never a prompt), then the
 * agent's status every 10 s while it runs the board. */
static void rp_tick(Hd *h) {
  if (!h->rp) { return; }
  if (!h->rp_probed) {
    h->rp_probed = TRUE;
    char *key = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "rp", "id_ed25519", NULL);
    if (g_file_test(key, G_FILE_TEST_EXISTS)) {
      skim_hd_rp_start(h->rp, SKIM_HD_RP_PROBE, NULL, NULL, NULL, NULL);
    }
    g_free(key);
    return;
  }
  ++h->rp_tick;
  if (h->rp_tick % 10 == 0 && skim_hd_rp_managed(h->rp)) {
    skim_hd_rp_start(h->rp, SKIM_HD_RP_STATUS, NULL, NULL, NULL, NULL);   /* busy: next time */
  } else if (h->rp_tick % 60 == 0 && !skim_hd_rp_managed(h->rp)) {
    /* not (yet) the agent's: a probe a minute while the key is there (an
     * install from elsewhere, a board that was rebooting) */
    char *key = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "rp", "id_ed25519", NULL);
    if (g_file_test(key, G_FILE_TEST_EXISTS)) {
      skim_hd_rp_start(h->rp, SKIM_HD_RP_PROBE, NULL, NULL, NULL, NULL);
    }
    g_free(key);
  }
}

/* GET /api/redpitaya (managers): rp_manage's state. */
static char *web_redpitaya(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                           gpointer user) {
  (void)q; (void)who; (void)st; (void)type;
  Hd *h = user;
  return h->rp ? skim_hd_rp_json(h->rp) : g_strdup("{\"ssh\":false}");
}

static gboolean form_true(GHashTable *form, const char *k) {
  const char *v = g_hash_table_lookup(form, k);
  return v && (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0);
}

static guint8 form_mask(GHashTable *form, const char *k) {
  const char *v = g_hash_table_lookup(form, k);
  return v ? (guint8)g_ascii_strtoull(v, NULL, 0) : 0;
}

/* POST /api/redpitaya (managers): action=connect (host, password — only
 * the first time) | probe | status | install (take_over, sel_a, sel_b) |
 * uninstall (take_over) | sel (sel_a, sel_b, persist). The job runs on;
 * GET /api/redpitaya follows it. */
static char *web_redpitaya_post(GHashTable *form, guint *st, gpointer user) {
  Hd *h = user;
  if (!h->rp) { return json_error(st, SOUP_STATUS_NOT_FOUND, "no Red Pitaya management here"); }
  const char *action = g_hash_table_lookup(form, "action");
  static const struct { const char *name; SkimHdRpJob job; } acts[] = {
    { "connect", SKIM_HD_RP_CONNECT }, { "probe", SKIM_HD_RP_PROBE },
    { "status", SKIM_HD_RP_STATUS },   { "install", SKIM_HD_RP_INSTALL },
    { "uninstall", SKIM_HD_RP_UNINSTALL }, { "sel", SKIM_HD_RP_SEL },
  };
  int job = -1;
  for (guint i = 0; action && i < G_N_ELEMENTS(acts); i++) {
    if (strcmp(action, acts[i].name) == 0) { job = (int)acts[i].job; }
  }
  if (job < 0) { return json_error(st, SOUP_STATUS_BAD_REQUEST, "unknown action"); }
  /* this skimmer's own stream is no reason to refuse an install */
  const char *ours[2] = { h->src_st.streaming && h->src_st.host[0] ? h->src_st.host : NULL, NULL };
  char *pw = g_hash_table_lookup(form, "password");
  SkimHdRpArgs a = {
    .host = g_hash_table_lookup(form, "host"),
    .password = pw && *pw ? pw : NULL,
    .take_over = form_true(form, "take_over"),
    .persist = form_true(form, "persist"),
    .sel_a = form_mask(form, "sel_a"),
    .sel_b = form_mask(form, "sel_b"),
    .sel_given = g_hash_table_contains(form, "sel_a") || g_hash_table_contains(form, "sel_b"),
    .ours = ours,
  };
  GError *e = NULL;
  const gboolean ok = skim_hd_rp_start(h->rp, (SkimHdRpJob)job, &a, NULL, NULL, &e);
  if (pw) { memset(pw, 0, strlen(pw)); }      /* the job has its own copy */
  if (!ok) {
    char *body = json_error(st, SOUP_STATUS_CONFLICT, e->message);
    g_error_free(e);
    return body;
  }
  g_message("redpitaya: %s started from the web page", action);
  return g_strdup("{\"ok\":true}");
}

static gboolean cli_job_over, cli_job_ok;

static void cli_job_done(SkimHdRp *rp, SkimHdRpJob job, gboolean ok, gpointer loop) {
  (void)rp; (void)job;
  cli_job_over = TRUE;
  cli_job_ok = ok;
  g_main_loop_quit(loop);
}

static gboolean cli_job(SkimHdRp *rp, SkimHdRpJob job, const SkimHdRpArgs *a) {
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  GError *e = NULL;
  cli_job_over = FALSE;
  if (!skim_hd_rp_start(rp, job, a, cli_job_done, loop, &e)) {
    g_printerr("redpitaya: %s\n", e->message);
    g_error_free(e);
    g_main_loop_unref(loop);
    return FALSE;
  }
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return cli_job_ok;
}

/* A password from the terminal, not echoed (NULL: no terminal). */
static char *read_password(const char *prompt) {
  FILE *tty = fopen("/dev/tty", "r+");
  if (!tty) { return NULL; }
  struct termios old, quiet;
  const int fd = fileno(tty);
  const gboolean term = tcgetattr(fd, &old) == 0;
  if (term) {
    quiet = old;
    quiet.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(fd, TCSAFLUSH, &quiet);
  }
  fputs(prompt, tty);
  fflush(tty);
  char buf[256];
  char *got = fgets(buf, sizeof(buf), tty) ? g_strdup(g_strchomp(buf)) : NULL;
  memset(buf, 0, sizeof(buf));
  if (term) { tcsetattr(fd, TCSAFLUSH, &old); }
  fputs("\n", tty);
  fclose(tty);
  return got;
}

/* --redpitaya CMD [HOST]: the same jobs as the web page, in the foreground. */
static int redpitaya_cli(const char *cmd, const char *host, const SkimHdConfig *c,
                         gboolean take_over) {
  static const struct { const char *name; SkimHdRpJob job; } acts[] = {
    { "connect", SKIM_HD_RP_CONNECT }, { "probe", SKIM_HD_RP_PROBE },
    { "status", SKIM_HD_RP_STATUS },   { "install", SKIM_HD_RP_INSTALL },
    { "uninstall", SKIM_HD_RP_UNINSTALL },
  };
  int job = -1;
  for (guint i = 0; i < G_N_ELEMENTS(acts); i++) {
    if (strcmp(cmd, acts[i].name) == 0) { job = (int)acts[i].job; }
  }
  if (job < 0) {
    g_printerr("--redpitaya: connect, probe, status, install or uninstall\n");
    return EXIT_CONFIG;
  }
  if (!host) { host = rp_default_host(c); }
  if (!host) {
    g_printerr("--redpitaya: which board? (HOST after the options, or [radio] host)\n");
    return EXIT_CONFIG;
  }
  char *state = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
  SkimHdRp *rp = skim_hd_rp_new(state, NULL, host);
  g_free(state);
  SkimHdRpArgs a = { .host = host, .take_over = take_over };
  /* every job needs the key working: connect (with its probe) first */
  gboolean ok = cli_job(rp, SKIM_HD_RP_CONNECT, &a);
  char *st = skim_hd_rp_json(rp);
  const gboolean need = !ok && strstr(st, "\"need_password\":true") != NULL;
  g_free(st);
  if (need) {
    char *prompt = g_strdup_printf("root password of %s (typed once, never stored): ", host);
    char *pw = read_password(prompt);
    g_free(prompt);
    if (pw) {
      a.password = pw;
      ok = cli_job(rp, SKIM_HD_RP_CONNECT, &a);
      memset(pw, 0, strlen(pw));
      g_free(pw);
      a.password = NULL;
    }
  }
  if (ok && job != SKIM_HD_RP_CONNECT && job != SKIM_HD_RP_PROBE) {
    const SkimHdRpArgs b = { .take_over = take_over };   /* install: the card's inputs kept */
    ok = cli_job(rp, (SkimHdRpJob)job, &b);
  }
  char *j = skim_hd_rp_json(rp);
  printf("%s\n", j);
  g_free(j);
  skim_hd_rp_free(rp);
  return ok ? 0 : 1;
}

static int find_radios_cli(void) {
  char *l = radios_lines("  ");
  if (l[0]) {
    printf("Red Pitayas (HPSDR P1) on this LAN:\n%s"
           "[radio] host=auto takes the first idle one; to pin one, put its address\n"
           "there (host=ADDRESS, or ADDRESS@ppm with its clock correction).\n", l);
  } else {
    printf("No Red Pitaya answered a discovery broadcast (UDP port %u) within 1 s.\n"
           "Is it on, on this LAN, and running sdr_receiver_hpsdr? A radio on another\n"
           "subnet is not reached by a broadcast: name it in [radio] host=ADDRESS.\n",
           SKIM_HPSDR_DEFAULT_PORT);
  }
  g_free(l);
  return 0;
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

/* ---- the instances on the LAN (headless/discovery.h) ---------------------------- */

/* A hub in the full sense: it passes a band on, or a worker is connected. */
static gboolean disco_has_workers(Hd *h) {
  if (!h->hub) { return FALSE; }
  for (guint i = 0; i < h->nb; i++) {
    if (h->band[i].worker) { return TRUE; }
  }
  char **idle = skim_hd_hub_idle_workers(h->hub);
  const gboolean any = idle[0] != NULL;
  g_strfreev(idle);
  return any;
}

/* What this instance tells a discovery request: who, which role, where its
 * feed and page are, its bands. */
static char *disco_info(gpointer user) {
  Hd *h = user;
  const SkimHdConfig *c = &h->cfg;
  GString *o = g_string_new("{\"name\":\"");
  skim_json_append(o, c->hub_name);
  g_string_append_printf(o, "\",\"version\":\"%s\",\"call\":\"", SKIMMER_VERSION);
  skim_json_append(o, c->feed_call ? c->feed_call : "");
  g_string_append_printf(o, "\",\"role\":\"%s\",\"http_port\":%d,\"feed_port\":%d,"
                         "\"hub_port\":%u,\"radio\":\"",
                         c->source == SKIM_HD_SOURCE_HUB ? "worker" : disco_has_workers(h) ? "hub" : "solo",
                         h->http_port, h->feed ? c->feed_port : 0,
                         h->hub ? skim_hd_hub_port(h->hub) : 0);
  skim_json_append(o, h->src_st.host);
  g_string_append(o, "\",\"bands\":[");
  for (guint i = 0; i < c->nb; i++) {
    char *m = skim_hd_band_modes(&c->band[i], c->engine);
    g_string_append_printf(o, "%s{\"name\":\"", i ? "," : "");
    skim_json_append(o, c->band[i].name);
    g_string_append_printf(o, "\",\"centre_hz\":%.0f,\"modes\":\"%s\"}", c->band[i].centre_hz,
                           m);
    g_free(m);
  }
  g_string_append(o, "]}");
  return g_string_free(o, FALSE);
}

/* GET /api/skimsrv: CW Skimmer Server's bands from its SkimSrv.ini on this PC
 * (wine), the band keys only — for Settings → Bands, "from CW Skimmer" */
static char *web_skimsrv(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                         gpointer user) {
  (void)q; (void)who; (void)st; (void)type; (void)user;
  return skim_hd_skimsrv_json(g_get_home_dir(), g_getenv("WINEPREFIX"));
}

/* GET /api/skimmers: a search (0.8 s — the main loop waits it out) */
static char *web_skimmers(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                          gpointer user) {
  (void)q; (void)who; (void)st; (void)type;
  Hd *h = user;
  char *found = skim_hd_discovery_find(800);
  GString *o = g_string_new("{\"self\":");
  char *me = disco_info(h);
  g_string_append(o, me);
  g_free(me);
  g_string_append_printf(o, ",\"found\":%s}", found);
  g_free(found);
  return g_string_free(o, FALSE);
}

/* A string or number field of one flat discovery answer ("" when absent). */
static char *disco_field(const char *obj, const char *key) {
  char *pat = g_strdup_printf("\"%s\":(\"((?:[^\"\\\\]|\\\\.)*)\"|(-?[0-9.]+))", key);
  GRegex *re = g_regex_new(pat, 0, 0, NULL);
  GMatchInfo *m = NULL;
  char *v = NULL;
  if (g_regex_match(re, obj, 0, &m)) {
    v = g_match_info_fetch(m, 2);
    if (!v || !v[0]) { g_free(v); v = g_match_info_fetch(m, 3); }
  }
  g_match_info_free(m);
  g_regex_unref(re);
  g_free(pat);
  return v ? v : g_strdup("");
}

/* --find-skimmers */
static int find_skimmers_cli(void) {
  char *found = skim_hd_discovery_find(1000);
  /* the answers are flat objects but for "bands": split at each "addr" */
  char **objs = g_strsplit(found, "{\"addr\":", -1);
  guint n = 0;
  for (char **o = objs + 1; *o; o++) {
    char *obj = g_strconcat("{\"addr\":", *o, NULL);
    char *addr = disco_field(obj, "addr"), *name = disco_field(obj, "name");
    char *role = disco_field(obj, "role"), *call = disco_field(obj, "call");
    char *ver = disco_field(obj, "version"), *http = disco_field(obj, "http_port");
    char *feed = disco_field(obj, "feed_port");
    printf("  %-15s %-14s %-7s %-10s v%-7s web :%s  feed %s\n", addr, name, role,
           call[0] ? call : "(no call)", ver, http, strcmp(feed, "0") ? feed : "off");
    n++;
    g_free(addr); g_free(name); g_free(role); g_free(call); g_free(ver); g_free(http);
    g_free(feed); g_free(obj);
  }
  if (!n) {
    printf("No skimmer answered a discovery broadcast (UDP port %u) within 1 s.\n",
           skim_hd_discovery_port());
  }
  g_strfreev(objs);
  g_free(found);
  return 0;
}

static void web_waterfall(SoupWebsocketConnection *conn, const char *peer, const char *visitor,
                          gpointer user) {
  skim_hd_waterfall_add(((Hd *)user)->wf, conn, peer, visitor);
}

/* What a signed-in visitor watches on the waterfall: to its visit. */
static void wf_view(const char *visitor, gconstpointer conn, const char *band, double lo,
                    double hi, gboolean audio, double af, gpointer user) {
  Hd *h = user;
  if (h->vis) {
    skim_hd_visitors_view(h->vis, visitor, conn, band, lo, hi, audio, af,
                          g_get_real_time() / G_USEC_PER_SEC);
  }
}

static char *web_log(GHashTable *q, SkimHdWebWho who, guint *st, const char **type,
                     gpointer user) {
  (void)q; (void)who; (void)st; (void)type; (void)user;
  return skim_log_ring_json();
}

/* ---- main ------------------------------------------------------------------------ */

static void on_scp(const SkimScpOutcome *out, gpointer user) {
  (void)user;
  g_message("MASTER.SCP: %s (%u calls loaded)", out->detail,
            (guint)skim_callsign_dict_size());
}

/* Who made the spot the feed is sending on this thread now: hub_spot and
 * peer_spot set it around their call, the tap reads it. */
static _Thread_local const char *spot_origin;

/* ---- the hub (headless/hub.h) --------------------------------------------------- */

/* What the worker `worker` is to decode: cut from this start's config. */
static char *hub_assign(const char *worker, gpointer user) {
  Hd *h = user;
  return skim_hd_config_worker_fragment(h->cfg_text, strlen(h->cfg_text), worker);
}

/* A worker's spot (its link's thread): counted on its band, out on the feed. */
static void hub_spot(const char *worker, const char *call, const char *mode, double freq_hz,
                     double snr_db, double speed, gpointer user) {
  Hd *h = user;
  if (!skim_hd_config_call_pass(&h->cfg, call)) { return; }   /* [filter] */
  const guint k = g_strcmp0(mode, "CW") == 0 ? M_CW : M_RTTY;
  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    if (b->worker && fabs(freq_hz - b->centre_hz) <= h->cfg.band[i].rate / 2.0) {
      g_atomic_int_inc(&b->remote_spots[k]);
      break;
    }
  }
  if (k == M_RTTY && !h->cfg.feed_rtty) { return; }
  SkimRbnFeed *f = h->feed ? h->feed : h->rbn;
  spot_origin = worker;
  if (f) { skim_rbn_feed_spot_external(f, call, mode, freq_hz, snr_db, speed); }
  spot_origin = NULL;
}

/* ---- the spots the feed sent (/api/spots) ---------------------------------------- */

#define SPOT_KEEP_S  (2 * 3600)
#define SPOT_MAX     20000

static void spot_log_add(Hd *h, const char *call, const char *mode, double freq_hz,
                         double snr_db, double speed) {
  SkimHdSpot sp = { 0 }, *s = &sp;
  s->t = g_get_real_time() / G_USEC_PER_SEC;
  g_strlcpy(s->call, call, sizeof(s->call));
  g_strlcpy(s->mode, mode, sizeof(s->mode));
  /* the mode may come from another skimmer's feed: letters and digits only
   * before it reaches a page */
  for (char *x = s->mode; *x; x++) {
    if (!g_ascii_isalnum(*x)) { *x = '?'; }
  }
  s->freq_hz = freq_hz;
  s->snr_db = snr_db;
  s->speed = speed;
  if (spot_origin) { g_strlcpy(s->src, spot_origin, sizeof(s->src)); }
  if (h->archive) {                            /* [archive]: on disk, by its day */
    const char *band = "";
    for (guint i = 0; i < h->nb; i++) {
      if (fabs(freq_hz - h->band[i].centre_hz) <= h->cfg.band[i].rate / 2.0) { band = h->band[i].name; break; }
    }
    skim_hd_archive_add(h->archive, s->t, call, mode, freq_hz, snr_db, speed, band, s->src);
  }
  skim_hd_spotlog_add(h->spot_log, s);
}

/* Save the spot log (each minute and at the end): a restart keeps the page. */
static void spot_log_save(Hd *h) {
  GError *err = NULL;
  if (h->spot_log && h->spot_path && !skim_hd_spotlog_save(h->spot_log, h->spot_path, &err)) {
    g_warning("spots: %s", err->message);
    g_clear_error(&err);
  }
}

typedef struct { Hd *h; GString *o; gint64 now, minutes; guint n; } SpotsJson;

static gboolean spot_json(const SkimHdSpot *s, gpointer user) {
  SpotsJson *j = user;
  Hd *h = j->h;
  GString *o = j->o;
  if (j->now - s->t > j->minutes * 60) { return FALSE; }
  const char *band = "";
  for (guint i = 0; i < h->nb; i++) {
    if (fabs(s->freq_hz - h->band[i].centre_hz) <= h->cfg.band[i].rate / 2.0) { band = h->band[i].name; break; }
  }
  char khz[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(khz, sizeof(khz), "%.1f", s->freq_hz / 1000.0);
  g_string_append_printf(o, "%s{\"t\":%" G_GINT64_FORMAT ",\"call\":\"", j->n++ ? "," : "", s->t);
  skim_json_append(o, s->call);
  g_string_append_printf(o, "\",\"mode\":\"%s\",\"khz\":%s,\"snr\":%.0f,\"speed\":%.0f,\"band\":\"",
                         s->mode, khz, s->snr_db, s->speed);
  skim_json_append(o, band);
  g_string_append(o, "\",\"src\":\"");
  skim_json_append(o, s->src);
  g_string_append(o, "\"}");
  return TRUE;
}

/* GET /api/spots?minutes=N: newest first, each with its band. */
static char *web_spots(GHashTable *q, SkimHdWebWho who, guint *st, const char **type, gpointer user) {
  const gboolean may = who == SKIM_HD_WEB_MANAGER;
  (void)st; (void)type;
  Hd *h = user;
  const char *mv = q ? g_hash_table_lookup(q, "minutes") : NULL;
  const gint64 minutes = CLAMP(mv ? g_ascii_strtoll(mv, NULL, 10) : 30, 1, SPOT_KEEP_S / 60);
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  GString *o = g_string_new(NULL);
  g_string_append_printf(o, "{\"now\":%" G_GINT64_FORMAT ",\"minutes\":%" G_GINT64_FORMAT
                         ",\"call\":\"", now, minutes);
  skim_json_append(o, h->cfg.feed_call ? h->cfg.feed_call : "");
  /* the locator whole for a manager, its square for everyone else */
  g_string_append_printf(o, "\",\"locator\":\"%.*s\",\"spots\":[", may ? 6 : 4, h->cfg.locator);
  SpotsJson j = { h, o, now, minutes, 0 };
  skim_hd_spotlog_foreach_newest(h->spot_log, spot_json, &j);
  g_string_append(o, "]}");
  return g_string_free(o, FALSE);
}

/* GET /api/wspr: the WSPR stations heard in the last 24 h (wspr.h). */
static char *web_wspr(GHashTable *q, SkimHdWebWho who, guint *st, const char **type, gpointer user) {
  (void)q; (void)who; (void)st; (void)type;
  Hd *h = user;
  if (!h->wspr) { return g_strdup("{\"off\":true}"); }
  return skim_hd_wspr_stations_json(h->wspr, 24, g_get_real_time() / G_USEC_PER_SEC);
}

/* GET /api/ft8: the FT8/FT4 decodes of the last 15 min (ft8.h) — ?dial=Hz one
 * dial, &since=<unix ms> the newer ones only (the palette polls). */
static char *web_ft8(GHashTable *q, SkimHdWebWho who, guint *st, const char **type, gpointer user) {
  (void)who; (void)st; (void)type;
  Hd *h = user;
  if (!h->ft8) { return g_strdup("{\"off\":true}"); }
  const char *d = q ? g_hash_table_lookup(q, "dial") : NULL, *s = q ? g_hash_table_lookup(q, "since") : NULL;
  const double dial = d ? g_ascii_strtod(d, NULL) : 0;
  const gint64 since = s ? g_ascii_strtoll(s, NULL, 10) : 0;
  return skim_hd_ft8_decodes_json(h->ft8, dial > 0 ? dial : 0, since > 0 ? since : 0);
}

/* GET /api/js8: the JS8 frames of the last 15 min (js8.h) — ?dial=Hz one
 * dial, &since=<unix ms> the newer ones only (the palette polls). */
static char *web_js8(GHashTable *q, SkimHdWebWho who, guint *st, const char **type, gpointer user) {
  (void)who; (void)st; (void)type;
  Hd *h = user;
  if (!h->js8) { return g_strdup("{\"off\":true}"); }
  const char *d = q ? g_hash_table_lookup(q, "dial") : NULL, *s = q ? g_hash_table_lookup(q, "since") : NULL;
  const double dial = d ? g_ascii_strtod(d, NULL) : 0;
  const gint64 since = s ? g_ascii_strtoll(s, NULL, 10) : 0;
  return skim_hd_js8_frames_json(h->js8, dial > 0 ? dial : 0, since > 0 ? since : 0);
}

/* A station JS8 heard sending (a decoder thread of js8.c, folded there):
 * [filter], then — confirmed only — PSK Reporter ([js8] upload) and the
 * telnet feed (never the RBN's port); a deep call not confirmed yet, or
 * [js8] feed=false, or no feed: the page alone. */
static void js8_spot(const char *call, double freq_hz, int snr_db, gboolean cq, const char *band,
                     const char *grid, gint64 t, gboolean confirmed, gpointer user) {
  Hd *h = user;
  if (!skim_hd_config_call_pass(&h->cfg, call)) { return; }   /* [filter] */
  if (confirmed && h->pskr && h->cfg.js8_upload) {
    skim_pskr_add(h->pskr, call, grid, freq_hz, snr_db, "JS8", band, t);
  }
  if (confirmed && h->cfg.js8_feed && h->feed) {
    skim_rbn_feed_spot_digital(h->feed, call, "JS8", freq_hz, snr_db, cq);   /* tap → the log */
  } else {
    spot_log_add(h, call, "JS8", freq_hz, snr_db, 0);
  }
}

/* A station FT8/FT4 heard sending (a decoder thread of ft8.c, already folded
 * there): [filter], then PSK Reporter ([ft8] upload: once an hour a band and
 * mode, pskreporter.h) and the telnet feed — never the RBN's port — or, with
 * [ft8] feed=false or no feed, the page alone. */
static void ft8_spot(const char *call, const char *mode, double freq_hz, int snr_db, gboolean cq,
                     const char *band, const char *grid, gint64 t, gpointer user) {
  Hd *h = user;
  if (!skim_hd_config_call_pass(&h->cfg, call)) { return; }   /* [filter] */
  if (h->pskr && h->cfg.ft8_upload) { skim_pskr_add(h->pskr, call, grid, freq_hz, snr_db, mode, band, t); }
  if (h->cfg.ft8_feed && h->feed) {
    skim_rbn_feed_spot_digital(h->feed, call, mode, freq_hz, snr_db, cq);   /* tap → the log */
  } else {
    spot_log_add(h, call, mode, freq_hz, snr_db, 0);
  }
}

/* Every spot the feed sends (the spotting thread): into the log, and a
 * worker's up the link to its hub. */
static void feed_tap(const char *call, const char *mode, double freq_hz, double snr_db,
                     double speed, gpointer user) {
  Hd *h = user;
  spot_log_add(h, call, mode, freq_hz, snr_db, speed);
  if (h->cfg.source != SKIM_HD_SOURCE_HUB || !h->src) { return; }
  char f[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(f, sizeof(f), "%.1f", freq_hz);
  char *line = g_strdup_printf("SPOT %s %s %s %.0f %.0f\n", call, mode, f, snr_db, speed);
  skim_iq_source_hub_send(h->src, line);
  g_free(line);
}

/* Every 10 s a worker tells its hub how its bands do (STAT) and its whole
 * status (STATUS) — the hub's page and history show them. */
static void worker_report(Hd *h) {
  GString *o = g_string_new(NULL);
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) {
      Dec *d = &h->band[i].dec[k];
      if (!d->p) { continue; }
      BandStat bs;
      band_stat(d, &bs);
      g_string_append_printf(o, "STAT %s %s %u %u %u %" G_GUINT64_FORMAT "\n", h->band[i].name,
                             MODE_KEY[k], bs.channels, bs.active, bs.stations, bs.spots);
    }
  }
  g_mutex_lock(&h->snap_lock);
  if (h->snap_json) { g_string_append_printf(o, "STATUS %s\n", h->snap_json); }
  g_mutex_unlock(&h->snap_lock);
  skim_iq_source_hub_send(h->src, o->str);
  g_string_free(o, TRUE);
}

/* A peer's CQ spot (peers thread): out on our feed, unless it is a duplicate
 * of what went out already (ours or another peer's). */
static void peer_spot(const char *peer, const char *call, const char *mode, double freq_hz,
                      double snr_db, double speed, gpointer user) {
  Hd *h = user;
  /* a peer's FT8/FT4/JS8 CQ: a digital line on the telnet feed, never on the
   * RBN Aggregator's port (as our own) — not a "0 BPS CQ" CW-style line */
  const gboolean digi = g_strcmp0(mode, "FT8") == 0 || g_strcmp0(mode, "FT4") == 0 ||
                        g_strcmp0(mode, "JS8") == 0;
  if (!digi && g_strcmp0(mode, "CW") != 0 && !h->cfg.feed_rtty) { return; }   /* CW-only feed */
  if (!skim_hd_config_call_pass(&h->cfg, call)) { return; }           /* [filter] */
  if (digi && !h->feed) { return; }                  /* the RBN port takes none */
  spot_origin = peer;
  if (digi) {
    skim_rbn_feed_spot_digital(h->feed, call, mode, freq_hz, snr_db, TRUE);
  } else {
    skim_rbn_feed_spot_external(h->feed ? h->feed : h->rbn, call, mode, freq_hz, snr_db, speed);
  }
  spot_origin = NULL;
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
    .iq_rate = cb->rate,
    .center_hz = b->centre_hz,
    .mode = rtty ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .cw_engine = cb->cw_engine,
    .decode_log_path = dlog,
    /* the local feed relays to the RBN's port; without it the RBN's alone */
    .rbn = rtty && !c->feed_rtty ? NULL : h->feed ? h->feed : h->rbn,
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
    .call_pass = skim_hd_config_filter_on(c) ? call_pass : NULL,
    .call_pass_user = h,
  };
  /* a phased band (ip=ab): the combiner's settings, [phase] and the band's phase= */
  SkimCombinerConfig phase;
  skim_combiner_config_default(&phase);
  phase.mode = (SkimPhaseMode)(cb->phase_mode >= 0 ? cb->phase_mode : c->phase_mode);
  phase.tc_s = c->phase_tc_s;
  phase.gate_db = c->phase_gate_db;
  phase.fixed_gain_db = c->phase_fixed_gain_db;
  phase.fixed_phase_deg = c->phase_fixed_phase_deg;
  pc.phased = cb->phased;
  pc.phase = cb->phased ? &phase : NULL;
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
  skim_hd_rp_askpass_main(argc, argv);        /* ssh's askpass: prints, exits */
  skim_log_ring_install();
  Hd *h = g_new0(Hd, 1);
  h->console_s = -1;
  h->http_port = -1;
  char *cfg = NULL;
  gboolean list_switches = FALSE, check_config = FALSE, fetch_model = FALSE;
  gboolean install_service = FALSE, uninstall_service = FALSE, find_radios = FALSE;
  gboolean find_skimmers = FALSE, version = FALSE;
  char *rp_cmd = NULL;
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
    { "find-radios", 0, 0, G_OPTION_ARG_NONE, &find_radios,
      "List the Red Pitayas on this LAN (address, receivers, idle or busy) and exit",
      NULL },
    { "find-skimmers", 0, 0, G_OPTION_ARG_NONE, &find_skimmers,
      "List the skimmer-headless instances on this LAN (address, name, role, call, "
      "ports) and exit", NULL },
    { "redpitaya", 0, 0, G_OPTION_ARG_STRING, &rp_cmd,
      "The skimmer's own Red Pitaya over SSH, then exit: connect (the root password "
      "once, never stored; the skimmer's key goes on the board), probe, status, "
      "install (the skimmer's agent in place of Pavel's two servers; --take-over "
      "even while another client streams), uninstall (Pavel's start.sh back). The "
      "board: HOST after the options, else the first [radio] host", "CMD" },
    { "install-service", 0, 0, G_OPTION_ARG_NONE, &install_service,
      "Write a systemd user service that runs this program (with --config, "
      "that config) and exit — it starts nothing; the next steps are printed", NULL },
    { "uninstall-service", 0, 0, G_OPTION_ARG_NONE, &uninstall_service,
      "Stop and remove that service and exit (the config stays)", NULL },
    { "list-switches", 0, 0, G_OPTION_ARG_NONE, &list_switches,
      "Print the SKIM_* environment switches the engine reads, and exit", NULL },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version,
      "Print \"skimmer-headless <version>\" (a development build: with its commit) and exit",
      NULL },
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
  GOptionContext *oc = g_option_context_new("— multi-band CW and RTTY skimmer on a Red Pitaya, no GUI");
  g_option_context_add_main_entries(oc, opts, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(oc, &argc, &argv, &err)) {
    g_printerr("%s\n", err->message);
    return EXIT_CONFIG;
  }
  g_option_context_free(oc);
  if (version) {
    printf("skimmer-headless %s%s\n", SKIMMER_VERSION, SKIMMER_BUILD);
    return 0;
  }
  if (list_switches) {
    skim_switches_print_known();
    return 0;
  }
  if (fetch_model) { return fetch_model_cli(); }
  if (find_radios) { return find_radios_cli(); }
  if (find_skimmers) { return find_skimmers_cli(); }
  if (install_service || uninstall_service) {
    gboolean ok;
    if (install_service) {
      char *exe = skim_hd_service_exe();
      char *conf = cfg ? g_canonicalize_filename(cfg, NULL) : NULL;
      ok = skim_hd_service_install(exe, conf, &err);
      g_free(conf);
      g_free(exe);
    } else {
      ok = skim_hd_service_uninstall(&err);
    }
    if (!ok) { g_printerr("service: %s\n", err->message); }
    return ok ? 0 : 1;
  }
  h->cfg_path = cfg ? cfg : g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                             "headless.ini", NULL);
  if (!g_file_test(h->cfg_path, G_FILE_TEST_EXISTS) && !check_config) {
    /* the radios on the LAN go into it as a comment: where the addresses are */
    char *l = radios_lines("#   ");
    char *note = l[0] ? g_strdup_printf("# On this LAN when this file was written:\n%s", l)
                      : g_strdup("# (no Red Pitaya answered on this LAN when this file was written)\n");
    g_free(l);
    const gboolean wrote = skim_hd_config_write_default(h->cfg_path, note, &err);
    g_free(note);
    if (!wrote) {
      g_printerr("config: %s\n", err->message);
      return EXIT_CONFIG;
    }
    g_message("config: %s did not exist — written with defaults", h->cfg_path);
  }
  SkimHdConfig *c = &h->cfg;
  if (!g_file_get_contents(h->cfg_path, &h->cfg_text, NULL, &err) ||
      !skim_hd_config_parse(c, h->cfg_text, strlen(h->cfg_text), h->cfg_path, &err)) {
    g_printerr("config: %s\n", err->message);
    return EXIT_CONFIG;
  }
  if (rp_cmd) { return redpitaya_cli(rp_cmd, argc > 1 ? argv[1] : NULL, c, h->take_over); }
  if (h->console_s < 0) {
    h->console_s = skim_hd_service_under_systemd() ? 0 : c->console_s >= 0 ? c->console_s : 10;
  }
  if (h->http_port < 0) { h->http_port = c->http_port >= 0 ? c->http_port : 8073; }
  /* A worker: its bands and decoding policy come from its hub — the link
   * first, the hub's fragment merged into this config, then the rest. */
  SkimHubLink *hub_link = NULL;
  char *hub_frag = NULL;
  if (c->source == SKIM_HD_SOURCE_HUB && !check_config) {
    g_message("hub link: looking for the hub (%s) as the worker %s", c->hosts_text, c->hub_name);
    hub_link = skim_hub_link_open(c, 10, &hub_frag, &err);
    if (!hub_link) {
      g_warning("hub link: %s — starting without bands, still looking", err->message);
      g_clear_error(&err);
    } else if (!hub_frag) {
      g_message("hub link: %s has no band for %s — waiting for one",
                skim_hub_link_desc(hub_link), c->hub_name);
    } else {
      char *merged = skim_hd_config_worker_merge(h->cfg_text, strlen(h->cfg_text), hub_frag,
                                                 strlen(hub_frag), &err);
      SkimHdConfig nc;
      if (merged && skim_hd_config_parse(&nc, merged, strlen(merged), h->cfg_path, &err)) {
        skim_hd_config_clear(c);
        *c = nc;
        g_free(h->cfg_text);
        h->cfg_text = merged;
        g_message("hub link: %s assigns %u band%s to %s", skim_hub_link_desc(hub_link), c->nb,
                  c->nb == 1 ? "" : "s", c->hub_name);
      } else {
        g_warning("hub link: the hub's bands do not parse here (%s) — none decoded",
                  err ? err->message : "?");
        g_clear_error(&err);
        g_free(merged);
      }
    }
  }
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
    for (guint i = 0, rx = 0; i < c->nlisted; i++) {
      const SkimHdBand *b = &c->listed[i];
      char *m = skim_hd_band_modes(b, c->engine);
      char *slot = b->off ? g_strdup("off") : g_strdup_printf("RX%u", ++rx);
      printf("  %-6s %-5s %.1f kHz  %s\n", slot, b->name, b->centre_hz / 1000.0, m);
      g_free(slot);
      g_free(m);
    }
    printf("  feed   %s port %d, %s\n", c->feed_call && c->feed_call[0] ? c->feed_call : "(no call)",
           c->feed_port, c->feed_rtty ? "CW and RTTY" : "CW only");
    if (c->rbn_on) { printf("  rbn    port %d, spots held %u s\n", c->rbn_port, c->rbn_delay_s); }
    printf("  gate   %s, %s (CW)\n", h->gate_id ? h->gate_id : "none", h->feed_gate_mode);
    if (c->filter_call) {
      printf("  filter only calls %s %s\n", c->filter_regex ? "matching" : "containing", c->filter_call);
    }
    if (c->filter_bad) { printf("  filter not the bad calls %s\n", c->filter_bad); }
    if (c->filter_mycall && c->feed_call && c->feed_call[0]) {
      printf("  filter not %s nor calls within %u characters of it\n", c->feed_call,
             c->filter_mycall_diff);
    }
    gboolean dcw = FALSE, v4 = FALSE;
    for (guint i = 0; i < c->nb; i++) {
      dcw |= c->band[i].cw && c->band[i].cw_engine == SKIM_CW_ENGINE_DEEPCW;
      v4 |= c->band[i].cw && !c->band[i].off && c->band[i].cw_engine == SKIM_CW_ENGINE_V4;
    }
    if (v4) {
      skim_decode_cw_v4_set_workers(c->v4_workers);
      if (!skim_decode_cw_v4_available(&err)) {
        printf("  v4 NOT AVAILABLE, those bands run v3: %s\n", err->message);
        g_clear_error(&err);
      } else {
        char *rt = skim_decode_cw_v4_runtime_info();
        printf("  v4 DeepCW v2 narrow, built in, on %s\n", rt);
        g_free(rt);
      }
    }
    if (dcw) {
      /* the bands asking for DeepCW start anyway — on v2 when it is not here */
      char *model = skim_decode_deepcw_model_path();
      if (!skim_deepcw_model_check(model, &err)) {
        printf("  deepcw NOT AVAILABLE, those bands run v3: %s\n", err->message);
      } else if (!skim_decode_deepcw_available(&err)) {
        printf("  deepcw NOT AVAILABLE, those bands run v3: %s\n", err->message);
      } else {
        printf("  deepcw %s, ONNX Runtime %s\n", model, skim_decode_deepcw_runtime_info());
      }
      g_clear_error(&err);
      g_free(model);
    }
    return 0;
  }
  /* [update]: a version installed a moment ago that keeps crashing goes
   * back to the one before it — before the radio is touched */
  {
    const char *ai = g_getenv("APPIMAGE");
    h->upd_state = skim_hd_update_state_path();
    h->upd_env = (SkimHdUpdateEnv){
      .appimage = ai && g_path_is_absolute(ai) && g_file_test(ai, G_FILE_TEST_IS_REGULAR)
                      ? g_strdup(ai) : NULL,
      .version = SKIMMER_VERSION,
      .build = SKIMMER_BUILD,
      .config_path = g_canonicalize_filename(h->cfg_path, NULL),
      .state_path = h->upd_state,
      .base_url = skim_hd_update_env_base_url(),
      .ca_path = skim_scp_env_ca_path(),
    };
    if (skim_hd_update_boot(&h->upd_env)) {
      g_warning("update: starting the earlier version again");
      skim_hd_update_exec(h->upd_env.appimage, h->argv);
      g_warning("update: exec %s failed: %s — this version goes on", h->upd_env.appimage,
                g_strerror(errno));
    }
  }
  g_mutex_init(&h->snap_lock);
  g_mutex_init(&h->dcw_lock);
  g_mutex_init(&h->v4b_lock);
  h->v4b_state = "none";
  h->v4_peak = NAN;
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
  h->temp_paths = temp_sensors();
  h->temp_c = NAN;
  h->temp_peak = NAN;
  g_message("skimmer-headless %s — %s %s, %u band%s × %u kHz, config %s", SKIMMER_VERSION,
            c->source == SKIM_HD_SOURCE_MOCK ? "mock source" : "HPSDR P1",
            c->source == SKIM_HD_SOURCE_MOCK ? "(no radio)" : c->hosts_text, c->nb,
            c->nb == 1 ? "" : "s", c->rate / 1000, h->cfg_path);
  h->switches_json = skim_switches_scan();
  if (skim_hd_service_under_systemd()) {
    g_message("service: running under systemd — exit 3 retries after a minute, a "
              "restart re-executes in place%s", h->console_s ? "" : "; console table off");
    if (skim_hd_service_unit_stale()) {
      g_warning("service: the unit predates KillMode=process — every stop of the AppImage "
                "ends in SIGBUS (status=7/BUS); run --install-service again");
    }
  }

  /* Callsign dictionary: loaded once for every pipeline, kept current; the
   * own call's filter lets the calls it knows through */
  skim_hd_config_set_known(skim_callsign_dict_has);
  char *scp = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux", "master.scp", NULL);
  if (g_file_test(scp, G_FILE_TEST_EXISTS) && !skim_callsign_dict_load(scp, &err)) {
    g_warning("MASTER.SCP %s not loaded: %s", scp, err->message);
    g_clear_error(&err);
  }
  h->scp = skim_scp_updater_new(scp, on_scp, h);
  skim_scp_updater_start(h->scp, 15);
  g_free(scp);

  /* [update]: the releases looked at once a day, the first time 10 min in */
  h->upd = skim_hd_updater_new(&h->upd_env, (SkimHdUpdateMode)c->update_mode, c->update_hour,
                               upd_restart, h);
  skim_hd_updater_start(h->upd, 600);
  {
    char *why = skim_hd_update_why_not(&h->upd_env);
    if (c->update_mode == SKIM_HD_UPDATE_OFF) {
      g_message("update: off — the releases are not looked at");
    } else if (why) {
      g_message("update: %s, a newer release only reported — %s",
                skim_hd_update_mode_name((SkimHdUpdateMode)c->update_mode), why);
    } else if (c->update_mode == SKIM_HD_UPDATE_AUTO) {
      g_message("update: auto — a newer release goes in at %02u UTC, into %s", c->update_hour,
                h->upd_env.appimage);
    } else {
      g_message("update: notify — a newer release is shown on the web page");
    }
    g_free(why);
  }

  /* [archive] first: the feed's tap writes into it from the first spot */
  if (c->archive_dir) {
    GError *aerr = NULL;
    h->archive = skim_hd_archive_new(c->archive_dir, &aerr);
    if (h->archive) {
      g_message("archive: every spot into %s, a gzip file a UTC day (YYYYMMDD-spots.gz)",
                c->archive_dir);
    } else {
      g_warning("archive: %s — no spot archive", aerr->message);
      g_clear_error(&aerr);
    }
  }
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

  /* a worker's spots go up its hub link: through its own feed when it has
   * one, else through a feed that listens nowhere */
  if (c->source == SKIM_HD_SOURCE_HUB && c->feed_call && c->feed_call[0] && !h->feed) {
    h->feed = skim_rbn_feed_new_sink(c->feed_call);
  }

  if (c->rbn_on) {
    if (!c->feed_call || !c->feed_call[0]) {
      g_warning("rbn: [feed] call is empty — no spots to the RBN");
    } else {
      h->rbn = skim_rbn_feed_new(c->feed_call, (guint16)c->rbn_port, &err);
      if (!h->rbn) {
        g_warning("rbn: port %d: %s — no spots to the RBN", c->rbn_port, err->message);
        g_clear_error(&err);
      } else {
        skim_rbn_feed_set_delay(h->rbn, c->rbn_delay_s);
        if (h->feed) { skim_rbn_feed_set_relay(h->feed, h->rbn); }
        g_message("rbn: the RBN Aggregator's port %d as %s, spots held %u s", c->rbn_port,
                  c->feed_call, c->rbn_delay_s);
      }
    }
  }

  /* every spot that goes out, whatever made it: the page's map and DXC */
  h->spot_log = skim_hd_spotlog_new(SPOT_KEEP_S, SPOT_MAX);
  h->spot_path = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "spots.tsv", NULL);
  {
    const guint n = skim_hd_spotlog_load(h->spot_log, h->spot_path,
                                         g_get_real_time() / G_USEC_PER_SEC);
    if (n) { g_message("spots: %u of the last %d h back from %s", n, SPOT_KEEP_S / 3600, h->spot_path); }
  }
  if (h->feed || h->rbn) { skim_rbn_feed_set_tap(h->feed ? h->feed : h->rbn, feed_tap, h); }

  if (c->peers && c->peers[0]) {
    if (!h->feed && !h->rbn) {
      g_warning("peers: no feed to send their spots on ([feed] call and port, or [rbn]) — "
                "not connected");
    } else {
      h->peers = skim_hd_peers_new((const char *const *)c->peers, c->feed_call, peer_spot, h);
      char *l = g_strjoinv(", ", c->peers);
      g_message("peers: their CQ spots go out as %s, duplicates dropped: %s", c->feed_call, l);
      g_free(l);
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
  skim_decode_cw_v4_set_workers(c->v4_workers);   /* before the first v4 channel */
  for (guint i = 0; i < h->nb; i++) {
    Band *b = &h->band[i];
    const SkimHdBand *cb = &c->band[i];
    g_strlcpy(b->name, cb->name, sizeof(b->name));
    b->centre_hz = cb->centre_hz;
    g_snprintf(b->label, sizeof(b->label), "RX%u %s", i + 1, b->name);
    if (cb->worker[0]) {                       /* received here, decoded there */
      b->worker = cb->worker;
      continue;
    }
    if (cb->cw && cb->rtty) { b->pairs = skim_rtty_pairs_new(); }
    for (guint k = 0; k < M_N; k++) {
      if (k == M_CW ? !cb->cw : !cb->rtty) { continue; }
      if (!dec_start(h, b, k, cb, logdir, day, c->feed_learn ? learndir : NULL, start,
                     &err)) {
        g_printerr("pipeline %s %s: %s\n", b->name, MODE_KEY[k], err->message);
        skim_hd_update_stopped(&h->upd_env);
        return 1;
      }
    }
  }
  if (c->feed_learn) { g_message("learn: decode tap in %s", learndir); }
  if (!h->gate) {
    g_message("feed gate: off");
  } else if (!h->feed && !h->rbn) {
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
  if (h->cfg.filter_call) {
    g_message("filter: only calls %s \"%s\" go anywhere — the rest are dropped where they "
              "are decoded", h->cfg.filter_regex ? "matching" : "containing", h->cfg.filter_call);
  }
  if (h->cfg.filter_bad) {
    g_message("filter: the bad calls (%s) are dropped where they are decoded", h->cfg.filter_bad);
  }
  if (h->cfg.filter_mycall) {
    if (h->cfg.feed_call && h->cfg.feed_call[0]) {
      g_message("filter: %s and the calls within %u characters of it are dropped where they "
                "are decoded", h->cfg.feed_call, h->cfg.filter_mycall_diff);
    } else {
      g_warning("filter: mycall=true, but [feed] call is empty — nothing to compare with");
    }
  }
  for (guint i = 0; i < h->nb; i++) {
    const Band *b = &h->band[i];
    if (b->worker) { continue; }               /* logged with the hub         */
    g_message("decode: %s%s%s%s%s", b->name, b->dec[M_CW].p ? " CW, engine " : "",
              b->dec[M_CW].p ? skim_pipeline_cw_engine_name(b->dec[M_CW].p) : "",
              b->dec[M_RTTY].p ? " + RTTY" : "",
              b->dec[M_RTTY].p && h->feed && !c->feed_rtty ? " (not on the feed)" : "");
    if (b->pairs && c->suppress_cw_on_rtty) {
      g_message("decode: %s CW stays off the RTTY stations RTTY copies", b->name);
    }
  }
  /* CW v4: what runs, once, and a band that asked for it and runs v3 */
  for (guint i = 0; i < h->nb; i++) {
    const Band *b = &h->band[i];
    if (!b->dec[M_CW].p || c->band[i].cw_engine != SKIM_CW_ENGINE_V4) { continue; }
    if (!h->v4_wanted) {
      char *rt = skim_decode_cw_v4_runtime_info();
      g_message("decode: CW v4 (DeepCW v2 narrow) on %s", rt);
      g_free(rt);
    }
    h->v4_wanted = TRUE;
    if (strcmp(skim_pipeline_cw_engine_name(b->dec[M_CW].p), "cw-v4") != 0) {
      g_warning("decode: %s asks for CW v4, runs %s", b->name,
                skim_pipeline_cw_engine_name(b->dec[M_CW].p));
    }
  }
  /* the compute check for the Settings page: cached per CPU, encoder,
   * threads and build — measured (2 s) only when that changed */
  if (!v4_bench_load(h)) { v4_bench_start(h); }
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
        h->dcw_fallback = g_strdup("the engine chose another decoder (SKIM_CW_ENGINE?)");
      }
      g_clear_error(&e);
      g_warning("decode: DeepCW asked for, not available (%s) — those bands run cw-v3.1",
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
  g_message("stats: %s (a record a minute for 7 days, per 15 min for 90 days, per hour for "
            "2 years)", stats_path);
  g_free(stats_path);

  {
    gboolean any_rtty = FALSE;
    for (guint i = 0; i < h->nb; i++) { any_rtty |= h->band[i].dec[M_RTTY].p != NULL; }
    if (c->rtty_adaptive && !any_rtty) {
      g_message("rtty: adaptive — no band decodes RTTY here, nothing to switch");
    }
    /* the thermal guard's limit: as set, else the sensor's critical − 15 °C */
    double lim = c->thermal_limit_c;
    if (c->thermal && lim <= 0) {
      const double crit = cpu_crit_c(h->temp_paths);
      lim = isnan(crit) ? 80.0 : crit - 15.0;
      g_message("thermal: limit %.0f °C (%s)", lim,
                isnan(crit) ? "no critical temperature from the sensor: 80 °C"
                            : "the sensor's critical temperature − 15 °C");
    }
    if (c->thermal && !h->temp_paths->len) {
      g_warning("thermal: no CPU temperature sensor found — the guard has nothing to go by");
    }
    const SkimHdGovernorConfig gc = {
      .rtty_adaptive = c->rtty_adaptive && any_rtty,
      .rtty_spots = c->rtty_adaptive_spots,
      .rtty_minutes = c->rtty_adaptive_minutes,
      .thermal = c->thermal,
      .limit_c = lim,
      .average_min = c->thermal_average_min,
      .restore_margin_c = c->thermal_margin_c,
      .probe_every_min = c->thermal_probe_every_min,
      .probe_min = c->thermal_probe_min,
      .v4_guard = c->v4_guard,
    };
    h->gov = skim_hd_governor_new(&gc, g_get_real_time() / (60 * G_USEC_PER_SEC));
    gboolean can[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {          /* a worker's band costs no decoding here */
      can[i] = !h->band[i].worker && (h->band[i].dec[M_CW].p || h->band[i].dec[M_RTTY].p);
    }
    skim_hd_governor_set_bands(h->gov, h->nb, names, can);
    gboolean v4[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {
      v4[i] = h->band[i].dec[M_CW].p &&
              strcmp(skim_pipeline_cw_engine_name(h->band[i].dec[M_CW].p), "cw-v4") == 0;
    }
    skim_hd_governor_set_v4(h->gov, v4);
    pause_apply(h);
  }

  if (c->wspr) {                               /* [wspr]: before the web page reads it */
    double centres[SKIM_HD_MAX_BANDS], rates[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {
      centres[i] = h->band[i].centre_hz;
      rates[i] = c->band[i].rate;
    }
    char *wdir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
    const SkimHdWsprConfig wc = {
      .call = c->wspr_call ? c->wspr_call : c->feed_call,
      .locator = c->wspr_locator[0] ? c->wspr_locator : c->locator,
      .upload = c->wspr_upload,
      .upload_url = c->wspr_upload_url,
      .dir = wdir,
      .flags = (c->wspr_deep ? SKIM_WSPR_DEEP : 0) | (c->wspr_subtract ? 0 : SKIM_WSPR_NO_SUBTRACT),
    };
    h->wspr = skim_hd_wspr_new(&wc, h->nb, names, centres, rates);
    g_free(wdir);
    char *dials = skim_hd_wspr_dials_text(h->wspr);
    char *wj = skim_hd_wspr_status_json(h->wspr);
    if (!*dials) {
      g_message("wspr: no WSPR frequency inside the bands received (dial + 1500 Hz within 45 %% "
                "of the rate around a centre) — nothing to decode");
    } else {
      g_message("wspr: decoding %s every even minute%s", dials,
                !strstr(wj, "\"decoder\":true") ? " — but no decoder (skimmer-wsprd) found"
                : c->wspr_upload ? "; spots to WSPRnet" : "");
    }
    g_free(wj);
    g_free(dials);
  }

  /* PSK Reporter ([ft8] / [js8] upload, one receiver under [ft8] call and
   * locator): before the spots come; a worker's hub reports its IQ */
  const gboolean up8 = c->ft8 && c->ft8_upload, upj = c->js8 && c->js8_upload;
  if ((up8 || upj) && c->source != SKIM_HD_SOURCE_HUB) {
    char *sw = g_strdup_printf("skimmer-for-linux %s%s (%s%s%s)", SKIMMER_VERSION, SKIMMER_BUILD,
                               up8 ? "WSJT-X 3.0.2 FT8/FT4" : "", up8 && upj ? ", " : "",
                               upj ? "JS8Call-improved JS8" : "");
    const SkimPskrConfig pc = {
      .call = c->ft8_call ? c->ft8_call : c->feed_call,
      .locator = c->ft8_locator[0] ? c->ft8_locator : c->locator,
      .software = sw, .host = c->ft8_pskr_host, .port = (guint16)c->ft8_pskr_port,
      .interval_s = c->ft8_pskr_host ? 1 : 0,  /* the gate's stand-in: at once */
    };
    h->pskr = skim_pskr_new(&pc);
    g_message("pskreporter: the %s%s%s stations heard to PSK Reporter (%s:%u) as %s %s, a datagram every %s",
              up8 ? "FT8/FT4" : "", up8 && upj ? " and " : "", upj ? "JS8" : "",
              c->ft8_pskr_host ? c->ft8_pskr_host : SKIM_PSKR_HOST,
              c->ft8_pskr_port ? c->ft8_pskr_port : SKIM_PSKR_PORT, pc.call, pc.locator,
              c->ft8_pskr_host ? "1 s (a stand-in)" : "5 min");
    g_free(sw);
  }

  if (c->ft8 && c->source == SKIM_HD_SOURCE_HUB) {   /* [ft8]: before the web page reads it */
    g_message("ft8: off on a worker — its hub decodes FT8/FT4 from the same IQ");
  } else if (c->ft8) {
    double centres[SKIM_HD_MAX_BANDS], rates[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {
      centres[i] = h->band[i].centre_hz;
      rates[i] = c->band[i].rate;
    }
    char *fdir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
    const SkimHdFt8Config fc = {
      .ft8 = c->ft8_ft8, .ft4 = c->ft8_ft4, .depth = c->ft8_depth,
      .lo_hz = c->ft8_lo_hz, .hi_hz = c->ft8_hi_hz, .threads = c->ft8_threads,
      .bands = (const char *const *)c->ft8_bands,
      .dir = fdir, .spot = ft8_spot, .user = h,
    };
    h->ft8 = skim_hd_ft8_new(&fc, h->nb, names, centres, rates);
    g_free(fdir);
    char *dials = skim_hd_ft8_dials_text(h->ft8);
    char *fj = skim_hd_ft8_status_json(h->ft8);
    if (!*dials) {
      g_message("ft8: no FT8/FT4 frequency inside the bands received (dial + %u-%u Hz within "
                "45 %% of the rate around a centre) — nothing to decode", c->ft8_lo_hz, c->ft8_hi_hz);
    } else {
      g_message("ft8: decoding %s (depth %u, %u thread%s)%s", dials, c->ft8_depth, c->ft8_threads,
                c->ft8_threads == 1 ? "" : "s",
                !strstr(fj, "\"decoder\":true") ? " — but no decoder (skimmer-ft8d) found"
                : !c->ft8_feed ? "; the web page only, not the feed"
                : !h->feed ? "; no telnet feed — the web page only" : "; senders to the feed");
    }
    g_free(fj);
    g_free(dials);
  }

  if (c->js8 && c->source == SKIM_HD_SOURCE_HUB) {   /* [js8]: before the web page reads it */
    g_message("js8: off on a worker — its hub decodes JS8 from the same IQ");
  } else if (c->js8) {
    double centres[SKIM_HD_MAX_BANDS], rates[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {
      centres[i] = h->band[i].centre_hz;
      rates[i] = c->band[i].rate;
    }
    char *jdir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
    const SkimHdJs8Config jc = {
      .submodes = c->js8_submodes, .lo_hz = c->js8_lo_hz, .hi_hz = c->js8_hi_hz,
      .threads = c->js8_threads, .serve = c->js8_serve, .clean_quality = c->js8_clean,
      .bands = (const char *const *)c->js8_bands, .dir = jdir, .spot = js8_spot, .user = h,
    };
    h->js8 = skim_hd_js8_new(&jc, h->nb, names, centres, rates);
    g_free(jdir);
    char *dials = skim_hd_js8_dials_text(h->js8);
    char *jj = skim_hd_js8_status_json(h->js8);
    if (!*dials) {
      g_message("js8: no JS8 frequency inside the bands received (dial + %u-%u Hz within "
                "45 %% of the rate around a centre) — nothing to decode", c->js8_lo_hz, c->js8_hi_hz);
    } else {
      GString *subs = g_string_new(NULL);
      static const char *const SUBS[] = { "normal", "fast", "turbo", "slow" };
      for (guint k = 0; k < 4; k++) {
        if (c->js8_submodes & (1u << k)) { g_string_append_printf(subs, "%s%s", subs->len ? "/" : "", SUBS[k]); }
      }
      g_message("js8: decoding %s (%s, %s, %u thread%s, clean from quality %.2f)%s", dials, subs->str,
                c->js8_serve ? "one decoder a dial kept running" : "a decoder run a period",
                c->js8_threads, c->js8_threads == 1 ? "" : "s", c->js8_clean,
                !strstr(jj, "\"decoder\":true") ? " — but no decoder (skimmer-js8d) found"
                : !c->js8_feed ? "; the web page only, not the feed"
                : !h->feed ? "; no telnet feed — the web page only" : "; confirmed senders to the feed");
      g_string_free(subs, TRUE);
    }
    g_free(jj);
    g_free(dials);
  }

  if (c->source != SKIM_HD_SOURCE_HUB && c->source != SKIM_HD_SOURCE_MOCK) {
    char *sdir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
    h->rp = skim_hd_rp_new(sdir, NULL, rp_default_host(c));
    g_free(sdir);
  }
  SkimHdWeb *web = NULL;
  if (h->http_port > 0) {
    double centres[SKIM_HD_MAX_BANDS], rates[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) {
      centres[i] = h->band[i].centre_hz;
      rates[i] = c->band[i].rate;
    }
    h->wf = skim_hd_waterfall_new(h->nb, names, centres, rates);
    const SkimHdWebHooks hooks = {
      .status = web_status, .stats = web_stats, .log = web_log, .config = web_config,
      .skimmers = web_skimmers, .spots = web_spots, .skimsrv = web_skimsrv, .wspr = web_wspr,
      .ft8 = web_ft8, .js8 = web_js8,
      .config_post = web_config_post, .config_import = web_config_import,
      .restart = web_restart, .rollback = web_rollback,
      .deepcw = web_deepcw,
      .v4bench = web_v4bench,
      .update = web_update, .update_post = web_update_post,
      .redpitaya = web_redpitaya, .redpitaya_post = web_redpitaya_post,
      .phase = web_phase_get, .phase_post = web_phase_post, .phase_hist = web_phase_hist,
      .waterfall = web_waterfall,
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
      if (c->visitors_login) {
        char *vdir = g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", NULL);
        h->vis = skim_hd_visitors_new(vdir, &err);
        g_free(vdir);
        if (!h->vis) {
          g_warning("visitors: %s — no sign-in, the page is open to all", err->message);
          g_clear_error(&err);
        } else {
          char *fr = g_strjoinv(", ", c->visitors_free);
          g_message("visitors: from outside %s they sign in with a call; the log: %s", fr,
                    skim_hd_visitors_path(h->vis));
          g_free(fr);
          skim_hd_waterfall_set_view_cb(h->wf, wf_view, h);
        }
      }
      /* free_from also says who is at home (sees CPU, temperature, lost) */
      skim_hd_web_set_visitors(web, h->vis, (const char *const *)c->visitors_free);
    }
  }

  /* the hub server: workers on other PCs connect for the bands named after
   * them (and to be listed as available, with nothing assigned yet) */
  if (c->hub_port > 0 && c->source != SKIM_HD_SOURCE_HUB) {
    const char *bw[SKIM_HD_MAX_BANDS];
    for (guint i = 0; i < h->nb; i++) { bw[i] = c->band[i].worker; }
    h->hub = skim_hd_hub_new((guint16)c->hub_port, (const char *const *)c->hub_allow, bw, h->nb,
                             hub_assign, hub_spot, h, &err);
    if (!h->hub) {
      g_warning("hub: port %d: %s — no workers", c->hub_port, err->message);
      g_clear_error(&err);
    } else {
      g_message("hub: workers may connect on port %u (from %s)", skim_hd_hub_port(h->hub),
                c->hub_allow[0]);
    }
  }
  for (guint i = 0; i < h->nb; i++) {
    if (h->band[i].worker) {
      g_message("decode: %s on the worker %s%s", h->band[i].name, h->band[i].worker,
                h->hub ? "" : " — but there is no hub server: not decoded at all");
    }
  }

  h->disco = skim_hd_discovery_new(disco_info, h, &err);
  if (!h->disco) {
    g_warning("discovery: %s — this skimmer will not be found on the LAN", err->message);
    g_clear_error(&err);
  }

  h->loop = g_main_loop_new(NULL, FALSE);
  g_unix_signal_add(SIGINT, on_signal, h);
  g_unix_signal_add(SIGTERM, on_signal, h);
  if (c->source == SKIM_HD_SOURCE_HUB) {
    h->src = skim_iq_source_hub_new(c, hub_link, hub_frag);   /* takes the link */
    h->src->cfg = c;
    h->src->iq_cb = rx_iq_cb;
    h->src->iq_user = h;
  } else {
    h->src = skim_iq_source_new(c, h->take_over, rx_iq_cb, h);
  }
  h->src->iq2_cb = rx_iq2_cb;                  /* phased bands (ip=ab)        */
  g_free(hub_frag);
  skim_iq_source_start(h->src);                /* first start attempt now     */
  tick(h);
  if (!h->exit_code) {                         /* a quit before run is lost   */
    g_timeout_add_seconds(1, tick, h);
    g_main_loop_run(h->loop);
  }

  /* Teardown: the radio first (stop — only if the stream is still ours),
   * then the engines, then the feed they spot into. */
  g_clear_pointer(&web, skim_hd_web_free);
  g_clear_pointer(&h->rp, skim_hd_rp_free);     /* a job in flight is cancelled */
  g_clear_pointer(&h->disco, skim_hd_discovery_free);
  g_cancellable_cancel(h->dcw_cancel);         /* a fetch in flight stops      */
  if (h->dcw_thread) { g_thread_join(h->dcw_thread); }
  if (h->v4b_thread) { g_thread_join(h->v4b_thread); }
  g_clear_pointer(&h->src, skim_iq_source_free);
  g_clear_pointer(&h->wspr, skim_hd_wspr_free);      /* nothing pushes into it */
  g_clear_pointer(&h->ft8, skim_hd_ft8_free);        /* … before the feed it spots to */
  g_clear_pointer(&h->js8, skim_hd_js8_free);
  g_clear_pointer(&h->pskr, skim_pskr_free);         /* … and what waits, sent once */
  g_clear_pointer(&h->wf, skim_hd_waterfall_free);   /* nothing pushes into it */
  g_clear_pointer(&h->vis, skim_hd_visitors_free);   /* the open visits logged */
  g_clear_pointer(&h->hub, skim_hd_hub_free);     /* nothing pushes any more   */
  for (guint i = 0; i < h->nb; i++) {
    for (guint k = 0; k < M_N; k++) { skim_pipeline_free(h->band[i].dec[k].p); }
    g_clear_pointer(&h->band[i].pairs, skim_rtty_pairs_unref);
    skim_rec_free(h->band[i].rec);
    g_clear_pointer(&h->band[i].bc, skim_band_combine_free);
    g_clear_pointer(&h->band[i].phist, skim_phase_hist_free);   /* the minute in hand */
    g_clear_pointer(&h->band[i].bc_y, g_free);
    g_clear_pointer(&h->band[i].bc_ca, g_free);
    g_clear_pointer(&h->band[i].bc_cb, g_free);
  }
  g_clear_pointer(&h->stats, skim_stats_free);  /* saves                       */
  g_clear_pointer(&h->gov, skim_hd_governor_free);
  g_clear_pointer(&h->archive, skim_hd_archive_free);   /* flushes             */
  g_clear_pointer(&h->archive_err, g_free);
  g_clear_pointer(&h->temp_paths, g_ptr_array_unref);
  g_clear_pointer(&h->peers, skim_hd_peers_free);  /* they spot into the feed */
  g_clear_pointer(&h->feed, skim_rbn_feed_free);   /* it relays to rbn: first */
  g_clear_pointer(&h->rbn, skim_rbn_feed_free);
  spot_log_save(h);                  /* nothing spots any more: the page's spots kept */
  g_clear_pointer(&h->spot_log, skim_hd_spotlog_free);
  g_clear_pointer(&h->spot_path, g_free);
  g_clear_pointer(&h->scp, skim_scp_updater_free);
  g_clear_pointer(&h->upd, skim_hd_updater_free);
  skim_hd_update_stopped(&h->upd_env);         /* an orderly end, whatever code */
  if (h->exit_code == EXIT_RESTART && g_strcmp0(g_getenv("SKIM_SUPERVISED"), "1") != 0) {
    g_message("restart: starting again%s%s", h->exec_path ? " from " : "",
              h->exec_path ? h->exec_path : "");
    fflush(stdout);
    if (h->exec_path) { skim_hd_update_exec(h->exec_path, h->argv); }
    execv("/proc/self/exe", h->argv);
    g_printerr("restart: exec failed: %s — exiting %d\n", g_strerror(errno), EXIT_RESTART);
  }
  g_message("stopped");
  return h->exit_code;
}
