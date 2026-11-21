/*
 * skimmer-governor-test — what decodes when (src/headless/governor.h).
 *
 * Adaptive RTTY: on at the start and tested at once (the partial minute of
 * the start not counted); a test at or under the limit turns it off until
 * the next full hour, over it keeps it on; each full hour tests again, on or
 * off; without rtty_adaptive it is always on and says nothing.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "headless/governor.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void quiet(const gchar *d, GLogLevelFlags l, const gchar *m, gpointer u) {
  (void)d; (void)l; (void)u;
  if (g_getenv("GOVERNOR_VERBOSE")) { printf("    | %s\n", m); }
}

/* 13:57 UTC on some day, in unix minutes */
#define DAY (gint64)(29400000 / 1440 * 1440)
#define AT(h, m) (DAY + (h) * 60 + (m))

static void rtty(void) {
  printf("adaptive RTTY\n");
  SkimHdGovernorConfig c = { .rtty_adaptive = TRUE, .rtty_spots = 100, .rtty_minutes = 2 };
  SkimHdGovernor *g = skim_hd_governor_new(&c, AT(13, 37));       /* start at 13:37:xx */
  check("on at the start", skim_hd_governor_rtty_on(g));
  /* 13:37 is partial: its 500 spots do not count; 13:38 + 13:39 = 60 */
  check("the start's partial minute: no change", !skim_hd_governor_minute(g, AT(13, 37), 500, NULL));
  check("first test minute: still on", !skim_hd_governor_minute(g, AT(13, 38), 30, NULL));
  check("test of 60 ≤ 100 ends: changed", skim_hd_governor_minute(g, AT(13, 39), 30, NULL));
  check("… off", !skim_hd_governor_rtty_on(g));
  GString *j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("status says off, last 60, until 14:00",
        strstr(j->str, "\"state\":\"off\"") && strstr(j->str, "\"last\":60") &&
        strstr(j->str, g_strdup_printf("\"until\":%" G_GINT64_FORMAT, AT(14, 0) * 60)));
  g_string_free(j, TRUE);
  for (int m = 40; m < 59; m++) { skim_hd_governor_minute(g, AT(13, m), 0, NULL); }
  check("off through the hour", !skim_hd_governor_rtty_on(g));
  check("13:59 ends: the full hour turns it on", skim_hd_governor_minute(g, AT(13, 59), 0, NULL));
  check("… on at 14:00", skim_hd_governor_rtty_on(g));
  check("14:00 counted", !skim_hd_governor_minute(g, AT(14, 0), 80, NULL));
  check("14:01: 80 + 21 = 101 > 100 — stays on (no change)", !skim_hd_governor_minute(g, AT(14, 1), 21, NULL));
  check("… on: a contest", skim_hd_governor_rtty_on(g));
  for (int m = 2; m < 59; m++) { skim_hd_governor_minute(g, AT(14, m), 0, NULL); }
  skim_hd_governor_minute(g, AT(14, 59), 0, NULL);                         /* 15:00: test */
  check("15:00 tests while on (still on)", skim_hd_governor_rtty_on(g));
  skim_hd_governor_minute(g, AT(15, 0), 50, NULL);
  check("15:00–15:01: exactly 100 is no contest — off",
        skim_hd_governor_minute(g, AT(15, 1), 50, NULL) && !skim_hd_governor_rtty_on(g));
  skim_hd_governor_free(g);

  /* a test that ends on a full hour starts the next one right away */
  g = skim_hd_governor_new(&c, AT(15, 57));
  skim_hd_governor_minute(g, AT(15, 57), 0, NULL);
  skim_hd_governor_minute(g, AT(15, 58), 0, NULL);
  skim_hd_governor_minute(g, AT(15, 59), 0, NULL);                         /* decides, 16:00 */
  check("a test deciding at the full hour: tests again at once (on)", skim_hd_governor_rtty_on(g));
  skim_hd_governor_free(g);

  /* off: always on, no status */
  SkimHdGovernorConfig off = { .rtty_adaptive = FALSE, .rtty_spots = 100, .rtty_minutes = 2 };
  g = skim_hd_governor_new(&off, AT(10, 0));
  gboolean changed = FALSE;
  for (int m = 0; m < 180; m++) { changed |= skim_hd_governor_minute(g, AT(10, m), 0, NULL); }
  j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("without rtty_adaptive: always on, never a change, no status",
        skim_hd_governor_rtty_on(g) && !changed && j->len == 0);
  g_string_free(j, TRUE);
  skim_hd_governor_free(g);
}

/* the guard's clock: a second at a time, temp °C, `secs` long; TRUE when any
 * second changed something */
static gint64 now;
static gboolean run(SkimHdGovernor *g, double temp, int secs) {
  gboolean ch = FALSE;
  for (int k = 0; k < secs; k++) { ch |= skim_hd_governor_temp(g, ++now, temp); }
  return ch;
}
static guint shed_count(SkimHdGovernor *g, guint nb) {
  guint n = 0;
  for (guint i = 0; i < nb; i++) { n += skim_hd_governor_band_shed(g, i); }
  return n;
}

static void thermal(void) {
  printf("thermal guard\n");
  SkimHdGovernorConfig c = { .thermal = TRUE, .limit_c = 85, .average_min = 5, .restore_margin_c = 3 };
  SkimHdGovernor *g = skim_hd_governor_new(&c, AT(10, 0));
  const char *names[] = { "80m", "40m", "20m", "15m" };
  const gboolean can[] = { TRUE, TRUE, TRUE, FALSE };             /* 15m: a worker's */
  skim_hd_governor_set_bands(g, 4, names, can);
  /* the last 30 min: 40m quietest, then 80m; 15m (a worker's) quieter still */
  const guint spots[] = { 20, 5, 90, 0 };
  for (int m = 0; m < 30; m++) { skim_hd_governor_minute(g, AT(10, m), 0, spots); }
  now = AT(10, 30) * 60;
  check("a minute of readings before there is a mean",
        !run(g, 70, 59) && isnan(skim_hd_governor_mean_c(g)) && !run(g, 70, 1) &&
        skim_hd_governor_mean_c(g) == 70);
  run(g, 70, 300 - 60);                                          /* a full 5-min window */
  /* a burst: 100 °C for 60 s moves the mean to 76 — nothing happens */
  check("a one-minute burst to 100 °C: nothing paused", !run(g, 100, 60) && !shed_count(g, 4));
  run(g, 70, 300);
  /* hot: 90 °C from now; the 5-min mean is over 85 from the 226th s, then
   * 3 min of the last 5 */
  check("hot, but not yet 3 min over the limit: nothing", !run(g, 90, 225 + 179) && !shed_count(g, 4));
  check("3 min over the limit: a band paused", run(g, 90, 1) && shed_count(g, 4) == 1);
  check("… 40m, the fewest spots (not the worker's 15m)",
        skim_hd_governor_band_shed(g, 1) && !skim_hd_governor_band_shed(g, 3));
  /* still hot: the next one 10 min later, not sooner */
  check("still hot: nothing more for 10 min", !run(g, 90, 599) && shed_count(g, 4) == 1);
  check("10 min on, still hot: 80m paused too", run(g, 90, 1) && skim_hd_governor_band_shed(g, 0));
  GString *j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("status: hot, 40m with its drop (none: 90 → 90), 80m", strstr(j->str, "\"state\":\"hot\"") &&
        strstr(j->str, "\"band\":\"40m\"") && strstr(j->str, "\"drop_c\":0.0") &&
        strstr(j->str, "\"band\":\"80m\""));
  g_string_free(j, TRUE);
  check("10 min more, still hot: one band always stays (20m)",
        !run(g, 90, 1200) && shed_count(g, 4) == 2 && !skim_hd_governor_band_shed(g, 2));
  /* cooling: 80m's drop measured 10 min after it was paused was 0 → 5 °C;
   * it needs ≤ 85 − 5 − 3 = 77 °C held for 20 min */
  run(g, 78, 600);
  check("78 °C (under the limit, not by 8 °C): nothing comes back", !run(g, 78, 1500) && shed_count(g, 4) == 2);
  /* 72 °C: the mean is ≤ 77 from the 50th s */
  check("≤ 77 °C for 20 min: not yet at 19:59", !run(g, 72, 49 + 1200) && shed_count(g, 4) == 2);
  check("… at 20:00 the last paused (80m) is back", run(g, 72, 1) &&
        !skim_hd_governor_band_shed(g, 0) && skim_hd_governor_band_shed(g, 1));
  check("the next one only 20 min later", !run(g, 72, 1199) && run(g, 72, 1) && !shed_count(g, 4));
  skim_hd_governor_free(g);

  /* a band whose pause cut 11 °C needs 85 − 11 − 3 = 71 °C */
  g = skim_hd_governor_new(&c, AT(10, 0));
  skim_hd_governor_set_bands(g, 3, names, can);
  now = AT(10, 30) * 60;
  run(g, 95, 360);                                  /* a mean from 60 s, 5 min over */
  check("one paused", shed_count(g, 3) == 1);
  run(g, 84, 600);                                                /* mean 95 → 84 */
  j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("its drop recorded: −11 °C, back at ≤ 71 °C",
        strstr(j->str, "\"drop_c\":11.0") && strstr(j->str, "\"back_at_c\":71.0"));
  g_string_free(j, TRUE);
  run(g, 73, 300 + 1500);
  check("73 °C is not reserve enough for it", shed_count(g, 3) == 1);
  run(g, 70, 1500);                                  /* ≤ 71 from the 200th s */
  check("70 °C for 20 min is", shed_count(g, 3) == 0);
  skim_hd_governor_free(g);

  /* hovering at the limit (the 1-min mean, as hra2 runs it): 90 s at 90 °C,
   * 30 s at 75 — the mean dips under 85 each 2 min, over it most of the
   * time. A dip does not start the count again: 40m at once, 80m 10 min on.
   * (Counted on end, the guard held one band off for good, 2026-11-20.) */
  SkimHdGovernorConfig c1 = { .thermal = TRUE, .limit_c = 85, .average_min = 1, .restore_margin_c = 3 };
  g = skim_hd_governor_new(&c1, AT(10, 0));
  skim_hd_governor_set_bands(g, 4, names, can);
  for (int m = 0; m < 30; m++) { skim_hd_governor_minute(g, AT(10, m), 0, spots); }
  now = AT(10, 30) * 60;
  run(g, 70, 60);
  for (int k = 0; k < 4; k++) { run(g, 90, 90); run(g, 75, 30); }       /* 8 min */
  check("hovering, mostly over: a band paused", shed_count(g, 4) == 1 && skim_hd_governor_band_shed(g, 1));
  for (int k = 0; k < 6; k++) { run(g, 90, 90); run(g, 75, 30); }       /* 12 min more */
  check("… still mostly over 10 min on: the next too", shed_count(g, 4) == 2 && skim_hd_governor_band_shed(g, 0));
  j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("status: the seconds over the limit in the last 5 min", strstr(j->str, "\"over_s\":") &&
        strstr(j->str, "\"over_need_s\":180"));
  g_string_free(j, TRUE);
  skim_hd_governor_free(g);
  /* over only briefly: 60 s at 95 °C, 120 s at 70 — the mean is over 85 for
   * well under 3 of each 5 min: nothing */
  g = skim_hd_governor_new(&c1, AT(10, 0));
  skim_hd_governor_set_bands(g, 4, names, can);
  now = AT(10, 30) * 60;
  run(g, 70, 60);
  for (int k = 0; k < 20; k++) { run(g, 95, 60); run(g, 70, 120); }    /* 1 h */
  check("over the limit now and then, mostly under: nothing paused", !shed_count(g, 4));
  skim_hd_governor_free(g);

  /* off: nothing ever, no status */
  SkimHdGovernorConfig off = { .thermal = FALSE, .limit_c = 85, .average_min = 5 };
  g = skim_hd_governor_new(&off, AT(10, 0));
  skim_hd_governor_set_bands(g, 3, names, can);
  j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("without [thermal]: never pauses, no status", !run(g, 100, 3600) && !shed_count(g, 3) &&
        j->len == 0 && isnan(skim_hd_governor_mean_c(g)));
  g_string_free(j, TRUE);
  skim_hd_governor_free(g);
}

int main(void) {
  g_log_set_default_handler(quiet, NULL);
  rtty();
  thermal();
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
