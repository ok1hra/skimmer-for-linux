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

static gboolean parse(SkimHdConfig *c, const char *text, GError **err) {
  return skim_hd_config_parse(c, text, strlen(text), "/etc/skim/test.ini", err);
}

int main(void) {
  printf("== the default ini\n");
  {
    char *dir = g_dir_make_tmp("skim-config-XXXXXX", NULL);
    char *path = g_build_filename(dir, "sub", "headless.ini", NULL);
    GError *err = NULL;
    check("written (directory created)", skim_hd_config_write_default(path, &err));
    SkimHdConfig c;
    const gboolean ok = skim_hd_config_load(&c, path, &err);
    check("parses", ok);
    if (!ok) { printf("       (%s)\n", err->message); }
    check("a Red Pitaya at 96 kHz, one host",
          ok && c.source == SKIM_HD_SOURCE_HPSDR_P1 && c.rate == 96000 &&
          g_strv_length(c.hosts) == 1 && strcmp(c.hosts[0], "192.168.1.21") == 0);
    check("six bands, CW v2, no RTTY",
          ok && c.nb == 6 && c.band[0].cw && !c.band[0].rtty &&
          c.band[0].cw_engine == SKIM_CW_ENGINE_V2 && strcmp(c.band[2].name, "40m") == 0 &&
          c.band[2].centre_hz == 7040000);
    check("feed on 7300 with no call, the policy left to the pipeline",
          ok && c.feed_port == 7300 && c.feed_call && !c.feed_call[0] &&
          c.feed_min_score == 0 && c.feed_fresh_s == 0 && !c.feed_gate && !c.feed_gate_mode);
    check("status 10 s / 8073, no recording",
          ok && c.console_s == 10 && c.http_port == 8073 && !c.rec_bands);
    check("RTTY spots go to the feed unless told otherwise", ok && c.feed_rtty);
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
    check("type=mock is the synthetic source",
          parse(&c, "[radio]\ntype=mock\n[bands]\n20m=14045500\n", NULL) &&
          c.source == SKIM_HD_SOURCE_MOCK);
    skim_hd_config_clear(&c);
  }

  printf("== refused, with the reason\n");
  {
    static const struct { const char *what, *text, *says; } BAD[] = {
      { "no band", "[radio]\nrate=96000\n", "no [bands]" },
      { "a centre that is no number", "[bands]\n20m=fourteen\n", "20m" },
      { "a centre out of range", "[bands]\n20m=14045\n", "not a centre" },
      { "an unknown mode", "[bands]\n20m=14045500 psk\n", "\"psk\"" },
      { "an unknown CW engine", "[bands]\n20m=14045500 cw:v3\n", "\"cw:v3\"" },
      { "nine bands", "[bands]\na=1900000\nb=3600000\nc=7000000\nd=10100000\n"
        "e=14000000\nf=18100000\ng=21000000\nh=24900000\ni=28000000\n", "8 receivers" },
      { "a rate the radio has not", "[radio]\nrate=44100\n[bands]\n20m=14045500\n", "44100" },
      { "a bad host@ppm", "[radio]\nhost=192.168.1.21@x\n[bands]\n20m=14045500\n",
        "192.168.1.21@x" },
      { "an unknown radio type", "[radio]\ntype=soapy\n[bands]\n20m=14045500\n", "soapy" },
      { "an unknown [decode] engine", "[decode]\nengine=v9\n[bands]\n20m=14045500\n", "v9" },
      { "[feed] rtty that is no boolean", "[bands]\n20m=14045500\n[feed]\nrtty=maybe\n",
        "rtty: true or false" },
      { "[decode] suppress_cw_on_rtty that is no boolean",
        "[bands]\n20m=14045500\n[decode]\nsuppress_cw_on_rtty=1x\n",
        "[decode] suppress_cw_on_rtty: true or false" },
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

  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
