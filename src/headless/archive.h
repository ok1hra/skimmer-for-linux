/* archive.h — every spot the feed sends, kept on disk ([archive] dir).
 *
 * One gzip file a UTC day, <dir>/YYYYMMDD-spots.gz, a tab-separated line a
 * spot:
 *
 *   # time_utc  khz  call  mode  snr_db  speed  band  source
 *   2026-11-18 19:50:12  7046.5  OK1MCK  CW  41  25  40m  local
 *
 * (speed: WPM, RTTY: baud; source: local, or the worker or peer whose spot
 * it was; the header line opens each day's file). Spots are kept in memory
 * and appended once a minute (and at exit) as one more gzip member — gzip
 * files may be concatenated, `zcat` / `zless` read them whole, and a crash
 * loses at most the last minute, never the day.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimHdArchive SkimHdArchive;

/* Keep the spots in dir (created). NULL + error when it cannot be. */
SkimHdArchive *skim_hd_archive_new(const char *dir, GError **error);
void           skim_hd_archive_free(SkimHdArchive *a);      /* flushes first */

/* One spot (any thread). t: unix s; src: "" or NULL = this skimmer. */
void skim_hd_archive_add(SkimHdArchive *a, gint64 t, const char *call, const char *mode,
                         double freq_hz, double snr_db, double speed, const char *band,
                         const char *src);

/* Append what is waiting to its day's file (the main loop, once a minute).
 * On an error the lines stay waiting for the next try (at most a day's). */
gboolean skim_hd_archive_flush(SkimHdArchive *a, GError **error);

/* For the status: the directory, today's file name, spots written since
 * start, spots waiting, the last error (NULL = none). Strings: the caller
 * frees them. */
void skim_hd_archive_status(SkimHdArchive *a, char **dir, char **today, guint64 *written,
                            guint *waiting, char **error);

/* The day file's name for unix time t: "YYYYMMDD-spots.gz" (UTC). */
char *skim_hd_archive_file_name(gint64 t);

G_END_DECLS
