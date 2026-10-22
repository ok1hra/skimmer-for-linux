/* rtty_pairs.c — see rtty_pairs.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "rtty_pairs.h"

#include <math.h>

#define SAME_PAIR_HZ 30.0

typedef struct {
  double hz;
  gint64 until_us;
} Pair;

struct _SkimRttyPairs {
  gint    refs;
  GMutex  lock;
  GArray *pairs;                /* Pair                                       */
};

SkimRttyPairs *skim_rtty_pairs_new(void) {
  SkimRttyPairs *b = g_new0(SkimRttyPairs, 1);
  b->refs = 1;
  g_mutex_init(&b->lock);
  b->pairs = g_array_new(FALSE, FALSE, sizeof(Pair));
  return b;
}

SkimRttyPairs *skim_rtty_pairs_ref(SkimRttyPairs *b) {
  if (b) { g_atomic_int_inc(&b->refs); }
  return b;
}

void skim_rtty_pairs_unref(SkimRttyPairs *b) {
  if (!b || !g_atomic_int_dec_and_test(&b->refs))
    return;
  g_array_free(b->pairs, TRUE);
  g_mutex_clear(&b->lock);
  g_free(b);
}

void skim_rtty_pairs_publish(SkimRttyPairs *b, gint64 now_us, const double *centre_hz,
                             guint n) {
  g_mutex_lock(&b->lock);
  for (guint i = 0; i < b->pairs->len;) {       /* expired pairs leave       */
    if (g_array_index(b->pairs, Pair, i).until_us < now_us) {
      g_array_remove_index_fast(b->pairs, i);
    } else {
      i++;
    }
  }
  for (guint k = 0; k < n; k++) {
    Pair *hit = NULL;
    for (guint i = 0; i < b->pairs->len && !hit; i++) {
      Pair *q = &g_array_index(b->pairs, Pair, i);
      if (fabs(q->hz - centre_hz[k]) <= SAME_PAIR_HZ) { hit = q; }
    }
    if (hit) {
      hit->hz = centre_hz[k];
      hit->until_us = now_us + SKIM_RTTY_PAIR_LINGER_US;
    } else {
      const Pair q = { centre_hz[k], now_us + SKIM_RTTY_PAIR_LINGER_US };
      g_array_append_val(b->pairs, q);
    }
  }
  g_mutex_unlock(&b->lock);
}

gboolean skim_rtty_pairs_covers(SkimRttyPairs *b, gint64 now_us, double hz) {
  gboolean on = FALSE;
  g_mutex_lock(&b->lock);
  for (guint i = 0; i < b->pairs->len && !on; i++) {
    const Pair *q = &g_array_index(b->pairs, Pair, i);
    on = q->until_us >= now_us &&
         fabs(hz - q->hz) <= SKIM_RTTY_PAIR_HALF_HZ + SKIM_RTTY_PAIR_EDGE_HZ;
  }
  g_mutex_unlock(&b->lock);
  return on;
}

guint skim_rtty_pairs_count(SkimRttyPairs *b, gint64 now_us) {
  guint n = 0;
  g_mutex_lock(&b->lock);
  for (guint i = 0; i < b->pairs->len; i++) {
    n += g_array_index(b->pairs, Pair, i).until_us >= now_us;
  }
  g_mutex_unlock(&b->lock);
  return n;
}
