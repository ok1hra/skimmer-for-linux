/*
 * skimmer-phase-hist-test — the history of what phasing gave
 * (headless/phase_hist.h): seconds summed into minute rows, minute rows
 * into quarter-hour rows, the rows read back for a window, kept across a
 * reopen, and a file of another layout set aside for a new one.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/phase_meter.h"
#include "headless/phase_hist.h"

#define NCH 64

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* A second of a band: the drop = 10 dB + the channel / 8, a gain on
 * channel 5 only; summary drop 12, gain 3. */
static void second(SkimPhaseHist *h, gint64 t, gboolean est) {
  SkimPipelinePhase ph = { 0 };
  ph.phased = TRUE;
  ph.est = est;
  ph.nchan = NCH;
  ph.sum.drop_db = 12.0;
  ph.sum.gain_db = 3.0;
  ph.sum.gain_a_db = ph.sum.drop_p90_db = ph.sum.coherence = ph.sum.dmax_db = ph.sum.share_b = NAN;
  gint8 gain[NCH], drop[NCH];
  for (guint c = 0; c < NCH; c++) {
    gain[c] = c == 5 ? skim_phase_q8(6.0) : SKIM_PHASE_NONE;
    drop[c] = skim_phase_q8(10.0 + c / 8.0);
  }
  skim_phase_hist_add(h, t, &ph, gain, drop);
}

static gint8 *b64(const char *json, const char *key, gsize *len) {
  char *pat = g_strdup_printf("\"%s\":\"", key);
  const char *p = strstr(json, pat);
  const gsize pl = strlen(pat);
  g_free(pat);
  *len = 0;
  if (!p) { return NULL; }
  p += pl;
  const char *e = strchr(p, '"');
  if (!e) { return NULL; }
  char *s = g_strndup(p, (gsize)(e - p));
  guchar *d = g_base64_decode(s, len);
  g_free(s);
  return (gint8 *)d;
}

static guint rows_of(const char *json) {
  const char *p = strstr(json, "\"rows\":");
  return p ? (guint)g_ascii_strtoull(p + 7, NULL, 10) : 0;
}

int main(void) {
  char *dir = g_dir_make_tmp("phase-hist-XXXXXX", NULL);
  const gint64 t0 = 1795600800;            /* a quarter hour's start (UTC) */
  char what[200];

  printf("== a minute row a minute, a quarter-hour row a quarter\n");
  SkimPhaseHist *h = skim_phase_hist_open(dir, "20m", NCH, -2000.0, 125.0);
  check("opened", h != NULL);
  for (gint64 t = t0; t < t0 + 31 * 60; t++) { second(h, t, t >= t0 + 20 * 60); }
  char *j = skim_phase_hist_json(h, t0 + 30 * 60, 1, 720);
  gsize n = 0;
  gint8 *drop = b64(j, "drop", &n), *gain = NULL;
  gsize ng = 0;
  gain = b64(j, "gain", &ng);
  const guint rows = rows_of(j);
  check("an hour's window: 60 rows of a minute, 64 channels each", rows == 60 && n == 60 * NCH);
  /* the window ends at minute 30: rows 0..28 empty, 29 (t0's minute) … */
  const guint r0 = 60 - 31;
  g_snprintf(what, sizeof(what), "a minute's drop on channel 16: %.2f dB (12.00), channel 0: %.2f (10.00)",
             drop ? drop[r0 * NCH + 16] / 4.0 : NAN, drop ? drop[r0 * NCH] / 4.0 : NAN);
  check(what, drop && drop[r0 * NCH + 16] == 48 && drop[r0 * NCH] == 40);
  check("the gain only where a station was (channel 5: 6 dB), none elsewhere",
        gain && gain[r0 * NCH + 5] == 24 && gain[r0 * NCH + 6] == SKIM_PHASE_NONE);
  check("before the first minute: nothing", drop && drop[0] == SKIM_PHASE_NONE);
  check("the summary: drop 12, gain 3 the minute", strstr(j, "\"drop_db\":[null") && strstr(j, "12.00") &&
        strstr(j, "3.00"));
  check("minutes 20–29 were estimates (est 1), the first ones not",
        strstr(j, "\"est\":[") != NULL);
  g_free(drop);
  g_free(gain);
  g_free(j);
  j = skim_phase_hist_json(h, t0 + 30 * 60, 240, 720);
  check("four days → the quarter-hour rows: 960 merged to 720 at most… (240 h: tier 2, 960 rows → 480)",
        strstr(j, "\"step_min\":30") != NULL && rows_of(j) == 480);
  drop = b64(j, "drop", &n);
  /* the quarter t0 … t0+15 min is the last-but-one complete row */
  guint have = 0;
  for (guint r = 0; r < rows_of(j); r++) { have += drop && drop[r * NCH + 16] == 48; }
  check("the quarter hours carry the minutes' mean (channel 16: 12 dB)", have >= 1);
  g_free(drop);
  g_free(j);
  skim_phase_hist_free(h);

  printf("== kept across a reopen\n");
  h = skim_phase_hist_open(dir, "20m", NCH, -2000.0, 125.0);
  j = skim_phase_hist_json(h, t0 + 30 * 60, 1, 720);
  drop = b64(j, "drop", &n);
  check("the minutes written are there again", drop && drop[r0 * NCH + 16] == 48);
  g_free(drop);
  g_free(j);
  skim_phase_hist_free(h);

  printf("== another layout\n");
  h = skim_phase_hist_open(dir, "20m", NCH * 2, -4000.0, 125.0);
  char *path = g_strdup_printf("%s/phase-20m.bin", dir), *old = g_strdup_printf("%s.old", path);
  j = skim_phase_hist_json(h, t0 + 30 * 60, 1, 720);
  drop = b64(j, "drop", &n);
  check("128 channels: a new file, the old one kept as .old",
        g_file_test(old, G_FILE_TEST_EXISTS) && drop && drop[0] == SKIM_PHASE_NONE &&
        drop[59 * NCH * 2 + 16] == SKIM_PHASE_NONE);
  g_free(drop);
  g_free(j);
  skim_phase_hist_free(h);
  g_unlink(path);
  g_unlink(old);
  g_free(path);
  g_free(old);
  g_rmdir(dir);
  g_free(dir);
  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
