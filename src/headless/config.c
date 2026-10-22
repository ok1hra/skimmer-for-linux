/* config.c — see config.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "config.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

/* The default config. Its %s is the radios a broadcast discovery found when
 * it was written (comment lines, or "" — skim_hd_config_write_default). */
static const char DEFAULT_CONFIG[] =
  "# skimmer-headless — a Red Pitaya (sdr_receiver_hpsdr): one receiver per band,\n"
  "# every band decoded in CW and RTTY, the spots on a telnet feed.\n"
  "# Status and these settings on the web page: http://<this machine>:8073/\n"
  "# 8 bands x 96 kHz is ~40 Mb/s of UDP from the radio: use a wired link.\n"
  "\n[radio]\n# hpsdr-p1: a Red Pitaya running sdr_receiver_hpsdr\ntype=hpsdr-p1\n"
  "# host=auto takes the first idle Red Pitaya on this LAN. To pin one, give its\n"
  "# address — `skimmer-headless --find-radios` lists the radios on the LAN.\n"
  "# A list is tried in order, auto may end it: host=192.168.1.21,auto\n"
  "%s"
  "host=auto\nrate=96000\n"
  "# Clock correction. A Red Pitaya's clock is off by a few ppm, and every spot\n"
  "# then lands f x ppm low (3.8 ppm = 53 Hz on 14 MHz; the RBN wants < 100 Hz).\n"
  "# Measure it with 0 here: compare your spots with the Reverse Beacon\n"
  "# Network's spots of the same stations (www.reversebeacon.net). Yours D Hz\n"
  "# low at F kHz on average: clock_ppm = D / F * 1000 (53 Hz at 14045 kHz =\n"
  "# 3.77). Or a known carrier: (true - shown) / true * 1e6. clock_ppm is for\n"
  "# every radio; host=ADDRESS@ppm gives one radio its own.\n"
  "clock_ppm=0\n"
  "\n[bands]\n"
  "# name=centre Hz [modes], in receiver order (RX1 first). At 96 kHz a band\n"
  "# spans its centre +-48 kHz. modes: cw, rtty (cw:deepcw = the neural CW\n"
  "# decoder). At most 8 — the Red Pitaya has 8 receivers: to have 12 m,\n"
  "# put the # in front of another band.\n"
  "160m=1845500 cw,rtty\n80m=3545500 cw,rtty\n40m=7045500 cw,rtty\n"
  "30m=10125000 cw,rtty\n20m=14045500 cw,rtty\n17m=18113500 cw,rtty\n"
  "15m=21045500 cw,rtty\n10m=28045500 cw,rtty\n#12m=24915500 cw,rtty\n"
  "\n[decode]\n# v2 (classical) or deepcw (needs ONNX Runtime + the model:\n"
  "# skimmer-headless --fetch-deepcw-model); without them a band runs v2\n"
  "engine=v2\n# per-band raw decode logs in ~/.local/share/skimmer-for-linux/headless\n"
  "log=false\n"
  "# on a band that decodes RTTY too, keep its CW decoders off the RTTY stations\n"
  "# it copies (an FSK tone reads as junk CW); off until measured live\n"
  "#suppress_cw_on_rtty=false\n"
  "\n[feed]\n# telnet spot feed (validated CQ spots only); port 0 = off.\n"
  "# call = YOUR callsign: the feed stays OFF until it is set.\n"
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
  "# the bands that decode RTTY spot it here too (\"45 BPS\"); false keeps the\n"
  "# feed CW only and the RTTY spots on the web page\n"
  "#rtty=true\n"
  "# the learned feed gate (skimmer-compare gate/learn.py) is built in; gate=FILE\n"
  "# runs another model (it must name every feature). gate_mode: shadow scores\n"
  "# every spot and only reports, on holds back the ones below its threshold,\n"
  "# off drops it\n"
  "#gate=feed-gate.ini\n#gate_mode=shadow\n"
  "\n[status]\n# console table every N s (0 = off); web page port (0 = off)\n"
  "console_s=10\nhttp_port=8073\n"
  "\n[web]\n# who may change the settings from the web page (addresses or\n"
  "# networks, comma separated); everyone else sees them read-only\n"
  "settings_from=127.0.0.1,::1\n"
  "\n[record]\n# raw IQ of these bands (cf32 + .meta for skimmer-replay) while\n"
  "# decoding; ~0.8 MB/s per band at 96 kHz; start_utc=HH:MM or empty = at once\n"
  "bands=\nminutes=10\ndir=/var/tmp/skimmer-iq\nstart_utc=\n";

gboolean skim_hd_config_write_default(const char *path, const char *radios, GError **error) {
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  char *text = g_strdup_printf(DEFAULT_CONFIG, radios ? radios : "");
  const gboolean ok = g_file_set_contents(path, text, -1, error);
  g_free(text);
  return ok;
}

void skim_hd_config_clear(SkimHdConfig *cfg) {
  g_free(cfg->path);
  g_strfreev(cfg->hosts);
  g_free(cfg->host_ppm);
  g_free(cfg->hosts_text);
  g_free(cfg->feed_call);
  g_free(cfg->feed_gate);
  g_free(cfg->feed_gate_mode);
  g_strfreev(cfg->settings_from);
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

/* A boolean key: dflt when absent, an error when it is no boolean. */
static gboolean opt_bool(GKeyFile *kf, const char *group, const char *key, gboolean dflt,
                         gboolean *out, const char *path, GError **error) {
  *out = dflt;
  if (!g_key_file_has_key(kf, group, key, NULL)) { return TRUE; }
  GError *e = NULL;
  *out = g_key_file_get_boolean(kf, group, key, &e);
  if (e) {
    g_clear_error(&e);
    return bad(error, path, "[%s] %s: true or false", group, key);
  }
  return TRUE;
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
  /* host[@ppm], …: each radio has its own sampling clock; "auto" = every
   * radio a broadcast discovery finds (none given = auto) */
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
    if (g_ascii_strcasecmp(*q, SKIM_HD_HOST_AUTO) == 0) {
      if (at) {
        ok = bad(error, path, "[radio] host \"auto@…\": auto takes clock_ppm; "
                 "give a radio its own as ADDRESS@ppm");
        break;
      }
      g_ptr_array_add(hl, g_strdup(SKIM_HD_HOST_AUTO));
    } else {
      g_ptr_array_add(hl, g_strdup(*q));
    }
    g_array_append_val(pl, ppm);
  }
  g_strfreev(parts);
  g_free(hv);
  if (ok && hl->len == 0) {
    g_ptr_array_add(hl, g_strdup(SKIM_HD_HOST_AUTO));
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
  if (!opt_bool(kf, "decode", "suppress_cw_on_rtty", FALSE, &h->suppress_cw_on_rtty, path,
                error)) {
    return FALSE;
  }

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
  if (!opt_bool(kf, "feed", "rtty", TRUE, &h->feed_rtty, path, error)) { return FALSE; }
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

  char *sf = g_key_file_get_string(kf, "web", "settings_from", NULL);
  GPtrArray *nets = g_ptr_array_new();
  char **sp = g_strsplit_set(sf ? sf : "", ", \t", -1);
  for (char **q = sp; *q && ok; q++) {
    if (!**q) { continue; }
    GInetAddressMask *m = g_inet_address_mask_new_from_string(*q, NULL);
    if (!m) {
      ok = bad(error, path, "[web] settings_from \"%s\" (an address or address/bits)", *q);
    } else {
      g_object_unref(m);
      g_ptr_array_add(nets, g_strdup(*q));
    }
  }
  g_strfreev(sp);
  g_free(sf);
  if (!nets->len) {
    g_ptr_array_add(nets, g_strdup("127.0.0.1"));
    g_ptr_array_add(nets, g_strdup("::1"));
  }
  g_ptr_array_add(nets, NULL);
  h->settings_from = (char **)g_ptr_array_free(nets, FALSE);
  if (!ok) { return FALSE; }

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

/* ---- editing ------------------------------------------------------------------- */

const char *const SKIM_HD_EDITABLE[] = {
  "radio.type", "radio.host", "radio.rate", "radio.clock_ppm",
  "bands",
  "decode.engine", "decode.log", "decode.suppress_cw_on_rtty",
  "feed.call", "feed.port", "feed.rtty",
  "status.console_s", "status.http_port",
  "record.bands", "record.minutes", "record.dir", "record.start_utc",
  NULL,
};

static gboolean editable(const char *key) {
  for (const char *const *k = SKIM_HD_EDITABLE; *k; k++) {
    if (strcmp(*k, key) == 0) { return TRUE; }
  }
  return FALSE;
}

char *skim_hd_config_apply_form(const char *text, gsize len, GHashTable *form,
                                GError **error) {
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_data(kf, text, len,
                                 G_KEY_FILE_KEEP_COMMENTS, error)) {
    g_key_file_free(kf);
    return NULL;
  }
  GHashTableIter it;
  gpointer kp, vp;
  gboolean ok = TRUE;
  g_hash_table_iter_init(&it, form);
  while (ok && g_hash_table_iter_next(&it, &kp, &vp)) {
    const char *key = kp, *val = vp;
    if (!editable(key)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "\"%s\" is not a setting the page may change", key);
      ok = FALSE;
    } else if (strcmp(key, "bands") == 0) {
      /* the whole group, in receiver order */
      gsize n = 0;
      char **old = g_key_file_get_keys(kf, "bands", &n, NULL);
      for (gsize i = 0; old && i < n; i++) { g_key_file_remove_key(kf, "bands", old[i], NULL); }
      g_strfreev(old);
      char **lines = g_strsplit_set(val, "\r\n", -1);
      for (char **l = lines; *l && ok; l++) {
        char *line = g_strstrip(*l);
        if (!*line) { continue; }
        char *eq = strchr(line, '=');
        if (!eq || eq == line) {
          g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      "band \"%s\": name=centre Hz [modes]", line);
          ok = FALSE;
          break;
        }
        *eq = '\0';
        g_key_file_set_value(kf, "bands", g_strstrip(line), g_strstrip(eq + 1));
      }
      g_strfreev(lines);
    } else if (strpbrk(val, "\r\n")) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                  "%s: one line only", key);
      ok = FALSE;
    } else {
      char *group = g_strndup(key, (gsize)(strchr(key, '.') - key));
      g_key_file_set_value(kf, group, strchr(key, '.') + 1, val);
      g_free(group);
    }
  }
  char *out = ok ? g_key_file_to_data(kf, NULL, NULL) : NULL;
  g_key_file_free(kf);
  return out;
}

static gint newest_first(gconstpointer a, gconstpointer b) {
  return strcmp(*(const char *const *)b, *(const char *const *)a);
}

char **skim_hd_config_backups(const char *path) {
  char *dir = g_path_get_dirname(path), *base = g_path_get_basename(path);
  char *prefix = g_strconcat(base, ".bak-", NULL);
  GPtrArray *list = g_ptr_array_new();
  GDir *d = g_dir_open(dir, 0, NULL);
  for (const char *e; d && (e = g_dir_read_name(d));) {
    if (g_str_has_prefix(e, prefix)) { g_ptr_array_add(list, g_strdup(e)); }
  }
  if (d) { g_dir_close(d); }
  g_ptr_array_sort(list, newest_first);      /* the UTC stamp sorts as text */
  g_ptr_array_add(list, NULL);
  g_free(prefix);
  g_free(base);
  g_free(dir);
  return (char **)g_ptr_array_free(list, FALSE);
}

/* Copy the current file to <path>.bak-<UTC>; drop the oldest past the cap. */
static char *backup_now(const char *path, GError **error) {
  char *cur = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &cur, &len, error)) { return NULL; }
  GDateTime *t = g_date_time_new_now_utc();
  char *stamp = g_date_time_format(t, "%Y%m%d-%H%M%SZ");
  g_date_time_unref(t);
  char *bak = g_strdup_printf("%s.bak-%s", path, stamp);
  for (guint k = 2; g_file_test(bak, G_FILE_TEST_EXISTS); k++) {   /* same second */
    g_free(bak);
    bak = g_strdup_printf("%s.bak-%s-%u", path, stamp, k);
  }
  g_free(stamp);
  const gboolean ok = g_file_set_contents(bak, cur, (gssize)len, error);
  g_free(cur);
  if (!ok) {
    g_free(bak);
    return NULL;
  }
  char **all = skim_hd_config_backups(path);
  char *dir = g_path_get_dirname(path);
  const guint nall = g_strv_length(all);
  for (guint i = SKIM_HD_BACKUPS; i < nall; i++) {
    char *old = g_build_filename(dir, all[i], NULL);
    g_remove(old);
    g_free(old);
  }
  g_free(dir);
  g_strfreev(all);
  char *name = g_path_get_basename(bak);
  g_free(bak);
  return name;
}

gboolean skim_hd_config_save(const char *path, const char *text, char **backup,
                             GError **error) {
  char *bak = backup_now(path, error);
  if (!bak) { return FALSE; }
  if (!g_file_set_contents(path, text, -1, error)) {          /* atomic rename */
    g_free(bak);
    return FALSE;
  }
  if (backup) { *backup = bak; } else { g_free(bak); }
  return TRUE;
}

gboolean skim_hd_config_restore(const char *path, const char *backup, GError **error) {
  char *base = g_path_get_basename(path);
  char *prefix = g_strconcat(base, ".bak-", NULL);
  const gboolean named_ok = g_str_has_prefix(backup, prefix) && !strchr(backup, '/');
  g_free(prefix);
  g_free(base);
  if (!named_ok) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "\"%s\" is no backup of %s",
                backup, path);
    return FALSE;
  }
  char *dir = g_path_get_dirname(path);
  char *src = g_build_filename(dir, backup, NULL);
  g_free(dir);
  char *text = NULL;
  gboolean ok = g_file_get_contents(src, &text, NULL, error);
  g_free(src);
  if (ok) { ok = skim_hd_config_save(path, text, NULL, error); }
  g_free(text);
  return ok;
}
