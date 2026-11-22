/* dcw4_callread.h — CW v4's call reader: the word the network just read,
 * scored against the calls MASTER.SCP knows near it, on the network's own
 * evidence.
 *
 * The greedy reading takes the best class frame by frame; CTC's likelihood
 * of a text sums every alignment of it over the same frames. Where a fade
 * or a click left a character ambiguous, a near call can hold more of the
 * posterior mass than what greedy read. The reader scores the raw word and
 * every dictionary call within two edits of it (skim_callsign_dict_near;
 * for a word the model tore before a digit — "H 1SH" at ≤ 12 WPM — also
 * the joined word and its near calls) by their CTC log-likelihood over the
 * word's frames, and takes a dictionary call that beats the raw reading AND
 * every other candidate by `sep` nats. Only a dictionary call is ever
 * written in (as v3's reader: decode_cw_v3.c rs_reread), so the feed never
 * gets a call the audio and MASTER.SCP did not both confirm.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* log P(label | rows): CTC's forward sum over rows[0..n) (log-probs [42]
 * each, NULL rows are read as certain blanks), blanks allowed before,
 * between and after; a repeated character needs a blank between. label:
 * SKIM_DCW4_ALPHABET characters and ' ' (the word-gap class). -INFINITY
 * when it cannot fit (more symbols than frames) or holds another char. */
double skim_dcw4_ctc_score(const float *const *rows, guint n, const char *label);

typedef struct {
  double sep;                  /* nats a call must win by (2.0)              */
} SkimDcw4CallreadConfig;

void skim_dcw4_callread_config_default(SkimDcw4CallreadConfig *cfg);

typedef struct {
  char   call[16];             /* the dictionary call that won              */
  double margin;               /* its lead over the next candidate, nats    */
  double raw_score, call_score;/* log-likelihoods (whole span)              */
  guint  candidates;           /* scored, the raw reading included          */
} SkimDcw4CallreadResult;

/* raw: the word as read, upper case, one inner space for a torn word ("H
 * 1SH"); rows[0..n): the frames that cover it with a margin. TRUE when a
 * dictionary call other than the raw reading (spaces removed) wins. */
gboolean skim_dcw4_callread(const SkimDcw4CallreadConfig *cfg, const float *const *rows,
                            guint n, const char *raw, SkimDcw4CallreadResult *res);

G_END_DECLS
