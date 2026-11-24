/*
 * skimmer-stats-test — the 7-day history (src/headless/stats.h).
 *
 * Records land in their minute's slot and come back out of /stats.json
 * summed or averaged per bucket; the file survives a reopen, with the bands
 * matched by name when the config re-orders, adds or drops them; a file of
 * another size starts a fresh ring instead of being misread; a version 1
 * file (before the CPU split), a version 3 file (before the CPU
 * temperature), a version 4 file (before the thermal pauses) and a version 5
 * file (before FT8 and the helpers' CPU: two modes) are carried over; the
 * temperature keeps each bucket's peak, a pause its share; FT8 is a mode of
 * its own.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "headless/stats.h"

enum { SM4 = 6 };                   /* versions 1–4's band metrics */
enum { OM = 2 };                    /* versions 1–5's modes: CW, RTTY */

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
          strstr(j, "\"step_min\":3,\"n\":20,\"res_min\":1,") &&
          strstr(j, "\"bands\":[\"20m\",\"40m\"]"));
    char *fr = g_strdup_printf("\"first\":%" G_GINT64_FORMAT ",", (now - 5 - (now - 5) % 60) * 60);
    check("the oldest record is named (for a time slider)", strstr(j, fr) != NULL);
    g_free(fr);
    char *sp = series_of(j, "spots", "20m");
    check("counts are summed per bucket, empty buckets null (… null,6,15)",
          sp && g_str_has_suffix(sp, "null,null,6,15") && g_str_has_prefix(sp, "null,"));
    char *ac = series_of(j, "active", "20m");
    check("levels are averaged (… 11,14)", ac && g_str_has_suffix(ac, ",11,14"));
    char *sn = series_of(j, "snr", "20m");
    check("SNR is averaged too (… 11,14)", sn && g_str_has_suffix(sn, ",11,14"));
    check("no RTTY recorded: rtty is null, nor FT8, nor JS8", strstr(j, "\"rtty\":null") &&
          strstr(j, "\"ft8\":null") && strstr(j, "\"js8\":null"));
    check("global series present", strstr(j, "\"global\":{\"cpu\":[") != NULL);
    g_free(sp); g_free(ac); g_free(sn); g_free(j);
    char *auto_j = skim_stats_json(s, now, 168, 0);
    check("7 days at the automatic step: 14-minute buckets, 720 of them",
          strstr(auto_j, "\"step_min\":14,\"n\":720") != NULL);
    g_free(auto_j);
    skim_stats_free(s);                          /* saves                    */
    check("saved on free (directory created)", g_file_test(path, G_FILE_TEST_EXISTS));
  }

  printf("== saving writes the new minutes in place\n");
  {
    const char *bands[] = { "20m", "40m" };
    GStatBuf a, b;
    g_stat(path, &a);
    SkimStats *s = skim_stats_open(path, bands, 2);
    SkimStatsMinute m = minute_of(42, 0, 1, 1);
    skim_stats_put(s, now + 1, &m);
    GError *err = NULL;
    check("saved", skim_stats_save(s, &err));
    g_stat(path, &b);
    check("… into the same file (same inode, same size) — not a rewrite",
          a.st_ino == b.st_ino && a.st_size == b.st_size);
    char *j = skim_stats_json(s, now + 1, 1, 60);
    skim_stats_free(s);
    s = skim_stats_open(path, bands, 2);
    char *j2 = skim_stats_json(s, now + 1, 1, 60);
    check("… and reopened, the history is the same (the new minute in it)",
          strcmp(j, j2) == 0 && strstr(j2, "\"20m\":[63]"));
    g_free(j);
    g_free(j2);
    skim_stats_free(s);
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

  printf("== a version 1 file (no CPU split)\n");
  {
    /* stats.c's version 1 layout, written by hand */
    typedef struct {
      char    magic[8];
      guint32 version, minutes, bands, modes, sm_n, sg_n;
      char    names[SKIM_STATS_BANDS][16];
    } H1;
    typedef struct {
      gint64 minute;
      float  band[SKIM_STATS_BANDS][OM][SM4];
      float  glob[5];
    } S1;
    const gsize len = sizeof(H1) + SKIM_STATS_MINUTES * sizeof(S1);
    char *buf = g_malloc0(len);
    H1 *h = (H1 *)buf;
    memcpy(h->magic, "SKIMSTAT", 8);
    h->version = 1;
    h->minutes = SKIM_STATS_MINUTES;
    h->bands = SKIM_STATS_BANDS;
    h->modes = OM;
    h->sm_n = SM4;
    h->sg_n = 5;
    g_strlcpy(h->names[0], "20m", 16);
    S1 *sl = (S1 *)(buf + sizeof(H1));
    for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
      for (guint b = 0; b < SKIM_STATS_BANDS; b++)
        for (guint k = 0; k < OM; k++)
          for (guint j = 0; j < SM4; j++) sl[i].band[b][k][j] = NAN;
      for (guint j = 0; j < 5; j++) sl[i].glob[j] = NAN;
    }
    S1 *one = &sl[now % SKIM_STATS_MINUTES];
    one->minute = now;
    one->band[0][0][SKIM_SM_SPOTS] = 7;
    one->glob[SKIM_SG_CPU] = 123;
    one->glob[SKIM_SG_RADIO] = 2;
    g_file_set_contents(path, buf, (gssize)len, NULL);
    g_free(buf);
    const char *bands[] = { "20m" };
    SkimStats *s = skim_stats_open(path, bands, 1);
    char *j = skim_stats_json(s, now, 1, 60);
    check("its spots and CPU carried over",
          strstr(j, "\"spots\":{\"20m\":[7]}") && strstr(j, "\"cpu\":[123]") &&
          strstr(j, "\"radio\":[2]"));
    check("… the CPU split empty (null)",
          strstr(j, "\"cpu_cw\":[null]") && strstr(j, "\"cpu_rtty\":[null]"));
    g_free(j);
    SkimStatsMinute m = minute_of(1, 1, 1, 1);
    m.glob[SKIM_SG_CPU_CW] = 80;
    m.glob[SKIM_SG_CPU_RTTY] = 20;
    skim_stats_put(s, now + 1, &m);
    skim_stats_free(s);                          /* written as this version  */
    s = skim_stats_open(path, bands, 1);
    j = skim_stats_json(s, now + 1, 2, 120);    /* one bucket: both minutes */
    char *jt = skim_stats_json(s, now, 720, 0);
    check("its week merged into the coarser rings (the old minute's 7 + the new one's 1)",
          strstr(jt, "\"res_min\":15") && strstr(jt, "\"20m\":[null") && strstr(jt, ",8]") != NULL);
    g_free(jt);
    check("saved as this version and read back: the old minute and the split",
          strstr(j, "\"cpu\":[81.5]") && strstr(j, "\"cpu_cw\":[80]") &&
          strstr(j, "\"cpu_rtty\":[20]") && strstr(j, "\"spots\":{\"20m\":[8]}"));
    g_free(j);
    skim_stats_free(s);
  }

  printf("== a version 3 file (the rings, no CPU temperature)\n");
  {
    /* stats.c's version 3 layout, written by hand: three rings of slots */
    typedef struct {
      char    magic[8];
      guint32 version, minutes, bands, modes, sm_n, sg_n;
      char    names[SKIM_STATS_BANDS][16];
      guint32 ntier, tier_res[3], tier_len[3], pad;
    } H3;
    enum { G3 = 7, N3 = SKIM_STATS_BANDS * OM * SM4 + G3 };
    typedef struct {
      gint64 start;
      float  band[SKIM_STATS_BANDS][OM][SM4];
      float  glob[G3];
      guint8 n[N3];
    } S3;
    const guint res[3] = { 1, 15, 60 }, tl[3] = { SKIM_STATS_MINUTES, 90 * 24 * 4, 2 * 365 * 24 };
    gsize len = sizeof(H3);
    for (int t = 0; t < 3; t++) { len += tl[t] * sizeof(S3); }
    char *buf = g_malloc0(len);
    H3 *h = (H3 *)buf;
    memcpy(h->magic, "SKIMSTAT", 8);
    h->version = 3;
    h->bands = SKIM_STATS_BANDS;
    h->modes = OM;
    h->sm_n = SM4;
    h->sg_n = G3;
    h->ntier = 3;
    for (int t = 0; t < 3; t++) { h->tier_res[t] = res[t]; h->tier_len[t] = tl[t]; }
    g_strlcpy(h->names[0], "20m", 16);
    gsize off = sizeof(H3);
    for (int t = 0; t < 3; t++) {
      S3 *sl = (S3 *)(buf + off);
      for (guint i = 0; i < tl[t]; i++) {
        for (guint b = 0; b < SKIM_STATS_BANDS; b++)
          for (guint k = 0; k < OM; k++)
            for (guint j = 0; j < SM4; j++) sl[i].band[b][k][j] = NAN;
        for (guint j = 0; j < G3; j++) sl[i].glob[j] = NAN;
      }
      /* the minute now, in each ring's slot that holds it */
      S3 *one = &sl[(now / res[t]) % tl[t]];
      one->start = now - now % res[t];
      one->band[0][0][SKIM_SM_SPOTS] = 9;
      one->n[SKIM_SM_SPOTS] = 1;
      one->glob[SKIM_SG_CPU] = 55;
      one->n[SKIM_STATS_BANDS * OM * SM4 + SKIM_SG_CPU] = 1;
      one->glob[SKIM_SG_CPU_RTTY] = 11;
      one->n[SKIM_STATS_BANDS * OM * SM4 + SKIM_SG_CPU_RTTY] = 1;
      off += tl[t] * sizeof(S3);
    }
    g_file_set_contents(path, buf, (gssize)len, NULL);
    g_free(buf);
    const char *bands[] = { "20m" };
    SkimStats *s = skim_stats_open(path, bands, 1);
    char *j = skim_stats_json(s, now, 1, 60);
    check("its spots, CPU and split carried over",
          strstr(j, "\"spots\":{\"20m\":[9]}") && strstr(j, "\"cpu\":[55]") &&
          strstr(j, "\"cpu_rtty\":[11]"));
    check("… the CPU temperature empty (null)", strstr(j, "\"cpu_temp\":[null]") != NULL);
    g_free(j);
    j = skim_stats_json(s, now, 720, 0);
    check("… in the 15-minute ring too", strstr(j, "\"res_min\":15") && strstr(j, ",9]"));
    g_free(j);
    /* the temperature: a bucket keeps its hottest minute */
    const float temps[] = { 50, 71.5f, 60 };
    for (int i = 0; i < 3; i++) {
      SkimStatsMinute m = minute_of(1, 0, 1, 1);
      m.glob[SKIM_SG_CPU_TEMP] = temps[i];
      skim_stats_put(s, now + 1 + i, &m);
    }
    skim_stats_free(s);                          /* written as this version  */
    s = skim_stats_open(path, bands, 1);
    j = skim_stats_json(s, now + 3, 1, 60);
    check("saved as this version: the old minute kept, the temperature's peak per bucket (71.5)",
          strstr(j, "\"spots\":{\"20m\":[12]}") && strstr(j, "\"cpu_temp\":[71.5]"));
    g_free(j);
    skim_stats_free(s);
    char *keep = g_strconcat(path, ".v3", NULL);
    GStatBuf kb;
    check("… the version 3 file kept beside it, as it was (for an older build)",
          g_stat(keep, &kb) == 0 && (gsize)kb.st_size == len);
    g_remove(keep);
    g_free(keep);
  }

  printf("== a version 4 file (no thermal pauses)\n");
  {
    /* stats.c's version 3 layout, written by hand: three rings of slots */
    typedef struct {
      char    magic[8];
      guint32 version, minutes, bands, modes, sm_n, sg_n;
      char    names[SKIM_STATS_BANDS][16];
      guint32 ntier, tier_res[3], tier_len[3], pad;
    } H3;
    enum { G3 = 8, N3 = SKIM_STATS_BANDS * OM * SM4 + G3 };
    typedef struct {
      gint64 start;
      float  band[SKIM_STATS_BANDS][OM][SM4];
      float  glob[G3];
      guint8 n[N3];
    } S3;
    const guint res[3] = { 1, 15, 60 }, tl[3] = { SKIM_STATS_MINUTES, 90 * 24 * 4, 2 * 365 * 24 };
    gsize len = sizeof(H3);
    for (int t = 0; t < 3; t++) { len += tl[t] * sizeof(S3); }
    char *buf = g_malloc0(len);
    H3 *h = (H3 *)buf;
    memcpy(h->magic, "SKIMSTAT", 8);
    h->version = 4;
    h->bands = SKIM_STATS_BANDS;
    h->modes = OM;
    h->sm_n = SM4;
    h->sg_n = G3;
    h->ntier = 3;
    for (int t = 0; t < 3; t++) { h->tier_res[t] = res[t]; h->tier_len[t] = tl[t]; }
    g_strlcpy(h->names[0], "20m", 16);
    gsize off = sizeof(H3);
    for (int t = 0; t < 3; t++) {
      S3 *sl = (S3 *)(buf + off);
      for (guint i = 0; i < tl[t]; i++) {
        for (guint b = 0; b < SKIM_STATS_BANDS; b++)
          for (guint k = 0; k < OM; k++)
            for (guint j = 0; j < SM4; j++) sl[i].band[b][k][j] = NAN;
        for (guint j = 0; j < G3; j++) sl[i].glob[j] = NAN;
      }
      /* the minute now, in each ring's slot that holds it */
      S3 *one = &sl[(now / res[t]) % tl[t]];
      one->start = now - now % res[t];
      one->band[0][0][SKIM_SM_SPOTS] = 9;
      one->n[SKIM_SM_SPOTS] = 1;
      one->glob[SKIM_SG_CPU] = 55;
      one->n[SKIM_STATS_BANDS * OM * SM4 + SKIM_SG_CPU] = 1;
      one->glob[SKIM_SG_CPU_RTTY] = 11;
      one->n[SKIM_STATS_BANDS * OM * SM4 + SKIM_SG_CPU_RTTY] = 1;
      off += tl[t] * sizeof(S3);
    }
    g_file_set_contents(path, buf, (gssize)len, NULL);
    g_free(buf);
    const char *bands[] = { "20m" };
    SkimStats *s = skim_stats_open(path, bands, 1);
    char *j = skim_stats_json(s, now, 1, 60);
    check("its spots, CPU and split carried over",
          strstr(j, "\"spots\":{\"20m\":[9]}") && strstr(j, "\"cpu\":[55]") &&
          strstr(j, "\"cpu_rtty\":[11]"));
    check("… the thermal pauses empty (null)", strstr(j, "\"thermal\":{\"20m\":[null]}") != NULL);
    g_free(j);
    j = skim_stats_json(s, now, 720, 0);
    check("… in the 15-minute ring too", strstr(j, "\"res_min\":15") && strstr(j, ",9]"));
    g_free(j);
    /* the temperature: a bucket keeps its hottest minute */
    const float temps[] = { 50, 71.5f, 60 };
    for (int i = 0; i < 3; i++) {
      SkimStatsMinute m = minute_of(1, 0, 1, 1);
      m.glob[SKIM_SG_CPU_TEMP] = temps[i];
      m.band[0][0][SKIM_SM_THERMAL] = i == 2 ? 1.0f : 0.0f;  /* paused the last minute */
      skim_stats_put(s, now + 1 + i, &m);
    }
    skim_stats_free(s);                          /* written as this version  */
    s = skim_stats_open(path, bands, 1);
    j = skim_stats_json(s, now + 3, 1, 60);
    check("saved as this version: the old minute kept, the temperature's peak per bucket (71.5)",
          strstr(j, "\"spots\":{\"20m\":[12]}") && strstr(j, "\"cpu_temp\":[71.5]"));
    check("… the thermal pauses: the share of the bucket (1 of 3 minutes)",
          strstr(j, "\"thermal\":{\"20m\":[0.3333]}") != NULL);
    g_free(j);
    skim_stats_free(s);
    char *keep = g_strconcat(path, ".v4", NULL);
    GStatBuf kb;
    check("… the version 4 file kept beside it, as it was (for an older build)",
          g_stat(keep, &kb) == 0 && (gsize)kb.st_size == len);
    g_remove(keep);
    g_free(keep);
  }

  printf("== a version 5 file (CW and RTTY only, no helpers' CPU)\n");
  {
    typedef struct {
      char    magic[8];
      guint32 version, minutes, bands, modes, sm_n, sg_n;
      char    names[SKIM_STATS_BANDS][16];
      guint32 ntier, tier_res[3], tier_len[3], pad;
    } H5;
    enum { G5 = 8, SM5 = 7, N5 = SKIM_STATS_BANDS * OM * SM5 + G5 };
    typedef struct {
      gint64 start;
      float  band[SKIM_STATS_BANDS][OM][SM5];
      float  glob[G5];
      guint8 n[N5];
    } S5;
    const guint res[3] = { 1, 15, 60 }, tl[3] = { SKIM_STATS_MINUTES, 90 * 24 * 4, 2 * 365 * 24 };
    gsize len = sizeof(H5);
    for (int t = 0; t < 3; t++) { len += tl[t] * sizeof(S5); }
    char *buf = g_malloc0(len);
    H5 *h = (H5 *)buf;
    memcpy(h->magic, "SKIMSTAT", 8);
    h->version = 5;
    h->bands = SKIM_STATS_BANDS;
    h->modes = OM;
    h->sm_n = SM5;
    h->sg_n = G5;
    h->ntier = 3;
    for (int t = 0; t < 3; t++) { h->tier_res[t] = res[t]; h->tier_len[t] = tl[t]; }
    g_strlcpy(h->names[0], "20m", 16);
    g_strlcpy(h->names[1], "40m", 16);
    gsize off = sizeof(H5);
    for (int t = 0; t < 3; t++) {
      S5 *sl = (S5 *)(buf + off);
      for (guint i = 0; i < tl[t]; i++) {
        for (guint b = 0; b < SKIM_STATS_BANDS; b++)
          for (guint k = 0; k < OM; k++)
            for (guint j = 0; j < SM5; j++) sl[i].band[b][k][j] = NAN;
        for (guint j = 0; j < G5; j++) sl[i].glob[j] = NAN;
      }
      S5 *one = &sl[(now / res[t]) % tl[t]];
      one->start = now - now % res[t];
      /* 40m RTTY (band 1, mode 1): 4 spots, active 2, paused a quarter */
      one->band[1][1][SKIM_SM_SPOTS] = 4;
      one->n[(1 * OM + 1) * SM5 + SKIM_SM_SPOTS] = 1;
      one->band[1][1][SKIM_SM_ACTIVE] = 2;
      one->n[(1 * OM + 1) * SM5 + SKIM_SM_ACTIVE] = 1;
      one->band[1][1][SKIM_SM_THERMAL] = 0.25f;
      one->n[(1 * OM + 1) * SM5 + SKIM_SM_THERMAL] = 1;
      one->glob[SKIM_SG_CPU_TEMP] = 66;
      one->n[SKIM_STATS_BANDS * OM * SM5 + SKIM_SG_CPU_TEMP] = 1;
      off += tl[t] * sizeof(S5);
    }
    g_file_set_contents(path, buf, (gssize)len, NULL);
    g_free(buf);
    const char *bands[] = { "20m", "40m" };
    SkimStats *s = skim_stats_open(path, bands, 2);
    char *j = skim_stats_json(s, now, 1, 60);
    char *rs = series_of(strstr(j, "\"rtty\":"), "spots", "40m");
    char *rt = series_of(strstr(j, "\"rtty\":"), "thermal", "40m");
    check("40m RTTY's spots and its pause carried over in their own mode (not FT8's)",
          rs && strcmp(rs, "4") == 0 && rt && strcmp(rt, "0.25") == 0);
    check("… the CPU temperature too; FT8 and the helpers' CPU empty",
          strstr(j, "\"cpu_temp\":[66]") && strstr(j, "\"ft8\":null") && strstr(j, "\"cpu_helpers\":[null]"));
    g_free(rs); g_free(rt); g_free(j);
    /* FT8 from now on: 40m, two minutes */
    for (int i = 0; i < 2; i++) {
      SkimStatsMinute m = minute_of(0, 0, 0, NAN);
      m.band[1][SKIM_STATS_FT8][SKIM_SM_SPOTS] = 10;
      m.band[1][SKIM_STATS_FT8][SKIM_SM_STATIONS] = 6;
      m.band[1][SKIM_STATS_FT8][SKIM_SM_ACTIVE] = i ? 30 : 20;
      m.band[1][SKIM_STATS_FT8][SKIM_SM_SNR] = -12;
      m.glob[SKIM_SG_CPU_HELPERS] = 12;
      skim_stats_put(s, now + 1 + i, &m);
    }
    skim_stats_free(s);                          /* written as version 7     */
    s = skim_stats_open(path, bands, 2);
    j = skim_stats_json(s, now + 2, 1, 60);
    const char *f8 = strstr(j, "\"ft8\":{");
    char *fs = series_of(f8, "spots", "40m"), *fa = series_of(f8, "active", "40m"),
         *fn = series_of(f8, "snr", "40m"), *f20 = series_of(f8, "spots", "20m");
    check("saved as version 7 and reopened: FT8's spots summed (20), active averaged (25), SNR -12",
          fs && strcmp(fs, "20") == 0 && fa && strcmp(fa, "25") == 0 && fn && strcmp(fn, "-12") == 0);
    check("… 20m had no FT8 (null); the helpers' CPU 12", f20 && strcmp(f20, "null") == 0 &&
          strstr(j, "\"cpu_helpers\":[12]"));
    rs = series_of(strstr(j, "\"rtty\":"), "spots", "40m");
    check("… the old RTTY minute still there", rs && strcmp(rs, "4") == 0);
    g_free(fs); g_free(fa); g_free(fn); g_free(f20); g_free(rs); g_free(j);
    skim_stats_free(s);
    char *keep = g_strconcat(path, ".v5", NULL);
    GStatBuf kb;
    check("… the version 5 file kept beside it, as it was (for an older build)",
          g_stat(keep, &kb) == 0 && (gsize)kb.st_size == len);
    g_remove(keep);
    g_free(keep);
    g_remove(path);
  }

  printf("== a version 6 file (CW, RTTY and FT8, no JS8)\n");
  {
    typedef struct {
      char    magic[8];
      guint32 version, minutes, bands, modes, sm_n, sg_n;
      char    names[SKIM_STATS_BANDS][16];
      guint32 ntier, tier_res[3], tier_len[3], pad;
    } H6;
    enum { M6 = 3, N6 = SKIM_STATS_BANDS * M6 * SKIM_SM_N + SKIM_SG_N };
    typedef struct {
      gint64 start;
      float  band[SKIM_STATS_BANDS][M6][SKIM_SM_N];
      float  glob[SKIM_SG_N];
      guint8 n[N6];
    } S6;
    const guint res[3] = { 1, 15, 60 }, tl[3] = { SKIM_STATS_MINUTES, 90 * 24 * 4, 2 * 365 * 24 };
    gsize len = sizeof(H6);
    for (int t = 0; t < 3; t++) { len += tl[t] * sizeof(S6); }
    char *buf = g_malloc0(len);
    H6 *h = (H6 *)buf;
    memcpy(h->magic, "SKIMSTAT", 8);
    h->version = 6;
    h->bands = SKIM_STATS_BANDS;
    h->modes = M6;
    h->sm_n = SKIM_SM_N;
    h->sg_n = SKIM_SG_N;
    h->ntier = 3;
    for (int t = 0; t < 3; t++) { h->tier_res[t] = res[t]; h->tier_len[t] = tl[t]; }
    g_strlcpy(h->names[0], "20m", 16);
    g_strlcpy(h->names[1], "40m", 16);
    gsize off = sizeof(H6);
    for (int t = 0; t < 3; t++) {
      S6 *sl = (S6 *)(buf + off);
      for (guint i = 0; i < tl[t]; i++) {
        for (guint b = 0; b < SKIM_STATS_BANDS; b++)
          for (guint k = 0; k < M6; k++)
            for (guint j = 0; j < SKIM_SM_N; j++) sl[i].band[b][k][j] = NAN;
        for (guint j = 0; j < SKIM_SG_N; j++) sl[i].glob[j] = NAN;
      }
      S6 *one = &sl[(now / res[t]) % tl[t]];
      one->start = now - now % res[t];
      /* 20m FT8 (band 0, mode 2): 7 spots; 40m RTTY (band 1, mode 1): 3 */
      one->band[0][2][SKIM_SM_SPOTS] = 7;
      one->n[(0 * M6 + 2) * SKIM_SM_N + SKIM_SM_SPOTS] = 1;
      one->band[0][2][SKIM_SM_ACTIVE] = 11;
      one->n[(0 * M6 + 2) * SKIM_SM_N + SKIM_SM_ACTIVE] = 1;
      one->band[1][1][SKIM_SM_SPOTS] = 3;
      one->n[(1 * M6 + 1) * SKIM_SM_N + SKIM_SM_SPOTS] = 1;
      one->band[1][1][SKIM_SM_ACTIVE] = 1;
      one->n[(1 * M6 + 1) * SKIM_SM_N + SKIM_SM_ACTIVE] = 1;
      one->glob[SKIM_SG_CPU_HELPERS] = 9;
      one->n[SKIM_STATS_BANDS * M6 * SKIM_SM_N + SKIM_SG_CPU_HELPERS] = 1;
      off += tl[t] * sizeof(S6);
    }
    g_file_set_contents(path, buf, (gssize)len, NULL);
    g_free(buf);
    const char *bands[] = { "20m", "40m" };
    SkimStats *s = skim_stats_open(path, bands, 2);
    char *j = skim_stats_json(s, now, 1, 60);
    char *fs = series_of(strstr(j, "\"ft8\":"), "spots", "20m");
    char *rs = series_of(strstr(j, "\"rtty\":"), "spots", "40m");
    check("20m FT8's spots and 40m RTTY's carried over in their own modes",
          fs && strcmp(fs, "7") == 0 && rs && strcmp(rs, "3") == 0);
    check("… the helpers' CPU too; JS8 empty", strstr(j, "\"cpu_helpers\":[9]") && strstr(j, "\"js8\":null"));
    g_free(fs); g_free(rs); g_free(j);
    for (int i = 0; i < 2; i++) {                 /* JS8 from now on: 40m */
      SkimStatsMinute m = minute_of(0, 0, 0, NAN);
      m.band[1][SKIM_STATS_JS8][SKIM_SM_SPOTS] = 2;
      m.band[1][SKIM_STATS_JS8][SKIM_SM_STATIONS] = 1;
      m.band[1][SKIM_STATS_JS8][SKIM_SM_ACTIVE] = i ? 5 : 3;
      m.band[1][SKIM_STATS_JS8][SKIM_SM_SNR] = -20;
      skim_stats_put(s, now + 1 + i, &m);
    }
    skim_stats_free(s);                          /* written as version 7     */
    s = skim_stats_open(path, bands, 2);
    j = skim_stats_json(s, now + 2, 1, 60);
    const char *j8 = strstr(j, "\"js8\":{");
    char *js = series_of(j8, "spots", "40m"), *ja = series_of(j8, "active", "40m"),
         *jn = series_of(j8, "snr", "40m"), *j20 = series_of(j8, "spots", "20m");
    check("saved as version 7 and reopened: JS8's spots summed (4), active averaged (4), SNR -20",
          js && strcmp(js, "4") == 0 && ja && strcmp(ja, "4") == 0 && jn && strcmp(jn, "-20") == 0);
    check("… 20m had no JS8 (null)", j20 && strcmp(j20, "null") == 0);
    fs = series_of(strstr(j, "\"ft8\":"), "spots", "20m");
    check("… the old FT8 minute still there", fs && strcmp(fs, "7") == 0);
    g_free(js); g_free(ja); g_free(jn); g_free(j20); g_free(fs); g_free(j);
    skim_stats_free(s);
    char *keep = g_strconcat(path, ".v6", NULL);
    GStatBuf kb;
    check("… the version 6 file kept beside it, as it was (for an older build)",
          g_stat(keep, &kb) == 0 && (gsize)kb.st_size == len);
    g_remove(keep);
    g_free(keep);
    g_remove(path);
  }

  printf("== the 15-minute and hour rings\n");
  {
    char *p2 = g_build_filename(dir, "tiers.bin", NULL);
    const char *bands[] = { "20m" };
    SkimStats *s = skim_stats_open(p2, bands, 1);
    const gint64 hb = now - now % 60;            /* the current hour            */
    const gint64 t0 = hb - 180;                  /* the three before: a spot a minute */
    for (gint64 m = t0; m < hb; m++) {
      SkimStatsMinute r = minute_of(1, 0, (m - t0) < 90 ? 10 : 20, NAN);
      r.glob[SKIM_SG_LOST] = m == t0 + 100 ? 7.5f : 0;
      skim_stats_put(s, m, &r);
    }
    char *j = skim_stats_json(s, now, 720, 0);  /* 30 days: past the minutes  */
    check("30 days: the 15-minute ring, hour buckets (step 60, 720 of them)",
          strstr(j, "\"step_min\":60,\"n\":720,\"res_min\":15") != NULL);
    char *sp = series_of(j, "spots", "20m");
    check("… counts summed across the ring's slots (… 60,60,60,null)",
          sp && g_str_has_suffix(sp, ",60,60,60,null"));
    char *ac = series_of(j, "active", "20m");
    check("… levels averaged by the minutes they hold (10, then 10/20 halves, 20)",
          ac && g_str_has_suffix(ac, ",10,15,20,null"));
    g_free(sp); g_free(ac);
    check("… the worst lost minute kept", strstr(j, ",7.5,") != NULL);
    g_free(j);
    j = skim_stats_json(s, now, 24 * 365, 0);   /* a year: the hour ring      */
    check("a year: the hour ring (res 60), whole hours per bucket",
          strstr(j, "\"res_min\":60") && strstr(j, "\"step_min\":780,"));
    sp = series_of(j, "spots", "20m");
    check("… the three hours' 180 spots in it", sp && strstr(sp, "180"));
    g_free(sp); g_free(j);
    j = skim_stats_json(s, now, 24 * 900, 0);
    check("asked for more than two years: two years", strstr(j, "\"res_min\":60") != NULL);
    g_free(j);
    j = skim_stats_json(s, hb - 120, 1, 60);    /* a window in the past       */
    check("a window ending two hours ago: that hour's 60 spots, its t0",
          strstr(j, "\"spots\":{\"20m\":[60]}") != NULL);
    g_free(j);
    j = skim_stats_json(s, now, 3, 1);
    skim_stats_free(s);
    s = skim_stats_open(p2, bands, 1);
    char *j2 = skim_stats_json(s, now, 3, 1);
    char *j3 = skim_stats_json(s, now, 720, 0);
    check("reopened: the same minutes, and the rings kept",
          strcmp(j, j2) == 0 && strstr(j3, ",60,60,60,null"));
    g_free(j); g_free(j2); g_free(j3);
    skim_stats_free(s);
    g_remove(p2);
    g_free(p2);
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
