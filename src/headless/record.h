/* record.h — [record]: a band's raw IQ to disk while it decodes.
 *
 * The receiver thread copies each block into a queue; a writer thread puts
 * it on disk — a slow disk never stalls the radio. Exactly the samples the
 * band's pipeline gets, so a replay of the file (skimmer-replay) is the live
 * input. The file is <dir>/hd-<band>-<UTC>.cf32 with a .meta sidecar.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _Rec Rec;

/* A recording of band into dir, minutes long, from at_us (real time; 0 =
 * at once); label and ppm go into the .meta. */
Rec *skim_rec_new(const char *dir, const char *band, const char *label, double ppm,
                  gint64 at_us, double minutes);

/* Receiver thread: one block (starts at at_us, stops after minutes). */
void skim_rec_push(Rec *r, const float *iq, guint n, double rate, double centre);

/* A phased band (docs/PHASING.md): before the start, antenna B is recorded
 * beside A — <dir>/hd-<band>-<UTC>-b.cf32, sample for sample (silence while
 * B is missing), each .meta naming the other (pair_b: / pair_a:), so
 * skimmer-replay and skimmer-phase-bench find the pair from A's file. */
void skim_rec_set_paired(Rec *r);
void skim_rec_push2(Rec *r, const float *iq, const float *iq_b, guint n, double rate,
                    double centre);

/* Teardown: an unfinished recording is closed with what it has. */
void skim_rec_free(Rec *r);

G_END_DECLS
