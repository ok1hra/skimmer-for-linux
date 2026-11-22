/* wspr_test.c — the WSPR gate: the dials a receiver takes, wsprd's lines,
 * the WSPRnet form, and end to end — two synthetic WSPR signals (wsprsim's
 * encoder, vendor/wsprd) in noise, in IQ at 48 and 192 kHz on its own
 * clock, through the shift and the two resamplers, the capture at the even
 * minute, the .c2 file and skimmer-wsprd (argv[1]): both calls, their
 * locators, powers and frequencies come back. Last, the whole SkimHdWspr:
 * its decoder thread, the stations of 24 h, and the upload to a local
 * stand-in for WSPRnet.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <string.h>

#include "headless/wspr.h"
#include "wsprsim_utils.h"

int printdata = 0;                     /* wsprsim.c's, which the encoder reads */

static int fails;
static void check(const char *what, gboolean ok) {
  g_print("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) { fails++; }
}

static const char *decoder;

/* ---- the synthetic air -------------------------------------------------------------- */

typedef struct {
  const char *msg;
  double audio;                         /* Hz from the WSPR centre (dial + 1500) */
  double snr;                           /* dB in 2500 Hz                          */
  unsigned char sym[162];
  double ph;
} Sig;

#define DIAL   14095600.0
#define CENTRE 14090000.0

static void encode(Sig *s) {
  char *hash = g_malloc0(32768 * 13), *loc = g_malloc0(32768 * 5);
  char m[32];
  g_strlcpy(m, s->msg, sizeof m);
  get_wspr_channel_symbols(m, hash, loc, s->sym);
  g_free(hash);
  g_free(loc);
}

static double gauss(GRand *r) {
  const double u = g_rand_double_range(r, 1e-12, 1), v = g_rand_double(r);
  return sqrt(-2 * log(u)) * cos(2 * G_PI * v);
}

/* IQ from cycle start − 2 s to + 116 s, in blocks, each with its time:
 * feed(user, iq, n, t_end_us). The signals start at + 1 s (WSPR's own). */
typedef void (*Feed)(gpointer user, const float *iq, guint n, gint64 t_end_us);
static void air(double rate, gint64 cycle_s, Sig *sig, guint nsig, Feed feed, gpointer user) {
  GRand *r = g_rand_new_with_seed(7);
  const double A = 0.01, df = 12000.0 / 8192, tsym = 8192.0 / 12000;
  const guint blk = 1024;
  float *iq = g_new(float, 2 * blk);
  const double t0 = -2.0;
  const guint64 total = (guint64)(118 * rate);
  /* white noise over the whole rate; its share in 2500 Hz sets each SNR */
  const double sigma = A * sqrt(pow(10, -sig[0].snr / 10) * rate / 2500) / sqrt(2);
  for (guint64 f0 = 0; f0 < total; f0 += blk) {
    for (guint i = 0; i < blk; i++) {
      const double t = t0 + (f0 + i) / rate;
      double re = sigma * gauss(r), im = sigma * gauss(r);
      for (guint k = 0; k < nsig; k++) {
        Sig *s = &sig[k];
        const double ts = t - 1.0;
        if (ts < 0 || ts >= 162 * tsym) { continue; }
        const int n = (int)(ts / tsym);
        const double f = DIAL + 1500 + s->audio + (s->sym[n] - 1.5) * df - CENTRE;
        s->ph = fmod(s->ph + 2 * G_PI * f / rate, 2 * G_PI);
        const double a = A * pow(10, (s->snr - sig[0].snr) / 20);
        re += a * cos(s->ph);
        im += a * sin(s->ph);
      }
      iq[2 * i] = (float)re;
      iq[2 * i + 1] = (float)im;
    }
    /* the arrival: its time, a little late now and then (jitter) */
    const double jit = g_rand_int_range(r, 0, 10) == 0 ? g_rand_double_range(r, 0, 0.03) : 0;
    const gint64 t_end = cycle_s * G_USEC_PER_SEC +
                         (gint64)((t0 + (f0 + blk - 1) / rate + jit) * 1e6);
    feed(user, iq, blk, t_end);
  }
  g_free(iq);
  g_rand_free(r);
}

/* ---- the DSP and the decoder ------------------------------------------------------- */

typedef struct {
  gint64 start;
  float *iq;
  guint  n;
} Got;

static void got_cb(gint64 start_s, float *iq, gpointer user) {
  Got *g = user;
  g->n++;
  g->start = start_s;
  g_free(g->iq);
  g->iq = iq;
}

static void rx_feed(gpointer user, const float *iq, guint n, gint64 t_end_us) {
  skim_wspr_rx_feed(user, iq, n, t_end_us);
}

static const SkimWsprSpot *find(GArray *a, const char *call) {
  for (guint i = 0; i < a->len; i++) {
    const SkimWsprSpot *s = &g_array_index(a, SkimWsprSpot, i);
    if (strcmp(s->call, call) == 0) { return s; }
  }
  return NULL;
}

static void end_to_end(double rate, const char *dir) {
  const gint64 cycle = 1795000080;      /* an even minute: 2026-11-18 11:08 UTC */
  g_assert(cycle % 120 == 0);
  Sig sig[2] = { { "OK1HRA JO70 37", 20, -18, { 0 }, 0 }, { "K1ABC FN42 30", -60, -22, { 0 }, 0 } };
  encode(&sig[0]);
  encode(&sig[1]);
  Got g = { 0 };
  SkimWsprRx *rx = skim_wspr_rx_new(rate, CENTRE, DIAL, got_cb, &g);
  air(rate, cycle, sig, 2, rx_feed, rx);
  skim_wspr_rx_free(rx);
  char *what = g_strdup_printf("%.0f kHz: one capture, at the even minute", rate / 1e3);
  check(what, g.n == 1 && g.start == cycle);
  g_free(what);
  if (!g.iq) { return; }
  GError *e = NULL;
  char *c2 = skim_wspr_write_c2(dir, g.start, DIAL, g.iq, &e);
  check("the .c2 file: YYMMDD_HHMM.c2, 14 + 4 + 8 + 45000 × 8 bytes",
        c2 && g_str_has_suffix(c2, "/261118_1108.c2") && ({
          GStatBuf st;
          g_stat(c2, &st) == 0 && st.st_size == 14 + 4 + 8 + 45000 * 8;
        }));
  GArray *spots = g_array_new(FALSE, TRUE, sizeof(SkimWsprSpot));
  const gboolean ok = c2 && skim_wspr_decode_file(decoder, dir, c2, g.start, "20m", DIAL, 0, spots,
                                                  NULL, &e);
  if (e) {
    g_print("  decoder: %s\n", e->message);
    g_clear_error(&e);
  }
  const SkimWsprSpot *a = find(spots, "OK1HRA"), *b = find(spots, "K1ABC");
  for (guint i = 0; i < spots->len; i++) {
    const SkimWsprSpot *s = &g_array_index(spots, SkimWsprSpot, i);
    g_print("  decoded: %s %s %d dBm, %.1f Hz, SNR %d, DT %.1f, drift %d\n", s->call, s->grid,
            s->dbm, s->freq_hz, s->snr, s->dt, s->drift);
  }
  what = g_strdup_printf("%.0f kHz: OK1HRA JO70 37 at +20 Hz (±2), DT within ±0.5 s, SNR about -18",
                         rate / 1e3);
  check(what, ok && a && strcmp(a->grid, "JO70") == 0 && a->dbm == 37 &&
                  fabs(a->freq_hz - (DIAL + 1520)) <= 2 && fabs(a->dt) <= 0.5 &&
                  abs(a->snr + 18) <= 3 && a->t == cycle && strcmp(a->band, "20m") == 0);
  g_free(what);
  what = g_strdup_printf("%.0f kHz: K1ABC FN42 30 at -60 Hz (the weaker one)", rate / 1e3);
  check(what, b && strcmp(b->grid, "FN42") == 0 && b->dbm == 30 && fabs(b->freq_hz - (DIAL + 1440)) <= 2);
  g_free(what);
  check("nothing else decoded", spots->len == 2);
  g_array_unref(spots);
  /* the options: a deeper search in one pass (-d -s) still decodes both */
  spots = g_array_new(FALSE, TRUE, sizeof(SkimWsprSpot));
  const gboolean ok2 = c2 && skim_wspr_decode_file(decoder, dir, c2, g.start, "20m", DIAL,
                                                   SKIM_WSPR_DEEP | SKIM_WSPR_NO_SUBTRACT, spots,
                                                   NULL, &e);
  g_clear_error(&e);
  what = g_strdup_printf("%.0f kHz: -d -s (deeper search, no subtraction) decodes both too", rate / 1e3);
  check(what, ok2 && find(spots, "OK1HRA") && find(spots, "K1ABC"));
  g_free(what);
  g_array_unref(spots);
  if (c2) { g_unlink(c2); }
  g_free(c2);
  g_free(g.iq);
}

/* ---- the whole: thread, stations, upload ------------------------------------------- */

typedef struct {
  GPtrArray *bodies;
  guint      status;
} Fake;

static void fake_cb(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *q,
                    gpointer user) {
  (void)srv; (void)path; (void)q;
  Fake *f = user;
  SoupMessageBody *b = soup_server_message_get_request_body(msg);
  GBytes *by = soup_message_body_flatten(b);
  g_ptr_array_add(f->bodies, g_strndup(g_bytes_get_data(by, NULL), g_bytes_get_size(by)));
  g_bytes_unref(by);
  soup_server_message_set_status(msg, f->status, NULL);
  soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC, "1 out of 1 spot(s) added", 24);
}

typedef struct {
  SkimHdWspr *w;
} WFeed;

static void w_feed(gpointer user, const float *iq, guint n, gint64 t_end_us) {
  skim_hd_wspr_push_at(((WFeed *)user)->w, 0, iq, n, t_end_us);
}

static void whole(const char *dir) {
  Fake f = { g_ptr_array_new_with_free_func(g_free), 200 };
  SoupServer *srv = soup_server_new(NULL, NULL);
  soup_server_add_handler(srv, "/post", fake_cb, &f, NULL);
  GError *e = NULL;
  g_assert(soup_server_listen_local(srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &e));
  GSList *uris = soup_server_get_uris(srv);
  char *base = g_uri_to_string(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  char *url = g_strconcat(base, "post/", NULL);
  g_free(base);

  const char *names[] = { "20m-ish", "40m" };
  const double centres[] = { CENTRE, 7045500 };
  const SkimHdWsprConfig cfg = { .call = "OK1XYZ", .locator = "JO60AA", .upload = TRUE,
                                 .upload_url = url, .dir = dir, .decoder = decoder };
  SkimHdWspr *w = skim_hd_wspr_new(&cfg, 2, names, centres, 48000);
  char *dials = skim_hd_wspr_dials_text(w);
  check("the dials taken: 20m 14095.6 (RX1), 40m 7038.6 (RX2)",
        strcmp(dials, "20m 14095.6, 40m 7038.6") == 0);
  g_free(dials);

  /* a cycle of the last few minutes: wspr.tsv keeps 24 h of real time */
  const gint64 cycle = (g_get_real_time() / G_USEC_PER_SEC / 120 - 2) * 120;
  GDateTime *cd = g_date_time_new_from_unix_utc(cycle);
  char *when = g_date_time_format(cd, "&date=%y%m%d&time=%H%M&sig=");
  g_date_time_unref(cd);
  Sig sig[1] = { { "OK1HRA JO70 37", 20, -18, { 0 }, 0 } };
  encode(&sig[0]);
  WFeed wf = { w };
  air(48000, cycle, sig, 1, w_feed, &wf);
  /* the decoder thread decodes, then posts to the stand-in */
  char *st = NULL;
  const gint64 until = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
  while (g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_free(st);
    st = skim_hd_wspr_status_json(w);
    if (strstr(st, "\"uploaded\":1,")) { break; }
    g_usleep(20000);
  }
  g_print("  status: %s\n", st);
  check("status: one cycle, one decode, uploaded, none queued",
        strstr(st, "\"cycles\":1,") && strstr(st, "\"decodes\":1,") &&
        strstr(st, "\"uploaded\":1,") && strstr(st, "\"queued\":0,") && strstr(st, "\"error\":null"));
  check("status: the stations of 30 min per receiver (its own band name), the newest",
        strstr(st, "\"rx\":[{\"band\":\"20m-ish\",\"stations\":1,\"last\":\"OK1HRA\"},"
                   "{\"band\":\"40m\",\"stations\":0,\"last\":\"\"}],\"recent_s\":1800,") != NULL);
  g_free(st);
  const char *body = f.bodies->len ? f.bodies->pdata[0] : "";
  g_print("  posted: %s\n", body);
  check("the post: function=wspr, rcall/rgrid ours, the dial, the date and time, the spot, mode=2",
        g_str_has_prefix(body, "function=wspr&rcall=OK1XYZ&rgrid=JO60AA&rqrg=14.095600&") &&
        strstr(body, when) &&
        strstr(body, "&tqrg=14.097120&tcall=OK1HRA&tgrid=JO70&dbm=37&version=skimmer-") &&
        g_str_has_suffix(body, "&mode=2"));
  char *js = skim_hd_wspr_stations_json(w, 24, cycle + 600);
  g_print("  stations: %s\n", js);
  check("stations of 24 h: OK1HRA JO70, 20m, once",
        g_str_has_prefix(js, "{\"hours\":24,\"spots\":1,\"stations\":[{\"call\":\"OK1HRA\",\"grid\":\"JO70\","
                             "\"bands\":\"20m\",\"band\":\"20m\",\"dbm\":37,\"n\":1,"));
  g_free(js);
  js = skim_hd_wspr_stations_json(w, 24, cycle + 25 * 3600);
  check("… and 25 h later none", strstr(js, "\"spots\":0,\"stations\":[]") != NULL);
  g_free(js);
  skim_hd_wspr_free(w);

  /* wspr.tsv keeps them: a new start sees the station again */
  w = skim_hd_wspr_new(&(SkimHdWsprConfig){ .dir = dir, .decoder = decoder }, 2, names, centres, 48000);
  js = skim_hd_wspr_stations_json(w, 24, cycle + 600);
  check("wspr.tsv: after a restart OK1HRA is still there", strstr(js, "\"call\":\"OK1HRA\"") != NULL);
  g_free(js);
  st = skim_hd_wspr_status_json(w);
  check("no call/locator, no upload asked: upload off", strstr(st, "\"upload\":false") != NULL);
  g_free(st);
  skim_hd_wspr_free(w);

  g_free(when);
  g_object_unref(srv);
  g_ptr_array_unref(f.bodies);
  g_free(url);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    g_printerr("usage: %s SKIMMER-WSPRD\n", argv[0]);
    return 2;
  }
  decoder = argv[1];

  /* the dials a receiver takes */
  check("40m 7038.6 inside 7045.5 at 96 kHz", skim_wspr_covers(7045500, 96000, 7038600));
  check("20m 14095.6 not inside 14045.5 at 96 kHz (14097.1 > 14088.7)",
        !skim_wspr_covers(14045500, 96000, 14095600));
  check("… but at 192 kHz", skim_wspr_covers(14091000, 192000, 14095600));

  /* wsprd's lines */
  SkimWsprSpot s = { 0 };
  check("a type 1 line", skim_wspr_parse_line("2150 -21  0.3  14.097062  0  K1ABC FN42 37 ", &s) &&
                             strcmp(s.call, "K1ABC") == 0 && strcmp(s.grid, "FN42") == 0 &&
                             s.dbm == 37 && s.snr == -21 && fabs(s.dt - 0.3) < 1e-9 &&
                             fabs(s.freq_hz - 14097062) < 0.5 && s.drift == 0);
  check("type 2: a compound call, no locator",
        skim_wspr_parse_line("2150 -12 -0.8  14.097001 -1  PJ4/K1ABC 37 ", &s) &&
            strcmp(s.call, "PJ4/K1ABC") == 0 && !s.grid[0] && s.drift == -1);
  check("type 3: <call> from the hash table, a 6-character locator",
        skim_wspr_parse_line("2150 -25  1.1  14.097150  0  <PJ4/K1ABC> FK52UD 37 ", &s) &&
            strcmp(s.call, "PJ4/K1ABC") == 0 && strcmp(s.grid, "FK52UD") == 0);
  check("an unresolved hash <...> is no spot",
        !skim_wspr_parse_line("2150 -25  1.1  14.097150  0  <...> FK52UD 37 ", &s));
  check("<DecodeFinished> is no spot", !skim_wspr_parse_line("<DecodeFinished>", &s));
  check("garbage is no spot", !skim_wspr_parse_line("2150 -25 x", &s));

  /* the WSPRnet form */
  SkimWsprSpot p = { .t = 1795000080, .dial_hz = 14095600, .freq_hz = 14097062, .snr = -21,
                     .dt = 0.3, .drift = 0, .dbm = 37 };
  g_strlcpy(p.call, "PJ4/K1ABC", sizeof p.call);
  g_strlcpy(p.grid, "FK52", sizeof p.grid);
  char *body = skim_wspr_post_body(&p, "OK1HRA", "JO70", "skimmer-0.8.6");
  check("the post body (the / escaped)",
        strcmp(body, "function=wspr&rcall=OK1HRA&rgrid=JO70&rqrg=14.095600&date=261118&time=1108"
                     "&sig=-21&dt=0.3&drift=0&tqrg=14.097062&tcall=PJ4%2FK1ABC&tgrid=FK52&dbm=37"
                     "&version=skimmer-0.8.6&mode=2") == 0);
  g_free(body);

  char *dir = g_dir_make_tmp("skimmer-wspr-XXXXXX", NULL);
  end_to_end(48000, dir);
  end_to_end(192000, dir);
  char *wd = g_build_filename(dir, "w", NULL);
  whole(wd);
  char *rm = g_strdup_printf("rm -rf '%s'", dir);
  if (system(rm) != 0) { g_printerr("could not remove %s\n", dir); }
  g_free(rm);
  g_free(wd);
  g_free(dir);

  g_print("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
