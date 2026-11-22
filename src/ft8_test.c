/* ft8_test.c — the ft8 gate: FT8/FT4 beside the skimmer (src/headless/ft8.c).
 * The dials a receiver takes; skimmer-ft8d's lines and the sender of each
 * kind of message; then end to end — FT8 and FT4 signals (tones from
 * upstream's ft8code / ft4code) in noise, in IQ at 48 and 192 kHz on its
 * own jittered clock, through the shift, the two filters, the USB audio,
 * the capture at the UTC cycle, the WAV and skimmer-ft8d: each message
 * comes back at its frequency, DT and SNR; a strong station BELOW the dial
 * does not (it would be the USB audio's mirror); each sender is a spot
 * once (a second cycle of the same is folded) at dial + its audio
 * frequency. Last, a decoder slower than the cycles: the waiting cycles are
 * skipped, counted, and the newest one is decoded.
 *
 * argv: SKIMMER-FT8D SKIMMER-FT8CODE SKIMMER-FT4CODE
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "headless/ft8.h"

static int fails;
static void check(const char *what, gboolean ok) {
  g_print("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) { fails++; }
}

static const char *ft8d, *ft8code, *ft4code;

#define CENTRE 14090000.0
#define DIAL8  14074000.0
#define DIAL4  14080000.0

/* ---- the synthetic air -------------------------------------------------------------- */

typedef struct {
  const char *msg;
  SkimFt8Mode mode;
  double rf;                            /* the tone-0 frequency, Hz (RF)          */
  double snr;                           /* dB in 2500 Hz                          */
  double dt;                            /* s after 0.5 s into its cycle            */
  guint  cycles;                        /* sent in this many cycles from the first */
  int    t[128];
  guint  nt;
  double ph;
} Sig;

static guint tones(const char *prog, const char *msg, int *t, guint max) {
  const char *argv[] = { prog, msg, NULL };
  char *out = NULL;
  int st = 0;
  if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out,
                    NULL, &st, NULL)) {
    return 0;
  }
  guint n = 0;
  const char *p = strstr(out, "Channel symbols");
  if (p) { p = strchr(p, '\n'); }
  if (p) { p = strchr(p + 1, '\n'); }
  for (p = p ? p + 1 : NULL; p && *p && *p != '\n'; p++) {
    if (*p >= '0' && *p <= '7' && n < max) { t[n++] = *p - '0'; }
  }
  g_free(out);
  return n;
}

static double gauss(GRand *r) {
  const double u = g_rand_double_range(r, 1e-12, 1), v = g_rand_double(r);
  return sqrt(-2 * log(u)) * cos(2 * G_PI * v);
}

/* IQ from base_s − 2 s for secs, in blocks of 1024 frames, each pushed with
 * the time of its last frame (one block in ten up to 30 ms late). Noise:
 * I and Q of unit variance over the rate. */
static void air(SkimHdFt8 *f, double rate, gint64 base_s, double secs, Sig *sig, guint nsig,
                guint seed) {
  GRand *r = g_rand_new_with_seed(seed);
  const guint blk = 1024;
  float *iq = g_new(float, 2 * blk);
  const guint64 total = (guint64)(secs * rate);
  for (guint k = 0; k < nsig; k++) {
    sig[k].ph = 0;
    sig[k].nt = tones(sig[k].mode == SKIM_FT8_MODE_FT4 ? ft4code : ft8code, sig[k].msg, sig[k].t, 128);
  }
  for (guint64 f0 = 0; f0 < total; f0 += blk) {
    for (guint i = 0; i < blk; i++) {
      const double t = -2.0 + (double)(f0 + i) / rate;       /* s from base */
      double re = gauss(r), im = gauss(r);
      for (guint k = 0; k < nsig; k++) {
        Sig *s = &sig[k];
        const double period = skim_ft8_period_ms(s->mode) / 1000.0;
        const double nsps = s->mode == SKIM_FT8_MODE_FT4 ? 576 : 1920;
        const double df = SKIM_FT8_RATE / nsps;
        if (t < 0) { continue; }
        const guint cyc = (guint)floor(t / period);
        const double ts = t - cyc * period - 0.5 - s->dt;    /* s into the message */
        const int sym = (int)floor(ts * SKIM_FT8_RATE / nsps);
        if (cyc >= s->cycles || ts < 0 || sym >= (int)s->nt) { continue; }
        const double fr = s->rf + df * s->t[sym] - CENTRE;
        s->ph += 2 * G_PI * fr / rate;
        const double a = sqrt(pow(10, s->snr / 10) * 2500.0 * 2.0 / rate);
        re += a * cos(s->ph);
        im += a * sin(s->ph);
      }
      iq[2 * i] = (float)re;
      iq[2 * i + 1] = (float)im;
    }
    gint64 t_end = base_s * G_USEC_PER_SEC +
                   (gint64)((-2.0 + (double)(f0 + blk - 1) / rate) * 1e6);
    if (g_rand_int_range(r, 0, 10) == 0) { t_end += g_rand_int_range(r, 0, 30000); }
    skim_hd_ft8_push_at(f, 0, iq, blk, t_end);
  }
  g_free(iq);
  g_rand_free(r);
}

/* ---- the spots ---------------------------------------------------------------------- */

static GMutex spot_lock;
static GString *spots;

static void spot_cb(const char *call, const char *mode, double freq_hz, int snr, gboolean cq,
                    const char *band, const char *grid, gint64 t, gpointer user) {
  (void)user; (void)t;
  g_mutex_lock(&spot_lock);
  g_string_append_printf(spots, "%s %s %.0f %d %s %s %s;", call, mode, freq_hz, snr,
                         cq ? "CQ" : "DE", band, grid && *grid ? grid : "-");
  g_mutex_unlock(&spot_lock);
}

/* a spot "CALL MODE …" of spots: its frequency and SNR */
static gboolean spot_of(const char *call, const char *mode, double *freq, int *snr, char *kind) {
  char *needle = g_strdup_printf("%s %s ", call, mode);
  g_mutex_lock(&spot_lock);
  const char *p = strstr(spots->str, needle);
  gboolean ok = p && sscanf(p + strlen(needle), "%lf %d %3s", freq, snr, kind) == 3;
  g_mutex_unlock(&spot_lock);
  g_free(needle);
  return ok;
}

static guint count(const char *hay, const char *needle) {
  guint n = 0;
  for (const char *p = hay; (p = strstr(p, needle)); p++) { n++; }
  return n;
}

/* ---- end to end --------------------------------------------------------------------- */

static void end_to_end(double rate, const char *dir) {
  g_mutex_lock(&spot_lock);
  g_string_truncate(spots, 0);
  g_mutex_unlock(&spot_lock);
  char *tag = g_strdup_printf("%.0f kHz", rate / 1e3);
  const char *names[] = { "20m" };
  const double centres[] = { CENTRE };
  const SkimHdFt8Config cfg = { .ft8 = TRUE, .ft4 = TRUE, .depth = 2, .lo_hz = 200,
                                .hi_hz = 3500, .threads = 2, .dir = dir, .decoder = ft8d,
                                .spot = spot_cb };
  SkimHdFt8 *f = skim_hd_ft8_new(&cfg, 1, names, centres, rate);
  char *dials = skim_hd_ft8_dials_text(f);
  char *what = g_strdup_printf("%s: the receiver takes 20m FT8 and FT4 (%s)", tag, dials);
  check(what, strcmp(dials, "20m FT8 14074.0, 20m FT4 14080.0") == 0);
  g_free(what);
  g_free(dials);

  Sig sig[] = {
    { .msg = "CQ OK1HRA JO70", .mode = SKIM_FT8_MODE_FT8, .rf = DIAL8 + 1000, .snr = -12, .dt = 0.0, .cycles = 2 },
    { .msg = "OK1HRA DL1ABC -12", .mode = SKIM_FT8_MODE_FT8, .rf = DIAL8 + 2200, .snr = -15, .dt = 0.3, .cycles = 2 },
    { .msg = "CQ TEST K1ABC FN42", .mode = SKIM_FT8_MODE_FT4, .rf = DIAL4 + 1200, .snr = -8, .dt = 0.0, .cycles = 1 },
    /* below the 20m FT8 dial, loud: USB must not hear it (its mirror would
     * sit at +1500 Hz) */
    { .msg = "CQ JA1XYZ PM95", .mode = SKIM_FT8_MODE_FT8, .rf = DIAL8 - 1500, .snr = 0, .dt = 0.0, .cycles = 2 },
  };
  const gint64 base_s = (g_get_real_time() / G_USEC_PER_SEC / 60) * 60 - 120;
  air(f, rate, base_s, 2.0 + 30.6, sig, G_N_ELEMENTS(sig), (guint)rate);
  skim_hd_ft8_drain(f);

  char *all = skim_hd_ft8_decodes_json(f, 0, 0);
  char *d8 = skim_hd_ft8_decodes_json(f, DIAL8, 0);
  char *d8b = skim_hd_ft8_decodes_json(f, DIAL8, base_s * 1000 + 15000);
  char *st = skim_hd_ft8_status_json(f);
  g_print("     %s\n     %s\n", all, st);

  what = g_strdup_printf("%s: FT8 \"CQ OK1HRA JO70\" in both cycles, \"OK1HRA DL1ABC -12\" too", tag);
  check(what, count(d8, "\"msg\":\"CQ OK1HRA JO70\"") == 2 &&
                  count(d8, "\"msg\":\"OK1HRA DL1ABC -12\"") == 2);
  g_free(what);
  what = g_strdup_printf("%s: … the second cycle alone since its start", tag);
  check(what, count(d8b, "\"msg\":") == 2);
  g_free(what);
  what = g_strdup_printf("%s: FT4 \"CQ TEST K1ABC FN42\" once (its first cycle)", tag);
  check(what, count(all, "\"msg\":\"CQ TEST K1ABC FN42\"") == 1 &&
                  strstr(all, "\"mode\":\"FT4\"") != NULL);
  g_free(what);
  what = g_strdup_printf("%s: the station below the dial is not heard (no mirror)", tag);
  check(what, !strstr(all, "JA1XYZ"));
  g_free(what);
  what = g_strdup_printf("%s: nothing else decoded (2 + 2 + 1)", tag);
  check(what, count(all, "\"msg\":") == 5);
  g_free(what);
  /* the first decode's frequency, DT, SNR */
  {
    const char *p = strstr(all, "\"msg\":\"CQ OK1HRA JO70\"");
    const char *q = p;
    while (q && q > all && *q != '{') { q--; }
    int snr = 0, df = 0;
    double dt = 0;
    const gboolean ok = q && sscanf(strstr(q, "\"snr\":"), "\"snr\":%d,\"dt\":%lf,\"df\":%d", &snr,
                                    &dt, &df) == 3;
    what = g_strdup_printf("%s: CQ OK1HRA at %d Hz (1000), DT %.1f (0.0), SNR %d (-12)", tag, df,
                           dt, snr);
    check(what, ok && abs(df - 1000) <= 2 && fabs(dt) <= 0.15 && abs(snr + 12) <= 3);
    g_free(what);
  }

  double fr = 0;
  int snr = 0;
  char kind[4] = "";
  g_mutex_lock(&spot_lock);
  g_print("     spots: %s\n", spots->str);
  const guint nspots = count(spots->str, ";");
  g_mutex_unlock(&spot_lock);
  what = g_strdup_printf("%s: three spots — the second cycle is folded", tag);
  check(what, nspots == 3);
  g_free(what);
  gboolean got = spot_of("OK1HRA", "FT8", &fr, &snr, kind);
  what = g_strdup_printf("%s: OK1HRA FT8 CQ at 14075000 (%.0f)", tag, fr);
  check(what, got && fabs(fr - 14075000) <= 3 && strcmp(kind, "CQ") == 0);
  g_free(what);
  what = g_strdup_printf("%s: DL1ABC FT8, not calling CQ: DE, at 14076200", tag);
  check(what, spot_of("DL1ABC", "FT8", &fr, &snr, kind) && fabs(fr - 14076200) <= 3 &&
                  strcmp(kind, "DE") == 0);
  g_free(what);
  g_mutex_lock(&spot_lock);
  const gboolean grids = strstr(spots->str, " CQ 20m JO70;") && strstr(spots->str, "DL1ABC FT8") &&
                         strstr(spots->str, " DE 20m -;") && strstr(spots->str, " CQ 20m FN42;");
  g_mutex_unlock(&spot_lock);
  what = g_strdup_printf("%s: each spot with its sender's locator when sent (OK1HRA JO70, K1ABC FN42, DL1ABC none)", tag);
  check(what, grids);
  g_free(what);
  what = g_strdup_printf("%s: K1ABC FT4 CQ at 14081200", tag);
  check(what, spot_of("K1ABC", "FT4", &fr, &snr, kind) && fabs(fr - 14081200) <= 3 &&
                  strcmp(kind, "CQ") == 0);
  g_free(what);
  what = g_strdup_printf("%s: the status: 2 FT8 + 4 FT4 cycles, 5 decodes, 3 spots, the 20m row "
                         "with its 3 senders", tag);
  check(what, strstr(st, "\"cycles\":6,") && strstr(st, "\"decodes\":5,") &&
                  strstr(st, "\"spots\":3,") && strstr(st, "\"skipped\":0,") &&
                  strstr(st, "{\"band\":\"20m\",\"stations\":3,") &&
                  strstr(st, "\"error\":null"));
  g_free(what);
  g_free(all);
  g_free(d8);
  g_free(d8b);
  g_free(st);
  skim_hd_ft8_free(f);
  g_free(tag);
}

/* A decoder slower than the cycles: one at work, the rest wait, and each
 * newer cycle of the dial skips the one before it. */
static void slow(const char *dir) {
  char *fake = g_build_filename(dir, "slow-ft8d", NULL);
  g_file_set_contents(fake, "#!/bin/sh\nsleep 3\n", -1, NULL);
  g_chmod(fake, 0755);
  const char *names[] = { "20m" };
  const double centres[] = { CENTRE };
  const SkimHdFt8Config cfg = { .ft8 = TRUE, .ft4 = FALSE, .depth = 1, .lo_hz = 200,
                                .hi_hz = 3500, .threads = 1, .dir = dir, .decoder = fake };
  SkimHdFt8 *f = skim_hd_ft8_new(&cfg, 1, names, centres, 48000);
  const gint64 base_s = (g_get_real_time() / G_USEC_PER_SEC / 60) * 60 - 300;
  air(f, 48000, base_s, 2.0 + 60.5, NULL, 0, 7);   /* four FT8 cycles, faster than 3 s */
  skim_hd_ft8_drain(f);
  char *st = skim_hd_ft8_status_json(f);
  g_print("     %s\n", st);
  check("slow decoder: of 4 cycles the 1st and the 4th decoded, the 2nd and 3rd skipped",
        strstr(st, "\"cycles\":2,") && strstr(st, "\"skipped\":2,") &&
            strstr(st, "\"ft4\":false") && strstr(st, "\"depth\":1"));
  g_free(st);
  skim_hd_ft8_free(f);
  g_free(fake);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    g_printerr("usage: %s SKIMMER-FT8D SKIMMER-FT8CODE SKIMMER-FT4CODE\n", argv[0]);
    return 2;
  }
  ft8d = argv[1];
  ft8code = argv[2];
  ft4code = argv[3];
  spots = g_string_new(NULL);

  /* the dials a receiver takes */
  check("20m FT8 inside 14045.5 at 96 kHz (14074.2-14077.5 ≤ 14088.7)",
        skim_ft8_covers(14045500, 96000, 14074000, 200, 3500));
  check("15m FT4 21140 not inside 21045.5 at 96 kHz",
        !skim_ft8_covers(21045500, 96000, 21140000, 200, 3500));
  check("… but 21074 FT8 is", skim_ft8_covers(21045500, 96000, 21074000, 200, 3500));

  /* skimmer-ft8d's lines */
  SkimFt8Decode d;
  check("an FT8 line",
        skim_ft8_parse_line("181500 -12  0.3 1234 ~  CQ OK1HRA JO70                          ", &d) &&
            d.snr == -12 && fabs(d.dt - 0.3) < 1e-9 && d.df == 1234 &&
            strcmp(d.msg, "CQ OK1HRA JO70") == 0 && strcmp(d.call, "OK1HRA") == 0 && d.cq &&
            !d.ap && !d.doubt);
  check("an FT4 line, a report: the second call sends",
        skim_ft8_parse_line("181507   3 -0.1  612 +  OK1HRA DL1ABC R-09                      ", &d) &&
            strcmp(d.call, "DL1ABC") == 0 && !d.cq);
  check("an AP decode is marked",
        skim_ft8_parse_line("181500 -20  0.1  900 ~  OK1HRA DL1ABC RR73                      a2", &d) &&
            d.ap && !d.doubt && strcmp(d.call, "DL1ABC") == 0);
  {
    /* decoder.f90's format: the message in 37 columns, the 37th '?' */
    char ln[128], m37[38];
    g_snprintf(m37, sizeof m37, "%-36s?", "OK1HRA DL1ABC 73");
    g_snprintf(ln, sizeof ln, "181500 -23  0.1  900 ~  %s a3", m37);
    check("… a doubtful one too ('?' in the 37th column)",
          skim_ft8_parse_line(ln, &d) && d.ap && d.doubt && strcmp(d.msg, "OK1HRA DL1ABC 73") == 0);
  }
  check("<DecodeFinished> is no decode", !skim_ft8_parse_line("<DecodeFinished>", &d));
  check("garbage is no decode", !skim_ft8_parse_line("181500 x", &d));

  /* the sender of each kind of message */
  struct { const char *msg, *call; gboolean cq; } S[] = {
    { "CQ OK1HRA JO70", "OK1HRA", TRUE },        { "CQ DX VK2XYZ QF56", "VK2XYZ", TRUE },
    { "CQ POTA K1ABC FN42", "K1ABC", TRUE },     { "CQ 290 K1ABC FN42", "K1ABC", TRUE },
    { "QRZ DL1ABC", "DL1ABC", TRUE },            { "CQ PJ4/K1ABC", "PJ4/K1ABC", TRUE },
    { "OK1HRA DL1ABC -12", "DL1ABC", FALSE },    { "OK1HRA DL1ABC RR73", "DL1ABC", FALSE },
    { "<...> DL1ABC -12", "DL1ABC", FALSE },     { "TU; OK1HRA DL1ABC 579 0012", "DL1ABC", FALSE },
    { "DE OK1HRA", "OK1HRA", FALSE },            { "K1ABC/R W9XYZ R FN42", "W9XYZ", FALSE },
    { "CQ <PJ4/K1ABC>", NULL, FALSE },           { "OK1HRA <PJ4/K1ABC> -12", NULL, FALSE },
    { "OK1HRA RR73; DL1ABC <VK9DX> -12", NULL, FALSE }, { "TNX BOB 73 GL", NULL, FALSE },
    { "0123456789ABCDEF01", NULL, FALSE },       { "CQ", NULL, FALSE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(S); i++) {
    char call[16];
    gboolean cq = FALSE;
    const gboolean ok = skim_ft8_sender(S[i].msg, call, sizeof call, &cq);
    char *w = g_strdup_printf("sender of \"%s\": %s%s", S[i].msg, S[i].call ? S[i].call : "none",
                              S[i].cq ? " (CQ)" : "");
    check(w, S[i].call ? ok && strcmp(call, S[i].call) == 0 && cq == S[i].cq : !ok && !call[0]);
    g_free(w);
  }

  /* the sender's locator */
  struct { const char *msg, *grid; } G[] = {
    { "CQ OK1HRA JO70", "JO70" }, { "CQ DX VK2XYZ QF56", "QF56" }, { "OK1HRA DL1ABC JN58", "JN58" },
    { "OK1HRA DL1ABC RR73", "" }, { "OK1HRA DL1ABC -12", "" }, { "CQ OK1HRA", "" },
    { "OK1HRA JO70 DL1ABC", "" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(G); i++) {
    char g[8];
    skim_ft8_sender_grid(G[i].msg, g);
    char *w = g_strdup_printf("the sender's locator of \"%s\": \"%s\"", G[i].msg, G[i].grid);
    check(w, strcmp(g, G[i].grid) == 0);
    g_free(w);
  }

  char *dir = g_dir_make_tmp("skimmer-ft8-XXXXXX", NULL);
  end_to_end(48000, dir);
  end_to_end(192000, dir);
  slow(dir);
  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  if (system(rm) != 0) { g_printerr("could not remove %s\n", dir); }
  g_free(rm);
  g_free(dir);
  g_string_free(spots, TRUE);

  g_print("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
