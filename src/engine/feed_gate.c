/* feed_gate.c — the learned RBN feed gate, shadow mode (see feed_gate.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "feed_gate.h"

#include <math.h>
#include <string.h>

static const char *const NAMES[SKIM_FEED_GATE_NFEAT + 1] = {
  "hear", "rep", "age", "st_read", "st_sc", "sc", "conf", "snr", "wpm", "de",
  "row_cq", "dict", "parts", "idle",
  "w_rows", "w_top", "w_conf", "w_conf_min",
  "n_near", "near_hear", "near_sim", "near_sim_hear",
  "len", "digits", "suffix", "slash", "scp", "night",
  "b160m", "b80m", "b40m", "b30m", "b20m", "b17m", "b15m",
  NULL,
};
enum {
  F_HEAR, F_REP, F_AGE, F_ST_READ, F_ST_SC, F_SC, F_CONF, F_SNR, F_WPM, F_DE,
  F_ROW_CQ, F_DICT, F_PARTS, F_IDLE,
  F_W_ROWS, F_W_TOP, F_W_CONF, F_W_CONF_MIN,
  F_N_NEAR, F_NEAR_HEAR, F_NEAR_SIM, F_NEAR_SIM_HEAR,
  F_LEN, F_DIGITS, F_SUFFIX, F_SLASH, F_SCP, F_NIGHT,
  F_BAND0,
};
G_STATIC_ASSERT(F_BAND0 + 7 == SKIM_FEED_GATE_NFEAT);

const char *const *skim_feed_gate_names(void) { return NAMES; }

struct _SkimFeedGate {
  double bias, threshold, win_s, near_khz;
  double w[SKIM_FEED_GATE_NFEAT], mean[SKIM_FEED_GATE_NFEAT],
         sd[SKIM_FEED_GATE_NFEAT];
  char  *id;
};

static gboolean parse_num(const char *s, double *out) {
  char *end = NULL;
  *out = g_ascii_strtod(s, &end);
  return end != s && isfinite(*out);
}

SkimFeedGate *skim_feed_gate_load(const char *path, GError **error) {
  GKeyFile *kf = g_key_file_new();
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, error)) {
    g_key_file_free(kf);
    return NULL;
  }
  SkimFeedGate *g = g_new0(SkimFeedGate, 1);
  for (guint k = 0; k < SKIM_FEED_GATE_NFEAT; k++) { g->sd[k] = 1.0; }
  g->win_s = 120.0;
  g->near_khz = 0.15;
  gboolean ok = TRUE;
  const char *bad = NULL;
  char *v;
  static const struct { const char *key; gboolean need; gsize off; } GATE[] = {
    { "bias", TRUE, G_STRUCT_OFFSET(SkimFeedGate, bias) },
    { "threshold", TRUE, G_STRUCT_OFFSET(SkimFeedGate, threshold) },
    { "win_s", FALSE, G_STRUCT_OFFSET(SkimFeedGate, win_s) },
    { "near_khz", FALSE, G_STRUCT_OFFSET(SkimFeedGate, near_khz) },
  };
  for (guint i = 0; ok && i < G_N_ELEMENTS(GATE); i++) {
    v = g_key_file_get_value(kf, "gate", GATE[i].key, NULL);
    if (!v) {
      if (GATE[i].need) { ok = FALSE; bad = GATE[i].key; }
      continue;
    }
    ok = parse_num(g_strstrip(v), G_STRUCT_MEMBER_P(g, GATE[i].off));
    if (!ok) { bad = GATE[i].key; }
    g_free(v);
  }
  if (ok && (g->win_s <= 0 || g->near_khz <= 0)) { ok = FALSE; bad = "win_s/near_khz"; }
  gsize n = 0;
  char **keys = ok ? g_key_file_get_keys(kf, "weights", &n, NULL) : NULL;
  for (gsize i = 0; ok && keys && i < n; i++) {
    gint k = -1;
    for (gint j = 0; j < SKIM_FEED_GATE_NFEAT; j++) {
      if (strcmp(keys[i], NAMES[j]) == 0) { k = j; break; }
    }
    v = g_key_file_get_value(kf, "weights", keys[i], NULL);
    char **f = g_strsplit_set(g_strstrip(v), " \t", -1);
    guint m = 0;
    double num[3];
    for (char **s = f; *s && ok; s++) {
      if (**s == '\0')
        continue;
      if (m == 3 || !parse_num(*s, &num[m])) { ok = FALSE; break; }
      m++;
    }
    if (k < 0 || m != 3 || num[2] <= 0) { ok = FALSE; }
    if (ok) {
      g->w[k] = num[0];
      g->mean[k] = num[1];
      g->sd[k] = num[2];
    } else {
      bad = keys[i];
    }
    g_strfreev(f);
    g_free(v);
  }
  if (!ok) {
    g_set_error(error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                "%s: bad or missing \"%s\"", path, bad ? bad : "?");
    g_strfreev(keys);
    g_key_file_free(kf);
    g_free(g);
    return NULL;
  }
  g->id = g_path_get_basename(path);
  g_strfreev(keys);
  g_key_file_free(kf);
  return g;
}

void skim_feed_gate_free(SkimFeedGate *g) {
  if (!g)
    return;
  g_free(g->id);
  g_free(g);
}

double skim_feed_gate_threshold(const SkimFeedGate *g) { return g->threshold; }
double skim_feed_gate_win_s(const SkimFeedGate *g) { return g->win_s; }
double skim_feed_gate_near_khz(const SkimFeedGate *g) { return g->near_khz; }
const char *skim_feed_gate_id(const SkimFeedGate *g) { return g->id; }

double skim_feed_gate_p(const SkimFeedGate *g, const double *x) {
  double z = g->bias;
  for (guint k = 0; k < SKIM_FEED_GATE_NFEAT; k++) {
    z += g->w[k] * (x[k] - g->mean[k]) / g->sd[k];
  }
  return 1.0 / (1.0 + exp(-z));
}

/* ---- likeness ------------------------------------------------------------- */

static gint levenshtein(const char *a, const char *b, gint limit) {
  const gint la = (gint)strlen(a), lb = (gint)strlen(b);
  if (ABS(la - lb) > limit)
    return limit + 1;
  gint prev[32], cur[32];
  if (lb >= 31)
    return limit + 1;
  for (gint j = 0; j <= lb; j++) { prev[j] = j; }
  for (gint i = 1; i <= la; i++) {
    cur[0] = i;
    gint lo = cur[0];
    for (gint j = 1; j <= lb; j++) {
      cur[j] = MIN(MIN(prev[j] + 1, cur[j - 1] + 1),
                   prev[j - 1] + (a[i - 1] != b[j - 1]));
      lo = MIN(lo, cur[j]);
    }
    if (lo > limit)
      return limit + 1;
    memcpy(prev, cur, sizeof(gint) * (gsize)(lb + 1));
  }
  return prev[lb];
}

gint skim_feed_gate_similar(const char *a, const char *b, gint k) {
  const gsize la = strlen(a), lb = strlen(b);
  if (strcmp(a, b) == 0 || MIN(la, lb) < 3)
    return -1;
  if (g_str_has_prefix(a, b) || g_str_has_prefix(b, a) ||
      g_str_has_suffix(a, b) || g_str_has_suffix(b, a))
    return (gint)(la > lb ? la - lb : lb - la);
  const gint d = levenshtein(a, b, k);
  return d <= k ? d : -1;
}

/* ---- the tracker ------------------------------------------------------------ */

typedef struct {
  gint64 t_us;
  double khz;
  char   call[16];
  guint  hear;
  double conf;
  gboolean top;
} WinRow;

typedef struct {
  gint64 t_us;
  double sc, conf, snr, wpm;
  gboolean de, cq, dict;
  guint  parts, idle;
  guint  hear, rep;
  double age, read, st_sc;
} LastRow;

struct _SkimFeedGateTrack {
  gint64      win_us;
  double      near_khz;
  GQueue     *win;                             /* WinRow*, oldest first      */
  GHashTable *last;                            /* call → LastRow*            */
  guint       rows;
};

#define LAST_KEEP_US ((gint64)2 * 3600 * G_USEC_PER_SEC)  /* a call silent this long */

SkimFeedGateTrack *skim_feed_gate_track_new(double win_s, double near_khz) {
  SkimFeedGateTrack *t = g_new0(SkimFeedGateTrack, 1);
  t->win_us = (gint64)(win_s * G_USEC_PER_SEC);
  t->near_khz = near_khz;
  t->win = g_queue_new();
  t->last = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  return t;
}

void skim_feed_gate_track_free(SkimFeedGateTrack *t) {
  if (!t)
    return;
  g_queue_free_full(t->win, g_free);
  g_hash_table_destroy(t->last);
  g_free(t);
}

/* The gatelog prints these with fixed decimals and learn.py reads them
 * back: round the same way so live and offline see the same numbers. */
static double rnd(double v, double scale) { return round(v * scale) / scale; }

static void trim(SkimFeedGateTrack *t, gint64 now_us) {
  WinRow *r;
  while ((r = g_queue_peek_head(t->win)) && r->t_us < now_us - t->win_us) {
    g_free(g_queue_pop_head(t->win));
  }
}

void skim_feed_gate_track_row(SkimFeedGateTrack *t, gint64 t_us, double hz,
                              double wpm, double snr_db, double confidence,
                              const SkimCallsignCand *cand,
                              const SkimStation *st) {
  if (!t || !cand)
    return;
  const char *call = st ? st->call : cand->call;
  if (!call[0])
    return;
  WinRow *w = g_new0(WinRow, 1);
  w->t_us = t_us;
  w->khz = rnd(hz, 10) / 1000.0;
  g_strlcpy(w->call, call, sizeof(w->call));
  w->hear = st ? st->hearings : 0;
  w->conf = rnd(confidence, 1000);
  w->top = strcmp(cand->call, call) == 0;
  g_queue_push_tail(t->win, w);
  trim(t, t_us);

  LastRow *l = g_hash_table_lookup(t->last, call);
  if (!l) {
    l = g_new0(LastRow, 1);
    g_hash_table_insert(t->last, g_strdup(call), l);
  }
  memset(l, 0, sizeof(*l));
  l->t_us = t_us;
  l->sc = rnd(cand->score, 100);
  l->conf = rnd(confidence, 1000);
  l->snr = rnd(snr_db, 10);
  l->wpm = rnd(wpm, 10);
  l->de = cand->de_marked;
  l->cq = cand->cq_context;
  l->dict = cand->dict;
  l->parts = cand->parts;
  l->idle = cand->idle_tokens;
  if (st) {
    l->hear = st->hearings;
    l->rep = st->reports;
    l->age = rnd((st->last_heard - st->first_heard) / 1e6, 10);
    l->read = rnd((t_us - st->heard_us) / 1e6, 10);
    l->st_sc = rnd(st->score, 100);
  }
  if (++t->rows % 4096 == 0) {                 /* forget calls long silent   */
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, t->last);
    while (g_hash_table_iter_next(&it, &k, &v)) {
      if (((LastRow *)v)->t_us < t_us - LAST_KEEP_US) { g_hash_table_iter_remove(&it); }
    }
  }
}

static double lg(double v) { return v > -1.0 ? log1p(v) : 0.0; }

void skim_feed_gate_track_features(SkimFeedGateTrack *t, gint64 t_us,
                                   gint64 wall_us, const char *call,
                                   double spot_hz, const char *band,
                                   double *x) {
  memset(x, 0, sizeof(double) * SKIM_FEED_GATE_NFEAT);
  const LastRow *l = g_hash_table_lookup(t->last, call);
  if (l) {
    x[F_HEAR] = lg(l->hear);
    x[F_REP] = lg(l->rep);
    x[F_AGE] = lg(l->age);
    x[F_ST_READ] = lg(l->read);
    x[F_ST_SC] = l->st_sc;
    x[F_SC] = l->sc;
    x[F_CONF] = l->conf;
    x[F_SNR] = l->snr;
    x[F_WPM] = l->wpm;
    x[F_DE] = l->de;
    x[F_ROW_CQ] = l->cq;
    x[F_DICT] = l->dict;
    x[F_PARTS] = l->parts;
    x[F_IDLE] = lg(l->idle);
  }

  trim(t, t_us);
  const double f = rnd(spot_hz / 1000.0, 10);
  guint own = 0, near_n = 0;
  double top = 0, conf = 0, conf_min = 0;
  guint near_hear = 0, sim_hear = 0;
  gboolean sim = FALSE;
  GHashTable *near = g_hash_table_new(g_str_hash, g_str_equal);  /* call → max hear */
  for (GList *it = t->win->head; it; it = it->next) {
    const WinRow *r = it->data;
    if (strcmp(r->call, call) == 0) {
      conf_min = own ? MIN(conf_min, r->conf) : r->conf;
      own++;
      top += r->top;
      conf += r->conf;
    } else if (fabs(r->khz - f) <= t->near_khz + 1e-9) {
      const guint h = GPOINTER_TO_UINT(g_hash_table_lookup(near, r->call));
      if (!g_hash_table_contains(near, r->call)) { near_n++; }
      g_hash_table_insert(near, (gpointer)r->call, GUINT_TO_POINTER(MAX(h, r->hear)));
    }
  }
  GHashTableIter hi;
  gpointer k, v;
  g_hash_table_iter_init(&hi, near);
  while (g_hash_table_iter_next(&hi, &k, &v)) {
    const guint h = GPOINTER_TO_UINT(v);
    near_hear = MAX(near_hear, h);
    if (skim_feed_gate_similar(k, call, 2) >= 0) {
      sim = TRUE;
      sim_hear = MAX(sim_hear, h);
    }
  }
  g_hash_table_destroy(near);
  x[F_W_ROWS] = lg(own);
  x[F_W_TOP] = own ? top / own : 0;
  x[F_W_CONF] = own ? conf / own : 0;
  x[F_W_CONF_MIN] = own ? conf_min : 0;
  x[F_N_NEAR] = lg(near_n);
  x[F_NEAR_HEAR] = lg(near_hear);
  x[F_NEAR_SIM] = sim;
  x[F_NEAR_SIM_HEAR] = lg(sim_hear);

  const gsize len = strlen(call);
  gint last_digit = -1, digits = 0;
  for (gsize i = 0; i < len; i++) {
    if (g_ascii_isdigit(call[i])) {
      digits++;
      last_digit = (gint)i;
    }
  }
  x[F_LEN] = (double)len;
  x[F_DIGITS] = digits;
  x[F_SUFFIX] = last_digit >= 0 ? (double)len - 1 - last_digit : (double)len;
  x[F_SLASH] = strchr(call, '/') != NULL;
  x[F_SCP] = skim_callsign_dict_has(call);
  const gint hour = (gint)((wall_us / G_USEC_PER_SEC) % 86400 / 3600);
  x[F_NIGHT] = hour >= 18 || hour < 6;
  for (guint b = 0; band && b < 7; b++) {
    if (strcmp(band, NAMES[F_BAND0 + b] + 1) == 0) { x[F_BAND0 + b] = 1.0; }
  }
}
