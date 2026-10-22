/* stats.h — skimmer-headless's history: one record a minute, seven days.
 *
 * Per band and mode (CW, RTTY): spots sent, stations that appeared, active
 * decoders, calls the fold held back, spots the learned gate scored below
 * its threshold, the median SNR of the spots. Globally: CPU, lost IQ, feed
 * clients, whether the radio streamed and which one.
 *
 * The ring lives in a fixed-size file (~4 MB) under
 * ~/.local/state/skimmer-for-linux/stats.bin, written every few minutes and
 * at exit, so a restart — every settings change is one — keeps the graphs.
 * Bands are matched by name: a band added later starts empty, a band
 * dropped from the config loses its history at the next save.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_STATS_MINUTES (7 * 24 * 60)
#define SKIM_STATS_BANDS   8
#define SKIM_STATS_MODES   2              /* 0 = CW, 1 = RTTY                */

typedef enum {
  SKIM_SM_SPOTS,           /* sent to the feed in the minute                 */
  SKIM_SM_STATIONS,        /* stations that appeared in the minute           */
  SKIM_SM_ACTIVE,          /* active decoders at the end of the minute       */
  SKIM_SM_FOLDED,          /* calls the twin fold held back                  */
  SKIM_SM_GATE_BELOW,      /* spots the learned gate scored below threshold  */
  SKIM_SM_SNR,             /* median SNR of the minute's spots (NaN: none)   */
  SKIM_SM_N,
} SkimStatsBandMetric;

typedef enum {
  SKIM_SG_CPU,             /* % of one core                                  */
  SKIM_SG_LOST,            /* % of the minute's IQ frames lost on the wire   */
  SKIM_SG_CLIENTS,         /* telnet feed clients                            */
  SKIM_SG_STREAMING,       /* 1 = the radio streamed                         */
  SKIM_SG_RADIO,           /* which [radio] host, 1-based (0 = none)         */
  SKIM_SG_N,
} SkimStatsGlobalMetric;

typedef struct {
  float band[SKIM_STATS_BANDS][SKIM_STATS_MODES][SKIM_SM_N];
  float glob[SKIM_SG_N];
} SkimStatsMinute;

typedef struct _SkimStats SkimStats;

/* Open (or start) the history at path for these bands (config order). A
 * file that is missing, foreign or of another size starts a fresh ring. */
SkimStats *skim_stats_open(const char *path, const char *const *bands, guint nb);
void       skim_stats_free(SkimStats *s);      /* saves first                 */

/* The record of the minute starting at unix_minute × 60 s (overwrites the
 * slot a week back). Bands in config order, as opened. */
void skim_stats_put(SkimStats *s, gint64 unix_minute, const SkimStatsMinute *m);

/* Write the ring to its file (atomically). */
gboolean skim_stats_save(SkimStats *s, GError **error);

/* The last hours (1..168) ending at now_minute, in buckets of step minutes
 * (0 = automatic: at most ~720 buckets), as a JSON body (see stats.c). */
char *skim_stats_json(SkimStats *s, gint64 now_minute, guint hours, guint step);

G_END_DECLS
