/* config.c — see config.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "config.h"

#include "engine/combiner.h"
#include "headless/dxcc.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
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
  "# spans its centre +-48 kHz. modes: cw, rtty (cw:v2 = the v0.7 decoder,\n"
  "# cw:v4 = DeepCW v2 narrow, cw:deepcw = DeepCW v1); off keeps the band\n"
  "# here without receiving it. At most 8 on — the Red Pitaya has 8 receivers: to have\n"
  "# 12 m, take off from it and put it on another band.\n"
  "160m=1845500 cw,rtty\n80m=3545500 cw,rtty\n40m=7045500 cw,rtty\n"
  "30m=10125000 cw,rtty\n20m=14045500 cw,rtty\n17m=18113500 cw,rtty\n"
  "15m=21045500 cw,rtty\n10m=28045500 cw,rtty\n12m=24915500 cw,rtty,off\n"
  "\n[decode]\n# v3 (classical, weak-signal front end), v4 (DeepCW v2 narrow, neural, on\n"
  "# the CPU: 3-5 dB more sensitive, far more under fading, ~3x v3's CPU on a\n"
  "# keyed channel), v2 (classical, as in v0.7) or deepcw (DeepCW v1: needs\n"
  "# ONNX Runtime + the model: skimmer-headless --fetch-deepcw-model; without\n"
  "# them a band runs v3)\n"
  "engine=v3\n"
  "# v4's inference threads (0 = half the CPUs); v4_guard: when they cannot keep\n"
  "# up, the v4 band with the fewest spots is paused (one always stays)\n"
  "#v4_workers=0\n#v4_guard=true\n# per-band raw decode logs in ~/.local/share/skimmer-for-linux/headless\n"
  "log=false\n"
  "# on a band that decodes RTTY too, keep its CW decoders off the RTTY stations\n"
  "# it copies (an FSK tone reads as junk CW); off until measured live\n"
  "#suppress_cw_on_rtty=false\n"
  "# rtty_adaptive=true: the RTTY decoders run only while there is a contest —\n"
  "# on at each full hour (and at start); if all bands together spot no more\n"
  "# than rtty_adaptive_spots RTTY spots in the first rtty_adaptive_minutes,\n"
  "# they stay off until the next full hour\n"
  "#rtty_adaptive=false\n#rtty_adaptive_spots=100\n#rtty_adaptive_minutes=2\n"
  "\n[feed]\n# telnet spot feed (validated CQ spots only); port 0 = off.\n"
  "# call = YOUR callsign: the feed stays OFF until it is set.\n"
  "call=\nport=7300\n"
  "# address = how the world reaches the feed (host:port or a telnet:// URL),\n"
  "# shown in the web page's header; empty = not shown\n"
  "address=\n"
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
  "\n[rbn]\n# the Reverse Beacon Network: the RBN Aggregator (www.reversebeacon.net)\n"
  "# connects to this port and relays the spots to the network. They are the\n"
  "# feed's spots under its call, each held delay_s seconds (0-99) first.\n"
  "enabled=false\nport=7550\ndelay_s=0\n"
  "\n[station]\n# your Maidenhead locator: the centre of the web page's map, the\n"
  "# distances and bearings of the spots\nlocator=\n"
  "\n[hub]\n# this skimmer on the LAN (default: the host name). Other PCs can decode\n"
  "# bands of this radio: [radio] type=hub on them, and @THEIRNAME after a band\n"
  "# here (20m=14045500 cw,rtty @pc2) — this one receives it and passes the IQ on\n"
  "# (~4.6 Mb/s a band at 96 kHz). allow = who may connect as a worker\n"
  "#name=\nport=7380\n#allow=10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8,::1\n"
  "\n[peers]\n# other skimmers' telnet feeds (host or host:port, comma separated):\n"
  "# their CQ spots go out on this feed under this call, duplicates dropped\n"
  "hosts=\n"
  "\n[status]\n# console table every N s (0 = off); web page port (0 = off)\n"
  "console_s=10\nhttp_port=8073\n"
  "\n[web]\n# who may change the settings from the web page (addresses or\n"
  "# networks, comma separated); everyone else sees them read-only\n"
  "settings_from=127.0.0.1,::1\n"
  "\n[visitors]\n# the web page's visitors from outside free_from (and everyone through a\n"
  "# proxy) sign in with their callsign before they see anything; a log of\n"
  "# the visits (call, address, how long, the frequencies watched on the\n"
  "# waterfall) in ~/.local/state/skimmer-for-linux/visitors.tsv, the refused\n"
  "# sign-ins in visitors-failed.tsv. free_from empty = the private networks\n"
  "login=true\nfree_from=\n"
  "\n[thermal]\n# the thermal guard: while the hottest CPU sensor, averaged over\n"
  "# average_min minutes, is over limit_c now and for 3 of the last 5 minutes\n"
  "# (a dip under it does not start the count again), the band with the\n"
  "# fewest spots in the last 30 minutes is paused (CW and RTTY); 10 minutes\n"
  "# later the next, if it is still hot (one band always stays). A band comes\n"
  "# back once the mean has stayed under limit_c by what pausing it saved (5 C\n"
  "# at least) plus restore_margin_c for 20 minutes. limit_c empty = the\n"
  "# sensor's critical temperature - 15 C (85 C on most Intel CPUs), else 80.\n"
  "# A band paused probe_every_min minutes is tested: on for probe_min minutes\n"
  "# in place of the band with the fewest spots; with more spots a minute the\n"
  "# two change places, else each goes back (probe_every_min=0: never)\n"
  "enabled=false\nlimit_c=\n#average_min=5\n#restore_margin_c=3\n#probe_every_min=30\n#probe_min=5\n"
  "\n[archive]\n# every spot the feed sends, kept in this directory: a gzip file a UTC\n"
  "# day, YYYYMMDD-spots.gz (zcat reads it), a tab-separated line a spot;\n"
  "# empty = off\ndir=\n"
  "\n[filter]\n# only the calls that match go anywhere — the feed, the RBN, the web page,\n"
  "# the statistics (empty = all pass): part of a call (any case), or with\n"
  "# regex=true a regular expression (any case), e.g. ^(OK|OL)|/P$\n"
  "call=\nregex=false\n"
  "# mycall=true drops this station's own call ([feed] call) and the calls\n"
  "# with up to mycall_diff changes against it, place by place (a letter\n"
  "# other, or missing from a cut call) — the busts of its own transmissions;\n"
  "# a call MASTER.SCP knows passes all the same\n"
  "mycall=false\nmycall_diff=2\n"
  "# bad calls, comma-separated, dropped the same way: a prefix (UA9 — every\n"
  "# call that starts with it) or a whole call (OK1ABC, also OK1ABC/P)\n"
  "bad=\n"
  "\n[rtty_palette]\n# the web page's RTTY palette over the waterfall (the browser only):\n"
  "# open_on_spot = a click on an RTTY spot opens it; bandwidth = the audio's\n"
  "# passband while it decodes (250-500 Hz); autotune centres a steady\n"
  "# mark/space pair once; afc follows it within afc_max_hz (10-180) at\n"
  "# afc_rate Hz a character (5-85); squelch_db 0 = off (1-10 dB over the\n"
  "# noise); usos = unshift on space; second_decoder = DEC 2 beside DEC 1;\n"
  "# clean = plain text instead of the time tape; sound = heard while decoding\n"
  "#open_on_spot=true\n#bandwidth=400\n#autotune=true\n#afc=true\n#afc_max_hz=60\n"
  "#afc_rate=60\n#reverse=false\n#usos=true\n#squelch_db=0\n#second_decoder=true\n"
  "#clean=false\n#sound=true\n"
  "\n[wspr]\n# WSPR decoded beside CW/RTTY: every WSPR-2 frequency (dial + 1400-1600 Hz)\n"
  "# inside a band received, each even minute (WSJT-X's wsprd, built in);\n"
  "# the stations heard in the last 24 h are on the web page (Statistics).\n"
  "# upload=true posts the spots to WSPRnet (wsprnet.org) under call and\n"
  "# locator (empty = [feed] call, [station] locator)\n"
  "# deep=true: a deeper candidate search (wsprd -d: slower, a few more\n"
  "# decodes); subtract=false: one pass, decoded signals not subtracted (-s)\n"
  "enabled=false\nupload=false\ncall=\nlocator=\ndeep=false\nsubtract=true\n"
  "\n[ft8]\n# FT8 and FT4 decoded beside CW/RTTY: every FT8/FT4 dial frequency inside a\n"
  "# band received, each cycle (15 s / 7.5 s), by WSJT-X's own decoders (built\n"
  "# in). Each station that sends (a CQ, a report, an RR73 …) is a spot on the\n"
  "# telnet feed at its own frequency, once a band and mode in 10 min (feed=false:\n"
  "# the web page only); hashed calls <...> never. depth 1-3 = WSJT-X's\n"
  "# Fast/Normal/Deep (3 costs much more CPU); lo_hz/hi_hz = the audio window\n"
  "# above the dial (lo 200-1000, hi 2000-4000); threads = decoders side by side\n"
  "# upload=true sends the stations heard to PSK Reporter (pskreporter.info) under\n"
  "# call and locator (empty = [feed] call, [station] locator), as WSJT-X does\n"
  "# bands = the [bands] names whose dials are decoded (empty = every band), e.g. 40m, 20m\n"
  "enabled=false\nft8=true\nft4=true\nfeed=true\ndepth=2\nlo_hz=200\nhi_hz=3500\nthreads=2\nbands=\n"
  "upload=false\ncall=\nlocator=\n"
  "\n[js8]\n# JS8 decoded beside CW/RTTY: every JS8 dial frequency inside a band received,\n"
  "# in every submode at once (normal 15 s, fast 10 s, turbo 6 s, slow 30 s), by\n"
  "# JS8Call-improved's own decoder (built in). Each station that sends (a\n"
  "# heartbeat, a CQ, a directed frame) is a spot at its own frequency, once a\n"
  "# band in 10 min. JS8 has only a 12-bit CRC: a frame read with many errors\n"
  "# (quality under clean) is deep — its call reaches the telnet feed only once\n"
  "# confirmed (a second decode within 30 min, MASTER.SCP, or confirmed on JS8\n"
  "# in the last 90 days); the web page shows it at once. feed=false: the web\n"
  "# page only. serve=true keeps one decoder a dial running, so it combines\n"
  "# repeated frames across periods. upload=true sends the stations heard to\n"
  "# PSK Reporter under [ft8] call and locator (empty = [feed] call, [station]\n"
  "# locator). bands = the [bands] names whose dials are decoded (empty = all)\n"
  "enabled=false\nsubmodes=normal,fast,turbo,slow\nfeed=true\nlo_hz=200\nhi_hz=3500\n"
  "threads=2\nbands=\nserve=true\nclean=0.80\nupload=false\n"
  "\n[update]\n# once a day a look at the releases on GitHub (ok1hra/skimmer-for-linux):\n"
  "# auto = a newer version is downloaded, checked (sha256, a trial run on\n"
  "# this config) and swapped in for the running AppImage at hour_utc, the old\n"
  "# one kept as <file>.prev; notify = only shown on the web page; off = never\n"
  "# asked. Only a release AppImage updates itself; a development build reports.\n"
  "mode=auto\nhour_utc=3\n"
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
  g_free(cfg->rp_b_host);
  g_free(cfg->feed_call);
  g_free(cfg->feed_gate);
  g_free(cfg->feed_gate_mode);
  g_free(cfg->feed_address);
  g_strfreev(cfg->settings_from);
  g_strfreev(cfg->visitors_free);
  g_strfreev(cfg->peers);
  g_free(cfg->hub_name);
  g_strfreev(cfg->hub_allow);
  g_strfreev(cfg->rec_bands);
  g_free(cfg->rec_dir);
  g_free(cfg->archive_dir);
  g_free(cfg->filter_call);
  g_free(cfg->filter_up);
  g_free(cfg->filter_bad);
  g_strfreev(cfg->filter_bad_pfx);
  g_strfreev(cfg->filter_bad_name);
  g_free(cfg->wspr_call);
  g_free(cfg->wspr_upload_url);
  g_free(cfg->ft8_call);
  g_strfreev(cfg->ft8_bands);
  g_free(cfg->ft8_pskr_host);
  g_strfreev(cfg->js8_bands);
  if (cfg->filter_re) { g_regex_unref(cfg->filter_re); }
  memset(cfg, 0, sizeof(*cfg));
}

/* Position by position: each place where the two differ counts one, and so
 * does each letter one of them lacks at the end (a cut call) — the letters
 * keep their places, so a call read backwards differs in all of them. */
guint skim_hd_call_distance(const char *a, const char *b) {
  const gsize n = strlen(a), m = strlen(b);
  guint d = 0;
  for (gsize i = 0; i < MAX(n, m); i++) {
    d += i >= n || i >= m || g_ascii_toupper(a[i]) != g_ascii_toupper(b[i]);
  }
  return d;
}

/* The own call's filter: the call, or any of its /-parts (OK1HRA/P, F/OK1HRA),
 * within mycall_diff changes of the own call's base (its longest part). */
static gboolean (*call_known)(const char *call);

void skim_hd_config_set_known(gboolean (*known)(const char *call)) { call_known = known; }

/* Is the call one of the own call's busts? The own call itself (or with a
 * /part: OK1HRA/P) always; a call a few changes off it only while the known
 * calls (MASTER.SCP) do not have it — a real neighbour passes. */
static gboolean near_mycall(const SkimHdConfig *cfg, const char *call) {
  char **mine = g_strsplit(cfg->feed_call, "/", -1);
  const char *base = mine[0];
  for (char **p = mine; *p; p++) {
    if (strlen(*p) > strlen(base)) { base = *p; }
  }
  guint d = skim_hd_call_distance(call, base);
  const char *closest = call;
  char **parts = g_strsplit(call, "/", -1);
  for (char **p = parts; *p; p++) {
    const guint dp = strlen(*p) >= 3 ? skim_hd_call_distance(*p, base) : G_MAXUINT;
    if (dp < d) { d = dp; closest = *p; }
  }
  gboolean bust = d <= cfg->filter_mycall_diff;
  if (bust && d > 0 && call_known && (call_known(call) || call_known(closest))) { bust = FALSE; }
  g_strfreev(parts);
  g_strfreev(mine);
  return bust;
}

gboolean skim_hd_config_filter_on(const SkimHdConfig *cfg) {
  return cfg->filter_call || cfg->filter_bad || (cfg->filter_mycall && cfg->feed_call && cfg->feed_call[0]);
}

/* A bad call: it starts with one of the prefixes, one of its /-parts is one
 * of them (a whole call: OK1ABC drops DL/OK1ABC too), or its DXCC's name
 * holds one of the names. */
static gboolean bad_call(const SkimHdConfig *cfg, const char *call) {
  char *up = g_ascii_strup(call, -1);
  gboolean bad = FALSE;
  if (cfg->filter_bad_pfx) {
    char **parts = g_strsplit(up, "/", -1);
    for (char **b = cfg->filter_bad_pfx; *b && !bad; b++) {
      bad = g_str_has_prefix(up, *b);
      for (char **p = parts; *p && !bad; p++) { bad = strcmp(*p, *b) == 0; }
    }
    g_strfreev(parts);
  }
  for (char **n = cfg->filter_bad_name; n && *n && !bad; n++) { bad = skim_hd_dxcc_name_has(up, *n); }
  g_free(up);
  return bad;
}

gboolean skim_hd_config_call_pass(const SkimHdConfig *cfg, const char *call) {
  if (cfg->filter_mycall && cfg->feed_call && cfg->feed_call[0] && near_mycall(cfg, call)) {
    return FALSE;
  }
  if (cfg->filter_bad && bad_call(cfg, call)) { return FALSE; }
  if (cfg->filter_re) { return g_regex_match(cfg->filter_re, call, 0, NULL); }
  if (!cfg->filter_up) { return TRUE; }
  char *up = g_ascii_strup(call, -1);
  const gboolean ok = strstr(up, cfg->filter_up) != NULL;
  g_free(up);
  return ok;
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

/* A key with a value: present and not blank. A blank value ("port=", what the
 * web form sends for an empty field) is the default, as if the key were absent. */
static gboolean given(GKeyFile *kf, const char *group, const char *key, GError **unused) {
  (void)unused;
  char *v = g_key_file_get_value(kf, group, key, NULL);
  const gboolean ok = v && *g_strstrip(v);
  g_free(v);
  return ok;
}

/* A boolean key: dflt when absent or blank, an error when it is no boolean. */
static gboolean opt_bool(GKeyFile *kf, const char *group, const char *key, gboolean dflt,
                         gboolean *out, const char *path, GError **error) {
  *out = dflt;
  if (!given(kf, group, key, NULL)) { return TRUE; }
  GError *e = NULL;
  *out = g_key_file_get_boolean(kf, group, key, &e);
  if (e) {
    g_clear_error(&e);
    return bad(error, path, "[%s] %s: true or false", group, key);
  }
  return TRUE;
}

/* A number key: dflt when absent or blank, an error outside lo–hi or when it
 * is no number (whole: no fraction allowed). */
static gboolean opt_num(GKeyFile *kf, const char *group, const char *key, double dflt, double lo,
                        double hi, gboolean whole, double *out, const char *path, GError **error) {
  *out = dflt;
  char *v = g_key_file_get_value(kf, group, key, NULL);
  if (!v || !g_strstrip(v)[0]) {
    g_free(v);
    return TRUE;
  }
  char *end = NULL;
  const double x = g_ascii_strtod(v, &end);
  const gboolean ok = end != v && !*end && x >= lo && x <= hi && (!whole || x == (double)(gint64)x);
  g_free(v);
  if (!ok) {
    return bad(error, path, whole ? "[%s] %s: a whole number, %g to %g" : "[%s] %s: %g to %g",
               group, key, lo, hi);
  }
  *out = x;
  return TRUE;
}

static gboolean engine_of(const char *s, SkimCwEngine *out) {
  if (g_strcmp0(s, "v3") == 0 || g_strcmp0(s, "v3.1") == 0) { *out = SKIM_CW_ENGINE_V3; return TRUE; }
  if (g_strcmp0(s, "v2") == 0) { *out = SKIM_CW_ENGINE_V2; return TRUE; }
  if (g_strcmp0(s, "deepcw") == 0) { *out = SKIM_CW_ENGINE_DEEPCW; return TRUE; }
  if (g_strcmp0(s, "v1") == 0) { *out = SKIM_CW_ENGINE_V1; return TRUE; }
  if (g_strcmp0(s, "v4") == 0) { *out = SKIM_CW_ENGINE_V4; return TRUE; }
  return FALSE;
}

/* The phased bands' modes (combiner.h's SkimPhaseMode, its names) — here,
 * so that config.c needs only the header: several gates build it alone. */
static const char *const PHASE_MODES[] = { "mvdr", "mrc", "fixed", "a", "b" };
G_STATIC_ASSERT(SKIM_PHASE_MVDR == 0 && SKIM_PHASE_MRC == 1 && SKIM_PHASE_FIXED == 2 &&
                SKIM_PHASE_A == 3 && SKIM_PHASE_B == 4);

static gboolean phase_mode_parse(const char *t, SkimPhaseMode *out) {
  for (guint i = 0; t && i < G_N_ELEMENTS(PHASE_MODES); i++) {
    if (g_ascii_strcasecmp(t, PHASE_MODES[i]) == 0) {
      *out = (SkimPhaseMode)i;
      return TRUE;
    }
  }
  return FALSE;
}

static const char *phase_mode_name(SkimPhaseMode m) {
  return (guint)m < G_N_ELEMENTS(PHASE_MODES) ? PHASE_MODES[m] : "?";
}

static const char *engine_name(SkimCwEngine e) {
  return e == SKIM_CW_ENGINE_DEEPCW ? "deepcw" : e == SKIM_CW_ENGINE_V1 ? "v1"
         : e == SKIM_CW_ENGINE_V3 ? "v3" : e == SKIM_CW_ENGINE_V4 ? "v4" : "v2";
}

char *skim_hd_band_modes(const SkimHdBand *b, SkimCwEngine dflt) {
  GString *s = g_string_new(NULL);
  if (b->cw) {
    g_string_append(s, "cw");
    if (b->cw_engine != dflt) { g_string_append_printf(s, ":%s", engine_name(b->cw_engine)); }
  }
  if (b->rtty) { g_string_append_printf(s, "%srtty", s->len ? "," : ""); }
  if (b->off) { g_string_append_printf(s, "%soff", s->len ? "," : ""); }
  if (b->worker[0]) { g_string_append_printf(s, " @%s", b->worker); }
  if (b->phased) {
    g_string_append(s, " ip=ab");
  } else {
    if (b->ip) { g_string_append(s, " ip=b"); }
    if (b->input) { g_string_append(s, " in=2"); }
  }
  if (b->phase_mode >= 0) {
    g_string_append_printf(s, " phase=%s", phase_mode_name((SkimPhaseMode)b->phase_mode));
  }
  return g_string_free(s, FALSE);
}

char *skim_hd_config_set_phase(const char *text, gsize len, const char *band, gint mode,
                               double gain_db, double phase_deg, const double mem[3][2],
                               GError **error) {
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_data(kf, text, len, G_KEY_FILE_KEEP_COMMENTS, error)) {
    g_key_file_free(kf);
    return NULL;
  }
  char *line = g_key_file_get_value(kf, "bands", band, NULL);
  if (!line) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no [bands] line %s", band);
    g_key_file_free(kf);
    return NULL;
  }
  /* the line again without its phase=, then the new one */
  GString *nl = g_string_new(NULL);
  char **tok = g_strsplit_set(line, " \t", -1);
  for (char **t = tok; *t; t++) {
    if (!**t) { continue; }
    char **sub = g_strsplit(*t, ",", -1);   /* "cw,ip=ab,phase=x" too */
    GString *keep = g_string_new(NULL);
    for (char **u = sub; *u; u++) {
      if (!g_str_has_prefix(*u, "phase=") && **u) {
        g_string_append_printf(keep, "%s%s", keep->len ? "," : "", *u);
      }
    }
    if (keep->len) { g_string_append_printf(nl, "%s%s", nl->len ? " " : "", keep->str); }
    g_string_free(keep, TRUE);
    g_strfreev(sub);
  }
  g_strfreev(tok);
  g_string_append_printf(nl, " phase=%s", phase_mode_name((SkimPhaseMode)mode));
  g_key_file_set_value(kf, "bands", band, nl->str);
  g_string_free(nl, TRUE);
  g_free(line);
  char *grp = g_strdup_printf("phase.%s", band);
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_key_file_set_value(kf, grp, "fixed_gain_db", g_ascii_formatd(num, sizeof(num), "%.2f", gain_db));
  g_key_file_set_value(kf, grp, "fixed_phase_deg", g_ascii_formatd(num, sizeof(num), "%.1f", phase_deg));
  for (guint k = 0; k < 3; k++) {
    char key[4] = { 'm', (char)('1' + k), 0 };
    if (mem && !isnan(mem[k][0]) && !isnan(mem[k][1])) {
      char a[G_ASCII_DTOSTR_BUF_SIZE], b[G_ASCII_DTOSTR_BUF_SIZE];
      char *v = g_strdup_printf("%s,%s", g_ascii_formatd(a, sizeof(a), "%.2f", mem[k][0]),
                                g_ascii_formatd(b, sizeof(b), "%.1f", mem[k][1]));
      g_key_file_set_value(kf, grp, key, v);
      g_free(v);
    } else {
      g_key_file_remove_key(kf, grp, key, NULL);
    }
  }
  g_free(grp);
  char *out = g_key_file_to_data(kf, NULL, NULL);
  g_key_file_free(kf);
  return out;
}

/* "<centre Hz> [mode,mode…]" */
static gboolean band_parse(SkimHdBand *b, const char *name, const char *value,
                           SkimCwEngine dflt, const char *path, GError **error) {
  g_strlcpy(b->name, name, sizeof(b->name));
  b->phase_mode = -1;
  b->fixed_gain_db = b->fixed_phase_deg = NAN;
  for (guint k = 0; k < 3; k++) { b->mem[k][0] = b->mem[k][1] = NAN; }
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
    char **m = g_strsplit_set(modes, ", \t", -1);
    for (char **q = m; *q && ok; q++) {
      char *t = g_strstrip(*q);
      if (!*t) { continue; }
      if (t[0] == '@') {
        gboolean name_ok = t[1] && strlen(t + 1) < sizeof(b->worker);
        for (const char *x = t + 1; *x && name_ok; x++) {
          name_ok = g_ascii_isalnum(*x) || *x == '-' || *x == '_' || *x == '.';
        }
        if (!name_ok) {
          ok = bad(error, path, "band %s: worker \"%s\" (@name: letters, digits, - _ .)",
                   name, t);
        } else {
          g_strlcpy(b->worker, t + 1, sizeof(b->worker));
        }
      } else if (g_strcmp0(t, "cw") == 0) {
        b->cw = TRUE;
        b->cw_engine = dflt;
      } else if (g_str_has_prefix(t, "cw:") && engine_of(t + 3, &b->cw_engine)) {
        b->cw = TRUE;
      } else if (g_strcmp0(t, "rtty") == 0) {
        b->rtty = TRUE;
      } else if (g_strcmp0(t, "off") == 0) {
        b->off = TRUE;
      } else if (g_strcmp0(t, "ip=a") == 0 || g_strcmp0(t, "ip=b") == 0) {
        b->ip = t[3] == 'b';
        b->phased = FALSE;
      } else if (g_strcmp0(t, "ip=ab") == 0) {
        b->ip = 0;
        b->phased = TRUE;
      } else if (g_strcmp0(t, "in=1") == 0 || g_strcmp0(t, "in=2") == 0) {
        b->input = t[3] == '2';
      } else if (g_str_has_prefix(t, "phase=")) {
        SkimPhaseMode pm;
        if (!phase_mode_parse(t + 6, &pm)) {
          ok = bad(error, path, "band %s: %s (phase=mvdr, mrc, fixed, a or b)", name, t);
        } else {
          b->phase_mode = (gint)pm;
        }
      } else {
        ok = bad(error, path, "band %s: mode \"%s\" (cw, cw:v3, cw:v4, cw:v2, cw:deepcw, rtty, off, "
                 "@worker, ip=a|b|ab, in=1|2, phase=…)", name, t);
      }
    }
    g_strfreev(m);
    if (ok && !b->cw && !b->rtty) {           /* "off" / "@x" / ip= alone: CW, as no modes */
      b->cw = TRUE;
      b->cw_engine = dflt;
    }
  }
  g_free(v);
  return ok;
}

/* A positive value, or the file's 0 meaning "off" (-1); absent = 0 (the
 * pipeline's default). */
static double opt_secs(GKeyFile *kf, const char *key) {
  if (!given(kf, "feed", key, NULL)) { return 0; }
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
  } else if (g_strcmp0(type, "hub") == 0) {
    h->source = SKIM_HD_SOURCE_HUB;
  } else {
    bad(error, path, "[radio] type \"%s\" (hpsdr-p1, mock or hub)", type);
    g_free(type);
    return FALSE;
  }
  g_free(type);
  h->ppm = given(kf, "radio", "clock_ppm", NULL)
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
  if (given(kf, "radio", "rate", NULL)) {
    h->rate = (guint)g_key_file_get_integer(kf, "radio", "rate", NULL);
  }
  if (h->rate != 48000 && h->rate != 96000 && h->rate != 192000 && h->rate != 384000) {
    return bad(error, path, "rate %u (48000, 96000, 192000 or 384000)", h->rate);
  }
  char *rpm = g_key_file_get_string(kf, "rp", "mode", NULL);
  if (rpm) { g_strstrip(rpm); }
  if (rpm && rpm[0] && strcmp(rpm, "p1") != 0 && strcmp(rpm, "extended") != 0) {
    bad(error, path, "[rp] mode \"%s\" (p1 or extended)", rpm);
    g_free(rpm);
    return FALSE;
  }
  h->rp_split = rpm && strcmp(rpm, "extended") == 0;
  g_free(rpm);
  char *bh = g_key_file_get_string(kf, "rp", "b_host", NULL);
  if (bh) { g_strstrip(bh); }
  if (bh && bh[0]) {
    if (bh[0] == '-' || strpbrk(bh, " \t,@")) {
      bad(error, path, "[rp] b_host \"%s\": one address", bh);
      g_free(bh);
      return FALSE;
    }
    h->rp_b_host = bh;
  } else {
    g_free(bh);
  }
  /* [phase] */
  {
    char *pm = g_key_file_get_string(kf, "phase", "mode", NULL);
    g_strstrip(pm ? pm : (pm = g_strdup("")));
    SkimPhaseMode m = SKIM_PHASE_MVDR;
    if (*pm && !phase_mode_parse(pm, &m)) {
      bad(error, path, "[phase] mode \"%s\": mvdr, mrc, fixed, a or b", pm);
      g_free(pm);
      return FALSE;
    }
    g_free(pm);
    h->phase_mode = (gint)m;
    h->phase_layer2 = given(kf, "phase", "layer2", NULL)
                          ? g_key_file_get_boolean(kf, "phase", "layer2", NULL) : TRUE;
    if (!opt_num(kf, "phase", "tc_s", 1.0, 0.1, 30, FALSE, &h->phase_tc_s, path, error) ||
        !opt_num(kf, "phase", "gate_db", 6.0, 1, 30, FALSE, &h->phase_gate_db, path, error) ||
        !opt_num(kf, "phase", "fixed_gain_db", 0.0, -40, 40, FALSE, &h->phase_fixed_gain_db, path,
                 error) ||
        !opt_num(kf, "phase", "fixed_phase_deg", 0.0, -360, 360, FALSE, &h->phase_fixed_phase_deg,
                 path, error)) {
      return FALSE;
    }
  }
  h->rp_b_rate = h->rate;
  if (given(kf, "rp", "b_rate", NULL)) {
    h->rp_b_rate = (guint)g_key_file_get_integer(kf, "rp", "b_rate", NULL);
  }
  if (h->rp_b_rate != 48000 && h->rp_b_rate != 96000 && h->rp_b_rate != 192000 &&
      h->rp_b_rate != 384000) {
    return bad(error, path, "[rp] b_rate %u (48000, 96000, 192000 or 384000)", h->rp_b_rate);
  }

  char *eng = g_key_file_get_string(kf, "decode", "engine", NULL);
  if (eng) { g_strstrip(eng); }
  h->engine = SKIM_CW_ENGINE_V3;
  if (eng && eng[0] && !engine_of(eng, &h->engine)) {
    bad(error, path, "[decode] engine \"%s\" (v3, v4, v2 or deepcw)", eng);
    g_free(eng);
    return FALSE;
  }
  g_free(eng);
  h->decode_log = given(kf, "decode", "log", NULL) &&
                  g_key_file_get_boolean(kf, "decode", "log", NULL);
  if (!opt_bool(kf, "decode", "suppress_cw_on_rtty", FALSE, &h->suppress_cw_on_rtty, path,
                error)) {
    return FALSE;
  }
  {
    double sp, mi;
    if (!opt_bool(kf, "decode", "rtty_adaptive", FALSE, &h->rtty_adaptive, path, error) ||
        !opt_num(kf, "decode", "rtty_adaptive_spots", 100, 0, 100000, TRUE, &sp, path, error) ||
        !opt_num(kf, "decode", "rtty_adaptive_minutes", 2, 1, 30, TRUE, &mi, path, error)) {
      return FALSE;
    }
    h->rtty_adaptive_spots = (guint)sp;
    h->rtty_adaptive_minutes = (guint)mi;
  }
  {
    double w;
    if (!opt_num(kf, "decode", "v4_workers", 0, 0, 64, TRUE, &w, path, error) ||
        !opt_bool(kf, "decode", "v4_guard", TRUE, &h->v4_guard, path, error)) {
      return FALSE;
    }
    h->v4_workers = (guint)w;
  }

  h->feed_call = g_key_file_get_string(kf, "feed", "call", NULL);
  if (h->feed_call) {                          /* a call is in capitals: DX de OK1ABC-# */
    g_strstrip(h->feed_call);
    for (char *x = h->feed_call; *x; x++) { *x = g_ascii_toupper(*x); }
  }
  h->feed_port = given(kf, "feed", "port", NULL)
                     ? g_key_file_get_integer(kf, "feed", "port", NULL) : 7300;
  h->feed_min_score = given(kf, "feed", "min_score", NULL)
                          ? g_key_file_get_double(kf, "feed", "min_score", NULL) : 0;
  h->feed_min_hearings = given(kf, "feed", "min_hearings", NULL)
      ? (guint)MAX(1, g_key_file_get_integer(kf, "feed", "min_hearings", NULL)) : 0;
  h->feed_settle_s = opt_secs(kf, "settle_s");               /* 0 = off      */
  h->feed_settle_nodict_s = opt_secs(kf, "settle_nodict_s"); /* 0 = settle_s */
  h->feed_fresh_s = opt_secs(kf, "fresh_s");                 /* 0 = off      */
  h->feed_fold_s = opt_secs(kf, "fold_s");                   /* 0 = off      */
  h->feed_learn = given(kf, "feed", "learn", NULL) &&
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
  /* address: what the page header offers as the telnet feed — host:port,
   * or a telnet://, http:// or https:// URL; printable, no spaces/quotes */
  char *addr = g_key_file_get_string(kf, "feed", "address", NULL);
  if (addr && g_strstrip(addr)[0]) {
    gboolean ok = strlen(addr) <= 120;
    for (const char *x = addr; ok && *x; x++) {
      ok = g_ascii_isgraph(*x) && !strchr("\"'<>\\`", *x);
    }
    const char *sch = strstr(addr, "://");
    if (ok && sch) {
      ok = g_ascii_strncasecmp(addr, "telnet://", 9) == 0 ||
           g_ascii_strncasecmp(addr, "http://", 7) == 0 ||
           g_ascii_strncasecmp(addr, "https://", 8) == 0;
    }
    if (!ok) {
      g_free(addr);
      return bad(error, path, "[feed] address: host:port, or a telnet://, http:// or https:// "
                 "URL (no spaces or quotes, 120 characters at most)");
    }
    h->feed_address = addr;
  } else {
    g_free(addr);
  }

  if (!opt_bool(kf, "rbn", "enabled", FALSE, &h->rbn_on, path, error)) { return FALSE; }
  h->rbn_port = 7550;
  if (given(kf, "rbn", "port", NULL)) {
    GError *e = NULL;
    h->rbn_port = g_key_file_get_integer(kf, "rbn", "port", &e);
    if (e || h->rbn_port < 1 || h->rbn_port > 65535) {
      g_clear_error(&e);
      return bad(error, path, "[rbn] port: 1 to 65535");
    }
  }
  if (h->rbn_on && h->feed_port > 0 && h->rbn_port == h->feed_port) {
    return bad(error, path, "[rbn] port %d is the [feed] port too", h->rbn_port);
  }
  if (given(kf, "rbn", "delay_s", NULL)) {
    GError *e = NULL;
    const int d = g_key_file_get_integer(kf, "rbn", "delay_s", &e);
    if (e || d < 0 || d > 99) {
      g_clear_error(&e);
      return bad(error, path, "[rbn] delay_s: whole seconds, 0 to 99");
    }
    h->rbn_delay_s = (guint)d;
  }

  {
    char *loc = g_key_file_get_string(kf, "station", "locator", NULL);
    if (loc) {
      g_strstrip(loc);
      for (char *x = loc; *x; x++) { *x = g_ascii_toupper(*x); }
      const gsize n = strlen(loc);
      const gboolean ok_loc = n == 0 ||
          ((n == 4 || n == 6) && loc[0] >= 'A' && loc[0] <= 'R' && loc[1] >= 'A' && loc[1] <= 'R' &&
           g_ascii_isdigit(loc[2]) && g_ascii_isdigit(loc[3]) &&
           (n == 4 || (loc[4] >= 'A' && loc[4] <= 'X' && loc[5] >= 'A' && loc[5] <= 'X')));
      if (!ok_loc) {
        bad(error, path, "[station] locator \"%s\" (a Maidenhead locator: JN18 or JN18EU)", loc);
        g_free(loc);
        return FALSE;
      }
      g_strlcpy(h->locator, loc, sizeof(h->locator));
      g_free(loc);
    }
  }
  h->hub_name = g_key_file_get_string(kf, "hub", "name", NULL);
  if (h->hub_name) { g_strstrip(h->hub_name); }
  if (!h->hub_name || !h->hub_name[0]) {
    g_free(h->hub_name);
    h->hub_name = g_strdup(g_get_host_name());
  }
  for (const char *x = h->hub_name; *x; x++) {
    if (!g_ascii_isalnum(*x) && *x != '-' && *x != '_' && *x != '.') {
      return bad(error, path, "[hub] name \"%s\": letters, digits, - _ . only", h->hub_name);
    }
  }
  h->hub_port = SKIM_HD_HUB_PORT;
  if (given(kf, "hub", "port", NULL)) {
    GError *e = NULL;
    h->hub_port = g_key_file_get_integer(kf, "hub", "port", &e);
    if (e || h->hub_port < 0 || h->hub_port > 65535) {
      g_clear_error(&e);
      return bad(error, path, "[hub] port: 0 (off) to 65535");
    }
  }
  {
    char *av = g_key_file_get_string(kf, "hub", "allow", NULL);
    GPtrArray *al = g_ptr_array_new();
    char **ap = g_strsplit_set(av ? av : "", ", \t", -1);
    gboolean aok = TRUE;
    for (char **q = ap; *q && aok; q++) {
      if (!**q) { continue; }
      GInetAddressMask *m = g_inet_address_mask_new_from_string(*q, NULL);
      if (!m) {
        aok = bad(error, path, "[hub] allow \"%s\" (an address or address/bits)", *q);
      } else {
        g_object_unref(m);
        g_ptr_array_add(al, g_strdup(*q));
      }
    }
    g_strfreev(ap);
    g_free(av);
    if (!al->len) {
      static const char *const PRIVATE[] = { "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                                             "127.0.0.0/8", "::1", "fe80::/10", "fc00::/7" };
      for (guint i = 0; i < G_N_ELEMENTS(PRIVATE); i++) { g_ptr_array_add(al, g_strdup(PRIVATE[i])); }
    }
    g_ptr_array_add(al, NULL);
    h->hub_allow = (char **)g_ptr_array_free(al, FALSE);
    if (!aok) { return FALSE; }
  }
  {
    char *pv = g_key_file_get_string(kf, "peers", "hosts", NULL);
    GPtrArray *pl = g_ptr_array_new();
    char **pp = g_strsplit_set(pv ? pv : "", ", \t", -1);
    for (char **q = pp; *q; q++) {
      if (**q) { g_ptr_array_add(pl, g_strdup(*q)); }
    }
    g_strfreev(pp);
    g_free(pv);
    g_ptr_array_add(pl, NULL);
    h->peers = (char **)g_ptr_array_free(pl, FALSE);
  }

  {
    /* [archive] dir: ~/ = the home, relative = next to this file */
    char *ad = g_key_file_get_string(kf, "archive", "dir", NULL);
    if (ad && g_strstrip(ad)[0]) {
      if (g_str_has_prefix(ad, "~/") || strcmp(ad, "~") == 0) {
        h->archive_dir = g_build_filename(g_get_home_dir(), ad + 1, NULL);
      } else if (g_path_is_absolute(ad)) {
        h->archive_dir = g_canonicalize_filename(ad, NULL);
      } else {
        char *base = g_path_get_dirname(path);
        h->archive_dir = g_canonicalize_filename(ad, base);
        g_free(base);
      }
    }
    g_free(ad);
  }
  {
    /* [filter]: the raw value — a regular expression keeps its backslashes */
    char *fc = g_key_file_get_value(kf, "filter", "call", NULL);
    if (fc) { g_strstrip(fc); }
    if (!opt_bool(kf, "filter", "regex", FALSE, &h->filter_regex, path, error)) {
      g_free(fc);
      return FALSE;
    }
    if (fc && fc[0]) {
      h->filter_call = fc;
      fc = NULL;
      if (h->filter_regex) {
        GError *e = NULL;
        h->filter_re = g_regex_new(h->filter_call, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &e);
        if (!h->filter_re) {
          bad(error, path, "[filter] call \"%s\" is not a regular expression: %s", h->filter_call,
              e->message);
          g_clear_error(&e);
          return FALSE;
        }
      } else {
        h->filter_up = g_ascii_strup(h->filter_call, -1);
      }
    }
    g_free(fc);
    if (!opt_bool(kf, "filter", "mycall", FALSE, &h->filter_mycall, path, error)) { return FALSE; }
    h->filter_mycall_diff = 2;
    char *dv = g_key_file_get_value(kf, "filter", "mycall_diff", NULL);
    if (dv && g_strstrip(dv)[0]) {                     /* empty = the default */
      char *end = NULL;
      const gint64 d = g_ascii_strtoll(dv, &end, 10);
      if (*end || d < 0 || d > 3) {
        g_free(dv);
        return bad(error, path, "[filter] mycall_diff: 0 to 3 characters");
      }
      h->filter_mycall_diff = (guint)d;
    }
    g_free(dv);
    /* bad: a token of letters alone, four or more (spaces, dots, dashes in
     * it: "Asiatic Russia", "St. Helena"), is a country's name; any other
     * a prefix or a call (A-Z, 0-9, /) */
    char *bv = g_key_file_get_value(kf, "filter", "bad", NULL);
    if (bv && g_strstrip(bv)[0]) {
      GPtrArray *pf = g_ptr_array_new(), *nm = g_ptr_array_new();
      char **tok = g_strsplit_set(bv, ",;", -1);
      gboolean ok = TRUE;
      for (char **t = tok; *t && ok; t++) {
        char *x = g_ascii_strup(g_strstrip(*t), -1);
        if (!*x) { g_free(x); continue; }
        gboolean letters = TRUE, callish = TRUE;
        guint nl = 0;
        for (const char *c = x; *c; c++) {
          if (g_ascii_isupper(*c)) { nl++; }
          else if (*c != ' ' && *c != '.' && *c != '-' && *c != '\'') { letters = FALSE; }
          if (!g_ascii_isupper(*c) && !g_ascii_isdigit(*c) && *c != '/') { callish = FALSE; }
        }
        if (letters && nl >= 4) {
          g_ptr_array_add(nm, x);
        } else if (callish) {
          g_ptr_array_add(pf, x);
        } else {
          ok = FALSE;
          bad(error, path, "[filter] bad: \"%s\" is neither a prefix, a call nor a country", x);
          g_free(x);
        }
      }
      g_strfreev(tok);
      if (!ok) {
        g_ptr_array_free(pf, TRUE);
        g_ptr_array_free(nm, TRUE);
        g_free(bv);
        return FALSE;
      }
      h->filter_bad = g_strdup(bv);
      if (pf->len) { g_ptr_array_add(pf, NULL); h->filter_bad_pfx = (char **)g_ptr_array_free(pf, FALSE); }
      else { g_ptr_array_free(pf, TRUE); }
      if (nm->len) { g_ptr_array_add(nm, NULL); h->filter_bad_name = (char **)g_ptr_array_free(nm, FALSE); }
      else { g_ptr_array_free(nm, TRUE); }
      if (!h->filter_bad_pfx && !h->filter_bad_name) { g_clear_pointer(&h->filter_bad, g_free); }
    }
    g_free(bv);
  }

  {
    /* [thermal]: limit_c blank = 0, the start takes the sensor's critical
     * temperature − 15 °C (80 °C without one) */
    double lim, avg, pev, pmin;
    if (!opt_bool(kf, "thermal", "enabled", FALSE, &h->thermal, path, error) ||
        !opt_num(kf, "thermal", "limit_c", 0, 40, 110, FALSE, &lim, path, error) ||
        !opt_num(kf, "thermal", "average_min", 5, 1, 30, TRUE, &avg, path, error) ||
        !opt_num(kf, "thermal", "restore_margin_c", 3, 0, 20, FALSE, &h->thermal_margin_c, path,
                 error) ||
        /* 0 = no rotation; else 10 min at least — a pause's drop is measured
         * 10 min after it */
        !opt_num(kf, "thermal", "probe_every_min", 30, 0, 1440, TRUE, &pev, path, error) ||
        !opt_num(kf, "thermal", "probe_min", 5, 1, 30, TRUE, &pmin, path, error)) {
      return FALSE;
    }
    if (pev > 0 && pev < 10) {
      return bad(error, path, "[thermal] probe_every_min: 0, or 10 to 1440");
    }
    h->thermal_limit_c = lim;
    h->thermal_average_min = (guint)avg;
    h->thermal_probe_every_min = (guint)pev;
    h->thermal_probe_min = (guint)pmin;
  }

  {
    /* [rtty_palette]: the page's only, checked here so a bad value is a
     * bad config like any other */
    double bw, mx, rate, sq;
    if (!opt_bool(kf, "rtty_palette", "open_on_spot", TRUE, &h->rtty_palette.open_on_spot, path, error) ||
        !opt_bool(kf, "rtty_palette", "autotune", TRUE, &h->rtty_palette.autotune, path, error) ||
        !opt_bool(kf, "rtty_palette", "afc", TRUE, &h->rtty_palette.afc, path, error) ||
        !opt_bool(kf, "rtty_palette", "reverse", FALSE, &h->rtty_palette.reverse, path, error) ||
        !opt_bool(kf, "rtty_palette", "usos", TRUE, &h->rtty_palette.usos, path, error) ||
        !opt_bool(kf, "rtty_palette", "second_decoder", TRUE, &h->rtty_palette.second_decoder, path,
                  error) ||
        !opt_bool(kf, "rtty_palette", "clean", FALSE, &h->rtty_palette.clean, path, error) ||
        !opt_bool(kf, "rtty_palette", "sound", TRUE, &h->rtty_palette.sound, path, error) ||
        !opt_num(kf, "rtty_palette", "bandwidth", 400, 250, 500, TRUE, &bw, path, error) ||
        !opt_num(kf, "rtty_palette", "afc_max_hz", 60, 10, 180, TRUE, &mx, path, error) ||
        !opt_num(kf, "rtty_palette", "afc_rate", 60, 5, 85, TRUE, &rate, path, error) ||
        !opt_num(kf, "rtty_palette", "squelch_db", 0, 0, 10, TRUE, &sq, path, error)) {
      return FALSE;
    }
    h->rtty_palette.bandwidth = (guint)bw;
    h->rtty_palette.afc_max_hz = (guint)mx;
    h->rtty_palette.afc_rate = (guint)rate;
    h->rtty_palette.squelch_db = (guint)sq;
  }

  {
    /* [wspr]: call and locator empty = [feed] call, [station] locator;
     * an upload needs both (WSPRnet files the spots under them) */
    if (!opt_bool(kf, "wspr", "enabled", FALSE, &h->wspr, path, error) ||
        !opt_bool(kf, "wspr", "upload", FALSE, &h->wspr_upload, path, error) ||
        !opt_bool(kf, "wspr", "deep", FALSE, &h->wspr_deep, path, error) ||
        !opt_bool(kf, "wspr", "subtract", TRUE, &h->wspr_subtract, path, error)) {
      return FALSE;
    }
    char *wc = g_key_file_get_string(kf, "wspr", "call", NULL);
    char *wl = g_key_file_get_string(kf, "wspr", "locator", NULL);
    char *wu = g_key_file_get_string(kf, "wspr", "upload_url", NULL);
    if (wc) { g_strstrip(wc); for (char *x = wc; *x; x++) { *x = g_ascii_toupper(*x); } }
    if (wl) { g_strstrip(wl); for (char *x = wl; *x; x++) { *x = g_ascii_toupper(*x); } }
    if (wu) { g_strstrip(wu); }
    gboolean ok_call = !wc || !*wc || (strlen(wc) >= 3 && strlen(wc) <= 15);
    for (const char *x = wc; ok_call && x && *x; x++) { ok_call = g_ascii_isalnum(*x) || *x == '/'; }
    const gsize nl = wl ? strlen(wl) : 0;
    const gboolean ok_loc = nl == 0 ||
        ((nl == 4 || nl == 6) && wl[0] >= 'A' && wl[0] <= 'R' && wl[1] >= 'A' && wl[1] <= 'R' &&
         g_ascii_isdigit(wl[2]) && g_ascii_isdigit(wl[3]) &&
         (nl == 4 || (wl[4] >= 'A' && wl[4] <= 'X' && wl[5] >= 'A' && wl[5] <= 'X')));
    if (!ok_call || !ok_loc) {
      bad(error, path, !ok_call ? "[wspr] call \"%s\" (3-15 of A-Z, 0-9, /)"
                                : "[wspr] locator \"%s\" (a Maidenhead locator: JN18 or JN18EU)",
          !ok_call ? wc : wl);
      g_free(wc); g_free(wl); g_free(wu);
      return FALSE;
    }
    h->wspr_call = wc && *wc ? g_strdup(wc) : NULL;
    g_strlcpy(h->wspr_locator, wl ? wl : "", sizeof h->wspr_locator);
    h->wspr_upload_url = wu && *wu ? g_strdup(wu) : NULL;
    g_free(wc); g_free(wl); g_free(wu);
    const char *rc = h->wspr_call ? h->wspr_call : h->feed_call;
    const char *rl = h->wspr_locator[0] ? h->wspr_locator : h->locator;
    if (h->wspr && h->wspr_upload && (!rc || !*rc || !rl[0])) {
      return bad(error, path, "[wspr] upload: WSPRnet needs the receiver's call and locator — "
                              "[wspr] call (or [feed] call) and [wspr] locator (or [station] locator)");
    }
  }

  {
    /* [ft8]: the window must leave the sharp filter room before the dial
     * (lo ≥ 200); lo ≤ 1000 and hi ≥ 2000 keep it 1 kHz wide at least */
    double dep, lo, hi, thr;
    if (!opt_bool(kf, "ft8", "enabled", FALSE, &h->ft8, path, error) ||
        !opt_bool(kf, "ft8", "ft8", TRUE, &h->ft8_ft8, path, error) ||
        !opt_bool(kf, "ft8", "ft4", TRUE, &h->ft8_ft4, path, error) ||
        !opt_bool(kf, "ft8", "feed", TRUE, &h->ft8_feed, path, error) ||
        !opt_num(kf, "ft8", "depth", 2, 1, 3, TRUE, &dep, path, error) ||
        !opt_num(kf, "ft8", "lo_hz", 200, 200, 1000, TRUE, &lo, path, error) ||
        !opt_num(kf, "ft8", "hi_hz", 3500, 2000, 4000, TRUE, &hi, path, error) ||
        !opt_num(kf, "ft8", "threads", 2, 1, 8, TRUE, &thr, path, error)) {
      return FALSE;
    }
    h->ft8_depth = (guint)dep;
    h->ft8_lo_hz = (guint)lo;
    h->ft8_hi_hz = (guint)hi;
    h->ft8_threads = (guint)thr;
    /* PSK Reporter: call and locator as [wspr]'s (empty = [feed] call,
     * [station] locator); an upload needs both */
    /* the bands whose dials are decoded (names checked once [bands] is read) */
    char *fb = g_key_file_get_string(kf, "ft8", "bands", NULL);
    if (fb && g_strstrip(fb)[0]) {
      char **v = g_strsplit_set(fb, ", ", -1);
      GPtrArray *a = g_ptr_array_new();
      for (char **x = v; *x; x++) {
        if (**x) { g_ptr_array_add(a, g_strdup(*x)); }
      }
      g_ptr_array_add(a, NULL);
      h->ft8_bands = (char **)g_ptr_array_free(a, FALSE);
      g_strfreev(v);
    }
    g_free(fb);
    if (!opt_bool(kf, "ft8", "upload", FALSE, &h->ft8_upload, path, error)) { return FALSE; }
    char *fc = g_key_file_get_string(kf, "ft8", "call", NULL);
    char *fl = g_key_file_get_string(kf, "ft8", "locator", NULL);
    char *fh = g_key_file_get_string(kf, "ft8", "pskreporter", NULL);
    if (fc) { g_strstrip(fc); for (char *x = fc; *x; x++) { *x = g_ascii_toupper(*x); } }
    if (fl) { g_strstrip(fl); for (char *x = fl; *x; x++) { *x = g_ascii_toupper(*x); } }
    if (fh) { g_strstrip(fh); }
    gboolean ok_call = !fc || !*fc || (strlen(fc) >= 3 && strlen(fc) <= 15);
    for (const char *x = fc; ok_call && x && *x; x++) { ok_call = g_ascii_isalnum(*x) || *x == '/'; }
    const gsize nl = fl ? strlen(fl) : 0;
    const gboolean ok_loc = nl == 0 ||
        ((nl == 4 || nl == 6) && fl[0] >= 'A' && fl[0] <= 'R' && fl[1] >= 'A' && fl[1] <= 'R' &&
         g_ascii_isdigit(fl[2]) && g_ascii_isdigit(fl[3]) &&
         (nl == 4 || (fl[4] >= 'A' && fl[4] <= 'X' && fl[5] >= 'A' && fl[5] <= 'X')));
    /* the gate's stand-in: host:port */
    guint64 port = 0;
    char *colon = fh && *fh ? strrchr(fh, ':') : NULL;
    const gboolean ok_host = !fh || !*fh ||
        (colon && colon != fh && g_ascii_string_to_unsigned(colon + 1, 10, 1, 65535, &port, NULL));
    if (!ok_call || !ok_loc || !ok_host) {
      bad(error, path, !ok_call ? "[ft8] call \"%s\" (3-15 of A-Z, 0-9, /)"
                     : !ok_loc ? "[ft8] locator \"%s\" (a Maidenhead locator: JN18 or JN18EU)"
                               : "[ft8] pskreporter \"%s\" (host:port)",
          !ok_call ? fc : !ok_loc ? fl : fh);
      g_free(fc); g_free(fl); g_free(fh);
      return FALSE;
    }
    h->ft8_call = fc && *fc ? g_strdup(fc) : NULL;
    g_strlcpy(h->ft8_locator, fl ? fl : "", sizeof h->ft8_locator);
    if (colon) {
      h->ft8_pskr_host = g_strndup(fh, (gsize)(colon - fh));
      h->ft8_pskr_port = (guint)port;
    }
    g_free(fc); g_free(fl); g_free(fh);
    const char *rc = h->ft8_call ? h->ft8_call : h->feed_call;
    const char *rl = h->ft8_locator[0] ? h->ft8_locator : h->locator;
    if (h->ft8 && h->ft8_upload && (!rc || !*rc || !rl[0])) {
      return bad(error, path, "[ft8] upload: PSK Reporter needs the receiver's call and locator — "
                              "[ft8] call (or [feed] call) and [ft8] locator (or [station] locator)");
    }
  }

  {
    /* [js8]: the window as [ft8]'s; the submodes by name */
    double lo, hi, thr, clean;
    if (!opt_bool(kf, "js8", "enabled", FALSE, &h->js8, path, error) ||
        !opt_bool(kf, "js8", "feed", TRUE, &h->js8_feed, path, error) ||
        !opt_bool(kf, "js8", "serve", TRUE, &h->js8_serve, path, error) ||
        !opt_bool(kf, "js8", "upload", FALSE, &h->js8_upload, path, error) ||
        !opt_num(kf, "js8", "lo_hz", 200, 200, 1000, TRUE, &lo, path, error) ||
        !opt_num(kf, "js8", "hi_hz", 3500, 2000, 4000, TRUE, &hi, path, error) ||
        !opt_num(kf, "js8", "threads", 2, 1, 8, TRUE, &thr, path, error) ||
        !opt_num(kf, "js8", "clean", 0.80, 0.50, 1.00, FALSE, &clean, path, error)) {
      return FALSE;
    }
    h->js8_lo_hz = (guint)lo;
    h->js8_hi_hz = (guint)hi;
    h->js8_threads = (guint)thr;
    h->js8_clean = clean;
    static const char *const SUBS[] = { "normal", "fast", "turbo", "slow" };
    h->js8_submodes = 0xf;
    if (given(kf, "js8", "submodes", NULL)) {
      char *sv = g_key_file_get_string(kf, "js8", "submodes", NULL);
      char **w = g_strsplit_set(sv ? sv : "", ", ", -1);
      guint bits = 0;
      const char *unknown = NULL;
      for (char **x = w; *x; x++) {
        if (!**x) { continue; }
        gboolean hit = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS(SUBS); i++) {
          if (g_ascii_strcasecmp(*x, SUBS[i]) == 0) { bits |= 1u << i; hit = TRUE; }
        }
        if (!hit && !unknown) { unknown = *x; }
      }
      if (unknown || !bits) {
        bad(error, path, "[js8] submodes \"%s\": some of normal, fast, turbo, slow",
            unknown ? unknown : (sv ? sv : ""));
        g_strfreev(w);
        g_free(sv);
        return FALSE;
      }
      h->js8_submodes = bits;
      g_strfreev(w);
      g_free(sv);
    }
    char *jb = g_key_file_get_string(kf, "js8", "bands", NULL);
    if (jb && g_strstrip(jb)[0]) {
      char **v = g_strsplit_set(jb, ", ", -1);
      GPtrArray *a = g_ptr_array_new();
      for (char **x = v; *x; x++) {
        if (**x) { g_ptr_array_add(a, g_strdup(*x)); }
      }
      g_ptr_array_add(a, NULL);
      h->js8_bands = (char **)g_ptr_array_free(a, FALSE);
      g_strfreev(v);
    }
    g_free(jb);
    const char *rc = h->ft8_call ? h->ft8_call : h->feed_call;
    const char *rl = h->ft8_locator[0] ? h->ft8_locator : h->locator;
    if (h->js8 && h->js8_upload && (!rc || !*rc || !rl[0])) {
      return bad(error, path, "[js8] upload: PSK Reporter needs the receiver's call and locator — "
                              "[ft8] call (or [feed] call) and [ft8] locator (or [station] locator)");
    }
  }

  {
    /* [update]: what the daily look at the releases may do, and when */
    h->update_mode = 2;
    char *m = given(kf, "update", "mode", NULL) ? g_key_file_get_string(kf, "update", "mode", NULL)
                                                : NULL;
    if (m) {
      g_strstrip(m);
      h->update_mode = strcmp(m, "off") == 0 ? 0 : strcmp(m, "notify") == 0 ? 1
                       : strcmp(m, "auto") == 0 ? 2 : 99;
    }
    if (h->update_mode == 99) {
      bad(error, path, "[update] mode \"%s\": auto, notify or off", m);
      g_free(m);
      return FALSE;
    }
    g_free(m);
    double hr;
    if (!opt_num(kf, "update", "hour_utc", 3, 0, 23, TRUE, &hr, path, error)) { return FALSE; }
    h->update_hour = (guint)hr;
  }

  h->console_s = given(kf, "status", "console_s", NULL)
                     ? g_key_file_get_integer(kf, "status", "console_s", NULL) : -1;
  h->http_port = given(kf, "status", "http_port", NULL)
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

  {
    /* [visitors]: sign-in for the page's visitors from outside the LAN */
    if (!opt_bool(kf, "visitors", "login", TRUE, &h->visitors_login, path, error)) { return FALSE; }
    char *fv = g_key_file_get_string(kf, "visitors", "free_from", NULL);
    GPtrArray *fl = g_ptr_array_new();
    char **fp = g_strsplit_set(fv ? fv : "", ", \t", -1);
    gboolean fok = TRUE;
    for (char **q = fp; *q && fok; q++) {
      if (!**q) { continue; }
      GInetAddressMask *m = g_inet_address_mask_new_from_string(*q, NULL);
      if (!m) {
        fok = bad(error, path, "[visitors] free_from \"%s\" (an address or address/bits)", *q);
      } else {
        g_object_unref(m);
        g_ptr_array_add(fl, g_strdup(*q));
      }
    }
    g_strfreev(fp);
    g_free(fv);
    if (!fl->len) {
      static const char *const LAN[] = { "127.0.0.0/8", "::1", "10.0.0.0/8", "172.16.0.0/12",
                                         "192.168.0.0/16", "169.254.0.0/16", "fc00::/7", "fe80::/10" };
      for (guint i = 0; i < G_N_ELEMENTS(LAN); i++) { g_ptr_array_add(fl, g_strdup(LAN[i])); }
    }
    g_ptr_array_add(fl, NULL);
    h->visitors_free = (char **)g_ptr_array_free(fl, FALSE);
    if (!fok) { return FALSE; }
  }

  char *rb = g_key_file_get_string(kf, "record", "bands", NULL);
  if (rb && g_strstrip(rb)[0]) {
    h->rec_bands = g_strsplit(rb, ",", -1);
    for (char **x = h->rec_bands; *x; x++) { g_strstrip(*x); }
    h->rec_minutes = given(kf, "record", "minutes", NULL)
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
    if (h->nlisted == SKIM_HD_MAX_LISTED) {
      ok = bad(error, path, "band %s: %d [bands] lines at most", keys[i], SKIM_HD_MAX_LISTED);
      break;
    }
    SkimHdBand *b = &h->listed[h->nlisted];
    char *v = g_key_file_get_value(kf, "bands", keys[i], NULL);
    ok = band_parse(b, keys[i], v, h->engine, path, error);
    g_free(v);
    for (guint k = 0; ok && k < h->nlisted; k++) {
      if (strcmp(h->listed[k].name, keys[i]) == 0) {
        ok = bad(error, path, "band %s twice", keys[i]);
      }
    }
    if (!ok) { break; }
    b->rate = b->ip ? h->rp_b_rate : h->rate;
    if (h->rp_split) { b->input = b->ip; }     /* two antennas: the IP names the input */
    {                                          /* [phase.<band>]: its own weight, memories */
      char *grp = g_strdup_printf("phase.%s", keys[i]);
      double v;
      if (given(kf, grp, "fixed_gain_db", NULL)) {
        if (!opt_num(kf, grp, "fixed_gain_db", 0, -40, 40, FALSE, &v, path, error)) { ok = FALSE; }
        b->fixed_gain_db = v;
      }
      if (ok && given(kf, grp, "fixed_phase_deg", NULL)) {
        if (!opt_num(kf, grp, "fixed_phase_deg", 0, -360, 360, FALSE, &v, path, error)) { ok = FALSE; }
        b->fixed_phase_deg = v;
      }
      for (guint k = 0; ok && k < 3; k++) {
        char key[4] = { 'm', (char)('1' + k), 0 };
        char *m = g_key_file_get_string(kf, grp, key, NULL);
        if (m && *g_strstrip(m)) {
          char *e1 = NULL, *e2 = NULL;
          const double g = g_ascii_strtod(m, &e1);
          const double ph = e1 && *e1 == ',' ? g_ascii_strtod(e1 + 1, &e2) : NAN;
          if (isnan(ph) || !e2 || *g_strstrip(e2) || fabs(g) > 40 || fabs(ph) > 360) {
            ok = bad(error, path, "[%s] %s = \"%s\": gain_db,phase_deg", grp, key, m);
          } else {
            b->mem[k][0] = g;
            b->mem[k][1] = ph;
          }
        }
        g_free(m);
      }
      g_free(grp);
      if (!ok) { break; }
    }
    if (b->phased) {
      if (b->input) {
        ok = bad(error, path, "band %s: ip=ab takes IN1 on IP A and IN2 on IP B — no in=", keys[i]);
      } else if (b->worker[0]) {
        ok = bad(error, path, "band %s: ip=ab is phased here, not on a worker (@%s)", keys[i],
                 b->worker);
      } else if (h->source != SKIM_HD_SOURCE_HPSDR_P1) {
        ok = bad(error, path, "band %s: ip=ab is a Red Pitaya's two inputs (type=hpsdr-p1 only)",
                 keys[i]);
      } else if (h->rate != h->rp_b_rate) {
        ok = bad(error, path, "band %s: ip=ab needs one rate on both IPs — [radio] rate %u, "
                 "[rp] b_rate %u", keys[i], h->rate, h->rp_b_rate);
      }
      if (!ok) { break; }
    }
    h->nlisted++;
    if (b->off) { continue; }
    if (b->phased && h->nb_ip[1] == SKIM_HD_IP_RX) {
      ok = bad(error, path, "band %s: ip=ab takes a receiver on IP B too, and its %d are taken",
               keys[i], SKIM_HD_IP_RX);
      break;
    }
    if (h->nb_ip[b->ip] == SKIM_HD_IP_RX) {
      ok = bad(error, path, "band %s: %d receivers an IP at most (IP %c) — put off on another "
               "band%s", keys[i], SKIM_HD_IP_RX, b->ip ? 'B' : 'A',
               b->ip ? "" : ", or ip=b to take it from the board's second IP");
      break;
    }
    if (b->ip && h->source != SKIM_HD_SOURCE_HPSDR_P1) {
      ok = bad(error, path, "band %s: ip=b is a Red Pitaya's second IP (type=hpsdr-p1 only)",
               keys[i]);
      break;
    }
    h->nb_ip[b->ip]++;
    if (b->phased) {
      h->nb_ip[1]++;
      h->nb_phased++;
    }
    h->band[h->nb++] = *b;
  }
  g_strfreev(keys);
  /* a worker learns its bands from its hub: none of its own is fine */
  if (ok && h->nlisted == 0 && h->source != SKIM_HD_SOURCE_HUB) {
    ok = bad(error, path, "no [bands] entries");
  }
  if (ok && h->nlisted && h->nb == 0) { ok = bad(error, path, "every band is off"); }
  for (guint i = 0; ok && i < h->nb; i++) {
    for (guint k = 0; ok && k < i; k++) {
      if (h->band[i].worker[0] && strcmp(h->band[i].worker, h->band[k].worker) == 0 &&
          h->band[i].rate != h->band[k].rate) {
        ok = bad(error, path, "bands %s and %s: one worker (@%s) takes one rate — they come "
                 "at %u and %u Hz", h->band[k].name, h->band[i].name, h->band[i].worker,
                 h->band[k].rate, h->band[i].rate);
      }
    }
  }
  /* 384 kHz: the board's ARM reads and sends 4 receivers an IP with room
   * to spare (docs/REDPITAYA.md, F0 and agent 0.3) — more is not measured */
  for (guint ip = 0; ok && ip < 2 && h->source == SKIM_HD_SOURCE_HPSDR_P1; ip++) {
    const guint r = ip ? h->rp_b_rate : h->rate;
    if (r == 384000 && h->nb_ip[ip] > SKIM_HD_IP_RX_384) {
      ok = bad(error, path, "IP %c at 384 kHz: %u receivers at most (%u asked) — the Red Pitaya's "
               "ARM; 192 kHz takes %u", ip ? 'B' : 'A', SKIM_HD_IP_RX_384, h->nb_ip[ip], SKIM_HD_IP_RX);
    }
  }
  if (ok && h->nb_ip[1] && !h->nb_ip[0]) {
    ok = bad(error, path, "every band is ip=b — then IP B is the only one: put its address in "
             "[radio] host and drop ip=b");
  }
  /* [js8] bands: each a band received */
  for (char **x = h->js8_bands; ok && x && *x; x++) {
    gboolean found = FALSE;
    for (guint i = 0; i < h->nb && !found; i++) { found = strcmp(h->band[i].name, *x) == 0; }
    if (!found && h->source != SKIM_HD_SOURCE_HUB) {
      ok = bad(error, path, "[js8] bands: \"%s\" is no band received ([bands])", *x);
    }
  }
  /* [ft8] bands: each a band received */
  for (char **x = h->ft8_bands; ok && x && *x; x++) {
    gboolean found = FALSE;
    for (guint i = 0; i < h->nb && !found; i++) { found = strcmp(h->band[i].name, *x) == 0; }
    if (!found && h->source != SKIM_HD_SOURCE_HUB) {
      ok = bad(error, path, "[ft8] bands: \"%s\" is no band received ([bands])", *x);
    }
  }
  for (guint i = 0; ok && i < h->nb; i++) {
    if (h->band[i].worker[0] && h->source == SKIM_HD_SOURCE_HUB) {
      ok = bad(error, path, "band %s: a worker cannot pass a band on (@%s)", h->band[i].name,
               h->band[i].worker);
    } else if (h->band[i].worker[0] && h->hub_port <= 0) {
      ok = bad(error, path, "band %s: @%s needs the hub server ([hub] port is 0)",
               h->band[i].name, h->band[i].worker);
    }
  }
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
  "radio.type", "radio.host", "radio.rate", "radio.clock_ppm", "rp.mode", "rp.b_host", "rp.b_rate",
  "bands",
  "decode.engine", "decode.log", "decode.suppress_cw_on_rtty", "decode.rtty_adaptive",
  "decode.rtty_adaptive_spots", "decode.rtty_adaptive_minutes", "decode.v4_workers", "decode.v4_guard",
  "feed.call", "feed.port", "feed.rtty", "feed.address",
  "rbn.enabled", "rbn.port", "rbn.delay_s",
  "peers.hosts", "hub.name", "hub.port", "station.locator",
  "status.console_s", "status.http_port",
  "record.bands", "record.minutes", "record.dir", "record.start_utc",
  "archive.dir", "visitors.login", "visitors.free_from", "thermal.enabled", "thermal.limit_c", "thermal.average_min",
  "thermal.restore_margin_c", "thermal.probe_every_min", "thermal.probe_min",
  "filter.call", "filter.regex", "filter.mycall", "filter.mycall_diff", "filter.bad",
  "rtty_palette.open_on_spot", "rtty_palette.bandwidth", "rtty_palette.autotune", "rtty_palette.afc",
  "rtty_palette.afc_max_hz", "rtty_palette.afc_rate", "rtty_palette.reverse", "rtty_palette.usos",
  "rtty_palette.squelch_db", "rtty_palette.second_decoder", "rtty_palette.clean", "rtty_palette.sound",
  "wspr.enabled", "wspr.upload", "wspr.call", "wspr.locator", "wspr.deep", "wspr.subtract",
  "ft8.enabled", "ft8.ft8", "ft8.ft4", "ft8.feed", "ft8.depth", "ft8.lo_hz", "ft8.hi_hz",
  "ft8.threads", "ft8.bands", "ft8.upload", "ft8.call", "ft8.locator",
  "js8.enabled", "js8.submodes", "js8.feed", "js8.lo_hz", "js8.hi_hz", "js8.threads",
  "js8.bands", "js8.serve", "js8.clean", "js8.upload",
  "update.mode", "update.hour_utc",
  NULL,
};

char *skim_hd_config_rtty_palette_json(const SkimHdConfig *c) {
  const char *const T = "true", *const F = "false";
#define B(x) (c->rtty_palette.x ? T : F)
  return g_strdup_printf("{\"open_on_spot\":%s,\"bandwidth\":%u,\"autotune\":%s,\"afc\":%s,"
                         "\"afc_max_hz\":%u,\"afc_rate\":%u,\"reverse\":%s,\"usos\":%s,"
                         "\"squelch_db\":%u,\"second_decoder\":%s,\"clean\":%s,\"sound\":%s}",
                         B(open_on_spot), c->rtty_palette.bandwidth, B(autotune), B(afc),
                         c->rtty_palette.afc_max_hz, c->rtty_palette.afc_rate, B(reverse), B(usos),
                         c->rtty_palette.squelch_db, B(second_decoder), B(clean), B(sound));
#undef B
}

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
      /* a call and a locator go into the file in capitals */
      char *v = strcmp(key, "feed.call") == 0 || strcmp(key, "station.locator") == 0 ||
                        strcmp(key, "wspr.call") == 0 || strcmp(key, "wspr.locator") == 0 ||
                        strcmp(key, "ft8.call") == 0 || strcmp(key, "ft8.locator") == 0
                    ? g_ascii_strup(val, -1) : g_strdup(val);
      g_key_file_set_value(kf, group, strchr(key, '.') + 1, v);
      g_free(v);
      g_free(group);
    }
  }
  char *out = ok ? g_key_file_to_data(kf, NULL, NULL) : NULL;
  g_key_file_free(kf);
  return out;
}

char *skim_hd_config_import(const char *cur, gsize cur_len, const char *in_text, gsize in_len,
                            GError **error) {
  GKeyFile *in = g_key_file_new(), *now = g_key_file_new();
  char *out = NULL;
  if (!g_key_file_load_from_data(in, in_text, in_len, G_KEY_FILE_KEEP_COMMENTS, error)) {
    g_prefix_error(error, "the imported file: ");
  } else {
    g_key_file_load_from_data(now, cur, cur_len, G_KEY_FILE_KEEP_COMMENTS, NULL);
    g_key_file_remove_group(in, "web", NULL);
    gsize n = 0;
    char **keys = g_key_file_get_keys(now, "web", &n, NULL);
    for (gsize i = 0; keys && i < n; i++) {
      char *v = g_key_file_get_value(now, "web", keys[i], NULL);
      g_key_file_set_value(in, "web", keys[i], v);
      g_free(v);
    }
    g_strfreev(keys);
    out = g_key_file_to_data(in, NULL, NULL);
  }
  g_key_file_free(in);
  g_key_file_free(now);
  return out;
}

/* ---- hub and worker ------------------------------------------------------------- */

/* [feed] keys a hub hands its workers: the policy, not its port or its logs */
static const char *const FEED_POLICY[] = {
  "call", "min_score", "min_hearings", "settle_s", "settle_nodict_s", "fresh_s", "fold_s",
  "rtty", "gate_mode", NULL,
};

char *skim_hd_config_worker_fragment(const char *hub_text, gsize len, const char *worker) {
  SkimHdConfig c;
  if (!skim_hd_config_parse(&c, hub_text, len, "hub", NULL)) { return NULL; }
  GKeyFile *in = g_key_file_new();
  g_key_file_load_from_data(in, hub_text, len, G_KEY_FILE_NONE, NULL);
  GString *o = g_string_new(NULL);
  guint n = 0;
  guint wrate = c.rate;                         /* its bands' one rate (parse checks) */
  for (guint i = 0; i < c.nb; i++) {
    if (g_strcmp0(c.band[i].worker, worker) == 0) { wrate = c.band[i].rate; break; }
  }
  g_string_append_printf(o, "# from the hub %s for the worker %s\n[radio]\nrate=%u\n[bands]\n",
                         c.hub_name, worker, wrate);
  for (guint i = 0; i < c.nb; i++) {
    SkimHdBand b = c.band[i];
    if (g_strcmp0(b.worker, worker) != 0) { continue; }
    b.worker[0] = '\0';
    b.ip = b.input = 0;                       /* the board's layout stays at the hub */
    b.phased = FALSE;
    char *m = skim_hd_band_modes(&b, c.engine);
    g_string_append_printf(o, "%s=%.0f %s\n", b.name, b.centre_hz, m);
    g_free(m);
    n++;
  }
  g_string_append(o, "[decode]\n");
  gsize nk = 0;
  char **keys = g_key_file_get_keys(in, "decode", &nk, NULL);
  for (gsize i = 0; keys && i < nk; i++) {
    char *v = g_key_file_get_value(in, "decode", keys[i], NULL);
    g_string_append_printf(o, "%s=%s\n", keys[i], g_strstrip(v));
    g_free(v);
  }
  g_strfreev(keys);
  g_string_append(o, "[feed]\n");
  for (const char *const *k = FEED_POLICY; *k; k++) {
    char *v = g_key_file_get_value(in, "feed", *k, NULL);
    if (v) { g_string_append_printf(o, "%s=%s\n", *k, g_strstrip(v)); }
    g_free(v);
  }
  g_key_file_free(in);
  skim_hd_config_clear(&c);
  if (!n) {
    g_string_free(o, TRUE);
    return NULL;
  }
  return g_string_free(o, FALSE);
}

char *skim_hd_config_worker_merge(const char *own, gsize own_len, const char *frag,
                                  gsize frag_len, GError **error) {
  GKeyFile *kf = g_key_file_new(), *fr = g_key_file_new();
  char *out = NULL;
  if (!g_key_file_load_from_data(kf, own, own_len, G_KEY_FILE_KEEP_COMMENTS, error) ||
      !g_key_file_load_from_data(fr, frag, frag_len, G_KEY_FILE_NONE, error)) {
    g_prefix_error(error, "worker config: ");
  } else {
    g_key_file_remove_group(kf, "bands", NULL);
    g_key_file_remove_group(kf, "decode", NULL);
    static const char *const GROUPS[] = { "bands", "decode", "feed", "radio" };
    for (guint g = 0; g < G_N_ELEMENTS(GROUPS); g++) {
      gsize n = 0;
      char **keys = g_key_file_get_keys(fr, GROUPS[g], &n, NULL);
      for (gsize i = 0; keys && i < n; i++) {
        if (g == 3 && strcmp(keys[i], "rate") != 0) { continue; }   /* [radio]: rate only */
        char *v = g_key_file_get_value(fr, GROUPS[g], keys[i], NULL);
        g_key_file_set_value(kf, GROUPS[g], keys[i], v);
        g_free(v);
      }
      g_strfreev(keys);
    }
    out = g_key_file_to_data(kf, NULL, NULL);
  }
  g_key_file_free(kf);
  g_key_file_free(fr);
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
