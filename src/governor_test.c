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
  check("the start's partial minute: no change", !skim_hd_governor_minute(g, AT(13, 37), 500));
  check("first test minute: still on", !skim_hd_governor_minute(g, AT(13, 38), 30));
  check("test of 60 ≤ 100 ends: changed", skim_hd_governor_minute(g, AT(13, 39), 30));
  check("… off", !skim_hd_governor_rtty_on(g));
  GString *j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("status says off, last 60, until 14:00",
        strstr(j->str, "\"state\":\"off\"") && strstr(j->str, "\"last\":60") &&
        strstr(j->str, g_strdup_printf("\"until\":%" G_GINT64_FORMAT, AT(14, 0) * 60)));
  g_string_free(j, TRUE);
  for (int m = 40; m < 59; m++) { skim_hd_governor_minute(g, AT(13, m), 0); }
  check("off through the hour", !skim_hd_governor_rtty_on(g));
  check("13:59 ends: the full hour turns it on", skim_hd_governor_minute(g, AT(13, 59), 0));
  check("… on at 14:00", skim_hd_governor_rtty_on(g));
  check("14:00 counted", !skim_hd_governor_minute(g, AT(14, 0), 80));
  check("14:01: 80 + 21 = 101 > 100 — stays on (no change)", !skim_hd_governor_minute(g, AT(14, 1), 21));
  check("… on: a contest", skim_hd_governor_rtty_on(g));
  for (int m = 2; m < 59; m++) { skim_hd_governor_minute(g, AT(14, m), 0); }
  skim_hd_governor_minute(g, AT(14, 59), 0);                         /* 15:00: test */
  check("15:00 tests while on (still on)", skim_hd_governor_rtty_on(g));
  skim_hd_governor_minute(g, AT(15, 0), 50);
  check("15:00–15:01: exactly 100 is no contest — off",
        skim_hd_governor_minute(g, AT(15, 1), 50) && !skim_hd_governor_rtty_on(g));
  skim_hd_governor_free(g);

  /* a test that ends on a full hour starts the next one right away */
  g = skim_hd_governor_new(&c, AT(15, 57));
  skim_hd_governor_minute(g, AT(15, 57), 0);
  skim_hd_governor_minute(g, AT(15, 58), 0);
  skim_hd_governor_minute(g, AT(15, 59), 0);                         /* decides, 16:00 */
  check("a test deciding at the full hour: tests again at once (on)", skim_hd_governor_rtty_on(g));
  skim_hd_governor_free(g);

  /* off: always on, no status */
  SkimHdGovernorConfig off = { .rtty_adaptive = FALSE, .rtty_spots = 100, .rtty_minutes = 2 };
  g = skim_hd_governor_new(&off, AT(10, 0));
  gboolean changed = FALSE;
  for (int m = 0; m < 180; m++) { changed |= skim_hd_governor_minute(g, AT(10, m), 0); }
  j = g_string_new(NULL);
  skim_hd_governor_json(g, j);
  check("without rtty_adaptive: always on, never a change, no status",
        skim_hd_governor_rtty_on(g) && !changed && j->len == 0);
  g_string_free(j, TRUE);
  skim_hd_governor_free(g);
}

int main(void) {
  g_log_set_default_handler(quiet, NULL);
  rtty();
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
