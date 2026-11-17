/*
 * skimmer-peers-test — [peers]: other skimmers' telnet feeds (headless/peers.h).
 *
 * The line parser takes our own feed's lines and CW Skimmer Server's (also
 * with the spotter glued to the kHz), RTTY with BPS, and refuses what is no
 * CQ spot or carries no valid call. Then a real feed (SkimRbnFeed on an
 * ephemeral port) plays the peer: the module logs in at its prompt, every
 * spot reaches the callback, and when the peer goes away the state says it
 * retries.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "engine/rbn_feed.h"
#include "headless/peers.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static GMutex lock;
static GString *got;

static void on_spot(const char *peer, const char *call, const char *mode, double f,
                    double snr, double speed, gpointer user) {
  (void)user;
  g_mutex_lock(&lock);
  g_string_append_printf(got, "%s %s %s %.0f %.0f %.0f;", peer, call, mode, f, snr, speed);
  g_mutex_unlock(&lock);
}

static gboolean wait_for(const char *needle, int ms) {
  for (int t = 0; t < ms; t += 20) {
    g_mutex_lock(&lock);
    const gboolean hit = strstr(got->str, needle) != NULL;
    g_mutex_unlock(&lock);
    if (hit) { return TRUE; }
    g_usleep(20 * 1000);
  }
  return FALSE;
}

int main(void) {
  printf("== the line parser\n");
  {
    static const struct { const char *line; gboolean ok; const char *call, *mode; double khz, snr, speed; } L[] = {
      { "DX de OK1HRA-#:   7032.0  DL1ABC       CW    25 dB  22 WPM  CQ      1234Z", TRUE, "DL1ABC", "CW", 7032.0, 25, 22 },
      { "DX de OK1HRA-#: 14083.5  sp2abc       RTTY  12 dB  45 BPS  CQ      0911Z\r", TRUE, "SP2ABC", "RTTY", 14083.5, 12, 45 },
      { "DX de K3LR-#:14025.1  W1AW         CW    31 dB  28 WPM  CQ      1700Z", TRUE, "W1AW", "CW", 14025.1, 31, 28 },
      { "DX de K3LR-#:  3525.0  VE3XYZ/P     CW     8 dB  18 WPM  CQ      1700Z", TRUE, "VE3XYZ/P", "CW", 3525.0, 8, 18 },
      { "DX de K3LR-#:  7025.0  DL1ABC       CW    20 dB  22 WPM  DX      1700Z", FALSE, NULL, NULL, 0, 0, 0 },
      { "DX de K3LR-#:  7025.0  E5E5E        CW    20 dB  22 WPM  CQ      1700Z", FALSE, NULL, NULL, 0, 0, 0 },
      { "K3LR-#: Hello OK1HRA, spots follow.", FALSE, NULL, NULL, 0, 0, 0 },
      { "WWV de W0MU <18>: SFI=150, A=5", FALSE, NULL, NULL, 0, 0, 0 },
    };
    for (guint i = 0; i < G_N_ELEMENTS(L); i++) {
      char call[24] = "", mode[12] = "";
      double f = 0, snr = 0, sp = 0;
      const gboolean ok = skim_hd_peer_parse(L[i].line, call, sizeof(call), mode, sizeof(mode),
                                             &f, &snr, &sp);
      char what[200];
      g_snprintf(what, sizeof(what), "%s \"%.40s…\"", L[i].ok ? "spot" : "skipped", L[i].line);
      check(what, ok == L[i].ok && (!ok || (strcmp(call, L[i].call) == 0 &&
                                            strcmp(mode, L[i].mode) == 0 &&
                                            fabs(f - L[i].khz * 1000) < 1 &&
                                            snr == L[i].snr && sp == L[i].speed)));
    }
  }

  printf("== a feed as the peer\n");
  {
    got = g_string_new(NULL);
    GError *err = NULL;
    SkimRbnFeed *peer = skim_rbn_feed_new("K3LR", 0, &err);
    char *spec = g_strdup_printf("127.0.0.1:%u", skim_rbn_feed_port(peer));
    const char *hosts[] = { spec, NULL };
    SkimHdPeers *ps = skim_hd_peers_new(hosts, "OK1HRA", on_spot, NULL);
    gboolean in = FALSE;
    for (int t = 0; t < 3000 && !in; t += 20) {
      in = skim_rbn_feed_clients(peer) == 1;
      g_usleep(20 * 1000);
    }
    check("logged in at the peer's prompt", in);
    char *j = skim_hd_peers_json(ps);
    check("state: logged in as OK1HRA", strstr(j, "\"logged in as OK1HRA\"") &&
          strstr(j, "\"connected\":true"));
    g_free(j);
    skim_rbn_feed_spot(peer, "DL1ABC", "CW", 7032000.0, 25.0, 22.0);
    skim_rbn_feed_spot(peer, "SP2ABC", "RTTY", 14083500.0, 12.0, 45.0);
    char *want = g_strdup_printf("%s DL1ABC CW 7032000 25 22;", spec);
    check("the CW spot reaches the callback, with the peer named", wait_for(want, 3000));
    check("… and the RTTY one", wait_for("SP2ABC RTTY 14083500 12 45;", 3000));
    g_free(want);
    skim_rbn_feed_free(peer);
    gboolean retry = FALSE;
    for (int t = 0; t < 3000 && !retry; t += 20) {
      j = skim_hd_peers_json(ps);
      retry = strstr(j, "retrying in 5 s") && strstr(j, "\"connected\":false");
      g_free(j);
      g_usleep(20 * 1000);
    }
    check("the peer gone: retrying in 5 s", retry);
    skim_hd_peers_free(ps);
    check("freed while waiting to retry", TRUE);
    g_free(spec);
    g_string_free(got, TRUE);
  }

  printf("\n%s (%d of %d checks failed)\n", fails ? "FAIL" : "PASS", fails, checks);
  return fails ? 1 : 0;
}
