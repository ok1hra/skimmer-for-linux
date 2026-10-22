/*
 * skimmer-replay — offline decode of a recorded IQ sample (M3 A/B harness).
 *
 *   skimmer-replay <file.cf32> [rate_hz] [center_hz]
 *   skimmer-replay <file.wav | -> [rate_hz] [center_hz]
 *
 * A .wav is a 16-bit stereo I/Q recording as SDR software writes it (CW
 * Skimmer, SpectraVue): rate from the fmt chunk, start time and centre from
 * the SpectraVue `auxi` chunk. "-" reads WAV from stdin, and WAVs concatenated
 * there (`ssh host cat a.wav b.wav | skimmer-replay -`) play as one stream —
 * a gap or overlap between their auxi start times is reported, not filled.
 * SKIM_REPLAY_CONJ=1 conjugates on input (an RF-inverted recording).
 * SKIM_REPLAY_IQ_SKEW=<samples> moves Q that far ahead of I (windowed-sinc
 * fractional delay) — for receivers whose two ADC channels do not sample at
 * the same instant: the image of every signal then mirrors around the
 * centre, coherent with it and the stronger the farther out (the CQWW 2012
 * recordings: +0.46 sample, image only 11 dB down at 35 kHz without it).
 *
 * Feeds a cf32 recording (float32 interleaved I/Q, true orientation — what
 * skimmer-tci-probe dumps) through the full engine pipeline offline:
 * channelizer → CW decoders → callsign extractors → station tracker. Rate and
 * centre default from the <file>.meta sidecar the probe writes. Decodes land
 * in <file>.decodes.log stamped with STREAM time, so two runs of different
 * decoder versions over the same sample diff line by line; the run ends with
 * a station table and summary counters for quick before/after comparison.
 *
 * SKIM_REPLAY_HOLDS="t0-t1,t0-t1,…" (stream seconds) replays the operator's
 * own transmissions: the TX flag is raised over each interval exactly as the
 * TCI trx broadcast would, so the pipeline's TX hold (and its grace) runs
 * as it did live. Take the intervals from the "TX hold" lines of the live
 * log of the same session. SKIM_REPLAY_MUTE (same format) zeroes the IQ
 * over its intervals, to the sample — what sdr-for-linux puts on the wire
 * from key-down on (gh#17), so a SYNTHETIC over can be laid anywhere into a
 * recording: mute from the key, raise the flag a poll later.
 * SKIM_REPLAY_FROM / SKIM_REPLAY_TO (stream seconds) replay a slice only;
 * hold and mute times stay absolute.
 *
 * SKIM_REPLAY_FEED=new|old wires an RBN telnet feed (ephemeral port, no
 * clients) in and traces every line it would put on the wire ("feed:" on
 * stderr): "new" = the default feed policy (two hearings or MASTER.SCP,
 * 8 s settle), "old" = the score gate alone, sent at once. After the file
 * ends the replay feeds 10 s of silence, so calls still held get to settle.
 *
 * SKIM_REPLAY_SPOTS=<file> measures against a reference skimmer that spots
 * S&P answers too: the feed (default policy unless SKIM_REPLAY_FEED says
 * otherwise) runs with its CQ requirement dropped, and every line it puts
 * out is written as "<UTC ISO> <kHz> <call> <dB> <wpm> CQ|-" — UTC from the
 * recording's start time (WAV auxi chunk or the .meta start_unix).
 *
 * SKIM_REPLAY_RTTY_PAIRS=1 (CW) runs an RTTY pipeline beside the CW one on
 * the same IQ, as skimmer-headless does on a "cw,rtty" band, and the CW
 * pipeline drops the decodes on the pairs it copies (rtty_pairs.h — the
 * [decode] suppress_cw_on_rtty switch). Its output is the CW pipeline's,
 * plus how many decodes were dropped.
 *
 * SKIM_REPLAY_TAP=<file> / SKIM_REPLAY_ROWS=<file> write the feed-gate
 * learning logs (a decode tap and the gate's rows, gatelog.h) — with
 * SKIM_REPLAY_FEED set the rows carry the feed's events too.
 *
 * The same MASTER.SCP the app uses (~/.config/skimmer-for-linux/master.scp)
 * is loaded when present — keep it that way for honest A/B against the app.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/pipeline.h"
#include "engine/rbn_feed.h"

#define BLK 2048                       /* frames per feed — the TCI block size */

static guint64 g_fragments;
static GHashTable *g_stations;         /* call → SkimStation* (last state)     */

static void text_cb(double freq_hz, const char *text, gpointer user) {
  (void)freq_hz; (void)text; (void)user;
  g_fragments++;
}

static void station_cb(const SkimStation *st, gpointer user) {
  (void)user;
  SkimStation *copy = g_hash_table_lookup(g_stations, st->call);
  if (!copy) {
    copy = g_new0(SkimStation, 1);
    g_hash_table_insert(g_stations, g_strdup(st->call), copy);
  }
  *copy = *st;
}

static void gone_cb(const SkimStation *st, gpointer user) {
  (void)user;
  g_hash_table_remove(g_stations, st->call);
}

/* Pull "key: value" doubles out of the probe's .meta sidecar. */
static double meta_get(const char *path, const char *key) {
  char *body = NULL;
  double v = 0;
  if (g_file_get_contents(path, &body, NULL, NULL)) {
    char *hit = strstr(body, key);
    if (hit) { v = g_ascii_strtod(hit + strlen(key), NULL); }
    g_free(body);
  }
  return v;
}

/* ---- input: cf32 or WAV (file or stdin) -------------------------------- */

#define SKEW_K 32               /* fractional-delay half length, samples  */

typedef struct {
  FILE    *f;
  gboolean wav;
  gboolean conj;
  guint64  left;          /* WAV: bytes left in the current data chunk       */
  double   rate, center;
  double   start_unix;    /* first sample, UTC seconds; 0 = unknown          */
  guint64  frames;        /* frames read so far                              */
  gint16   s16[BLK * 2];
  double   skew;          /* Q ahead of I, samples; 0 = no correction        */
  float    skew_h[2 * SKEW_K + 1];
  float    skew_q[2 * SKEW_K];   /* Q history across blocks               */
  float    skew_i[SKEW_K];       /* I history (the filter's delay)         */
} Src;

static guint16 le16(const guint8 *b) { return (guint16)(b[0] | b[1] << 8); }
static guint32 le32(const guint8 *b) {
  return (guint32)b[0] | (guint32)b[1] << 8 | (guint32)b[2] << 16 |
         (guint32)b[3] << 24;
}

/* Windows SYSTEMTIME (8 little-endian WORDs, UTC) → Unix seconds. */
static double systemtime_unix(const guint8 *b) {
  GDateTime *dt = g_date_time_new_utc(le16(b), le16(b + 2), le16(b + 6),
                                      le16(b + 8), le16(b + 10), le16(b + 12));
  if (!dt) { return 0; }
  const double t = (double)g_date_time_to_unix(dt) + le16(b + 14) / 1000.0;
  g_date_time_unref(dt);
  return t;
}

static gboolean skip_bytes(FILE *f, guint64 n) {
  guint8 tmp[4096];
  while (n) {
    const size_t k = fread(tmp, 1, MIN(n, sizeof(tmp)), f);
    if (!k) { return FALSE; }
    n -= k;
  }
  return TRUE;
}

/* Parse one RIFF/WAVE header up to its data chunk. magic = the 4 bytes of
 * "RIFF" already read (NULL = read them). FALSE at a clean EOF or a header
 * this reader does not take (message on stderr then). */
static gboolean wav_header(Src *s, const guint8 *magic) {
  guint8 h[12];
  if (magic) {
    memcpy(h, magic, 4);
    if (fread(h + 4, 1, 8, s->f) != 8) { return FALSE; }
  } else if (fread(h, 1, 12, s->f) != 12) {
    return FALSE;
  }
  if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    fprintf(stderr, "wav: not a RIFF/WAVE header\n");
    return FALSE;
  }
  double rate = 0, center = 0, start = 0;
  for (;;) {
    guint8 c[8];
    if (fread(c, 1, 8, s->f) != 8) { return FALSE; }
    const guint32 n = le32(c + 4);
    if (!memcmp(c, "data", 4)) {
      s->left = n;
      break;
    }
    guint8 *body = g_malloc(n + 1);
    if (fread(body, 1, n + (n & 1), s->f) != n + (n & 1)) {
      g_free(body);
      return FALSE;
    }
    if (!memcmp(c, "fmt ", 4) && n >= 16) {
      if (le16(body) != 1 || le16(body + 2) != 2 || le16(body + 14) != 16) {
        fprintf(stderr, "wav: need 16-bit PCM stereo I/Q (format %u, %u ch, "
                "%u bit)\n", le16(body), le16(body + 2), le16(body + 14));
        g_free(body);
        return FALSE;
      }
      rate = le32(body + 4);
    } else if (!memcmp(c, "auxi", 4) && n >= 36) {
      start  = systemtime_unix(body);
      center = le32(body + 32);
    }
    g_free(body);
  }
  if (rate <= 0) {
    fprintf(stderr, "wav: no fmt chunk before data\n");
    return FALSE;
  }
  if (!s->rate) {                      /* first file of the stream          */
    s->rate = rate;
    s->center = center;
    s->start_unix = start;
  } else {
    if (rate != s->rate || (center && fabs(center - s->center) > 0.5)) {
      fprintf(stderr, "wav: next file changes rate/centre (%.0f/%.0f)\n",
              rate, center);
      return FALSE;
    }
    const double expect = s->start_unix + (double)s->frames / s->rate;
    if (start && s->start_unix && fabs(start - expect) > 0.05) {
      fprintf(stderr, "wav: next file starts %+.3f s off the stream "
              "(at %.1f s) — not filled\n", start - expect,
              (double)s->frames / s->rate);
    }
  }
  return TRUE;
}

static double bessel_i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
  }
  return sum;
}

/* Kaiser-windowed sinc taps for Q(n + skew), unity DC gain. */
static void skew_init(Src *s, double skew) {
  s->skew = skew;
  const double beta = 8.0;
  double sum = 0;
  for (int k = -SKEW_K; k <= SKEW_K; k++) {
    const double x = k - skew;
    const double r = x / (SKEW_K + 1);
    const double w = r * r < 1 ? bessel_i0(beta * sqrt(1 - r * r)) / bessel_i0(beta) : 0;
    const double sn = fabs(x) < 1e-9 ? 1 : sin(G_PI * x) / (G_PI * x);
    s->skew_h[k + SKEW_K] = (float)(sn * w);
    sum += sn * w;
  }
  for (int k = 0; k <= 2 * SKEW_K; k++) { s->skew_h[k] /= (float)sum; }
}

/* Q resampled at n + skew; the stream comes out SKEW_K samples late. */
static void skew_apply(Src *s, float *buf, size_t n) {
  static float q[2 * SKEW_K + BLK], iv[SKEW_K + BLK];
  memcpy(q, s->skew_q, sizeof(s->skew_q));
  memcpy(iv, s->skew_i, sizeof(s->skew_i));
  for (size_t j = 0; j < n; j++) {
    q[2 * SKEW_K + j] = buf[2 * j + 1];
    iv[SKEW_K + j] = buf[2 * j];
  }
  for (size_t j = 0; j < n; j++) {
    float acc = 0;
    const float *qq = q + j;           /* q[K + j + k], k = -K..K           */
    for (int k = 0; k <= 2 * SKEW_K; k++) { acc += s->skew_h[k] * qq[k]; }
    buf[2 * j] = iv[j];
    buf[2 * j + 1] = acc;
  }
  memcpy(s->skew_q, q + n, sizeof(s->skew_q));
  memcpy(s->skew_i, iv + n, sizeof(s->skew_i));
}

static gboolean src_open(Src *s, const char *path) {
  memset(s, 0, sizeof(*s));
  const char *cj = g_getenv("SKIM_REPLAY_CONJ");
  s->conj = cj && cj[0] && strcmp(cj, "0");
  const char *sk = g_getenv("SKIM_REPLAY_IQ_SKEW");
  if (sk && sk[0]) { skew_init(s, g_ascii_strtod(sk, NULL)); }
  s->f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
  if (!s->f) { return FALSE; }
  guint8 m[4];
  if (fread(m, 1, 4, s->f) != 4) { return FALSE; }
  if (!memcmp(m, "RIFF", 4)) {
    s->wav = TRUE;
    return wav_header(s, m);
  }
  if (s->f == stdin) {
    fprintf(stderr, "stdin takes WAV only\n");
    return FALSE;
  }
  rewind(s->f);
  return TRUE;
}

/* Up to max frames as float I/Q; 0 at the end of the stream. */
static size_t src_read(Src *s, float *buf, size_t max) {
  size_t n;
  if (!s->wav) {
    n = fread(buf, 2 * sizeof(float), max, s->f);
  } else {
    while (s->left < 4) {              /* next concatenated file, or EOF    */
      if (s->left && !skip_bytes(s->f, s->left)) { return 0; }
      s->left = 0;
      guint8 m[4];
      if (fread(m, 1, 4, s->f) != 4 || !wav_header(s, m)) { return 0; }
    }
    n = fread(s->s16, 4, MIN(max, s->left / 4), s->f);
    s->left -= n * 4;
    for (size_t i = 0; i < 2 * n; i++) { buf[i] = s->s16[i] / 32768.0f; }
  }
  if (s->skew != 0) { skew_apply(s, buf, n); }
  if (s->conj) {
    for (size_t i = 0; i < n; i++) { buf[2 * i + 1] = -buf[2 * i + 1]; }
  }
  s->frames += n;
  return n;
}

/* Skip to frame `to` (a seek for cf32, read-and-drop for WAV/stdin). */
static void src_skip(Src *s, guint64 to) {
  if (!s->wav) {
    fseeko(s->f, (off_t)to * 2 * (off_t)sizeof(float), SEEK_SET);
    s->frames = to;
    return;
  }
  static float junk[BLK * 2];
  while (s->frames < to && src_read(s, junk, MIN(BLK, to - s->frames)) > 0) {}
}

/* ---- SKIM_REPLAY_SPOTS --------------------------------------------------- */

static FILE  *g_spots;
static double g_utc0;                  /* UTC of stream frame 0             */
static const Src *g_src;

static void feed_cb(const SkimStation *st, gpointer user) {
  (void)user;
  const double t = g_utc0 + (double)g_src->frames / g_src->rate;
  GDateTime *dt = g_date_time_new_from_unix_utc((gint64)t);
  char *ts = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%SZ");
  fprintf(g_spots, "%s %.1f %s %.0f %.0f %s\n", ts, st->freq_hz / 1000.0,
          st->call, st->snr_db, st->speed, st->cq ? "CQ" : "-");
  g_free(ts);
  g_date_time_unref(dt);
}

/* SKIM_REPLAY_HOLDS → [t0, t1) pairs in stream seconds. */
static GArray *holds_parse(const char *spec) {
  GArray *a = g_array_new(FALSE, FALSE, sizeof(double));
  if (!spec || !spec[0]) { return a; }
  char **parts = g_strsplit(spec, ",", -1);
  for (char **q = parts; *q; q++) {
    char *dash = strchr(*q, '-');
    if (!dash) { continue; }
    const double t0 = g_ascii_strtod(*q, NULL);
    const double t1 = g_ascii_strtod(dash + 1, NULL);
    if (t1 > t0) { g_array_append_val(a, t0); g_array_append_val(a, t1); }
  }
  g_strfreev(parts);
  return a;
}

static gboolean holds_tx_at(const GArray *a, double t) {
  for (guint i = 0; i + 1 < a->len; i += 2) {
    if (t >= g_array_index(a, double, i) && t < g_array_index(a, double, i + 1)) {
      return TRUE;
    }
  }
  return FALSE;
}

static int by_freq(gconstpointer a, gconstpointer b) {
  const SkimStation *sa = *(const SkimStation *const *)a;
  const SkimStation *sb = *(const SkimStation *const *)b;
  return (sa->freq_hz > sb->freq_hz) - (sa->freq_hz < sb->freq_hz);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-replay <file.cf32 | file.wav | -> "
            "[rate_hz] [center_hz]\n");
    return 2;
  }
  const char *path = argv[1];

  static Src src;
  if (!src_open(&src, path)) {
    fprintf(stderr, "cannot open %s\n", path);
    return 2;
  }
  g_src = &src;
  char *meta = g_strdup_printf("%s.meta", path);
  double rate   = argc > 2 ? g_ascii_strtod(argv[2], NULL)
                : src.wav  ? src.rate : meta_get(meta, "rate_hz:");
  double center = argc > 3 ? g_ascii_strtod(argv[3], NULL)
                : src.wav  ? src.center : meta_get(meta, "center_hz:");
  if (!src.wav) {
    src.rate = rate;
    src.start_unix = meta_get(meta, "start_unix:");
  }
  g_free(meta);
  if (rate <= 0 || center <= 0) {
    fprintf(stderr, "no rate/centre — need the .meta sidecar, a WAV auxi "
            "chunk or CLI args\n");
    return 2;
  }

  char *dict = g_build_filename(g_get_user_config_dir(), "skimmer-for-linux",
                                "master.scp", NULL);
  /* stdin: the log goes beside SKIM_REPLAY_SPOTS, so parallel runs part */
  const char *spots_path = g_getenv("SKIM_REPLAY_SPOTS");
  char *dlog = g_strdup_printf("%s.decodes.log",
                               strcmp(path, "-") ? path
                               : spots_path && spots_path[0] ? spots_path
                                                             : "stdin");
  remove(dlog);                        /* fresh log — runs must diff cleanly  */
  /* SKIM_MODE=rtty replays through the RTTY backend + wide bank (the same
   * config path the app's Mode preference takes); default CW. */
  const char *menv = g_getenv("SKIM_MODE");
  const gboolean rtty = menv && g_ascii_strcasecmp(menv, "rtty") == 0;
  SkimPipelineConfig cfg = {
    .host = NULL,
    .port = 0,
    .iq_rate = 0,
    .mode = rtty ? SKIM_PIPELINE_MODE_RTTY : SKIM_PIPELINE_MODE_CW,
    .chan_bw_hz = 0,                   /* mode default: 125 Hz CW, 250 RTTY  */
    .dict_path = g_file_test(dict, G_FILE_TEST_EXISTS) ? dict : NULL,
    .decode_log_path = dlog,
    /* SKIM_REPLAY_TAP / SKIM_REPLAY_ROWS: the feed-gate learning logs
     * (gatelog.h) of this replay — a decode tap for skimmer-tap-replay and
     * the gate's rows, as skimmer-headless writes them with [feed] learn. */
    .tap_path = g_getenv("SKIM_REPLAY_TAP"),
    .gatelog_path = g_getenv("SKIM_REPLAY_ROWS"),
  };
  const char *fenv = g_getenv("SKIM_REPLAY_FEED");
  const char *senv = g_getenv("SKIM_REPLAY_SPOTS");
  if (senv && senv[0]) {
    g_spots = fopen(senv, "w");
    if (!g_spots) {
      fprintf(stderr, "cannot write %s\n", senv);
      return 2;
    }
    if (!fenv || !fenv[0]) { fenv = "new"; }
    cfg.rbn_any_caller = TRUE;
  }
  SkimRbnFeed *feed = NULL;
  if (fenv && fenv[0]) {
    GError *ferr = NULL;
    feed = skim_rbn_feed_new("REPLAY", 0, &ferr);
    if (!feed) {
      fprintf(stderr, "feed: %s\n", ferr ? ferr->message : "?");
      g_clear_error(&ferr);
      return 1;
    }
    cfg.rbn = feed;
    if (g_ascii_strcasecmp(fenv, "old") == 0) {
      cfg.rbn_min_hearings = 1;
      cfg.rbn_settle_s = -1;
    }
    g_setenv("SKIM_FEED_TRACE", "1", FALSE);
  }
  /* SKIM_CW_ENGINE=v1|v2|deepcw picks the CW engine (the app's "CW engine"
   * preference takes the same config field); the pipeline resolves the
   * DeepCW availability itself and falls back to v2 with a warning. A
   * replay runs DeepCW inference INLINE (deterministic, stream order) —
   * the app uses the async workers. */
  g_setenv("SKIM_DEEPCW_SYNC", "1", FALSE);
  SkimPipeline *rp = NULL;                 /* SKIM_REPLAY_RTTY_PAIRS: RTTY beside */
  const char *penv = g_getenv("SKIM_REPLAY_RTTY_PAIRS");
  if (!rtty && penv && penv[0] && strcmp(penv, "0") != 0) {
    cfg.rtty_pairs = skim_rtty_pairs_new();
    const SkimPipelineConfig rc = { .mode = SKIM_PIPELINE_MODE_RTTY,
                                    .rtty_pairs = cfg.rtty_pairs };
    rp = skim_pipeline_new(&rc);
    GError *rerr = NULL;
    if (!skim_pipeline_start_offline(rp, &rerr)) {
      fprintf(stderr, "FAIL — RTTY beside: %s\n", rerr ? rerr->message : "?");
      return 1;
    }
  }
  SkimPipeline *p = skim_pipeline_new(&cfg);
  printf("=== skimmer-replay %s — %.0f Hz, centre %.0f Hz, %s%s, engine %s, "
         "dict %s ===\n",
         path, rate, center, rtty ? "RTTY" : "CW",
         rp ? " (off the RTTY pairs an RTTY pipeline copies)" : "",
         skim_pipeline_cw_engine_name(p), cfg.dict_path ? "yes" : "NO");
  g_free(dict);
  g_stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  skim_pipeline_set_text_cb(p, text_cb, NULL);
  skim_pipeline_set_station_cb(p, station_cb, NULL);
  skim_pipeline_set_station_gone_cb(p, gone_cb, NULL);
  if (g_spots) { skim_pipeline_set_feed_cb(p, feed_cb, NULL); }

  GError *err = NULL;
  if (!skim_pipeline_start_offline(p, &err)) {
    fprintf(stderr, "FAIL — %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return 1;
  }

  GArray *holds = holds_parse(g_getenv("SKIM_REPLAY_HOLDS"));
  GArray *mutes = holds_parse(g_getenv("SKIM_REPLAY_MUTE"));
  if (holds->len) { printf("TX holds replayed: %u\n", holds->len / 2); }
  if (mutes->len) { printf("muted intervals: %u\n", mutes->len / 2); }

  gint64 t0 = g_get_monotonic_time();
  static float buf[BLK * 2];
  guint64 frames = 0;
  size_t n;
  {
    const char *from = g_getenv("SKIM_REPLAY_FROM");
    if (from && from[0]) {
      frames = (guint64)(g_ascii_strtod(from, NULL) * rate) / BLK * BLK;
      src_skip(&src, frames);
      frames = src.frames;
    }
  }
  g_utc0 = src.start_unix;
  if (src.start_unix) {                /* tap/rows wall clock = real UTC     */
    skim_pipeline_set_wall_origin(
        p, (gint64)((src.start_unix + (double)frames / rate) * G_USEC_PER_SEC));
  }
  if (src.start_unix) {
    GDateTime *dt = g_date_time_new_from_unix_utc((gint64)src.start_unix);
    char *ts = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
    printf("recording starts %s.%03d UTC\n", ts,
           (int)(fmod(src.start_unix, 1.0) * 1000 + 0.5));
    g_free(ts);
    g_date_time_unref(dt);
  }
  const char *to_env = g_getenv("SKIM_REPLAY_TO");
  const guint64 frames_to = to_env && to_env[0]
      ? (guint64)(g_ascii_strtod(to_env, NULL) * rate) : G_MAXUINT64;
  const guint64 frames_from = frames;
  while (frames < frames_to && (n = src_read(&src, buf, BLK)) > 0) {
    if (holds->len) {
      skim_pipeline_set_tx_hold(p, holds_tx_at(holds, (double)frames / rate));
    }
    for (guint m = 0; m + 1 < mutes->len; m += 2) {
      const double a = g_array_index(mutes, double, m) * rate - (double)frames;
      const double z = g_array_index(mutes, double, m + 1) * rate - (double)frames;
      if (z <= 0 || a >= (double)n) { continue; }
      const gsize i0 = a > 0 ? (gsize)a : 0, i1 = z < (double)n ? (gsize)z : n;
      memset(buf + 2 * i0, 0, (i1 - i0) * 2 * sizeof(float));
    }
    if (rp) { skim_pipeline_feed(rp, buf, (guint)n, rate, center); }
    skim_pipeline_feed(p, buf, (guint)n, rate, center);
    frames += n;
  }
  if (src.f != stdin) { fclose(src.f); }
  if (feed) {                          /* let the held calls settle          */
    memset(buf, 0, sizeof(buf));
    for (guint64 q = 0; q < (guint64)(10 * rate); q += BLK) {
      if (rp) { skim_pipeline_feed(rp, buf, BLK, rate, center); }
      skim_pipeline_feed(p, buf, BLK, rate, center);
    }
  }
  g_array_free(holds, TRUE);
  g_array_free(mutes, TRUE);
  double wall   = (double)(g_get_monotonic_time() - t0) / G_USEC_PER_SEC;
  double stream = (double)(frames - frames_from) / rate;

  skim_pipeline_stop(p);

  /* Station table, sorted by frequency — the band as the decoder saw it. */
  GPtrArray *rows = g_ptr_array_new();
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, g_stations);
  while (g_hash_table_iter_next(&it, &key, &val)) { g_ptr_array_add(rows, val); }
  g_ptr_array_sort(rows, by_freq);
  printf("\n%-12s %10s %5s %5s %7s %6s %3s\n",
         "call", "kHz", rtty ? "bd" : "wpm", "dB", "reports", "score", "cq");
  for (guint i = 0; i < rows->len; i++) {
    const SkimStation *st = g_ptr_array_index(rows, i);
    char khz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(khz, sizeof(khz), "%.2f", st->freq_hz / 1000.0);
    printf("%-12s %10s %5.0f %5.0f %7u %6.2f %3s\n", st->call, khz, st->speed,
           st->snr_db, st->reports, st->score, st->cq ? "CQ" : "");
  }

  printf("\nsummary: %.1f s stream in %.1f s wall (%.1fx), %" G_GUINT64_FORMAT
         " frames, %" G_GUINT64_FORMAT " fragments, %u stations\n",
         stream, wall, stream / MAX(wall, 0.001), frames, g_fragments,
         rows->len);
  printf("decode log: %s\n", dlog);
  if (rp) {
    printf("on RTTY pairs: %" G_GUINT64_FORMAT " CW decodes dropped\n",
           skim_pipeline_rtty_suppressed(p));
  }
  if (feed) {
    double ms, ss, fs;
    guint mh;
    skim_pipeline_rbn_policy(p, &ms, &mh, &ss, &fs);
    printf("feed policy: score >= %.2f, %u hearings, %.0f s settle, %.0f s "
           "fresh — %" G_GUINT64_FORMAT " lines\n", ms, mh, ss, fs,
           skim_pipeline_rbn_spots(p));
  }

  g_ptr_array_free(rows, TRUE);
  g_hash_table_destroy(g_stations);
  skim_pipeline_free(p);
  if (rp) {
    skim_pipeline_stop(rp);
    skim_pipeline_free(rp);
    skim_rtty_pairs_unref(cfg.rtty_pairs);
  }
  if (feed) { skim_rbn_feed_free(feed); }
  if (g_spots) { fclose(g_spots); }
  g_free(dlog);
  return 0;
}
