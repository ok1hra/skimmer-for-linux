/* dcw4_stitch.c — see dcw4_stitch.h.
 *
 * Ported from e04/deepcw-engine examples/python/decode_morse.py at 79104fb
 * (to_dilated_log_probs, decode_ctc_greedy, to_lane_text,
 * PosteriorStitcher) through SDRIAK's C++ (decode_deepcw2.cpp, bubnikv/
 * sdriak 850a08d), under the MIT licence below. The history of committed
 * rows is this file's own (GPL-3.0-or-later).
 *
 * ---- licence of the ported stitcher -----------------------------------------
 * MIT License
 *
 * Copyright (c) 2026 e04
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ------------------------------------------------------------------------------
 *
 * Part of skimmer-for-linux.
 */
#include "dcw4_stitch.h"
#include "dcw4_kernel.h"

#include <math.h>
#include <string.h>

#define NC     SKIM_DCW4_CLASSES
#define BLANK  SKIM_DCW4_BLANK
#define SPACE  SKIM_DCW4_SPACE

typedef enum { SYM_NONE, SYM_CHAR, SYM_SPACE } Symbol;

struct _SkimDcw4Stitch {
  SkimDcw4StitchConfig cfg;
  double  fps;
  gint64  left_context_f, freeze_guard_f, word_boundary_f, max_segment_f;

  gint64  origin;                      /* the stream's first frame (0 upstream) */
  gint64  row_base;
  /* ours: frames [gap_lo, gap_hi) no window covered (the channel sends none
   * while its line is quiet) are not stored — rows [row_base, gap_lo) are,
   * then the rows from gap_hi on. Upstream keeps a zero row for each, and
   * after hours of quiet a channel's next over took a row for every frame
   * of them (2026-11-24, 25 GB on hra2). No gap: both G_MAXINT64. */
  gint64  gap_lo, gap_hi;
  gint64  rows_cap;                    /* the most rows since the arrays were made */
  GArray *sums;                        /* float [rows][42]  */
  GArray *weights;                     /* float [rows]      */
  gint64  covered_end, frozen_end, committed_end;
  Symbol  previous;
  gint64  blank_frames;
  gboolean has_committed;

  /* this take's output */
  GArray *spans;                       /* SkimDcw4Span */
  GString *pending;

  /* ours: committed frames' rows, a ring over absolute frames */
  float  *hist;                        /* [hist_n][42] */
  gint64  hist_n, hist_lo, hist_hi;    /* rows [hist_lo, hist_hi) are valid */
};

void skim_dcw4_stitch_config_default(SkimDcw4StitchConfig *c) {
  c->max_segment_s = 12.0;
  c->min_pending_s = 2.0;
  c->step_s = 3.0;
  c->left_context_s = 1.0;
  c->freeze_guard_s = 2.0;
  c->left_ramp_s = 2.0;
  c->right_ramp_s = 1.0;
  c->dilation = 1;
  c->word_boundary_s = 2.01;
  c->min_view_weight = 1e-3;
  c->uncovered_log_prob = -30.0;
  c->history_s = 12.0;
}

SkimDcw4Stitch *skim_dcw4_stitch_new(const SkimDcw4StitchConfig *cfg, double fps) {
  SkimDcw4Stitch *s = g_new0(SkimDcw4Stitch, 1);
  s->cfg = *cfg;
  s->fps = fps;
  s->left_context_f = llround(cfg->left_context_s * fps);
  s->freeze_guard_f = llround(cfg->freeze_guard_s * fps);
  s->max_segment_f = llround(cfg->max_segment_s * fps);
  /* SDRIAK: upstream counts ceil(134 hops / stride) of ITS 15 ms hop; the
   * seconds are kept here, on this backend's frame rate. */
  s->word_boundary_f = (gint64)ceil(cfg->word_boundary_s * fps);
  s->sums = g_array_new(FALSE, TRUE, sizeof(float));
  s->weights = g_array_new(FALSE, TRUE, sizeof(float));
  s->spans = g_array_new(FALSE, FALSE, sizeof(SkimDcw4Span));
  s->pending = g_string_new(NULL);
  s->hist_n = MAX(llround(cfg->history_s * fps), 1);
  s->gap_lo = s->gap_hi = G_MAXINT64;
  return s;
}

void skim_dcw4_stitch_free(SkimDcw4Stitch *s) {
  if (!s) { return; }
  g_array_free(s->sums, TRUE);
  g_array_free(s->weights, TRUE);
  g_array_free(s->spans, TRUE);
  g_string_free(s->pending, TRUE);
  g_free(s->hist);
  g_free(s);
}

gint64 skim_dcw4_stitch_covered_end(const SkimDcw4Stitch *s) { return s->covered_end; }
gint64 skim_dcw4_stitch_frozen_end(const SkimDcw4Stitch *s) { return s->frozen_end; }
gint64 skim_dcw4_stitch_committed_end(const SkimDcw4Stitch *s) { return s->committed_end; }
gint64 skim_dcw4_stitch_blank_frames(const SkimDcw4Stitch *s) { return s->blank_frames; }
guint skim_dcw4_stitch_stored_rows(const SkimDcw4Stitch *s) { return s->weights->len; }

static gint64 context_start(const SkimDcw4Stitch *s) {
  return MAX(s->origin, s->frozen_end - s->left_context_f);
}

gint64 skim_dcw4_stitch_window_start(const SkimDcw4Stitch *s, gint64 end_frame) {
  return MAX(context_start(s), end_frame - s->max_segment_f);
}

static void log_softmax_row(float *v) {
  float peak = v[0];
  for (int c = 1; c < NC; c++) { peak = MAX(peak, v[c]); }
  double sum = 0.0;
  for (int c = 0; c < NC; c++) { sum += exp((double)(v[c] - peak)); }
  const float lse = peak + (float)log(sum);
  for (int c = 0; c < NC; c++) { v[c] -= lse; }
}

static double smoothstep(double v) {
  const double t = CLAMP(v, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

/* to_dilated_log_probs(): every class but blank takes its best value within
 * ±dilation frames, so a spike placed a frame apart by two windows still
 * adds up. Returns [count][42], g_free. */
static float *dilated_log_probs(const SkimDcw4Stitch *s, const float *logits, guint count) {
  float *lp = g_memdup2(logits, (gsize)count * NC * sizeof(float));
  for (guint f = 0; f < count; f++) { log_softmax_row(lp + (gsize)f * NC); }
  float *dil = g_new(float, (gsize)count * NC);
  const guint r = (guint)MAX(s->cfg.dilation, 0);
  for (guint f = 0; f < count; f++) {
    const guint first = f >= r ? f - r : 0;
    const guint last = MIN(count - 1, f + r);
    float *d = dil + (gsize)f * NC;
    for (int c = 0; c < NC; c++) {
      float v = lp[(gsize)f * NC + c];
      if (c != BLANK) {
        for (guint o = first; o <= last; o++) { v = MAX(v, lp[(gsize)o * NC + c]); }
      }
      d[c] = v;
    }
    log_softmax_row(d);
  }
  g_free(lp);
  return dil;
}

static gboolean has_gap(const SkimDcw4Stitch *s) { return s->gap_lo < s->gap_hi; }
static gint64 gap_len(const SkimDcw4Stitch *s) { return has_gap(s) ? s->gap_hi - s->gap_lo : 0; }

static void clear_gap(SkimDcw4Stitch *s) { s->gap_lo = s->gap_hi = G_MAXINT64; }

/* the frame after the last stored row (the gap counted) */
static gint64 stored_end(const SkimDcw4Stitch *s) {
  return s->row_base + (gint64)s->weights->len + gap_len(s);
}

/* frame → its stored row; -1: not stored (dropped, in the gap, not yet
 * covered) — read as uncovered, exactly as upstream's zero-weight row */
static gint64 row_of(const SkimDcw4Stitch *s, gint64 f) {
  if (f < s->row_base) { return -1; }
  if (f < s->gap_lo) { return f - s->row_base < (gint64)s->weights->len ? f - s->row_base : -1; }
  if (f < s->gap_hi) { return -1; }
  const gint64 r = f - s->row_base - gap_len(s);
  return r < (gint64)s->weights->len ? r : -1;
}

/* a gap at row_base is no gap: the rows simply start after it */
static void normalise_gap(SkimDcw4Stitch *s) {
  if (has_gap(s) && s->gap_lo <= s->row_base) {
    s->row_base = s->gap_hi;
    clear_gap(s);
  }
}

/* the gap's zero rows stored after all (a window reaching into it) */
static void fill_gap(SkimDcw4Stitch *s) {
  if (!has_gap(s)) { return; }
  const guint at = (guint)(s->gap_lo - s->row_base), n = (guint)gap_len(s);
  const guint tail = s->weights->len - at;
  g_array_set_size(s->sums, s->sums->len + n * NC);
  g_array_set_size(s->weights, s->weights->len + n);
  float *sums = (float *)s->sums->data, *weights = (float *)s->weights->data;
  memmove(sums + (gsize)(at + n) * NC, sums + (gsize)at * NC, (gsize)tail * NC * sizeof(float));
  memset(sums + (gsize)at * NC, 0, (gsize)n * NC * sizeof(float));
  memmove(weights + at + n, weights + at, (gsize)tail * sizeof(float));
  memset(weights + at, 0, (gsize)n * sizeof(float));
  s->rows_cap = MAX(s->rows_cap, (gint64)s->weights->len);
  clear_gap(s);
}

/* rows up to end_frame for a window from start: a window after a quiet
 * stretch leaves that stretch as the gap (one at a time — a second one is
 * stored as upstream stores it; CW v4 takes after every window, and that
 * drops the first one) */
static void ensure_rows(SkimDcw4Stitch *s, gint64 start, gint64 end_frame) {
  if (has_gap(s) && start < s->gap_hi && end_frame > s->gap_lo) { fill_gap(s); }
  const gint64 se = stored_end(s);
  if (end_frame <= se) { return; }
  gint64 from = se;
  if (start > se && !has_gap(s)) {
    s->gap_lo = se;
    s->gap_hi = start;
    normalise_gap(s);
    from = start;
  }
  const gint64 missing = end_frame - from;
  g_array_set_size(s->sums, s->sums->len + (guint)(missing * NC));
  g_array_set_size(s->weights, s->weights->len + (guint)missing);
  s->rows_cap = MAX(s->rows_cap, (gint64)s->weights->len);
}

static void drop_stored(SkimDcw4Stitch *s, gint64 rows) {
  rows = MIN(rows, (gint64)s->weights->len);
  if (rows <= 0) { return; }
  g_array_remove_range(s->sums, 0, (guint)(rows * NC));
  g_array_remove_range(s->weights, 0, (guint)rows);
  s->row_base += rows;
}

static void drop_rows_before(SkimDcw4Stitch *s, gint64 frame) {
  if (frame <= s->row_base) { return; }
  if (!has_gap(s) || frame <= s->gap_lo) {
    drop_stored(s, frame - s->row_base);
  } else {
    drop_stored(s, s->gap_lo - s->row_base);      /* the rows before the gap */
    if (frame < s->gap_hi) {
      normalise_gap(s);                           /* the rows now start after it */
    } else {
      s->row_base = s->gap_hi;
      clear_gap(s);
      drop_stored(s, frame - s->row_base);
    }
  }
  /* GArray never gives memory back: a quiet channel would keep its
   * biggest over's arrays for good */
  if (s->rows_cap > 256 && (gint64)s->weights->len * 4 < s->rows_cap) {
    GArray *sums = g_array_sized_new(FALSE, TRUE, sizeof(float), s->sums->len);
    GArray *weights = g_array_sized_new(FALSE, TRUE, sizeof(float), s->weights->len);
    g_array_append_vals(sums, s->sums->data, s->sums->len);
    g_array_append_vals(weights, s->weights->data, s->weights->len);
    g_array_free(s->sums, TRUE);
    g_array_free(s->weights, TRUE);
    s->sums = sums;
    s->weights = weights;
    s->rows_cap = weights->len;
  }
}

/* add_window(): the window's frames beyond the frozen point are averaged
 * in, weighted down near its edges (no left ramp at the stream's start, no
 * right ramp on the final window). */
void skim_dcw4_stitch_add_window(SkimDcw4Stitch *s, gint64 start, const float *logp,
                                 guint count, gboolean is_final) {
  if (count == 0) {
    if (is_final) { s->frozen_end = s->covered_end; }
    return;
  }
  const gint64 end = start + count;
  ensure_rows(s, start, end);
  float *lp = dilated_log_probs(s, logp, count);
  float *sums = (float *)s->sums->data, *weights = (float *)s->weights->data;
  for (gint64 f = MAX((gint64)0, s->frozen_end - start); f < (gint64)count; f++) {
    const double left = start == s->origin ? INFINITY : (double)f / s->fps;
    const double right = is_final ? INFINITY : (double)((gint64)count - 1 - f) / s->fps;
    const float w = (float)MAX(s->cfg.min_view_weight,
                               smoothstep(left / s->cfg.left_ramp_s) *
                               smoothstep(right / s->cfg.right_ramp_s));
    const gint64 row = row_of(s, start + f);
    if (row < 0) { continue; }         /* SDRIAK guard: a frame already dropped */
    float *sr = sums + (gsize)row * NC;
    const float *l = lp + (gsize)f * NC;
    for (int c = 0; c < NC; c++) { sr[c] += w * l[c]; }
    weights[row] += w;
  }
  g_free(lp);
  s->covered_end = MAX(s->covered_end, end);
  s->frozen_end = is_final ? s->covered_end : MAX(s->frozen_end, end - s->freeze_guard_f);
}

/* A take's read [start, end) with the gap in it cut to `keep` frames, the
 * last of them standing for the gap's last frame: index i is frame
 * start + i, or start + i + cut_k from index `cut` on. Every frame of the
 * gap reads the same (uncovered), so the greedy reading only sees a
 * shorter blank run — still ≥ word_boundary_f, the one length it tests. */
typedef struct {
  gint64 start, n, cut, cut_k;
} Read;

static Read read_plan(const SkimDcw4Stitch *s, gint64 start, gint64 end) {
  Read r = { start, end - start, G_MAXINT64, 0 };
  if (!has_gap(s)) { return r; }
  const gint64 gl = MAX(s->gap_lo, start), gh = MIN(s->gap_hi, end);
  const gint64 keep = s->word_boundary_f + 1;
  if (gh - gl > keep) {
    r.cut_k = gh - gl - keep;
    r.cut = gl - start + keep - 1;
    r.n -= r.cut_k;
  }
  return r;
}

static gint64 read_frame(const Read *r, gint64 i) { return r->start + i + (i >= r->cut ? r->cut_k : 0); }

/* frame → index; -1: a frame of the gap cut out (it reads uncovered) */
static gint64 read_index(const Read *r, gint64 f) {
  const gint64 i = f - r->start;
  if (i < r->cut) { return i; }
  if (i < r->cut + r->cut_k) { return -1; }
  return i - r->cut_k;
}

/* _read_scores(): the averaged log-probs; an uncovered frame reads blank.
 * Returns [r->n][42], g_free. */
static float *read_scores(const SkimDcw4Stitch *s, const Read *r) {
  float *sc = g_new(float, (gsize)r->n * NC);
  const float *sums = (const float *)s->sums->data, *weights = (const float *)s->weights->data;
  for (gint64 i = 0; i < r->n; i++) {
    float *o = sc + (gsize)i * NC;
    const gint64 row = row_of(s, read_frame(r, i));
    const float w = row >= 0 ? weights[row] : 0.0f;
    if (w > 0) {
      const float *sr = sums + (gsize)row * NC;
      for (int c = 0; c < NC; c++) { o[c] = sr[c] / w; }
    } else {
      for (int c = 0; c < NC; c++) { o[c] = (float)s->cfg.uncovered_log_prob; }
      o[BLANK] = 0.0f;
    }
  }
  return sc;
}

/* decode_ctc_greedy(): best path; a blank run of word_boundary frames or
 * more between two characters becomes a word gap (counting the blanks
 * carried over from the last take at the start); then the spans (frames
 * relative to the read). argmax[n] is filled. */
static GArray *decode_greedy(const SkimDcw4Stitch *s, const float *scores, gint64 n, int *argmax) {
  for (gint64 f = 0; f < n; f++) {
    const float *row = scores + (gsize)f * NC;
    int best = 0;
    for (int c = 1; c < NC; c++) { if (row[c] > row[best]) { best = c; } }
    argmax[f] = best;
  }
  int *idx = g_memdup2(argmax, (gsize)MAX(n, 1) * sizeof(int));
  int previous = s->previous == SYM_CHAR ? -2 : s->previous == SYM_SPACE ? SPACE : -1;
  for (gint64 f = 0; f < n;) {
    if (idx[f] != BLANK) {
      previous = idx[f];
      f++;
      continue;
    }
    const gint64 run_start = f;
    while (f < n && idx[f] == BLANK) { f++; }
    const int following = f < n ? idx[f] : -1;
    const gint64 run = f - run_start + (run_start == 0 ? s->blank_frames : 0);
    if (run >= s->word_boundary_f && previous != -1 && previous != SPACE && following >= 0 &&
        following != SPACE) {
      for (gint64 k = run_start; k < f; k++) { idx[k] = SPACE; }
    }
  }
  GArray *spans = g_array_new(FALSE, FALSE, sizeof(SkimDcw4Span));
  int prev = -1;
  gint active = -1;
  for (gint64 f = 0; f < n; f++) {
    const int c = idx[f];
    if (c == BLANK) {
      prev = -1;
      active = -1;
      continue;
    }
    if (c == prev) {
      if (active >= 0) { g_array_index(spans, SkimDcw4Span, active).end = f; }
      continue;
    }
    prev = c;
    const SkimDcw4Span sp = { (guint8)c, f, f, 0.0f };
    g_array_append_val(spans, sp);
    active = (gint)spans->len - 1;
  }
  g_free(idx);
  return spans;
}

/* to_lane_text(): spaces only between characters; a leading one is a flag. */
static gboolean lane_text(GString *out, const SkimDcw4Span *spans, guint from, guint to,
                          gboolean prepend) {
  gboolean pending_space = FALSE;
  for (guint i = from; i < to; i++) {
    if (spans[i].cls == SPACE) {
      if (out->len == 0) { prepend = TRUE; } else { pending_space = TRUE; }
      continue;
    }
    if (pending_space) {
      g_string_append_c(out, ' ');
      pending_space = FALSE;
    }
    g_string_append_c(out, SKIM_DCW4_ALPHABET[spans[i].cls]);
  }
  return prepend;
}

/* ours: rows [from, to) of scores (read from `start`) into the history */
static void history_put(SkimDcw4Stitch *s, const float *scores, const Read *r, gint64 from,
                        gint64 to) {
  if (to <= from) { return; }
  if (!s->hist) {                                   /* the first over: made now */
    s->hist = g_new(float, (gsize)s->hist_n * NC);
    s->hist_lo = s->hist_hi = from;
  }
  if (from != s->hist_hi) { s->hist_lo = from; }    /* a gap: start over */
  /* only the last hist_n frames stay readable (an earlier one's slot is
   * written again by a later frame of this same call) */
  for (gint64 f = MAX(from, to - s->hist_n); f < to; f++) {
    const gint64 i = read_index(r, f);               /* -1: the gap's, as its last */
    memcpy(s->hist + (gsize)(f % s->hist_n) * NC, scores + (gsize)(i >= 0 ? i : r->cut) * NC,
           NC * sizeof(float));
  }
  s->hist_hi = to;
  s->hist_lo = MAX(s->hist_lo, s->hist_hi - s->hist_n);
}

const float *skim_dcw4_stitch_row(const SkimDcw4Stitch *s, gint64 frame) {
  if (!s->hist || frame < s->hist_lo || frame >= s->hist_hi) { return NULL; }
  return s->hist + (gsize)(frame % s->hist_n) * NC;
}

void skim_dcw4_stitch_forget(SkimDcw4Stitch *s) {
  g_clear_pointer(&s->hist, g_free);
  s->hist_lo = s->hist_hi = 0;
}

/* take(): commit every character that ends before the frozen point (all on
 * the final take), release the frames up to the next character, and read
 * the rest as pending. */
void skim_dcw4_stitch_take(SkimDcw4Stitch *s, gboolean final, SkimDcw4Take *t) {
  memset(t, 0, sizeof *t);
  g_array_set_size(s->spans, 0);
  g_string_truncate(s->pending, 0);
  t->spans = (const SkimDcw4Span *)s->spans->data;
  t->pending = s->pending->str;
  const gint64 start = s->committed_end;
  if (s->covered_end - start <= 0) { return; }
  const Read r = read_plan(s, start, s->covered_end);
  const gint64 n = r.n;
  const gboolean starts_word = !s->has_committed || s->previous == SYM_SPACE;
  float *scores = read_scores(s, &r);
  int *argmax = g_new(int, (gsize)n);
  GArray *spans = decode_greedy(s, scores, n, argmax);
  SkimDcw4Span *sp = (SkimDcw4Span *)spans->data;
  for (guint i = 0; i < spans->len; i++) {
    sp[i].p = (float)exp((double)scores[(gsize)sp[i].start * NC + sp[i].cls]);
    sp[i].start = read_frame(&r, sp[i].start);
    sp[i].end = read_frame(&r, sp[i].end);
  }
  const gint64 limit = final ? G_MAXINT64 : s->frozen_end;
  guint cc = 0;
  for (guint i = 0; i < spans->len; i++) {
    if (sp[i].end + 1 >= limit) { break; }
    if (sp[i].cls != SPACE) { cc = i + 1; }
  }
  if (cc > 0) {
    GString *dummy = g_string_new(NULL);
    t->starts_word = lane_text(dummy, sp, 0, cc, starts_word);
    g_string_free(dummy, TRUE);
    g_array_append_vals(s->spans, sp, cc);
    s->has_committed = TRUE;
    s->committed_end = sp[cc - 1].end + 1;
    s->previous = SYM_CHAR;
    s->blank_frames = 0;
  }
  gint64 next_char = G_MAXINT64;
  for (guint i = cc; i < spans->len; i++) {
    if (sp[i].cls != SPACE) {
      next_char = sp[i].start;
      break;
    }
  }
  const gint64 release_end = MIN(final ? s->covered_end : s->frozen_end, next_char) - 1;
  Symbol ps = s->previous;
  gint64 bf = s->blank_frames;
  for (gint64 f = s->committed_end; f < release_end; f++) {
    const gint64 i = read_index(&r, f);
    if (i < 0) {                                     /* the gap cut out: all blank */
      const gint64 skip = MIN(read_frame(&r, r.cut), release_end) - f;
      bf += skip;
      f += skip - 1;
      continue;
    }
    if (argmax[i] == SPACE) {
      ps = SYM_SPACE;
      bf = 0;
    } else {
      bf++;
    }
  }
  if (release_end > s->committed_end) {
    s->previous = ps;
    s->blank_frames = bf;
    s->committed_end = release_end;
  }
  t->ends_word = s->has_committed && s->previous == SYM_SPACE;
  history_put(s, scores, &r, start, s->committed_end);
  drop_rows_before(s, s->committed_end);
  /* pending: the spans that reach past the committed end */
  GArray *pend = g_array_new(FALSE, FALSE, sizeof(SkimDcw4Span));
  for (guint i = cc; i < spans->len; i++) {
    if (sp[i].end >= s->committed_end) { g_array_append_val(pend, sp[i]); }
  }
  t->pending_starts_word = lane_text(s->pending, (const SkimDcw4Span *)pend->data, 0, pend->len,
                                     !s->has_committed || s->previous == SYM_SPACE);
  g_array_free(pend, TRUE);
  g_array_free(spans, TRUE);
  g_free(argmax);
  g_free(scores);
  t->spans = (const SkimDcw4Span *)s->spans->data;
  t->n_spans = s->spans->len;
  t->pending = s->pending->str;
}

/* SDRIAK: a restart after a gap the channel did not see. */
void skim_dcw4_stitch_restart(SkimDcw4Stitch *s, gint64 frame) {
  s->origin = s->row_base = s->covered_end = s->frozen_end = s->committed_end = frame;
  g_array_set_size(s->sums, 0);
  g_array_set_size(s->weights, 0);
  clear_gap(s);
  s->previous = s->has_committed ? SYM_SPACE : SYM_NONE;
  s->blank_frames = 0;
  s->hist_lo = s->hist_hi = frame;
}
