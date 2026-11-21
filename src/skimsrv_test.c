/* skimsrv_test.c — the skimsrv gate: CW Skimmer Server's SkimSrv.ini under
 * a wine prefix is found (Application Data and AppData/Roaming, any user),
 * the newest file wins, only the band keys of [Skimmer] leave — never [User]
 * or another key, never a value that is not digits, commas and a dot — and
 * a home without one says where it looked.
 * Part of skimmer-for-linux. GPL-3.0-or-later. */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utime.h>

#include "headless/skimsrv.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static char *put(const char *dir, const char *rel, const char *text, long mtime) {
  char *p = g_build_filename(dir, rel, NULL);
  char *d = g_path_get_dirname(p);
  g_mkdir_with_parents(d, 0755);
  g_file_set_contents(p, text, -1, NULL);
  struct utimbuf t = { mtime, mtime };
  utime(p, &t);
  g_free(d);
  return p;
}

int main(void) {
  char *home = g_dir_make_tmp("skimsrv-XXXXXX", NULL);

  char *j = skim_hd_skimsrv_json(home, NULL);
  check("no wine: not found, where it looked",
        strstr(j, "\"found\":false") && strstr(j, ".wine/drive_c/users"));
  g_free(j);

  /* a Windows file (CRLF), the operator's data in [User], a key of another
   * section with the same name, a value that may not leave */
  const char *ini =
    "[Window]\r\nRate=7\r\n"
    "[User]\r\nCall=OK1XYZ\r\nName=Jan Novak\r\nQth=Somewhere 12\r\n"
    "[Skimmer]\r\n"
    "CenterFreqs96=1845500,3545500,7045500,10145500,14045500,18113500,21045500,24935500,28045500\r\n"
    "SegmentSel96=111111101\r\n"
    "SegmentSel48=110<script>\r\n"
    "CwSegments=1800000-2000000\r\n"
    "DeviceName=01 RP-F0FF08 v25\r\n"
    "Rate=1\r\n"
    "FreqCalibration=0.999995540\r\n";
  char *old = put(home, ".wine/drive_c/users/dan/Application Data/Afreet/Products/SkimSrv/SkimSrv.ini",
                  ini, 1700000000);
  j = skim_hd_skimsrv_json(home, NULL);
  check("found under Application Data", strstr(j, "\"found\":true") && strstr(j, "Application Data"));
  check("the band keys of [Skimmer]: rate, centres, selection, calibration",
        strstr(j, "Rate=1\\u000a") && strstr(j, "CenterFreqs96=1845500,") && strstr(j, "SegmentSel96=111111101\\u000a") &&
        strstr(j, "FreqCalibration=0.999995540\\u000a"));
  check("never [User], nor another key, nor a bad value, nor [Window]'s Rate",
        !strstr(j, "OK1XYZ") && !strstr(j, "Novak") && !strstr(j, "Qth") && !strstr(j, "DeviceName") &&
        !strstr(j, "CwSegments") && !strstr(j, "script") && !strstr(j, "SegmentSel48") && !strstr(j, "Rate=7"));
  g_free(j);

  /* a newer one of another user under AppData/Roaming wins */
  char *newer = put(home, ".wine/drive_c/users/op/AppData/Roaming/Afreet/Products/SkimSrv/SkimSrv.ini",
                    "[Skimmer]\nRate=2\nSegmentSel192=0011\n", 1800000000);
  j = skim_hd_skimsrv_json(home, NULL);
  check("the newest file wins (AppData/Roaming)", strstr(j, "AppData/Roaming") && strstr(j, "Rate=2\\u000a"));
  g_free(j);

  /* $WINEPREFIX is looked in too */
  char *pfx = g_build_filename(home, "pfx", NULL);
  char *third = put(pfx, "drive_c/users/x/AppData/Roaming/Afreet/Products/SkimSrv/SkimSrv.ini",
                    "[Skimmer]\nRate=0\n", 1900000000);
  j = skim_hd_skimsrv_json(home, pfx);
  check("$WINEPREFIX looked in", strstr(j, "/pfx/") && strstr(j, "Rate=0\\u000a"));
  g_free(j);

  /* a file without the keys does not count */
  put(pfx, "drive_c/users/x/AppData/Roaming/Afreet/Products/SkimSrv/SkimSrv.ini",
      "[User]\nCall=OK1XYZ\n", 2000000000);
  j = skim_hd_skimsrv_json(home, pfx);
  check("a file without band keys is passed over", strstr(j, "Rate=2\\u000a") && !strstr(j, "OK1XYZ"));
  g_free(j);

  g_free(old); g_free(newer); g_free(third); g_free(pfx);
  char *rm = g_strdup_printf("rm -rf '%s'", home);
  if (system(rm) != 0) { /* a temp dir left */ }
  g_free(rm);
  g_free(home);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
