/* phase_hist.h — what phasing gave, kept in time (docs/PHASING.md,
 * "Measuring what it gives").
 *
 * A coarse waterfall per phased band: one row a minute for 7 days and one
 * per quarter hour for 90 days; in each row one byte a channel (the
 * channel = a pixel: skim_phase_q8, 0.25 dB a step, SKIM_PHASE_NONE = no
 * reading) for two planes — the gain over the better antenna where a
 * station was, and the floor drop (the interference taken out) — plus the
 * band's summary of the minute (the medians: gain, gain over A, drop and
 * its 90th percentile, coherence, D_max, B's share, stations). The values
 * are what the pipeline measured (or, in modes A, B and Fixed, what it
 * estimates the adaptive weight would give — the row says which).
 *
 * The file (<state>/phase-<band>.bin, ≈ 30 MB at 768 channels) is held
 * open and each row is written in place when its minute ends; a file of
 * another channel count starts afresh (the old one kept as .old).
 *
 * Threading: the owner's main loop only.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

#include "engine/pipeline.h"

G_BEGIN_DECLS

typedef struct _SkimPhaseHist SkimPhaseHist;

/* The history of band (its name) with nchan channels at spacing_hz, the
 * first first_hz from the centre; NULL when the file cannot be made. */
SkimPhaseHist *skim_phase_hist_open(const char *dir, const char *band, guint nchan,
                                    double first_hz, double spacing_hz);
void           skim_phase_hist_free(SkimPhaseHist *h);   /* writes the minute in hand */

/* Once a second: the snapshot and its rows (pipeline.h; gain, drop). The
 * minute it falls in is summed; a new minute writes the last one. */
void skim_phase_hist_add(SkimPhaseHist *h, gint64 now_s, const SkimPipelinePhase *ph,
                         const gint8 *gain, const gint8 *drop);

/* The rows of [end_s - hours·3600, end_s] as JSON for the page: one row
 * every step minutes (1 up to 7 days, 15 beyond; merged to at most
 * max_rows by averaging), oldest first; per plane the bytes as base64
 * (nchan a row, SKIM_PHASE_NONE where nothing was read), the summary as
 * arrays (null for none). */
char *skim_phase_hist_json(SkimPhaseHist *h, gint64 end_s, guint hours, guint max_rows);

G_END_DECLS
