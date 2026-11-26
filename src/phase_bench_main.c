/*
 * skimmer-phase-bench — what two-antenna phasing gives on a recorded pair
 * (docs/PHASING.md, P0): the same channelizer, combiner and meters as the
 * skimmer, run offline on antenna A's and antenna B's IQ.
 *
 *   skimmer-phase-bench A.cf32 B.cf32 [options]
 *     --rate HZ        the pair's rate (default: the .meta sidecar)
 *     --centre HZ      its centre (default: the .meta; labels only)
 *     --conj / --no-conj  conjugate on read (default: when the .meta says
 *                      the file is RF-inverted, as rp-spike writes it)
 *     --lag N          B is N samples late (default: measured, -8..8)
 *     --every S        a summary line every S seconds (default 10)
 *     --hold S         hold the weights after S seconds ("adapt, then fix")
 *     --csv FILE       every refresh, every channel with a number:
 *                      t,chan,freq_hz,mode,snr_a,snr_b,snr_y,gain,gain_a,
 *                      drop,gain_est,drop_est
 *
 * Reads .meta of either kind: the skimmer's [record] (rate_hz:, center_hz:,
 * TRUE orientation) and rp-spike's pair (rate=, center_hz=, RF-inverted).
 * MVDR and MRC run side by side on the same channels; mode A is the
 * reference both are measured against. The report: summary lines in time,
 * the noise groups (coherence, D_max, the floor drop each mode reaches),
 * the stations at the end (SNR_2500 on A, B and each combination), and the
 * CPU the second channelizer costs (how many bands can be phased).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "engine/channelizer.h"
#include "engine/combiner.h"
#include "engine/phase_meter.h"

#define SPACING 125.0
#define BLK     4096
#define MAXLAG  8

static double meta_num(const char *body, const char *k1, const char *k2) {
  for (guint i = 0; i < 2; i++) {
    const char *k = i ? k2 : k1;
    const char *hit = body ? strstr(body, k) : NULL;
    if (hit) { return g_ascii_strtod(hit + strlen(k), NULL); }
  }
  return 0;
}

static double cpu_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

/* B's lag against A (samples): the peak of |Σ a(n)·b*(n−d)| over the first
 * second, normalised. */
static int measure_lag(FILE *fa, FILE *fb, double rate, double *peak_out) {
  const guint n = (guint)MIN(rate, 1 << 20);
  float *a = g_new(float, 2 * n), *b = g_new(float, 2 * n);
  const size_t ga = fread(a, 2 * sizeof(float), n, fa);
  const size_t gb = fread(b, 2 * sizeof(float), n, fb);
  fseek(fa, 0, SEEK_SET);
  fseek(fb, 0, SEEK_SET);
  const guint m = (guint)MIN(ga, gb);
  int best = 0;
  double bestv = -1, pa = 0, pb = 0;
  for (guint i = 0; i < m; i++) {
    pa += (double)a[2 * i] * a[2 * i] + (double)a[2 * i + 1] * a[2 * i + 1];
    pb += (double)b[2 * i] * b[2 * i] + (double)b[2 * i + 1] * b[2 * i + 1];
  }
  for (int d = -MAXLAG; d <= MAXLAG; d++) {
    double re = 0, im = 0;
    for (guint i = MAXLAG; i + MAXLAG < m; i++) {
      const guint j = (guint)((int)i - d);
      re += (double)a[2 * i] * b[2 * j] + (double)a[2 * i + 1] * b[2 * j + 1];
      im += (double)a[2 * i + 1] * b[2 * j] - (double)a[2 * i] * b[2 * j + 1];
    }
    const double v = (re * re + im * im) / MAX(pa * pb, 1e-30);
    if (v > bestv) {
      bestv = v;
      best = d;
    }
  }
  g_free(a);
  g_free(b);
  *peak_out = bestv;
  return best;
}

typedef struct {
  const char      *name;
  SkimCombiner    *comb;
  SkimPhaseMeter  *met;
} Lane;

static void db(char *buf, gsize n, double v) {
  if (isnan(v)) { g_strlcpy(buf, "    —", n); } else { g_snprintf(buf, n, "%+5.1f", v); }
}

static void print_line(double t, const Lane *l) {
  SkimPhaseSummary s;
  skim_phase_meter_summary(l->met, &s);
  char g[16], ga[16], d[16], d9[16], ge[16], de[16];
  db(g, sizeof(g), s.gain_db);
  db(ga, sizeof(ga), s.gain_a_db);
  db(d, sizeof(d), s.drop_db);
  db(d9, sizeof(d9), s.drop_p90_db);
  db(ge, sizeof(ge), s.gain_est_db);
  db(de, sizeof(de), s.drop_est_db);
  printf("%7.0f  %-4s  %4u  %s  %s   %s  %s  %4.2f   %5.3f %5.1f   %4.2f  %4.2f   %s  %s\n",
         t, l->name, s.stations, g, ga, d, d9, isnan(s.drop_share) ? 0 : s.drop_share,
         isnan(s.coherence) ? 0 : s.coherence, isnan(s.dmax_db) ? 0 : s.dmax_db,
         isnan(s.share_b) ? 0 : s.share_b, isnan(s.same_dir) ? 0 : s.same_dir, ge, de);
}

int main(int argc, char **argv) {
  double rate = 0, centre = NAN, every = 10, hold = 0;
  int conj = -1, lag = G_MININT;
  const char *csv_path = NULL, *pa = NULL, *pb = NULL;
  for (int i = 1; i < argc; i++) {
    const char *o = argv[i];
    if (!strcmp(o, "--rate") && i + 1 < argc) { rate = g_ascii_strtod(argv[++i], NULL); }
    else if (!strcmp(o, "--centre") && i + 1 < argc) { centre = g_ascii_strtod(argv[++i], NULL); }
    else if (!strcmp(o, "--conj")) { conj = 1; }
    else if (!strcmp(o, "--no-conj")) { conj = 0; }
    else if (!strcmp(o, "--lag") && i + 1 < argc) { lag = atoi(argv[++i]); }
    else if (!strcmp(o, "--every") && i + 1 < argc) { every = g_ascii_strtod(argv[++i], NULL); }
    else if (!strcmp(o, "--hold") && i + 1 < argc) { hold = g_ascii_strtod(argv[++i], NULL); }
    else if (!strcmp(o, "--csv") && i + 1 < argc) { csv_path = argv[++i]; }
    else if (o[0] == '-' && o[1]) {
      fprintf(stderr, "unknown option %s\n", o);
      return 2;
    } else if (!pa) { pa = o; } else if (!pb) { pb = o; }
    else {
      fprintf(stderr, "too many files\n");
      return 2;
    }
  }
  if (!pa || !pb) {
    fprintf(stderr, "usage: skimmer-phase-bench A.cf32 B.cf32 [--rate HZ] [--centre HZ] "
                    "[--conj|--no-conj] [--lag N] [--every S] [--hold S] [--csv FILE]\n");
    return 2;
  }
  char *meta = NULL, *mp = g_strdup_printf("%s.meta", pa);
  g_file_get_contents(mp, &meta, NULL, NULL);
  g_free(mp);
  if (rate <= 0) { rate = meta_num(meta, "rate_hz:", "rate="); }
  if (isnan(centre)) { centre = meta_num(meta, "center_hz:", "center_hz="); }
  if (conj < 0) { conj = meta && strstr(meta, "RF-inverted") ? 1 : 0; }
  g_free(meta);
  if (rate <= 0) {
    fprintf(stderr, "%s: no rate (no .meta; give --rate)\n", pa);
    return 2;
  }
  FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
  if (!fa || !fb) {
    perror(!fa ? pa : pb);
    return 2;
  }
  double peak = NAN;
  if (lag == G_MININT) { lag = measure_lag(fa, fb, rate, &peak); }
  /* B late by lag: drop lag samples of B (or −lag of A) */
  if (lag > 0) { fseek(fb, (long)lag * 2 * sizeof(float), SEEK_SET); }
  if (lag < 0) { fseek(fa, (long)-lag * 2 * sizeof(float), SEEK_SET); }

  SkimChannelizer *ba = skim_channelizer_new(rate, SPACING);
  SkimChannelizer *bb = skim_channelizer_new(rate, SPACING);
  if (!ba || !bb) {
    fprintf(stderr, "rate %.0f does not split into %.0f Hz channels\n", rate, SPACING);
    return 2;
  }
  const guint nchan = skim_channelizer_count(ba);
  const double crate = skim_channelizer_out_rate(ba);
  Lane lane[2] = { { "mvdr", NULL, NULL }, { "mrc", NULL, NULL } };
  for (guint l = 0; l < 2; l++) {
    SkimCombinerConfig cfg;
    skim_combiner_config_default(&cfg);
    cfg.mode = l == 0 ? SKIM_PHASE_MVDR : SKIM_PHASE_MRC;
    lane[l].comb = skim_combiner_new(nchan, crate, &cfg);
    lane[l].met = skim_phase_meter_new(nchan, crate, SKIM_SNR_CW_ENBW * SPACING);
  }
  FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
  if (csv_path && !csv) {
    perror(csv_path);
    return 2;
  }
  if (csv) { fprintf(csv, "t,chan,freq_hz,mode,snr_a,snr_b,snr_y,gain,gain_a,drop,gain_est,drop_est\n"); }

  printf("skimmer-phase-bench: %s + %s\n", pa, pb);
  printf("  rate %.0f Hz, centre %.0f Hz, %s, B lag %d samples%s", rate, centre,
         conj ? "conjugated on read" : "as stored", lag, isnan(peak) ? " (given)" : "");
  if (!isnan(peak)) { printf(" (measured, |ρ|² %.3f)", peak); }
  printf("\n  %u channels of %.0f Hz, groups of 32\n\n", nchan, SPACING);
  printf("      t  mode  stns   G_best  G_A   drop   p90  ≥3dB  |γ|²  D_max  shB  same  "
         "estG  estD\n");

  float *ia = g_new(float, 2 * BLK), *ib = g_new(float, 2 * BLK);
  float ca[2 * 64], cb[2 * 64], y[2 * 64];
  guint64 frames = 0;
  double next_upd = 1.0, next_line = every, t = 0;
  double cpu_bank_b = 0, cpu_bank_a = 0, cpu_comb = 0;
  gboolean held = FALSE;
  for (;;) {
    const size_t na = fread(ia, 2 * sizeof(float), BLK, fa);
    const size_t nb = fread(ib, 2 * sizeof(float), BLK, fb);
    const guint n = (guint)MIN(na, nb);
    if (n == 0)
      break;
    if (conj) {
      for (guint i = 0; i < n; i++) {
        ia[2 * i + 1] = -ia[2 * i + 1];
        ib[2 * i + 1] = -ib[2 * i + 1];
      }
    }
    double c0 = cpu_s();
    skim_channelizer_push(ba, ia, n);
    double c1 = cpu_s();
    skim_channelizer_push(bb, ib, n);
    double c2 = cpu_s();
    cpu_bank_a += c1 - c0;
    cpu_bank_b += c2 - c1;
    for (guint k = 0; k < nchan; k++) {
      guint got;
      while ((got = skim_channelizer_read(ba, k, ca, 64)) > 0) {
        const guint gb2 = skim_channelizer_read(bb, k, cb, got);
        if (gb2 != got) { g_error("channel %u: A %u frames, B %u", k, got, gb2); }
        const double c3 = cpu_s();
        skim_combiner_process(lane[0].comb, k, ca, cb, y, got);
        skim_phase_meter_push(lane[0].met, k, ca, cb, y, got);
        cpu_comb += cpu_s() - c3;
        skim_combiner_process(lane[1].comb, k, ca, cb, y, got);
        skim_phase_meter_push(lane[1].met, k, ca, cb, y, got);
      }
    }
    frames += n;
    t = frames / rate;
    if (t >= next_upd) {
      next_upd += 1.0;
      const double c4 = cpu_s();
      skim_combiner_update(lane[0].comb);
      skim_phase_meter_refresh(lane[0].met, lane[0].comb);
      cpu_comb += cpu_s() - c4;
      skim_combiner_update(lane[1].comb);
      skim_phase_meter_refresh(lane[1].met, lane[1].comb);
      if (hold > 0 && t >= hold && !held) {
        held = TRUE;
        skim_combiner_hold(lane[0].comb, TRUE);
        skim_combiner_hold(lane[1].comb, TRUE);
      }
      for (guint l = 0; csv && l < 2; l++) {
        for (guint k = 0; k < nchan; k++) {
          const double g = skim_phase_meter_gain(lane[l].met, k);
          const double d = skim_phase_meter_drop(lane[l].met, k);
          const double ge = skim_phase_meter_gain_est(lane[l].met, k);
          const double de = skim_phase_meter_drop_est(lane[l].met, k);
          if (isnan(g) && isnan(d) && isnan(ge) && isnan(de))
            continue;
          fprintf(csv, "%.0f,%u,%.0f,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n", t, k,
                  centre + skim_channelizer_offset_hz(ba, k), lane[l].name,
                  skim_phase_meter_snr(lane[l].met, SKIM_PH_A, k),
                  skim_phase_meter_snr(lane[l].met, SKIM_PH_B, k),
                  skim_phase_meter_snr(lane[l].met, SKIM_PH_Y, k), g,
                  skim_phase_meter_gain_a(lane[l].met, k), d, ge, de);
        }
      }
    }
    if (t >= next_line) {
      next_line += every;
      print_line(t, &lane[0]);
      print_line(t, &lane[1]);
    }
  }
  if (frames == 0) {
    fprintf(stderr, "no samples\n");
    return 1;
  }

  printf("\nnoise groups at the end (|γ|², D_max = the deepest null it allows; the floor "
         "drop each mode reaches, median of the quiet channels)\n");
  printf("  group       from        to   |γ|²  D_max   mvdr    mrc\n");
  const guint ng = skim_combiner_groups(lane[0].comb);
  double *v = g_new(double, nchan);
  for (guint g = 0; g < ng; g++) {
    /* groups run in frequency order from the lowest channel (the
     * channelizer's M/2+1 … M−1, 0 … M/2): positions 32g … 32g+31 */
    const double lo = centre + ((double)g * 32 - nchan / 2.0) * SPACING;
    const double hi = centre + (MIN((double)(g + 1) * 32, nchan) - 1 - nchan / 2.0) * SPACING;
    double med[2] = { NAN, NAN };
    for (guint l = 0; l < 2; l++) {
      guint m = 0;
      for (guint k = 0; k < nchan; k++) {
        if (skim_combiner_group_of(lane[0].comb, k) != g)
          continue;
        const double d = skim_phase_meter_drop(lane[l].met, k);
        if (!isnan(d)) { v[m++] = d; }
      }
      for (guint i = 1; i < m; i++)
        for (guint j = i; j > 0 && v[j - 1] > v[j]; j--) {
          const double x = v[j]; v[j] = v[j - 1]; v[j - 1] = x;
        }
      if (m) { med[l] = v[m / 2]; }
    }
    const double coh = skim_combiner_coherence(lane[0].comb, g);
    char m0[16], m1[16];
    db(m0, sizeof(m0), med[0]);
    db(m1, sizeof(m1), med[1]);
    printf("  %5u  %9.0f %9.0f  %5.3f  %5.1f  %s  %s\n", g, lo - SPACING / 2, hi + SPACING / 2,
           coh, -10.0 * log10(MAX(1e-6, 1.0 - coh)), m0, m1);
  }
  g_free(v);

  printf("\nstations at the end (SNR_2500 dB; G = over the better antenna)\n");
  printf("        freq_hz   SNR_A  SNR_B   mvdr      G   mrc       G   estG  same\n");
  for (guint k0 = 0; k0 < nchan; k0++) {
    const guint k = (k0 + nchan / 2 + 1) % nchan;   /* low to high */
    SkimCombinerChanInfo info;
    skim_combiner_chan_info(lane[0].comb, k, &info);
    const double g0 = skim_phase_meter_gain(lane[0].met, k);
    if (isnan(g0))
      continue;
    char ge[16];
    db(ge, sizeof(ge), info.gain_est_db);
    printf("  %13.0f  %+6.1f %+6.1f  %+6.1f  %+5.1f %+6.1f  %+5.1f  %s  %4.2f\n",
           centre + skim_channelizer_offset_hz(ba, k),
           skim_phase_meter_snr(lane[0].met, SKIM_PH_A, k),
           skim_phase_meter_snr(lane[0].met, SKIM_PH_B, k),
           skim_phase_meter_snr(lane[0].met, SKIM_PH_Y, k), g0,
           skim_phase_meter_snr(lane[1].met, SKIM_PH_Y, k),
           skim_phase_meter_gain(lane[1].met, k), ge, isnan(info.align) ? 0 : info.align);
  }

  printf("\nCPU for %.1f s of a %.0f Hz band: channelizer A %.2f s, B %.2f s (the second "
         "bank: %.1f %% of one core), combiner + meters %.2f s (%.1f %%)\n",
         t, rate, cpu_bank_a, cpu_bank_b, 100.0 * cpu_bank_b / t, cpu_comb,
         100.0 * cpu_comb / t);

  for (guint l = 0; l < 2; l++) {
    skim_phase_meter_free(lane[l].met);
    skim_combiner_free(lane[l].comb);
  }
  skim_channelizer_free(ba);
  skim_channelizer_free(bb);
  g_free(ia);
  g_free(ib);
  fclose(fa);
  fclose(fb);
  if (csv) { fclose(csv); }
  return 0;
}
