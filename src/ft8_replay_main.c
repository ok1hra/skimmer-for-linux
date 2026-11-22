/* skimmer-ft8-replay — a recorded IQ file (cf32 + .meta, as skimmer-hpsdr-probe
 * and [record] write it) through the whole [ft8] (src/headless/ft8.c): the
 * DSP, the cycles on the recording's own clock, skimmer-ft8d. Prints every
 * decode, the spots it would send, and the decoder's CPU time — the bench on
 * real air, and with --keep DIR the cycles' WAVs to run WSJT-X's own jt9 on.
 * Faster than real time, yet every cycle is decoded (no skips).
 *
 *   skimmer-ft8-replay FILE.cf32 [--depth 1-3] [--threads N] [--no-ft4]
 *                      [--keep DIR] [--decoder PATH] [--start UNIX]
 *
 * The start is the .meta's start_unix (else its "start:" to the second);
 * --start overrides it.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "headless/ft8.h"

static guint nspots;
static GMutex lock;

static void spot_cb(const char *call, const char *mode, double freq_hz, int snr, gboolean cq,
                    const char *band, const char *grid, gint64 t, gpointer user) {
  (void)user; (void)t;
  g_mutex_lock(&lock);
  printf("SPOT %-12s %s %9.1f kHz %4d dB %s %s %s\n", call, mode, freq_hz / 1e3, snr, cq ? "CQ" : "DE",
         band, grid && *grid ? grid : "-");
  nspots++;
  g_mutex_unlock(&lock);
}

static gboolean meta(const char *path, double *centre, double *rate, double *start) {
  char *m = g_strconcat(path, ".meta", NULL), *text = NULL;
  const gboolean ok = g_file_get_contents(m, &text, NULL, NULL);
  g_free(m);
  if (!ok) { return FALSE; }
  char **lines = g_strsplit(text, "\n", -1);
  double su = 0;
  for (char **l = lines; *l; l++) {
    if (g_str_has_prefix(*l, "center_hz:")) { *centre = g_ascii_strtod(*l + 10, NULL); }
    if (g_str_has_prefix(*l, "rate_hz:")) { *rate = g_ascii_strtod(*l + 8, NULL); }
    if (g_str_has_prefix(*l, "start_unix:")) { su = g_ascii_strtod(*l + 11, NULL); }
    int Y, M, D, h, mi, s;
    if (g_str_has_prefix(*l, "start:") &&
        sscanf(*l + 6, " %d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &s) == 6) {
      int off = 0;
      const char *plus = strrchr(*l, '+'), *minus = strrchr(*l + 10, '-');
      const char *z = plus ? plus : minus;
      if (z && strlen(z) >= 5) {
        const int v = atoi(z + 1);
        off = (v / 100 * 3600 + v % 100 * 60) * (*z == '-' ? -1 : 1);
      }
      GDateTime *dt = g_date_time_new_utc(Y, M, D, h, mi, s);
      if (dt) {
        *start = (double)g_date_time_to_unix(dt) - off;
        g_date_time_unref(dt);
      }
    }
  }
  if (su > 0) { *start = su; }
  g_strfreev(lines);
  g_free(text);
  return *centre > 0 && *rate > 0 && *start > 0;
}

int main(int argc, char **argv) {
  const char *file = NULL, *keep = NULL, *decoder = NULL;
  guint depth = 2, threads = 2;
  gboolean ft4 = TRUE;
  double start = 0, start_arg = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--depth") && i + 1 < argc) { depth = (guint)atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--threads") && i + 1 < argc) { threads = (guint)atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--keep") && i + 1 < argc) { keep = argv[++i]; }
    else if (!strcmp(argv[i], "--decoder") && i + 1 < argc) { decoder = argv[++i]; }
    else if (!strcmp(argv[i], "--start") && i + 1 < argc) { start_arg = g_ascii_strtod(argv[++i], NULL); }
    else if (!strcmp(argv[i], "--no-ft4")) { ft4 = FALSE; }
    else if (!file) { file = argv[i]; }
  }
  double centre = 0, rate = 0;
  if (!file || !meta(file, &centre, &rate, &start)) {
    fprintf(stderr, "usage: %s FILE.cf32 [--depth 1-3] [--threads N] [--no-ft4] [--keep DIR] "
                    "[--decoder PATH] [--start UNIX]   (FILE.cf32.meta beside it)\n", argv[0]);
    return 2;
  }
  if (start_arg > 0) { start = start_arg; }
  FILE *in = fopen(file, "rb");
  if (!in) {
    perror(file);
    return 2;
  }
  char *dir = g_dir_make_tmp("skimmer-ft8-replay-XXXXXX", NULL);
  const char *names[] = { "rx" };
  const SkimHdFt8Config cfg = { .ft8 = TRUE, .ft4 = ft4, .depth = depth, .lo_hz = 200,
                                .hi_hz = 3500, .threads = threads, .dir = dir,
                                .decoder = decoder, .keep_wav = keep, .spot = spot_cb };
  SkimHdFt8 *f = skim_hd_ft8_new(&cfg, 1, names, &centre, rate);
  char *dials = skim_hd_ft8_dials_text(f);
  printf("# %s: centre %.1f kHz, %.0f kHz, start %.3f; dials %s; depth %u, %u threads\n", file,
         centre / 1e3, rate / 1e3, start, dials, depth, threads);
  g_free(dials);
  const guint blk = (guint)(rate / 10);                 /* 100 ms */
  float *iq = g_new(float, 2 * blk);
  guint64 frames = 0;
  const gint64 t0 = g_get_monotonic_time();
  gint64 dsp_us = 0;
  size_t got;
  /* faster than real time, but every cycle decoded: each 7.5 s of IQ waits
   * for the decoders (live, a cycle they are still behind on is skipped) */
  const guint64 step = (guint64)(rate * 7.5);
  guint64 next = step;
  while ((got = fread(iq, 2 * sizeof(float), blk, in)) > 0) {
    frames += got;
    const gint64 t_end = (gint64)llround((start + (double)(frames - 1) / rate) * 1e6);
    const gint64 a = g_get_monotonic_time();
    skim_hd_ft8_push_at(f, 0, iq, (guint)got, t_end);
    dsp_us += g_get_monotonic_time() - a;
    if (frames >= next) {
      skim_hd_ft8_drain(f);
      next += step;
    }
  }
  fclose(in);
  const double feed_s = dsp_us / 1e6;
  skim_hd_ft8_drain(f);
  const double wall_s = (g_get_monotonic_time() - t0) / 1e6;
  char *all = skim_hd_ft8_decodes_json(f, 0, 0);
  char *st = skim_hd_ft8_status_json(f);
  /* the decodes, one a line */
  for (char *p = strstr(all, "{\"t\":"); p; p = strstr(p + 1, "{\"t\":")) {
    char *e = strchr(p, '}');
    printf("DEC %.*s\n", (int)(e - p + 1), p);
  }
  struct rusage ru;
  getrusage(RUSAGE_CHILDREN, &ru);
  const double child = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec +
                       ru.ru_stime.tv_usec / 1e6;
  printf("# %.0f s of IQ: the DSP %.1f s, all %.1f s wall; skimmer-ft8d %.1f s CPU; %u spots\n",
         frames / rate, feed_s, wall_s, child, nspots);
  printf("# status %s\n", st);
  g_free(all);
  g_free(st);
  skim_hd_ft8_free(f);
  g_free(iq);
  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  if (system(rm) != 0) { fprintf(stderr, "could not remove %s\n", dir); }
  g_free(rm);
  g_free(dir);
  return 0;
}
