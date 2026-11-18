/* archive.c — see archive.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "archive.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <unistd.h>

#define WAIT_MAX 200000                /* lines kept while the disk refuses  */

typedef struct {
  gint64 day;                          /* unix day (UTC) of the spot         */
  char  *text;                         /* the line, '\n' included            */
} Line;

struct _SkimHdArchive {
  char      *dir;
  GMutex     lock;
  GPtrArray *wait;                     /* Line*, oldest first                */
  guint64    written;
  char      *error;                    /* the last flush's, NULL = fine      */
};

static void line_free(gpointer p) {
  Line *l = p;
  g_free(l->text);
  g_free(l);
}

char *skim_hd_archive_file_name(gint64 t) {
  GDateTime *d = g_date_time_new_from_unix_utc(t);
  char *s = g_date_time_format(d, "%Y%m%d-spots.gz");
  g_date_time_unref(d);
  return s;
}

SkimHdArchive *skim_hd_archive_new(const char *dir, GError **error) {
  if (g_mkdir_with_parents(dir, 0755) != 0) {
    const int e = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e), "%s: %s", dir, g_strerror(e));
    return NULL;
  }
  if (g_access(dir, W_OK) != 0) {
    const int e = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e), "%s: %s", dir, g_strerror(e));
    return NULL;
  }
  SkimHdArchive *a = g_new0(SkimHdArchive, 1);
  a->dir = g_strdup(dir);
  g_mutex_init(&a->lock);
  a->wait = g_ptr_array_new_with_free_func(line_free);
  return a;
}

void skim_hd_archive_free(SkimHdArchive *a) {
  if (!a) { return; }
  GError *err = NULL;
  if (!skim_hd_archive_flush(a, &err)) {
    g_warning("archive: %s", err->message);
    g_clear_error(&err);
  }
  g_ptr_array_unref(a->wait);
  g_mutex_clear(&a->lock);
  g_free(a->error);
  g_free(a->dir);
  g_free(a);
}

void skim_hd_archive_add(SkimHdArchive *a, gint64 t, const char *call, const char *mode,
                         double freq_hz, double snr_db, double speed, const char *band,
                         const char *src) {
  if (!a) { return; }
  GDateTime *d = g_date_time_new_from_unix_utc(t);
  char *when = g_date_time_format(d, "%Y-%m-%d %H:%M:%S");
  g_date_time_unref(d);
  char khz[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(khz, sizeof(khz), "%.1f", freq_hz / 1000.0);
  Line *l = g_new(Line, 1);
  l->day = t / 86400;
  l->text = g_strdup_printf("%s\t%s\t%s\t%s\t%.0f\t%.0f\t%s\t%s\n", when, khz, call, mode, snr_db,
                            speed, band && band[0] ? band : "-",
                            src && src[0] ? src : "local");
  g_free(when);
  g_mutex_lock(&a->lock);
  g_ptr_array_add(a->wait, l);
  if (a->wait->len > WAIT_MAX) { g_ptr_array_remove_range(a->wait, 0, a->wait->len - WAIT_MAX); }
  g_mutex_unlock(&a->lock);
}

/* One gzip member with these lines, appended to the day's file. */
static gboolean append_day(const char *dir, gint64 day, GPtrArray *lines, guint from, guint to,
                           GError **error) {
  char *name = skim_hd_archive_file_name(day * 86400);
  char *path = g_build_filename(dir, name, NULL);
  g_free(name);
  const gboolean fresh = !g_file_test(path, G_FILE_TEST_EXISTS);
  GFile *f = g_file_new_for_path(path);
  GFileOutputStream *fo = g_file_append_to(f, G_FILE_CREATE_NONE, NULL, error);
  g_object_unref(f);
  gboolean ok = fo != NULL;
  if (ok) {
    GZlibCompressor *z = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
    GOutputStream *out = g_converter_output_stream_new(G_OUTPUT_STREAM(fo), G_CONVERTER(z));
    g_object_unref(z);
    GString *buf = g_string_new(fresh ? "# time_utc\tkhz\tcall\tmode\tsnr_db\tspeed\tband\tsource\n"
                                      : "");
    for (guint i = from; i < to; i++) { g_string_append(buf, ((Line *)lines->pdata[i])->text); }
    ok = g_output_stream_write_all(out, buf->str, buf->len, NULL, NULL, error) &&
         g_output_stream_close(out, NULL, error);
    g_string_free(buf, TRUE);
    g_object_unref(out);
    g_object_unref(fo);
  }
  if (!ok) { g_prefix_error(error, "%s: ", path); }
  g_free(path);
  return ok;
}

gboolean skim_hd_archive_flush(SkimHdArchive *a, GError **error) {
  if (!a) { return TRUE; }
  g_mutex_lock(&a->lock);
  GPtrArray *lines = a->wait;
  a->wait = g_ptr_array_new_with_free_func(line_free);
  g_mutex_unlock(&a->lock);
  /* runs of one day each (midnight splits the minute) */
  guint done = 0;
  gboolean ok = TRUE;
  GError *e = NULL;
  while (ok && done < lines->len) {
    const gint64 day = ((Line *)lines->pdata[done])->day;
    guint end = done;
    while (end < lines->len && ((Line *)lines->pdata[end])->day == day) { end++; }
    ok = append_day(a->dir, day, lines, done, end, &e);
    if (ok) { done = end; }
  }
  g_mutex_lock(&a->lock);
  a->written += done;
  g_clear_pointer(&a->error, g_free);
  if (!ok) {
    a->error = g_strdup(e->message);
    /* what did not go out waits, ahead of what came in meanwhile */
    g_ptr_array_remove_range(lines, 0, done);
    g_ptr_array_extend_and_steal(lines, a->wait);
    a->wait = lines;
    if (a->wait->len > WAIT_MAX) { g_ptr_array_remove_range(a->wait, 0, a->wait->len - WAIT_MAX); }
    lines = NULL;
  }
  g_mutex_unlock(&a->lock);
  if (lines) { g_ptr_array_unref(lines); }
  if (e) { g_propagate_error(error, e); }
  return ok;
}

void skim_hd_archive_status(SkimHdArchive *a, char **dir, char **today, guint64 *written,
                            guint *waiting, char **error) {
  g_mutex_lock(&a->lock);
  if (dir) { *dir = g_strdup(a->dir); }
  if (today) { *today = skim_hd_archive_file_name(g_get_real_time() / G_USEC_PER_SEC); }
  if (written) { *written = a->written; }
  if (waiting) { *waiting = a->wait->len; }
  if (error) { *error = g_strdup(a->error); }
  g_mutex_unlock(&a->lock);
}
