/* js8_test.c — the js8 gate: JS8 beside the skimmer (src/headless/js8.c,
 * js8_unpack.c, digi_rx.c) with skimmer-js8d (vendor/js8call).
 *
 * The frames: WIFILT's js8-protocol.js unpacked these raw frames to these
 * fields (node, 2026-11-24) — ours must say the same; our packers make the
 * same raw frames. skimmer-js8d's lines, each kind of frame's sender, the
 * submodes, the dials a receiver takes. Then end to end: a JS8 station in
 * each submode (tones from upstream's own encoder, skimmer-js8code) in
 * noise, in IQ at 48 and 192 kHz on its own jittered clock, through the
 * shift, the filters, the USB audio, every submode's UTC periods, the WAV
 * and skimmer-js8d (kept running, then once a period): each frame comes
 * back at its frequency in its submode, a loud station BELOW the dial does
 * not (USB's mirror), each sender is a spot once a band in 10 min. With
 * every frame taken as deep: a call goes to the page first and to the feed
 * once a second period confirms it, or MASTER.SCP does, or js8-known.tsv
 * (which the run writes and the next one reads). Last, a decoder slower
 * than the periods: the waiting ones are skipped, counted.
 *
 * argv: SKIMMER-JS8D SKIMMER-JS8CODE
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "callsign.h"
#include "headless/js8.h"

static int fails;
static void check(const char *what, gboolean ok) {
  g_print("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) { fails++; }
}

static const char *js8d, *js8code;

#define CENTRE 14090000.0
#define DIAL   14078000.0

/* ---- frames --------------------------------------------------------------------- */

typedef struct {
  const char *raw;
  int type;
  SkimJs8Kind kind;
  gboolean cq;
  const char *from, *to, *grid, *cmd, *text;
} Vec;

/* js8-protocol.js (WIFILT fd86b55): decodeFrame() of buildHeartbeatFrames,
 * buildCqFrames, buildReplyFrames — kind, from, to, grid, command, text */
static const Vec VEC[] = {
  { "31-QkpgqOT6W", 3, SKIM_JS8_HEARTBEAT, FALSE, "OK1HRA", "@HB", "JO70", "HEARTBEAT", "OK1HRA: @HB JO70" },
  { "2URtg4DOO++u", 3, SKIM_JS8_HEARTBEAT, FALSE, "K1ABC", "@HB", "", "HEARTBEAT", "K1ABC: @HB" },
  { "1hC3gwiLPTTH", 3, SKIM_JS8_HEARTBEAT, TRUE, "DL1ABC", "@ALLCALL", "JO62", "CQ DX", "DL1ABC: @ALLCALL CQ DX JO62" },
  { "3vLu-YRVH528", 3, SKIM_JS8_HEARTBEAT, TRUE, "VK2XYZ/P", "@ALLCALL", "QF57", "CQ CQ CQ", "VK2XYZ/P: @ALLCALL CQ CQ CQ QF57" },
  { "TBx2Q+APtVaJ", 3, SKIM_JS8_DIRECTED, FALSE, "OK1HRA", "W1AW", "", "SNR", "OK1HRA: W1AW SNR -12" },
  { "OjTQ7+GGOrm0", 3, SKIM_JS8_DIRECTED, FALSE, "3DA0XY", "@ALLCALL", "", "73", "3DA0XY: @ALLCALL 73" },
  { "TBx2Q-oqhIH0", 3, SKIM_JS8_DIRECTED, FALSE, "OK1HRA", "G4ABC/P", "", "GRID?", "OK1HRA: G4ABC/P GRID?" },
  { "B801IRK2CSlu", 1, SKIM_JS8_COMPOUND, FALSE, "PA/OK1ABC", "", "JN89", "", "PA/OK1ABC:" },
  { "KkmQZSal8+Je", 2, SKIM_JS8_COMPOUND_DIRECTED, FALSE, "", "@ARESGA", "", "HEARING?", "@ARESGA HEARING?" },
  { "TBx2Q+GGPoy0", 1, SKIM_JS8_DIRECTED, FALSE, "OK1HRA", "@APRSIS", "", "GRID", "OK1HRA: @APRSIS GRID" },
  { "XpFFxeu2F+++", 2, SKIM_JS8_DATA, FALSE, "", "", "", "", "" },
};

static void frames(void) {
  for (guint i = 0; i < G_N_ELEMENTS(VEC); i++) {
    const Vec *v = &VEC[i];
    SkimJs8Frame f;
    const gboolean ok = skim_js8_unpack(v->raw, v->type, &f);
    char *what = g_strdup_printf("frame %s → \"%s\" (%s %s %s %s)", v->raw, f.text, f.from, f.to,
                                 f.grid, f.cmd);
    check(what, ok && f.kind == v->kind && f.cq == v->cq && strcmp(f.from, v->from) == 0 &&
                    strcmp(f.to, v->to) == 0 && strcmp(f.grid, v->grid) == 0 &&
                    strcmp(f.cmd, v->cmd) == 0 && strcmp(f.text, v->text) == 0);
    g_free(what);
  }
  SkimJs8Frame f;
  check("a frame with a character outside alphabet72 is refused", !skim_js8_unpack("31-Qkpgq T6W", 3, &f));
  check("… and one of 11 characters", !skim_js8_unpack("31-QkpgqOT6", 3, &f));
  check("a fast-data frame (type 4) is data whatever its bits", skim_js8_unpack("31-QkpgqOT6W", 4, &f) &&
                                                                     f.kind == SKIM_JS8_DATA);
  char raw[13];
  check("packer: heartbeat OK1HRA JO70 = 31-QkpgqOT6W",
        skim_js8_pack_heartbeat("OK1HRA", "JO70", FALSE, 0, raw) && strcmp(raw, "31-QkpgqOT6W") == 0);
  check("packer: CQ DX DL1ABC JO62 = 1hC3gwiLPTTH",
        skim_js8_pack_heartbeat("DL1ABC", "JO62", TRUE, 1, raw) && strcmp(raw, "1hC3gwiLPTTH") == 0);
  check("packer: OK1HRA: W1AW SNR -12 = TBx2Q+APtVaJ",
        skim_js8_pack_directed("OK1HRA", "W1AW", 25, -12 + 31, raw) && strcmp(raw, "TBx2Q+APtVaJ") == 0);
  check("packer: OK1HRA: G4ABC/P GRID? = TBx2Q-oqhIH0",
        skim_js8_pack_directed("OK1HRA", "G4ABC/P", 4, 0, raw) && strcmp(raw, "TBx2Q-oqhIH0") == 0);
  check("packer: compound PA/OK1ABC JN89 = B801IRK2CSlu",
        skim_js8_pack_compound(SKIM_JS8_COMPOUND, "PA/OK1ABC", skim_js8_pack_grid("JN89"), 0, raw) &&
            strcmp(raw, "B801IRK2CSlu") == 0);
  check("packer: a compound call does not fit the directed frame",
        !skim_js8_pack_directed("PA/OK1ABC", "W1AW", 25, 0, raw));

  /* senders */
  char call[20];
  skim_js8_unpack("31-QkpgqOT6W", 3, &f);
  check("sender of a heartbeat: OK1HRA", skim_js8_sender(&f, call, sizeof call) && !strcmp(call, "OK1HRA"));
  skim_js8_unpack("3vLu-YRVH528", 3, &f);
  check("sender of a portable CQ: VK2XYZ/P", skim_js8_sender(&f, call, sizeof call) && !strcmp(call, "VK2XYZ/P"));
  skim_js8_unpack("TBx2Q+APtVaJ", 3, &f);
  check("sender of a directed frame: its from (OK1HRA), not its to",
        skim_js8_sender(&f, call, sizeof call) && !strcmp(call, "OK1HRA"));
  skim_js8_unpack("B801IRK2CSlu", 1, &f);
  check("sender of a compound frame: PA/OK1ABC", skim_js8_sender(&f, call, sizeof call) &&
                                                     !strcmp(call, "PA/OK1ABC"));
  skim_js8_unpack("KkmQZSal8+Je", 2, &f);
  check("a compound-directed frame alone names no sender (only its addressee)",
        !skim_js8_sender(&f, call, sizeof call));
  skim_js8_unpack("XpFFxeu2F+++", 2, &f);
  check("a data frame names no sender", !skim_js8_sender(&f, call, sizeof call));
  skim_js8_pack_directed("<....>", "W1AW", 28, 0, raw);
  skim_js8_unpack(raw, 3, &f);
  check("a directed frame from <....> names no sender", !strcmp(f.from, "<....>") &&
                                                            !skim_js8_sender(&f, call, sizeof call));

  /* the helper's lines */
  SkimJs8Decode d;
  check("line: 143015 0 -18 0.32 1234.5 3 0.917 31-QkpgqOT6W",
        skim_js8_parse_line("143015 0 -18 0.32 1234.5 3 0.917 31-QkpgqOT6W", &d) && d.sub == 0 &&
            d.snr == -18 && fabs(d.dt - 0.32) < 1e-9 && fabs(d.df - 1234.5) < 1e-9 && d.type == 3 &&
            fabs(d.quality - 0.917) < 1e-6 && !strcmp(d.f.from, "OK1HRA"));
  check("line: slow is JS8's submode 4, index 3",
        skim_js8_parse_line("143000 4 -26 0.10 800.0 3 0.700 31-QkpgqOT6W", &d) && d.sub == 3);
  check("line: <DecodeFinished> is none", !skim_js8_parse_line("<DecodeFinished>", &d));
  check("line: ultra (8) is not one of ours", !skim_js8_parse_line("143015 8 -18 0.3 1234.5 3 0.9 31-QkpgqOT6W", &d));
  check("line: junk is none", !skim_js8_parse_line("143015 0 -18 x", &d));

  guint bits = 0;
  check("submodes: \"normal,slow\" = bits 0 and 3", skim_js8_parse_submodes("normal,slow", &bits) &&
                                                       bits == (SKIM_JS8_M_NORMAL | SKIM_JS8_M_SLOW));
  check("submodes: \"normal, fast turbo,slow\" = all",
        skim_js8_parse_submodes("normal, fast turbo,slow", &bits) && bits == SKIM_JS8_M_ALL);
  check("submodes: \"ultra\" is refused", !skim_js8_parse_submodes("ultra", &bits));
  check("submodes: none is refused", !skim_js8_parse_submodes("", &bits));
  check("20m JS8 14078 inside 14045.5 at 96 kHz (14078.2-14081.5 ≤ 14088.7)",
        skim_js8_covers(14045500, 96000, 14078000, 200, 3500));
  check("20m JS8 14078 not inside 14024 at 48 kHz", !skim_js8_covers(14024000, 48000, 14078000, 200, 3500));
}

/* ---- the synthetic air -------------------------------------------------------------- */

typedef struct {
  char   raw[13];
  int    type;
  guint  sub;                           /* index 0..3                             */
  double rf;                            /* the tone-0 frequency, Hz (RF)          */
  double snr;                           /* dB in 2500 Hz                          */
  guint  periods;                       /* sent in this many periods from the first */
  int    t[79];
  guint  nt;
  double ph;
} Sig;

static const double START_S[] = { 0.5, 0.2, 0.1, 0.5 };
static const double NSPS[] = { 1920, 1200, 600, 3840 };

static guint tones(guint sub, int type, const char *raw, int *t) {
  char m[8], ty[8];
  g_snprintf(m, sizeof m, "%d", skim_js8_sub_number(sub));
  g_snprintf(ty, sizeof ty, "%d", type);
  const char *argv[] = { js8code, m, ty, raw, NULL };
  char *out = NULL;
  int st = 0;
  if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                    &st, NULL)) {
    return 0;
  }
  guint n = 0;
  for (const char *p = out; p && *p && n < 79; p++) {
    if (*p >= '0' && *p <= '7') { t[n++] = *p - '0'; }
  }
  g_free(out);
  return n;
}

static double gauss(GRand *r) {
  const double u = g_rand_double_range(r, 1e-12, 1), v = g_rand_double(r);
  return sqrt(-2 * log(u)) * cos(2 * G_PI * v);
}

/* IQ from base_s − 2 s for secs (base_s on a minute: every submode's period
 * starts there), in blocks of 1024 frames pushed with the time of their last
 * frame (one block in ten up to 30 ms late). Noise: I and Q of unit
 * variance over the rate. `drain`: wait for the decoders every 3 s of air,
 * as real time would (else the periods pile up and are skipped). */
static void air(SkimHdJs8 *j, double rate, gint64 base_s, double secs, Sig *sig, guint nsig,
                guint seed, gboolean drain) {
  GRand *r = g_rand_new_with_seed(seed);
  const guint blk = 1024;
  float *iq = g_new(float, 2 * blk);
  const guint64 total = (guint64)(secs * rate);
  for (guint k = 0; k < nsig; k++) {
    sig[k].ph = 0;
    sig[k].nt = tones(sig[k].sub, sig[k].type, sig[k].raw, sig[k].t);
  }
  for (guint64 f0 = 0; f0 < total; f0 += blk) {
    for (guint i = 0; i < blk; i++) {
      const double t = -2.0 + (double)(f0 + i) / rate;
      double re = gauss(r), im = gauss(r);
      for (guint k = 0; k < nsig; k++) {
        Sig *s = &sig[k];
        if (t < 0 || s->nt != 79) { continue; }
        const double period = skim_js8_sub_period_ms(s->sub) / 1000.0;
        const double df = SKIM_JS8_RATE / NSPS[s->sub];
        const guint cyc = (guint)floor(t / period);
        const double ts = t - cyc * period - START_S[s->sub];
        const int sym = (int)floor(ts * SKIM_JS8_RATE / NSPS[s->sub]);
        if (cyc >= s->periods || ts < 0 || sym >= 79) { continue; }
        const double fr = s->rf + df * s->t[sym] - CENTRE;
        s->ph += 2 * G_PI * fr / rate;
        const double a = sqrt(pow(10, s->snr / 10) * 2500.0 * 2.0 / rate);
        re += a * cos(s->ph);
        im += a * sin(s->ph);
      }
      iq[2 * i] = (float)re;
      iq[2 * i + 1] = (float)im;
    }
    gint64 t_end = base_s * G_USEC_PER_SEC + (gint64)((-2.0 + (double)(f0 + blk - 1) / rate) * 1e6);
    if (g_rand_int_range(r, 0, 10) == 0) { t_end += g_rand_int_range(r, 0, 30000); }
    skim_hd_js8_push_at(j, 0, iq, blk, t_end);
    if (drain && !g_getenv("SKIM_JS8_TEST_NODRAIN") && (f0 / blk) % (guint64)(3 * rate / blk) == 0) { skim_hd_js8_drain(j); }
  }
  g_free(iq);
  g_rand_free(r);
}

/* ---- the spots ---------------------------------------------------------------------- */

static GMutex spot_lock;
static GString *spots;

static void spot_cb(const char *call, double freq_hz, int snr, gboolean cq, const char *band,
                    const char *grid, gint64 t, gboolean confirmed, gpointer user) {
  (void)user; (void)t; (void)snr;
  g_mutex_lock(&spot_lock);
  g_string_append_printf(spots, "%s %.0f %s %s %s %s;", call, freq_hz, cq ? "CQ" : "DE", band,
                         grid && *grid ? grid : "-", confirmed ? "FEED" : "PAGE");
  g_mutex_unlock(&spot_lock);
}

static guint count(const char *hay, const char *needle) {
  guint n = 0;
  for (const char *p = hay; (p = strstr(p, needle)); p++) { n++; }
  return n;
}

/* a spot of call: its frequency within 3 Hz of hz and the rest exactly */
static gboolean spot_near(const char *call, double hz, const char *rest) {
  char *needle = g_strdup_printf("%s ", call);
  g_mutex_lock(&spot_lock);
  gboolean ok = FALSE;
  for (const char *p = spots->str; !ok && (p = strstr(p, needle)); p++) {
    if (p != spots->str && p[-1] != ';') { continue; }
    double f = 0;
    int used = 0;
    if (sscanf(p + strlen(needle), "%lf %n", &f, &used) == 1 && fabs(f - hz) <= 3) {
      const char *r = p + strlen(needle) + used;
      ok = strncmp(r, rest, strlen(rest)) == 0;
    }
  }
  g_mutex_unlock(&spot_lock);
  g_free(needle);
  return ok;
}

static gboolean has_spot(const char *s) {
  g_mutex_lock(&spot_lock);
  const gboolean ok = strstr(spots->str, s) != NULL;
  g_mutex_unlock(&spot_lock);
  return ok;
}

/* ---- end to end --------------------------------------------------------------------- */

static Sig *stations(guint *n) {
  static Sig sig[5];
  memset(sig, 0, sizeof sig);
  skim_js8_pack_heartbeat("OK1HRA", "JO70", FALSE, 0, sig[0].raw);       /* normal, 2 periods */
  sig[0].type = 3; sig[0].sub = 0; sig[0].rf = DIAL + 1000; sig[0].snr = -10; sig[0].periods = 2;
  skim_js8_pack_heartbeat("DL1ABC", "JO62", TRUE, 1, sig[1].raw);        /* fast */
  sig[1].type = 3; sig[1].sub = 1; sig[1].rf = DIAL + 1600; sig[1].snr = -8; sig[1].periods = 1;
  skim_js8_pack_directed("K1ABC", "W1AW", 25, -7 + 31, sig[2].raw);      /* turbo */
  sig[2].type = 3; sig[2].sub = 2; sig[2].rf = DIAL + 2200; sig[2].snr = -6; sig[2].periods = 1;
  skim_js8_pack_heartbeat("VK2XYZ", "QF56", FALSE, 0, sig[3].raw);       /* slow */
  sig[3].type = 3; sig[3].sub = 3; sig[3].rf = DIAL + 2800; sig[3].snr = -12; sig[3].periods = 1;
  /* below the dial, loud: USB must not hear it (its mirror: +1500 Hz) */
  skim_js8_pack_heartbeat("JA1XYZ", "PM95", FALSE, 0, sig[4].raw);
  sig[4].type = 3; sig[4].sub = 0; sig[4].rf = DIAL - 1500; sig[4].snr = 0; sig[4].periods = 2;
  *n = 5;
  return sig;
}

static void end_to_end(double rate, gboolean serve, const char *dir) {
  g_mutex_lock(&spot_lock);
  g_string_truncate(spots, 0);
  g_mutex_unlock(&spot_lock);
  char *tag = g_strdup_printf("%.0f kHz, %s", rate / 1e3, serve ? "kept running" : "once a period");
  const char *names[] = { "20m" };
  const double centres[] = { CENTRE };
  const SkimHdJs8Config cfg = { .submodes = SKIM_JS8_M_ALL, .lo_hz = 200, .hi_hz = 3500,
                                .threads = 2, .serve = serve, .clean_quality = 0.75, .dir = dir,
                                .decoder = js8d, .no_known = TRUE, .spot = spot_cb };
  SkimHdJs8 *j = skim_hd_js8_new(&cfg, 1, names, centres, rate);
  char *dials = skim_hd_js8_dials_text(j);
  char *what = g_strdup_printf("%s: the receiver takes 20m JS8 (%s)", tag, dials);
  check(what, strcmp(dials, "20m 14078.0") == 0);
  g_free(what);
  g_free(dials);

  guint nsig;
  Sig *sig = stations(&nsig);
  const gint64 base_s = (g_get_real_time() / G_USEC_PER_SEC / 60) * 60 - 120;
  air(j, rate, base_s, 2.0 + 30.6, sig, nsig, (guint)rate, TRUE);
  skim_hd_js8_drain(j);
  char *all = skim_hd_js8_frames_json(j, 0, 0);
  char *st = skim_hd_js8_status_json(j);
  g_print("     %s\n     %s\n", all, st);

  what = g_strdup_printf("%s: normal: OK1HRA's heartbeat in both periods", tag);
  check(what, count(all, "\"sub\":0,") == 2 && count(all, "\"raw\":\"31-QkpgqOT6W\"") == 2);
  g_free(what);
  what = g_strdup_printf("%s: fast: DL1ABC's CQ once", tag);
  check(what, count(all, "\"sub\":1,") == 1 && strstr(all, "\"text\":\"DL1ABC: @ALLCALL CQ DX JO62\""));
  g_free(what);
  what = g_strdup_printf("%s: turbo: K1ABC: W1AW SNR -07 once", tag);
  check(what, count(all, "\"sub\":2,") == 1 && strstr(all, "\"text\":\"K1ABC: W1AW SNR -07\""));
  g_free(what);
  what = g_strdup_printf("%s: slow: VK2XYZ's heartbeat once", tag);
  check(what, count(all, "\"sub\":4,") == 1 && strstr(all, "\"call\":\"VK2XYZ\""));
  g_free(what);
  what = g_strdup_printf("%s: the station below the dial is not heard (no mirror), nothing else", tag);
  check(what, !strstr(all, "JA1XYZ") && count(all, "\"raw\":") == 5);
  g_free(what);
  {
    const char *p = strstr(all, "\"sub\":2,");
    double df = 0;
    const gboolean ok = p && sscanf(strstr(p, "\"df\":"), "\"df\":%lf", &df) == 1;
    what = g_strdup_printf("%s: K1ABC at %.1f Hz (2200)", tag, df);
    check(what, ok && fabs(df - 2200) <= 3);
    g_free(what);
  }
  g_mutex_lock(&spot_lock);
  g_print("     spots: %s\n", spots->str);
  const guint nspots = count(spots->str, ";");
  g_mutex_unlock(&spot_lock);
  what = g_strdup_printf("%s: four spots, all to the feed (clean) — OK1HRA's second period folded", tag);
  check(what, nspots == 4 && count(spots->str, "FEED;") == 4);
  g_free(what);
  what = g_strdup_printf("%s: OK1HRA 14079000 CQ (a heartbeat) JO70; DL1ABC 14079600 CQ JO62", tag);
  check(what, spot_near("OK1HRA", 14079000, "CQ 20m JO70 FEED;") && spot_near("DL1ABC", 14079600, "CQ 20m JO62 FEED;"));
  g_free(what);
  what = g_strdup_printf("%s: K1ABC 14080200 DE (directed), no locator; VK2XYZ 14080800 CQ QF57", tag);
  check(what, spot_near("K1ABC", 14080200, "DE 20m - FEED;") && spot_near("VK2XYZ", 14080800, "CQ 20m QF57 FEED;"));
  g_free(what);
  what = g_strdup_printf("%s: the status: 2+3+5+1 periods, 5 frames, 4 spots, the 20m row with its "
                         "4 senders", tag);
  check(what, strstr(st, "\"cycles\":11,") && strstr(st, "\"decodes\":5,") &&
                  strstr(st, "\"spots\":4,") && strstr(st, "\"skipped\":0,") &&
                  strstr(st, "{\"band\":\"20m\",\"stations\":4,") && strstr(st, "\"error\":null"));
  g_free(what);
  char *d = skim_hd_js8_frames_json(j, DIAL, base_s * 1000 + 15000);
  what = g_strdup_printf("%s: the frames since the second normal period: its heartbeat and later ones", tag);
  check(what, count(d, "\"raw\":\"31-QkpgqOT6W\"") == 1);
  g_free(what);
  g_free(d);
  g_free(all);
  g_free(st);
  skim_hd_js8_free(j);
  g_free(tag);
}

/* Every frame taken as deep (clean quality above 1): the page at once, the
 * feed once confirmed. */
static void deep(const char *dir) {
  g_mutex_lock(&spot_lock);
  g_string_truncate(spots, 0);
  g_mutex_unlock(&spot_lock);
  char *scp = g_build_filename(dir, "MASTER.SCP", NULL);
  g_file_set_contents(scp, "# test\nK1ABC\nW1AW\n", -1, NULL);
  check("deep: a MASTER.SCP with K1ABC loads", skim_callsign_dict_load(scp, NULL));
  char *state = g_build_filename(dir, "deep", NULL);
  g_mkdir_with_parents(state, 0755);
  const char *names[] = { "20m" };
  const double centres[] = { CENTRE };
  SkimHdJs8Config cfg = { .submodes = SKIM_JS8_M_ALL, .lo_hz = 200, .hi_hz = 3500, .threads = 2,
                          .serve = TRUE, .clean_quality = 1.01, .dir = state, .decoder = js8d,
                          .spot = spot_cb };
  SkimHdJs8 *j = skim_hd_js8_new(&cfg, 1, names, centres, 96000);
  guint nsig;
  Sig *sig = stations(&nsig);
  const gint64 base_s = (g_get_real_time() / G_USEC_PER_SEC / 60) * 60 - 180;
  air(j, 96000, base_s, 2.0 + 30.6, sig, nsig, 96, TRUE);
  skim_hd_js8_drain(j);
  char *st = skim_hd_js8_status_json(j);
  g_mutex_lock(&spot_lock);
  g_print("     %s\n     spots: %s\n", st, spots->str);
  g_mutex_unlock(&spot_lock);
  check("deep: OK1HRA to the page in its first period, to the feed in its second (confirmed)",
        spot_near("OK1HRA", 14079000, "CQ 20m JO70 PAGE;") && spot_near("OK1HRA", 14079000, "CQ 20m JO70 FEED;"));
  check("deep: K1ABC to the feed at once (MASTER.SCP)", spot_near("K1ABC", 14080200, "DE 20m - FEED;") &&
                                                           !spot_near("K1ABC", 14080200, "DE 20m - PAGE;"));
  check("deep: DL1ABC and VK2XYZ, heard once, the page only",
        spot_near("DL1ABC", 14079600, "CQ 20m JO62 PAGE;") && !spot_near("DL1ABC", 14079600, "CQ 20m JO62 FEED;") &&
            spot_near("VK2XYZ", 14080800, "CQ 20m QF57 PAGE;") && !spot_near("VK2XYZ", 14080800, "CQ 20m QF57 FEED;"));
  check("deep: the status counts 5 deep frames, 2 confirmed, 2 unconfirmed senders",
        strstr(st, "\"deep\":5,") && strstr(st, "\"confirmed\":2,") && strstr(st, "\"unconfirmed\":") &&
            strstr(st, "\"known\":2,"));
  g_free(st);
  skim_hd_js8_free(j);
  char *known = g_build_filename(state, "js8-known.tsv", NULL), *text = NULL;
  g_file_get_contents(known, &text, NULL, NULL);
  check("deep: js8-known.tsv keeps the confirmed calls (OK1HRA, K1ABC), not the others",
        text && strstr(text, "OK1HRA\t") && strstr(text, "K1ABC\t") && !strstr(text, "DL1ABC") &&
            !strstr(text, "VK2XYZ"));
  g_free(text);

  /* the next run reads it: OK1HRA is confirmed from its first period */
  g_mutex_lock(&spot_lock);
  g_string_truncate(spots, 0);
  g_mutex_unlock(&spot_lock);
  skim_callsign_dict_load("/nonexistent", NULL);
  j = skim_hd_js8_new(&cfg, 1, names, centres, 96000);
  st = skim_hd_js8_status_json(j);
  check("deep: a new run knows the 2 calls", strstr(st, "\"known\":2,") != NULL);
  g_free(st);
  sig = stations(&nsig);
  sig[0].periods = 1;
  air(j, 96000, base_s + 60, 2.0 + 16, sig, 1, 97, TRUE);
  skim_hd_js8_drain(j);
  g_mutex_lock(&spot_lock);
  g_print("     spots: %s\n", spots->str);
  g_mutex_unlock(&spot_lock);
  check("deep: … so OK1HRA goes to the feed from its first period",
        spot_near("OK1HRA", 14079000, "CQ 20m JO70 FEED;") && !has_spot("PAGE;"));
  skim_hd_js8_free(j);
  g_free(known);
  g_free(state);
  g_free(scp);
}

/* A decoder slower than the periods: one at work, the rest wait, and each
 * newer period of the dial skips the one before it. */
static void slow(const char *dir) {
  char *fake = g_build_filename(dir, "slow-js8d", NULL);
  g_file_set_contents(fake, "#!/bin/sh\nsleep 3\necho '<DecodeFinished>'\n", -1, NULL);
  g_chmod(fake, 0755);
  const char *names[] = { "20m" };
  const double centres[] = { CENTRE };
  const SkimHdJs8Config cfg = { .submodes = SKIM_JS8_M_NORMAL, .lo_hz = 200, .hi_hz = 3500,
                                .threads = 1, .serve = FALSE, .clean_quality = 0.75, .dir = dir,
                                .decoder = fake, .no_known = TRUE };
  SkimHdJs8 *j = skim_hd_js8_new(&cfg, 1, names, centres, 48000);
  const gint64 base_s = (g_get_real_time() / G_USEC_PER_SEC / 60) * 60 - 300;
  air(j, 48000, base_s, 2.0 + 60.5, NULL, 0, 7, FALSE);   /* four normal periods, faster than 3 s */
  skim_hd_js8_drain(j);
  char *st = skim_hd_js8_status_json(j);
  g_print("     %s\n", st);
  check("slow decoder: of 4 periods the 1st and the 4th decoded, the 2nd and 3rd skipped",
        strstr(st, "\"cycles\":2,") && strstr(st, "\"skipped\":2,") &&
            strstr(st, "\"submodes\":[{\"name\":\"normal\""));
  g_free(st);
  skim_hd_js8_free(j);
  g_free(fake);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    g_printerr("usage: %s SKIMMER-JS8D SKIMMER-JS8CODE\n", argv[0]);
    return 2;
  }
  js8d = argv[1];
  js8code = argv[2];
  spots = g_string_new(NULL);
  const char *only = g_getenv("SKIM_JS8_TEST_ONLY");      /* debugging: one part */
  if (!only || !strcmp(only, "frames")) { frames(); }
  char *dir = g_dir_make_tmp("skimmer-js8-test-XXXXXX", NULL);
  if (!only || !strcmp(only, "e2e")) {
    end_to_end(48000, TRUE, dir);
    end_to_end(192000, FALSE, dir);
  }
  if (!only || !strcmp(only, "deep")) { deep(dir); }
  if (!only || !strcmp(only, "slow")) { slow(dir); }
  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  if (system(rm) != 0) { g_printerr("could not remove %s\n", dir); }
  g_free(rm);
  g_free(dir);
  g_string_free(spots, TRUE);
  g_print("%s\n", fails ? "js8: FAIL" : "js8: all passed");
  return fails ? 1 : 0;
}
