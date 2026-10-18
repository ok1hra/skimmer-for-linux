/* fftw_lock.h — one process-wide lock around FFTW plan creation/destruction.
 *
 * FFTW's planner is NOT thread-safe: fftw(f)_plan_* and fftw(f)_destroy_plan
 * share global planner state; only fftw(f)_execute on distinct plans may run
 * concurrently. One pipeline never noticed. Six (skimmer-headless, one per
 * band) build their channel banks on six engine threads at the first IQ
 * block — and crashed inside fftwf's planner (2026-09-29). Every plan
 * create/destroy in the engine takes this lock.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_FFTW_LOCK_H
#define SKIMMER_FFTW_LOCK_H

#include <glib.h>

G_BEGIN_DECLS

void skim_fftw_lock(void);
void skim_fftw_unlock(void);

G_END_DECLS

#endif /* SKIMMER_FFTW_LOCK_H */
