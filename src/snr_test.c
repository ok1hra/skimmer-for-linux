/*
 * skimmer-snr-test — the shadow SNR meter (engine/snr_meter.h) against its
 * definition: a keyed carrier of known SNR_2500 = P_key_down / (N0 · 2500 Hz)
 * in white noise, through the CW channelizer, reads back within tolerance —
 *   from -10 to +40 dB (the old env_hi/env_lo topped out near 30)
 *   on a busy band: keyed neighbours do not lift the floor (one station
 *     per 500 Hz; one per 375 Hz leaves no clean channel — snr_meter.c)
 *   off the channel centre (the ±0.24 · spacing a decode may sit at)
 *   Es/N0_dot = SNR_2500 + 21.8 dB at 20 WPM
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>

#include "engine/channelizer.h"
#include "engine/snr_meter.h"

#define FS      96000.0
#define SPACING 125.0
#define CHAN    40                      /* the station's channel           */
#define SECS    20

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* A CW keyer at 20 WPM: random dits and dahs, a character gap every four
 * elements — about half key-down, like real text. Edges are shaped
 * (sin², 5 ms) as a transmitter shapes them: hard keying's clicks would
 * spread a strong station over the whole band. */
#define RAMP_S 0.005
typedef struct { GRand *r; guint left; gboolean down; guint nel; double lin; } Keyer;

static double keyer_step(Keyer *k, guint dot) {
  if (k->left == 0) {
    if (k->down) {
      k->down = FALSE;
      k->left = (++k->nel % 4 == 0) ? 3 * dot : dot;
    } else {
      k->down = TRUE;
      k->left = g_rand_boolean(k->r) ? dot : 3 * dot;
    }
  }
  k->left--;
  const double step = 1.0 / (RAMP_S * FS);
  k->lin = k->down ? MIN(1.0, k->lin + step) : MAX(0.0, k->lin - step);
  const double e = sin(G_PI / 2 * k->lin);
  return e * e;
}

typedef struct { double hz, snr2500; } Tx;

/* SNR_2500 the meter reads on CHAN after SECS of txs[] in unit noise. */
static double run(const Tx *txs, guint ntx) {
  SkimChannelizer *bank = skim_channelizer_new(FS, SPACING);
  const guint nchan = skim_channelizer_count(bank);
  SkimSnrMeter *m = skim_snr_meter_new(nchan, skim_channelizer_out_rate(bank),
                                       SKIM_SNR_CW_ENBW * SPACING);
  GRand *r = g_rand_new_with_seed(7);
  const double sigma = 1.0, n0 = sigma * sigma / FS;
  const guint dot = (guint)(1.2 / 20 * FS), blk = 4800;
  Keyer *key = g_new0(Keyer, ntx);
  double *amp = g_new(double, ntx), *ph = g_new0(double, ntx);
  for (guint t = 0; t < ntx; t++) {
    key[t].r = g_rand_new_with_seed(100 + t);
    amp[t] = sqrt(pow(10.0, txs[t].snr2500 / 10.0) * n0 * 2500.0);
  }
  float *iq = g_new(float, 2 * blk), out[2 * 64];
  for (guint b = 0; b < SECS * FS / blk; b++) {
    for (guint n = 0; n < blk; n++) {
      const double u1 = g_rand_double(r) + 1e-300, u2 = g_rand_double(r);
      const double g = sqrt(-log(u1)) * sigma;  /* E|x|² = sigma² */
      double i = g * cos(2 * G_PI * u2), q = g * sin(2 * G_PI * u2);
      for (guint t = 0; t < ntx; t++) {
        const double e = keyer_step(&key[t], dot) * amp[t];
        i += e * cos(ph[t]);
        q += e * sin(ph[t]);
        ph[t] = remainder(ph[t] + 2 * G_PI * txs[t].hz / FS, 2 * G_PI);
      }
      iq[2 * n] = (float)i;
      iq[2 * n + 1] = (float)q;
    }
    skim_channelizer_push(bank, iq, blk);
    for (guint c = 0; c < nchan; c++) {
      guint got;
      while ((got = skim_channelizer_read(bank, c, out, 64)) > 0) {
        skim_snr_meter_push(m, c, out, got);
      }
    }
  }
  const double db = skim_snr_meter_db(m, CHAN);
  for (guint t = 0; t < ntx; t++) { g_rand_free(key[t].r); }
  g_free(key);
  g_free(amp);
  g_free(ph);
  g_free(iq);
  g_rand_free(r);
  skim_snr_meter_free(m);
  skim_channelizer_free(bank);
  return db;
}

int main(void) {
  const double f0 = CHAN * SPACING;
  char what[160];

  printf("== a lone station, SNR_2500 from -10 to +40 dB\n");
  const double levels[] = { -10, 0, 10, 20, 30, 40 };
  for (guint i = 0; i < G_N_ELEMENTS(levels); i++) {
    const Tx tx = { f0 + 5.0, levels[i] };
    const double got = run(&tx, 1);
    /* at -10 dB the channel holds +3.7 dB: mark samples are picked from
     * noise as well, a dB or two of bias is expected there */
    const double tol = levels[i] < 0 ? 3.0 : 1.0;
    g_snprintf(what, sizeof(what), "SNR_2500 %+.0f dB reads %+.1f (±%.0f)",
               levels[i], got, tol);
    check(what, fabs(got - levels[i]) <= tol);
  }

  printf("== off the channel centre\n");
  {
    const Tx tx = { f0 + 0.24 * SPACING, 20.0 };
    const double got = run(&tx, 1);
    g_snprintf(what, sizeof(what), "+20 dB at +30 Hz reads %+.1f (±1)", got);
    check(what, fabs(got - 20.0) <= 1.0);
  }

  printf("== a busy band: every fourth neighbour keyed at +30 dB\n");
  {
    Tx txs[24];
    guint n = 0;
    txs[n++] = (Tx){ f0 + 5.0, 10.0 };
    for (gint d = -32; d <= 32 && n < G_N_ELEMENTS(txs); d += 4) {
      if (d != 0) { txs[n++] = (Tx){ f0 + d * SPACING + 7.0, 30.0 }; }
    }
    const double got = run(txs, n);
    /* the clean channels sit 250 Hz from a station +44 dB in its own
     * channel: even 5 ms edges put real power there, which any receiver
     * hears as noise — 2-3 dB of the floor is theirs, not the meter's */
    g_snprintf(what, sizeof(what),
               "+10 dB beside %u keyed neighbours reads %+.1f (-3..+1)", n - 1, got);
    check(what, got >= 7.0 && got <= 11.0);
  }

  printf("== Es/N0_dot\n");
  check("20 WPM: SNR_2500 + 21.8 dB",
        fabs(skim_snr_esn0_dot(0.0, 20.0) - 21.76) < 0.01);
  check("no speed: NAN", isnan(skim_snr_esn0_dot(0.0, 0.0)));

  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
