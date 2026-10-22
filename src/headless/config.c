/* config.c — see config.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "config.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static const char DEFAULT_CONFIG[] =
  "# skimmer-headless — one Red Pitaya, one pipeline per band.\n"
  "# Bands are receivers in order (RX1 first), at most 8, all at one rate.\n"
  "# 7 x 96 kHz is ~35 Mb/s of UDP: use a wired link.\n"
  "\n[radio]\n# hpsdr-p1: a Red Pitaya running sdr_receiver_hpsdr\ntype=hpsdr-p1\n"
  "# one radio, or a list tried in order: the first idle one is used;\n"
  "# host@ppm gives that radio its own clock_ppm (192.168.1.21@3.81,192.168.1.71)\n"
  "host=192.168.1.21\nrate=96000\n"
  "# sampling-clock error: (true - shown) / true * 1e6, measured with 0 here;\n"
  "# for the radios without @ppm\n"
  "clock_ppm=0\n"
  "\n[bands]\n"
  "# name=centre Hz [modes]; with 96 kHz each band spans centre +-48 kHz;\n"
  "# modes: cw (default), cw:v2, cw:deepcw, rtty — e.g. 20m=14045500 cw,rtty\n"
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
  "#min_score=0.90\n#min_hearings=2\n#settle_s=8\n"
  "# a call MASTER.SCP does not know waits settle_nodict_s instead (a misread\n"
  "# of a nearby call is held back once the right one is read); 0 = settle_s\n"
  "#settle_nodict_s=60\n"
  "# fresh_s=N also wants the call READ within the last N s: a stale candidate\n"
  "# on a quiet channel is not a station (off unless set; 120 = the station TTL)\n"
  "#fresh_s=120\n"
  "# fold_s=N holds back a call alike to one spotted within N s and 300 Hz that\n"
  "# MASTER.SCP knows better or that was read more (a glued or cut twin: F6FXXK,\n"
  "# RK3D next to F6FXX, RK3DJW); off unless set\n"
  "#fold_s=600\n"
  "# learn=true records what the feed gate saw, for learning a better one: per\n"
  "# band a decode tap in ~/.local/share/skimmer-for-linux/headless/learn —\n"
  "# skimmer-tap-replay turns it into the gate's rows (~80 MB/day/band raw)\n"
  "#learn=false\n"
  "# the learned feed gate (skimmer-compare gate/learn.py) is built in; gate=FILE\n"
  "# runs another model (it must name every feature). gate_mode: shadow scores\n"
  "# every spot and only reports, on holds back the ones below its threshold,\n"
  "# off drops it\n"
  "#gate=feed-gate.ini\n#gate_mode=shadow\n"
  "\n[status]\n# console table every N s (0 = off); web page port (0 = off)\n"
  "console_s=10\nhttp_port=8073\n"
  "\n[record]\n# raw IQ of these bands (cf32 + .meta for skimmer-replay) while\n"
  "# decoding; ~0.8 MB/s per band at 96 kHz; start_utc=HH:MM or empty = at once\n"
  "bands=\nminutes=10\ndir=/var/tmp/skimmer-iq\nstart_utc=\n";

gboolean skim_hd_config_write_default(const char *path, GError **error) {
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  return g_file_set_contents(path, DEFAULT_CONFIG, -1, error);
}

void skim_hd_config_clear(SkimHdConfig *cfg) {
  g_free(cfg->path);
  g_strfreev(cfg->hosts);
  g_free(cfg->host_ppm);
  g_free(cfg->hosts_text);
  g_free(cfg->feed_call);
  g_free(cfg->feed_gate);
  g_free(cfg->feed_gate_mode);
  g_strfreev(cfg->rec_bands);
  g_free(cfg->rec_dir);
  memset(cfg, 0, sizeof(*cfg));
}

static gboolean bad(GError **error, const char *path, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static gboolean bad(GError **error, const char *path, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: %s", path, msg);
  g_free(msg);
  return FALSE;
}

static gboolean engine_of(const char *s, SkimCwEngine *out) {
  if (g_strcmp0(s, "v2") == 0) { *out = SKIM_CW_ENGINE_V2; return TRUE; }
  if (g_strcmp0(s, "deepcw") == 0) { *out = SKIM_CW_ENGINE_DEEPCW; return TRUE; }
  if (g_strcmp0(s, "v1") == 0) { *out = SKIM_CW_ENGINE_V1; return TRUE; }
  return FALSE;
}

static const char *engine_name(SkimCwEngine e) {
  return e == SKIM_CW_ENGINE_DEEPCW ? "deepcw" : e == SKIM_CW_ENGINE_V1 ? "v1" : "v2";
}

char *skim_hd_band_modes(const SkimHdBand *b, SkimCwEngine dflt) {
  GString *s = g_string_new(NULL);
  if (b->cw) {
    g_string_append(s, "cw");
    if (b->cw_engine != dflt) { g_string_append_printf(s, ":%s", engine_name(b->cw_engine)); }
  }
  if (b->rtty) { g_string_append_printf(s, "%srtty", s->len ? "," : ""); }
  return g_string_free(s, FALSE);
}

/* "<centre Hz> [mode,mode…]" */
static gboolean band_parse(SkimHdBand *b, const char *name, const char *value,
                           SkimCwEngine dflt, const char *path, GError **error) {
  g_strlcpy(b->name, name, sizeof(b->name));
  char *v = g_strstrip(g_strdup(value));
  char *end = NULL;
  b->centre_hz = g_ascii_strtod(v, &end);
  if (end == v || b->centre_hz < 100000 || b->centre_hz > 62000000 ||
      (*end && !g_ascii_isspace(*end))) {
    g_free(v);
    return bad(error, path, "band %s=%s: not a centre in Hz", name, value);
  }
  char *modes = g_strstrip(end);
  gboolean ok = TRUE;
  if (!*modes) {
    b->cw = TRUE;
    b->cw_engine = dflt;
  } else {
    char **m = g_strsplit(modes, ",", -1);
    for (char **q = m; *q && ok; q++) {
      char *t = g_strstrip(*q);
      if (g_strcmp0(t, "cw") == 0) {
        b->cw = TRUE;
        b->cw_engine = dflt;
      } else if (g_str_has_prefix(t, "cw:") && engine_of(t + 3, &b->cw_engine)) {
        b->cw = TRUE;
      } else if (g_strcmp0(t, "rtty") == 0) {
        b->rtty = TRUE;
      } else {
        ok = bad(error, path, "band %s: mode \"%s\" (cw, cw:v2, cw:deepcw, rtty)", name, t);
      }
    }
    g_strfreev(m);
  }
  g_free(v);
  return ok;
}

/* A positive value, or the file's 0 meaning "off" (-1); absent = 0 (the
 * pipeline's default). */
static double opt_secs(GKeyFile *kf, const char *key) {
  if (!g_key_file_has_key(kf, "feed", key, NULL)) { return 0; }
  const double v = g_key_file_get_double(kf, "feed", key, NULL);
  return v > 0 ? v : -1;
}

static gboolean parse(SkimHdConfig *h, GKeyFile *kf, GError **error) {
  const char *path = h->path;
  char *type = g_key_file_get_string(kf, "radio", "type", NULL);
  if (type) { g_strstrip(type); }
  if (!type || !type[0] || g_strcmp0(type, "hpsdr-p1") == 0) {
    h->source = SKIM_HD_SOURCE_HPSDR_P1;
  } else if (g_strcmp0(type, "mock") == 0) {
    h->source = SKIM_HD_SOURCE_MOCK;
  } else {
    bad(error, path, "[radio] type \"%s\" (hpsdr-p1 or mock)", type);
    g_free(type);
    return FALSE;
  }
  g_free(type);
  h->ppm = g_key_file_has_key(kf, "radio", "clock_ppm", NULL)
               ? g_key_file_get_double(kf, "radio", "clock_ppm", NULL) : 0;
  /* host[@ppm], …: each radio has its own sampling clock */
  char *hv = g_key_file_get_string(kf, "radio", "host", NULL);
  GPtrArray *hl = g_ptr_array_new_with_free_func(g_free);
  GArray *pl = g_array_new(FALSE, FALSE, sizeof(double));
  char **parts = g_strsplit_set(hv ? hv : "", ",; \t", -1);
  gboolean ok = TRUE;
  for (char **q = parts; *q && ok; q++) {
    if (!**q) { continue; }
    char *at = strchr(*q, '@');
    double ppm = h->ppm;
    if (at) {
      char *end = NULL;
      ppm = g_ascii_strtod(at + 1, &end);
      if (end == at + 1 || *end || at == *q) {
        ok = bad(error, path, "[radio] host \"%s\" (host or host@ppm)", *q);
        break;
      }
      *at = '\0';
    }
    g_ptr_array_add(hl, g_strdup(*q));
    g_array_append_val(pl, ppm);
  }
  g_strfreev(parts);
  g_free(hv);
  if (ok && hl->len == 0) {
    g_ptr_array_add(hl, g_strdup("192.168.1.21"));
    g_array_append_val(pl, h->ppm);
  }
  g_ptr_array_add(hl, NULL);
  g_ptr_array_set_free_func(hl, NULL);
  h->hosts = (char **)g_ptr_array_free(hl, FALSE);
  h->host_ppm = (double *)g_array_free(pl, FALSE);
  if (!ok) { return FALSE; }
  h->hosts_text = g_strjoinv(", ", h->hosts);
  h->rate = 96000;
  if (g_key_file_has_key(kf, "radio", "rate", NULL)) {
    h->rate = (guint)g_key_file_get_integer(kf, "radio", "rate", NULL);
  }
  if (h->rate != 48000 && h->rate != 96000 && h->rate != 192000) {
    return bad(error, path, "rate %u (48000, 96000 or 192000)", h->rate);
  }

  char *eng = g_key_file_get_string(kf, "decode", "engine", NULL);
  if (eng) { g_strstrip(eng); }
  h->engine = SKIM_CW_ENGINE_V2;
  if (eng && eng[0] && !engine_of(eng, &h->engine)) {
    bad(error, path, "[decode] engine \"%s\" (v2 or deepcw)", eng);
    g_free(eng);
    return FALSE;
  }
  g_free(eng);
  h->decode_log = g_key_file_has_key(kf, "decode", "log", NULL) &&
                  g_key_file_get_boolean(kf, "decode", "log", NULL);

  h->feed_call = g_key_file_get_string(kf, "feed", "call", NULL);
  if (h->feed_call) { g_strstrip(h->feed_call); }
  h->feed_port = g_key_file_has_key(kf, "feed", "port", NULL)
                     ? g_key_file_get_integer(kf, "feed", "port", NULL) : 7300;
  h->feed_min_score = g_key_file_has_key(kf, "feed", "min_score", NULL)
                          ? g_key_file_get_double(kf, "feed", "min_score", NULL) : 0;
  h->feed_min_hearings = g_key_file_has_key(kf, "feed", "min_hearings", NULL)
      ? (guint)MAX(1, g_key_file_get_integer(kf, "feed", "min_hearings", NULL)) : 0;
  h->feed_settle_s = opt_secs(kf, "settle_s");               /* 0 = off      */
  h->feed_settle_nodict_s = opt_secs(kf, "settle_nodict_s"); /* 0 = settle_s */
  h->feed_fresh_s = opt_secs(kf, "fresh_s");                 /* 0 = off      */
  h->feed_fold_s = opt_secs(kf, "fold_s");                   /* 0 = off      */
  h->feed_learn = g_key_file_has_key(kf, "feed", "learn", NULL) &&
                  g_key_file_get_boolean(kf, "feed", "learn", NULL);
  char *gate = g_key_file_get_string(kf, "feed", "gate", NULL);
  if (gate && g_strstrip(gate)[0]) {
    char *dir = g_path_get_dirname(path);          /* relative: next to the ini */
    char *x = gate[0] == '~' ? g_build_filename(g_get_home_dir(), gate + 1, NULL)
                             : g_strdup(gate);
    h->feed_gate = g_path_is_absolute(x) ? g_strdup(x) : g_build_filename(dir, x, NULL);
    g_free(x);
    g_free(dir);
  }
  g_free(gate);
  h->feed_gate_mode = g_key_file_get_string(kf, "feed", "gate_mode", NULL);
  if (h->feed_gate_mode) { g_strstrip(h->feed_gate_mode); }

  h->console_s = g_key_file_has_key(kf, "status", "console_s", NULL)
                     ? g_key_file_get_integer(kf, "status", "console_s", NULL) : -1;
  h->http_port = g_key_file_has_key(kf, "status", "http_port", NULL)
                     ? g_key_file_get_integer(kf, "status", "http_port", NULL) : -1;

  char *rb = g_key_file_get_string(kf, "record", "bands", NULL);
  if (rb && g_strstrip(rb)[0]) {
    h->rec_bands = g_strsplit(rb, ",", -1);
    for (char **x = h->rec_bands; *x; x++) { g_strstrip(*x); }
    h->rec_minutes = g_key_file_has_key(kf, "record", "minutes", NULL)
                         ? g_key_file_get_double(kf, "record", "minutes", NULL) : 10;
    if (h->rec_minutes <= 0) { h->rec_minutes = 10; }
    h->rec_dir = g_key_file_get_string(kf, "record", "dir", NULL);
    if (!h->rec_dir || !g_strstrip(h->rec_dir)[0]) {
      g_free(h->rec_dir);
      h->rec_dir = g_strdup("/var/tmp/skimmer-iq");
    }
    char *at = g_key_file_get_string(kf, "record", "start_utc", NULL);
    guint hh, mm;
    if (at && sscanf(g_strstrip(at), "%u:%u", &hh, &mm) == 2 && hh < 24 && mm < 60) {
      GDateTime *now = g_date_time_new_now_utc();
      GDateTime *t = g_date_time_new_utc(g_date_time_get_year(now),
                                         g_date_time_get_month(now),
                                         g_date_time_get_day_of_month(now), hh, mm, 0);
      if (g_date_time_compare(t, now) <= 0) {   /* today's is gone: tomorrow */
        GDateTime *t2 = g_date_time_add_days(t, 1);
        g_date_time_unref(t);
        t = t2;
      }
      h->rec_at_us = g_date_time_to_unix(t) * G_USEC_PER_SEC;
      g_date_time_unref(t);
      g_date_time_unref(now);
    }
    g_free(at);
  }
  g_free(rb);

  gsize n = 0;
  char **keys = g_key_file_get_keys(kf, "bands", &n, NULL);   /* file order */
  for (gsize i = 0; keys && i < n && ok; i++) {
    if (h->nb == SKIM_HD_MAX_BANDS) {
      ok = bad(error, path, "band %s: %d receivers at most", keys[i], SKIM_HD_MAX_BANDS);
      break;
    }
    char *v = g_key_file_get_value(kf, "bands", keys[i], NULL);
    ok = band_parse(&h->band[h->nb], keys[i], v, h->engine, path, error);
    g_free(v);
    for (guint k = 0; ok && k < h->nb; k++) {
      if (strcmp(h->band[k].name, keys[i]) == 0) {
        ok = bad(error, path, "band %s twice", keys[i]);
      }
    }
    if (ok) { h->nb++; }
  }
  g_strfreev(keys);
  if (ok && h->nb == 0) { ok = bad(error, path, "no [bands] entries"); }
  return ok;
}

gboolean skim_hd_config_parse(SkimHdConfig *cfg, const char *text, gsize len,
                              const char *path, GError **error) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->path = g_strdup(path);
  GKeyFile *kf = g_key_file_new();
  gboolean ok = g_key_file_load_from_data(kf, text, len, G_KEY_FILE_NONE, error);
  if (!ok) {
    g_prefix_error(error, "%s: ", path);
  } else {
    ok = parse(cfg, kf, error);
  }
  g_key_file_free(kf);
  if (!ok) { skim_hd_config_clear(cfg); }
  return ok;
}

gboolean skim_hd_config_load(SkimHdConfig *cfg, const char *path, GError **error) {
  char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &text, &len, error)) {
    memset(cfg, 0, sizeof(*cfg));
    return FALSE;
  }
  const gboolean ok = skim_hd_config_parse(cfg, text, len, path, error);
  g_free(text);
  return ok;
}
