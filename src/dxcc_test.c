/*
 * skimmer-dxcc-test — a call's DXCC from the map's data (headless/dxcc.h)
 * and [filter] bad (headless/config.h): prefixes, whole calls, country
 * names. The DXCCs are the ones SkMap.locate (web/map.js) gives the page;
 * with a file of calls it prints each one's, to be set beside the page's.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "headless/config.h"
#include "headless/dxcc.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void dx(const char *call, const char *want) {
  const char *p = NULL, *n = NULL;
  skim_hd_dxcc_find(call, &p, &n);
  char *what = g_strdup_printf("%s → %s (got %s)", call, want ? want : "none", p ? p : "none");
  check(what, want ? p && strcmp(p, want) == 0 : p == NULL);
  g_free(what);
}

static gboolean parse(SkimHdConfig *c, const char *text, GError **err) {
  return skim_hd_config_parse(c, text, strlen(text), "/etc/skim/test.ini", err);
}

int main(int argc, char **argv) {
  /* skimmer-dxcc-test CALLS: each call's DXCC prefix ("-" none) — set beside
   * SkMap.locate's for the same file, the port is checked call by call */
  if (argc > 1) {
    char *text = NULL;
    if (!g_file_get_contents(argv[1], &text, NULL, NULL)) { return 2; }
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
      if (!**l) { continue; }
      const char *p = NULL;
      skim_hd_dxcc_find(*l, &p, NULL);
      printf("%s\t%s\n", *l, p ? p : "-");
    }
    g_strfreev(lines);
    g_free(text);
    return 0;
  }
  printf("== a call's DXCC (the map's labels and IP-rotator's aliases)\n");
  dx("OK1HRA", "OK");
  dx("OL5Y", "OK");
  dx("DL1ABC", "DL");
  dx("DA0HQ", "DL");
  dx("UA3ABC", "UA/E");
  dx("RA9XY", "UA/AS");
  dx("R0AA", "UA/AS");
  dx("UA2FF", "UA2");
  dx("EW1AB", "EU");
  dx("M0ABC", "G");
  dx("MM0XYZ", "GM");
  dx("2E0AAA", "G");
  dx("EA8AB", "EA8");
  dx("W1AW", "K");
  dx("KL7AB", "KL");
  dx("AH6X", "KH6");
  dx("JA1ABC", "JA");
  dx("OK1HRA/P", "OK");
  dx("F/OK1HRA", "OK");
  dx("SV9/OK1HRA", "OK");          /* the longest part, as the page */
  dx("QQQ", NULL);

  printf("== [filter] bad\n");
  {
    SkimHdConfig c;
    GError *err = NULL;
    check("none: everything passes",
          parse(&c, "[bands]\n20m=14045500\n", NULL) && !c.filter_bad && skim_hd_config_call_pass(&c, "UA3ABC"));
    skim_hd_config_clear(&c);

    check("prefixes and calls",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nbad=UA9, ok1abc ,3B8\n", &err) && c.filter_bad &&
          skim_hd_config_filter_on(&c));
    check("  UA9XYZ out", !skim_hd_config_call_pass(&c, "UA9XYZ"));
    check("  UA3XYZ in", skim_hd_config_call_pass(&c, "UA3XYZ"));
    check("  OK1ABC out, any case", !skim_hd_config_call_pass(&c, "ok1abc"));
    check("  OK1ABC/P and DL/OK1ABC out", !skim_hd_config_call_pass(&c, "OK1ABC/P") &&
          !skim_hd_config_call_pass(&c, "DL/OK1ABC"));
    check("  OK1AB in", skim_hd_config_call_pass(&c, "OK1AB"));
    check("  3B8CF out", !skim_hd_config_call_pass(&c, "3B8CF"));
    skim_hd_config_clear(&c);

    check("country names",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nbad=Russia\n", &err) && c.filter_bad_name &&
          !c.filter_bad_pfx);
    check("  European, Asiatic Russia out", !skim_hd_config_call_pass(&c, "RA3ABC") &&
          !skim_hd_config_call_pass(&c, "UA0ABC") && !skim_hd_config_call_pass(&c, "R9DX"));
    check("  Kaliningrad, Franz Josef Land out", !skim_hd_config_call_pass(&c, "UA2FF") &&
          !skim_hd_config_call_pass(&c, "R1FJA"));
    check("  Belarus (Belorussia) out", !skim_hd_config_call_pass(&c, "EW8AB"));
    check("  Ukraine, Czech Republic in", skim_hd_config_call_pass(&c, "UR5AB") &&
          skim_hd_config_call_pass(&c, "OK1HRA"));
    skim_hd_config_clear(&c);

    check("a name's part, a prefix beside it",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nbad=germ, JA\n", &err));
    check("  DL1AB, JA1XYZ out; OK1HRA in", !skim_hd_config_call_pass(&c, "DL1AB") &&
          !skim_hd_config_call_pass(&c, "JA1XYZ") && skim_hd_config_call_pass(&c, "OK1HRA"));
    skim_hd_config_clear(&c);

    check("with the pass filter: both hold",
          parse(&c, "[bands]\n20m=14045500\n[filter]\ncall=OK\nbad=OK1ABC\n", &err) &&
          skim_hd_config_call_pass(&c, "OK1HRA") && !skim_hd_config_call_pass(&c, "OK1ABC") &&
          !skim_hd_config_call_pass(&c, "DL1AB"));
    skim_hd_config_clear(&c);

    g_clear_error(&err);
    check("a token of other characters refused",
          !parse(&c, "[bands]\n20m=14045500\n[filter]\nbad=UA9, R*\n", &err) && err &&
          strstr(err->message, "R*"));
    g_clear_error(&err);
    check("empty tokens and a blank value are nothing",
          parse(&c, "[bands]\n20m=14045500\n[filter]\nbad= , ,\n", &err) && !c.filter_bad);
    skim_hd_config_clear(&c);
  }

  printf("%d of %d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
