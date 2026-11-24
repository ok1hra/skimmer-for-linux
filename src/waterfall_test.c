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

  printf("== the history's bytes render as the power does\n");
  {
    /* a history row: the averaged power as bytes, display order */
    guint8 *disp = g_new(guint8, N), hb[1024], hp[1024];
    for (guint d = 0; d < N; d++) {
      const float v = acc.acc[(d + N / 2) % N];
      const double db = v > 0 ? 10 * log10(v) : -300;
      disp[d] = (guint8)CLAMP(lround((db - SKIM_WF_DB_FLOOR) * 2), 0, 255);
    }
    gboolean same = TRUE;
    const double win[3][2] = { { -48000, 48000 }, { 11500, 12500 }, { -31000, -29000 } };
    for (int k = 0; k < 3; k++) {
      skim_hd_wf_render(acc.acc, N, rate, centre, centre + win[k][0], centre + win[k][1], 1024, hp);
      skim_hd_wf_render_bytes(disp, N, rate, centre, centre + win[k][0], centre + win[k][1], 1024, hb);
      for (guint i = 0; i < 1024; i++) { same = same && hb[i] == hp[i]; }
    }
    check("whole band, 1 kHz and 2 kHz windows: byte for byte the same picture", same);
    g_free(disp);
  }

  printf("== outside the band\n");
  guint8 o[64];
  skim_hd_wf_render(acc.acc, N, rate, centre, centre + 60000, centre + 70000, 64, o);
  gboolean zero = TRUE;
  for (guint i = 0; i < 64; i++) { zero = zero && o[i] == 0; }
  check("a window past the band's edge is all 0", zero);

  printf("== the audio: tuned on the +12 kHz tone, and 300 Hz off it\n");
  {
    /* µ-law back to linear, then the level at the BFO tone (Goertzel) */
    double lvl[2], rms[2];
    const double tune[2] = { 12000, 12300 };
    for (int k = 0; k < 2; k++) {
      SkimWfAudio *au = skim_wf_audio_new(rate, tune[k], 100);
      for (guint off = 0; off < frames;) {
        const guint n = MIN(1023u, frames - off);
        skim_wf_audio_feed(au, iq + 2 * off, n);
        off += n;
      }
      GByteArray *pcm = skim_wf_audio_take(au);
      const double lv = skim_wf_audio_level(au);
      if (k == 0) {
        g_snprintf(what, sizeof(what), "the S-meter's level on the -40 dBFS tone: %.1f dBFS", lv);
        check(what, lv > -41.5 && lv < -38.5);
      } else {
        /* the loudest 10 ms of the second: the filters' start is in it */
        g_snprintf(what, sizeof(what), "… 300 Hz off it, 100 Hz wide, 20 dB under it at least: %.1f dBFS", lv);
        check(what, lv < -60);
      }
      double s1 = 0, s2 = 0, e = 0;
      const double cf = 2 * cos(2 * G_PI * SKIM_WF_AUDIO_BFO / SKIM_WF_AUDIO_RATE);
      const guint skip = 2000;                    /* the filters and AGC settle */
      for (guint i = skip; i < pcm->len; i++) {
        const guint8 u = ~pcm->data[i];
        const int t = ((u & 0x0f) << 3) + 0x84, v = (t << ((u & 0x70) >> 4)) - 0x84;
        const double x = ((u & 0x80) ? -v : v) / 32768.0;
        const double s0 = x + cf * s1 - s2;
        s2 = s1; s1 = s0;
        e += x * x;
      }
      const guint n = pcm->len - skip;
      lvl[k] = sqrt(s1 * s1 + s2 * s2 - cf * s1 * s2) / n;
      rms[k] = sqrt(e / n);
      if (k == 0) {
        g_snprintf(what, sizeof(what), "8 kHz out: %u bytes a second (µ-law)", pcm->len);
        check(what, pcm->len >= 7900 && pcm->len <= 8000);
      }
      g_byte_array_unref(pcm);
      skim_wf_audio_free(au);
    }
    g_snprintf(what, sizeof(what), "tuned on it: a loud 600 Hz tone (rms %.2f, at 600 Hz %.2f)", rms[0], lvl[0]);
    check(what, rms[0] > 0.15 && lvl[0] > 0.6 * rms[0] / 1.4142);
    /* 300 Hz off with a 100 Hz passband: the AGC lifts the residue, so the
     * test is the tone at the BFO (600 Hz) falling far below the tuned case */
    g_snprintf(what, sizeof(what), "300 Hz off, 100 Hz wide: the 600 Hz tone gone (%.4f vs %.2f)", lvl[1], lvl[0]);
    check(what, lvl[1] < lvl[0] / 30);
  }

  skim_wf_analyser_free(an);
  g_free(acc.acc);
  g_free(iq);
  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
