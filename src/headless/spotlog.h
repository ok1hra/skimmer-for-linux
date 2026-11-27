/* spotlog.h — the spots the feed sent, the last hours of them, for the web
 * page (/api/spots: the map, the DX cluster, the band ladder).
 *
 * Kept in memory, newest last, at most keep_s seconds and max spots; saved
 * to a small file once a minute and at exit (and before a restart's exec)
 * and read back at the start, so a restart does not empty the page. The
 * file is tab-separated, a spot a line, oldest first:
 *
 *   # skimmer-headless spots 1
 *   1763583012  7046500.0  OK1MCK  CW  41  25  (source: empty = this skimmer)
 *
 * and a phased band's spot one field more: what phasing gave it, dB over
 * the better antenna and over antenna A, "+2.1/+3.4" ("nan" where a meter
 * had none). A build before it skips such a line; an unphased spot's line
 * is as before.
 *
 * A line that does not read is skipped; a spot older than keep_s (or more
 * than five minutes ahead of the clock) is not taken back. Thread-safe.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gint64 t;                    /* unix s                                      */
  char   call[16], mode[8];
  double freq_hz, snr_db, speed;
  char   src[64];              /* "" = this skimmer; a worker or a peer        */
  gboolean phased;             /* a phased band's: phasing's gain (rbn_feed.h)  */
  double phase_db, phase_a_db; /* over the better antenna, over A (NAN: none)   */
} SkimHdSpot;

typedef struct _SkimHdSpotLog SkimHdSpotLog;

SkimHdSpotLog *skim_hd_spotlog_new(gint64 keep_s, guint max);
void           skim_hd_spotlog_free(SkimHdSpotLog *l);

/* One spot (copied). Spots older than keep_s before it, and the oldest
 * beyond max, leave. */
void skim_hd_spotlog_add(SkimHdSpotLog *l, const SkimHdSpot *s);

/* Newest first, under the log's lock; fn returns FALSE to stop. */
typedef gboolean (*SkimHdSpotFn)(const SkimHdSpot *s, gpointer user);
void skim_hd_spotlog_foreach_newest(SkimHdSpotLog *l, SkimHdSpotFn fn, gpointer user);

guint skim_hd_spotlog_length(SkimHdSpotLog *l);

/* Write the log to path (atomically; the directory is made). Nothing is
 * written when no spot came since the last save — TRUE then too. */
gboolean skim_hd_spotlog_save(SkimHdSpotLog *l, const char *path, GError **error);

/* Read path back into the log (before the first add); now: unix s.
 * Returns the spots taken; a missing file is 0. */
guint skim_hd_spotlog_load(SkimHdSpotLog *l, const char *path, gint64 now);

G_END_DECLS
