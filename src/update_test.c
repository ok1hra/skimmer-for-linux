/*
 * skimmer-update-test — offline gate for [update] (src/headless/update.h).
 *
 * A file:// mirror of the GitHub API (SkimHdUpdateEnv.base_url), a fake
 * "AppImage" (a shell script that answers --version and --check-config) and
 * a fake clock. Proven here:
 *   - versions compare as numbers, and a release's JSON is read from a real
 *     API answer (tests/update/release-v0.8.7.json) and from one with
 *     escapes and no AppImage;
 *   - up to date, not due (the daily limit lives in the state file), newer
 *     without a download (notify), a development build or no AppImage
 *     never downloads;
 *   - every refusal — a wrong digest, none published, a short file, a
 *     preflight whose --version names another version, one that refuses
 *     the config — leaves the running file as it was and no staged file;
 *   - the good path: staged, installed (the old file kept as .prev, the
 *     new one in place), a crash loop rolled back after
 *     SKIM_HD_UPDATE_CRASHES and that version skipped, the next release
 *     taken, accepted after its run, the rollback asked for;
 *   - the app handle: Install now of a staged version restarts into it.
 */
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "headless/update.h"

static int fails, checks;
static void check(const char *what, int ok) {
  checks++;
  if (!ok) { fails++; }
  printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  fflush(stdout);
}

static gint64 fake_now = 1800000000;          /* 2027-01-15 08:00 UTC         */
static gint64 now_cb(gpointer u) { (void)u; return fake_now; }

static char *T, *mirror, *app, *cfg, *state;

static char *slurp(const char *path) {
  char *t = NULL;
  if (!g_file_get_contents(path, &t, NULL, NULL)) { return g_strdup(""); }
  return t;
}

/* A fake skimmer-headless of `version`; refuse = its --check-config exits 2. */
static char *fake_program(const char *version, gboolean refuse) {
  return g_strdup_printf("#!/bin/sh\n# fake skimmer-headless %s\n"
                         "case \"$1\" in\n"
                         "--version) echo \"skimmer-headless %s\" ;;\n"
                         "--check-config) %s ;;\n"
                         "esac\nexit 0\n",
                         version, version,
                         refuse ? "echo 'config: [update] mode \"maybe\": auto, notify or off' >&2; exit 2"
                                : "echo fine");
}

static void write_file(const char *path, const char *text, int mode) {
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(path, text, -1, NULL);
  g_chmod(path, mode);
}

typedef enum { DIG_GOOD, DIG_WRONG, DIG_NONE } Digest;

/* The mirror's latest release: tag v<version>, its asset = `program`. */
static void publish(const char *version, const char *program, Digest dig, gint64 size_delta) {
  char *name = g_strdup_printf("skimmer-headless-%s-x86_64.AppImage", version);
  char *asset = g_build_filename(mirror, "dl", name, NULL);
  write_file(asset, program, 0644);
  char *sha = g_compute_checksum_for_string(G_CHECKSUM_SHA256, program, -1);
  if (dig == DIG_WRONG) { sha[0] = sha[0] == '0' ? '1' : '0'; }
  char *digest = dig == DIG_NONE ? g_strdup("null") : g_strdup_printf("\"sha256:%s\"", sha);
  char *json = g_strdup_printf(
      "{\"url\":\"x\",\"html_url\":\"https://github.com/ok1hra/skimmer-for-linux/releases/tag/v%s\","
      "\"tag_name\":\"v%s\",\"draft\":false,\"prerelease\":false,\"author\":{\"login\":\"a\",\"id\":1},"
      "\"assets\":[{\"name\":\"other.txt\",\"size\":3,\"digest\":null,\"browser_download_url\":\"file:///nonexistent\"},"
      "{\"url\":\"y\",\"name\":\"%s\",\"uploader\":{\"login\":\"a\"},\"size\":%" G_GINT64_FORMAT ",\"digest\":%s,"
      "\"browser_download_url\":\"file://%s\"}],"
      "\"published_at\":\"2027-01-01T00:00:00Z\",\"body\":\"notes \\\"quoted\\\" \\\\ and \\u00e9\\n\"}",
      version, version, name, (gint64)strlen(program) + size_delta, digest, asset);
  char *api = g_build_filename(mirror, "repos", "ok1hra", "skimmer-for-linux", "releases", "latest", NULL);
  write_file(api, json, 0644);
  g_free(api);
  g_free(json);
  g_free(digest);
  g_free(sha);
  g_free(asset);
  g_free(name);
}

static SkimHdUpdateEnv env_of(const char *version, const char *build, const char *appimage) {
  static char *base;
  if (!base) { base = g_strconcat("file://", mirror, NULL); }
  return (SkimHdUpdateEnv){
    .appimage = appimage, .version = version, .build = build, .config_path = cfg,
    .state_path = state, .base_url = base, .now_s = now_cb,
  };
}

static gboolean exists(const char *p) { return g_file_test(p, G_FILE_TEST_EXISTS); }

static char *staged(void) {
  char *dir = g_path_get_dirname(app), *base = g_path_get_basename(app);
  char *n = g_strdup_printf(".%s.new", base);
  char *p = g_build_filename(dir, n, NULL);
  g_free(n);
  g_free(base);
  g_free(dir);
  return p;
}

static gboolean app_is(const char *version) {
  char *t = slurp(app);
  char *want = g_strdup_printf("# fake skimmer-headless %s\n", version);
  const gboolean ok = strstr(t, want) != NULL;
  g_free(want);
  g_free(t);
  return ok;
}

static char *st_get(const char *key) {
  GKeyFile *kf = g_key_file_new();
  char *v = NULL;
  if (g_key_file_load_from_file(kf, state, G_KEY_FILE_NONE, NULL)) {
    v = g_key_file_get_string(kf, "update", key, NULL);
  }
  g_key_file_free(kf);
  return v;
}

static gboolean st_is(const char *key, const char *want) {
  char *v = st_get(key);
  const gboolean ok = g_strcmp0(v, want) == 0;
  if (!ok) { printf("       state %s = %s, expected %s\n", key, v ? v : "(none)", want ? want : "(none)"); }
  g_free(v);
  return ok;
}

static SkimHdUpdateOutcome run(const SkimHdUpdateEnv *e, gboolean force, gboolean download) {
  SkimHdUpdateOutcome o;
  fake_now += 60;                              /* past the Check-now spacing  */
  skim_hd_update_check(e, force, download, &o);
  printf("       → %d %s\n", o.result, o.detail);
  return o;
}

static void restarted_cb(const char *why, gpointer user) {
  (void)why;
  (*(int *)user)++;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <tests/update>\n", argv[0]);
    return 2;
  }
  T = g_dir_make_tmp("skim-update-XXXXXX", NULL);
  mirror = g_build_filename(T, "mirror", NULL);
  app = g_build_filename(T, "app", "skimmer-headless-x86_64.AppImage", NULL);
  cfg = g_build_filename(T, "headless.ini", NULL);
  state = g_build_filename(T, "state", "update.state", NULL);
  write_file(cfg, "[radio]\ntype=mock\n", 0644);

  printf("versions\n");
  check("0.8.10 > 0.8.9", skim_hd_update_vercmp("0.8.10", "0.8.9") > 0);
  check("0.8.7 = 0.8.7", skim_hd_update_vercmp("0.8.7", "0.8.7") == 0);
  check("0.8 = 0.8.0", skim_hd_update_vercmp("0.8", "0.8.0") == 0);
  check("0.9 > 0.8.99", skim_hd_update_vercmp("0.9", "0.8.99") > 0);
  check("1.0.0 > 0.99.99", skim_hd_update_vercmp("1.0.0", "0.99.99") > 0);
  check("a suffix is ignored", skim_hd_update_vercmp("0.8.7-gabc1234", "0.8.7") == 0);
  check("no number sorts low", skim_hd_update_vercmp("latest", "0.0.1") < 0);
  check("0.8.7 is a version", skim_hd_update_version_ok("0.8.7"));
  check("0.8. / v0.8 / '' / 0..8 are not",
        !skim_hd_update_version_ok("0.8.") && !skim_hd_update_version_ok("v0.8") &&
        !skim_hd_update_version_ok("") && !skim_hd_update_version_ok("0..8"));

  printf("the release's JSON\n");
  {
    char *p = g_build_filename(argv[1], "release-v0.8.7.json", NULL), *body = NULL;
    gsize n = 0;
    SkimHdRelease r;
    const gboolean got = g_file_get_contents(p, &body, &n, NULL);
    check("the real API answer reads", got && skim_hd_update_parse_release(body, n, &r));
    check("  tag v0.8.7, version 0.8.7", strcmp(r.tag, "v0.8.7") == 0 && strcmp(r.version, "0.8.7") == 0);
    check("  its AppImage asset", strcmp(r.asset_url, "https://github.com/ok1hra/skimmer-for-linux/"
                                         "releases/download/v0.8.7/skimmer-headless-0.8.7-x86_64.AppImage") == 0);
    check("  its digest and size",
          strcmp(r.sha256, "241d36cfc36d34b72ab2c25dd8f86f4a56c478bf047c5c4574c674b5934079f9") == 0 &&
          r.size == 19241464);
    check("  the page", strcmp(r.html_url, "https://github.com/ok1hra/skimmer-for-linux/releases/tag/v0.8.7") == 0);
    check("  the time", strcmp(r.published, "2026-11-22T00:40:26Z") == 0);
    check("  a truncated answer does not", !skim_hd_update_parse_release(body, n / 2, &r));
    g_free(body);
    g_free(p);
    const char *noasset = "{\"tag_name\":\"v1.2.3\",\"body\":\"a \\\"b\\\" \\\\ \\u00e9 }{][\",\"assets\":[]}";
    check("escapes, no asset: reads, asset_url empty",
          skim_hd_update_parse_release(noasset, strlen(noasset), &r) && !r.asset_url[0] &&
          strcmp(r.version, "1.2.3") == 0);
    const char *badtag = "{\"tag_name\":\"nightly\",\"assets\":[]}";
    check("a tag that is no version: refused", !skim_hd_update_parse_release(badtag, strlen(badtag), &r));
    const char *html = "<html>rate limited</html>";
    check("an error page: refused", !skim_hd_update_parse_release(html, strlen(html), &r));
  }

  char *old = fake_program("0.9.0", FALSE);
  write_file(app, old, 0755);
  char *stg = staged();
  char *prev = g_strconcat(app, ".prev", NULL);
  SkimHdUpdateEnv e = env_of("0.9.0", "", app);

  printf("checks\n");
  {
    publish("0.9.0", old, DIG_GOOD, 0);
    SkimHdUpdateOutcome o = run(&e, FALSE, TRUE);
    check("the same version: up to date", o.result == SKIM_HD_UPDATE_UP_TO_DATE);
    check("  the latest is in the state", st_is("latest", "0.9.0"));
    o = run(&e, FALSE, TRUE);
    check("again an hour later: not due", o.result == SKIM_HD_UPDATE_NOT_DUE && o.wait_s > 20 * 3600);
    fake_now += 25 * 3600;
    char *p91 = fake_program("0.9.1", FALSE);
    publish("0.9.1", p91, DIG_GOOD, 0);
    o = run(&e, FALSE, FALSE);
    check("a day later, newer, notify: NEWER, nothing downloaded",
          o.result == SKIM_HD_UPDATE_NEWER && strcmp(o.version, "0.9.1") == 0 && !exists(stg));
    SkimHdUpdateEnv dev = env_of("0.9.0", "-gabc1234", app);
    o = run(&dev, TRUE, TRUE);
    check("a development build: NEWER, never downloads", o.result == SKIM_HD_UPDATE_NEWER &&
          strstr(o.detail, "development build") && !exists(stg));
    SkimHdUpdateEnv tree = env_of("0.9.0", "", NULL);
    o = run(&tree, TRUE, TRUE);
    check("not an AppImage: NEWER, never downloads", o.result == SKIM_HD_UPDATE_NEWER && !exists(stg));
    g_free(p91);
  }

  printf("refusals (the running file stays, nothing staged)\n");
  {
    char *p91 = fake_program("0.9.1", FALSE);
    publish("0.9.1", p91, DIG_WRONG, 0);
    SkimHdUpdateOutcome o = run(&e, TRUE, TRUE);
    check("a wrong digest", o.result == SKIM_HD_UPDATE_FAILED && strstr(o.detail, "sha256") &&
          !exists(stg) && app_is("0.9.0"));
    check("  retried in a day, not an hour", o.wait_s > 20 * 3600);
    publish("0.9.1", p91, DIG_NONE, 0);
    o = run(&e, TRUE, TRUE);
    check("no digest published", o.result == SKIM_HD_UPDATE_FAILED && strstr(o.detail, "no sha256") &&
          !exists(stg) && app_is("0.9.0"));
    publish("0.9.1", p91, DIG_GOOD, 100);
    o = run(&e, TRUE, TRUE);
    check("shorter than the release says", o.result == SKIM_HD_UPDATE_FAILED && !exists(stg) &&
          app_is("0.9.0"));
    char *liar = fake_program("0.9.2", FALSE);
    publish("0.9.1", liar, DIG_GOOD, 0);
    o = run(&e, TRUE, TRUE);
    check("the program names another version", o.result == SKIM_HD_UPDATE_FAILED &&
          strstr(o.detail, "--version") && !exists(stg) && app_is("0.9.0"));
    char *picky = fake_program("0.9.1", TRUE);
    publish("0.9.1", picky, DIG_GOOD, 0);
    o = run(&e, TRUE, TRUE);
    check("the new version refuses this config", o.result == SKIM_HD_UPDATE_FAILED &&
          strstr(o.detail, "refuses this config") && strstr(o.detail, "[update] mode") &&
          !exists(stg) && app_is("0.9.0"));
    char *part = g_strconcat(stg, ".part", NULL);
    check("  no partial file left", !exists(part));
    g_free(part);
    g_free(picky);
    g_free(liar);
    g_free(p91);
  }

  printf("install, a crash loop, the next release\n");
  {
    char *p91 = fake_program("0.9.1", FALSE);
    publish("0.9.1", p91, DIG_GOOD, 0);
    SkimHdUpdateOutcome o = run(&e, TRUE, TRUE);
    check("good: READY, staged", o.result == SKIM_HD_UPDATE_READY && exists(stg) && app_is("0.9.0"));
    check("  the state names it", st_is("ready", "0.9.1"));
    o = run(&e, TRUE, TRUE);
    check("again: READY, still staged", o.result == SKIM_HD_UPDATE_READY && exists(stg));
    char *v = NULL;
    GError *err = NULL;
    check("installed", skim_hd_update_install(&e, &v, &err) && g_strcmp0(v, "0.9.1") == 0);
    if (err) { printf("       %s\n", err->message); g_clear_error(&err); }
    g_free(v);
    check("  0.9.1 in place, 0.9.0 kept as .prev, nothing staged",
          app_is("0.9.1") && strstr(slurp(prev), "fake skimmer-headless 0.9.0") && !exists(stg));
    check("  pending 0.9.0 → 0.9.1", st_is("pending_from", "0.9.0") && st_is("pending_to", "0.9.1"));
    check("  nothing left to install", !skim_hd_update_install(&e, &v, &err));
    g_clear_error(&err);

    SkimHdUpdateEnv n = env_of("0.9.1", "", app);
    check("first start of 0.9.1: no crash counted", !skim_hd_update_boot(&n) && st_is("crashes", "0"));
    check("crash 1 (no orderly stop): counted", !skim_hd_update_boot(&n) && st_is("crashes", "1"));
    check("crash 2", !skim_hd_update_boot(&n) && st_is("crashes", "2"));
    check("crash 3: rolled back, exec again", skim_hd_update_boot(&n));
    check("  0.9.0 in place, no .prev, 0.9.1 skipped", app_is("0.9.0") && !exists(prev) &&
          st_is("skip", "0.9.1") && st_is("pending_to", NULL));
    check("0.9.0 starts: nothing counted", !skim_hd_update_boot(&e));
    skim_hd_update_stopped(&e);
    check("  stopped in order: no mark", st_is("running", NULL));

    o = run(&e, TRUE, TRUE);
    check("0.9.1 again: NEWER, skipped", o.result == SKIM_HD_UPDATE_NEWER && strstr(o.detail, "rolled back") &&
          !exists(stg));
    char *p92 = fake_program("0.9.2", FALSE);
    publish("0.9.2", p92, DIG_GOOD, 0);
    o = run(&e, TRUE, TRUE);
    check("0.9.2: READY", o.result == SKIM_HD_UPDATE_READY && exists(stg));
    check("  installed", skim_hd_update_install(&e, &v, &err));
    g_clear_error(&err);
    g_free(v);
    SkimHdUpdateEnv n2 = env_of("0.9.2", "", app);
    check("0.9.2 starts", !skim_hd_update_boot(&n2));
    skim_hd_update_accept(&n2);
    check("  accepted after its run: no pending", st_is("pending_to", NULL));
    check("  a crash after that is not counted", !skim_hd_update_boot(&n2) && st_is("crashes", NULL));
    o = run(&n2, TRUE, TRUE);
    check("  and 0.9.2 is up to date", o.result == SKIM_HD_UPDATE_UP_TO_DATE);
    check("rollback asked for", skim_hd_update_rollback(&n2, "asked for", &err) && app_is("0.9.0") &&
          st_is("skip", "0.9.2"));
    g_clear_error(&err);
    check("  a second one: nothing kept", !skim_hd_update_rollback(&e, "again", &err));
    g_clear_error(&err);
    g_free(p92);
    g_free(p91);
  }

  printf("the app handle\n");
  {
    char *p93 = fake_program("0.9.3", FALSE);
    publish("0.9.3", p93, DIG_GOOD, 0);
    SkimHdUpdateOutcome o = run(&e, TRUE, TRUE);
    check("0.9.3 staged", o.result == SKIM_HD_UPDATE_READY);
    int restarts = 0;
    SkimHdUpdater *u = skim_hd_updater_new(&e, SKIM_HD_UPDATE_AUTO, 3, restarted_cb, &restarts);
    char *j = skim_hd_updater_json(u);
    check("the page's JSON: mode, ready, latest", strstr(j, "\"mode\":\"auto\"") &&
          strstr(j, "\"ready\":\"0.9.3\"") && strstr(j, "\"latest\":\"0.9.3\"") &&
          strstr(j, "\"newer\":true") && strstr(j, "\"hour_utc\":3"));
    g_free(j);
    char *why = skim_hd_updater_action(u, "bogus");
    check("an unknown action is refused", why != NULL);
    g_free(why);
    why = skim_hd_updater_action(u, "install");
    check("Install now: installed, restart asked for", !why && restarts == 1 && app_is("0.9.3"));
    g_free(why);
    skim_hd_updater_free(u);
    g_free(p93);
  }

  g_free(prev);
  g_free(stg);
  g_free(old);
  char *rm[] = { "rm", "-rf", T, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  printf("%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}
