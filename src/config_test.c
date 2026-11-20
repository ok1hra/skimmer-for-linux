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
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

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

  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
