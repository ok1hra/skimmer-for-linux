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
 * File (version 8): a header, then the three rings one after the other.
 * Native byte order — the file never leaves the machine that wrote it.
 * Versions 1 and 2 (the minutes only) are read, their week merged into the
 * coarser rings; versions 3–7 (the rings: 8 bands, two to four modes, fewer
 * metrics) are read slot by slot; either way the file is written anew
 * (≈ 84 MB: 16 bands since version 8 — a Red Pitaya's two IPs).
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
 *     "ft8":  { … } | null              (FT8 + FT4; null while none decoded)
 *     "js8":  { … } | null              (all submodes; null while none decoded)
 *     "global": { "cpu": [ … ], "lost": …, "clients": …, "streaming": …, "radio": …,
 *                 "cpu_cw": …, "cpu_rtty": …, "cpu_temp": …, "cpu_helpers": …,
 *                 "rp_temp": …, "rp_load": …, "rp_mbps_a": …, "rp_mbps_b": … } }
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
#define VERSION 8                    /* 3: the 15-minute and hour rings;
                                      * 4: the CPU temperature;
                                      * 5: the thermal guard's pauses;
                                      * 6: FT8, the helpers' CPU;
                                      * 7: JS8;
                                      * 8: 16 bands, the Red Pitaya's series */
#define OLD_BANDS 8                  /* versions 1–7                         */
#define V7_SG_N 9                    /* versions 6 and 7's global metrics    */
#define OLD_MODES 2                  /* versions 1–5: CW, RTTY               */
#define V6_MODES 3                   /* version 6: CW, RTTY, FT8             */
#define V4_SM_N 6                    /* versions 1–4's band metrics          */
#define V5_SG_N 8                    /* version 5's global metrics           */
#define V1_SG_N 5                    /* version 1's global metrics           */
#define V3_SG_N 7                    /* versions 2 and 3's                   */
#define V4_SG_N 8                    /* version 4's                          */
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
  [SKIM_SM_THERMAL] = { "thermal", AGG_MEAN },
};
static const struct { const char *name; Agg agg; } GLOB_M[SKIM_SG_N] = {
  [SKIM_SG_CPU] = { "cpu", AGG_MEAN },          [SKIM_SG_LOST] = { "lost", AGG_MAX },
  [SKIM_SG_CLIENTS] = { "clients", AGG_MEAN },  [SKIM_SG_STREAMING] = { "streaming", AGG_MEAN },
  [SKIM_SG_RADIO] = { "radio", AGG_LAST },
  [SKIM_SG_CPU_CW] = { "cpu_cw", AGG_MEAN },    [SKIM_SG_CPU_RTTY] = { "cpu_rtty", AGG_MEAN },
  [SKIM_SG_CPU_TEMP] = { "cpu_temp", AGG_MAX },  [SKIM_SG_CPU_HELPERS] = { "cpu_helpers", AGG_MEAN },
  [SKIM_SG_RP_TEMP] = { "rp_temp", AGG_MAX },    [SKIM_SG_RP_LOAD] = { "rp_load", AGG_MEAN },
  [SKIM_SG_RP_MBPS_A] = { "rp_mbps_a", AGG_MEAN }, [SKIM_SG_RP_MBPS_B] = { "rp_mbps_b", AGG_MEAN },
};

typedef struct {
  char    magic[8];
  guint32 version, minutes, bands, modes, sm_n, sg_n;
  char    names[OLD_BANDS][16];
} Header;                                      /* versions 1 and 2           */

typedef struct {
  Header  h;                                   /* versions 3–7, minutes = 0  */
  guint32 ntier, tier_res[NTIER], tier_len[NTIER], pad;
} Header3;

/* Version 8: the same, with 16 band names. */
typedef struct {
  char    magic[8];
  guint32 version, minutes, bands, modes, sm_n, sg_n;
  char    names[SKIM_STATS_BANDS][16];
  guint32 ntier, tier_res[NTIER], tier_len[NTIER], pad;
} Header8;

/* A slot of any ring: the minute its span starts at (0 = empty), the values
 * (as a minute record) and how many minutes each value merges. */
typedef struct {
  gint64          start;
  SkimStatsMinute m;
  guint8          n[NV];
} Slot;

/* A minute record of versions 2 and 3 (7 global metrics). */
typedef struct {
  float band[OLD_BANDS][OLD_MODES][V4_SM_N];
  float glob[V3_SG_N];
} MinuteV3;
#define NV3 (OLD_BANDS * OLD_MODES * V4_SM_N + V3_SG_N)

/* Version 4: a ring slot (8 global metrics, no thermal pauses). */
typedef struct {
  float band[OLD_BANDS][OLD_MODES][V4_SM_N];
  float glob[V4_SG_N];
} MinuteV4;
#define NV4 (OLD_BANDS * OLD_MODES * V4_SM_N + V4_SG_N)

/* Version 5: CW and RTTY with this version's band metrics, 8 global ones. */
typedef struct {
  float band[OLD_BANDS][OLD_MODES][SKIM_SM_N];
  float glob[V5_SG_N];
} MinuteV5;
#define NV5 (OLD_BANDS * OLD_MODES * SKIM_SM_N + V5_SG_N)
typedef struct {
  gint64   start;
  MinuteV5 m;
  guint8   n[NV5];
} SlotV5;
typedef struct {
  gint64   start;
  MinuteV4 m;
  guint8   n[NV4];
} SlotV4;

/* Version 6: CW, RTTY, FT8 with this version's metrics. */
typedef struct {
  float band[OLD_BANDS][V6_MODES][SKIM_SM_N];
  float glob[V7_SG_N];
} MinuteV6;
#define NV6 (OLD_BANDS * V6_MODES * SKIM_SM_N + V7_SG_N)
typedef struct {
  gint64   start;
  MinuteV6 m;
  guint8   n[NV6];
} SlotV6;

/* Version 7: 8 bands, CW, RTTY, FT8, JS8, 9 global metrics. */
typedef struct {
  float band[OLD_BANDS][SKIM_STATS_MODES][SKIM_SM_N];
  float glob[V7_SG_N];
} MinuteV7;
#define NV7 (OLD_BANDS * SKIM_STATS_MODES * SKIM_SM_N + V7_SG_N)
typedef struct {
  gint64   start;
  MinuteV7 m;
  guint8   n[NV7];
} SlotV7;

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
  float  band[OLD_BANDS][OLD_MODES][V4_SM_N];
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

/* A ringed file of version 3–7 (slot: its size) — or this version's. */
static gsize file_len_of(gsize slot) {
  gsize n = sizeof(Header3);
  for (guint t = 0; t < NTIER; t++) { n += (gsize)TIER[t].len * slot; }
  return n;
}
static gsize file_len(void) {
  gsize n = sizeof(Header8);
  for (guint t = 0; t < NTIER; t++) { n += (gsize)TIER[t].len * sizeof(Slot); }
  return n;
}

static gsize ring_offset_of(guint t, gsize slot) {
  gsize n = sizeof(Header3);
  for (guint u = 0; u < t; u++) { n += (gsize)TIER[u].len * slot; }
  return n;
}
static gsize ring_offset(guint t) {
  gsize n = sizeof(Header8);
  for (guint u = 0; u < t; u++) { n += (gsize)TIER[u].len * sizeof(Slot); }
  return n;
}

/* A version 1–7 record (8 bands; and its merge counts, NULL: none) as this
 * version's: its `modes` modes' sm_n band metrics and sg_n globals in
 * place, the newer bands, modes and metrics empty. */
static void from_old(SkimStatsMinute *o, guint8 *on, const float *band, guint sm_n,
                     const float *glob, guint sg_n, const guint8 *in_n, guint modes) {
  empty_minute(o);
  if (on) { memset(on, 0, NV); }
  for (guint b = 0; b < OLD_BANDS; b++) {
    for (guint k = 0; k < modes; k++) {
      const guint c = b * modes + k, d = b * SKIM_STATS_MODES + k;
      memcpy(o->band[b][k], band + c * sm_n, sm_n * sizeof(float));
      if (on) { memcpy(on + d * SKIM_SM_N, in_n + c * sm_n, sm_n); }
    }
  }
  memcpy(o->glob, glob, sg_n * sizeof(float));
  if (on) {
    memcpy(on + SKIM_STATS_BANDS * SKIM_STATS_MODES * SKIM_SM_N,
           in_n + OLD_BANDS * modes * sm_n, sg_n);
  }
}

/* The file's band column (of its nfile) for each of ours (by name), -1 = none. */
static void columns(const char names[][16], guint nfile, const SkimStats *s, int *from) {
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    from[b] = -1;
    for (guint f = 0; b < s->nb && f < nfile; f++) {
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
  columns(h->names, OLD_BANDS, s, from);
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
      from_old(&src, NULL, &in1[i].band[0][0][0], V4_SM_N, in1[i].glob, V1_SG_N, NULL, OLD_MODES);
    } else {
      minute = in[i].minute;
      from_old(&src, NULL, &in[i].m.band[0][0][0], V4_SM_N, in[i].m.glob, V3_SG_N, NULL, OLD_MODES);
    }
    remap(&mm, &src, from, s->nb, NULL, NULL);
    for (guint t = 0; t < NTIER; t++) { merge(s, t, minute, &mm); }
  }
  g_array_unref(order);
  g_message("stats: %s is a version %u history — carried over, its week merged into the "
            "15-minute and hour rings", s->path, h->version);
}

/* Keep the file as it was beside it (path.vN), for an older build that
 * reads that version only (and would start afresh on this one). */
static void keep_old(const SkimStats *s, const char *data, gsize len, guint version, char **kept) {
  *kept = g_strdup_printf("%s.v%u", s->path, version);
  if (!g_file_test(*kept, G_FILE_TEST_EXISTS)) { g_file_set_contents(*kept, data, (gssize)len, NULL); }
}

/* TRUE when the file can be written in place as it is (this version, these
 * bands). */
static gboolean load(SkimStats *s) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(s->path, &data, &len, NULL)) { return FALSE; }
  const Header *h = (const Header *)data;
  const gboolean head_ok = len >= sizeof(Header) && memcmp(h->magic, MAGIC, 8) == 0 &&
                           h->bands == (h->version >= 8 ? SKIM_STATS_BANDS : OLD_BANDS) &&
                           h->modes == (h->version >= 7 ? SKIM_STATS_MODES
                                        : h->version == 6 ? V6_MODES : OLD_MODES) &&
                           h->sm_n == (h->version >= 5 ? SKIM_SM_N : V4_SM_N);
  const gboolean v1 = head_ok && h->version == 1 && h->sg_n == V1_SG_N &&
                      h->minutes == SKIM_STATS_MINUTES &&
                      len == sizeof(Header) + SKIM_STATS_MINUTES * sizeof(SlotV1);
  const gboolean v2 = head_ok && h->version == 2 && h->sg_n == V3_SG_N &&
                      h->minutes == SKIM_STATS_MINUTES &&
                      len == sizeof(Header) + SKIM_STATS_MINUTES * sizeof(SlotV2);
  const Header3 *h3 = (const Header3 *)data;
  const Header8 *h8 = (const Header8 *)data;
  gboolean rings_ok = len >= sizeof(Header3) && h3->ntier == NTIER;
  for (guint t = 0; rings_ok && t < NTIER; t++) {
    rings_ok = h3->tier_res[t] == TIER[t].res && h3->tier_len[t] == TIER[t].len;
  }
  gboolean rings8_ok = len >= sizeof(Header8) && h8->ntier == NTIER;
  for (guint t = 0; rings8_ok && t < NTIER; t++) {
    rings8_ok = h8->tier_res[t] == TIER[t].res && h8->tier_len[t] == TIER[t].len;
  }
  const gboolean v3 = head_ok && rings_ok && h->version == 3 && h->sg_n == V3_SG_N &&
                      len == file_len_of(sizeof(SlotV3));
  const gboolean v4 = head_ok && rings_ok && h->version == 4 && h->sg_n == V4_SG_N &&
                      len == file_len_of(sizeof(SlotV4));
  const gboolean v5 = head_ok && rings_ok && h->version == 5 && h->sg_n == V5_SG_N &&
                      len == file_len_of(sizeof(SlotV5));
  const gboolean v6 = head_ok && rings_ok && h->version == 6 && h->sg_n == V7_SG_N &&
                      len == file_len_of(sizeof(SlotV6));
  const gboolean v7 = head_ok && rings_ok && h->version == 7 && h->sg_n == V7_SG_N &&
                      len == file_len_of(sizeof(SlotV7));
  const gboolean v8 = head_ok && rings8_ok && h->version == VERSION && h->sg_n == SKIM_SG_N &&
                      len == file_len();
  gboolean same = FALSE;
  if (v1 || v2) {
    load_old(s, data, h, v1);
    s->dirty = TRUE;                           /* written as this version   */
  } else if (v3 || v4 || v5 || v6 || v7) {           /* slot by slot, the new empty */
    int from[SKIM_STATS_BANDS];
    columns(h->names, OLD_BANDS, s, from);
    for (guint t = 0; t < NTIER; t++) {
      const gsize at = ring_offset_of(t, v3 ? sizeof(SlotV3) : v4 ? sizeof(SlotV4)
                                          : v5 ? sizeof(SlotV5) : v6 ? sizeof(SlotV6)
                                                                     : sizeof(SlotV7));
      const SlotV3 *in3 = (const SlotV3 *)(data + at);
      const SlotV4 *in4 = (const SlotV4 *)(data + at);
      const SlotV5 *in5 = (const SlotV5 *)(data + at);
      const SlotV6 *in6 = (const SlotV6 *)(data + at);
      const SlotV7 *in7 = (const SlotV7 *)(data + at);
      for (guint i = 0; i < TIER[t].len; i++) {
        SkimStatsMinute m;
        guint8 n[NV];
        if (v3) {
          from_old(&m, n, &in3[i].m.band[0][0][0], V4_SM_N, in3[i].m.glob, V3_SG_N, in3[i].n, OLD_MODES);
        } else if (v4) {
          from_old(&m, n, &in4[i].m.band[0][0][0], V4_SM_N, in4[i].m.glob, V4_SG_N, in4[i].n, OLD_MODES);
        } else if (v5) {
          from_old(&m, n, &in5[i].m.band[0][0][0], SKIM_SM_N, in5[i].m.glob, V5_SG_N, in5[i].n, OLD_MODES);
        } else if (v6) {
          from_old(&m, n, &in6[i].m.band[0][0][0], SKIM_SM_N, in6[i].m.glob, V7_SG_N, in6[i].n, V6_MODES);
        } else {
          from_old(&m, n, &in7[i].m.band[0][0][0], SKIM_SM_N, in7[i].m.glob, V7_SG_N, in7[i].n,
                   SKIM_STATS_MODES);
        }
        Slot *o = &s->ring[t][i];
        o->start = v3 ? in3[i].start : v4 ? in4[i].start : v5 ? in5[i].start
                 : v6 ? in6[i].start : in7[i].start;
        remap(&o->m, &m, from, s->nb, o->n, n);
      }
    }
    s->dirty = TRUE;                           /* written as this version   */
    char *kept;
    keep_old(s, data, len, h->version, &kept);
    g_message("stats: %s is a version %u history — carried over, %s now (the old "
              "file kept as %s, for an older build)", s->path, h->version,
              v3 ? "the CPU temperature, the thermal pauses, FT8 and JS8 start"
              : v4 ? "the thermal pauses, FT8 and JS8 start"
              : v5 ? "FT8, JS8 and the helpers' CPU start"
              : v6 ? "JS8 starts" : "16 bands and the Red Pitaya's series start",
              kept);
    g_free(kept);
  } else if (v8) {
    int from[SKIM_STATS_BANDS];
    columns(h8->names, SKIM_STATS_BANDS, s, from);
    for (guint t = 0; t < NTIER; t++) {
      const Slot *in = (const Slot *)(data + ring_offset(t));
      for (guint i = 0; i < TIER[t].len; i++) {
        Slot *o = &s->ring[t][i];
        o->start = in[i].start;
        remap(&o->m, &in[i].m, from, s->nb, o->n, in[i].n);
      }
    }
    same = memcmp(h8->names, s->names, sizeof(s->names)) == 0;
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

float skim_stats_peak_sum(const SkimStats *s, gint64 now_minute, guint minutes, guint mode,
                          SkimStatsBandMetric metric) {
  float peak = NAN;
  minutes = MIN(minutes, (guint)SKIM_STATS_MINUTES);
  for (guint k = 0; k < minutes; k++) {
    const gint64 minute = now_minute - k;
    const Slot *sl = &s->ring[0][(guint)(minute % SKIM_STATS_MINUTES)];
    if (sl->start != minute) { continue; }
    float sum = 0;
    gboolean any = FALSE;
    for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
      const float v = sl->m.band[b][mode][metric];
      if (!isnan(v)) { sum += v; any = TRUE; }
    }
    if (any && (isnan(peak) || sum > peak)) { peak = sum; }
  }
  return peak;
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
  Header8 *h = (Header8 *)buf;
  memcpy(h->magic, MAGIC, 8);
  h->version = VERSION;
  h->bands = SKIM_STATS_BANDS;
  h->modes = SKIM_STATS_MODES;
  h->sm_n = SKIM_SM_N;
  h->sg_n = SKIM_SG_N;
  memcpy(h->names, s->names, sizeof(h->names));
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
  static const char *MODE[SKIM_STATS_MODES] = { "cw", "rtty", "ft8", "js8" };
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
