/* deepcw_model.c — see deepcw_model.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "deepcw_model.h"

#include <curl/curl.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0"
#endif

/* The files of deepcw-engine at SKIM_DEEPCW_COMMIT, with their digests
 * (fetched and checked by hand 2026-10-22; the model's is the one
 * docs/DEEPCW.md recorded when it was first evaluated). dest NULL = the
 * model itself at model_path; the others go beside it. */
static const struct {
  const char *name, *dest, *sha256;
  guint64     size;
} FILES[] = {
  { "model.onnx.json", "%s.json",
    "b4342157b90229ee7380e165f3d8036179b80d1e6ae02f2557388b7fcd558c01", 946 },
  { "LICENSE", "LICENSE",
    "0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0", 34523 },
  { "model.onnx", NULL, SKIM_DEEPCW_MODEL_SHA, 15139839 },
};

#define ERR (g_quark_from_static_string("skim-deepcw-model"))

char *skim_deepcw_env_base_url(void) {
  const char *env = g_getenv("SKIM_DEEPCW_URL");
  return env && env[0] ? g_strdup(env) : NULL;
}

gboolean skim_deepcw_model_check(const char *model_path, GError **error) {
  char *data = NULL;
  gsize len = 0;
  if (!g_file_test(model_path, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, ERR, 1, "no model at %s (--fetch-deepcw-model)", model_path);
    return FALSE;
  }
  GError *e = NULL;
  if (!g_file_get_contents(model_path, &data, &len, &e)) {
    g_set_error(error, ERR, 2, "%s", e->message);
    g_error_free(e);
    return FALSE;
  }
  char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guint8 *)data, len);
  g_free(data);
  const gboolean ok = strcmp(sha, SKIM_DEEPCW_MODEL_SHA) == 0;
  if (!ok) {
    g_set_error(error, ERR, 3, "%s is not the model this version runs (sha256 %.8s…, "
                "expected %.8s…) — --fetch-deepcw-model replaces it",
                model_path, sha, SKIM_DEEPCW_MODEL_SHA);
  }
  g_free(sha);
  return ok;
}

/* --- one download into memory ---------------------------------------------------- */

static gpointer global_init(gpointer unused) {
  (void)unused;
  signal(SIGPIPE, SIG_IGN);              /* CURLOPT_NOSIGNAL: see scp_update.c */
  return GINT_TO_POINTER(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

typedef struct {
  GByteArray              *body;
  guint64                  cap;
  const SkimDeepcwFetch   *f;
  guint64                  done_before, total;  /* bytes of the earlier files */
} Get;

static size_t on_body(char *p, size_t sz, size_t n, void *user) {
  Get *g = user;
  if (g->body->len + sz * n > g->cap) { return 0; }    /* aborts: too big     */
  g_byte_array_append(g->body, (const guint8 *)p, (guint)(sz * n));
  return sz * n;
}

static int on_progress(void *user, curl_off_t dt, curl_off_t dn, curl_off_t ut,
                       curl_off_t un) {
  (void)dt; (void)dn; (void)ut; (void)un;
  Get *g = user;
  if (g->f->cancel && g_cancellable_is_cancelled(g->f->cancel)) { return 1; }
  if (g->f->progress) {
    g->f->progress(g->done_before + g->body->len, g->total, g->f->user);
  }
  return 0;
}

static GBytes *get(const SkimDeepcwFetch *f, const char *url, guint64 cap,
                   guint64 done_before, guint64 total, GError **error) {
  CURL *h = curl_easy_init();
  if (!h) {
    g_set_error(error, ERR, 10, "libcurl would not start");
    return NULL;
  }
  const char *protos = g_str_has_prefix(url, "file://")   ? "file"
                       : g_str_has_prefix(url, "http://") ? "http,https"
                                                          : "https";
  char ua[128], cerr[CURL_ERROR_SIZE] = "";
  g_snprintf(ua, sizeof(ua), "skimmer-for-linux/%s", SKIMMER_VERSION);
  Get g = { g_byte_array_new(), cap, f, done_before, total };
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1000L);  /* B/s for …          */
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);     /* … 30 s: stalled    */
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, protos);
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, protos);
  curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &g);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &g);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, cerr);
  if (f->ca_path) { curl_easy_setopt(h, CURLOPT_CAINFO, f->ca_path); }
  const CURLcode rc = curl_easy_perform(h);
  curl_easy_cleanup(h);
  if (rc == CURLE_OK) { return g_byte_array_free_to_bytes(g.body); }
  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  } else if (rc == CURLE_WRITE_ERROR) {
    g_set_error(error, ERR, 11, "%s: larger than expected", url);
  } else {
    g_set_error(error, ERR, 12, "%s: %s", url, cerr[0] ? cerr : curl_easy_strerror(rc));
  }
  g_byte_array_unref(g.body);
  return NULL;
}

/* --- fetch ------------------------------------------------------------------------- */

static char *dest_of(const char *model_path, guint i) {
  if (!FILES[i].dest) { return g_strdup(model_path); }
  if (strstr(FILES[i].dest, "%s")) { return g_strdup_printf(FILES[i].dest, model_path); }
  char *dir = g_path_get_dirname(model_path);
  char *p = g_build_filename(dir, FILES[i].dest, NULL);
  g_free(dir);
  return p;
}

gboolean skim_deepcw_model_fetch(const SkimDeepcwFetch *f, GError **error) {
  static GOnce once = G_ONCE_INIT;
  if (!GPOINTER_TO_INT(g_once(&once, global_init, NULL))) {
    g_set_error(error, ERR, 10, "libcurl would not start");
    return FALSE;
  }
  const char *base = f->base_url ? f->base_url : SKIM_DEEPCW_UPSTREAM;
  guint64 total = 0, done = 0;
  for (guint i = 0; i < G_N_ELEMENTS(FILES); i++) { total += FILES[i].size; }

  GBytes *got[G_N_ELEMENTS(FILES)] = { NULL };
  gboolean ok = TRUE;
  for (guint i = 0; i < G_N_ELEMENTS(FILES) && ok; i++) {
    char *url = g_strdup_printf("%s/%s/%s", base, SKIM_DEEPCW_COMMIT, FILES[i].name);
    got[i] = get(f, url, FILES[i].size, done, total, error);
    g_free(url);
    if (!got[i]) {
      ok = FALSE;
      break;
    }
    gsize n = 0;
    const guint8 *d = g_bytes_get_data(got[i], &n);
    char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, d, n);
    if (strcmp(sha, FILES[i].sha256) != 0) {
      g_set_error(error, ERR, 13, "%s: sha256 %.8s…, expected %.8s… — not installed",
                  FILES[i].name, sha, FILES[i].sha256);
      ok = FALSE;
    }
    g_free(sha);
    done += n;
  }

  /* every file checked: in place, the model last (it is what the engine
   * looks for — its companions are there before it is) */
  char *dir = g_path_get_dirname(f->model_path);
  if (ok && g_mkdir_with_parents(dir, 0755) != 0) {
    g_set_error(error, ERR, 14, "%s: %s", dir, g_strerror(errno));
    ok = FALSE;
  }
  if (ok) {
    char *notice_path = g_build_filename(dir, "NOTICE", NULL);
    char *notice = g_strdup_printf(
        "DeepCW model (model.onnx, model.onnx.json) by e04, licensed AGPL-3.0-only\n"
        "(LICENSE beside this file). Not part of skimmer-for-linux; fetched\n"
        "unmodified from its own repository:\n\n  %s/%s\n  https://github.com/e04/deepcw-engine\n\n"
        "model.onnx sha256 %s\n", base, SKIM_DEEPCW_COMMIT, SKIM_DEEPCW_MODEL_SHA);
    ok = g_file_set_contents(notice_path, notice, -1, error);
    g_free(notice);
    g_free(notice_path);
  }
  for (guint i = 0; i < G_N_ELEMENTS(FILES) && ok; i++) {
    char *p = dest_of(f->model_path, i);
    gsize n = 0;
    const char *d = g_bytes_get_data(got[i], &n);
    ok = g_file_set_contents(p, d, (gssize)n, error);
    g_free(p);
  }
  g_free(dir);
  for (guint i = 0; i < G_N_ELEMENTS(FILES); i++) {
    if (got[i]) { g_bytes_unref(got[i]); }
  }
  return ok;
}
