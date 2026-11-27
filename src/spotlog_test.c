/*
 * skimmer-spotlog-test — the page's spots across a restart (src/headless/spotlog.h).
 *
 * Saved and read back the same, newest first; a spot older than keep_s or
 * ahead of the clock is not taken back, nor a line that does not read; a
 * tab in a peer's name does not split its line; the log keeps max spots;
 * a save with nothing new writes nothing; a missing file is no spots.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "headless/spotlog.h"

#include <math.h>

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static SkimHdSpot spot(gint64 t, const char *call, double hz, const char *src) {
  SkimHdSpot s = { 0 };
  s.t = t;
  g_strlcpy(s.call, call, sizeof(s.call));
  g_strlcpy(s.mode, "CW", sizeof(s.mode));
  s.freq_hz = hz;
  s.snr_db = 17;
  s.speed = 25;
  g_strlcpy(s.src, src, sizeof(s.src));
  return s;
}

typedef struct { GPtrArray *calls; } Got;
static gboolean collect(const SkimHdSpot *s, gpointer user) {
  Got *g = user;
  g_ptr_array_add(g->calls, g_strdup_printf("%s@%.1f/%s", s->call, s->freq_hz, s->src));
  return TRUE;
}
static char *all(SkimHdSpotLog *l) {
  Got g = { g_ptr_array_new_with_free_func(g_free) };
  skim_hd_spotlog_foreach_newest(l, collect, &g);
  g_ptr_array_add(g.calls, NULL);
  char *s = g_strjoinv(" ", (char **)g.calls->pdata);
  g_ptr_array_unref(g.calls);
  return s;
}

static gboolean phase_of(const SkimHdSpot *s, gpointer user) {
  if (!s->phased) { g_string_append_printf(user, "%s- ", s->call); return TRUE; }
  g_string_append_printf(user, "%s%+.1f/%s ", s->call, s->phase_db,
                         isnan(s->phase_a_db) ? "nan" : "x");
  return TRUE;
}

static gboolean first_mode(const SkimHdSpot *s, gpointer user) {
  g_strlcpy(user, s->mode, 8);
  return FALSE;
}

int main(void) {
  char *dir = g_dir_make_tmp("skim-spotlog-XXXXXX", NULL);
  char *path = g_build_filename(dir, "state", "spots.tsv", NULL);   /* the directory is made */
  const gint64 now = 1763583000;

  printf("== saved, read back\n");
  SkimHdSpotLog *a = skim_hd_spotlog_new(7200, 100);
  SkimHdSpot s1 = spot(now - 600, "OK1MCK", 7046500, "");
  SkimHdSpot s2 = spot(now - 60, "DL1ABC", 14025300, "hub\tworker");
  skim_hd_spotlog_add(a, &s1);
  skim_hd_spotlog_add(a, &s2);
  GError *err = NULL;
  check("saved", skim_hd_spotlog_save(a, path, &err));
  g_clear_error(&err);
  SkimHdSpotLog *b = skim_hd_spotlog_new(7200, 100);
  check("both spots back", skim_hd_spotlog_load(b, path, now) == 2);
  char *got = all(b);
  check("the same, newest first, a tab in the source a space", strcmp(got, "DL1ABC@14025300.0/hub worker OK1MCK@7046500.0/") == 0);
  printf("       (%s)\n", got);
  g_free(got);
  skim_hd_spotlog_free(b);

  printf("== not taken back: old, ahead of the clock, bad lines\n");
  b = skim_hd_spotlog_new(300, 100);       /* keep 5 min: OK1MCK (10 min) is too old */
  check("an old spot left out", skim_hd_spotlog_load(b, path, now) == 1);
  skim_hd_spotlog_free(b);
  b = skim_hd_spotlog_new(7200, 100);
  check("spots ahead of the clock left out", skim_hd_spotlog_load(b, path, now - 3600) == 0);
  skim_hd_spotlog_free(b);
  {
    char *text = g_strdup_printf("# skimmer-headless spots 1\n"
                                 "%" G_GINT64_FORMAT "\t7030000.0\tG3XYZ\tCW\t20.0\t22.0\t\n"
                                 "garbage\n"
                                 "%" G_GINT64_FORMAT "\tx\tG4AAA\tCW\t20.0\t22.0\t\n"
                                 "%" G_GINT64_FORMAT "\t7031000.0\t\tCW\t20.0\t22.0\t\n"
                                 "%" G_GINT64_FORMAT "\t7032000.0\tG5BBB\tC<W\t20.0\t22.0\tpeer\n"
                                 "%" G_GINT64_FORMAT "\t7033000.0\tG6OLD\tCW\t20.0\t22.0\t\n",
                                 now - 100, now - 90, now - 80, now - 70, now - 200);
    g_file_set_contents(path, text, -1, NULL);
    g_free(text);
    b = skim_hd_spotlog_new(7200, 100);
    check("the good lines only (a line out of time order too is left out)",
          skim_hd_spotlog_load(b, path, now) == 2);
    got = all(b);
    check("G5BBB and G3XYZ", strcmp(got, "G5BBB@7032000.0/peer G3XYZ@7030000.0/") == 0);
    g_free(got);
    char mode[8] = "";
    skim_hd_spotlog_foreach_newest(b, first_mode, mode);
    check("a mode with markup read as letters and digits only", strcmp(mode, "C?W") == 0);
    skim_hd_spotlog_free(b);
  }

  printf("== a phased band's spot: phasing's gain across a restart\n");
  {
    SkimHdSpotLog *p = skim_hd_spotlog_new(7200, 100);
    SkimHdSpot q1 = spot(now - 30, "JA1PH", 7012300, "");
    q1.phased = TRUE;
    q1.phase_db = 2.14;
    q1.phase_a_db = NAN;
    SkimHdSpot q2 = spot(now - 20, "W1UNP", 7013300, "");
    skim_hd_spotlog_add(p, &q1);
    skim_hd_spotlog_add(p, &q2);
    check("saved", skim_hd_spotlog_save(p, path, NULL));
    char *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    check("the phased line one field more (+2.1/nan), the other as before",
          text && strstr(text, "\tJA1PH\tCW\t17.0\t25.0\t\t+2.1/nan\n") &&
              strstr(text, "\tW1UNP\tCW\t17.0\t25.0\t\n"));
    g_free(text);
    skim_hd_spotlog_free(p);
    p = skim_hd_spotlog_new(7200, 100);
    check("both back", skim_hd_spotlog_load(p, path, now) == 2);
    GString *ph = g_string_new(NULL);
    skim_hd_spotlog_foreach_newest(p, phase_of, ph);
    check("… W1UNP not phased, JA1PH +2.1 over the better antenna, none over A",
          strcmp(ph->str, "W1UNP- JA1PH+2.1/nan ") == 0);
    printf("       (%s)\n", ph->str);
    g_string_free(ph, TRUE);
    skim_hd_spotlog_free(p);
  }

  printf("== at most max; a save with nothing new; no file\n");
  SkimHdSpotLog *c = skim_hd_spotlog_new(7200, 3);
  for (int i = 0; i < 5; i++) {
    SkimHdSpot s = spot(now - 50 + i, i == 4 ? "LAST" : "X", 7000000 + i, "");
    skim_hd_spotlog_add(c, &s);
  }
  check("3 kept", skim_hd_spotlog_length(c) == 3);
  check("saved", skim_hd_spotlog_save(c, path, NULL));
  g_unlink(path);
  check("nothing new: TRUE, no file written", skim_hd_spotlog_save(c, path, NULL) &&
                                             !g_file_test(path, G_FILE_TEST_EXISTS));
  check("no file: no spots", skim_hd_spotlog_load(c, path, now) == 0);
  skim_hd_spotlog_free(c);
  skim_hd_spotlog_free(a);

  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  g_spawn_command_line_sync(rm, NULL, NULL, NULL, NULL);
  g_free(rm);
  g_free(path);
  g_free(dir);
  printf("%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
