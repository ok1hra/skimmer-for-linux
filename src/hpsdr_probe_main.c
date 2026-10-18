/*
 * skimmer-hpsdr-probe — live gate against an HPSDR Protocol 1 receiver
 * (Red Pitaya sdr_receiver_hpsdr).
 *
 *   skimmer-hpsdr-probe <host> [rate] [center_hz] [seconds] [dump.cf32] [ppm]
 *     host       the radio (e.g. 192.168.1.21); port 1024
 *     rate       48/96/192[000] (default 192000)
 *     center_hz  DDC centre (default 14080000)
 *     seconds    capture length (default 10)
 *     dump       optional: append raw IQ (cf32 interleaved, as delivered) to
 *                this file + a <dump>.meta sidecar (center_hz:, rate_hz:) —
 *                skimmer-replay and SKIM_IQ_FILE read it unchanged
 *     ppm        sampling-clock correction (default 0)
 *
 * Prints the discovery reply, then streams N seconds and reports packets,
 * lost (zero-filled) frames, effective vs. nominal rate, level and the
 * strongest spectrum peaks with ABSOLUTE frequencies. Orientation check:
 * run twice with the centre moved by a few kHz — a real station keeps its
 * absolute frequency, a mirrored spectrum moves it by twice the step.
 *
 * NEVER run this against a radio another client is using: a start takes
 * its stream over (the probe refuses a busy radio).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/hpsdr_p1.h"

#define FFT_N     8192
#define SKIP_BLKS 4

static GMutex  p_lock;
static guint64 p_frames;
static guint   p_blocks;
static gint64  p_t_first, p_t_last;
static double  p_sum_p2;
static float   p_peak;
static float   p_fft_buf[FFT_N * 2];
static guint   p_fft_fill;
static FILE   *p_dump;

static void iq_cb(const float *iq, guint nframes, double rate, double center,
                  gpointer user) {
  (void)rate; (void)center; (void)user;
  const gint64 now = g_get_monotonic_time();
  if (p_dump) { fwrite(iq, 2 * sizeof(float), nframes, p_dump); }
  g_mutex_lock(&p_lock);
  if (!p_blocks) { p_t_first = now; }
  p_t_last = now;
  p_blocks++;
  p_frames += nframes;
  for (guint i = 0; i < nframes; i++) {
    const float vi = iq[2 * i], vq = iq[2 * i + 1];
    p_sum_p2 += (double)vi * vi + (double)vq * vq;
    const float a = MAX(fabsf(vi), fabsf(vq));
    if (a > p_peak) { p_peak = a; }
  }
  if (p_blocks > SKIP_BLKS && p_fft_fill < FFT_N) {
    const guint take = MIN(nframes, FFT_N - p_fft_fill);
    memcpy(p_fft_buf + 2 * p_fft_fill, iq, take * 2 * sizeof(float));
    p_fft_fill += take;
  }
  g_mutex_unlock(&p_lock);
}

static void fft(double *re, double *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) { j ^= bit; }
    j |= bit;
    if (i < j) {
      double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * G_PI / len;
    const double wr = cos(ang), wi = sin(ang);
    for (int i = 0; i < n; i += len) {
      double cr = 1.0, ci = 0.0;
      for (int k = 0; k < len / 2; k++) {
        const int a = i + k, b = i + k + len / 2;
        const double tr = re[b] * cr - im[b] * ci;
        const double ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        const double ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <host> [rate] [center_hz] [seconds] [dump.cf32] [ppm]\n",
            argv[0]);
    return 2;
  }
  const char *host   = argv[1];
  guint       rate   = argc > 2 ? (guint)atoi(argv[2]) : 192000;
  double      center = argc > 3 ? g_ascii_strtod(argv[3], NULL) : 14080000;
  int         secs   = argc > 4 ? atoi(argv[4]) : 10;
  const char *dump   = argc > 5 && argv[5][0] ? argv[5] : NULL;
  double      ppm    = argc > 6 ? g_ascii_strtod(argv[6], NULL) : 0;
  if (rate < 1000) { rate *= 1000; }

  GError *err = NULL;
  SkimHpsdrInfo info;
  if (!skim_hpsdr_discover(host, SKIM_HPSDR_DEFAULT_PORT, 1000, &info, &err)) {
    printf("FAIL — %s\n", err->message);
    g_clear_error(&err);
    return 1;
  }
  printf("=== skimmer-hpsdr-probe — %s:%u ===\n", host, SKIM_HPSDR_DEFAULT_PORT);
  printf("discovery: \"%s\" mac %02x:%02x:%02x:%02x:%02x:%02x code %u board %u, "
         "%u RX, %s\n", info.name, info.mac[0], info.mac[1], info.mac[2],
         info.mac[3], info.mac[4], info.mac[5], info.code_version,
         info.board_id, info.nrx, info.busy ? "BUSY" : "idle");
  if (info.busy) {
    printf("FAIL — the radio streams to another client; not taking it over\n");
    return 1;
  }

  if (dump) {
    p_dump = fopen(dump, "ab");
    if (!p_dump) {
      printf("FAIL — cannot open dump file %s\n", dump);
      return 1;
    }
  }
  GDateTime *t_start = g_date_time_new_now_local();

  SkimHpsdrClient *c = skim_hpsdr_client_new(host, SKIM_HPSDR_DEFAULT_PORT);
  skim_hpsdr_client_set_iq_cb(c, iq_cb, NULL);
  if (!skim_hpsdr_client_start(c, rate, center, ppm, FALSE, &err)) {
    printf("FAIL — %s\n", err->message);
    g_clear_error(&err);
    skim_hpsdr_client_free(c);
    return 1;
  }
  printf("streaming %u Hz IQ at %.0f Hz (word %u Hz, %+.2f ppm), %d s\n",
         rate, center, skim_hpsdr_client_freq_word(c), ppm, secs);

  for (int s = 0; s < secs; s++) {
    g_usleep(G_USEC_PER_SEC);
    printf("  %2d s  %" G_GUINT64_FORMAT " packets, %" G_GUINT64_FORMAT
           " lost frames\r", s + 1, skim_hpsdr_client_packets(c),
           skim_hpsdr_client_lost_frames(c));
    fflush(stdout);
  }
  printf("\n");
  const guint64 packets = skim_hpsdr_client_packets(c);
  const guint64 lost = skim_hpsdr_client_lost_frames(c);
  skim_hpsdr_client_stop(c);

  SkimHpsdrInfo after;
  if (skim_hpsdr_discover(host, SKIM_HPSDR_DEFAULT_PORT, 1000, &after, NULL)) {
    printf("after stop: %s\n", after.busy ? "BUSY (stop did not land!)" : "idle");
  }

  g_mutex_lock(&p_lock);
  const guint64 frames = p_frames;
  const double span_s = (double)(p_t_last - p_t_first) / G_USEC_PER_SEC;
  const double rms = frames ? sqrt(p_sum_p2 / (double)frames) : 0;
  const float peak = p_peak;
  const guint blocks = p_blocks;
  g_mutex_unlock(&p_lock);

  const double eff = span_s > 0 && blocks > 1
                         ? (double)(frames - frames / blocks) / span_s : 0;
  const double dev = (eff - rate) / rate * 100.0;
  printf("\nIQ stats:\n");
  printf("  packets %" G_GUINT64_FORMAT ", frames %" G_GUINT64_FORMAT
         ", lost (zero-filled) %" G_GUINT64_FORMAT " = %.3f %%\n",
         packets, frames, lost, frames ? 100.0 * (double)lost / (double)frames : 0);
  printf("  effective rate %.1f Hz (%+.3f %% vs %u) — %s\n", eff, dev, rate,
         fabs(dev) < 2.0 ? "ok" : "OUT OF TOLERANCE");
  printf("  RMS %.1f dBFS, peak %.4f\n", rms > 0 ? 20.0 * log10(rms) : -999.0, peak);

  if (p_dump) {
    fclose(p_dump);
    char *meta_path = g_strdup_printf("%s.meta", dump);
    FILE *meta = fopen(meta_path, "a");
    if (meta) {
      char *ts = g_date_time_format(t_start, "%Y-%m-%d %H:%M:%S %z");
      fprintf(meta,
              "file: %s\nstart: %s\nsource: hpsdr-p1://%s:%u (%s)\n"
              "format: cf32 interleaved I,Q — TRUE spectrum orientation\n"
              "center_hz: %.0f\nrate_hz: %u\nclock_ppm: %.3f\nframes: %"
              G_GUINT64_FORMAT "\nlost_frames: %" G_GUINT64_FORMAT
              "\nduration_s: %.1f\n---\n",
              dump, ts, host, SKIM_HPSDR_DEFAULT_PORT, info.name, center, rate,
              ppm, frames, lost, span_s);
      fclose(meta);
      g_free(ts);
      printf("  dump closed, sidecar %s written\n", meta_path);
    }
    g_free(meta_path);
  }
  g_date_time_unref(t_start);

  if (p_fft_fill == FFT_N) {
    static double re[FFT_N], im[FFT_N], db[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
      const double w = 0.5 * (1.0 - cos(2.0 * G_PI * i / (FFT_N - 1)));
      re[i] = p_fft_buf[2 * i] * w;
      im[i] = p_fft_buf[2 * i + 1] * w;
    }
    fft(re, im, FFT_N);
    double maxdb = -999.0;
    for (int k = 0; k < FFT_N; k++) {
      db[k] = 10.0 * log10(re[k] * re[k] + im[k] * im[k] + 1e-30);
      if (db[k] > maxdb) { maxdb = db[k]; }
    }
    printf("\nspectrum peaks (absolute = centre + offset):\n");
    gboolean used[FFT_N] = { FALSE };
    for (int p = 0; p < 10; p++) {
      int best = -1;
      for (int k = 0; k < FFT_N; k++) {
        /* skip DC ± 2 bins and the outer 5 % (anti-alias skirts) */
        const int s = k <= FFT_N / 2 ? k : k - FFT_N;
        if (abs(s) < 3 || abs(s) > FFT_N * 45 / 100) { continue; }
        if (!used[k] && (best < 0 || db[k] > db[best])) { best = k; }
      }
      if (best < 0 || db[best] < maxdb - 70.0) { break; }
      for (int k = best - 8; k <= best + 8; k++) { used[(k + FFT_N) % FFT_N] = TRUE; }
      const int s = best <= FFT_N / 2 ? best : best - FFT_N;
      const double off = (double)s * rate / FFT_N;
      printf("  %+9.0f Hz  %12.0f Hz  %6.1f dB\n", off, center + off,
             db[best] - maxdb);
    }
  }

  skim_hpsdr_client_free(c);
  const gboolean pass = fabs(dev) < 2.0 && frames > 0;
  printf("\n%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
