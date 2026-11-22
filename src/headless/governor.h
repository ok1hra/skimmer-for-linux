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
 * reads 95 °C for seconds on a machine whose mean is 78 °C). The mean over
 * limit_c now and for 3 of the last 5 minutes (a dip under it does not
 * start the count again: a mean that hovers at the limit, mostly over it,
 * is hot — 2026-11-20 on hra2 it held one band off for 70 min at minute
 * peaks of 85–100 °C, the next never went): the band (decoded here, not by
 * a worker) with the fewest spots in the last 30 minutes is paused, CW and
 * RTTY; 10 minutes later the drop of the mean since is recorded with it —
 * and if the mean is still hot so the next one goes. One band always stays. A paused
 * band comes back last-in first-out once the mean has stayed at or under
 * limit − max(its drop, 5 °C) − restore_margin_c for 20 minutes, at most
 * one every 20 minutes: the reserve must exceed what the band itself costs,
 * so that it does not heat the CPU straight back over the limit.
 * "The fewest spots" is spots a minute over the minutes of the last 30 the
 * band decoded (one back from a pause 5 min ago is judged by those 5).
 *
 * The rotation (probe_every_min, probe_min): a paused band has no spots,
 * so it could never show that its band opened — on a machine that stays
 * hot for hours (hra2) the guard would keep the bands it paused in the
 * morning off all day. A band paused probe_every_min (30; since its pause
 * or its last test) is tested: on for probe_min (5) minutes in place of
 * the band decoding now with the fewest spots a minute (the same CPU). It
 * wins with more spots a minute than that band had before and 2 spots at
 * least: the two change places (the pause keeps its drop and its place in
 * the order of return); else each goes back. One test at a time, the band
 * waiting longest first; pausing and bringing back wait meanwhile. Five
 * minutes are enough: a band that is open spots in the first minute (hra2
 * 2026-11-22, after a restart: 80 m and 40 m 10+ in it), and the one it
 * would replace — the quietest — spots about 0–1 a minute.
 *
 * The v4 load guard ([decode] v4_guard, default on; CW v4 only): CW v4
 * runs its network on a fixed pool of threads, one window a keyed channel
 * every step; when the pool cannot keep up, a channel's next window comes
 * while its last one still waits, and that tick is skipped — late. Over
 * SKIM_GOV_LATE of a minute's ticks late, for 3 of the last 5 minutes:
 * the band running v4 with the fewest spots a minute (the same count as
 * the thermal guard's) is paused, 10 min later the next if still late;
 * one v4 band always stays (Dan, 2026-11-22: "jedno pásmo utáhne vždy").
 * A paused band comes back last-in first-out after 20 minutes without a
 * late minute, at most one every 20 minutes. Its pauses are its own — the
 * thermal guard's pauses and tests go on beside them, and neither picks a
 * band the other has paused.
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
  guint    probe_every_min;            /* a paused band tested after (0: never) */
  guint    probe_min;                  /* … for this long (0: never)            */
  gboolean v4_guard;                   /* the v4 load guard                     */
} SkimHdGovernorConfig;

/* the guard's clock (s): over the limit OVER_NEED_S of the last OVER_S
 * pauses a band, at least NEXT_S between two, the drop measured NEXT_S
 * after, the reserve held RESTORE_S brings one back (and as long between two) */
#define SKIM_GOV_OVER_S     300
#define SKIM_GOV_OVER_NEED_S 180
#define SKIM_GOV_NEXT_S     600
#define SKIM_GOV_RESTORE_S 1200
#define SKIM_GOV_MIN_DROP_C 5.0
#define SKIM_GOV_SPOTS_MIN  30          /* the spots that pick a band: minutes  */
#define SKIM_GOV_MAX_BANDS  16
/* the v4 load guard: a minute is late over this share of its ticks late */
#define SKIM_GOV_LATE       0.05

/* now_min: the unix minute of the start (the one running now). */
SkimHdGovernor *skim_hd_governor_new(const SkimHdGovernorConfig *cfg, gint64 now_min);
void            skim_hd_governor_free(SkimHdGovernor *g);

/* The bands, in receiver order; can[i]: the thermal guard may pause band i
 * (decoded here). Before the first minute. */
void skim_hd_governor_set_bands(SkimHdGovernor *g, guint n, const char *const *names,
                                const gboolean *can);

/* Minute `minute` (unix) just ended; rtty_spots: the RTTY spots all the
 * bands decoded here sent in it; band_spots: each band's spots, CW + RTTY
 * (NULL: none). TRUE when the RTTY decoders' state changed, or the
 * thermal guard's test of a paused band started or ended. */
gboolean skim_hd_governor_minute(SkimHdGovernor *g, gint64 minute, guint rtty_spots,
                                 const guint *band_spots);

/* Once a second: the hottest CPU sensor now (NAN: none). TRUE when a band
 * was paused or came back. */
gboolean skim_hd_governor_temp(SkimHdGovernor *g, gint64 now_s, double temp_c);

/* The thermal guard pauses band i now. */
gboolean skim_hd_governor_band_shed(const SkimHdGovernor *g, guint i);

/* Which bands decode CW with v4 here (the load guard's candidates). */
void skim_hd_governor_set_v4(SkimHdGovernor *g, const gboolean *v4);

/* Minute `minute` (unix) just ended: the share of its v4 ticks that came
 * late (NAN: none due). Call after skim_hd_governor_minute. TRUE when a
 * band was paused or came back. */
gboolean skim_hd_governor_load(SkimHdGovernor *g, gint64 minute, double late_share);

/* The v4 load guard pauses band i now. */
gboolean skim_hd_governor_band_load_shed(const SkimHdGovernor *g, guint i);

/* The mean the guard judges by (NAN: not yet, or the guard off). */
double skim_hd_governor_mean_c(const SkimHdGovernor *g);

/* The RTTY decoders run now (always TRUE without rtty_adaptive). */
gboolean skim_hd_governor_rtty_on(const SkimHdGovernor *g);

/* Its state for status.json: ,"rtty_adaptive":{…}, ,"thermal":{…} and
 * ,"v4_guard":{…} appended — each only while on. */
void skim_hd_governor_json(const SkimHdGovernor *g, GString *out);

G_END_DECLS
