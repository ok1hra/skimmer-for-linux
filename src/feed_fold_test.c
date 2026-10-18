/*
 * skimmer-feed-fold-test — the RBN feed's twin fold (engine/feed_fold.h).
 *
 *   a glued or cut twin of a call sent lately on the frequency is folded
 *     when the sent call is better attested (dictionary, then hearings)
 *   a better twin goes out (the feed often sends the garble first)
 *   not folded: another frequency, past the window, an unlike call,
 *     a call whose own run of lines goes on
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <stdio.h>

#include "engine/feed_fold.h"

#define S(x) ((gint64)(x) * G_USEC_PER_SEC)

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static gboolean folded_by(SkimFeedFold *f, const char *call, double hz, gint64 t,
                          guint hear, gboolean dict, const char *by) {
  const char *got = skim_feed_fold_check(f, call, hz, t, hear, dict);
  return by ? g_strcmp0(got, by) == 0 : got == NULL;
}

int main(void) {
  /* the pipeline's shape: 600 s window, own run 600 + 600 s, 300 Hz */
  SkimFeedFold *f = skim_feed_fold_new(S(600), S(1200), 300.0);

  printf("== a twin of a call sent lately\n");
  skim_feed_fold_sent(f, "F6FXX", 14030000.0, S(0), 3, TRUE);
  check("glued tail (F6FXXK), SCP knows F6FXX, read as often → folded",
        folded_by(f, "F6FXXK", 14030050.0, S(100), 3, FALSE, "F6FXX"));
  check("SCP knows the twin but the call was read more (SP2RCL vs SP2R) → sent",
        folded_by(f, "F6FXXK", 14030050.0, S(100), 4, FALSE, NULL));
  check("cut call (F6FX) → folded", folded_by(f, "F6FX", 14029900.0, S(100), 3, FALSE,
                                              "F6FXX"));
  check("400 Hz away → not folded",
        folded_by(f, "F6FXXK", 14030400.0, S(100), 3, FALSE, NULL));
  check("past the window (700 s) → not folded",
        folded_by(f, "F6FXXK", 14030050.0, S(700), 3, FALSE, NULL));
  check("an unlike call → not folded",
        folded_by(f, "DL7XYZ", 14030050.0, S(100), 1, FALSE, NULL));

  printf("== the better twin goes out\n");
  skim_feed_fold_sent(f, "RK3D", 7020000.0, S(0), 2, FALSE);
  check("RK3DJW (SCP) after the garble RK3D → sent",
        folded_by(f, "RK3DJW", 7020020.0, S(60), 2, TRUE, NULL));
  skim_feed_fold_sent(f, "UA9ABC", 3520000.0, S(0), 5, FALSE);
  check("neither in SCP, the twin read less → folded",
        folded_by(f, "UA9ABD", 3520000.0, S(60), 2, FALSE, "UA9ABC"));
  check("neither in SCP, read as often → sent",
        folded_by(f, "UA9ABD", 3520000.0, S(60), 5, FALSE, NULL));
  check("neither in SCP, read more → sent",
        folded_by(f, "UA9ABD", 3520000.0, S(60), 6, FALSE, NULL));
  check("in SCP, the sent one not → sent even read less",
        folded_by(f, "UA9ABD", 3520000.0, S(60), 2, TRUE, NULL));

  printf("== a run goes on\n");
  skim_feed_fold_sent(f, "OK1AB", 10110000.0, S(0), 2, FALSE);
  skim_feed_fold_sent(f, "OK1ABC", 10110000.0, S(10), 9, TRUE);
  check("OK1AB sent 300 s ago: its run goes on, not folded",
        folded_by(f, "OK1AB", 10110000.0, S(300), 2, FALSE, NULL));
  check("OK1AB silent 1300 s, OK1ABC still sent lately → folded",
        (skim_feed_fold_sent(f, "OK1ABC", 10110000.0, S(1250), 9, TRUE),
         folded_by(f, "OK1AB", 10110000.0, S(1300), 2, FALSE, "OK1ABC")));
  check("a new run counts its hearings afresh",
        (skim_feed_fold_sent(f, "UA9ABC", 3520000.0, S(2000), 1, FALSE),
         folded_by(f, "UA9ABD", 3520000.0, S(2010), 2, FALSE, NULL)));

  skim_feed_fold_free(f);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
