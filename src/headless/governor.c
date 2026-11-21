/* governor.c — see governor.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "governor.h"

#include <math.h>
#include <string.h>

typedef enum { RT_TEST, RT_ON, RT_OFF } RttyState;
static const char *const RT_NAME[] = { "test", "on", "off" };

typedef struct {
  guint    band;
  gint64   at;                         /* unix s it was paused                 */
  double   at_c;                       /* the mean then                        */
  double   drop_c;                     /* the mean's drop 10 min later; NAN    */
} Shed;

struct _SkimHdGovernor {
  SkimHdGovernorConfig cfg;
  /* adaptive RTTY */
  RttyState rt;
  gint64    test_from;                 /* the test's first whole minute        */
  guint     count;                     /* RTTY spots in the test so far        */
  gint      last;                      /* the last test's count, -1 = none yet */
  gint64    next_test;                 /* the next full hour (a minute)        */
  /* the thermal guard */
  guint     nb;
  char      names[SKIM_GOV_MAX_BANDS][16];
  gboolean  can[SKIM_GOV_MAX_BANDS];
  gboolean  shed_on[SKIM_GOV_MAX_BANDS];
  guint     spots[SKIM_GOV_MAX_BANDS][SKIM_GOV_SPOTS_MIN];   /* a ring a band  */
  guint     spos;
  double   *temps;                     /* a ring of per-second readings        */
  guint     tcap, tlen, tpos;
  double    mean;                      /* NAN: under a minute of readings      */
  guint8    over[SKIM_GOV_OVER_S];     /* a ring: the mean over the limit, a s */
  guint     opos, on;                  /* … its next slot, its seconds over    */
  gint64    over_since, below_since, last_change;            /* unix s, 0 = no;
                                        * over_since: the hot spell's start,
                                        * till 5 min without a second over     */
  Shed      shed[SKIM_GOV_MAX_BANDS];  /* the paused, oldest first             */
  guint     nshed;
  gboolean  told_none;                 /* "nothing left to pause" said         */
};

static gint64 next_hour(gint64 minute) { return (minute + 59) / 60 * 60; }

SkimHdGovernor *skim_hd_governor_new(const SkimHdGovernorConfig *cfg, gint64 now_min) {
  SkimHdGovernor *g = g_new0(SkimHdGovernor, 1);
  g->cfg = *cfg;
  if (g->cfg.rtty_minutes < 1) { g->cfg.rtty_minutes = 1; }
  if (g->cfg.average_min < 1) { g->cfg.average_min = 1; }
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
  g->mean = NAN;
  g->tcap = g->cfg.average_min * 60;
  g->temps = g_new0(double, g->tcap);
  return g;
}

void skim_hd_governor_free(SkimHdGovernor *g) {
  if (!g) { return; }
  g_free(g->temps);
  g_free(g);
}

void skim_hd_governor_set_bands(SkimHdGovernor *g, guint n, const char *const *names,
                                const gboolean *can) {
  g->nb = MIN(n, SKIM_GOV_MAX_BANDS);
  for (guint i = 0; i < g->nb; i++) {
    g_strlcpy(g->names[i], names[i], sizeof(g->names[i]));
    g->can[i] = can ? can[i] : TRUE;
  }
  if (g->cfg.thermal) {
    guint k = 0;
    for (guint i = 0; i < g->nb; i++) { k += g->can[i]; }
    g_message("thermal: guard on — over %.1f °C (the %u-min mean of the hottest CPU sensor) "
              "for %d of the last %d min pauses the band with the fewest spots in %d min, "
              "%u of %u bands may be paused (one always stays)", g->cfg.limit_c,
              g->cfg.average_min, SKIM_GOV_OVER_NEED_S / 60, SKIM_GOV_OVER_S / 60,
              SKIM_GOV_SPOTS_MIN, k > 0 ? k - 1 : 0, g->nb);
  }
}

static void hm(gint64 minute, char out[8]) {
  const gint64 m = ((minute % 1440) + 1440) % 1440;
  g_snprintf(out, 8, "%02d:%02d", (int)(m / 60), (int)(m % 60));
}

gboolean skim_hd_governor_minute(SkimHdGovernor *g, gint64 minute, guint rtty_spots,
                                 const guint *band_spots) {
  g->spos = (g->spos + 1) % SKIM_GOV_SPOTS_MIN;               /* the 30-min ring */
  for (guint i = 0; i < g->nb; i++) { g->spots[i][g->spos] = band_spots ? band_spots[i] : 0; }
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

/* ---- the thermal guard ----------------------------------------------------------- */

static guint spots30(const SkimHdGovernor *g, guint i) {
  guint s = 0;
  for (guint k = 0; k < SKIM_GOV_SPOTS_MIN; k++) { s += g->spots[i][k]; }
  return s;
}

/* The band to pause: the fewest spots in 30 min among those that may go
 * (a tie: the later one); -1 when only one would be left. */
static int pick(const SkimHdGovernor *g) {
  int best = -1;
  guint live = 0, low = G_MAXUINT;
  for (guint i = 0; i < g->nb; i++) {
    if (!g->can[i] || g->shed_on[i]) { continue; }
    live++;
    const guint s = spots30(g, i);
    if (s <= low) { low = s; best = (int)i; }
  }
  return live > 1 ? best : -1;
}

/* The reserve under the limit a paused band needs before it comes back:
 * what pausing it took off the mean (5 °C at least), and the margin. */
static double need_c(const SkimHdGovernor *g, const Shed *s) {
  const double drop = isnan(s->drop_c) ? SKIM_GOV_MIN_DROP_C : MAX(s->drop_c, SKIM_GOV_MIN_DROP_C);
  return drop + g->cfg.restore_margin_c;
}

gboolean skim_hd_governor_temp(SkimHdGovernor *g, gint64 now_s, double temp_c) {
  if (!g->cfg.thermal) { return FALSE; }
  if (!isnan(temp_c)) {
    g->temps[g->tpos] = temp_c;
    g->tpos = (g->tpos + 1) % g->tcap;
    if (g->tlen < g->tcap) { g->tlen++; }
  }
  if (g->tlen < 60) { return FALSE; }                 /* a minute at least      */
  double sum = 0;
  for (guint k = 0; k < g->tlen; k++) { sum += g->temps[k]; }
  g->mean = sum / g->tlen;
  const double lim = g->cfg.limit_c;
  /* what each pause did: the mean's drop 10 min after it */
  for (guint k = 0; k < g->nshed; k++) {
    Shed *s = &g->shed[k];
    if (isnan(s->drop_c) && now_s - s->at >= SKIM_GOV_NEXT_S) {
      s->drop_c = s->at_c - g->mean;
      if (s->drop_c > 0) {
        g_message("thermal: %s paused %d min: %.1f → %.1f °C (−%.1f °C); it comes back once "
                  "the mean stays ≤ %.1f °C for %d min", g->names[s->band], SKIM_GOV_NEXT_S / 60,
                  s->at_c, g->mean, s->drop_c, lim - need_c(g, s), SKIM_GOV_RESTORE_S / 60);
      } else {
        g_message("thermal: %s paused %d min: %.1f → %.1f °C, no measurable drop (counted as "
                  "%.0f °C)", g->names[s->band], SKIM_GOV_NEXT_S / 60, s->at_c, g->mean,
                  SKIM_GOV_MIN_DROP_C);
      }
    }
  }
  gboolean changed = FALSE;
  const gboolean over = g->mean > lim;
  g->on += (guint)over - g->over[g->opos];             /* the last 5 min's      */
  g->over[g->opos] = (guint8)over;
  g->opos = (g->opos + 1) % SKIM_GOV_OVER_S;
  if (!g->on) {                                       /* 5 min under: cooled   */
    g->over_since = 0;
    g->told_none = FALSE;
  }
  if (over) {
    g->below_since = 0;
    if (!g->over_since) { g->over_since = now_s; }
    if (g->on >= SKIM_GOV_OVER_NEED_S &&
        (!g->last_change || now_s - g->last_change >= SKIM_GOV_NEXT_S)) {
      const int b = pick(g);
      if (b >= 0) {
        Shed *s = &g->shed[g->nshed++];
        s->band = (guint)b;
        s->at = now_s;
        s->at_c = g->mean;
        s->drop_c = NAN;
        g->shed_on[b] = TRUE;
        g->last_change = now_s;
        changed = TRUE;
        g_message("thermal: %.1f °C (the %u-min mean) over %.1f °C for %.1f of the last %d "
                  "min — %s paused (%u spots in %d min, the fewest)", g->mean,
                  g->cfg.average_min, lim, g->on / 60.0, SKIM_GOV_OVER_S / 60, g->names[b],
                  spots30(g, (guint)b), SKIM_GOV_SPOTS_MIN);
      } else if (!g->told_none) {
        g->told_none = TRUE;
        g_warning("thermal: %.1f °C over %.1f °C, and only one band is left — it stays on",
                  g->mean, lim);
      }
    }
    return changed;
  }
  if (!g->nshed) { return FALSE; }
  Shed *s = &g->shed[g->nshed - 1];                   /* the last one first     */
  if (g->mean > lim - need_c(g, s)) {
    g->below_since = 0;
    return FALSE;
  }
  if (!g->below_since) { g->below_since = now_s; }
  if (now_s - g->below_since >= SKIM_GOV_RESTORE_S && now_s - g->last_change >= SKIM_GOV_RESTORE_S) {
    g_message("thermal: %.1f °C ≤ %.1f − %.1f °C for %d min — %s back on", g->mean, lim,
              need_c(g, s), SKIM_GOV_RESTORE_S / 60, g->names[s->band]);
    g->shed_on[s->band] = FALSE;
    g->nshed--;
    g->last_change = now_s;
    g->below_since = now_s;                           /* the next waits as long */
    changed = TRUE;
  }
  return changed;
}

gboolean skim_hd_governor_band_shed(const SkimHdGovernor *g, guint i) {
  return g->cfg.thermal && i < g->nb && g->shed_on[i];
}

double skim_hd_governor_mean_c(const SkimHdGovernor *g) { return g->cfg.thermal ? g->mean : NAN; }

/* ---- status.json --------------------------------------------------------------- */

static void num_or_null(GString *out, const char *fmt, double v) {
  if (isnan(v)) {
    g_string_append(out, "null");
  } else {
    char b[G_ASCII_DTOSTR_BUF_SIZE];
    g_string_append(out, g_ascii_formatd(b, sizeof(b), fmt, v));
  }
}

void skim_hd_governor_json(const SkimHdGovernor *g, GString *out) {
  if (g->cfg.rtty_adaptive) {
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
  if (g->cfg.thermal) {
    /* state: hot (over the limit now), cooling (under it with bands paused,
     * the reserve not yet held), ok */
    const char *st = g->mean > g->cfg.limit_c ? "hot" : g->nshed ? "cooling" : "ok";
    g_string_append(out, ",\"thermal\":{\"mean_c\":");
    num_or_null(out, "%.1f", g->mean);
    g_string_append(out, ",\"limit_c\":");
    num_or_null(out, "%.1f", g->cfg.limit_c);
    g_string_append_printf(out, ",\"average_min\":%u,\"state\":\"%s\",\"over_since\":%" G_GINT64_FORMAT
                           ",\"over_s\":%u,\"over_need_s\":%d,\"shed\":[", g->cfg.average_min, st,
                           g->over_since, g->on, SKIM_GOV_OVER_NEED_S);
    for (guint k = 0; k < g->nshed; k++) {
      const Shed *s = &g->shed[k];
      g_string_append_printf(out, "%s{\"band\":\"%s\",\"at\":%" G_GINT64_FORMAT ",\"at_c\":",
                             k ? "," : "", g->names[s->band], s->at);
      num_or_null(out, "%.1f", s->at_c);
      g_string_append(out, ",\"drop_c\":");
      num_or_null(out, "%.1f", s->drop_c);
      g_string_append(out, ",\"back_at_c\":");
      num_or_null(out, "%.1f", g->cfg.limit_c - need_c(g, s));
      g_string_append_c(out, '}');
    }
    g_string_append(out, "]}");
  }
}
