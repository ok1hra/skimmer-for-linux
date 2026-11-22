/* pskreporter_test.c — the pskreporter gate (src/headless/pskreporter.h).
 *
 * The datagram byte for byte against https://pskreporter.info/pskdev.html:
 * its receiver record example (N1DQ, FN42hn, Homebrew v5.6), its two
 * templates, a sender record laid out field by field, a long list split over
 * datagrams under the limit, every set a multiple of 4. Then the uploader
 * against a local UDP listener: one source port, the observation domain
 * kept, the sequence number counting the reports, the templates in the
 * first three datagrams, a call once an hour a band and mode, a locator
 * heard before filled in, the status.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "headless/pskreporter.h"

static int fails;
static void check(const char *what, gboolean ok) {
  g_print("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) { fails++; }
}

static GBytes *hex(const char *s) {
  GByteArray *b = g_byte_array_new();
  for (const char *p = s; *p;) {
    while (*p == ' ') { p++; }
    if (!*p) { break; }
    guint v = 0;
    sscanf(p, "%2x", &v);
    const guint8 x = (guint8)v;
    g_byte_array_append(b, &x, 1);
    p += 2;
  }
  return g_byte_array_free_to_bytes(b);
}

static gboolean has_at(GBytes *d, gsize at, GBytes *want) {
  gsize n = 0, m = 0;
  const guint8 *a = g_bytes_get_data(d, &n), *w = g_bytes_get_data(want, &m);
  return at + m <= n && memcmp(a + at, w, m) == 0;
}

static guint count_of(const char *hay, const char *needle) {
  guint n = 0;
  for (const char *q = hay; (q = strstr(q, needle)); q++) { n++; }
  return n;
}

static guint g16(const guint8 *p) { return (guint)p[0] << 8 | p[1]; }
static guint32 g32(const guint8 *p) { return (guint32)p[0] << 24 | (guint32)p[1] << 16 | (guint32)p[2] << 8 | p[3]; }

/* A datagram's sets: is every set a multiple of 4, the lengths adding up;
 * the sender records decoded into calls (+ grid) "CALL/GRID;" and their count. */
typedef struct {
  guint version, len, nsets, nrec;
  guint32 seq, odid;
  gboolean aligned, tmpl;
  GString *calls;
} Dgram;

static gboolean parse(const guint8 *d, gsize n, Dgram *o) {
  memset(o, 0, sizeof *o);
  o->calls = g_string_new(NULL);
  if (n < 16) { return FALSE; }
  o->version = g16(d);
  o->len = g16(d + 2);
  o->seq = g32(d + 8);
  o->odid = g32(d + 12);
  o->aligned = TRUE;
  gsize at = 16;
  while (at + 4 <= n) {
    const guint id = g16(d + at), sl = g16(d + at + 2);
    if (sl < 4 || at + sl > n) { return FALSE; }
    o->aligned &= sl % 4 == 0;
    o->nsets++;
    if (id == 2 || id == 3) { o->tmpl = TRUE; }
    if (id == 0x9993) {
      gsize p = at + 4;
      while (p < at + sl && d[p]) {                 /* a record starts with a call length */
        const guint cl = d[p];
        g_string_append_len(o->calls, (const char *)d + p + 1, cl);
        p += 1 + cl + 4 + 1 + 1;                   /* call, freq, sNR, iMD           */
        p += 1 + d[p];                             /* mode                           */
        p += 1;                                    /* informationSource              */
        const guint gl = d[p];
        g_string_append_c(o->calls, '/');
        g_string_append_len(o->calls, (const char *)d + p + 1, gl);
        g_string_append_c(o->calls, ';');
        p += 1 + gl + 4;                           /* grid, flowStartSeconds         */
        o->nrec++;
      }
    }
    at += sl;
  }
  return at == n && o->len == n;
}

/* ---- the listener ------------------------------------------------------------------ */

static GSocket *listener(guint16 *port) {
  GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
  GInetAddress *lo = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  GSocketAddress *a = g_inet_socket_address_new(lo, 0);
  g_socket_bind(s, a, TRUE, NULL);
  g_object_unref(a);
  g_object_unref(lo);
  GSocketAddress *got = g_socket_get_local_address(s, NULL);
  *port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(got));
  g_object_unref(got);
  g_socket_set_timeout(s, 8);
  return s;
}

/* One datagram (≤ 8 s), and the sender's port. */
static gssize receive(GSocket *s, guint8 *buf, gsize n, guint16 *from_port) {
  GSocketAddress *from = NULL;
  const gssize r = g_socket_receive_from(s, &from, (char *)buf, n, NULL, NULL);
  if (from) {
    *from_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(from));
    g_object_unref(from);
  }
  return r;
}

int main(void) {
  /* ---- the datagram ---- */
  GBytes *rx_example = hex("99 92 00 20 04 4E 31 44 51 06 46 4E 34 32 68 6E 0D 48 6F 6D 65 62 72 65 77 "
                           "20 76 35 2E 36 00 00");
  guint used = 0;
  GBytes *d = skim_pskr_encode("N1DQ", "FN42hn", "Homebrew v5.6", 7, 0x01020304, 1200960114, FALSE,
                               NULL, 0, 1400, &used);
  GBytes *head = hex("00 0A 00 30 47 95 32 72 00 00 00 07 01 02 03 04");
  check("the header: version 10, length 48, the time, sequence 7, the domain",
        g_bytes_get_size(d) == 48 && has_at(d, 0, head));
  check("the receiver record exactly as the page's example (N1DQ, FN42hn, Homebrew v5.6)",
        has_at(d, 16, rx_example));
  g_bytes_unref(d);
  g_bytes_unref(head);

  GBytes *t_rx = hex("00 03 00 24 99 92 00 03 00 01 80 02 FF FF 00 00 76 8F 80 04 FF FF 00 00 76 8F "
                     "80 08 FF FF 00 00 76 8F 00 00");
  GBytes *t_tx = hex("00 02 00 44 99 93 00 08 80 01 FF FF 00 00 76 8F 80 05 00 04 00 00 76 8F 80 06 "
                     "00 01 00 00 76 8F 80 07 00 01 00 00 76 8F 80 0A FF FF 00 00 76 8F 80 0B 00 01 "
                     "00 00 76 8F 80 03 FF FF 00 00 76 8F 00 96 00 04");
  SkimPskrReport r = { .freq_hz = 14075000, .snr = -12, .t = 1795377600 };
  g_strlcpy(r.call, "OK1HRA", sizeof r.call);
  g_strlcpy(r.mode, "FT8", sizeof r.mode);
  g_strlcpy(r.grid, "JO70", sizeof r.grid);
  d = skim_pskr_encode("N1DQ", "FN42hn", "Homebrew v5.6", 0, 1, 1200960114, TRUE, &r, 1, 1400, &used);
  check("with the templates: the page's receiver template, then its sender template "
        "(call, frequency, sNR, iMD, mode, source, locator, time)",
        has_at(d, 16, t_rx) && has_at(d, 16 + 36, t_tx) && has_at(d, 16 + 36 + 68, rx_example));
  /* 14075000 = 0x00D6C478; -12 = F4; iMD 0; "FT8"; source 1; "JO70"; 1795377600 = 0x6B0349C0 */
  GBytes *rec = hex("99 93 00 20 06 4F 4B 31 48 52 41 00 D6 C4 78 F4 00 03 46 54 38 01 04 4A 4F 37 30 "
                    "6B 03 49 C0 00");
  check("the sender record field by field, padded to 4", used == 1 && has_at(d, 16 + 36 + 68 + 32, rec));
  Dgram g;
  const guint8 *dp = g_bytes_get_data(d, NULL);
  check("… and it parses: one record, OK1HRA/JO70, every set a multiple of 4",
        parse(dp, g_bytes_get_size(d), &g) && g.nrec == 1 && strcmp(g.calls->str, "OK1HRA/JO70;") == 0 &&
            g.aligned && g.tmpl);
  g_string_free(g.calls, TRUE);
  g_bytes_unref(d);
  g_bytes_unref(rec);

  SkimPskrReport many[200];
  for (guint i = 0; i < 200; i++) {
    many[i] = r;
    g_snprintf(many[i].call, sizeof many[i].call, "DL%uABC", i);
    many[i].grid[0] = i % 2 ? '\0' : 'J';
  }
  guint total = 0, parts = 0;
  gboolean fit = TRUE;
  while (total < 200) {
    d = skim_pskr_encode("OK1HRA", "JO70AB", "skimmer", total, 1, 1, parts == 0, many + total,
                         200 - total, 1400, &used);
    fit &= g_bytes_get_size(d) <= 1400 && used > 0 &&
           parse(g_bytes_get_data(d, NULL), g_bytes_get_size(d), &g) && g.nrec == used && g.aligned;
    g_string_free(g.calls, TRUE);
    total += used;
    parts++;
    g_bytes_unref(d);
  }
  char *w = g_strdup_printf("200 reports in %u datagrams, each ≤ 1400 bytes, its sets aligned", parts);
  check(w, fit && parts >= 3 && total == 200);
  g_free(w);

  /* ---- the uploader ---- */
  guint16 port = 0;
  GSocket *ls = listener(&port);
  const SkimPskrConfig cfg = { .call = "OK1HRA", .locator = "JO70AB", .software = "skimmer test",
                               .host = "127.0.0.1", .port = port, .interval_s = 1 };
  SkimPskr *p = skim_pskr_new(&cfg);
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  skim_pskr_add(p, "DL1ABC", "JN58", 14075100, -10, "FT8", "20m", now);
  skim_pskr_add(p, "JA1XYZ", "", 14076400, -3, "FT8", "20m", now);
  skim_pskr_add(p, "DL1ABC", "", 14075300, -9, "FT8", "20m", now + 30);    /* within the hour */
  skim_pskr_add(p, "DL1ABC", "", 14081200, -5, "FT4", "20m", now + 30);    /* another mode   */
  guint8 buf[2048];
  guint16 from1 = 0, from2 = 0, from3 = 0;
  gssize n = receive(ls, buf, sizeof buf, &from1);
  Dgram a;
  check("a datagram arrives", n > 0 && parse(buf, (gsize)n, &a));
  check("… the templates in it, the receiver record, three reports (DL1ABC FT8 once, "
        "its FT4 with the locator heard before, JA1XYZ without one)",
        a.tmpl && a.nrec == 3 && strstr(a.calls->str, "DL1ABC/JN58;") &&
            strstr(a.calls->str, "JA1XYZ/;") && count_of(a.calls->str, "DL1ABC/JN58;") == 2);
  g_print("     %s\n", a.calls->str);
  check("… sequence 0 first", a.seq == 0);
  skim_pskr_add(p, "VK2QSO", "QF56", 14074600, -18, "FT8", "20m", now + 40);
  Dgram b;
  n = receive(ls, buf, sizeof buf, &from2);
  check("the next datagram: sequence 3 (the reports so far), the same domain, the same source port",
        n > 0 && parse(buf, (gsize)n, &b) && b.seq == 3 && b.odid == a.odid && from2 == from1 && b.nrec == 1);
  check("… the templates again (the first three datagrams)", b.tmpl);
  skim_pskr_add(p, "W1AW", "FN31", 14075900, -7, "FT8", "20m", now + 50);
  Dgram c;
  n = receive(ls, buf, sizeof buf, &from3);
  check("the third: sequence 4, the templates a last time", n > 0 && parse(buf, (gsize)n, &c) &&
                                                               c.seq == 4 && c.tmpl && from3 == from1);
  skim_pskr_add(p, "K1ABC", "FN42", 14075950, -1, "FT8", "20m", now + 60);
  Dgram e;
  n = receive(ls, buf, sizeof buf, &from3);
  check("the fourth: no templates (sent three times, the hour not over)",
        n > 0 && parse(buf, (gsize)n, &e) && !e.tmpl && e.seq == 5 && e.nrec == 1);
  char *st = skim_pskr_status_json(p);
  g_print("     %s\n", st);
  check("the status: 6 reports in 4 datagrams, the locator as its square, no error",
        strstr(st, "\"sent\":6,\"datagrams\":4,\"queued\":0") && strstr(st, "\"locator\":\"JO70\"") &&
            strstr(st, "\"error\":null"));
  g_free(st);
  skim_pskr_free(p);
  for (Dgram *x = (Dgram[]){ a, b, c, e }, *end = x + 4; x < end; x++) { g_string_free(x->calls, TRUE); }
  g_object_unref(ls);

  /* nobody there: the reports stay, the error says so — never a crash */
  const SkimPskrConfig nope = { .call = "OK1HRA", .locator = "JO70", .host = "skimmer-gate.invalid",
                                .port = 4739, .interval_s = 1 };
  p = skim_pskr_new(&nope);
  skim_pskr_add(p, "DL1ABC", "JN58", 14075100, -10, "FT8", "20m", now);
  for (int i = 0; i < 60; i++) {
    st = skim_pskr_status_json(p);
    const gboolean err = !strstr(st, "\"error\":null");
    if (err) { break; }
    g_free(st);
    st = NULL;
    g_usleep(100 * 1000);
  }
  check("a host that does not resolve: kept (1 queued), the error shown",
        st && strstr(st, "\"queued\":1") && strstr(st, "skimmer-gate.invalid"));
  g_free(st);
  skim_pskr_free(p);

  g_bytes_unref(rx_example);
  g_bytes_unref(t_rx);
  g_bytes_unref(t_tx);
  g_print("%s\n", fails ? "FAILED" : "all passed");
  return fails ? 1 : 0;
}
