/* ft8d_test.c — the ft8d gate: skimmer-ft8d (WSJT-X's FT8/FT4 decoders,
 * vendor/wsjtx-ft) on synthetic air. The channel tones come from upstream's
 * own ft8code / ft4code (so the encoder — and its CRC-14, which is ours in
 * wsjtx_stubs.c — is checked by the decoder's Fortran get_crc14); the
 * signals are continuous-phase FSK in white noise at a known SNR (2500 Hz),
 * DT and audio frequency, written as jt9 reads them (12 kHz mono 16-bit,
 * YYMMDD_HHMMSS.wav). Both messages come back with their SNR, DT and
 * frequency, nothing else does; two files in one run are two cycles; a
 * WAV at the wrong rate is refused.
 *
 * argv: SKIMMER-FT8D SKIMMER-FT8CODE SKIMMER-FT4CODE
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, gboolean ok) {
  g_print("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) { fails++; }
}

static const char *ft8d, *ft8code, *ft4code;

#define FS 12000

/* The channel tones of msg as upstream's ftNcode prints them. */
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
  if (p) { p = strchr(p, '\n'); }               /* the "Sync  Data" header … */
  if (p) { p = strchr(p + 1, '\n'); }           /* … then the tones          */
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

typedef struct {
  const char *msg;
  double f0, snr, dt;                   /* Hz, dB in 2500 Hz, s after 0.5 s */
} Sig;

/* One cycle of air: ft8 = 15 s, 1920 samples/symbol, 6.25 Hz; ft4 =
 * 7.5 s, 576 samples/symbol, 20.833 Hz, the ramp symbols included.
 * Signals start at 0.5 s + dt (WSJT-X's DT = 0). */
static gboolean write_wav(const char *path, gboolean ft4, const Sig *sig, guint nsig, guint seed) {
  const guint n = ft4 ? FS * 15 / 2 : FS * 15, nsps = ft4 ? 576 : 1920;
  const double df = (double)FS / nsps;
  double *x = g_new0(double, n);
  GRand *r = g_rand_new_with_seed(seed);
  for (guint i = 0; i < n; i++) { x[i] = gauss(r); }   /* unit variance over FS/2 */
  g_rand_free(r);
  for (guint k = 0; k < nsig; k++) {
    int t[128];
    const guint nt = tones(ft4 ? ft4code : ft8code, sig[k].msg, t, 128);
    if (nt != (ft4 ? 105u : 79u)) {
      g_printerr("%s: %u tones\n", sig[k].msg, nt);
      g_free(x);
      return FALSE;
    }
    const double a = sqrt(2 * pow(10, sig[k].snr / 10) * 2500.0 / (FS / 2.0));
    const guint s0 = (guint)lround((0.5 + sig[k].dt) * FS);
    double ph = 0;
    for (guint j = 0; j < nt * nsps && s0 + j < n; j++) {
      ph += 2 * G_PI * (sig[k].f0 + df * t[j / nsps]) / FS;
      x[s0 + j] += a * sin(ph);
    }
  }
  double peak = 0;
  for (guint i = 0; i < n; i++) { peak = MAX(peak, fabs(x[i])); }
  gint16 *pcm = g_new(gint16, n);
  for (guint i = 0; i < n; i++) { pcm[i] = (gint16)lround(x[i] / peak * 20000); }
  g_free(x);
  FILE *f = fopen(path, "wb");
  if (!f) {
    g_free(pcm);
    return FALSE;
  }
  const guint32 data = n * 2, riff = 36 + data, fmt = 16, rate = FS, byte_rate = FS * 2;
  const guint16 pcm_fmt = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f); fwrite(&pcm_fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f);
  fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  const gboolean ok = fwrite(pcm, 2, n, f) == n;
  g_free(pcm);
  return fclose(f) == 0 && ok;
}

/* A line of skimmer-ft8d (jt9's): HHMMSS snr dt freq ~|+  message  [aN] */
typedef struct {
  int hhmmss, snr, freq;
  double dt;
  char mode;
  char msg[40];
} Dec;

static guint run(const char *const *files, guint nfiles, gboolean ft4, Dec *out, guint max,
                 guint *finished, int *status) {
  GPtrArray *a = g_ptr_array_new();
  g_ptr_array_add(a, (char *)ft8d);
  g_ptr_array_add(a, ft4 ? "-4" : "-8");
  g_ptr_array_add(a, "-d");
  g_ptr_array_add(a, "2");
  for (guint i = 0; i < nfiles; i++) { g_ptr_array_add(a, (char *)files[i]); }
  g_ptr_array_add(a, NULL);
  char *so = NULL;
  *status = -1;
  *finished = 0;
  guint n = 0;
  if (g_spawn_sync(NULL, (char **)a->pdata, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &so,
                   NULL, status, NULL)) {
    char **lines = g_strsplit(so, "\n", -1);
    for (char **l = lines; *l; l++) {
      if (strstr(*l, "<DecodeFinished>")) {
        (*finished)++;
        continue;
      }
      Dec d = { 0 };
      int pos = 0;
      if (n < max && sscanf(*l, "%6d %d %lf %d %c %n", &d.hhmmss, &d.snr, &d.dt, &d.freq, &d.mode,
                            &pos) == 5) {
        g_strlcpy(d.msg, g_strstrip(*l + pos), sizeof d.msg);
        out[n++] = d;
      }
    }
    g_strfreev(lines);
  }
  g_free(so);
  g_ptr_array_free(a, TRUE);
  return n;
}

static const Dec *find(const Dec *d, guint n, const char *msg) {
  for (guint i = 0; i < n; i++) {
    if (strcmp(d[i].msg, msg) == 0) { return &d[i]; }
  }
  return NULL;
}

static void expect(const char *tag, const Dec *d, guint n, const Sig *s, int hhmmss, char mode) {
  const Dec *x = find(d, n, s->msg);
  char *what = g_strdup_printf("%s: \"%s\" decoded", tag, s->msg);
  check(what, x != NULL);
  g_free(what);
  if (!x) { return; }
  what = g_strdup_printf("%s: \"%s\" UTC %06d, mode %c, freq %d (%.0f), dt %.1f (%.1f), "
                         "snr %d (%.0f)", tag, s->msg, x->hhmmss, x->mode, x->freq, s->f0, x->dt,
                         s->dt, x->snr, s->snr);
  check(what, x->hhmmss == hhmmss && x->mode == mode && abs(x->freq - (int)s->f0) <= 2 &&
                  fabs(x->dt - s->dt) <= 0.15 && fabs(x->snr - s->snr) <= 3);
  g_free(what);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    g_printerr("usage: %s SKIMMER-FT8D SKIMMER-FT8CODE SKIMMER-FT4CODE\n", argv[0]);
    return 2;
  }
  ft8d = argv[1];
  ft8code = argv[2];
  ft4code = argv[3];

  /* upstream's encoder: FT8's Costas arrays where they belong */
  int t[128];
  const guint n8 = tones(ft8code, "CQ OK1HRA JO70", t, 128);
  const int costas[7] = { 3, 1, 4, 0, 6, 5, 2 };
  gboolean sync = n8 == 79;
  for (guint k = 0; sync && k < 7; k++) {
    sync = t[k] == costas[k] && t[36 + k] == costas[k] && t[72 + k] == costas[k];
  }
  check("ft8code: 79 tones, the Costas array at 0, 36 and 72", sync);
  /* FT4: 103 symbols and a ramp symbol (tone 0) at each end, as upstream's
   * gen_ft4wave sends them — synthesized whole, they give DT 0 at 0.5 s */
  const guint n4 = tones(ft4code, "CQ OK1HRA JO70", t, 128);
  check("ft4code: 105 tones (103 + the two ramp symbols)", n4 == 105 && t[0] == 0 && t[104] == 0);

  char *dir = g_dir_make_tmp("skimmer-ft8d-XXXXXX", NULL);
  Dec d[32];
  guint fin = 0;
  int st = 0;

  /* FT8: a CQ and a report, one weak, in one cycle */
  const Sig s8[] = { { "CQ OK1HRA JO70", 1000, -15, 0.0 },
                     { "DL1ABC OK1HRA -12", 1800, -19, 0.3 } };
  char *w1 = g_build_filename(dir, "261122_181500.wav", NULL);
  char *w2 = g_build_filename(dir, "261122_181515.wav", NULL);
  const Sig s8b[] = { { "OK1HRA DL1ABC R-09", 1500, -12, -0.4 },
                      { "CQ DX VK2XYZ QF56", 600, -17, 0.8 } };
  check("FT8 WAVs written", write_wav(w1, FALSE, s8, 2, 1) && write_wav(w2, FALSE, s8b, 2, 2));
  const char *one[] = { w1 };
  guint n = run(one, 1, FALSE, d, 32, &fin, &st);
  check("FT8: exit 0, one <DecodeFinished>", st == 0 && fin == 1);
  check("FT8: exactly the two messages", n == 2);
  expect("FT8", d, n, &s8[0], 181500, '~');
  expect("FT8", d, n, &s8[1], 181500, '~');

  /* two cycles in one run: each with its own UTC */
  const char *two[] = { w1, w2 };
  n = run(two, 2, FALSE, d, 32, &fin, &st);
  check("FT8 two files: exit 0, two <DecodeFinished>, four messages", st == 0 && fin == 2 && n == 4);
  expect("FT8 2nd cycle", d, n, &s8b[0], 181515, '~');
  expect("FT8 2nd cycle", d, n, &s8b[1], 181515, '~');

  /* FT4: 7.5 s */
  const Sig s4[] = { { "CQ OK1HRA JO70", 1000, -10, 0.0 },
                     { "DL1ABC OK1HRA -12", 2200, -14, 0.3 } };
  char *w4 = g_build_filename(dir, "261122_181507.wav", NULL);
  check("FT4 WAV written", write_wav(w4, TRUE, s4, 2, 3));
  const char *four[] = { w4 };
  n = run(four, 1, TRUE, d, 32, &fin, &st);
  check("FT4: exit 0, one <DecodeFinished>, exactly the two messages",
        st == 0 && fin == 1 && n == 2);
  expect("FT4", d, n, &s4[0], 181507, '+');
  expect("FT4", d, n, &s4[1], 181507, '+');

  /* noise alone: nothing */
  char *w0 = g_build_filename(dir, "261122_181530.wav", NULL);
  check("noise WAV written", write_wav(w0, FALSE, NULL, 0, 4));
  const char *zero[] = { w0 };
  n = run(zero, 1, FALSE, d, 32, &fin, &st);
  check("FT8 noise only: no decode", st == 0 && fin == 1 && n == 0);

  /* a WAV that is not 12 kHz is refused (exit 2), not misread */
  char *bad = g_build_filename(dir, "261122_181545.wav", NULL);
  {
    gchar *raw = NULL;
    gsize len = 0;
    if (g_file_get_contents(w0, &raw, &len, NULL) && len > 44) {
      const guint32 r8 = 8000, br = 16000;
      memcpy(raw + 24, &r8, 4);
      memcpy(raw + 28, &br, 4);
      g_file_set_contents(bad, raw, (gssize)len, NULL);
    }
    g_free(raw);
  }
  const char *badv[] = { bad };
  n = run(badv, 1, FALSE, d, 32, &fin, &st);
  check("an 8 kHz WAV is refused", g_spawn_check_wait_status(st, NULL) == FALSE && n == 0);

  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  if (system(rm) != 0) { g_printerr("could not remove %s\n", dir); }
  g_free(rm);
  g_free(w0);
  g_free(w1);
  g_free(w2);
  g_free(w4);
  g_free(bad);
  g_free(dir);

  g_print("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
