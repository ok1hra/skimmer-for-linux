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
} SkimHdGovernorConfig;


/* now_min: the unix minute of the start (the one running now). */
SkimHdGovernor *skim_hd_governor_new(const SkimHdGovernorConfig *cfg, gint64 now_min);
void            skim_hd_governor_free(SkimHdGovernor *g);

/* Minute `minute` (unix) just ended; rtty_spots: the RTTY spots all the
 * bands decoded here sent in it. TRUE when the RTTY decoders' state changed. */
gboolean skim_hd_governor_minute(SkimHdGovernor *g, gint64 minute, guint rtty_spots);

/* The RTTY decoders run now (always TRUE without rtty_adaptive). */
gboolean skim_hd_governor_rtty_on(const SkimHdGovernor *g);

/* Its state for status.json: "rtty_adaptive":{…} appended (nothing while
 * off), with a leading comma. */
void skim_hd_governor_json(const SkimHdGovernor *g, GString *out);

G_END_DECLS
