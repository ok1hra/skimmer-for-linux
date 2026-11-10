/* oracle.c — see oracle.h. Measurement only (docs/WEAK-SIGNAL-PLAN.md, 0.1).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "oracle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  double t0, t1, hz, wpm;
  char   call[16];
} Iv;

static guint  g_modes;
static Iv    *g_iv;
static guint  g_n;
static double g_maxlen;
static double g_from;

static int iv_cmp(const void *a, const void *b) {
  const double x = ((const Iv *)a)->t0, y = ((const Iv *)b)->t0;
  return x < y ? -1 : x > y;
}

static void load(void) {
  const char *path = g_getenv("SKIM_ORACLE");
  const char *mode = g_getenv("SKIM_ORACLE_MODE");
  if (!path || !path[0] || !mode || !mode[0])
    return;
  guint m = 0;
  gchar **v = g_strsplit(mode, ",", -1);
  for (gchar **q = v; *q; q++) {
    if (!strcmp(*q, "gate")) { m |= SKIM_ORACLE_GATE; }
    else if (!strcmp(*q, "clock")) { m |= SKIM_ORACLE_CLOCK; }
    else if (!strcmp(*q, "freq")) { m |= SKIM_ORACLE_FREQ; }
    else if (!strcmp(*q, "ghost")) { m |= SKIM_ORACLE_GHOST; }
    else if (!strcmp(*q, "gatesoft")) { m |= SKIM_ORACLE_GSOFT; }
    else if (!strcmp(*q, "combine")) { m |= SKIM_ORACLE_COMBINE; }
    else if (!strcmp(*q, "all")) {
      m |= SKIM_ORACLE_GATE | SKIM_ORACLE_CLOCK | SKIM_ORACLE_FREQ;
    } else if (strcmp(*q, "none")) {
      g_error("SKIM_ORACLE_MODE: unknown \"%s\"", *q);
    }
  }
  g_strfreev(v);
  const char *mp = g_getenv("SKIM_ORACLE_MINPROM");
  const double minprom = mp && mp[0] ? g_ascii_strtod(mp, NULL) : 6.0;
  const char *fr = g_getenv("SKIM_REPLAY_FROM");
  g_from = fr && fr[0] ? g_ascii_strtod(fr, NULL) : 0.0;

  FILE *f = fopen(path, "r");
  if (!f) { g_error("SKIM_ORACLE: cannot open %s", path); }
  GArray *a = g_array_new(FALSE, FALSE, sizeof(Iv));
  char line[512], call[64];
  guint skipped = 0;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#')
      continue;
    Iv iv;
    double snr, prom;
    if (sscanf(line, "%lf %lf %lf %lf %63s %lf %lf", &iv.t0, &iv.t1, &iv.hz,
               &iv.wpm, call, &snr, &prom) != 7)
      continue;
    if (prom < minprom || iv.wpm <= 0) {
      skipped++;
      continue;
    }
    g_strlcpy(iv.call, call, sizeof(iv.call));
    g_array_append_val(a, iv);
    g_maxlen = MAX(g_maxlen, iv.t1 - iv.t0);
  }
  fclose(f);
  g_n = a->len;
  g_iv = (Iv *)g_array_free(a, FALSE);
  qsort(g_iv, g_n, sizeof(Iv), iv_cmp);
  g_modes = m;
  g_printerr("oracle: %s mode 0x%x — %u intervals, %u under %.1f dB prominence "
             "skipped\n", path, m, g_n, skipped, minprom);
}

guint skim_oracle_modes(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    load();
    g_once_init_leave(&once, 1);
  }
  return g_modes;
}

double skim_oracle_time(double t_s) {
  return g_from + t_s;
}

gboolean skim_oracle_at(double chan_hz, double half_hz, double t_s,
                        SkimOracleHit *hit) {
  if (!g_n)
    return FALSE;
  /* First interval that may still cover t_s: t0 >= t_s - maxlen. */
  guint lo = 0, hi = g_n;
  const double from = t_s - g_maxlen;
  while (lo < hi) {
    const guint mid = (lo + hi) / 2;
    if (g_iv[mid].t0 < from) { lo = mid + 1; } else { hi = mid; }
  }
  double best = half_hz;
  gboolean got = FALSE;
  for (guint i = lo; i < g_n && g_iv[i].t0 <= t_s; i++) {
    if (g_iv[i].t1 < t_s)
      continue;
    const double d = fabs(g_iv[i].hz - chan_hz);
    if (d <= best) {
      best = d;
      hit->carrier_hz = g_iv[i].hz;
      hit->wpm = g_iv[i].wpm;
      g_strlcpy(hit->call, g_iv[i].call, sizeof(hit->call));
      got = TRUE;
    }
  }
  return got;
}
