/*
 * skimmer-config-test — headless.ini parsing (src/headless/config.h).
 *
 * The default ini parses to the defaults; a band names its modes (cw,
 * cw:deepcw, rtty) or none (cw with [decode] engine); host@ppm lists keep
 * each radio's clock; 0 in the file switches a feed timer off; and every
 * malformed value stops the parse with a message that names it — the same
 * call a settings page validates with before it writes the file.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "engine/combiner.h"
#include "headless/config.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* the known calls of the own call's filter case */
static gboolean known_test(const char *k) {
  return !strcmp(k, "OK1BR") || !strcmp(k, "OK1RR") || !strcmp(k, "OK1HRA");
}

static gboolean parse(SkimHdConfig *c, const char *text, GError **err) {
  return skim_hd_config_parse(c, text, strlen(text), "/etc/skim/test.ini", err);
}

int main(void) {
  printf("== the default ini\n");
  {
    char *dir = g_dir_make_tmp("skim-config-XXXXXX", NULL);
    char *path = g_build_filename(dir, "sub", "headless.ini", NULL);
    GError *err = NULL;
    check("written (directory created)", skim_hd_config_write_default(path, "#   192.0.2.7  R_PITAYA, 8 RX, idle\n", &err));
    SkimHdConfig c;
    const gboolean ok = skim_hd_config_load(&c, path, &err);
    check("parses", ok);
    if (!ok) { printf("       (%s)\n", err->message); }
    check("a Red Pitaya at 96 kHz, found on the LAN (host=auto)",
          ok && c.source == SKIM_HD_SOURCE_HPSDR_P1 && c.rate == 96000 &&
          g_strv_length(c.hosts) == 1 && strcmp(c.hosts[0], "auto") == 0);
    check("eight bands on (160 … 10 m), CW v3 and RTTY on each; 12 m listed, off",
          ok && c.nb == 8 && c.nlisted == 9 && c.listed[8].off &&
          strcmp(c.listed[8].name, "12m") == 0 && c.listed[8].rtty && c.band[0].cw && c.band[0].rtty && c.band[7].rtty &&
          c.band[0].cw_engine == SKIM_CW_ENGINE_V3 && strcmp(c.band[2].name, "40m") == 0 &&
          c.band[2].centre_hz == 7045500 && strcmp(c.band[7].name, "10m") == 0);
    {
      char *text = NULL;
      g_file_get_contents(path, &text, NULL, NULL);
      check("the radios found on the LAN noted in it, and how to find them",
            text && strstr(text, "#   192.0.2.7  R_PITAYA") && strstr(text, "--find-radios") &&
            strstr(text, "clock_ppm = D / F * 1000"));
      g_free(text);
    }
    check("feed on 7300 with no call, the policy left to the pipeline",
          ok && c.feed_port == 7300 && c.feed_call && !c.feed_call[0] &&
          c.feed_min_score == 0 && c.feed_fresh_s == 0 && !c.feed_gate && !c.feed_gate_mode);
    check("status 10 s / 8073, no recording",
          ok && c.console_s == 10 && c.http_port == 8073 && !c.rec_bands);
    check("RTTY spots go to the feed unless told otherwise", ok && c.feed_rtty);
    check("the RBN port off, 7550, no delay", ok && !c.rbn_on && c.rbn_port == 7550 &&
          c.rbn_delay_s == 0);
    check("CW is not kept off the RTTY pairs unless told to", ok && !c.suppress_cw_on_rtty);
    skim_hd_config_clear(&c);
    g_remove(path);
    char *sub = g_path_get_dirname(path);
    g_rmdir(sub);
    g_rmdir(dir);
    g_free(sub);
    g_free(path);
    g_free(dir);
  }

  printf("== CW v4\n");
  {
    SkimHdConfig c;
    GError *err = NULL;
    gboolean ok = parse(&c, "[decode]\nengine=v4\nv4_workers=3\n"
                            "[bands]\n40m=7045500\n20m=14045500 cw:v3\n", &err);
    check("engine=v4, v4_workers=3 parse", ok);
    check("a band with no modes runs v4, cw:v3 stays v3",
          ok && c.band[0].cw_engine == SKIM_CW_ENGINE_V4 && c.band[1].cw_engine == SKIM_CW_ENGINE_V3);
    check("v4_workers 3, v4_guard on by default", ok && c.v4_workers == 3 && c.v4_guard);
    char *m = ok ? skim_hd_band_modes(&c.band[1], SKIM_CW_ENGINE_V3) : NULL;
    char *m4 = ok ? skim_hd_band_modes(&c.band[0], SKIM_CW_ENGINE_V3) : NULL;
    check("written back: cw:v4 against a v3 default", m4 && strcmp(m4, "cw:v4") == 0 && m && strcmp(m, "cw") == 0);
    g_free(m); g_free(m4);
    g_clear_error(&err);
    ok = parse(&c, "[decode]\nv4_guard=false\n[bands]\n40m=7045500 cw:v4\n", NULL);
    check("v4_guard=false, cw:v4 per band", ok && !c.v4_guard && c.band[0].cw_engine == SKIM_CW_ENGINE_V4);
    check("v4_workers 65 is refused", !parse(&c, "[decode]\nv4_workers=65\n[bands]\n40m=7045500\n", NULL));
    check("engine=v5 is refused, the message names v4",
          !parse(&c, "[decode]\nengine=v5\n[bands]\n40m=7045500\n", &err) && err && strstr(err->message, "v4"));
    g_clear_error(&err);
  }

  printf("== bands and modes\n");
  {
    SkimHdConfig c;
    GError *err = NULL;
    const gboolean ok = parse(&c,
        "[decode]\nengine=deepcw\n"
        "[bands]\n160m=1845500\n40m = 7045500 cw,rtty\n20m=14045500 cw:v2, rtty\n"
        "15m=21045500 rtty\n17m=18113500 cw:deepcw\n", &err);
    check("parses", ok);
    if (!ok) { printf("       (%s)\n", err->message); }
    check("no modes: CW with [decode] engine (deepcw)",
          ok && c.band[0].cw && !c.band[0].rtty && c.band[0].cw_engine == SKIM_CW_ENGINE_DEEPCW);
    check("cw,rtty: both, the default engine",
          ok && c.band[1].cw && c.band[1].rtty && c.band[1].centre_hz == 7045500 &&
          c.band[1].cw_engine == SKIM_CW_ENGINE_DEEPCW);
    check("cw:v2, rtty (space after the comma): v2 and RTTY",
          ok && c.band[2].cw && c.band[2].rtty && c.band[2].cw_engine == SKIM_CW_ENGINE_V2);
    check("rtty alone: no CW", ok && !c.band[3].cw && c.band[3].rtty);
    check("file order is receiver order", ok && c.nb == 5 && strcmp(c.band[4].name, "17m") == 0);
    char *m1 = ok ? skim_hd_band_modes(&c.band[2], c.engine) : NULL;
    char *m2 = ok ? skim_hd_band_modes(&c.band[0], c.engine) : NULL;
    check("modes written back: \"cw:v2,rtty\" and \"cw\"",
          m1 && strcmp(m1, "cw:v2,rtty") == 0 && m2 && strcmp(m2, "cw") == 0);
    g_free(m1);
    g_free(m2);
    skim_hd_config_clear(&c);
  }

  printf("== bands kept but off\n");
  {
    SkimHdConfig c;
    GError *err = NULL;
    const gboolean ok = parse(&c,
        "[bands]\n160m=1845500 cw,off\n80m=3545500\n12m=24915500 off\n"
        "a=1900000\nb=3600000\nc=7000000\nd=10100000\ne=14000000\nf=18100000\n"
        "g=21000000\n", &err);
    check("parses: eight on, three off (more lines than receivers)", ok && c.nb == 8 &&
          c.nlisted == 10);
    if (!ok) { printf("       (%s)\n", err->message); }
    check("the receivers skip the bands off (80m is RX1)",
          ok && strcmp(c.band[0].name, "80m") == 0 && strcmp(c.band[1].name, "a") == 0);
    check("listed keeps file order, off marked; \"off\" alone is CW",
          ok && strcmp(c.listed[0].name, "160m") == 0 && c.listed[0].off &&
          c.listed[2].off && c.listed[2].cw && !c.listed[1].off);
    char *m = ok ? skim_hd_band_modes(&c.listed[0], c.engine) : NULL;
    check("modes written back: \"cw,off\"", m && strcmp(m, "cw,off") == 0);
    g_free(m);
    skim_hd_config_clear(&c);
    g_clear_error(&err);
    check("refused: every band off",
          !parse(&c, "[bands]\n20m=14045500 off\n", &err) && err &&
          strstr(err->message, "every band is off"));
    g_clear_error(&err);
  }

  printf("== hub and worker\n");
  {
    static const char HUB[] =
        "[radio]\nrate=96000\n[bands]\n40m=7045500 cw,rtty\n20m=14045500 cw,rtty @pc2\n"
        "15m=21045500 cw:v2 @pc2\n10m=28045500 @pc3,off\n[decode]\nengine=v3\nsuppress_cw_on_rtty=true\n"
        "[feed]\ncall=OK1HRA\nport=7300\nmin_score=0.85\nlearn=true\n[hub]\nname=central\n";
    SkimHdConfig c;
    GError *err = NULL;
    const gboolean ok = parse(&c, HUB, &err);
    check("a hub: @pc2 on two bands, 40m here", ok && c.nb == 3 && !c.band[0].worker[0] &&
          strcmp(c.band[1].worker, "pc2") == 0 && c.band[1].rtty &&
          strcmp(c.band[2].worker, "pc2") == 0 && c.hub_port == 7380 &&
          strcmp(c.hub_name, "central") == 0 && g_strv_length(c.hub_allow) >= 4);
    if (!ok) { printf("       (%s)\n", err->message); }
    char *m = ok ? skim_hd_band_modes(&c.band[1], c.engine) : NULL;
    check("modes written back: \"cw,rtty @pc2\"", m && strcmp(m, "cw,rtty @pc2") == 0);
    g_free(m);
    skim_hd_config_clear(&c);
    g_clear_error(&err);
    char *frag = skim_hd_config_worker_fragment(HUB, strlen(HUB), "pc2");
    check("pc2's fragment: its two bands, no @, the rate, decode and the feed policy only",
          frag && strstr(frag, "20m=14045500 cw,rtty\n") && strstr(frag, "15m=21045500 cw:v2\n") &&
          !strstr(frag, "40m") && !strstr(frag, "@") && strstr(frag, "rate=96000") &&
          strstr(frag, "suppress_cw_on_rtty=true") && strstr(frag, "min_score=0.85") &&
          strstr(frag, "call=OK1HRA") && !strstr(frag, "port=") && !strstr(frag, "learn"));
    check("pc3 (its only band off) and an unknown worker get nothing",
          !skim_hd_config_worker_fragment(HUB, strlen(HUB), "pc3") &&
          !skim_hd_config_worker_fragment(HUB, strlen(HUB), "pc9"));
    static const char OWN[] =
        "# pc2\n[radio]\ntype=hub\nhost=auto\n[feed]\nport=7301\nmin_score=0.99\n"
        "[status]\nhttp_port=8074\n[hub]\nname=pc2\n";
    check("a worker's own ini parses without [bands]", parse(&c, OWN, &err) && c.nb == 0 &&
          c.source == SKIM_HD_SOURCE_HUB && strcmp(c.hub_name, "pc2") == 0);
    skim_hd_config_clear(&c);
    g_clear_error(&err);
    char *merged = frag ? skim_hd_config_worker_merge(OWN, strlen(OWN), frag, strlen(frag), &err) : NULL;
    const gboolean mok = merged && parse(&c, merged, &err);
    check("merged: the hub's bands and policy, the worker's own type, port, page and name",
          mok && c.nb == 2 && c.source == SKIM_HD_SOURCE_HUB && c.feed_port == 7301 &&
          c.feed_min_score == 0.85 && c.suppress_cw_on_rtty && c.http_port == 8074 &&
          strcmp(c.hub_name, "pc2") == 0 && strcmp(c.feed_call, "OK1HRA") == 0 &&
          c.band[1].cw_engine == SKIM_CW_ENGINE_V2 && strstr(merged, "# pc2"));
    if (!mok) { printf("       (%s)\n", err ? err->message : "?"); }
    if (mok) { skim_hd_config_clear(&c); }
    g_clear_error(&err);
    g_free(merged);
    g_free(frag);
    check("refused: @worker on a worker",
          !parse(&c, "[radio]\ntype=hub\n[bands]\n20m=14045500 @pc3\n", &err) &&
          strstr(err->message, "cannot pass"));
    g_clear_error(&err);
    check("refused: @worker with the hub server off",
          !parse(&c, "[bands]\n20m=14045500 @pc3\n[hub]\nport=0\n", &err) &&
          strstr(err->message, "[hub] port is 0"));
    g_clear_error(&err);
    check("refused: a bad worker name",
          !parse(&c, "[bands]\n20m=14045500 @pc/3\n", &err) && strstr(err->message, "@pc/3"));
    g_clear_error(&err);
  }

  printf("== radio and feed\n");
  {
    SkimHdConfig c;
    GError *err = NULL;
    const gboolean ok = parse(&c,
        "[radio]\nhost=192.168.1.21@3.81, 192.168.1.71\nclock_ppm=-1.5\nrate=192000\n"
        "[bands]\n20m=14045500\n"
        "[feed]\ncall= OK1HRA \nport=7302\nmin_score=0.9\nsettle_s=0\nfresh_s=120\n"
        "gate=gates/g.ini\ngate_mode= on\n"
        "[record]\nbands=20m, 40m\nminutes=0\n", &err);
    check("parses", ok);
    if (!ok) { printf("       (%s)\n", err->message); }
    check("two radios in order, each with its clock",
          ok && g_strv_length(c.hosts) == 2 && strcmp(c.hosts[1], "192.168.1.71") == 0 &&
          c.host_ppm[0] == 3.81 && c.host_ppm[1] == -1.5 &&
          strcmp(c.hosts_text, "192.168.1.21, 192.168.1.71") == 0);
    check("rate 192 kHz", ok && c.rate == 192000);
    {
      SkimHdConfig a;
      static const char T[] = "[radio]\nhost=192.168.1.21@3.81, AUTO\nclock_ppm=1\n[bands]\n20m=14045500\n";
      GError *e = NULL;
      const gboolean aok = skim_hd_config_parse(&a, T, strlen(T), "/tmp/a.ini", &e);
      check("a named radio, then auto (at clock_ppm)",
            aok && g_strv_length(a.hosts) == 2 && strcmp(a.hosts[1], "auto") == 0 &&
            a.host_ppm[1] == 1.0);
      if (aok) { skim_hd_config_clear(&a); }
      g_clear_error(&e);
      static const char N[] = "[bands]\n20m=14045500\n";
      const gboolean nok = skim_hd_config_parse(&a, N, strlen(N), "/tmp/n.ini", &e);
      check("no host = auto", nok && strcmp(a.hosts[0], "auto") == 0);
      if (nok) { skim_hd_config_clear(&a); }
      g_clear_error(&e);
    }
    check("call trimmed, port, score", ok && strcmp(c.feed_call, "OK1HRA") == 0 &&
          c.feed_port == 7302 && c.feed_min_score == 0.9);
    check("settle_s=0 in the file = off (-1), fresh_s kept",
          ok && c.feed_settle_s < 0 && c.feed_fresh_s == 120 && c.feed_fold_s == 0);
    check("a relative gate resolves next to the ini; the mode trimmed",
          ok && strcmp(c.feed_gate, "/etc/skim/gates/g.ini") == 0 &&
          strcmp(c.feed_gate_mode, "on") == 0);
    check("record: both bands, minutes 0 → 10, the default dir",
          ok && g_strv_length(c.rec_bands) == 2 && strcmp(c.rec_bands[1], "40m") == 0 &&
          c.rec_minutes == 10 && strcmp(c.rec_dir, "/var/tmp/skimmer-iq") == 0);
    check("no rtty= in [feed]: RTTY on the feed", ok && c.feed_rtty);
    skim_hd_config_clear(&c);
    check("[feed] rtty=false keeps the feed CW only",
          parse(&c, "[bands]\n20m=14045500 cw,rtty\n[feed]\nrtty=false\n", NULL) &&
          !c.feed_rtty && c.band[0].rtty);
    skim_hd_config_clear(&c);
    check("[decode] suppress_cw_on_rtty=true",
          parse(&c, "[bands]\n20m=14045500 cw,rtty\n[decode]\nsuppress_cw_on_rtty=true\n", NULL) &&
          c.suppress_cw_on_rtty);
    skim_hd_config_clear(&c);
    check("[decode] adaptive RTTY off by default: 100 spots in 2 min",
          parse(&c, "[bands]\n20m=14045500 cw,rtty\n[decode]\nrtty_adaptive_spots=\n", NULL) &&
          !c.rtty_adaptive && c.rtty_adaptive_spots == 100 && c.rtty_adaptive_minutes == 2);
    skim_hd_config_clear(&c);
    check("[decode] rtty_adaptive=true, 50 spots in 3 min",
          parse(&c, "[bands]\n20m=14045500 cw,rtty\n[decode]\nrtty_adaptive=true\n"
                    "rtty_adaptive_spots=50\nrtty_adaptive_minutes=3\n", NULL) &&
          c.rtty_adaptive && c.rtty_adaptive_spots == 50 && c.rtty_adaptive_minutes == 3);
    skim_hd_config_clear(&c);
    check("[decode] rtty_adaptive_minutes refused out of 1–30, spots not whole",
          !parse(&c, "[bands]\n20m=14045500\n[decode]\nrtty_adaptive_minutes=0\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[decode]\nrtty_adaptive_minutes=31\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[decode]\nrtty_adaptive_spots=2.5\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[decode]\nrtty_adaptive_spots=many\n", NULL));
    skim_hd_config_clear(&c);
    check("[visitors] sign-in on by default, free from the private networks",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && c.visitors_login &&
          g_strv_contains((const char *const *)c.visitors_free, "192.168.0.0/16") &&
          g_strv_contains((const char *const *)c.visitors_free, "fc00::/7"));
    skim_hd_config_clear(&c);
    check("[visitors] off, free from one network",
          parse(&c, "[bands]\n20m=14045500\n[visitors]\nlogin=false\nfree_from=203.0.113.0/24\n", NULL) &&
          !c.visitors_login && g_strv_length(c.visitors_free) == 1);
    skim_hd_config_clear(&c);
    check("[visitors] free_from refused when it is no network",
          !parse(&c, "[bands]\n20m=14045500\n[visitors]\nfree_from=lan\n", NULL));
    check("[thermal] off by default: limit from the sensor, 5 min, margin 3, a test 5 min each 30",
          parse(&c, "[bands]\n20m=14045500\n[thermal]\nlimit_c=\n", NULL) && !c.thermal &&
          c.thermal_limit_c == 0 && c.thermal_average_min == 5 && c.thermal_margin_c == 3 &&
          c.thermal_probe_every_min == 30 && c.thermal_probe_min == 5);
    skim_hd_config_clear(&c);
    check("[thermal] probe_every_min=0 (no rotation), =60 with probe_min=10",
          parse(&c, "[bands]\n20m=14045500\n[thermal]\nprobe_every_min=0\n", NULL) &&
          c.thermal_probe_every_min == 0 &&
          parse(&c, "[bands]\n20m=14045500\n[thermal]\nprobe_every_min=60\nprobe_min=10\n", NULL) &&
          c.thermal_probe_every_min == 60 && c.thermal_probe_min == 10);
    skim_hd_config_clear(&c);
    check("[thermal] refused: probe_every_min=5, probe_min=0",
          !parse(&c, "[bands]\n20m=14045500\n[thermal]\nprobe_every_min=5\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[thermal]\nprobe_min=0\n", NULL));
    skim_hd_config_clear(&c);
    check("[thermal] on at 82.5 °C, 10 min, margin 2",
          parse(&c, "[bands]\n20m=14045500\n[thermal]\nenabled=true\nlimit_c=82.5\naverage_min=10\n"
                    "restore_margin_c=2\n", NULL) &&
          c.thermal && c.thermal_limit_c == 82.5 && c.thermal_average_min == 10 &&
          c.thermal_margin_c == 2);
    skim_hd_config_clear(&c);
    check("[thermal] refused: limit 30 °C, 120 °C, average 0 min",
          !parse(&c, "[bands]\n20m=14045500\n[thermal]\nlimit_c=30\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[thermal]\nlimit_c=120\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[thermal]\naverage_min=0\n", NULL));
    check("[wspr] default: off, no upload",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.wspr && !c.wspr_upload &&
          !c.wspr_call && !c.wspr_locator[0] && !c.wspr_deep && c.wspr_subtract);
    check("[wspr] deep=true, subtract=false read",
          parse(&c, "[bands]\n20m=14045500\n[wspr]\ndeep=true\nsubtract=false\n", NULL) &&
          c.wspr_deep && !c.wspr_subtract);
    skim_hd_config_clear(&c);
    check("[wspr] on, upload with [feed] call and [station] locator; call/locator in capitals",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=ok1hra\n[station]\nlocator=jo70\n"
                    "[wspr]\nenabled=true\nupload=true\n", NULL) && c.wspr && c.wspr_upload &&
          parse(&c, "[bands]\n20m=14045500\n[wspr]\nenabled=true\nupload=true\ncall=ok1hra/p\n"
                    "locator=jo70ab\n", NULL) &&
          g_strcmp0(c.wspr_call, "OK1HRA/P") == 0 && strcmp(c.wspr_locator, "JO70AB") == 0);
    skim_hd_config_clear(&c);
    check("[wspr] refused: an upload without a call or a locator, a bad call, a bad locator",
          !parse(&c, "[bands]\n20m=14045500\n[station]\nlocator=JO70\n[wspr]\nenabled=true\nupload=true\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[wspr]\nenabled=true\nupload=true\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[wspr]\ncall=OK-1\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[wspr]\nlocator=JO7\n", NULL));
    check("[ft8] default: off; FT8 + FT4, to the feed, depth 2, 200-3500 Hz, 2 threads",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.ft8 && c.ft8_ft8 && c.ft8_ft4 &&
          c.ft8_feed && c.ft8_depth == 2 && c.ft8_lo_hz == 200 && c.ft8_hi_hz == 3500 &&
          c.ft8_threads == 2);
    check("[ft8] blank values are the defaults (the web form's empty fields)",
          parse(&c, "[bands]\n20m=14045500\n[ft8]\nenabled=true\ndepth=\nlo_hz=\nhi_hz=\nthreads=\n", NULL) &&
          c.ft8 && c.ft8_depth == 2 && c.ft8_lo_hz == 200 && c.ft8_hi_hz == 3500 && c.ft8_threads == 2);
    check("[ft8] read: FT4 off, page only, depth 3, 300-3000 Hz, 4 threads",
          parse(&c, "[bands]\n20m=14045500\n[ft8]\nenabled=true\nft4=false\nfeed=false\ndepth=3\n"
                    "lo_hz=300\nhi_hz=3000\nthreads=4\n", NULL) &&
          c.ft8 && c.ft8_ft8 && !c.ft8_ft4 && !c.ft8_feed && c.ft8_depth == 3 && c.ft8_lo_hz == 300 &&
          c.ft8_hi_hz == 3000 && c.ft8_threads == 4);
    check("[ft8] PSK Reporter: off by default; on with [feed] call and [station] locator; "
          "call/locator in capitals; the gate's host:port",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.ft8_upload && !c.ft8_call &&
          !c.ft8_locator[0] && !c.ft8_pskr_host &&
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=ok1hra\n[station]\nlocator=jo70\n"
                    "[ft8]\nenabled=true\nupload=true\n", NULL) && c.ft8_upload &&
          parse(&c, "[bands]\n20m=14045500\n[ft8]\nenabled=true\nupload=true\ncall=ok1hra/p\n"
                    "locator=jo70ab\npskreporter=127.0.0.1:14739\n", NULL) &&
          g_strcmp0(c.ft8_call, "OK1HRA/P") == 0 && strcmp(c.ft8_locator, "JO70AB") == 0 &&
          g_strcmp0(c.ft8_pskr_host, "127.0.0.1") == 0 && c.ft8_pskr_port == 14739);
    skim_hd_config_clear(&c);
    check("[ft8] refused: an upload without a call or a locator, a bad call, locator, host:port",
          !parse(&c, "[bands]\n20m=14045500\n[station]\nlocator=JO70\n[ft8]\nenabled=true\nupload=true\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[ft8]\nenabled=true\nupload=true\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\ncall=OK-1\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nlocator=JO7\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\npskreporter=localhost\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\npskreporter=localhost:0\n", NULL));
    check("[ft8] bands: none = all (NULL); \"40m, 20m\" read; a band not received refused",
          parse(&c, "[bands]\n20m=14045500\n40m=7045500\n", NULL) && !c.ft8_bands &&
          parse(&c, "[bands]\n20m=14045500\n40m=7045500\n[ft8]\nbands=40m, 20m\n", NULL) &&
          c.ft8_bands && g_strv_length(c.ft8_bands) == 2 && strcmp(c.ft8_bands[0], "40m") == 0 &&
          strcmp(c.ft8_bands[1], "20m") == 0 &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nbands=40m\n", NULL));
    skim_hd_config_clear(&c);
    check("[ft8] the narrowest window, 1000-2000 Hz, one thread",
          parse(&c, "[bands]\n20m=14045500\n[ft8]\nlo_hz=1000\nhi_hz=2000\nthreads=1\n", NULL) &&
          c.ft8_lo_hz == 1000 && c.ft8_hi_hz == 2000 && c.ft8_threads == 1);
    check("[ft8] refused: depth 4, lo 100 (no room before the dial), lo 1100, hi 1900, hi 5000, "
          "0 or 9 threads",
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\ndepth=4\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nlo_hz=100\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nlo_hz=1100\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nhi_hz=1900\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nhi_hz=5000\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nthreads=0\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[ft8]\nthreads=9\n", NULL));
    skim_hd_config_clear(&c);
    check("[js8] default: off; all four submodes, to the feed, 200-3500 Hz, 2 threads, kept "
          "running, clean from 0.80, no upload",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.js8 && c.js8_submodes == 0xf &&
          c.js8_feed && c.js8_lo_hz == 200 && c.js8_hi_hz == 3500 && c.js8_threads == 2 &&
          c.js8_serve && fabs(c.js8_clean - 0.80) < 1e-9 && !c.js8_upload && !c.js8_bands);
    check("[js8] blank values are the defaults (the web form's empty fields)",
          parse(&c, "[bands]\n20m=14045500\n[js8]\nenabled=true\nsubmodes=\nlo_hz=\nhi_hz=\n"
                    "threads=\nclean=\n", NULL) &&
          c.js8 && c.js8_submodes == 0xf && c.js8_lo_hz == 200 && c.js8_threads == 2 &&
          fabs(c.js8_clean - 0.80) < 1e-9);
    check("[js8] read: normal + slow, page only, 300-3000 Hz, 4 threads, once a period, clean 0.9",
          parse(&c, "[bands]\n20m=14045500\n40m=7045500\n[js8]\nenabled=true\nsubmodes=Normal, slow\n"
                    "feed=false\nlo_hz=300\nhi_hz=3000\nthreads=4\nserve=false\nclean=0.9\n"
                    "bands=40m\n", NULL) &&
          c.js8 && c.js8_submodes == 0x9 && !c.js8_feed && c.js8_lo_hz == 300 && c.js8_hi_hz == 3000 &&
          c.js8_threads == 4 && !c.js8_serve && fabs(c.js8_clean - 0.9) < 1e-9 && c.js8_bands &&
          g_strv_length(c.js8_bands) == 1 && strcmp(c.js8_bands[0], "40m") == 0);
    skim_hd_config_clear(&c);
    check("[js8] refused: ultra, no submode, clean 0.4, lo 100, 9 threads, a band not received, "
          "an upload without a call",
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nsubmodes=normal,ultra\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nsubmodes=,\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nclean=0.4\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nlo_hz=100\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nthreads=9\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[js8]\nbands=40m\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[station]\nlocator=JO70\n[js8]\nenabled=true\nupload=true\n", NULL));
    check("[js8] upload: under [ft8] call/locator, else [feed] call and [station] locator",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=ok1hra\n[station]\nlocator=jo70\n"
                    "[js8]\nenabled=true\nupload=true\n", NULL) && c.js8_upload &&
          parse(&c, "[bands]\n20m=14045500\n[ft8]\ncall=ok1hra\nlocator=jo70\n"
                    "[js8]\nenabled=true\nupload=true\n", NULL));
    skim_hd_config_clear(&c);
    check("[rtty_palette] defaults: on a spot, 400 Hz, autotune, AFC 60/60, squelch off, sound",
          parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nbandwidth=\n", NULL) &&
          c.rtty_palette.open_on_spot && c.rtty_palette.bandwidth == 400 && c.rtty_palette.autotune &&
          c.rtty_palette.afc && c.rtty_palette.afc_max_hz == 60 && c.rtty_palette.afc_rate == 60 &&
          !c.rtty_palette.reverse && c.rtty_palette.usos && c.rtty_palette.squelch_db == 0 &&
          c.rtty_palette.second_decoder && !c.rtty_palette.clean && c.rtty_palette.sound);
    {
      char *j = skim_hd_config_rtty_palette_json(&c);
      check("[rtty_palette] as JSON for the page",
            strcmp(j, "{\"open_on_spot\":true,\"bandwidth\":400,\"autotune\":true,\"afc\":true,"
                      "\"afc_max_hz\":60,\"afc_rate\":60,\"reverse\":false,\"usos\":true,"
                      "\"squelch_db\":0,\"second_decoder\":true,\"clean\":false,\"sound\":true}") == 0);
      g_free(j);
    }
    skim_hd_config_clear(&c);
    check("[rtty_palette] set: 300 Hz, AFC 120 Hz at 20, squelch 3, reverse, clean, silent",
          parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nopen_on_spot=false\nbandwidth=300\n"
                    "afc_max_hz=120\nafc_rate=20\nsquelch_db=3\nreverse=true\nclean=true\nsound=false\n",
                NULL) &&
          !c.rtty_palette.open_on_spot && c.rtty_palette.bandwidth == 300 &&
          c.rtty_palette.afc_max_hz == 120 && c.rtty_palette.afc_rate == 20 &&
          c.rtty_palette.squelch_db == 3 && c.rtty_palette.reverse && c.rtty_palette.clean &&
          !c.rtty_palette.sound);
    skim_hd_config_clear(&c);
    check("[rtty_palette] refused: 200 Hz, AFC 200 Hz, squelch 11, sound=maybe",
          !parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nbandwidth=200\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nafc_max_hz=200\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nsquelch_db=11\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[rtty_palette]\nsound=maybe\n", NULL));
    check("[feed] address: none by default, empty = none",
          parse(&c, "[bands]\n20m=14045500\n[feed]\naddress=\n", NULL) && !c.feed_address);
    skim_hd_config_clear(&c);
    check("[feed] address host:port kept, a telnet:// URL too",
          parse(&c, "[bands]\n20m=14045500\n[feed]\naddress= skimmer.example.net:7300 \n", NULL) &&
          strcmp(c.feed_address, "skimmer.example.net:7300") == 0);
    skim_hd_config_clear(&c);
    check("… telnet://203.0.113.5:7300",
          parse(&c, "[bands]\n20m=14045500\n[feed]\naddress=telnet://203.0.113.5:7300\n", NULL) &&
          strcmp(c.feed_address, "telnet://203.0.113.5:7300") == 0);
    skim_hd_config_clear(&c);
    check("[feed] address refused: a space, a quote, javascript:",
          !parse(&c, "[bands]\n20m=14045500\n[feed]\naddress=a b:7300\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[feed]\naddress=x\"y\n", NULL) &&
          !parse(&c, "[bands]\n20m=14045500\n[feed]\naddress=javascript://x\n", NULL));
    check("[peers] two feeds, one with its port",
          parse(&c, "[bands]\n20m=14045500\n[peers]\nhosts=192.168.1.30, pc2:7302\n", NULL) &&
          g_strv_length(c.peers) == 2 && strcmp(c.peers[1], "pc2:7302") == 0);
    skim_hd_config_clear(&c);
    check("[station] locator jn18eu → JN18EU",
          parse(&c, "[bands]\n20m=14045500\n[station]\nlocator=jn18eu\n", NULL) &&
          strcmp(c.locator, "JN18EU") == 0);
    skim_hd_config_clear(&c);
    check("[station] locator refused when it is none (JO6)",
          !parse(&c, "[bands]\n20m=14045500\n[station]\nlocator=JO6\n", NULL));
    check("[rbn] on, port 7551, held 99 s",
          parse(&c, "[bands]\n20m=14045500\n[rbn]\nenabled=true\nport=7551\ndelay_s=99\n", NULL) &&
          c.rbn_on && c.rbn_port == 7551 && c.rbn_delay_s == 99);
    skim_hd_config_clear(&c);
    check("type=mock is the synthetic source",
          parse(&c, "[radio]\ntype=mock\n[bands]\n20m=14045500\n", NULL) &&
          c.source == SKIM_HD_SOURCE_MOCK);
    skim_hd_config_clear(&c);
  }

  printf("== the spot archive\n");
  {
    SkimHdConfig c;
    check("none: off", parse(&c, "[bands]\n20m=14045500\n[archive]\ndir=\n", NULL) && !c.archive_dir);
    skim_hd_config_clear(&c);
    char *home = g_build_filename(g_get_home_dir(), "spots", NULL);
    check("~/spots: in the home", parse(&c, "[bands]\n20m=14045500\n[archive]\ndir=~/spots\n", NULL) &&
          c.archive_dir && strcmp(c.archive_dir, home) == 0);
    skim_hd_config_clear(&c);
    g_free(home);
    check("a relative path: next to the config file",
          parse(&c, "[bands]\n20m=14045500\n[archive]\ndir=spots/../log\n", NULL) &&
          c.archive_dir && strcmp(c.archive_dir, "/etc/skim/log") == 0);
    skim_hd_config_clear(&c);
    check("an absolute path as it is",
          parse(&c, "[bands]\n20m=14045500\n[archive]\ndir=/srv/spots/\n", NULL) &&
          c.archive_dir && strcmp(c.archive_dir, "/srv/spots") == 0);
    skim_hd_config_clear(&c);
  }

  printf("== the callsign filter\n");
  {
    SkimHdConfig c;
    check("none: every call passes",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.filter_call &&
          skim_hd_config_call_pass(&c, "RA3AA"));
    skim_hd_config_clear(&c);
    check("plain text: part of a call, any case",
          parse(&c, "[bands]\n20m=14045500\n[filter]\ncall=ok1\n", NULL) &&
          skim_hd_config_call_pass(&c, "OK1HRA") && !skim_hd_config_call_pass(&c, "DL1ABC"));
    skim_hd_config_clear(&c);
    /* IP-rotator's default DX filter: every call but the Russian and Belarusian ones */
    const gboolean ok = parse(&c, "[bands]\n20m=14045500\n[filter]\nregex=true\n"
                              "call=^(?!^(?:R[A-Z]?|U[A-I0]|E[UVW])[0-9]{1,3}[A-Z]{1,4}(?:/[A-Z0-9]+)?$).+\n",
                              NULL);
    gboolean pass = ok, drop = ok;
    for (const char *const *k = (const char *const[]){ "OK1HRA", "DL1ABC", "UR5A", "K1AR", "ok1hra", NULL }; ok && *k; k++) {
      pass = pass && skim_hd_config_call_pass(&c, *k);
    }
    for (const char *const *k = (const char *const[]){ "RA3AA", "R7AB", "UA9XYZ", "UI8A", "EW1AB", "EU7A", "RK3A/P", NULL }; ok && *k; k++) {
      drop = drop && !skim_hd_config_call_pass(&c, *k);
    }
    check("IP-rotator's default regex parses (lookahead, backslash-free, raw value)", ok);
    check("… it passes OK1HRA DL1ABC UR5A K1AR (any case)", pass);
    check("… it drops RA3AA R7AB UA9XYZ UI8A EW1AB EU7A RK3A/P", drop);
    skim_hd_config_clear(&c);
    check("a backslash stays in the expression (\\d)",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nregex=true\ncall=^OK\\d\n", NULL) &&
          skim_hd_config_call_pass(&c, "OK1HRA") && !skim_hd_config_call_pass(&c, "OKAB"));
    skim_hd_config_clear(&c);
    /* the own call's filter: [feed] call and the calls within mycall_diff edits */
    check("changes place by place: OK1HRA–OK1HRB 1, –OK1HR (cut) 1, –OK1H 2, –OK2HRB 2, –OK1BR 2, "
          "–OK1RA 3, backwards ARH1KO 6, –DL1ABC 5",
          skim_hd_call_distance("OK1HRA", "OK1HRB") == 1 && skim_hd_call_distance("OK1HRA", "ok1hr") == 1 &&
          skim_hd_call_distance("OK1HRA", "OK1H") == 2 && skim_hd_call_distance("OK1HRA", "OK2HRB") == 2 &&
          skim_hd_call_distance("OK1HRA", "OK1BR") == 2 && skim_hd_call_distance("OK1HRA", "OK1RA") == 3 &&
          skim_hd_call_distance("OK1HRA", "ARH1KO") == 6 && skim_hd_call_distance("OK1HRA", "DL1ABC") == 5);
    check("mycall on: not in force without a [feed] call",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nmycall=true\n", NULL) &&
          !skim_hd_config_filter_on(&c) && skim_hd_config_call_pass(&c, "OK1HRA"));
    skim_hd_config_clear(&c);
    check("mycall on, OK1HRA, default 2: drops OK1HRA OK1HRB OK1HR OK2HRB OK1HRA/P (and OK1RR, OK1BR: 2 off), passes OK2ABC DL1ABC ARH1KO OK1RA",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[filter]\nmycall=true\n", NULL) &&
          c.filter_mycall_diff == 2 && skim_hd_config_filter_on(&c) &&
          !skim_hd_config_call_pass(&c, "OK1HRA") && !skim_hd_config_call_pass(&c, "OK1HRB") &&
          !skim_hd_config_call_pass(&c, "OK1HR") && !skim_hd_config_call_pass(&c, "OK2HRB") &&
          !skim_hd_config_call_pass(&c, "OK1HRA/P") && !skim_hd_config_call_pass(&c, "OK1RR") &&
          !skim_hd_config_call_pass(&c, "OK1BR") && skim_hd_config_call_pass(&c, "OK2ABC") &&
          skim_hd_config_call_pass(&c, "ARH1KO") && skim_hd_config_call_pass(&c, "OK1RA") &&
          skim_hd_config_call_pass(&c, "DL1ABC"));
    skim_hd_config_clear(&c);
    /* with the known calls: a real neighbour passes, a bust and the own call do not */
    skim_hd_config_set_known(known_test);
    check("known calls: OK1BR, OK1RR, OK1BR/P pass; OK1HRB, OK1HR stay out; OK1HRA (known) itself too",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[filter]\nmycall=true\n", NULL) &&
          skim_hd_config_call_pass(&c, "OK1BR") && skim_hd_config_call_pass(&c, "OK1RR") &&
          skim_hd_config_call_pass(&c, "OK1BR/P") && !skim_hd_config_call_pass(&c, "OK1HRB") &&
          !skim_hd_config_call_pass(&c, "OK1HR") && !skim_hd_config_call_pass(&c, "OK1HRA") &&
          !skim_hd_config_call_pass(&c, "OK1HRA/P"));
    skim_hd_config_clear(&c);
    skim_hd_config_set_known(NULL);
    check("mycall_diff=0: only the call itself (and with a /suffix)",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[filter]\nmycall=true\nmycall_diff=0\n", NULL) &&
          !skim_hd_config_call_pass(&c, "OK1HRA") && !skim_hd_config_call_pass(&c, "OK1HRA/P") &&
          skim_hd_config_call_pass(&c, "OK1HRB"));
    skim_hd_config_clear(&c);
    check("mycall with a pattern: both apply",
          parse(&c, "[bands]\n20m=14045500\n[feed]\ncall=OK1HRA\n[filter]\ncall=OK\nmycall=true\n", NULL) &&
          !skim_hd_config_call_pass(&c, "OK1HRB") && skim_hd_config_call_pass(&c, "OK2ABC") &&
          !skim_hd_config_call_pass(&c, "DL1ABC"));
    skim_hd_config_clear(&c);
    GError *err = NULL;
    check("mycall_diff past 3 is refused",
          !parse(&c, "[bands]\n20m=14045500\n[filter]\nmycall_diff=5\n", &err) && err &&
          strstr(err->message, "mycall_diff"));
    g_clear_error(&err);
    check("a bad expression is refused with the reason",
          !parse(&c, "[bands]\n20m=14045500\n[filter]\nregex=true\ncall=(OK\n", &err) &&
          err && strstr(err->message, "not a regular expression"));
    g_clear_error(&err);
  }

  printf("== refused, with the reason\n");
  {
    static const struct { const char *what, *text, *says; } BAD[] = {
      { "no band", "[radio]\nrate=96000\n", "no [bands]" },
      { "a centre that is no number", "[bands]\n20m=fourteen\n", "20m" },
      { "a centre out of range", "[bands]\n20m=14045\n", "not a centre" },
      { "an unknown mode", "[bands]\n20m=14045500 psk\n", "\"psk\"" },
      { "an unknown CW engine", "[bands]\n20m=14045500 cw:v9\n", "\"cw:v9\"" },
      { "nine bands", "[bands]\na=1900000\nb=3600000\nc=7000000\nd=10100000\n"
        "e=14000000\nf=18100000\ng=21000000\nh=24900000\ni=28000000\n", "8 receivers" },
      { "a rate the radio has not", "[radio]\nrate=44100\n[bands]\n20m=14045500\n", "44100" },
      { "a bad host@ppm", "[radio]\nhost=192.168.1.21@x\n[bands]\n20m=14045500\n",
        "192.168.1.21@x" },
      { "auto with a ppm", "[radio]\nhost=auto@3\n[bands]\n20m=14045500\n", "auto@" },
      { "an unknown radio type", "[radio]\ntype=soapy\n[bands]\n20m=14045500\n", "soapy" },
      { "an unknown [decode] engine", "[decode]\nengine=v9\n[bands]\n20m=14045500\n", "v9" },
      { "[feed] rtty that is no boolean", "[bands]\n20m=14045500\n[feed]\nrtty=maybe\n",
        "rtty: true or false" },
      { "[decode] suppress_cw_on_rtty that is no boolean",
        "[bands]\n20m=14045500\n[decode]\nsuppress_cw_on_rtty=1x\n",
        "[decode] suppress_cw_on_rtty: true or false" },
      { "an RBN delay past 99 s", "[bands]\n20m=14045500\n[rbn]\ndelay_s=100\n", "0 to 99" },
      { "an RBN delay that is no number", "[bands]\n20m=14045500\n[rbn]\ndelay_s=5s\n",
        "delay_s" },
      { "the RBN on the feed's port",
        "[bands]\n20m=14045500\n[feed]\nport=7300\n[rbn]\nenabled=true\nport=7300\n",
        "[feed] port too" },
      { "not an ini", "[bands\n20m=1\n", "test.ini" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(BAD); i++) {
      SkimHdConfig c;
      GError *err = NULL;
      const gboolean ok = parse(&c, BAD[i].text, &err);
      char what[160];
      g_snprintf(what, sizeof(what), "%s (\"%s\")", BAD[i].what,
                 err ? err->message : "no error");
      check(what, !ok && err && strstr(err->message, BAD[i].says) &&
                  strstr(err->message, "test.ini") && c.nb == 0 && !c.hosts);
      g_clear_error(&err);
    }
  }

  printf("== the settings page: edit, save, back up, restore\n");
  {
    char *dir = g_dir_make_tmp("skim-edit-XXXXXX", NULL);
    char *path = g_build_filename(dir, "headless.ini", NULL);
    static const char INI[] =
        "# my skimmer\n[radio]\nhost=192.168.1.21\nrate=96000\n"
        "[bands]\n160m=1845500\n40m=7045500\n"
        "[feed]\ncall=OK1HRA\nport=7302\n# tuned live\nmin_score=0.90\ngate=g.ini\n"
        "[web]\nsettings_from=127.0.0.1\n";
    g_file_set_contents(path, INI, -1, NULL);
    GHashTable *f = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(f, "radio.host", "192.168.1.21@3.81, 192.168.1.71");
    g_hash_table_insert(f, "bands", "20m=14045500 cw,rtty\r\n\n40m = 7045500\n");
    g_hash_table_insert(f, "feed.port", "7300");
    GError *err = NULL;
    char *out = skim_hd_config_apply_form(INI, strlen(INI), f, &err);
    SkimHdConfig c;
    const gboolean ok = out && skim_hd_config_parse(&c, out, strlen(out), path, &err);
    check("the form applied parses", ok);
    if (!ok) { printf("       (%s)\n", err ? err->message : "?"); }
    check("hosts, port and the new band list (20m with RTTY, then 40m) taken",
          ok && g_strv_length(c.hosts) == 2 && c.feed_port == 7300 && c.nb == 2 &&
          strcmp(c.band[0].name, "20m") == 0 && c.band[0].rtty &&
          strcmp(c.band[1].name, "40m") == 0);
    check("what the form does not touch stays: call, policy, gate, settings_from, comments",
          ok && strcmp(c.feed_call, "OK1HRA") == 0 && c.feed_min_score == 0.90 &&
          g_str_has_suffix(c.feed_gate, "/g.ini") && strcmp(c.settings_from[0], "127.0.0.1") == 0 &&
          strstr(out, "# my skimmer") && strstr(out, "# tuned live"));
    skim_hd_config_clear(&c);
    g_clear_error(&err);

    GHashTable *bad = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(bad, "web.settings_from", "0.0.0.0/0");
    char *no = skim_hd_config_apply_form(INI, strlen(INI), bad, &err);
    check("refused: a key the page may not change (web.settings_from)",
          !no && err && strstr(err->message, "web.settings_from"));
    g_clear_error(&err);
    g_hash_table_remove_all(bad);
    g_hash_table_insert(bad, "feed.call", "OK1HRA\nport=1");
    no = skim_hd_config_apply_form(INI, strlen(INI), bad, &err);
    check("refused: a value with a line break (no key smuggled in)", !no && err);
    g_clear_error(&err);
    g_hash_table_unref(bad);

    GHashTable *low = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(low, "feed.call", "ok1rak/p");
    g_hash_table_insert(low, "station.locator", "jo60wa");
    char *up = skim_hd_config_apply_form(INI, strlen(INI), low, &err);
    check("a call and a locator typed small go into the file in capitals",
          up && strstr(up, "call=OK1RAK/P") && strstr(up, "locator=JO60WA") && !strstr(up, "ok1rak"));
    g_free(up);
    g_hash_table_unref(low);
    {
      static const char SMALL[] = "[bands]\n40m=7045500\n[feed]\ncall=ok1rak\n";
      const gboolean sok = skim_hd_config_parse(&c, SMALL, strlen(SMALL), path, &err);
      check("a call written small by hand is used in capitals",
            sok && c.feed_call && strcmp(c.feed_call, "OK1RAK") == 0);
      if (sok) { skim_hd_config_clear(&c); }
      g_clear_error(&err);
    }

    static const char FOREIGN[] =
        "# exported elsewhere\n[radio]\nhost=auto\n[bands]\n20m=14045500 cw,rtty\n"
        "[web]\nsettings_from=0.0.0.0/0\n";
    char *imp = skim_hd_config_import(INI, strlen(INI), FOREIGN, strlen(FOREIGN), &err);
    const gboolean iok = imp && skim_hd_config_parse(&c, imp, strlen(imp), path, &err);
    check("an imported file parses, its bands and radio taken",
          iok && c.nb == 1 && strcmp(c.band[0].name, "20m") == 0 &&
          strcmp(c.hosts[0], "auto") == 0 && strstr(imp, "# exported elsewhere"));
    check("… but [web] settings_from stays this machine's (0.0.0.0/0 not taken)",
          iok && g_strv_length(c.settings_from) == 1 &&
          strcmp(c.settings_from[0], "127.0.0.1") == 0 && !strstr(imp, "0.0.0.0"));
    if (iok) { skim_hd_config_clear(&c); }
    g_free(imp);
    g_clear_error(&err);
    imp = skim_hd_config_import(INI, strlen(INI), "not [an ini", 11, &err);
    check("an import that is no ini refused", !imp && err && strstr(err->message, "imported"));
    g_clear_error(&err);

    char *bak = NULL;
    check("saved, the old file backed up", skim_hd_config_save(path, out, &bak, &err) && bak &&
          g_str_has_prefix(bak, "headless.ini.bak-"));
    char *now = NULL;
    g_file_get_contents(path, &now, NULL, NULL);
    check("the config is the new text", now && strcmp(now, out) == 0);
    g_free(now);
    char **list = skim_hd_config_backups(path);
    check("one backup listed", g_strv_length(list) == 1 && strcmp(list[0], bak) == 0);
    check("restore refuses a name that is no backup",
          !skim_hd_config_restore(path, "../../etc/passwd", &err) && err);
    g_clear_error(&err);
    check("restore puts the backup back", skim_hd_config_restore(path, bak, &err));
    g_file_get_contents(path, &now, NULL, NULL);
    check("… the old text is the config again", now && strcmp(now, INI) == 0);
    g_free(now);
    g_strfreev(list);
    list = skim_hd_config_backups(path);
    check("… and the replaced one is a backup itself (two now)", g_strv_length(list) == 2);
    g_strfreev(list);
    for (int i = 0; i < 14; i++) { skim_hd_config_save(path, INI, NULL, NULL); }
    list = skim_hd_config_backups(path);
    check("at most 10 backups kept", g_strv_length(list) == SKIM_HD_BACKUPS);
    for (char **x = list; *x; x++) {
      char *p = g_build_filename(dir, *x, NULL);
      g_remove(p);
      g_free(p);
    }
    g_strfreev(list);
    g_free(bak);
    g_free(out);
    g_hash_table_unref(f);
    g_remove(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
  }

  printf("== a blank value is the default (what the web form sends for an empty field)\n");
  {
    static const char BL[] =
        "[bands]\n20m=14045500\n[feed]\nport=\n[hub]\nport= \n[rbn]\nport=\ndelay_s=\n"
        "[status]\nconsole_s=\nhttp_port=\n[radio]\nclock_ppm=\nrate=\n[record]\nminutes=\n"
        "[decode]\nlog=\n";
    SkimHdConfig c;
    GError *e = NULL;
    const gboolean ok = skim_hd_config_parse(&c, BL, strlen(BL), "/tmp/blank.ini", &e);
    check(e ? e->message : "parses", ok);
    check("feed 7300, hub 7380, RBN 7550 delay 0, web/console unset (-1)",
          ok && c.feed_port == 7300 && c.hub_port == SKIM_HD_HUB_PORT && c.rbn_port == 7550 &&
          c.rbn_delay_s == 0 && c.http_port == -1 && c.console_s == -1);
    check("clock 0 ppm, rate 96 kHz, no decode log", ok && c.ppm == 0 && c.rate == 96000 && !c.decode_log);
    if (ok) { skim_hd_config_clear(&c); }
    g_clear_error(&e);
  }

  printf("== phased bands (ip=ab, docs/PHASING.md)\n");
  {
    SkimHdConfig c;
    GError *e = NULL;
    gboolean ok = parse(&c, "[radio]\nhost=192.0.2.7\n[phase]\nmode=mrc\ntc_s=2\n[bands]\n"
                            "20m=14045500 cw ip=ab\n40m=7025000 ip=ab phase=fixed\n80m=3525000\n", &e);
    check(e ? e->message : "parses", ok);
    check("20m and 40m phased, 80m not; A and B each two or three receivers",
          ok && c.band[0].phased && c.band[1].phased && !c.band[2].phased && c.band[0].ip == 0 &&
          c.nb_ip[0] == 3 && c.nb_ip[1] == 2 && c.nb_phased == 2);
    check("[phase] mode mrc, tc_s 2, gate 6 dB by default; 40m phase=fixed",
          ok && c.phase_mode == SKIM_PHASE_MRC && c.phase_tc_s == 2.0 && c.phase_gate_db == 6.0 &&
          c.band[0].phase_mode == -1 && c.band[1].phase_mode == SKIM_PHASE_FIXED);
    char *m = ok ? skim_hd_band_modes(&c.band[1], c.engine) : NULL;
    check("written back: \"cw ip=ab phase=fixed\"", m && strcmp(m, "cw ip=ab phase=fixed") == 0);
    g_free(m);
    if (ok) { skim_hd_config_clear(&c); }
    g_clear_error(&e);
    struct { const char *ini, *want; } bad_cases[] = {
      { "[radio]\nhost=a\n[bands]\n20m=14045500 ip=ab in=2\n", "no in=" },
      { "[radio]\nhost=a\n[bands]\n20m=14045500 ip=ab @w1\n", "not on a worker" },
      { "[radio]\ntype=mock\n[bands]\n20m=14045500 ip=ab\n", "type=hpsdr-p1 only" },
      { "[radio]\nhost=a\nrate=96000\n[rp]\nb_rate=192000\n[bands]\n20m=14045500 ip=ab\n",
        "one rate on both IPs" },
      { "[radio]\nhost=a\n[bands]\n20m=14045500 ip=ab phase=best\n", "phase=mvdr, mrc" },
      { "[radio]\nhost=a\n[phase]\nmode=strong\n[bands]\n20m=14045500 ip=ab\n", "[phase] mode" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(bad_cases); i++) {
      GError *e2 = NULL;
      const gboolean ok2 = parse(&c, bad_cases[i].ini, &e2);
      char what[200];
      g_snprintf(what, sizeof(what), "refused: %s", e2 ? e2->message : "(accepted)");
      check(what, !ok2 && e2 && strstr(e2->message, bad_cases[i].want));
      if (ok2) { skim_hd_config_clear(&c); }
      g_clear_error(&e2);
    }
    /* eight phased bands fill both IPs; a ninth has no receiver on B */
    GString *ini = g_string_new("[radio]\nhost=a\n[bands]\n");
    for (int i = 0; i < 9; i++) { g_string_append_printf(ini, "b%d=%d000000 ip=ab\n", i, i + 2); }
    GError *e3 = NULL;
    const gboolean ok3 = parse(&c, ini->str, &e3);
    check("a ninth phased band is refused: IP B's eight receivers are taken",
          !ok3 && e3 && strstr(e3->message, "IP B too"));
    if (ok3) { skim_hd_config_clear(&c); }
    g_clear_error(&e3);
    g_string_free(ini, TRUE);
  }

  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
