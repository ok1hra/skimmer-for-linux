/*
 * skimmer-stats-test — the 7-day history (src/headless/stats.h).
 *
 * Records land in their minute's slot and come back out of /stats.json
 * summed or averaged per bucket; the file survives a reopen, with the bands
 * matched by name when the config re-orders, adds or drops them; a file of
 * another size starts a fresh ring instead of being misread.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "headless/stats.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static SkimStatsMinute minute_of(float spots20, float spots40, float active20, float snr20) {
  SkimStatsMinute m;
  for (guint b = 0; b < SKIM_STATS_BANDS; b++)
    for (guint k = 0; k < SKIM_STATS_MODES; k++)
      for (guint j = 0; j < SKIM_SM_N; j++) m.band[b][k][j] = NAN;
  for (guint j = 0; j < SKIM_SG_N; j++) m.glob[j] = 0;
  m.band[0][0][SKIM_SM_SPOTS] = spots20;
  m.band[0][0][SKIM_SM_ACTIVE] = active20;
  m.band[0][0][SKIM_SM_SNR] = snr20;
  m.band[1][0][SKIM_SM_SPOTS] = spots40;
  m.band[1][0][SKIM_SM_ACTIVE] = 1;
  m.glob[SKIM_SG_CPU] = 40;
  m.glob[SKIM_SG_STREAMING] = 1;
  return m;
}

/* The value list of "key":{ … "band":[…] } in a stats JSON, as text. */
static char *series_of(const char *json, const char *metric, const char *band) {
  char *k = g_strdup_printf("\"%s\":{", metric);
  const char *m = strstr(json, k);
  g_free(k);
  if (!m) { return NULL; }
  char *bk = g_strdup_printf("\"%s\":[", band);
  const char *b = strstr(m, bk);
  const gsize skip = strlen(bk);
  g_free(bk);
  if (!b) { return NULL; }
  const char *e = strchr(b, ']');
  return g_strndup(b + skip, (gsize)(e - b - skip));
}

int main(void) {
  char *dir = g_dir_make_tmp("skim-stats-XXXXXX", NULL);
  char *path = g_build_filename(dir, "sub", "stats.bin", NULL);
  const gint64 now = 29000000;                  /* a unix minute, 6-aligned */

  printf("== records and buckets\n");
  {
    const char *bands[] = { "20m", "40m" };
    SkimStats *s = skim_stats_open(path, bands, 2);
    /* the last six minutes: 20 m spots 1..6, SNR 10..15; none before */
    for (int i = 0; i < 6; i++) {
      SkimStatsMinute m = minute_of((float)(i + 1), 2, (float)(10 + i), (float)(10 + i));
      skim_stats_put(s, now - 5 + i, &m);
    }
    char *j = skim_stats_json(s, now, 1, 3);   /* 1 h in 3-minute buckets */
    check("1 h in 3-minute buckets: n 20, step 3, both bands",
          strstr(j, "\"step_min\":3,\"n\":20,\"bands\":[\"20m\",\"40m\"]") != NULL);
    char *sp = series_of(j, "spots", "20m");
    check("counts are summed per bucket, empty buckets null (… null,6,15)",
          sp && g_str_has_suffix(sp, "null,null,6,15") && g_str_has_prefix(sp, "null,"));
    char *ac = series_of(j, "active", "20m");
    check("levels are averaged (… 11,14)", ac && g_str_has_suffix(ac, ",11,14"));
    char *sn = series_of(j, "snr", "20m");
    check("SNR is averaged too (… 11,14)", sn && g_str_has_suffix(sn, ",11,14"));
    check("no RTTY recorded: rtty is null", strstr(j, "\"rtty\":null") != NULL);
    check("global series present", strstr(j, "\"global\":{\"cpu\":[") != NULL);
    g_free(sp); g_free(ac); g_free(sn); g_free(j);
    char *auto_j = skim_stats_json(s, now, 168, 0);
    check("7 days at the automatic step: 14-minute buckets, 720 of them",
          strstr(auto_j, "\"step_min\":14,\"n\":720") != NULL);
    g_free(auto_j);
    skim_stats_free(s);                          /* saves                    */
    check("saved on free (directory created)", g_file_test(path, G_FILE_TEST_EXISTS));
  }

  printf("== reopened with the bands re-ordered, one added, one dropped\n");
  {
    const char *bands[] = { "15m", "20m" };
    SkimStats *s = skim_stats_open(path, bands, 2);
    char *j = skim_stats_json(s, now, 1, 3);
    char *sp20 = series_of(j, "spots", "20m"), *sp15 = series_of(j, "spots", "15m");
    check("20 m keeps its history in its new column", sp20 && g_str_has_suffix(sp20, ",6,15"));
    check("15 m starts empty", sp15 && !strstr(sp15, "6") && g_str_has_prefix(sp15, "null"));
    check("40 m is gone from the answer", !strstr(j, "\"40m\""));
    g_free(sp20); g_free(sp15); g_free(j);
    skim_stats_free(s);
  }

  printf("== a foreign file\n");
  {
    g_file_set_contents(path, "not a history", -1, NULL);
    const char *bands[] = { "20m" };
    SkimStats *s = skim_stats_open(path, bands, 1);
    char *j = skim_stats_json(s, now, 1, 60);
    check("starts a fresh ring (one bucket, null)", strstr(j, "\"spots\":{\"20m\":[null]}") != NULL);
    g_free(j);
    skim_stats_free(s);
  }

  g_remove(path);
  char *sub = g_path_get_dirname(path);
  g_rmdir(sub);
  g_rmdir(dir);
  g_free(sub); g_free(path); g_free(dir);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
