/* stats.h — skimmer-headless's history: a record a minute for seven days,
 * a record per 15 minutes for 90 days, an hourly one for two years.
 *
 * Per band and mode (CW, RTTY): spots sent, stations that appeared, active
 * decoders, calls the fold held back, spots the learned gate scored below
 * its threshold, the median SNR of the spots, the share of the minute the
 * thermal guard had the band paused. Globally: CPU (all, and the
 * CW and RTTY pipelines' share), the CPU temperature (peak), lost IQ, feed
 * clients, whether the radio streamed and which one.
 *
 * The three rings live in a fixed-size file (~19 MB, held in memory too)
 * under ~/.local/state/skimmer-for-linux/stats.bin; every few minutes and at
 * exit the slots changed since are written into it in place (a few kB), so
 * a restart — every settings change is one — keeps the graphs.
 * Bands are matched by name: a band added later starts empty, a band
 * dropped from the config loses its history at the next save. Older files
 * are read and carried over: version 1 (no CPU split), 3 (no CPU
 * temperature) and 4 (no thermal pauses) with those empty.
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
  SKIM_SM_THERMAL,         /* share of the minute the thermal guard had the
                            * band paused, 0–1 (NaN: the guard is off)       */
  SKIM_SM_N,
} SkimStatsBandMetric;

typedef enum {
  SKIM_SG_CPU,             /* % of one core (the minute's mean since v2)     */
  SKIM_SG_LOST,            /* % of the minute's IQ frames lost on the wire   */
  SKIM_SG_CLIENTS,         /* telnet feed clients                            */
  SKIM_SG_STREAMING,       /* 1 = the radio streamed                         */
  SKIM_SG_RADIO,           /* which [radio] host, 1-based (0 = none)         */
  SKIM_SG_CPU_CW,          /* % of one core: the CW pipelines' threads       */
  SKIM_SG_CPU_RTTY,        /* % of one core: the RTTY pipelines' threads     */
  SKIM_SG_CPU_TEMP,        /* °C: the hottest CPU sensor, the minute's peak  */
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

/* Write what changed to the file: the new slots in place, or the whole
 * ring (atomically) when the file is missing, old or of other bands. */
gboolean skim_stats_save(SkimStats *s, GError **error);

/* The hours (1..17520, two years) ending at now_minute (a later minute
 * than the present for a window in the past: pass that window's end), in buckets of
 * step minutes (0 = automatic: at most ~720 buckets), from the finest ring
 * that covers them, as a JSON body (see stats.c). */
char *skim_stats_json(SkimStats *s, gint64 now_minute, guint hours, guint step);

G_END_DECLS
