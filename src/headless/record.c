/* record.c — see record.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "record.h"

#include <stdio.h>
#include <string.h>

struct _Rec {
  char        *path;           /* …/hd-<band>-<UTC>.cf32                       */
  char         label[96];
  double       ppm;
  gint64       at_us;          /* real time to start, 0 = at once              */
  guint64      max_frames;
  double       minutes;
  /* receiver thread */
  gboolean     started, ended;
  guint64      frames;
  double       rate, centre;
  gint64       t0_us;          /* real time of the first block                 */
  /* writer */
  GAsyncQueue *q;
  GThread     *th;
};

static gpointer REC_END = (gpointer)&REC_END;  /* queue sentinel              */

static void rec_meta(Rec *r, gint64 t1_us) {
  GDateTime *t = g_date_time_new_from_unix_local(r->t0_us / G_USEC_PER_SEC);
  char *start = g_date_time_format(t, "%Y-%m-%d %H:%M:%S %z");
  g_date_time_unref(t);
  char *meta = g_strdup_printf(
      "file: %s\nstart: %s\nstart_unix: %.3f\n"
      "source: skimmer-headless %s\n"
      "format: cf32 interleaved I,Q — TRUE spectrum orientation\n"
      "center_hz: %.0f\nrate_hz: %.0f\nclock_ppm: %.3f\nframes: %" G_GUINT64_FORMAT "\n"
      "duration_s: %.1f\nwall_s: %.1f\n---\n",
      r->path, start, r->t0_us / 1e6, r->label, r->centre, r->rate, r->ppm, r->frames,
      r->rate > 0 ? r->frames / r->rate : 0.0, (t1_us - r->t0_us) / 1e6);
  char *mp = g_strdup_printf("%s.meta", r->path);
  GError *err = NULL;
  if (!g_file_set_contents(mp, meta, -1, &err)) {
    g_warning("record: %s: %s", mp, err->message);
    g_clear_error(&err);
  }
  g_free(mp);
  g_free(meta);
  g_free(start);
}

static gpointer rec_writer(gpointer data) {
  Rec *r = data;
  FILE *f = fopen(r->path, "wb");
  if (!f) { g_warning("record: cannot write %s", r->path); }
  for (;;) {
    GBytes *b = g_async_queue_pop(r->q);
    if ((gpointer)b == REC_END) { break; }
    gsize len;
    const void *d = g_bytes_get_data(b, &len);
    if (f && fwrite(d, 1, len, f) != len) {
      g_warning("record: %s: write failed — recording stopped", r->path);
      fclose(f);
      f = NULL;
    }
    g_bytes_unref(b);
  }
  if (f) { fclose(f); }
  rec_meta(r, g_get_real_time());
  g_message("record: %s done (%.1f min)", r->path, r->frames / r->rate / 60.0);
  return NULL;
}

/* Receiver thread: start at the set time, stop after the set length. */
void skim_rec_push(Rec *r, const float *iq, guint n, double rate, double centre) {
  if (r->ended) { return; }
  const gint64 now = g_get_real_time();
  if (!r->started) {
    if (r->at_us && now < r->at_us) { return; }
    GDateTime *t = g_date_time_new_from_unix_utc(now / G_USEC_PER_SEC);
    char *ts = g_date_time_format(t, "%Y%m%d-%H%M%SZ");
    g_date_time_unref(t);
    char *dir = g_path_get_dirname(r->path);   /* path holds dir/band so far  */
    char *band = g_path_get_basename(r->path);
    g_free(r->path);
    r->path = g_strdup_printf("%s/hd-%s-%s.cf32", dir, band, ts);
    g_free(dir);
    g_free(band);
    g_free(ts);
    r->rate = rate;
    r->centre = centre;
    r->t0_us = now;
    r->max_frames = (guint64)(r->minutes * 60.0 * rate);
    r->q = g_async_queue_new();
    r->th = g_thread_new("record", rec_writer, r);
    r->started = TRUE;
    g_message("record: %s started (%.1f min)", r->path, r->minutes);
  }
  const guint take = (guint)MIN((guint64)n, r->max_frames - r->frames);
  g_async_queue_push(r->q, g_bytes_new(iq, (gsize)take * 2 * sizeof(float)));
  r->frames += take;
  if (r->frames >= r->max_frames) {
    r->ended = TRUE;
    g_async_queue_push(r->q, REC_END);
  }
}

/* Teardown: an unfinished recording is closed with what it has. */
void skim_rec_free(Rec *r) {
  if (!r) { return; }
  if (r->started) {
    if (!r->ended) { g_async_queue_push(r->q, REC_END); }
    g_thread_join(r->th);
    g_async_queue_unref(r->q);
  }
  g_free(r->path);
  g_free(r);
}

Rec *skim_rec_new(const char *dir, const char *band, const char *label, double ppm,
                  gint64 at_us, double minutes) {
  Rec *r = g_new0(Rec, 1);
  r->path = g_build_filename(dir, band, NULL);  /* the UTC stamp comes at start */
  g_strlcpy(r->label, label, sizeof(r->label));
  r->ppm = ppm;
  r->at_us = at_us;
  r->minutes = minutes;
  return r;
}
