/* gatelog.c — the feed-gate learning logs (see gatelog.h).
 *
 * Numbers go out through g_ascii_formatd: the process may run under a
 * locale with a decimal comma, and both files are parsed by machines.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "gatelog.h"

#include <stdio.h>
#include <string.h>

struct _SkimGateLog {
  FILE *tap;
  FILE *rows;
};

static FILE *open_append(const char *path, const char *what) {
  if (!path)
    return NULL;
  FILE *f = fopen(path, "a");
  if (!f) { g_warning("gatelog: %s %s not writable", what, path); }
  return f;
}

SkimGateLog *skim_gatelog_open(const char *tap_path, const char *rows_path,
                               const char *engine, guint nslot) {
  FILE *tap = open_append(tap_path, "tap");
  FILE *rows = open_append(rows_path, "rows");
  if (!tap && !rows)
    return NULL;
  SkimGateLog *g = g_new0(SkimGateLog, 1);
  g->tap = tap;
  g->rows = rows;
  if (tap) {
    fprintf(tap, "# skimmer tap v1 engine=%s nslot=%u\n",
            engine ? engine : "?", nslot);
    fflush(tap);
  }
  return g;
}

void skim_gatelog_close(SkimGateLog *g) {
  if (!g)
    return;
  if (g->tap) { fclose(g->tap); }
  if (g->rows) { fclose(g->rows); }
  g_free(g);
}

/* "%.<prec>f" in the C locale, into one of a few rotating buffers so a
 * single fprintf can take several. Per thread: every band's pipeline logs
 * from its own engine thread. */
static const char *num(double v, int prec) {
  static _Thread_local char buf[16][G_ASCII_DTOSTR_BUF_SIZE];
  static _Thread_local guint k;
  char *b = buf[k++ % G_N_ELEMENTS(buf)];
  char fmt[8];
  g_snprintf(fmt, sizeof(fmt), "%%.%df", prec);
  return g_ascii_formatd(b, G_ASCII_DTOSTR_BUF_SIZE, fmt, v);
}

void skim_gatelog_tap_text(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                           guint ix, double hz, double wpm, double snr_db,
                           double confidence, gboolean contested,
                           const char *text) {
  if (!g || !g->tap)
    return;
  fprintf(g->tap, "T %" G_GINT64_FORMAT " %" G_GINT64_FORMAT " %u %s %s %s %s %d |",
          t_us, wall_us, ix, num(hz, 1), num(wpm, 1), num(snr_db, 1),
          num(confidence, 3), contested ? 1 : 0);
  for (const char *s = text; *s; s++) {         /* one line per decode       */
    fputc(*s == '\n' || *s == '\r' ? ' ' : *s, g->tap);
  }
  fputs("|\n", g->tap);
  fflush(g->tap);
}

void skim_gatelog_tap_clock(SkimGateLog *g, gint64 t_us, gint64 wall_us) {
  if (!g || !g->tap)
    return;
  fprintf(g->tap, "C %" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n", t_us, wall_us);
  fflush(g->tap);
}

void skim_gatelog_tap_reset(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                            gint ix) {
  if (!g || !g->tap)
    return;
  fprintf(g->tap, "R %" G_GINT64_FORMAT " %" G_GINT64_FORMAT " %d\n", t_us,
          wall_us, ix);
  fflush(g->tap);
}

void skim_gatelog_cand(SkimGateLog *g, gint64 t_us, gint64 wall_us, guint ix,
                       double hz, double wpm, double snr_db, double confidence,
                       gboolean contested, const SkimCallsignCand *cand,
                       const SkimStation *st, gboolean gate) {
  if (!g || !g->rows || !cand)
    return;
  fprintf(g->rows,
          "{\"ev\":\"c\",\"w\":%s,\"t\":%s,\"ix\":%u,\"hz\":%s,\"wpm\":%s,"
          "\"snr\":%s,\"conf\":%s,\"ct\":%d,\"call\":\"%s\",\"sc\":%s,"
          "\"n\":%u,\"parts\":%u,\"idle\":%u,\"read\":%s,\"de\":%d,\"cq\":%d,"
          "\"dict\":%d",
          num(wall_us / 1e6, 3), num(t_us / 1e6, 3), ix, num(hz, 1),
          num(wpm, 1), num(snr_db, 1), num(confidence, 3), contested ? 1 : 0,
          cand->call, num(cand->score, 2), cand->count, cand->parts,
          cand->idle_tokens, num((t_us - cand->last_us) / 1e6, 1),
          cand->de_marked ? 1 : 0,
          cand->cq_context ? 1 : 0, cand->dict ? 1 : 0);
  if (st) {
    fprintf(g->rows,
            ",\"st\":{\"call\":\"%s\",\"hz\":%s,\"snr\":%s,\"wpm\":%s,"
            "\"sc\":%s,\"hear\":%u,\"cq\":%d,\"rep\":%u,\"age\":%s,\"read\":%s},"
            "\"gate\":%d",
            st->call, num(st->freq_hz, 1), num(st->snr_db, 1),
            num(st->speed, 1), num(st->score, 2), st->hearings,
            st->cq ? 1 : 0, st->reports,
            num((st->last_heard - st->first_heard) / 1e6, 1),
            num((t_us - st->heard_us) / 1e6, 1), gate ? 1 : 0);
  }
  fputs("}\n", g->rows);
  fflush(g->rows);
}

void skim_gatelog_event(SkimGateLog *g, gint64 t_us, gint64 wall_us,
                        const char *ev, const char *call, double hz,
                        double snr_db, double wpm) {
  if (!g || !g->rows)
    return;
  fprintf(g->rows,
          "{\"ev\":\"%s\",\"w\":%s,\"t\":%s,\"call\":\"%s\",\"hz\":%s,"
          "\"snr\":%s,\"wpm\":%s}\n",
          ev, num(wall_us / 1e6, 3), num(t_us / 1e6, 3), call, num(hz, 1),
          num(snr_db, 1), num(wpm, 1));
  fflush(g->rows);
}
