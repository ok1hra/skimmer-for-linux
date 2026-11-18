/* stats.c — see stats.h. Part of skimmer-for-linux. GPL-3.0-or-later.
 *
 * Three rings, round-robin archives as in RRD: TIER[t] keeps len slots of
 * res minutes each — 1 min × 7 days, 15 min × 90 days, 1 h × 2 years. Every
 * minute put lands in all three: its own slot in the first, merged into the
 * 15-minute and the hour slot that hold it (counts summed, levels averaged
 * over the minutes that had a value, the worst lost, the last radio). The
 * slot of a minute in tier t is (minute / res mod len); a slot whose start
 * is not the current one is cleared first.
 *
 * File (version 4): a header, then the three rings one after the other.
 * Native byte order — the file never leaves the machine that wrote it.
 * Versions 1 and 2 (the minutes only) are read, their week merged into the
 * coarser rings; version 3 (the rings, no CPU temperature) is read slot by
 * slot; either way the file is written anew.
 *
 * Saving writes only the slots changed since the last save, in place (pwrite
 * on the file kept open): a few kB every few minutes. The whole file is
 * written — atomically, to a temp file renamed over it — only when there is
 * none yet, when it is of an older version, or when the config's bands
 * differ from the file's columns (they are matched by name and re-ordered).
 * A slot torn by a power cut costs that slot.
 *
 * /stats.json:
 *   { "now": <unix s>, "t0": <unix s of the first bucket>, "step_min": S, "n": N,
 *     "res_min": R (the ring the answer comes from),
 *     "first": <unix s of the oldest record kept> | null,
 *     "bands": ["160m", …],
 *     "cw":   { "spots": { "160m": [v|null × N], … }, "stations": …, "active": …,
 *               "folded": …, "gate_below": …, "snr": … },
 *     "rtty": { … } | null              (null while no band decoded RTTY)
 *     "global": { "cpu": [ … ], "lost": …, "clients": …, "streaming": …, "radio": …,
 *                 "cpu_cw": …, "cpu_rtty": …, "cpu_temp": … } }
 *   A bucket sums the counts (spots, stations, folded, gate_below), averages
 *   the levels (active, cpu, clients, snr, streaming = the share of minutes
 *   streamed) over the minutes that had them, takes the worst lost, the
 *   hottest CPU temperature and the last radio. null = no record. The answer comes from the finest ring that
 *   covers the span; its step is a whole number of that ring's slots.
 */
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "json.h"

#define MAGIC   "SKIMSTAT"
#define VERSION 4                    /* 3: the 15-minute and hour rings;
                                      * 4: the CPU temperature               */
#define V1_SG_N 5                    /* version 1's global metrics           */
#define V3_SG_N 7                    /* versions 2 and 3's                   */
#define NV      (SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N + SKIM_SG_N)
#define NTIER   3

static const struct { guint res, len; } TIER[NTIER] = {
  { 1, SKIM_STATS_MINUTES },                   /* 1 min × 7 days            */
  { 15, 90 * 24 * 4 },                         /* 15 min × 90 days          */
  { 60, 2 * 365 * 24 },                        /* 1 h × 2 years             */
};

typedef enum { AGG_SUM, AGG_MEAN, AGG_MAX, AGG_LAST } Agg;

static const struct { const char *name; Agg agg; } BAND_M[SKIM_SM_N] = {
  [SKIM_SM_SPOTS] = { "spots", AGG_SUM },       [SKIM_SM_STATIONS] = { "stations", AGG_SUM },
  [SKIM_SM_ACTIVE] = { "active", AGG_MEAN },    [SKIM_SM_FOLDED] = { "folded", AGG_SUM },
  [SKIM_SM_GATE_BELOW] = { "gate_below", AGG_SUM }, [SKIM_SM_SNR] = { "snr", AGG_MEAN },
};
static const struct { const char *name; Agg agg; } GLOB_M[SKIM_SG_N] = {
  [SKIM_SG_CPU] = { "cpu", AGG_MEAN },          [SKIM_SG_LOST] = { "lost", AGG_MAX },
  [SKIM_SG_CLIENTS] = { "clients", AGG_MEAN },  [SKIM_SG_STREAMING] = { "streaming", AGG_MEAN },
  [SKIM_SG_RADIO] = { "radio", AGG_LAST },
  [SKIM_SG_CPU_CW] = { "cpu_cw", AGG_MEAN },    [SKIM_SG_CPU_RTTY] = { "cpu_rtty", AGG_MEAN },
  [SKIM_SG_CPU_TEMP] = { "cpu_temp", AGG_MAX },
};

typedef struct {
  char    magic[8];
  guint32 version, minutes, bands, modes, sm_n, sg_n;
  char    names[SKIM_STATS_BANDS][16];
} Header;                                      /* versions 1 and 2           */

typedef struct {
  Header  h;                                   /* versions 3 and 4, minutes = 0 */
  guint32 ntier, tier_res[NTIER], tier_len[NTIER], pad;
} Header3;

/* A slot of any ring: the minute its span starts at (0 = empty), the values
 * (as a minute record) and how many minutes each value merges. */
typedef struct {
  gint64          start;
  SkimStatsMinute m;
  guint8          n[NV];
} Slot;

/* A minute record of versions 2 and 3 (7 global metrics). */
typedef struct {
  float band[SKIM_STATS_BANDS][SKIM_STATS_MODES][SKIM_SM_N];
  float glob[V3_SG_N];
} MinuteV3;
#define NV3 (SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N + V3_SG_N)

/* Version 3: a ring slot of those. */
typedef struct {
  gint64   start;
  MinuteV3 m;
  guint8   n[NV3];
} SlotV3;

/* Versions 1 and 2: a minute and its record (version 1: 5 global metrics). */
typedef struct {
  gint64   minute;
  MinuteV3 m;
} SlotV2;
typedef struct {
  gint64 minute;
  float  band[SKIM_STATS_BANDS][SKIM_STATS_MODES][SKIM_SM_N];
  float  glob[V1_SG_N];
} SlotV1;

struct _SkimStats {
  char    *path;
  char     names[SKIM_STATS_BANDS][16];
  guint    nb;
  Slot    *ring[NTIER];
  guint8  *dirty_slot[NTIER];                  /* bitmaps: changed since a save */
  gboolean dirty;                              /* something to write          */
  gboolean full;                               /* … the whole file            */
  int      fd;                                 /* open for pwrite; -1         */
};

/* The values of a record as one array, and the aggregation of each. */
static inline float *vals(SkimStatsMinute *m) { return (float *)m; }
static inline const float *cvals(const SkimStatsMinute *m) { return (const float *)m; }
G_STATIC_ASSERT(sizeof(SkimStatsMinute) == NV * sizeof(float));

static Agg agg_of(guint v) {
  const guint nb = SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N;
  return v < nb ? BAND_M[v % SKIM_SM_N].agg : GLOB_M[v - nb].agg;
}

static void empty_minute(SkimStatsMinute *m) {
  float *v = vals(m);
  for (guint i = 0; i < NV; i++) { v[i] = NAN; }
}

static void slot_clear(Slot *sl, gint64 start) {
  sl->start = start;
  empty_minute(&sl->m);
  memset(sl->n, 0, sizeof(sl->n));
}

static void mark(SkimStats *s, guint t, guint i) {
  s->dirty_slot[t][i / 8] |= (guint8)(1u << (i % 8));
  s->dirty = TRUE;
}

/* One minute into ring t. */
static void merge(SkimStats *s, guint t, gint64 minute, const SkimStatsMinute *m) {
  const gint64 start = minute - minute % TIER[t].res;
  const guint i = (guint)((minute / TIER[t].res) % TIER[t].len);
  Slot *sl = &s->ring[t][i];
  if (sl->start != start) { slot_clear(sl, start); }
  float *v = vals(&sl->m);
  const float *x = cvals(m);
  for (guint j = 0; j < NV; j++) {
    if (isnan(x[j])) { continue; }
    const guint8 n = sl->n[j];
    switch (agg_of(j)) {
      case AGG_SUM:  v[j] = n ? v[j] + x[j] : x[j]; break;
      case AGG_MEAN: v[j] = n ? (v[j] * n + x[j]) / (n + 1) : x[j]; break;
      case AGG_MAX:  v[j] = n ? MAX(v[j], x[j]) : x[j]; break;
      case AGG_LAST: v[j] = x[j]; break;
    }
    if (n < 255) { sl->n[j] = n + 1; }
  }
  mark(s, t, i);
}

/* ---- the file -------------------------------------------------------------------- */

static gsize file_len_of(gsize slot) {
  gsize n = sizeof(Header3);
  for (guint t = 0; t < NTIER; t++) { n += (gsize)TIER[t].len * slot; }
  return n;
}
static gsize file_len(void) { return file_len_of(sizeof(Slot)); }

static gsize ring_offset_of(guint t, gsize slot) {
  gsize n = sizeof(Header3);
  for (guint u = 0; u < t; u++) { n += (gsize)TIER[u].len * slot; }
  return n;
}
static gsize ring_offset(guint t) { return ring_offset_of(t, sizeof(Slot)); }

/* A version 2/3 record (and its merge counts) as this version's: the CPU
 * temperature empty. */
static void from_v3(SkimStatsMinute *o, guint8 *on, const MinuteV3 *in, const guint8 *in_n) {
  empty_minute(o);
  memcpy(o->band, in->band, sizeof(o->band));
  memcpy(o->glob, in->glob, sizeof(in->glob));
  if (on) {
    memset(on, 0, NV);
    memcpy(on, in_n, NV3);           /* the band values, then the globals */
  }
}

/* The file's band column for each of ours (by name), -1 = none. */
static void columns(const char names[][16], const SkimStats *s, int *from) {
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    from[b] = -1;
    for (guint f = 0; b < s->nb && f < SKIM_STATS_BANDS; f++) {
      if (names[f][0] && strncmp(names[f], s->names[b], 16) == 0) { from[b] = (int)f; }
    }
  }
}

static void remap(SkimStatsMinute *o, const SkimStatsMinute *in, const int *from, guint nb,
                  guint8 *on, const guint8 *in_n) {
  SkimStatsMinute m;
  empty_minute(&m);
  guint8 n[NV] = { 0 };
  memcpy(m.glob, in->glob, sizeof(m.glob));
  const guint gb = SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N;
  if (in_n) { memcpy(n + gb, in_n + gb, SKIM_SG_N); }
  for (guint b = 0; b < nb; b++) {
    if (from[b] < 0) { continue; }
    memcpy(m.band[b], in->band[from[b]], sizeof(m.band[b]));
    if (in_n) {
      const guint per = SKIM_STATS_MODES * SKIM_SM_N;
      memcpy(n + b * per, in_n + from[b] * per, per);
    }
  }
  *o = m;
  if (on) { memcpy(on, n, NV); }
}

/* Version 1 or 2: the week of minutes, merged into every ring. */
static void load_old(SkimStats *s, const char *data, const Header *h, gboolean v1) {
  int from[SKIM_STATS_BANDS];
  columns(h->names, s, from);
  const SlotV2 *in = (const SlotV2 *)(data + sizeof(Header));
  const SlotV1 *in1 = (const SlotV1 *)(data + sizeof(Header));
  /* oldest first, so the coarse slots fill in time order */
  GArray *order = g_array_new(FALSE, FALSE, sizeof(guint));
  gint64 newest = 0;
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
    newest = MAX(newest, v1 ? in1[i].minute : in[i].minute);
  }
  for (gint64 m = newest - SKIM_STATS_MINUTES + 1; newest && m <= newest; m++) {
    const guint i = (guint)(m % SKIM_STATS_MINUTES);
    if ((v1 ? in1[i].minute : in[i].minute) == m) { g_array_append_val(order, i); }
  }
  for (guint k = 0; k < order->len; k++) {
    const guint i = g_array_index(order, guint, k);
    SkimStatsMinute mm, src;
    empty_minute(&src);
    gint64 minute;
    if (v1) {                                  /* the CPU split stays NaN    */
      minute = in1[i].minute;
      memcpy(src.band, in1[i].band, sizeof(src.band));
      memcpy(src.glob, in1[i].glob, sizeof(in1[i].glob));
    } else {
      minute = in[i].minute;
      from_v3(&src, NULL, &in[i].m, NULL);
    }
    remap(&mm, &src, from, s->nb, NULL, NULL);
    for (guint t = 0; t < NTIER; t++) { merge(s, t, minute, &mm); }
  }
  g_array_unref(order);
  g_message("stats: %s is a version %u history — carried over, its week merged into the "
            "15-minute and hour rings", s->path, h->version);
}

/* TRUE when the file can be written in place as it is (version 4, these
 * bands). */
static gboolean load(SkimStats *s) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(s->path, &data, &len, NULL)) { return FALSE; }
  const Header *h = (const Header *)data;
  const gboolean head_ok = len >= sizeof(Header) && memcmp(h->magic, MAGIC, 8) == 0 &&
                           h->bands == SKIM_STATS_BANDS && h->modes == SKIM_STATS_MODES &&
                           h->sm_n == SKIM_SM_N;
  const gboolean v1 = head_ok && h->version == 1 && h->sg_n == V1_SG_N &&
                      h->minutes == SKIM_STATS_MINUTES &&
                      len == sizeof(Header) + SKIM_STATS_MINUTES * sizeof(SlotV1);
  const gboolean v2 = head_ok && h->version == 2 && h->sg_n == V3_SG_N &&
                      h->minutes == SKIM_STATS_MINUTES &&
                      len == sizeof(Header) + SKIM_STATS_MINUTES * sizeof(SlotV2);
  const Header3 *h3 = (const Header3 *)data;
  gboolean rings_ok = len >= sizeof(Header3) && h3->ntier == NTIER;
  for (guint t = 0; rings_ok && t < NTIER; t++) {
    rings_ok = h3->tier_res[t] == TIER[t].res && h3->tier_len[t] == TIER[t].len;
  }
  const gboolean v3 = head_ok && rings_ok && h->version == 3 && h->sg_n == V3_SG_N &&
                      len == file_len_of(sizeof(SlotV3));
  const gboolean v4 = head_ok && rings_ok && h->version == VERSION && h->sg_n == SKIM_SG_N &&
                      len == file_len();
  gboolean same = FALSE;
  if (v1 || v2) {
    load_old(s, data, h, v1);
    s->dirty = TRUE;                           /* written as version 4      */
  } else if (v3) {                             /* slot by slot, temp empty  */
    int from[SKIM_STATS_BANDS];
    columns(h->names, s, from);
    for (guint t = 0; t < NTIER; t++) {
      const SlotV3 *in = (const SlotV3 *)(data + ring_offset_of(t, sizeof(SlotV3)));
      for (guint i = 0; i < TIER[t].len; i++) {
        SkimStatsMinute m;
        guint8 n[NV];
        from_v3(&m, n, &in[i].m, in[i].n);
        Slot *o = &s->ring[t][i];
        o->start = in[i].start;
        remap(&o->m, &m, from, s->nb, o->n, n);
      }
    }
    s->dirty = TRUE;                           /* written as version 4      */
    /* an older build reads version 3 only (and would start afresh on 4):
     * the file as it was stays beside it */
    char *keep = g_strconcat(s->path, ".v3", NULL);
    if (!g_file_test(keep, G_FILE_TEST_EXISTS)) { g_file_set_contents(keep, data, (gssize)len, NULL); }
    g_message("stats: %s is a version 3 history — carried over, the CPU temperature "
              "starts now (the old file kept as %s, for an older build)", s->path, keep);
    g_free(keep);
  } else if (v4) {
    int from[SKIM_STATS_BANDS];
    columns(h->names, s, from);
    for (guint t = 0; t < NTIER; t++) {
      const Slot *in = (const Slot *)(data + ring_offset(t));
      for (guint i = 0; i < TIER[t].len; i++) {
        Slot *o = &s->ring[t][i];
        o->start = in[i].start;
        remap(&o->m, &in[i].m, from, s->nb, o->n, in[i].n);
      }
    }
    same = memcmp(h->names, s->names, sizeof(s->names)) == 0;
    if (!same) { s->dirty = TRUE; }            /* re-ordered: written whole */
  } else {
    g_warning("stats: %s is not this version's history — starting a fresh one", s->path);
  }
  g_free(data);
  return same;
}

SkimStats *skim_stats_open(const char *path, const char *const *bands, guint nb) {
  SkimStats *s = g_new0(SkimStats, 1);
  s->path = g_strdup(path);
  s->nb = MIN(nb, (guint)SKIM_STATS_BANDS);
  for (guint b = 0; b < s->nb; b++) { g_strlcpy(s->names[b], bands[b], 16); }
  for (guint t = 0; t < NTIER; t++) {
    s->ring[t] = g_new(Slot, TIER[t].len);
    for (guint i = 0; i < TIER[t].len; i++) { slot_clear(&s->ring[t][i], 0); }
    s->dirty_slot[t] = g_new0(guint8, TIER[t].len / 8 + 1);
  }
  s->fd = -1;
  s->full = !load(s);
  if (!s->full) {
    s->fd = g_open(s->path, O_RDWR | O_CLOEXEC, 0);
    if (s->fd < 0) { s->full = TRUE; }
  }
  return s;
}

void skim_stats_put(SkimStats *s, gint64 unix_minute, const SkimStatsMinute *m) {
  for (guint t = 0; t < NTIER; t++) { merge(s, t, unix_minute, m); }
}

static void clean(SkimStats *s) {
  for (guint t = 0; t < NTIER; t++) { memset(s->dirty_slot[t], 0, TIER[t].len / 8 + 1); }
  s->dirty = FALSE;
}

/* The changed slots, each at its place in the open file. */
static gboolean save_slots(SkimStats *s, GError **error) {
  for (guint t = 0; t < NTIER; t++) {
    const gsize base = ring_offset(t);
    for (guint i = 0; i < TIER[t].len; i++) {
      if (!(s->dirty_slot[t][i / 8] & (1u << (i % 8)))) { continue; }
      const off_t at = (off_t)(base + (gsize)i * sizeof(Slot));
      if (pwrite(s->fd, &s->ring[t][i], sizeof(Slot), at) != (gssize)sizeof(Slot)) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s: %s", s->path,
                    g_strerror(errno));
        return FALSE;
      }
    }
  }
  return TRUE;
}

gboolean skim_stats_save(SkimStats *s, GError **error) {
  if (!s->dirty) { return TRUE; }
  if (!s->full && s->fd >= 0) {
    if (save_slots(s, error)) {
      clean(s);
      return TRUE;
    }
    s->full = TRUE;                            /* next time: the whole file */
    return FALSE;
  }
  const gsize len = file_len();
  char *buf = g_malloc0(len);
  Header3 *h = (Header3 *)buf;
  memcpy(h->h.magic, MAGIC, 8);
  h->h.version = VERSION;
  h->h.bands = SKIM_STATS_BANDS;
  h->h.modes = SKIM_STATS_MODES;
  h->h.sm_n = SKIM_SM_N;
  h->h.sg_n = SKIM_SG_N;
  memcpy(h->h.names, s->names, sizeof(h->h.names));
  h->ntier = NTIER;
  for (guint t = 0; t < NTIER; t++) {
    h->tier_res[t] = TIER[t].res;
    h->tier_len[t] = TIER[t].len;
    memcpy(buf + ring_offset(t), s->ring[t], (gsize)TIER[t].len * sizeof(Slot));
  }
  char *dir = g_path_get_dirname(s->path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  const gboolean ok = g_file_set_contents(s->path, buf, (gssize)len, error);
  g_free(buf);
  if (ok) {                                    /* a new file: open it anew  */
    if (s->fd >= 0) { close(s->fd); }
    s->fd = g_open(s->path, O_RDWR | O_CLOEXEC, 0);
    s->full = s->fd < 0;
    clean(s);
  }
  return ok;
}

void skim_stats_free(SkimStats *s) {
  if (!s) { return; }
  GError *err = NULL;
  if (!skim_stats_save(s, &err)) {
    g_warning("stats: %s", err->message);
    g_clear_error(&err);
  }
  if (s->fd >= 0) { close(s->fd); }
  for (guint t = 0; t < NTIER; t++) {
    g_free(s->dirty_slot[t]);
    g_free(s->ring[t]);
  }
  g_free(s->path);
  g_free(s);
}

/* ---- JSON ------------------------------------------------------------------------ */

typedef struct {
  double acc;
  guint  n;                                    /* minutes in it              */
  gint64 at;                                   /* AGG_LAST: the newest slot  */
} Bucket;

static void bucket_add(Bucket *k, Agg agg, float v, guint n, gint64 start) {
  if (isnan(v) || !n) { return; }
  switch (agg) {
    case AGG_MAX:  k->acc = k->n ? MAX(k->acc, v) : v; break;
    case AGG_LAST: if (!k->n || start >= k->at) { k->acc = v; k->at = start; } break;
    case AGG_MEAN: k->acc += (double)v * n; break;  /* weighted by its minutes */
    case AGG_SUM:  k->acc += v; break;
  }
  k->n += n;
}

static void bucket_out(GString *o, const Bucket *k, Agg agg, guint i) {
  if (i) { g_string_append_c(o, ','); }
  if (!k->n) { g_string_append(o, "null"); return; }
  const double v = agg == AGG_MEAN ? k->acc / k->n : k->acc;
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(num, sizeof(num), "%.4g", v);
  g_string_append(o, num);
}

/* One series from ring t: value index v. */
static void series(GString *o, SkimStats *s, guint t, gint64 first, guint step, guint n,
                   guint v) {
  Bucket *bk = g_new0(Bucket, n);
  const Agg agg = agg_of(v);
  const gint64 end = first + (gint64)step * n;
  for (guint i = 0; i < TIER[t].len; i++) {
    const Slot *sl = &s->ring[t][i];
    if (!sl->start || sl->start < first || sl->start >= end) { continue; }
    bucket_add(&bk[(sl->start - first) / step], agg, cvals(&sl->m)[v], sl->n[v], sl->start);
  }
  g_string_append_c(o, '[');
  for (guint i = 0; i < n; i++) { bucket_out(o, &bk[i], agg, i); }
  g_string_append_c(o, ']');
  g_free(bk);
}

static gboolean mode_used(SkimStats *s, guint t, guint k) {
  for (guint i = 0; i < TIER[t].len; i++) {
    for (guint b = 0; s->ring[t][i].start && b < s->nb; b++) {
      if (!isnan(s->ring[t][i].m.band[b][k][SKIM_SM_ACTIVE])) { return TRUE; }
    }
  }
  return FALSE;
}

static guint band_value(guint b, guint k, guint j) {
  return (b * SKIM_STATS_MODES + k) * SKIM_SM_N + j;
}

char *skim_stats_json(SkimStats *s, gint64 now_minute, guint hours, guint step) {
  const guint max_h = TIER[NTIER - 1].res * TIER[NTIER - 1].len / 60;
  hours = CLAMP(hours, 1u, max_h);
  const guint span = hours * 60;
  /* the finest ring that covers the span; the step a whole number of its slots */
  guint t = 0;
  while (t + 1 < NTIER && (gsize)TIER[t].res * TIER[t].len < span) { t++; }
  const guint res = TIER[t].res;
  if (!step) { step = MAX(1u, (span + 719) / 720); }
  step = CLAMP(step, 1u, span);
  step = (step + res - 1) / res * res;
  const guint n = (span + step - 1) / step;
  /* buckets end with the current minute, aligned to the step */
  const gint64 last = now_minute - now_minute % step;
  const gint64 first = last - (gint64)step * (n - 1);
  GString *o = g_string_new(NULL);
  gint64 oldest = 0;                           /* for a time slider          */
  for (guint u = 0; u < NTIER; u++) {
    for (guint i = 0; i < TIER[u].len; i++) {
      const gint64 st = s->ring[u][i].start;
      if (st && (!oldest || st < oldest)) { oldest = st; }
    }
  }
  g_string_append_printf(o, "{\"now\":%" G_GINT64_FORMAT ",\"t0\":%" G_GINT64_FORMAT
                         ",\"step_min\":%u,\"n\":%u,\"res_min\":%u,\"first\":",
                         now_minute * 60, first * 60, step, n, res);
  if (oldest) { g_string_append_printf(o, "%" G_GINT64_FORMAT, oldest * 60); }
  else { g_string_append(o, "null"); }
  g_string_append(o, ",\"bands\":[");
  for (guint b = 0; b < s->nb; b++) {
    g_string_append_printf(o, "%s\"", b ? "," : "");
    skim_json_append(o, s->names[b]);
    g_string_append_c(o, '"');
  }
  g_string_append_c(o, ']');
  static const char *MODE[SKIM_STATS_MODES] = { "cw", "rtty" };
  for (guint k = 0; k < SKIM_STATS_MODES; k++) {
    g_string_append_printf(o, ",\"%s\":", MODE[k]);
    if (k && !mode_used(s, t, k)) { g_string_append(o, "null"); continue; }
    g_string_append_c(o, '{');
    for (guint j = 0; j < SKIM_SM_N; j++) {
      g_string_append_printf(o, "%s\"%s\":{", j ? "," : "", BAND_M[j].name);
      for (guint b = 0; b < s->nb; b++) {
        g_string_append_printf(o, "%s\"", b ? "," : "");
        skim_json_append(o, s->names[b]);
        g_string_append(o, "\":");
        series(o, s, t, first, step, n, band_value(b, k, j));
      }
      g_string_append_c(o, '}');
    }
    g_string_append_c(o, '}');
  }
  g_string_append(o, ",\"global\":{");
  const guint gb = SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N;
  for (guint j = 0; j < SKIM_SG_N; j++) {
    g_string_append_printf(o, "%s\"%s\":", j ? "," : "", GLOB_M[j].name);
    series(o, s, t, first, step, n, gb + j);
  }
  g_string_append(o, "}}");
  return g_string_free(o, FALSE);
}
