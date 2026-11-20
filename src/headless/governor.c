/* governor.c — see governor.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "governor.h"

typedef enum { RT_TEST, RT_ON, RT_OFF } RttyState;
static const char *const RT_NAME[] = { "test", "on", "off" };

struct _SkimHdGovernor {
  SkimHdGovernorConfig cfg;
  /* adaptive RTTY */
  RttyState rt;
  gint64    test_from;                 /* the test's first whole minute        */
  guint     count;                     /* RTTY spots in the test so far        */
  gint      last;                      /* the last test's count, -1 = none yet */
  gint64    next_test;                 /* the next full hour (a minute)        */
};

static gint64 next_hour(gint64 minute) { return (minute + 59) / 60 * 60; }

SkimHdGovernor *skim_hd_governor_new(const SkimHdGovernorConfig *cfg, gint64 now_min) {
  SkimHdGovernor *g = g_new0(SkimHdGovernor, 1);
  g->cfg = *cfg;
  if (g->cfg.rtty_minutes < 1) { g->cfg.rtty_minutes = 1; }
  g->last = -1;
  /* the start tests at once: the minute running now is partial, so the test
   * counts from the next one (the decoders run from now on) */
  g->rt = RT_TEST;
  g->test_from = now_min + 1;
  g->next_test = next_hour(now_min + 1);
  if (g->cfg.rtty_adaptive) {
    g_message("rtty: adaptive — RTTY on now; it stays on when all bands spot more than %u "
              "RTTY spots in %u min, a test at start and each full hour",
              g->cfg.rtty_spots, g->cfg.rtty_minutes);
  }
  return g;
}

void skim_hd_governor_free(SkimHdGovernor *g) { g_free(g); }

static void hm(gint64 minute, char out[8]) {
  const gint64 m = ((minute % 1440) + 1440) % 1440;
  g_snprintf(out, 8, "%02d:%02d", (int)(m / 60), (int)(m % 60));
}

gboolean skim_hd_governor_minute(SkimHdGovernor *g, gint64 minute, guint rtty_spots) {
  if (!g->cfg.rtty_adaptive) { return FALSE; }
  const gboolean was_on = skim_hd_governor_rtty_on(g);
  const gint64 next = minute + 1;                       /* the minute starting */
  if (g->rt == RT_TEST) {
    if (minute >= g->test_from) { g->count += rtty_spots; }
    if (next >= g->test_from + (gint64)g->cfg.rtty_minutes) {
      g->last = (gint)g->count;
      g->next_test = next_hour(next);
      char at[8];
      hm(g->next_test, at);
      if (g->count > g->cfg.rtty_spots) {
        g->rt = RT_ON;
        g_message("rtty: adaptive — %u RTTY spots in %u min > %u, a contest: RTTY stays on "
                  "(the next test at %s UTC)", g->count, g->cfg.rtty_minutes, g->cfg.rtty_spots, at);
      } else {
        g->rt = RT_OFF;
        g_message("rtty: adaptive — %u RTTY spots in %u min ≤ %u: RTTY off until %s UTC",
                  g->count, g->cfg.rtty_minutes, g->cfg.rtty_spots, at);
      }
    }
  }
  if (g->rt != RT_TEST && next % 60 == 0) {             /* a full hour: test again */
    if (g->rt == RT_OFF) {
      g_message("rtty: adaptive — the full hour: RTTY on for a %u-min test", g->cfg.rtty_minutes);
    }
    g->rt = RT_TEST;
    g->test_from = next;
    g->count = 0;
  }
  return was_on != skim_hd_governor_rtty_on(g);
}

gboolean skim_hd_governor_rtty_on(const SkimHdGovernor *g) {
  return !g->cfg.rtty_adaptive || g->rt != RT_OFF;
}

void skim_hd_governor_json(const SkimHdGovernor *g, GString *out) {
  if (!g->cfg.rtty_adaptive) { return; }
  g_string_append_printf(out, ",\"rtty_adaptive\":{\"state\":\"%s\",\"on\":%s,\"spots\":%u,"
                         "\"minutes\":%u,\"count\":%u,\"last\":",
                         RT_NAME[g->rt], skim_hd_governor_rtty_on(g) ? "true" : "false",
                         g->cfg.rtty_spots, g->cfg.rtty_minutes,
                         g->rt == RT_TEST ? g->count : 0);
  if (g->last < 0) {
    g_string_append(out, "null");
  } else {
    g_string_append_printf(out, "%d", g->last);
  }
  /* unix s: the test's end while testing, else the next test */
  const gint64 t = g->rt == RT_TEST ? g->test_from + g->cfg.rtty_minutes : g->next_test;
  g_string_append_printf(out, ",\"until\":%" G_GINT64_FORMAT "}", t * 60);
}
