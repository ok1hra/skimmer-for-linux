/* osd_stub.c — NOT upstream: the skimmer's own stand-in for WSJT-X's
 * Fortran OSD decoder (osdwspr.f90), so wsprd builds with C alone.
 *
 * wsprd calls osdwspr_() only with "-o N" (its default depth is -1 =
 * disabled), and skimmer-wsprd is never started with -o. Reached anyway,
 * it says so and reports no codeword (dmin huge: wsprd rejects it).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <stdio.h>
#include <string.h>

void osdwspr_(float ss[], unsigned char apmask[], int *ndeep, unsigned char cw[], int *nhardmin,
              float *dmin) {
  (void)ss; (void)apmask; (void)ndeep;
  static int told;
  if (!told) { fprintf(stderr, "skimmer-wsprd: built without the OSD decoder (-o ignored)\n"); told = 1; }
  memset(cw, 0, 162);
  *nhardmin = 162;
  *dmin = 1e30f;
}
