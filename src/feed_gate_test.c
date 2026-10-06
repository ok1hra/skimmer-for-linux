/*
 * skimmer-feed-gate-test — the learned feed gate (engine/feed_gate.h).
 *
 *   a known ini gives a known p; every malformed ini is refused
 *   the tracker: own window rows, the newest row's numbers, neighbours
 *     within near_khz, a similar neighbour, call shape, band, night
 *   likeness agrees with skimcmp.match.similar()
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/feed_gate.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static gint idx(const char *name) {
  const char *const *n = skim_feed_gate_names();
  for (gint i = 0; n[i]; i++) {
    if (strcmp(n[i], name) == 0)
      return i;
  }
  return -1;
}

static char *write_ini(const char *dir, const char *name, const char *text) {
  char *path = g_build_filename(dir, name, NULL);
  g_file_set_contents(path, text, -1, NULL);
  return path;
}

static SkimCallsignCand cand_of(const char *call, double score) {
  SkimCallsignCand c;
  memset(&c, 0, sizeof(c));
  g_strlcpy(c.call, call, sizeof(c.call));
  c.score = score;
  c.count = 2;
  c.parts = 1;
  c.cq_context = TRUE;
  return c;
}

static SkimStation st_of(const char *call, double hz, guint hear, gint64 now) {
  SkimStation s;
  memset(&s, 0, sizeof(s));
  g_strlcpy(s.call, call, sizeof(s.call));
  s.freq_hz = hz;
  s.hearings = hear;
  s.reports = 3;
  s.score = 1.0;
  s.cq = TRUE;
  s.first_heard = now - 10 * G_USEC_PER_SEC;
  s.last_heard = now;
  s.heard_us = now - 2 * G_USEC_PER_SEC;
  return s;
}

int main(void) {
  char *dir = g_dir_make_tmp("feedgate-XXXXXX", NULL);
  const gint64 S = G_USEC_PER_SEC;

  printf("== the model\n");
  {
    char *path = write_ini(dir, "ok.ini",
        "[gate]\nbias = 0.5\nthreshold = 0.30\nwin_s = 60\nnear_khz = 0.2\n"
        "[weights]\nhear = 2.0 1.0 0.5\nscp = -1.0 0 1\n");
    GError *err = NULL;
    SkimFeedGate *g = skim_feed_gate_load(path, &err);
    check("a well-formed ini loads", g != NULL);
    double x[SKIM_FEED_GATE_NFEAT] = { 0 };
    x[idx("hear")] = 1.5;
    x[idx("scp")] = 1.0;
    x[idx("snr")] = 99.0;                      /* left out → weighs nothing */
    const double want = 1.0 / (1.0 + exp(-(0.5 + 2.0 * (1.5 - 1.0) / 0.5 - 1.0)));
    check("p = σ(bias + Σ w·(x − mean)/sd)", g && fabs(skim_feed_gate_p(g, x) - want) < 1e-12);
    check("threshold, window, neighbourhood and id come from the ini",
          g && skim_feed_gate_threshold(g) == 0.30 && skim_feed_gate_win_s(g) == 60 &&
          skim_feed_gate_near_khz(g) == 0.2 && strcmp(skim_feed_gate_id(g), "ok.ini") == 0);
    skim_feed_gate_free(g);
    g_free(path);

    static const struct { const char *name, *text; } BAD[] = {
      { "no bias", "[gate]\nthreshold = 0.3\n" },
      { "no threshold", "[gate]\nbias = 0\n" },
      { "a bias that is no number", "[gate]\nbias = x\nthreshold = 0.3\n" },
      { "an unknown feature", "[gate]\nbias = 0\nthreshold = 0.3\n[weights]\nfoo = 1 0 1\n" },
      { "two numbers", "[gate]\nbias = 0\nthreshold = 0.3\n[weights]\nhear = 1 0\n" },
      { "four numbers", "[gate]\nbias = 0\nthreshold = 0.3\n[weights]\nhear = 1 0 1 2\n" },
      { "sd 0", "[gate]\nbias = 0\nthreshold = 0.3\n[weights]\nhear = 1 0 0\n" },
      { "win_s 0", "[gate]\nbias = 0\nthreshold = 0.3\nwin_s = 0\n" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(BAD); i++) {
      char *p = write_ini(dir, "bad.ini", BAD[i].text);
      SkimFeedGate *b = skim_feed_gate_load(p, &err);
      char what[96];
      g_snprintf(what, sizeof(what), "refused: %s", BAD[i].name);
      check(what, b == NULL && err != NULL);
      g_clear_error(&err);
      skim_feed_gate_free(b);
      g_free(p);
    }
    char *missing = g_build_filename(dir, "none.ini", NULL);
    check("refused: a missing file", skim_feed_gate_load(missing, &err) == NULL && err);
    g_clear_error(&err);
    g_free(missing);
  }

  printf("== the features\n");
  {
    SkimFeedGateTrack *t = skim_feed_gate_track_new(120, 0.15);
    const gint64 t0 = 1000 * S;
    /* OK1BR read three times at 7020.0 kHz, top candidate in two rows */
    SkimCallsignCand c = cand_of("OK1BR", 1.0);
    SkimStation s = st_of("OK1BR", 7020000, 3, t0);
    s.heard_us = t0 - 40 * S;                  /* last READ 30 s before the newest row */
    skim_feed_gate_track_row(t, t0 - 200 * S, 7020000, 25, 10, 0.50, &c, &s);  /* too old */
    skim_feed_gate_track_row(t, t0 - 30 * S, 7020000, 25, 10, 0.80, &c, &s);
    SkimCallsignCand other = cand_of("OK1BRX", 0.7);
    skim_feed_gate_track_row(t, t0 - 20 * S, 7020000, 25, 12, 0.60, &other, &s);
    c.score = 0.987;
    skim_feed_gate_track_row(t, t0 - 10 * S, 7020004, 26.04, 14.26, 0.7004, &c, &s);
    /* neighbours: OK1BQ (similar) 100 Hz off, EA3XX 120 Hz off, DL1AA 400 Hz off */
    SkimCallsignCand nb = cand_of("OK1BQ", 0.9);
    SkimStation nbs = st_of("OK1BQ", 7020100, 5, t0);
    skim_feed_gate_track_row(t, t0 - 5 * S, 7020100, 25, 10, 0.7, &nb, &nbs);
    SkimCallsignCand ea = cand_of("EA3XX", 0.9);
    SkimStation eas = st_of("EA3XX", 7019880, 9, t0);
    skim_feed_gate_track_row(t, t0 - 5 * S, 7019880, 25, 10, 0.7, &ea, &eas);
    SkimCallsignCand dl = cand_of("DL1AA", 0.9);
    SkimStation dls = st_of("DL1AA", 7020400, 20, t0);
    skim_feed_gate_track_row(t, t0 - 5 * S, 7020400, 25, 10, 0.7, &dl, &dls);

    double x[SKIM_FEED_GATE_NFEAT];
    const gint64 wall = (gint64)(22 * 3600) * S;                /* 22:00 UTC */
    skim_feed_gate_track_features(t, t0, wall, "OK1BR", 7020030, "40m", x);
    check("own window: 3 rows inside 120 s, the 200 s old one gone",
          fabs(x[idx("w_rows")] - log1p(3)) < 1e-12);
    check("own window: top candidate in 2 of 3 rows",
          fabs(x[idx("w_top")] - 2.0 / 3) < 1e-12);
    check("own window: mean and min confidence",
          fabs(x[idx("w_conf")] - (0.8 + 0.6 + 0.7) / 3) < 1e-9 &&
          fabs(x[idx("w_conf_min")] - 0.6) < 1e-12);
    check("newest row, rounded as the gatelog prints it (0.99, 14.3, 26.0, 0.700)",
          x[idx("sc")] == 0.99 && x[idx("snr")] == 14.3 && x[idx("wpm")] == 26.0 &&
          x[idx("conf")] == 0.7 && x[idx("row_cq")] == 1.0);
    check("station record: log1p of hearings, reports, age, last read",
          fabs(x[idx("hear")] - log1p(3)) < 1e-12 && fabs(x[idx("rep")] - log1p(3)) < 1e-12 &&
          fabs(x[idx("age")] - log1p(10)) < 1e-12 &&
          fabs(x[idx("st_read")] - log1p(30)) < 1e-12);
    check("neighbours within 0.15 kHz of 7020.0: OK1BQ and EA3XX, not DL1AA",
          fabs(x[idx("n_near")] - log1p(2)) < 1e-12 &&
          fabs(x[idx("near_hear")] - log1p(9)) < 1e-12);
    check("a similar neighbour (OK1BQ) and how well it was heard",
          x[idx("near_sim")] == 1.0 && fabs(x[idx("near_sim_hear")] - log1p(5)) < 1e-12);
    check("call shape: len 5, 1 digit, suffix 2, no slash",
          x[idx("len")] == 5 && x[idx("digits")] == 1 && x[idx("suffix")] == 2 &&
          x[idx("slash")] == 0);
    check("band one-hot and night",
          x[idx("b40m")] == 1.0 && x[idx("b80m")] == 0.0 && x[idx("night")] == 1.0);
    skim_feed_gate_track_features(t, t0, wall, "ZZ9ZZ", 7020030, NULL, x);
    check("a call never seen: no record, no window, no band",
          x[idx("hear")] == 0 && x[idx("w_rows")] == 0 && x[idx("b40m")] == 0);
    skim_feed_gate_track_free(t);
  }

  printf("== likeness (skimcmp.match.similar)\n");
  check("prefix/suffix: length difference",
        skim_feed_gate_similar("EA3G", "EA3GEH", 2) == 2 &&
        skim_feed_gate_similar("OK1BR", "1BR", 2) == 2);
  check("one substitution", skim_feed_gate_similar("OK1BR", "OK1BQ", 2) == 1);
  check("too far, equal, too short → none",
        skim_feed_gate_similar("OK1BR", "DL7XYZ", 2) < 0 &&
        skim_feed_gate_similar("OK1BR", "OK1BR", 2) < 0 &&
        skim_feed_gate_similar("K1", "K2", 2) < 0);

  g_rmdir(dir);
  g_free(dir);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
