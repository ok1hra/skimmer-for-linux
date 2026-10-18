/* fftw_lock.c — see fftw_lock.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "fftw_lock.h"

static GMutex s_fftw_planner;

void skim_fftw_lock(void)   { g_mutex_lock(&s_fftw_planner); }
void skim_fftw_unlock(void) { g_mutex_unlock(&s_fftw_planner); }
