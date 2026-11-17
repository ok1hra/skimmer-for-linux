/* stats.c — see stats.h. Part of skimmer-for-linux. GPL-3.0-or-later.
 *
 * File: a header, then SKIM_STATS_MINUTES slots, the slot of a minute at
 * (unix minute mod SKIM_STATS_MINUTES). Native byte order — the file never
 * leaves the machine that wrote it.
 *
 * Saving writes only the slots put since the last save, in place (pwrite
 * on the file kept open): a few kB every few minutes, not the whole 4 MB.
 * The whole file is written — atomically, to a temp file renamed over it —
 * only when there is none yet, when it is of an older version, or when the
 * config's bands differ from the file's columns (they are matched by name
 * and re-ordered). A slot torn by a power cut costs that one minute.
 *
 * /stats.json:
 *   { "now": <unix s>, "t0": <unix s of the first bucket>, "step_min": S, "n": N,
 *     "bands": ["160m", …],
 *     "cw":   { "spots": { "160m": [v|null × N], … }, "stations": …, "active": …,
 *               "folded": …, "gate_below": …, "snr": … },
 *     "rtty": { … } | null              (null while no band decoded RTTY)
 *     "global": { "cpu": [ … ], "lost": …, "clients": …, "streaming": …, "radio": …,
 *                 "cpu_cw": …, "cpu_rtty": … } }
 *   A bucket sums the counts (spots, stations, folded, gate_below), averages
 *   the levels (active, cpu, clients, snr, streaming = the share of minutes
 *   streamed), takes the worst lost and the last radio. null = no record.
 */
#include "stats.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "json.h"

#define MAGIC "SKIMSTAT"
#define VERSION 2                    /* 2: + cpu_cw, cpu_rtty               */
#define V1_SG_N 5                    /* version 1's global metrics           */

typedef struct {
  char    magic[8];
  guint32 version, minutes, bands, modes, sm_n, sg_n;
  char    names[SKIM_STATS_BANDS][16];
} Header;

typedef struct {
  gint64          minute;                /* unix minute; 0 = empty           */
  SkimStatsMinute m;
} Slot;

/* A version 1 slot: the same, with the first V1_SG_N global metrics. */
typedef struct {
  gint64 minute;
  float  band[SKIM_STATS_BANDS][SKIM_STATS_MODES][SKIM_SM_N];
  float  glob[V1_SG_N];
} SlotV1;

struct _SkimStats {
  char   *path;
  char    names[SKIM_STATS_BANDS][16];
  guint   nb;
  Slot   *slot;                          /* SKIM_STATS_MINUTES              */
  gboolean dirty;                        /* something to write              */
  gboolean full;                         /* … the whole file (see the top)  */
  guint8  *dirty_slot;                   /* bitmap: slots put since a save  */
  int      fd;                           /* the file, open for pwrite; -1   */
};

static void empty_minute(SkimStatsMinute *m) {
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    for (guint k = 0; k < SKIM_STATS_MODES; k++) {
      for (guint j = 0; j < SKIM_SM_N; j++) { m->band[b][k][j] = NAN; }
    }
  }
  for (guint j = 0; j < SKIM_SG_N; j++) { m->glob[j] = NAN; }
}

/* The file's columns, re-ordered to this config's bands by name. TRUE when
 * the file can be written in place as it is (this version, these bands). */
static gboolean load(SkimStats *s) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(s->path, &data, &len, NULL)) { return FALSE; }
  const Header *h = (const Header *)data;
  const gboolean v1 = len >= sizeof(Header) && h->version == 1;
  const gsize slot = v1 ? sizeof(SlotV1) : sizeof(Slot);
  if (len < sizeof(Header) || len != sizeof(Header) + SKIM_STATS_MINUTES * slot ||
      memcmp(h->magic, MAGIC, 8) != 0 || (h->version != VERSION && !v1) ||
      h->minutes != SKIM_STATS_MINUTES || h->bands != SKIM_STATS_BANDS ||
      h->modes != SKIM_STATS_MODES || h->sm_n != SKIM_SM_N ||
      h->sg_n != (v1 ? V1_SG_N : SKIM_SG_N)) {
    g_warning("stats: %s is not this version's history — starting a fresh one", s->path);
    g_free(data);
    return FALSE;
  }
  const gboolean same = !v1 && memcmp(h->names, s->names, sizeof(s->names)) == 0;
  int from[SKIM_STATS_BANDS];
  for (guint b = 0; b < SKIM_STATS_BANDS; b++) {
    from[b] = -1;
    for (guint f = 0; b < s->nb && f < SKIM_STATS_BANDS; f++) {
      if (h->names[f][0] && strncmp(h->names[f], s->names[b], 16) == 0) { from[b] = (int)f; }
    }
  }
  const Slot *in = (const Slot *)(data + sizeof(Header));
  const SlotV1 *in1 = (const SlotV1 *)(data + sizeof(Header));
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
    Slot *o = &s->slot[i];
    o->minute = v1 ? in1[i].minute : in[i].minute;
    empty_minute(&o->m);
    if (v1) {                                  /* the CPU split stays NaN    */
      memcpy(o->m.glob, in1[i].glob, sizeof(in1[i].glob));
    } else {
      memcpy(o->m.glob, in[i].m.glob, sizeof(o->m.glob));
    }
    for (guint b = 0; b < s->nb; b++) {
      if (from[b] >= 0) {
        memcpy(o->m.band[b], v1 ? in1[i].band[from[b]] : in[i].m.band[from[b]],
               sizeof(o->m.band[b]));
      }
    }
  }
  if (v1) {
    g_message("stats: %s is a version 1 history — carried over (no CPU split before now)",
              s->path);
    s->dirty = TRUE;                           /* written as version 2      */
  }
  if (!same) { s->dirty = TRUE; }              /* re-ordered: written whole */
  g_free(data);
  return same;
}

SkimStats *skim_stats_open(const char *path, const char *const *bands, guint nb) {
  SkimStats *s = g_new0(SkimStats, 1);
  s->path = g_strdup(path);
  s->nb = MIN(nb, (guint)SKIM_STATS_BANDS);
  for (guint b = 0; b < s->nb; b++) { g_strlcpy(s->names[b], bands[b], 16); }
  s->slot = g_new0(Slot, SKIM_STATS_MINUTES);
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) { empty_minute(&s->slot[i].m); }
  s->dirty_slot = g_new0(guint8, SKIM_STATS_MINUTES / 8 + 1);
  s->fd = -1;
  s->full = !load(s);
  if (!s->full) {
    s->fd = g_open(s->path, O_RDWR | O_CLOEXEC, 0);
    if (s->fd < 0) { s->full = TRUE; }
  }
  return s;
}

void skim_stats_put(SkimStats *s, gint64 unix_minute, const SkimStatsMinute *m) {
  const guint i = (guint)(unix_minute % SKIM_STATS_MINUTES);
  Slot *o = &s->slot[i];
  o->minute = unix_minute;
  o->m = *m;
  s->dirty_slot[i / 8] |= (guint8)(1u << (i % 8));
  s->dirty = TRUE;
}

/* The slots put since the last save, each at its place in the open file. */
static gboolean save_slots(SkimStats *s, GError **error) {
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
    if (!(s->dirty_slot[i / 8] & (1u << (i % 8)))) { continue; }
    const off_t at = (off_t)(sizeof(Header) + (gsize)i * sizeof(Slot));
    if (pwrite(s->fd, &s->slot[i], sizeof(Slot), at) != (gssize)sizeof(Slot)) {
      g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s: %s", s->path,
                  g_strerror(errno));
      return FALSE;
    }
  }
  return TRUE;
}

gboolean skim_stats_save(SkimStats *s, GError **error) {
  if (!s->dirty) { return TRUE; }
  if (!s->full && s->fd >= 0) {
    if (save_slots(s, error)) {
      memset(s->dirty_slot, 0, SKIM_STATS_MINUTES / 8 + 1);
      s->dirty = FALSE;
      return TRUE;
    }
    s->full = TRUE;                            /* next time: the whole file */
    return FALSE;
  }
  const gsize len = sizeof(Header) + SKIM_STATS_MINUTES * sizeof(Slot);
  char *buf = g_malloc0(len);
  Header *h = (Header *)buf;
  memcpy(h->magic, MAGIC, 8);
  h->version = VERSION;
  h->minutes = SKIM_STATS_MINUTES;
  h->bands = SKIM_STATS_BANDS;
  h->modes = SKIM_STATS_MODES;
  h->sm_n = SKIM_SM_N;
  h->sg_n = SKIM_SG_N;
  memcpy(h->names, s->names, sizeof(h->names));
  memcpy(buf + sizeof(Header), s->slot, SKIM_STATS_MINUTES * sizeof(Slot));
  char *dir = g_path_get_dirname(s->path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  const gboolean ok = g_file_set_contents(s->path, buf, (gssize)len, error);
  g_free(buf);
  if (ok) {                                    /* a new file: open it anew  */
    if (s->fd >= 0) { close(s->fd); }
    s->fd = g_open(s->path, O_RDWR | O_CLOEXEC, 0);
    s->full = s->fd < 0;
    memset(s->dirty_slot, 0, SKIM_STATS_MINUTES / 8 + 1);
    s->dirty = FALSE;
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
  g_free(s->dirty_slot);
  g_free(s->slot);
  g_free(s->path);
  g_free(s);
}

/* ---- JSON ------------------------------------------------------------------------ */

typedef enum { AGG_SUM, AGG_MEAN, AGG_MAX, AGG_LAST } Agg;

typedef struct {
  double acc;
  guint  n;
} Bucket;

static void bucket_add(Bucket *k, Agg agg, float v) {
  if (isnan(v)) { return; }
  if (agg == AGG_MAX) { k->acc = k->n ? MAX(k->acc, v) : v; }
  else if (agg == AGG_LAST) { k->acc = v; }
  else { k->acc += v; }
  k->n++;
}

static void bucket_out(GString *o, const Bucket *k, Agg agg, guint i) {
  if (i) { g_string_append_c(o, ','); }
  if (!k->n) { g_string_append(o, "null"); return; }
  const double v = agg == AGG_MEAN ? k->acc / k->n : k->acc;
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(num, sizeof(num), "%.4g", v);
  g_string_append(o, num);
}

/* One series: band b / mode k / metric j (b < 0: global metric j). */
static void series(GString *o, SkimStats *s, gint64 first, guint step, guint n, int b,
                   guint k, guint j, Agg agg) {
  Bucket *bk = g_new0(Bucket, n);
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
    const Slot *sl = &s->slot[i];
    if (!sl->minute || sl->minute < first || sl->minute >= first + (gint64)step * n) {
      continue;
    }
    const guint at = (guint)((sl->minute - first) / step);
    bucket_add(&bk[at], agg, b < 0 ? sl->m.glob[j] : sl->m.band[b][k][j]);
  }
  g_string_append_c(o, '[');
  for (guint i = 0; i < n; i++) { bucket_out(o, &bk[i], agg, i); }
  g_string_append_c(o, ']');
  g_free(bk);
}

static gboolean mode_used(SkimStats *s, guint k) {
  for (guint i = 0; i < SKIM_STATS_MINUTES; i++) {
    for (guint b = 0; s->slot[i].minute && b < s->nb; b++) {
      if (!isnan(s->slot[i].m.band[b][k][SKIM_SM_ACTIVE])) { return TRUE; }
    }
  }
  return FALSE;
}

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
};

char *skim_stats_json(SkimStats *s, gint64 now_minute, guint hours, guint step) {
  hours = CLAMP(hours, 1u, 168u);
  const guint span = hours * 60;
  if (!step) { step = MAX(1u, (span + 719) / 720); }
  step = CLAMP(step, 1u, span);
  const guint n = (span + step - 1) / step;
  /* buckets end with the current minute, aligned to the step */
  const gint64 last = now_minute - now_minute % step;
  const gint64 first = last - (gint64)step * (n - 1);
  GString *o = g_string_new(NULL);
  g_string_append_printf(o, "{\"now\":%" G_GINT64_FORMAT ",\"t0\":%" G_GINT64_FORMAT
                         ",\"step_min\":%u,\"n\":%u,\"bands\":[",
                         now_minute * 60, first * 60, step, n);
  for (guint b = 0; b < s->nb; b++) {
    g_string_append_printf(o, "%s\"", b ? "," : "");
    skim_json_append(o, s->names[b]);
    g_string_append_c(o, '"');
  }
  g_string_append_c(o, ']');
  static const char *MODE[SKIM_STATS_MODES] = { "cw", "rtty" };
  for (guint k = 0; k < SKIM_STATS_MODES; k++) {
    g_string_append_printf(o, ",\"%s\":", MODE[k]);
    if (k && !mode_used(s, k)) { g_string_append(o, "null"); continue; }
    g_string_append_c(o, '{');
    for (guint j = 0; j < SKIM_SM_N; j++) {
      g_string_append_printf(o, "%s\"%s\":{", j ? "," : "", BAND_M[j].name);
      for (guint b = 0; b < s->nb; b++) {
        g_string_append_printf(o, "%s\"", b ? "," : "");
        skim_json_append(o, s->names[b]);
        g_string_append(o, "\":");
        series(o, s, first, step, n, (int)b, k, j, BAND_M[j].agg);
      }
      g_string_append_c(o, '}');
    }
    g_string_append_c(o, '}');
  }
  g_string_append(o, ",\"global\":{");
  for (guint j = 0; j < SKIM_SG_N; j++) {
    g_string_append_printf(o, "%s\"%s\":", j ? "," : "", GLOB_M[j].name);
    series(o, s, first, step, n, -1, 0, j, GLOB_M[j].agg);
  }
  g_string_append(o, "}}");
  return g_string_free(o, FALSE);
}
