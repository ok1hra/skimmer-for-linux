/*
 * skimmer-tap-replay — replay a decode tap through the post-decode path.
 *
 *   skimmer-tap-replay [options] <tap>...
 *
 *     --rows FILE       write the gate's view (gatelog JSON lines: "c" rows,
 *                       hold/send/drop/spot/gone events) — the learning set
 *     --tap-out FILE    write the tap back out (a replay of a tap must
 *                       reproduce it line for line — the faithfulness check)
 *     --scp FILE        MASTER.SCP (default ~/.config/skimmer-for-linux/master.scp,
 *                       the file skimmer-headless loads)
 *     --min-score X --min-hearings N --settle-s S --fresh-s S
 *                       the hand gate's policy, as headless.ini [feed] takes it
 *     --nchan N         extractor slots to size for (default 4096 channels)
 *
 * A decode tap (gatelog.h) is what skimmer-headless's extractors ate live,
 * one line per decode, after ghost arbitration and the frequency lock. This
 * feeds it back into an OFFLINE pipeline on the tap's own clock: extractor
 * → station table → RBN feed policy, the very code that ran live, so a gate
 * or a feature can change and be recomputed over every night recorded so
 * far. A "# skimmer tap" header starts a fresh pipeline: headless was
 * restarted there, with empty extractors and an empty station table.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/pipeline.h"
#include "engine/rbn_feed.h"

typedef struct {
  const char *rows, *tap_out, *scp;
  double      min_score, settle_s, fresh_s;
  guint       min_hearings, nchan;
} Opts;

typedef struct {
  SkimPipeline *p;
  SkimRbnFeed  *feed;
  guint         nslot;
  guint64       texts, resets, bad, sessions;
} Run;

static void run_end(Run *r) {
  if (!r->p)
    return;
  skim_pipeline_stop(r->p);
  skim_pipeline_free(r->p);
  r->p = NULL;
}

static gboolean run_begin(Run *r, const Opts *o, guint nslot) {
  run_end(r);
  SkimPipelineConfig cfg = {
    .mode             = SKIM_PIPELINE_MODE_CW,
    .dict_path        = g_file_test(o->scp, G_FILE_TEST_EXISTS) ? o->scp : NULL,
    .rbn              = r->feed,
    .rbn_min_score    = o->min_score,
    .rbn_min_hearings = o->min_hearings,
    .rbn_settle_s     = o->settle_s,
    .rbn_fresh_s      = o->fresh_s,
    .tap_path         = o->tap_out,
    .gatelog_path     = o->rows,
  };
  r->p = skim_pipeline_new(&cfg);
  GError *err = NULL;
  if (!skim_pipeline_start_offline(r->p, &err) ||
      !skim_pipeline_tap_begin(r->p, o->nchan, nslot)) {
    fprintf(stderr, "tap-replay: cannot start a replay pipeline (%s; nslot %u)\n",
            err ? err->message : "slot count differs from this build", nslot);
    g_clear_error(&err);
    run_end(r);
    return FALSE;
  }
  r->nslot = nslot;
  r->sessions++;
  return TRUE;
}

/* "T <t_us> <wall_us> <ix> <hz> <wpm> <snr> <conf> <contested> |text|" */
static gboolean parse_text(const char *line, gint64 *t, gint64 *w, guint *ix,
                           double *hz, SkimDecode *d, gboolean *contested) {
  const char *bar = strchr(line, '|');
  const char *end = strrchr(line, '|');
  if (!bar || end == bar)
    return FALSE;
  char *head = g_strndup(line + 2, (gsize)(bar - line - 2));
  gchar **f = g_strsplit_set(g_strstrip(head), " ", -1);
  gboolean ok = g_strv_length(f) == 8;
  if (ok) {
    memset(d, 0, sizeof(*d));
    *t  = g_ascii_strtoll(f[0], NULL, 10);
    *w  = g_ascii_strtoll(f[1], NULL, 10);
    *ix = (guint)g_ascii_strtoull(f[2], NULL, 10);
    *hz = g_ascii_strtod(f[3], NULL);
    d->speed      = g_ascii_strtod(f[4], NULL);
    d->snr_db     = g_ascii_strtod(f[5], NULL);
    d->confidence = g_ascii_strtod(f[6], NULL);
    *contested    = f[7][0] == '1';
    const gsize n = MIN((gsize)(end - bar - 1), sizeof(d->text) - 1);
    memcpy(d->text, bar + 1, n);
    d->text[n] = '\0';
  }
  g_strfreev(f);
  g_free(head);
  return ok;
}

static void replay_file(Run *r, const Opts *o, const char *path) {
  FILE *fh = fopen(path, "r");
  if (!fh) {
    fprintf(stderr, "tap-replay: %s: cannot open\n", path);
    return;
  }
  char *line = NULL;
  size_t cap = 0;
  ssize_t len;
  while ((len = getline(&line, &cap, fh)) >= 0) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    if (g_str_has_prefix(line, "# skimmer tap v1")) {
      const char *ns = strstr(line, "nslot=");
      if (!run_begin(r, o, ns ? (guint)atoi(ns + 6) : 0))
        break;
      continue;
    }
    if (!r->p) {
      r->bad++;                        /* data before any header             */
      continue;
    }
    if (line[0] == 'T' && line[1] == ' ') {
      gint64 t, w;
      guint ix;
      double hz;
      SkimDecode d;
      gboolean ct;
      if (!parse_text(line, &t, &w, &ix, &hz, &d, &ct) ||
          ix >= o->nchan * r->nslot) {
        r->bad++;
        continue;
      }
      skim_pipeline_tap_text(r->p, t, w, ix, hz, &d, ct);
      r->texts++;
    } else if (line[0] == 'R' && line[1] == ' ') {
      gint64 t, w;
      gint ix;
      if (sscanf(line + 2, "%" G_GINT64_FORMAT " %" G_GINT64_FORMAT " %d",
                 &t, &w, &ix) != 3) {
        r->bad++;
        continue;
      }
      skim_pipeline_tap_reset(r->p, t, w, ix);
      r->resets++;
    } else if (line[0] == 'C' && line[1] == ' ') {
      gint64 t, w;
      if (sscanf(line + 2, "%" G_GINT64_FORMAT " %" G_GINT64_FORMAT, &t, &w) != 2) {
        r->bad++;
        continue;
      }
      skim_pipeline_tap_tick(r->p, t, w);
    } else if (line[0] && line[0] != '#') {
      r->bad++;
    }
  }
  free(line);
  fclose(fh);
}

int main(int argc, char **argv) {
  char *scp_default = g_build_filename(g_get_user_config_dir(),
                                       "skimmer-for-linux", "master.scp", NULL);
  Opts o = { .scp = scp_default, .nchan = 4096 };
  gchar *rows = NULL, *tap_out = NULL, *scp = NULL;
  gint hearings = 0, nchan = 0;
  gchar **files = NULL;
  GOptionEntry ent[] = {
    { "rows", 0, 0, G_OPTION_ARG_FILENAME, &rows, "gatelog rows out", "FILE" },
    { "tap-out", 0, 0, G_OPTION_ARG_FILENAME, &tap_out, "tap out", "FILE" },
    { "scp", 0, 0, G_OPTION_ARG_FILENAME, &scp, "MASTER.SCP", "FILE" },
    { "min-score", 0, 0, G_OPTION_ARG_DOUBLE, &o.min_score, "hand gate score", "X" },
    { "min-hearings", 0, 0, G_OPTION_ARG_INT, &hearings, "hand gate hearings", "N" },
    { "settle-s", 0, 0, G_OPTION_ARG_DOUBLE, &o.settle_s, "hand gate settle", "S" },
    { "fresh-s", 0, 0, G_OPTION_ARG_DOUBLE, &o.fresh_s,
      "the call must have been READ within S s (0 = default: off; < 0 = off)", "S" },
    { "nchan", 0, 0, G_OPTION_ARG_INT, &nchan, "channels to size for", "N" },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &files, NULL, "TAP…" },
    { NULL },
  };
  GOptionContext *ctx = g_option_context_new("— replay a decode tap through the feed gate");
  g_option_context_add_main_entries(ctx, ent, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &err) || !files) {
    fprintf(stderr, "%s\n", err ? err->message : "no tap given (--help)");
    return 2;
  }
  g_option_context_free(ctx);
  if (scp) { o.scp = scp; }
  o.rows = rows;
  o.tap_out = tap_out;
  o.min_hearings = hearings > 0 ? (guint)hearings : 0;
  if (nchan > 0) { o.nchan = (guint)nchan; }

  Run r = { 0 };
  r.feed = skim_rbn_feed_new("REPLAY", 0, &err);   /* ephemeral, no clients */
  if (!r.feed) {
    fprintf(stderr, "tap-replay: feed: %s\n", err ? err->message : "?");
    return 1;
  }
  for (guint i = 0; files[i]; i++) { replay_file(&r, &o, files[i]); }
  run_end(&r);
  fprintf(stderr, "tap-replay: %u file(s), %" G_GUINT64_FORMAT " session(s), %"
          G_GUINT64_FORMAT " decodes, %" G_GUINT64_FORMAT " resets, %"
          G_GUINT64_FORMAT " bad lines\n", g_strv_length(files), r.sessions,
          r.texts, r.resets, r.bad);
  skim_rbn_feed_free(r.feed);
  g_strfreev(files);
  g_free(rows);
  g_free(tap_out);
  g_free(scp);
  g_free(scp_default);
  return 0;
}
