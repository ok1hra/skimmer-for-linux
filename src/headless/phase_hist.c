/* phase_hist.c — see phase_hist.h.
 *
 * File: a 64-byte header (SKPHASE1, version, channels, the first channel's
 * offset and the spacing, the two rings' lengths), then the minute ring
 * and the quarter-hour ring, each row: its minute (unix minutes; -1 =
 * never written), flags (bit 0: estimates, bit 1: B was missing), the
 * seconds summed, the summary (8 floats), gain[nchan], drop[nchan].
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "phase_hist.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define MAGIC    "SKPHASE1"
#define VERSION  1
#define N1       (7 * 24 * 60)          /* a minute a row, 7 days          */
#define N2       (90 * 24 * 4)          /* a quarter hour a row, 90 days   */
#define NSUM     8
#define HDR      64

static const char *const SUM_KEY[NSUM] = { "gain_db", "gain_a_db", "drop_db", "drop_p90_db",
                                            "coherence", "dmax_db", "share_b", "stations" };

typedef struct {
  gint64  minute;
  guint8  flags;
  guint8  pad[3];
  guint32 nsec;
  float   sum[NSUM];
} RowHead;

/* A row being summed (a minute's seconds, or a quarter's minutes). */
typedef struct {
  gint64   key;                  /* its minute (-1 = none)               */
  double  *sg, *sd;              /* per channel: sums of dB              */
  guint16 *ng, *nd;
  double   ss[NSUM];
  guint    ns[NSUM];
  guint    n, est, missing;
} Acc;

struct _SkimPhaseHist {
  char   *path;
  int     fd;
  guint   nchan;
  double  first_hz, spacing_hz;
  gsize   row;                   /* bytes a row                          */
  guint8 *mem;                   /* the whole file                       */
  Acc     a1, a2;
};

static gsize ring_off(const SkimPhaseHist *h, guint tier, guint slot) {
  return HDR + (tier == 1 ? 0 : (gsize)N1 * h->row) + (gsize)slot * h->row;
}

static RowHead *head_at(SkimPhaseHist *h, guint tier, guint slot) {
  return (RowHead *)(h->mem + ring_off(h, tier, slot));
}

static void acc_init(Acc *a, guint nchan) {
  memset(a, 0, sizeof(*a));
  a->key = -1;
  a->sg = g_new0(double, nchan);
  a->sd = g_new0(double, nchan);
  a->ng = g_new0(guint16, nchan);
  a->nd = g_new0(guint16, nchan);
}

static void acc_clear(Acc *a, guint nchan) {
  memset(a->sg, 0, nchan * sizeof(double));
  memset(a->sd, 0, nchan * sizeof(double));
  memset(a->ng, 0, nchan * sizeof(guint16));
  memset(a->nd, 0, nchan * sizeof(guint16));
  memset(a->ss, 0, sizeof(a->ss));
  memset(a->ns, 0, sizeof(a->ns));
  a->n = a->est = a->missing = 0;
  a->key = -1;
}

static void acc_free(Acc *a) {
  g_free(a->sg);
  g_free(a->sd);
  g_free(a->ng);
  g_free(a->nd);
}

/* The row of acc into ring tier at its slot (memory and file). */
static void acc_write(SkimPhaseHist *h, Acc *a, guint tier) {
  if (a->key < 0 || a->n == 0)
    return;
  const guint slot = tier == 1 ? (guint)(a->key % N1) : (guint)((a->key / 15) % N2);
  RowHead *r = head_at(h, tier, slot);
  memset(r, 0, sizeof(*r));
  r->minute = tier == 1 ? a->key : a->key / 15 * 15;
  r->flags = (a->est * 2 > a->n ? 1 : 0) | (a->missing * 2 > a->n ? 2 : 0);
  r->nsec = a->n;
  for (guint k = 0; k < NSUM; k++) { r->sum[k] = a->ns[k] ? (float)(a->ss[k] / a->ns[k]) : NAN; }
  gint8 *g = (gint8 *)(r + 1), *d = g + h->nchan;
  for (guint c = 0; c < h->nchan; c++) {
    g[c] = a->ng[c] ? skim_phase_q8(a->sg[c] / a->ng[c]) : SKIM_PHASE_NONE;
    d[c] = a->nd[c] ? skim_phase_q8(a->sd[c] / a->nd[c]) : SKIM_PHASE_NONE;
  }
  if (h->fd >= 0 && pwrite(h->fd, r, h->row, (off_t)ring_off(h, tier, slot)) != (ssize_t)h->row) {
    g_warning("phase history: %s: %s", h->path, g_strerror(errno));
  }
}

/* A finished minute row into the quarter's sums. */
static void acc_fold(SkimPhaseHist *h, const Acc *m, Acc *q) {
  const RowHead *r = head_at(h, 1, (guint)(m->key % N1));
  const gint8 *g = (const gint8 *)(r + 1), *d = g + h->nchan;
  for (guint c = 0; c < h->nchan; c++) {
    if (g[c] != SKIM_PHASE_NONE) { q->sg[c] += g[c] / 4.0; q->ng[c]++; }
    if (d[c] != SKIM_PHASE_NONE) { q->sd[c] += d[c] / 4.0; q->nd[c]++; }
  }
  for (guint k = 0; k < NSUM; k++) {
    if (!isnan(r->sum[k])) { q->ss[k] += r->sum[k]; q->ns[k]++; }
  }
  q->n++;
  if (r->flags & 1) { q->est++; }
  if (r->flags & 2) { q->missing++; }
}

SkimPhaseHist *skim_phase_hist_open(const char *dir, const char *band, guint nchan,
                                    double first_hz, double spacing_hz) {
  if (!dir || !band || nchan == 0 || nchan > 16384)
    return NULL;
  g_mkdir_with_parents(dir, 0700);
  SkimPhaseHist *h = g_new0(SkimPhaseHist, 1);
  h->path = g_strdup_printf("%s/phase-%s.bin", dir, band);
  h->nchan = nchan;
  h->first_hz = first_hz;
  h->spacing_hz = spacing_hz;
  h->row = sizeof(RowHead) + 2 * (gsize)nchan;
  const gsize size = HDR + ((gsize)N1 + N2) * h->row;
  h->mem = g_malloc(size);
  gboolean ok = FALSE;
  char *old = NULL;
  gsize len = 0;
  if (g_file_get_contents(h->path, &old, &len, NULL) && len == size && memcmp(old, MAGIC, 8) == 0) {
    guint32 v, n;
    double f, s;
    memcpy(&v, old + 8, 4);
    memcpy(&n, old + 12, 4);
    memcpy(&f, old + 16, 8);
    memcpy(&s, old + 24, 8);
    ok = v == VERSION && n == nchan && fabs(f - first_hz) < 0.5 && fabs(s - spacing_hz) < 0.01;
  }
  if (ok) {
    memcpy(h->mem, old, size);
  } else {
    if (old) {
      char *keep = g_strdup_printf("%s.old", h->path);
      g_rename(h->path, keep);
      g_message("phase history: %s was of another layout — kept as %s, a new one begins",
                h->path, keep);
      g_free(keep);
    }
    memset(h->mem, 0, size);
    memcpy(h->mem, MAGIC, 8);
    const guint32 v = VERSION, n = nchan, n1 = N1, n2 = N2;
    memcpy(h->mem + 8, &v, 4);
    memcpy(h->mem + 12, &n, 4);
    memcpy(h->mem + 16, &first_hz, 8);
    memcpy(h->mem + 24, &spacing_hz, 8);
    memcpy(h->mem + 32, &n1, 4);
    memcpy(h->mem + 36, &n2, 4);
    for (guint i = 0; i < N1; i++) { head_at(h, 1, i)->minute = -1; }
    for (guint i = 0; i < N2; i++) { head_at(h, 2, i)->minute = -1; }
  }
  g_free(old);
  if (!ok) {
    GError *e = NULL;
    if (!g_file_set_contents(h->path, (const char *)h->mem, (gssize)size, &e)) {
      g_warning("phase history: %s: %s — not kept", h->path, e->message);
      g_clear_error(&e);
    }
  }
  h->fd = open(h->path, O_WRONLY | O_CLOEXEC);
  acc_init(&h->a1, nchan);
  acc_init(&h->a2, nchan);
  return h;
}

void skim_phase_hist_free(SkimPhaseHist *h) {
  if (!h)
    return;
  acc_write(h, &h->a1, 1);
  if (h->a1.key >= 0 && h->a1.n) { acc_fold(h, &h->a1, &h->a2); }
  acc_write(h, &h->a2, 2);
  if (h->fd >= 0) { close(h->fd); }
  acc_free(&h->a1);
  acc_free(&h->a2);
  g_free(h->mem);
  g_free(h->path);
  g_free(h);
}

void skim_phase_hist_add(SkimPhaseHist *h, gint64 now_s, const SkimPipelinePhase *ph,
                         const gint8 *gain, const gint8 *drop) {
  if (!h || !ph || !ph->phased)
    return;
  const gint64 minute = now_s / 60;
  if (h->a1.key != minute) {
    if (h->a1.key >= 0 && h->a1.n) {
      acc_write(h, &h->a1, 1);
      if (h->a2.key >= 0 && h->a2.key / 15 != h->a1.key / 15) {
        acc_write(h, &h->a2, 2);
        acc_clear(&h->a2, h->nchan);
      }
      if (h->a2.key < 0) { h->a2.key = h->a1.key / 15 * 15; }
      acc_fold(h, &h->a1, &h->a2);
    }
    acc_clear(&h->a1, h->nchan);
    h->a1.key = minute;
  }
  Acc *a = &h->a1;
  const guint n = MIN(ph->nchan, h->nchan);
  for (guint c = 0; c < n; c++) {
    if (gain && gain[c] != SKIM_PHASE_NONE) { a->sg[c] += gain[c] / 4.0; a->ng[c]++; }
    if (drop && drop[c] != SKIM_PHASE_NONE) { a->sd[c] += drop[c] / 4.0; a->nd[c]++; }
  }
  const SkimPhaseSummary *u = &ph->sum;
  const double v[NSUM] = { u->gain_db, u->gain_a_db, u->drop_db, u->drop_p90_db, u->coherence,
                           u->dmax_db, u->share_b, (double)u->stations };
  for (guint k = 0; k < NSUM; k++) {
    if (!isnan(v[k])) { a->ss[k] += v[k]; a->ns[k]++; }
  }
  a->n++;
  if (ph->est) { a->est++; }
  if (ph->b_missing) { a->missing++; }
}

char *skim_phase_hist_json(SkimPhaseHist *h, gint64 end_s, guint hours, guint max_rows) {
  if (!h)
    return g_strdup("{\"rows\":0}");
  if (hours == 0) { hours = 24; }
  if (max_rows == 0) { max_rows = 720; }
  const guint tier = hours <= 168 ? 1 : 2, step = tier == 1 ? 1 : 15;
  const gint64 end_min = end_s / 60 / step * step;
  guint nrows = hours * 60 / step;
  const guint cap = tier == 1 ? N1 : N2;
  nrows = MIN(nrows, cap);
  const guint f = (nrows + max_rows - 1) / max_rows;
  const guint out = (nrows + f - 1) / f;
  const gint64 start_min = end_min - (gint64)(out * f - 1) * step;
  const guint nc = h->nchan;
  gint8 *og = g_new(gint8, (gsize)out * nc), *od = g_new(gint8, (gsize)out * nc);
  double *sg = g_new(double, nc), *sd = g_new(double, nc);
  guint *ng = g_new(guint, nc), *nd = g_new(guint, nc);
  GString *sum[NSUM], *est = g_string_new("["), *miss = g_string_new("[");
  for (guint k = 0; k < NSUM; k++) { sum[k] = g_string_new("["); }
  for (guint o = 0; o < out; o++) {
    memset(sg, 0, nc * sizeof(double));
    memset(sd, 0, nc * sizeof(double));
    memset(ng, 0, nc * sizeof(guint));
    memset(nd, 0, nc * sizeof(guint));
    double ss[NSUM] = { 0 };
    guint ns[NSUM] = { 0 }, nrow = 0, ne = 0, nm = 0;
    for (guint j = 0; j < f; j++) {
      const gint64 m = start_min + (gint64)(o * f + j) * step;
      if (m < 0)
        continue;
      const guint slot = tier == 1 ? (guint)(m % N1) : (guint)((m / 15) % N2);
      const RowHead *r = head_at(h, tier, slot);
      if (r->minute != m)
        continue;
      const gint8 *g = (const gint8 *)(r + 1), *d = g + nc;
      for (guint c = 0; c < nc; c++) {
        if (g[c] != SKIM_PHASE_NONE) { sg[c] += g[c]; ng[c]++; }
        if (d[c] != SKIM_PHASE_NONE) { sd[c] += d[c]; nd[c]++; }
      }
      for (guint k = 0; k < NSUM; k++) {
        if (!isnan(r->sum[k])) { ss[k] += r->sum[k]; ns[k]++; }
      }
      nrow++;
      if (r->flags & 1) { ne++; }
      if (r->flags & 2) { nm++; }
    }
    for (guint c = 0; c < nc; c++) {
      og[(gsize)o * nc + c] = ng[c] ? (gint8)lround(sg[c] / ng[c]) : SKIM_PHASE_NONE;
      od[(gsize)o * nc + c] = nd[c] ? (gint8)lround(sd[c] / nd[c]) : SKIM_PHASE_NONE;
    }
    for (guint k = 0; k < NSUM; k++) {
      if (ns[k]) {
        g_string_append_printf(sum[k], "%s%.2f", o ? "," : "", ss[k] / ns[k]);
      } else {
        g_string_append_printf(sum[k], "%snull", o ? "," : "");
      }
    }
    g_string_append_printf(est, "%s%d", o ? "," : "", nrow && ne * 2 > nrow ? 1 : 0);
    g_string_append_printf(miss, "%s%d", o ? "," : "", nrow && nm * 2 > nrow ? 1 : 0);
  }
  char *bg = g_base64_encode((const guchar *)og, (gsize)out * nc);
  char *bd = g_base64_encode((const guchar *)od, (gsize)out * nc);
  GString *j = g_string_new(NULL);
  g_string_append_printf(j, "{\"nchan\":%u,\"first_hz\":%.1f,\"spacing_hz\":%.1f,\"step_min\":%u,"
                         "\"t0\":%" G_GINT64_FORMAT ",\"rows\":%u,\"gain\":\"%s\",\"drop\":\"%s\","
                         "\"est\":%s],\"b_missing\":%s],\"sum\":{",
                         nc, h->first_hz, h->spacing_hz, step * f, start_min * 60, out, bg, bd,
                         est->str, miss->str);
  for (guint k = 0; k < NSUM; k++) {
    g_string_append_printf(j, "%s\"%s\":%s]", k ? "," : "", SUM_KEY[k], sum[k]->str);
    g_string_free(sum[k], TRUE);
  }
  g_string_append(j, "}}");
  g_string_free(est, TRUE);
  g_string_free(miss, TRUE);
  g_free(bg);
  g_free(bd);
  g_free(og);
  g_free(od);
  g_free(sg);
  g_free(sd);
  g_free(ng);
  g_free(nd);
  return g_string_free(j, FALSE);
}
