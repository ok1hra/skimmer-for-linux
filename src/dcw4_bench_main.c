/* dcw4_bench_main.c — skimmer-dcw4-bench: what CW v4's network costs on
 * this machine. Runs windows of a keyed tile (W s, default 6: CW v4's
 * left context 1 s + freeze guard 2 s + step 3 s) through the built-in net
 * on every encoder this CPU has, on 1 and on N threads at once, and says
 * per encoder:
 *   - ms a window, windows a second per thread and for all N;
 *   - keyed channels carried in real time (one window per channel every
 *     step S s): windows/s × S;
 *   - CPU per keyed channel and second of audio (ms/s).
 * The network only — the channel's DSP and the stitcher add a few per cent
 * (SDRIAK: inference is ~97 % of a keyed channel's cost).
 *
 *   skimmer-dcw4-bench [--secs 3] [--threads N] [--win 6] [--step 3] [--isa auto|avx2|sse|generic]
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "engine/dcw4_kernel.h"

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const SkimDcw4Net *net;
  SkimDcw4Isa        isa;
  const float       *tile;
  guint              T;
  gint64             until;     /* monotonic µs */
  guint              windows;
  double             cpu_s;
} Job;

static double thread_cpu_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static gpointer worker(gpointer p) {
  Job *j = p;
  float *logp = g_new(float, (gsize)skim_dcw4_output_frames(j->T) * SKIM_DCW4_CLASSES);
  skim_dcw4_net_run(j->net, j->tile, j->T, logp, j->isa);      /* warm the scratch */
  const double c0 = thread_cpu_s();
  while (g_get_monotonic_time() < j->until) {
    skim_dcw4_net_run(j->net, j->tile, j->T, logp, j->isa);
    j->windows++;
  }
  j->cpu_s = thread_cpu_s() - c0;
  g_free(logp);
  return NULL;
}

/* windows a second on n threads for secs; *cpu_ms: CPU ms a window */
static double run(const SkimDcw4Net *net, SkimDcw4Isa isa, const float *tile, guint T, guint n,
                  double secs, double *cpu_ms) {
  Job *jobs = g_new0(Job, n);
  GThread **th = g_new(GThread *, n);
  const gint64 t0 = g_get_monotonic_time();
  for (guint i = 0; i < n; i++) {
    jobs[i] = (Job){ net, isa, tile, T, t0 + (gint64)(secs * 1e6), 0, 0 };
    th[i] = g_thread_new("dcw4-bench", worker, &jobs[i]);
  }
  guint w = 0;
  double cpu = 0;
  for (guint i = 0; i < n; i++) {
    g_thread_join(th[i]);
    w += jobs[i].windows;
    cpu += jobs[i].cpu_s;
  }
  const double wall = (g_get_monotonic_time() - t0) * 1e-6;
  *cpu_ms = w ? cpu * 1e3 / w : 0;
  g_free(jobs);
  g_free(th);
  return w / wall;
}

int main(int argc, char **argv) {
  double secs = 3, win = 6, step = 3;
  int threads = (int)g_get_num_processors();
  char *isa_name = NULL;
  GOptionEntry opts[] = {
    { "secs", 0, 0, G_OPTION_ARG_DOUBLE, &secs, "seconds per measurement (3)", "S" },
    { "threads", 0, 0, G_OPTION_ARG_INT, &threads, "threads at once (all CPUs)", "N" },
    { "win", 0, 0, G_OPTION_ARG_DOUBLE, &win, "window, s (6 = L1 + G2 + S3)", "W" },
    { "step", 0, 0, G_OPTION_ARG_DOUBLE, &step, "a channel's window every S s (3)", "S" },
    { "isa", 0, 0, G_OPTION_ARG_STRING, &isa_name, "one encoder only", "auto|avx2|sse|generic" },
    { NULL, 0, 0, 0, NULL, NULL, NULL },
  };
  GOptionContext *ctx = g_option_context_new("— CW v4's network on this CPU");
  g_option_context_add_main_entries(ctx, opts, NULL);
  GError *err = NULL;
  if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
    fprintf(stderr, "%s\n", err->message);
    return 2;
  }
  g_option_context_free(ctx);
  SkimDcw4Isa only = SKIM_DCW4_ISA_AUTO;
  if (isa_name && !skim_dcw4_isa_from_name(isa_name, &only)) {
    fprintf(stderr, "--isa %s: not auto, avx2, sse or generic\n", isa_name);
    return 2;
  }
  threads = CLAMP(threads, 1, 256);
  SkimDcw4Net *net = skim_dcw4_net_new_builtin(SKIM_DCW4_ISA_AUTO, &err);
  if (!net) {
    fprintf(stderr, "the built-in net: %s\n", err->message);
    return 1;
  }

  /* a keyed tile: dits and dahs at ~25 WPM over noise, normalised */
  const guint T = (guint)lround(win * 62.5);
  float *tile = g_new(float, (gsize)T * 3);
  guint64 r = 88172645463325252ull;
  for (guint t = 0; t < T; t++) {
    const gboolean on = (t / 3) % 4 != 3 && (t / 12) % 3 != 2;
    for (guint b = 0; b < 3; b++) {
      r ^= r << 13; r ^= r >> 7; r ^= r << 17;
      const double noise = (double)(r >> 11) / 9007199254740992.0;
      tile[t * 3 + b] = (float)(noise + (on && b == 1 ? 4.0 : on ? 2.0 : 0.0));
    }
  }
  skim_dcw4_normalize(tile, (gsize)T * 3);

  char *cpu = NULL;
  if (g_file_get_contents("/proc/cpuinfo", &cpu, NULL, NULL)) {
    char *m = strstr(cpu, "model name");
    char *e = m ? strchr(m, '\n') : NULL;
    if (m && e) { printf("CPU: %.*s, %u threads\n", (int)(e - m - 13), m + 13, g_get_num_processors()); }
    g_free(cpu);
  }
  printf("window %.1f s (%u frames), a channel's window every %.1f s, %.0f s a measurement\n\n",
         win, T, step, secs);
  printf("%-8s %9s %10s | %3s %10s %11s %13s\n", "encoder", "ms/window", "windows/s", "N",
         "windows/s", "channels", "ms CPU per s");
  SkimDcw4Isa have[3];
  const guint n = skim_dcw4_isas_available(have);
  for (guint i = 0; i < n; i++) {
    if (only != SKIM_DCW4_ISA_AUTO && have[i] != only) { continue; }
    double c1, cn;
    const double w1 = run(net, have[i], tile, T, 1, secs, &c1);
    const double wn = run(net, have[i], tile, T, (guint)threads, secs, &cn);
    printf("%-8s %9.2f %10.0f | %3d %10.0f %11.0f %13.2f\n", skim_dcw4_isa_name(have[i]), c1,
           w1, threads, wn, wn * step, cn / step);
  }
  printf("\nchannels = keyed channels the N threads carry in real time (windows/s × step);\n"
         "ms CPU per s = one keyed channel's network cost per second of audio, at N threads.\n");
  g_free(tile);
  skim_dcw4_net_free(net);
  return 0;
}
