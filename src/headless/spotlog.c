/* spotlog.c — the spots the feed sent, kept across a restart (spotlog.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "headless/spotlog.h"

#define HEADER "# skimmer-headless spots 1\n"

struct _SkimHdSpotLog {
  GMutex lock;
  GQueue q;                    /* SkimHdSpot*, oldest first                   */
  gint64 keep_s;
  guint  max;
  guint64 gen, saved_gen;      /* adds so far; the count at the last save      */
};

SkimHdSpotLog *skim_hd_spotlog_new(gint64 keep_s, guint max) {
  SkimHdSpotLog *l = g_new0(SkimHdSpotLog, 1);
  g_mutex_init(&l->lock);
  g_queue_init(&l->q);
  l->keep_s = keep_s;
  l->max = max;
  return l;
}

void skim_hd_spotlog_free(SkimHdSpotLog *l) {
  if (!l) { return; }
  g_queue_clear_full(&l->q, g_free);
  g_mutex_clear(&l->lock);
  g_free(l);
}

static void push(SkimHdSpotLog *l, const SkimHdSpot *s) {
  g_queue_push_tail(&l->q, g_memdup2(s, sizeof(*s)));
  SkimHdSpot *o;
  while ((o = g_queue_peek_head(&l->q)) && (s->t - o->t > l->keep_s || l->q.length > l->max)) {
    g_free(g_queue_pop_head(&l->q));
  }
}

void skim_hd_spotlog_add(SkimHdSpotLog *l, const SkimHdSpot *s) {
  g_mutex_lock(&l->lock);
  push(l, s);
  l->gen++;
  g_mutex_unlock(&l->lock);
}

void skim_hd_spotlog_foreach_newest(SkimHdSpotLog *l, SkimHdSpotFn fn, gpointer user) {
  g_mutex_lock(&l->lock);
  for (GList *e = l->q.tail; e; e = e->prev) {
    if (!fn(e->data, user)) { break; }
  }
  g_mutex_unlock(&l->lock);
}

guint skim_hd_spotlog_length(SkimHdSpotLog *l) {
  g_mutex_lock(&l->lock);
  const guint n = l->q.length;
  g_mutex_unlock(&l->lock);
  return n;
}

/* A field as one token of the line: no tab, no line break. */
static void field(GString *o, const char *s) {
  for (; *s; s++) { g_string_append_c(o, (*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s); }
}

gboolean skim_hd_spotlog_save(SkimHdSpotLog *l, const char *path, GError **error) {
  GString *o = g_string_new(HEADER);
  g_mutex_lock(&l->lock);
  const guint64 gen = l->gen;
  const gboolean same = gen == l->saved_gen;
  for (GList *e = same ? NULL : l->q.head; e; e = e->next) {
    const SkimHdSpot *s = e->data;
    char f[G_ASCII_DTOSTR_BUF_SIZE], snr[G_ASCII_DTOSTR_BUF_SIZE], sp[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(f, sizeof(f), "%.1f", s->freq_hz);
    g_ascii_formatd(snr, sizeof(snr), "%.1f", s->snr_db);
    g_ascii_formatd(sp, sizeof(sp), "%.1f", s->speed);
    g_string_append_printf(o, "%" G_GINT64_FORMAT "\t%s\t", s->t, f);
    field(o, s->call);
    g_string_append_c(o, '\t');
    field(o, s->mode);
    g_string_append_printf(o, "\t%s\t%s\t", snr, sp);
    field(o, s->src);
    g_string_append_c(o, '\n');
  }
  g_mutex_unlock(&l->lock);
  if (same) {
    g_string_free(o, TRUE);
    return TRUE;
  }
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  const gboolean ok = g_file_set_contents(path, o->str, (gssize)o->len, error);
  g_string_free(o, TRUE);
  if (ok) {
    g_mutex_lock(&l->lock);
    l->saved_gen = gen;
    g_mutex_unlock(&l->lock);
  }
  return ok;
}

/* A number field: the whole of it, or FALSE. */
static gboolean num(const char *s, double *v) {
  char *end = NULL;
  *v = g_ascii_strtod(s, &end);
  return *s && end && !*end;
}

guint skim_hd_spotlog_load(SkimHdSpotLog *l, const char *path, gint64 now) {
  char *text = NULL;
  if (!g_file_get_contents(path, &text, NULL, NULL)) { return 0; }
  guint n = 0;
  char **lines = g_strsplit(text, "\n", -1);
  g_mutex_lock(&l->lock);
  for (char **ln = lines; *ln; ln++) {
    if (!**ln || **ln == '#') { continue; }
    char **f = g_strsplit(*ln, "\t", -1);
    SkimHdSpot s = { 0 };
    double t, fr, snr, sp;
    if (g_strv_length(f) == 7 && num(f[0], &t) && num(f[1], &fr) && num(f[4], &snr) &&
        num(f[5], &sp) && f[2][0] && f[3][0] && t > now - l->keep_s && t <= now + 300 && fr > 0) {
      s.t = (gint64)t;
      s.freq_hz = fr;
      s.snr_db = snr;
      s.speed = sp;
      g_strlcpy(s.call, f[2], sizeof(s.call));
      g_strlcpy(s.mode, f[3], sizeof(s.mode));
      for (char *x = s.mode; *x; x++) {        /* as at the add: no markup     */
        if (!g_ascii_isalnum(*x)) { *x = '?'; }
      }
      g_strlcpy(s.src, f[6], sizeof(s.src));
      /* in time order only: a line out of order would break the trimming */
      const SkimHdSpot *last = g_queue_peek_tail(&l->q);
      if (!last || s.t >= last->t) {
        push(l, &s);
        n++;
      }
    }
    g_strfreev(f);
  }
  l->saved_gen = l->gen;       /* what is on disk is the log now               */
  g_mutex_unlock(&l->lock);
  g_strfreev(lines);
  g_free(text);
  return n;
}
