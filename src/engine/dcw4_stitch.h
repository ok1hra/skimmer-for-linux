/* dcw4_stitch.h — CW v4's streaming read of the network: e04's posterior
 * stitcher (deepcw-engine examples/python/decode_morse.py at 79104fb,
 * PosteriorStitcher; MIT), as SDRIAK ported it (decode_deepcw2.cpp,
 * bubnikv/sdriak 850a08d, GPL-3.0-or-later), in C.
 *
 * Windows of at most max_segment_s come every step_s, each starting
 * left_context_s before the frozen point. Every output frame's log-probs
 * are the average of every window that covered it, weighted by smoothstep
 * ramps from the window's edges (left_ramp_s from its start, right_ramp_s
 * to its end) after a ±dilation frame max of every non-blank class (a
 * spike placed a frame apart by two windows still adds up). Greedy CTC on
 * the average; a character is committed once it ends before the freeze
 * guard (freeze_guard_s before the newest window's end); a blank run of
 * word_boundary_s between two characters is a word gap, carried across
 * takes.
 *
 * What SDRIAK added and is kept: a stream origin and restart() (a TX hold's
 * gap the channel did not see), the committed characters' spans with their
 * posteriors. What this file adds: the committed frames' averaged
 * log-probs stay readable for history_s (skim_dcw4_stitch_row) — the
 * evidence CW v4's call reader (dcw4_callread.h) scores candidates on.
 *
 * Frames here are OUTPUT frames (two input frames of 16 ms each: 31.25/s).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later; the stitcher is ported from
 * MIT code, see dcw4_stitch.c.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  double max_segment_s;       /* MAX_SEGMENT_SECONDS (12)                     */
  double min_pending_s;       /* MIN_PENDING_SECONDS: the first window (2)    */
  double step_s;              /* STEP_SECONDS: inference cadence              */
  double left_context_s;      /* STITCH_LEFT_CONTEXT_SECONDS                  */
  double freeze_guard_s;      /* STITCH_FREEZE_GUARD_SECONDS (2)              */
  double left_ramp_s;         /* STITCH_LEFT_RAMP_SECONDS (2)                 */
  double right_ramp_s;        /* STITCH_RIGHT_RAMP_SECONDS (1)                */
  int    dilation;            /* STITCH_DILATION_FRAMES (1)                   */
  double word_boundary_s;     /* WORD_BOUNDARY_BLANK_HOPS: 134 hops of 15 ms  */
  double min_view_weight;     /* MIN_VIEW_WEIGHT (1e-3)                       */
  double uncovered_log_prob;  /* UNCOVERED_LOG_PROB (−30)                     */
  double history_s;           /* ours: committed frames kept readable (12)    */
} SkimDcw4StitchConfig;

/* CW v4's setting: upstream's constants with SDRIAK's recommended skimmer
 * tick — left context 1 s, freeze guard 2 s, step 3 s (DeepCW.md: 3.4×
 * cheaper than upstream's 4, 2, 1 at no measured loss; text 2–5 s behind
 * the air). */
void skim_dcw4_stitch_config_default(SkimDcw4StitchConfig *cfg);

/* A committed character (cls < 40) or word gap (cls 40) over output frames
 * [start, end] (inclusive); p = its averaged posterior at its first frame. */
typedef struct {
  guint8 cls;
  gint64 start, end;
  float  p;
} SkimDcw4Span;

typedef struct {
  /* committed this take, word gaps included; valid until the next call */
  const SkimDcw4Span *spans;
  guint    n_spans;
  /* the committed text starts a new word (upstream's prepend_space) */
  gboolean starts_word;
  /* the frames released after the last committed character hold a word
   * gap: the last word is complete */
  gboolean ends_word;
  /* the uncommitted reading (upstream's pending lane text) */
  const char *pending;
  gboolean pending_starts_word;
} SkimDcw4Take;

typedef struct _SkimDcw4Stitch SkimDcw4Stitch;

/* frames_per_second: output frames a second (31.25 at 250 Hz). */
SkimDcw4Stitch *skim_dcw4_stitch_new(const SkimDcw4StitchConfig *cfg, double frames_per_second);
void            skim_dcw4_stitch_free(SkimDcw4Stitch *s);

/* The start of the window that ends at output frame end_frame. */
gint64 skim_dcw4_stitch_window_start(const SkimDcw4Stitch *s, gint64 end_frame);
/* logp [count][42] (log-probs) of the window starting at output frame
 * start; is_final: the last window (no right ramp, everything freezes). */
void   skim_dcw4_stitch_add_window(SkimDcw4Stitch *s, gint64 start, const float *logp,
                                   guint count, gboolean is_final);
/* Rows held for the averaging (a quiet stretch between overs holds none). */
guint  skim_dcw4_stitch_stored_rows(const SkimDcw4Stitch *s);
/* Commit what is frozen (everything when final) and read the rest. */
void   skim_dcw4_stitch_take(SkimDcw4Stitch *s, gboolean final, SkimDcw4Take *out);
/* The stream restarts at output frame `frame` after a gap the channel did
 * not see (TX hold); text after it starts a new word. */
void   skim_dcw4_stitch_restart(SkimDcw4Stitch *s, gint64 frame);

gint64 skim_dcw4_stitch_covered_end(const SkimDcw4Stitch *s);
gint64 skim_dcw4_stitch_frozen_end(const SkimDcw4Stitch *s);
gint64 skim_dcw4_stitch_committed_end(const SkimDcw4Stitch *s);
/* Blank frames released since the last committed character. */
gint64 skim_dcw4_stitch_blank_frames(const SkimDcw4Stitch *s);

/* The averaged log-probs [42] of a committed output frame, NULL when it is
 * not committed yet or older than history_s (or before a restart). */
const float *skim_dcw4_stitch_row(const SkimDcw4Stitch *s, gint64 frame);
/* The committed rows let go (the over ended): 52 KB a keyed channel, so
 * only the channels in an over hold them. */
void skim_dcw4_stitch_forget(SkimDcw4Stitch *s);

G_END_DECLS
