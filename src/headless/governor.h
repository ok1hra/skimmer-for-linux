/* governor.h — what decodes when: the decoders skimmer-headless pauses on its
 * own, and why.
 *
 * Adaptive RTTY ([decode] rtty_adaptive): the RTTY decoders cost CPU on
 * every band all the time, and outside a contest they copy little. With it
 * on they run only while a contest does — a test at each full hour (UTC)
 * and at the start: the RTTY decoders on for `minutes` whole minutes; when
 * all bands together spot more than `spots` RTTY spots in them, they stay
 * on (a contest), else off until the next full hour. The test repeats each
 * hour while on as well, so the end of a contest is seen within the hour.
 * The start tests at once (a restart from the web page in the middle of a
 * contest must not lose the hour).
 *
 * The thermal guard ([thermal] enabled): the hottest CPU sensor, averaged
 * over average_min minutes (5: the heat sink's own pace — a burst of work
 * reads 95 °C for seconds on a machine whose mean is 78 °C). Over limit_c
 * for 5 minutes on end: the band (decoded here, not by a worker) with the
 * fewest spots in the last 30 minutes is paused, CW and RTTY; 10 minutes
 * later the drop of the mean since is recorded with it — and if the mean is
 * still over the limit the next one goes. One band always stays. A paused
 * band comes back last-in first-out once the mean has stayed at or under
 * limit − max(its drop, 5 °C) − restore_margin_c for 20 minutes, at most
 * one every 20 minutes: the reserve must exceed what the band itself costs,
 * so that it does not heat the CPU straight back over the limit.
 *
 * The owner pauses a paused mode's pipeline by not pushing it IQ (its
 * engine thread idles, the stations age out); it asks here after each call.
 * Time comes in from outside (unix minutes) — the gate drives it.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimHdGovernor SkimHdGovernor;

typedef struct {
  gboolean rtty_adaptive;
  guint    rtty_spots;                 /* more than this in the test: a contest */
  guint    rtty_minutes;               /* the test's whole minutes (≥ 1)        */
  gboolean thermal;
  double   limit_c;                    /* the mean's limit, °C                  */
  guint    average_min;                /* the mean's window (≥ 1 min)           */
  double   restore_margin_c;           /* reserve beyond a band's drop, °C     */
} SkimHdGovernorConfig;

/* the guard's clock (s): over the limit this long pauses a band, at least
 * this long between two, the drop measured this long after, the reserve
 * held this long brings one back (and as long between two) */
#define SKIM_GOV_OVER_S     300
#define SKIM_GOV_NEXT_S     600
#define SKIM_GOV_RESTORE_S 1200
#define SKIM_GOV_MIN_DROP_C 5.0
#define SKIM_GOV_SPOTS_MIN  30          /* the spots that pick a band: minutes  */
#define SKIM_GOV_MAX_BANDS  16

/* now_min: the unix minute of the start (the one running now). */
SkimHdGovernor *skim_hd_governor_new(const SkimHdGovernorConfig *cfg, gint64 now_min);
void            skim_hd_governor_free(SkimHdGovernor *g);

/* The bands, in receiver order; can[i]: the thermal guard may pause band i
 * (decoded here). Before the first minute. */
void skim_hd_governor_set_bands(SkimHdGovernor *g, guint n, const char *const *names,
                                const gboolean *can);

/* Minute `minute` (unix) just ended; rtty_spots: the RTTY spots all the
 * bands decoded here sent in it; band_spots: each band's spots, CW + RTTY
 * (NULL: none). TRUE when the RTTY decoders' state changed. */
gboolean skim_hd_governor_minute(SkimHdGovernor *g, gint64 minute, guint rtty_spots,
                                 const guint *band_spots);

/* Once a second: the hottest CPU sensor now (NAN: none). TRUE when a band
 * was paused or came back. */
gboolean skim_hd_governor_temp(SkimHdGovernor *g, gint64 now_s, double temp_c);

/* The thermal guard pauses band i now. */
gboolean skim_hd_governor_band_shed(const SkimHdGovernor *g, guint i);

/* The mean the guard judges by (NAN: not yet, or the guard off). */
double skim_hd_governor_mean_c(const SkimHdGovernor *g);

/* The RTTY decoders run now (always TRUE without rtty_adaptive). */
gboolean skim_hd_governor_rtty_on(const SkimHdGovernor *g);

/* Its state for status.json: ,"rtty_adaptive":{…} and ,"thermal":{…}
 * appended — each only while on. */
void skim_hd_governor_json(const SkimHdGovernor *g, GString *out);

G_END_DECLS
