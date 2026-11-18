/*
 * skimmer-waterfall-test — the waterfall's DSP (src/headless/waterfall.h).
 *
 * A tone at a known offset and level lands in its pixel at its level (the
 * full band and zoomed in), the half-overlapping FFTs come at the hop, the
 * spectrum is in true orientation (a tone below the centre on the left),
 * and a window outside the band is empty.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "headless/waterfall.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

typedef struct { float *acc; guint n, frames; } Acc;

static void on_frame(const float *p, guint n, gpointer user) {
  Acc *a = user;
  for (guint k = 0; k < n; k++) { a->acc[k] += p[k]; }
  a->frames++;
}

static guint argmax(const guint8 *v, guint n) {
  guint m = 0;
  for (guint i = 1; i < n; i++) { if (v[i] > v[m]) { m = i; } }
  return m;
}

int main(void) {
  const double rate = 96000, centre = 14045500;
  const guint N = skim_hd_wf_fft_size(rate);
  printf("== FFT size\n");
  check("8192 at 96 kHz, 16384 at 192 kHz",
        N == 8192 && skim_hd_wf_fft_size(192000) == 16384);

  /* two tones: +12 kHz at -40 dBFS, -30 kHz at -60 dBFS; a little noise */
  const guint frames = 96000;                     /* one second            */
  float *iq = g_new(float, 2 * frames);
  GRand *r = g_rand_new_with_seed(7);
  for (guint i = 0; i < frames; i++) {
    const double t = i / rate;
    const double a1 = 0.01, a2 = 0.001;
    iq[2 * i] = (float)(a1 * cos(2 * G_PI * 12000 * t) + a2 * cos(2 * G_PI * -30000 * t) +
                        1e-6 * g_rand_double_range(r, -1, 1));
    iq[2 * i + 1] = (float)(a1 * sin(2 * G_PI * 12000 * t) + a2 * sin(2 * G_PI * -30000 * t) +
                            1e-6 * g_rand_double_range(r, -1, 1));
  }
  g_rand_free(r);
  SkimWfAnalyser *an = skim_wf_analyser_new(N);
  Acc acc = { g_new0(float, N), N, 0 };
  /* fed in odd-sized blocks, as a source does */
  for (guint off = 0; off < frames;) {
    const guint n = MIN(1023u, frames - off);
    skim_wf_analyser_feed(an, iq + 2 * off, n, on_frame, &acc);
    off += n;
  }
  for (guint k = 0; k < N; k++) { acc.acc[k] /= acc.frames; }
  printf("== the frames\n");
  check("half-overlapping: (96000 - 8192) / 4096 + 1 = 22 frames in a second", acc.frames == 22);

  printf("== the whole band, 1024 px\n");
  guint8 row[1024];
  skim_hd_wf_render(acc.acc, N, rate, centre, centre - rate / 2, centre + rate / 2, 1024, row);
  const guint p = argmax(row, 1024);
  /* +12 kHz: (12000 + 48000) / 96000 × 1024 = 640 */
  check("the +12 kHz tone in pixel 640 (true orientation)", p == 640);
  char what[96];
  g_snprintf(what, sizeof(what), "… at -40 dBFS: byte %u ≈ (−40 + 160) × 2 = 240", row[p]);
  check(what, abs((int)row[p] - 240) <= 2);
  /* -30 kHz: (−30000 + 48000) / 96000 × 1024 = 192 */
  g_snprintf(what, sizeof(what), "the −30 kHz tone in pixel 192 at -60 dBFS (byte %u ≈ 200)", row[192]);
  check(what, abs((int)row[192] - 200) <= 2 && row[191] < row[192] && row[193] < row[192]);
  check("the floor far from both well below them", row[400] < 120 && row[900] < 120);

  printf("== zoomed in: 1 kHz around the +12 kHz tone, 100 px\n");
  guint8 z[100];
  skim_hd_wf_render(acc.acc, N, rate, centre, centre + 11500, centre + 12500, 100, z);
  const guint zp = argmax(z, 100);
  check("the tone in the middle (pixel 49 or 50), a bin wide", zp == 49 || zp == 50);
  check("… at the same level", abs((int)z[zp] - 240) <= 2);
  check("… and 300 Hz off it, down by more than 60 dB", z[20] < z[zp] - 120);

  printf("== outside the band\n");
  guint8 o[64];
  skim_hd_wf_render(acc.acc, N, rate, centre, centre + 60000, centre + 70000, 64, o);
  gboolean zero = TRUE;
  for (guint i = 0; i < 64; i++) { zero = zero && o[i] == 0; }
  check("a window past the band's edge is all 0", zero);

  skim_wf_analyser_free(an);
  g_free(acc.acc);
  g_free(iq);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
