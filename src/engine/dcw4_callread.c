/* dcw4_callread.c — see dcw4_callread.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "dcw4_callread.h"
#include "dcw4_kernel.h"
#include "callsign.h"

#include <math.h>
#include <string.h>

#define NC    SKIM_DCW4_CLASSES
#define BLANK SKIM_DCW4_BLANK
#define SPACE SKIM_DCW4_SPACE
#define MAX_LABEL 32
#define MAX_CAND  48

static int class_of(char ch) {
  if (ch == ' ') { return SPACE; }
  const char *p = strchr(SKIM_DCW4_ALPHABET, ch);
  return p && ch ? (int)(p - SKIM_DCW4_ALPHABET) : -1;
}

static inline double lse2(double a, double b) {
  if (a == -INFINITY) { return b; }
  if (b == -INFINITY) { return a; }
  return a > b ? a + log1p(exp(b - a)) : b + log1p(exp(a - b));
}

static inline double lp_of(const float *row, int c) {
  if (!row) { return c == BLANK ? 0.0 : -30.0; }
  return row[c];
}

double skim_dcw4_ctc_score(const float *const *rows, guint n, const char *label) {
  int lab[MAX_LABEL];
  guint L = 0;
  for (const char *p = label; *p; p++) {
    if (L == MAX_LABEL) { return -INFINITY; }
    const int c = class_of(*p);
    if (c < 0) { return -INFINITY; }
    lab[L++] = c;
  }
  const guint S = 2 * L + 1;              /* blank, l0, blank, l1, …, blank */
  if (n == 0) { return L == 0 ? 0.0 : -INFINITY; }
  double a[2 * MAX_LABEL + 1], b[2 * MAX_LABEL + 1];
  for (guint s = 0; s < S; s++) { a[s] = -INFINITY; }
  a[0] = lp_of(rows[0], BLANK);
  if (L > 0) { a[1] = lp_of(rows[0], lab[0]); }
  for (guint t = 1; t < n; t++) {
    const float *row = rows[t];
    for (guint s = 0; s < S; s++) {
      const int c = (s & 1) ? lab[s / 2] : BLANK;
      double v = a[s];
      if (s >= 1) { v = lse2(v, a[s - 1]); }
      if (s >= 2 && (s & 1) && lab[s / 2] != lab[s / 2 - 1]) { v = lse2(v, a[s - 2]); }
      b[s] = v == -INFINITY ? -INFINITY : v + lp_of(row, c);
    }
    memcpy(a, b, S * sizeof(double));
  }
  return S >= 2 ? lse2(a[S - 1], a[S - 2]) : a[S - 1];
}

void skim_dcw4_callread_config_default(SkimDcw4CallreadConfig *cfg) {
  cfg->sep = 2.0;
}

static gboolean seen(char (*cand)[SKIM_CALLSIGN_NEAR_LEN], guint n, const char *w) {
  for (guint i = 0; i < n; i++) { if (strcmp(cand[i], w) == 0) { return TRUE; } }
  return FALSE;
}

gboolean skim_dcw4_callread(const SkimDcw4CallreadConfig *cfg, const float *const *rows,
                            guint n, const char *raw, SkimDcw4CallreadResult *res) {
  memset(res, 0, sizeof *res);
  const gsize rl = strlen(raw);
  if (rl < 3 || rl >= SKIM_CALLSIGN_NEAR_LEN) { return FALSE; }
  char flat[SKIM_CALLSIGN_NEAR_LEN];
  gsize fl = 0;
  for (const char *p = raw; *p; p++) { if (*p != ' ') { flat[fl++] = *p; } }
  flat[fl] = '\0';
  if (fl < 3) { return FALSE; }

  /* candidates: the joined word itself when torn, and the calls near it */
  char cand[MAX_CAND][SKIM_CALLSIGN_NEAR_LEN];
  guint nc = skim_callsign_dict_near(flat, cand, MAX_CAND - 1);
  if (fl != rl && !seen(cand, nc, flat) && skim_callsign_dict_has(flat)) {
    g_strlcpy(cand[nc++], flat, SKIM_CALLSIGN_NEAR_LEN);
  }
  if (nc == 0) { return FALSE; }

  const double raw_score = skim_dcw4_ctc_score(rows, n, raw);
  res->raw_score = raw_score;
  res->candidates = nc + 1;
  /* the best and the runner-up among the dictionary's, the raw reading
   * counted as a rival unless it is that same call */
  double best = -INFINITY, second = strcmp(flat, raw) == 0 ? -INFINITY : raw_score;
  guint bi = 0;
  for (guint i = 0; i < nc; i++) {
    const double sc = strcmp(cand[i], raw) == 0 ? raw_score : skim_dcw4_ctc_score(rows, n, cand[i]);
    if (sc > best) {
      second = MAX(second, best);
      best = sc;
      bi = i;
    } else {
      second = MAX(second, sc);
    }
  }
  if (strcmp(cand[bi], raw) == 0) { return FALSE; }          /* the reading stands */
  /* a correction must beat the raw reading itself by sep as well */
  second = MAX(second, raw_score);
  if (best == -INFINITY || best - second < cfg->sep) { return FALSE; }
  g_strlcpy(res->call, cand[bi], sizeof res->call);
  res->call_score = best;
  res->margin = best - second;
  return TRUE;
}
