/* band_combine.c — see band_combine.h.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "band_combine.h"

#include <fftw3.h>
#include <math.h>
#include <string.h>

#include "fftw_lock.h"

struct _SkimBandCombine {
  double         rate;
  guint          N, H;          /* FFT, hop (N/2)                          */
  float         *win;           /* √Hann (periodic)                        */
  float         *wa, *wb;       /* per bin: ca, cb ({re, im})              */
  float         *ina, *inb;     /* the last N frames of A, B               */
  guint          fill;          /* new frames since the last hop           */
  float         *ola;           /* overlap-add accumulator (N frames)      */
  float         *outq;          /* frames ready to go out                  */
  guint          outn, outcap;
  fftwf_complex *fa, *fb, *fy;
  fftwf_plan     pa, pb, py;
};

SkimBandCombine *skim_band_combine_new(double rate, guint nfft) {
  if (rate <= 0)
    return NULL;
  if (nfft == 0) { nfft = rate <= 96000 ? 2048 : (guint)(rate / 48); }
  nfft &= ~1u;
  if (nfft < 64)
    return NULL;
  SkimBandCombine *bc = g_new0(SkimBandCombine, 1);
  bc->rate = rate;
  bc->N = nfft;
  bc->H = nfft / 2;
  bc->win = g_new(float, nfft);
  for (guint n = 0; n < nfft; n++) { bc->win[n] = (float)sin(G_PI * n / nfft); }
  bc->wa = g_new0(float, 2 * nfft);
  bc->wb = g_new0(float, 2 * nfft);
  for (guint k = 0; k < nfft; k++) { bc->wa[2 * k] = 1.0f; }  /* A until told */
  bc->ina = g_new0(float, 2 * nfft);
  bc->inb = g_new0(float, 2 * nfft);
  bc->ola = g_new0(float, 2 * nfft);
  bc->outcap = 4 * nfft;
  bc->outq = g_new0(float, 2 * bc->outcap);
  bc->fa = fftwf_malloc(sizeof(fftwf_complex) * nfft);
  bc->fb = fftwf_malloc(sizeof(fftwf_complex) * nfft);
  bc->fy = fftwf_malloc(sizeof(fftwf_complex) * nfft);
  skim_fftw_lock();
  bc->pa = fftwf_plan_dft_1d((int)nfft, bc->fa, bc->fa, FFTW_FORWARD, FFTW_ESTIMATE);
  bc->pb = fftwf_plan_dft_1d((int)nfft, bc->fb, bc->fb, FFTW_FORWARD, FFTW_ESTIMATE);
  bc->py = fftwf_plan_dft_1d((int)nfft, bc->fy, bc->fy, FFTW_BACKWARD, FFTW_ESTIMATE);
  skim_fftw_unlock();
  skim_band_combine_reset(bc);
  return bc;
}

void skim_band_combine_free(SkimBandCombine *bc) {
  if (!bc)
    return;
  skim_fftw_lock();
  fftwf_destroy_plan(bc->pa);
  fftwf_destroy_plan(bc->pb);
  fftwf_destroy_plan(bc->py);
  skim_fftw_unlock();
  fftwf_free(bc->fa);
  fftwf_free(bc->fb);
  fftwf_free(bc->fy);
  g_free(bc->win);
  g_free(bc->wa);
  g_free(bc->wb);
  g_free(bc->ina);
  g_free(bc->inb);
  g_free(bc->ola);
  g_free(bc->outq);
  g_free(bc);
}

void skim_band_combine_reset(SkimBandCombine *bc) {
  memset(bc->ina, 0, 2 * bc->N * sizeof(float));
  memset(bc->inb, 0, 2 * bc->N * sizeof(float));
  memset(bc->ola, 0, 2 * bc->N * sizeof(float));
  bc->fill = 0;
  /* H frames of silence in hand: every call can give out as many as it
   * takes in (the queue never runs dry between hops) */
  memset(bc->outq, 0, 2 * bc->H * sizeof(float));
  bc->outn = bc->H;
}

void skim_band_combine_set_weights(SkimBandCombine *bc, const float *ca, const float *cb,
                                   guint n, double first_hz, double spacing_hz) {
  if (!bc || n == 0 || spacing_hz <= 0)
    return;
  for (guint k = 0; k < bc->N; k++) {
    const double f = (k < bc->N / 2 ? (double)k : (double)k - bc->N) * bc->rate / bc->N;
    double x = (f - first_hz) / spacing_hz;
    if (x < 0) { x = 0; }
    if (x > n - 1) { x = n - 1; }
    const guint i0 = (guint)floor(x), i1 = MIN(i0 + 1, n - 1);
    const float t = (float)(x - i0);
    for (int c = 0; c < 2; c++) {
      bc->wa[2 * k + c] = (1 - t) * ca[2 * i0 + c] + t * ca[2 * i1 + c];
      bc->wb[2 * k + c] = (1 - t) * cb[2 * i0 + c] + t * cb[2 * i1 + c];
    }
  }
}

/* One hop: the last N frames → H frames of the combination queued. */
static void hop(SkimBandCombine *bc) {
  const guint N = bc->N, H = bc->H;
  for (guint n = 0; n < N; n++) {
    const float w = bc->win[n];
    bc->fa[n][0] = bc->ina[2 * n] * w;
    bc->fa[n][1] = bc->ina[2 * n + 1] * w;
    bc->fb[n][0] = bc->inb[2 * n] * w;
    bc->fb[n][1] = bc->inb[2 * n + 1] * w;
  }
  fftwf_execute(bc->pa);
  fftwf_execute(bc->pb);
  for (guint k = 0; k < N; k++) {
    const float ar = bc->fa[k][0], ai = bc->fa[k][1], br = bc->fb[k][0], bi = bc->fb[k][1];
    const float car = bc->wa[2 * k], cai = bc->wa[2 * k + 1];
    const float cbr = bc->wb[2 * k], cbi = bc->wb[2 * k + 1];
    bc->fy[k][0] = car * ar - cai * ai + cbr * br - cbi * bi;
    bc->fy[k][1] = car * ai + cai * ar + cbr * bi + cbi * br;
  }
  fftwf_execute(bc->py);
  const float scale = 1.0f / N;
  for (guint n = 0; n < N; n++) {
    const float w = bc->win[n] * scale;
    bc->ola[2 * n] += bc->fy[n][0] * w;
    bc->ola[2 * n + 1] += bc->fy[n][1] * w;
  }
  /* the first H frames are complete: out, and the accumulator moves on */
  if (bc->outn + H > bc->outcap) {           /* never: the caller drains */
    bc->outn = 0;
  }
  memcpy(bc->outq + 2 * bc->outn, bc->ola, 2 * H * sizeof(float));
  bc->outn += H;
  memmove(bc->ola, bc->ola + 2 * H, 2 * H * sizeof(float));
  memset(bc->ola + 2 * H, 0, 2 * H * sizeof(float));
}

void skim_band_combine_process(SkimBandCombine *bc, const float *a, const float *b,
                               float *out, guint n) {
  const guint N = bc->N, H = bc->H;
  guint done = 0;
  while (done < n) {
    /* take frames until the next hop is due */
    const guint m = MIN(n - done, H - bc->fill);
    const guint keep = N - H + bc->fill;       /* frames already in the window */
    memcpy(bc->ina + 2 * keep, a + 2 * done, 2 * m * sizeof(float));
    memcpy(bc->inb + 2 * keep, b + 2 * done, 2 * m * sizeof(float));
    bc->fill += m;
    /* out goes as much as came in (out may alias a: a's part was copied) */
    memcpy(out + 2 * done, bc->outq, 2 * m * sizeof(float));
    memmove(bc->outq, bc->outq + 2 * m, 2 * (bc->outn - m) * sizeof(float));
    bc->outn -= m;
    done += m;
    if (bc->fill == H) {
      hop(bc);
      memmove(bc->ina, bc->ina + 2 * H, 2 * (N - H) * sizeof(float));
      memmove(bc->inb, bc->inb + 2 * H, 2 * (N - H) * sizeof(float));
      bc->fill = 0;
    }
  }
}
