/*
 * skimmer-visitors-test — the web page's visitors (src/headless/visitors.h)
 * and the sign-in gate in front of the data (src/headless/web.c).
 *
 * The callsign check; the cookie: signed, its call, its age, another key's
 * refused; a visit's frequencies (10 s or more, summed, the longest first)
 * and its end after 10 idle minutes; the refused sign-ins, capped a minute.
 * Then a real server on loopback with loopback NOT free: no data and no
 * waterfall without signing in, a bad call refused (and logged), a page
 * from elsewhere refused, a good call's cookie opens the data, a forged
 * one does not; free_from with loopback opens it again — but not through a
 * proxy.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "headless/visitors.h"
#include "headless/web.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void quiet(const gchar *d, GLogLevelFlags l, const gchar *m, gpointer u) {
  (void)d; (void)l; (void)u;
  if (g_getenv("VISITORS_VERBOSE")) { printf("    | %s\n", m); }
}

static char *slurp(const char *dir, const char *name) {
  char *p = g_build_filename(dir, name, NULL), *t = NULL;
  if (!g_file_get_contents(p, &t, NULL, NULL)) { t = g_strdup(""); }
  g_free(p);
  return t;
}

static guint lines(const char *t) {
  guint n = 0;
  for (; *t; t++) { n += *t == '\n'; }
  return n;
}

static void calls(void) {
  printf("the callsign\n");
  char c[SKIM_VIS_CALL_MAX + 1];
  const char *why = NULL;
  check("ok1abc → OK1ABC", skim_hd_visitor_call_ok(" ok1abc ", c, &why) && strcmp(c, "OK1ABC") == 0);
  check("OK1ABC/P passes", skim_hd_visitor_call_ok("OK1ABC/P", c, &why));
  check("F/OK1ABC passes", skim_hd_visitor_call_ok("f/ok1abc", c, &why) && strcmp(c, "F/OK1ABC") == 0);
  check("K1A (3 characters, a digit) passes", skim_hd_visitor_call_ok("K1A", c, &why));
  check("empty refused (no callsign entered)", !skim_hd_visitor_call_ok("  ", c, &why) &&
        strstr(why, "no callsign"));
  check("AB refused (too short)", !skim_hd_visitor_call_ok("AB", c, &why));
  check("ABC refused (no digit)", !skim_hd_visitor_call_ok("ABC", c, &why) && strstr(why, "digit"));
  check("123 refused (no letter)", !skim_hd_visitor_call_ok("123", c, &why));
  check("OK1<b> refused (characters)", !skim_hd_visitor_call_ok("OK1<b>", c, &why));
  check("16 characters refused", !skim_hd_visitor_call_ok("OK1ABCDEFGHIJKLM", c, &why));
  check("/OK1ABC and OK1//A refused", !skim_hd_visitor_call_ok("/OK1ABC", c, &why) &&
        !skim_hd_visitor_call_ok("OK1//A", c, &why));
}

static void tokens(const char *dir, const char *dir2) {
  printf("the cookie\n");
  SkimHdVisitors *v = skim_hd_visitors_new(dir, NULL);
  SkimHdVisitors *o = skim_hd_visitors_new(dir2, NULL);
  char call[SKIM_VIS_CALL_MAX + 1];
  const gint64 now = 1795000000;
  char *t = skim_hd_visitors_token(v, "OK1ABC", now);
  check("a token of ours: its call", skim_hd_visitors_check(v, t, now + 60, call) &&
        strcmp(call, "OK1ABC") == 0);
  check("… after 29 days still", skim_hd_visitors_check(v, t, now + 29 * 86400, call));
  check("… after 30 days no", !skim_hd_visitors_check(v, t, now + SKIM_VIS_MAX_AGE_S, call));
  check("another key's: refused", !skim_hd_visitors_check(o, t, now + 60, call));
  char *forged = g_strdup(t);
  forged[1] = 'X';                                    /* OX1ABC, same signature */
  check("the call changed: refused", !skim_hd_visitors_check(v, forged, now + 60, call));
  char *later = g_strdup_printf("OK1ABC.%" G_GINT64_FORMAT "%s", now + 100, strrchr(t, '.'));
  check("the time changed: refused", !skim_hd_visitors_check(v, later, now + 200, call));
  check("garbage refused", !skim_hd_visitors_check(v, "x", now, call) &&
        !skim_hd_visitors_check(v, "", now, call) && !skim_hd_visitors_check(v, NULL, now, call));
  skim_hd_visitors_free(v);
  v = skim_hd_visitors_new(dir, NULL);
  check("the key survives a restart", skim_hd_visitors_check(v, t, now + 60, call));
  char *kp = g_build_filename(dir, "visitor.key", NULL);
  GStatBuf st;
  check("visitor.key is 0600", g_stat(kp, &st) == 0 && (st.st_mode & 0777) == 0600);
  g_free(kp);
  g_free(later);
  g_free(forged);
  g_free(t);
  skim_hd_visitors_free(v);
  skim_hd_visitors_free(o);
}

static void visits(const char *dir) {
  printf("visits\n");
  SkimHdVisitors *v = skim_hd_visitors_new(dir, NULL);
  gint64 now = 1795000000;
  int c1, c2;                                          /* two viewers' ids   */
  skim_hd_visitors_seen(v, "tokA", "OK1ABC", "203.0.113.5", NULL, now);
  /* viewer 1: 20m around 14.025 for 30 s, audio at 14.02530 for 12 s of it,
   * then 9 s on 40m; viewer 2: 20m at 14.025 for 8 s more */
  skim_hd_visitors_view(v, "tokA", &c1, "20m", 14000000, 14050000, FALSE, 0, now);
  skim_hd_visitors_view(v, "tokA", &c1, "20m", 14000000, 14050000, TRUE, 14025300, now + 18);
  skim_hd_visitors_view(v, "tokA", &c1, "40m", 7000000, 7090000, FALSE, 0, now + 30);
  skim_hd_visitors_view(v, "tokA", &c2, "20m", 14000000, 14050000, FALSE, 0, now + 31);
  skim_hd_visitors_view(v, "tokA", &c1, NULL, 0, 0, FALSE, 0, now + 39);
  skim_hd_visitors_view(v, "tokA", &c2, NULL, 0, 0, FALSE, 0, now + 39);
  skim_hd_visitors_seen(v, "tokA", "OK1ABC", "203.0.113.5", NULL, now + 100);
  skim_hd_visitors_view(v, "tokB", &c1, "20m", 1, 2, FALSE, 0, now);  /* no visit: nothing */
  char *j = skim_hd_visitors_json(v, 10, now + 100);
  check("an open visit, its frequencies so far",
        strstr(j, "\"call\":\"OK1ABC\"") && strstr(j, "wf:20m@14.025=38,audio:20m@14.0253=12"));
  g_free(j);
  skim_hd_visitors_tick(v, now + 100 + SKIM_VIS_IDLE_S - 1);
  char *log = slurp(dir, "visitors.tsv");
  check("not ended before 10 idle minutes", lines(log) == 0);
  g_free(log);
  skim_hd_visitors_tick(v, now + 100 + SKIM_VIS_IDLE_S);
  log = slurp(dir, "visitors.tsv");
  check("ended after 10 idle minutes: one line", lines(log) == 1);
  char **f = g_strsplit(log, "\t", -1);
  check("start, end, 100 s, call, address, no proxy",
        g_strv_length(f) == 7 && strcmp(f[2], "100") == 0 && strcmp(f[3], "OK1ABC") == 0 &&
        strcmp(f[4], "203.0.113.5") == 0 && strcmp(f[5], "-") == 0 &&
        g_str_has_prefix(f[0], "2026-") && g_str_has_suffix(f[0], "Z"));
  check("20m 38 s (30 + 8), its audio 12 s; 40m's 9 s left out",
        g_strv_length(f) == 7 && strcmp(f[6], "wf:20m@14.025=38,audio:20m@14.0253=12\n") == 0);
  g_strfreev(f);
  g_free(log);
  /* a visit through a proxy, signed out */
  skim_hd_visitors_seen(v, "tokC", "DL1XYZ", "127.0.0.1", "198.51.100.7", now);
  skim_hd_visitors_leave(v, "tokC", now + 5);
  log = slurp(dir, "visitors.tsv");
  check("signed out: logged at once, with the forwarded-for",
        lines(log) == 2 && strstr(log, "DL1XYZ\t127.0.0.1\t198.51.100.7\t-\n"));
  g_free(log);
  /* the refused: 30 a minute, the rest counted */
  for (int i = 0; i < 35; i++) {
    skim_hd_visitors_failed(v, now + 3, "203.0.113.9", NULL, i ? "ab" : "x\ty\n<z>", "too short");
  }
  skim_hd_visitors_failed(v, now + 61, "203.0.113.9", NULL, "ok", "no digit");
  char *fl = slurp(dir, "visitors-failed.tsv");
  check("30 refused lines a minute, the 5 others counted, then the next minute's",
        lines(fl) == 32 && strstr(fl, "5 more refused sign-ins that minute"));
  check("what was typed escaped (no tab or line break)", strstr(fl, "x\\x09y\\x0a<z>") != NULL);
  g_free(fl);
  j = skim_hd_visitors_json(v, 5, now + 100);
  check("the managers' JSON: the last visits and refusals",
        strstr(j, "\"visits\":[{\"start\"") && strstr(j, "\"call\":\"DL1XYZ\"") &&
        strstr(j, "\"failed\":[{\"time\""));
  g_free(j);
  j = skim_hd_visitors_public_json(v, 24, now + 100);
  check("the public JSON: the visits of 24 h, call and seconds, no address",
        strstr(j, "\"call\":\"DL1XYZ\",") == NULL && strstr(j, "\"secs\":\"5\",\"call\":\"DL1XYZ\"") &&
        strstr(j, "\"call\":\"OK1ABC\"") && !strstr(j, "203.0.113") && !strstr(j, "wf:"));
  g_free(j);
  j = skim_hd_visitors_public_json(v, 24, now + 2 * 86400);
  check("… two days later: none", strstr(j, "\"visits\":[]") != NULL);
  g_free(j);
  skim_hd_visitors_seen(v, "tokD", "OK2QQ", "203.0.113.6", NULL, now);
  skim_hd_visitors_free(v);
  log = slurp(dir, "visitors.tsv");
  check("a stop logs the open visit", lines(log) == 3 && strstr(log, "OK2QQ"));
  g_free(log);
}

/* ---- the gate, over HTTP ------------------------------------------------------ */

typedef struct {
  guint16 port;
  GMainLoop *loop;
} Srv;

static Srv srv;

/* One request; the status line's code, the body and the Set-Cookie value. */
static int http(const char *method, const char *path, const char *headers, const char *body,
                char **out_body, char **out_cookie) {
  GSocketClient *cl = g_socket_client_new();
  GSocketConnection *c = g_socket_client_connect_to_host(cl, "127.0.0.1", srv.port, NULL, NULL);
  g_object_unref(cl);
  if (!c) { return -1; }
  char *req = g_strdup_printf("%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%s%s%s"
                              "Connection: close\r\n\r\n%s", method, path, srv.port,
                              headers ? headers : "",
                              body ? "Content-Type: application/x-www-form-urlencoded\r\n" : "",
                              body ? g_strdup_printf("Content-Length: %zu\r\n", strlen(body)) : "",
                              body ? body : "");
  GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(c));
  g_output_stream_write_all(os, req, strlen(req), NULL, NULL, NULL);
  g_free(req);
  GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(c));
  GString *all = g_string_new(NULL);
  char buf[4096];
  gssize n;
  while ((n = g_input_stream_read(is, buf, sizeof(buf), NULL, NULL)) > 0) {
    g_string_append_len(all, buf, n);
  }
  g_object_unref(c);
  int code = -1;
  sscanf(all->str, "HTTP/1.%*d %d", &code);
  const char *b = strstr(all->str, "\r\n\r\n");
  if (out_body) { *out_body = g_strdup(b ? b + 4 : ""); }
  if (out_cookie) {
    const char *ck = strstr(all->str, "Set-Cookie: skv=");
    *out_cookie = ck ? g_strndup(ck + 16, strcspn(ck + 16, ";\r\n")) : NULL;
  }
  g_string_free(all, TRUE);
  return code;
}

/* answers who it was asked by: 0 from outside, 1 at home, 2 a manager */
static char *hook_status(GHashTable *q, SkimHdWebWho who, guint *st, const char **ty, gpointer u) {
  (void)q; (void)st; (void)ty; (void)u;
  return g_strdup_printf("{\"ok\":true,\"who\":%d}", (int)who);
}

static SkimHdWeb *web;
static SkimHdVisitors *gvis;
static const char *const NOT_LO[] = { "10.99.0.0/16", NULL };
static const char *const LO[] = { "127.0.0.0/8", NULL };

static gpointer client(gpointer data) {
  (void)data;
  char *body = NULL, *ck = NULL;
  check("no cookie: /status.json 401 login",
        http("GET", "/status.json", NULL, NULL, &body, NULL) == 401 && strstr(body, "login"));
  g_free(body);
  check("… /stats.json, /api/spots, /api/wspr, /api/ft8 and /api/js8 too",
        http("GET", "/stats.json", NULL, NULL, NULL, NULL) == 401 &&
        http("GET", "/api/spots", NULL, NULL, NULL, NULL) == 401 &&
        http("GET", "/api/wspr", NULL, NULL, NULL, NULL) == 401 &&
        http("GET", "/api/ft8", NULL, NULL, NULL, NULL) == 401 &&
        http("GET", "/api/js8", NULL, NULL, NULL, NULL) == 401);
  check("… the page itself is served", http("GET", "/", NULL, NULL, NULL, NULL) != 401);
  http("GET", "/api/visitor", NULL, NULL, &body, NULL);
  check("/api/visitor: need, no call", strstr(body, "\"need\":true") && strstr(body, "\"call\":null"));
  g_free(body);
  check("call=ab: 400, the reason", http("POST", "/api/visitor", NULL, "call=ab", &body, &ck) == 400 &&
        strstr(body, "3 characters") && !ck);
  g_free(body);
  check("a page from elsewhere may not sign in",
        http("POST", "/api/visitor", "Origin: http://evil.example\r\n", "call=OK1ABC", NULL, &ck) == 403 && !ck);
  /* the Origin must be this page: the server's Host */
  char *orig = g_strdup_printf("Origin: http://127.0.0.1:%u\r\n", srv.port);
  int code = http("POST", "/api/visitor", orig, "call=ok1abc", &body, &ck);
  check("call=ok1abc from this page: 200, OK1ABC, a cookie", code == 200 && strstr(body, "OK1ABC") &&
        ck && g_str_has_prefix(ck, "OK1ABC."));
  g_free(body);
  char *hdr = g_strdup_printf("Cookie: a=1; skv=%s\r\n", ck ? ck : "");
  check("with the cookie: /status.json 200, asked as from outside",
        http("GET", "/status.json", hdr, NULL, &body, NULL) == 200 && strstr(body, "\"who\":0"));
  g_free(body);
  http("GET", "/api/visitor", hdr, NULL, &body, NULL);
  check("/api/visitor: no need, the call", strstr(body, "\"need\":false") && strstr(body, "\"call\":\"OK1ABC\""));
  g_free(body);
  char *forged = g_strdup_printf("Cookie: skv=OK1ABD%s\r\n", ck ? ck + 6 : "");
  check("a forged cookie: 401", http("GET", "/status.json", forged, NULL, NULL, NULL) == 401);
  http("GET", "/api/visitors", hdr, NULL, &body, NULL);
  check("the visitors' log from outside: the last 24 h, calls and times, no address",
        body && strstr(body, "\"public\":true") && strstr(body, "\"call\":\"OK1ABC\"") &&
        !strstr(body, "addr") && !strstr(body, "failed"));
  g_free(body);
  check("… and without a cookie: 401", http("GET", "/api/visitors", NULL, NULL, NULL, NULL) == 401);
  check("SkimSrv.ini's bands: 403 for a visitor", http("GET", "/api/skimsrv", hdr, NULL, NULL, NULL) == 403);
  g_free(forged);
  g_free(hdr);
  g_free(orig);
  g_free(ck);
  /* loopback free now */
  skim_hd_web_set_visitors(web, gvis, LO);
  check("loopback in free_from: 200 without a cookie, asked as at home",
        http("GET", "/status.json", NULL, NULL, &body, NULL) == 200 && strstr(body, "\"who\":1"));
  g_free(body);
  http("GET", "/api/visitors", NULL, NULL, &body, NULL);
  check("… the visitors' log whole at home (addresses, refusals)",
        body && strstr(body, "\"addr\":") && strstr(body, "\"failed\":") && !strstr(body, "\"public\""));
  g_free(body);
  check("… but not through a proxy",
        http("GET", "/status.json", "X-Forwarded-For: 198.51.100.7\r\n", NULL, NULL, NULL) == 401);
  skim_hd_web_set_visitors(web, NULL, LO);
  check("sign-in off: everyone has the data — from outside without the machine readings",
        http("GET", "/status.json", "X-Forwarded-For: 198.51.100.7\r\n", NULL, &body, NULL) == 200 &&
        strstr(body, "\"who\":0"));
  g_free(body);
  check("… at home with them", http("GET", "/status.json", NULL, NULL, &body, NULL) == 200 &&
        strstr(body, "\"who\":1"));
  g_free(body);
  g_main_loop_quit(srv.loop);
  return NULL;
}

static void gate(const char *dir) {
  printf("the sign-in gate\n");
  gvis = skim_hd_visitors_new(dir, NULL);
  const SkimHdWebHooks hooks = { .status = hook_status, .stats = hook_status, .spots = hook_status };
  static const char *const MGR[] = { "10.98.0.1", NULL };          /* nobody here manages */
  for (int tries = 0; tries < 20 && !web; tries++) {
    srv.port = (guint16)(30000 + g_random_int_range(0, 20000));
    web = skim_hd_web_new(srv.port, MGR, &hooks, NULL);
  }
  if (!web) {
    check("a port for the server", FALSE);
    return;
  }
  skim_hd_web_set_visitors(web, gvis, NOT_LO);
  srv.loop = g_main_loop_new(NULL, FALSE);
  GThread *t = g_thread_new("client", client, NULL);
  g_main_loop_run(srv.loop);
  g_thread_join(t);
  g_main_loop_unref(srv.loop);
  skim_hd_web_free(web);
  char *fl = slurp(dir, "visitors-failed.tsv");
  check("the refused sign-in logged with what was typed", strstr(fl, "\tab\t") != NULL);
  g_free(fl);
  skim_hd_visitors_free(gvis);
}

int main(void) {
  g_log_set_default_handler(quiet, NULL);
  char *d1 = g_dir_make_tmp("skim-vis-XXXXXX", NULL), *d2 = g_dir_make_tmp("skim-vis-XXXXXX", NULL);
  char *d3 = g_dir_make_tmp("skim-vis-XXXXXX", NULL), *d4 = g_dir_make_tmp("skim-vis-XXXXXX", NULL);
  calls();
  tokens(d1, d2);
  visits(d3);
  gate(d4);
  char *rm = g_strdup_printf("rm -rf '%s' '%s' '%s' '%s'", d1, d2, d3, d4);
  if (system(rm) != 0) { /* left in /tmp */ }
  g_free(rm);
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
