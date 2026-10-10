/* feed_fold.c — see feed_fold.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "feed_fold.h"

#include "feed_gate.h"

#include <math.h>

typedef struct {
  double   hz;
  gint64   at_us;              /* the newest line                            */
  guint    hear;               /* most hearings a line of this run carried   */
  gboolean dict;
} Sent;

struct _SkimFeedFold {
  GHashTable *by_call;         /* call (owned) → Sent* (owned)               */
  gint64      window_us, self_us, keep_us;
  double      df_hz;
};

SkimFeedFold *skim_feed_fold_new(gint64 window_us, gint64 self_us, double df_hz) {
  SkimFeedFold *f = g_new0(SkimFeedFold, 1);
  f->by_call   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  f->window_us = window_us;
  f->self_us   = self_us;
  f->keep_us   = MAX(window_us, self_us);
  f->df_hz     = df_hz;
  return f;
}

void skim_feed_fold_free(SkimFeedFold *f) {
  if (!f)
    return;
  g_hash_table_destroy(f->by_call);
  g_free(f);
}

void skim_feed_fold_sent(SkimFeedFold *f, const char *call, double freq_hz,
                         gint64 now_us, guint hearings, gboolean dict) {
  Sent *s = g_hash_table_lookup(f->by_call, call);
  if (!s) {
    s = g_new0(Sent, 1);
    g_hash_table_insert(f->by_call, g_strdup(call), s);
  } else if (now_us - s->at_us > f->self_us) {
    s->hear = 0;                               /* a new run, a new count     */
  }
  s->hz    = freq_hz;
  s->at_us = now_us;
  s->hear  = MAX(s->hear, hearings);
  s->dict  = dict;
}

const char *skim_feed_fold_check(SkimFeedFold *f, const char *call,
                                 double freq_hz, gint64 now_us, guint hearings,
                                 gboolean dict) {
  const Sent *own = g_hash_table_lookup(f->by_call, call);
  if (own && now_us - own->at_us <= f->self_us)
    return NULL;                               /* its run goes on            */
  const char *by = NULL;
  GHashTableIter it;
  gpointer key, val;
  g_hash_table_iter_init(&it, f->by_call);
  while (g_hash_table_iter_next(&it, &key, &val)) {
    const Sent *s = val;
    if (now_us - s->at_us > f->keep_us) {
      g_hash_table_iter_remove(&it);
      continue;
    }
    if (by || now_us - s->at_us > f->window_us ||
        fabs(s->hz - freq_hz) > f->df_hz ||
        skim_feed_gate_similar(call, key, 2) < 0)
      continue;
    /* the dictionary speaks only for a call heard at least as often: a cut
     * twin that happens to be a call too (SP2R of SP2RCL, SM5D of SM5DL)
     * must not silence the station keyed over and over */
    if ((s->dict && !dict && s->hear >= hearings) ||
        (s->dict == dict && s->hear > hearings)) {
      by = key;
    }
  }
  return by;
}
