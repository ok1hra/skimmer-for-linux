/*
 * skimmer-archive-test — the spot archive (src/headless/archive.h).
 *
 * Spots land in their UTC day's YYYYMMDD-spots.gz (a minute across midnight
 * splits into two files); each flush appends one more gzip member that
 * `gzip -dc` reads with the rest; the header opens a day's file once; a
 * flush the disk refuses keeps the lines and writes them at the next one.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "headless/archive.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* The whole file, decompressed by gzip (every member). */
static char *unzip(const char *dir, const char *name) {
  char *path = g_build_filename(dir, name, NULL);
  char *q = g_shell_quote(path);
  char *cmd = g_strdup_printf("gzip -dc %s", q);
  char *out = NULL;
  int st = 0;
  if (!g_spawn_command_line_sync(cmd, &out, NULL, &st, NULL) || st != 0) { g_clear_pointer(&out, g_free); }
  g_free(cmd); g_free(q); g_free(path);
  return out;
}

static guint count(const char *s, const char *what) {
  guint n = 0;
  for (const char *p = s; p && (p = strstr(p, what)); p += strlen(what)) { n++; }
  return n;
}

int main(void) {
  char *dir = g_dir_make_tmp("skim-archive-XXXXXX", NULL);
  char *sub = g_build_filename(dir, "spots", NULL);
  const gint64 midnight = 1795046400;          /* 2026-11-19 00:00:00 UTC */

  printf("== the day's file name\n");
  {
    char *n = skim_hd_archive_file_name(midnight - 1);
    check("23:59:59 on the 18th: 20261118-spots.gz", strcmp(n, "20261118-spots.gz") == 0);
    g_free(n);
  }

  printf("== spots across midnight, two flushes\n");
  {
    GError *err = NULL;
    SkimHdArchive *a = skim_hd_archive_new(sub, &err);
    check("opened (directory created)", a && g_file_test(sub, G_FILE_TEST_IS_DIR));
    skim_hd_archive_add(a, midnight - 30, "OK1MCK", "CW", 7046500, 41, 25, "40m", NULL);
    skim_hd_archive_add(a, midnight - 2, "DL1ABC", "CW", 14025300, 12.4, 28, "20m", "pc2");
    skim_hd_archive_add(a, midnight + 5, "OK1RTY", "RTTY", 14080000, 20, 45, "20m", "");
    check("flushed", skim_hd_archive_flush(a, &err));
    char *d18 = unzip(sub, "20261118-spots.gz"), *d19 = unzip(sub, "20261119-spots.gz");
    check("the 18th has its two spots, the 19th its one",
          d18 && d19 && count(d18, "\n") == 3 && count(d19, "\n") == 2);
    check("a tab-separated line: time, kHz, call, mode, dB, speed, band, source",
          d18 && strstr(d18, "2026-11-18 23:59:30\t7046.5\tOK1MCK\tCW\t41\t25\t40m\tlocal\n") &&
          strstr(d18, "2026-11-18 23:59:58\t14025.3\tDL1ABC\tCW\t12\t28\t20m\tpc2\n"));
    check("the header opens the file", d18 && g_str_has_prefix(d18, "# time_utc\tkhz\tcall\t"));
    g_free(d18); g_free(d19);
    skim_hd_archive_add(a, midnight + 60, "OK1HRA", "CW", 3546500, 30, 24, "80m", NULL);
    check("flushed again", skim_hd_archive_flush(a, &err));
    d19 = unzip(sub, "20261119-spots.gz");
    check("… one more gzip member: gzip reads both, the header once",
          d19 && count(d19, "\n") == 3 && count(d19, "# time_utc") == 1 && strstr(d19, "OK1RTY") &&
          strstr(d19, "OK1HRA"));
    g_free(d19);
    check("nothing waiting: a flush with no spots writes nothing", skim_hd_archive_flush(a, &err));
    guint64 w = 0;
    guint wait = 9;
    char *e = NULL, *today = NULL;
    skim_hd_archive_status(a, NULL, &today, &w, &wait, &e);
    check("status: 4 written, none waiting, no error, today's name", w == 4 && wait == 0 && !e &&
          g_str_has_suffix(today, "-spots.gz") && strlen(today) == 17);
    g_free(today);

    printf("== the disk refuses\n");
    /* the day's file in the way as a directory: the append fails */
    char *blocked = g_build_filename(sub, "20261120-spots.gz", NULL);
    g_mkdir(blocked, 0755);
    skim_hd_archive_add(a, midnight + 86400 + 10, "OK2AA", "CW", 7010000, 10, 20, "40m", NULL);
    check("the flush fails", !skim_hd_archive_flush(a, &err) && err);
    g_clear_error(&err);
    skim_hd_archive_status(a, NULL, NULL, &w, &wait, &e);
    check("… the spot waits, the error is in the status", wait == 1 && w == 4 && e);
    g_free(e);
    g_rmdir(blocked);
    skim_hd_archive_add(a, midnight + 86400 + 20, "OK2BB", "CW", 7011000, 10, 20, "40m", NULL);
    check("the next flush writes it, with the newer one", skim_hd_archive_flush(a, &err));
    char *d20 = unzip(sub, "20261120-spots.gz");
    check("… both, in order", d20 && strstr(d20, "OK2AA") && strstr(d20, "OK2BB") &&
          strstr(d20, "OK2AA") < strstr(d20, "OK2BB"));
    g_free(d20);
    skim_hd_archive_status(a, NULL, NULL, &w, &wait, &e);
    check("… the error cleared", !e && wait == 0 && w == 6);
    g_free(blocked);
    skim_hd_archive_add(a, midnight + 90, "OK1ZZ", "CW", 7012000, 10, 20, "40m", NULL);
    skim_hd_archive_free(a);                       /* flushes                  */
    d19 = unzip(sub, "20261119-spots.gz");
    check("freeing flushes what waits", d19 && strstr(d19, "OK1ZZ"));
    g_free(d19);
  }

  printf("== a directory that cannot be made\n");
  {
    GError *err = NULL;
    SkimHdArchive *a = skim_hd_archive_new("/proc/skim-archive-test", &err);
    check("refused with the reason", !a && err);
    g_clear_error(&err);
  }

  char *rm = g_strdup_printf("rm -rf %s", dir);
  g_spawn_command_line_sync(rm, NULL, NULL, NULL, NULL);
  g_free(rm); g_free(sub); g_free(dir);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
