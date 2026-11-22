/* update.c — see update.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "update.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "json.h"

#ifndef SKIMMER_VERSION
#define SKIMMER_VERSION "0"
#endif

#define ERR (g_quark_from_static_string("skim-hd-update"))
#define GROUP "update"
#define FORCE_SPACING_S 30            /* Check now, pressed twice: one request */

const char *skim_hd_update_mode_name(SkimHdUpdateMode m) {
  return m == SKIM_HD_UPDATE_OFF ? "off" : m == SKIM_HD_UPDATE_NOTIFY ? "notify" : "auto";
}

/* --- versions -------------------------------------------------------------------- */

#define VER_PARTS 6

/* The leading dotted numbers of s; FALSE = none. */
static gboolean ver_parse(const char *s, guint64 v[VER_PARTS]) {
  memset(v, 0, sizeof(guint64) * VER_PARTS);
  if (!s || !g_ascii_isdigit(*s)) { return FALSE; }
  for (guint i = 0; i < VER_PARTS && g_ascii_isdigit(*s); i++) {
    char *end = NULL;
    v[i] = g_ascii_strtoull(s, &end, 10);
    s = end;
    if (*s != '.') { break; }
    s++;
  }
  return TRUE;
}

int skim_hd_update_vercmp(const char *a, const char *b) {
  guint64 va[VER_PARTS], vb[VER_PARTS];
  const gboolean oa = ver_parse(a, va), ob = ver_parse(b, vb);
  if (!oa || !ob) { return (int)oa - (int)ob; }
  for (guint i = 0; i < VER_PARTS; i++) {
    if (va[i] != vb[i]) { return va[i] < vb[i] ? -1 : 1; }
  }
  return 0;
}

gboolean skim_hd_update_version_ok(const char *v) {
  if (!v || !g_ascii_isdigit(v[0]) || strlen(v) > 31) { return FALSE; }
  for (const char *p = v; *p; p++) {
    if (*p == '.') {
      if (!g_ascii_isdigit(p[1])) { return FALSE; }   /* "0..8", "0.8."       */
    } else if (!g_ascii_isdigit(*p)) {
      return FALSE;
    }
  }
  return TRUE;
}

/* --- the release, from the API's JSON ------------------------------------------ */

typedef struct {
  char   name[128], url[512], digest[80];
  gint64 size;
} Asset;

#define MAX_ASSETS 32

/* A string value into out, or null as "". */
static gboolean rd_str(SkimJsonR *j, char *out, gsize n) {
  if (j->p < j->end && *j->p == '"') { return skim_json_string(j, out, n); }
  out[0] = '\0';
  return skim_json_skip(j, 1);
}

static gboolean rd_int(SkimJsonR *j, gint64 *v) {
  const char *s = j->p;
  if (!skim_json_skip(j, 1)) { return FALSE; }
  char num[24];
  const gsize k = MIN((gsize)(j->p - s), sizeof(num) - 1);
  memcpy(num, s, k);
  num[k] = '\0';
  char *end = NULL;
  *v = g_ascii_strtoll(num, &end, 10);
  if (end == num || *end) { *v = -1; }
  return TRUE;
}

/* One object: each key handed to `on`, which reads its value (TRUE) or
 * leaves it to be skipped (FALSE). */
typedef gboolean (*OnKey)(SkimJsonR *j, const char *key, gpointer user, gboolean *ok);

static gboolean rd_object(SkimJsonR *j, OnKey on, gpointer user) {
  skim_json_ws(j);
  if (j->p >= j->end || *j->p++ != '{') { return FALSE; }
  skim_json_ws(j);
  if (j->p < j->end && *j->p == '}') { j->p++; return TRUE; }
  for (;;) {
    char key[40];
    skim_json_ws(j);
    if (!skim_json_string(j, key, sizeof(key))) { return FALSE; }
    skim_json_ws(j);
    if (j->p >= j->end || *j->p++ != ':') { return FALSE; }
    skim_json_ws(j);
    gboolean ok = TRUE;
    if (!on(j, key, user, &ok)) { ok = skim_json_skip(j, 1); }
    if (!ok) { return FALSE; }
    skim_json_ws(j);
    if (j->p >= j->end) { return FALSE; }
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == '}') { j->p++; return TRUE; }
    return FALSE;
  }
}

static gboolean on_asset_key(SkimJsonR *j, const char *key, gpointer user, gboolean *ok) {
  Asset *a = user;
  if (strcmp(key, "name") == 0) { *ok = rd_str(j, a->name, sizeof(a->name)); }
  else if (strcmp(key, "browser_download_url") == 0) { *ok = rd_str(j, a->url, sizeof(a->url)); }
  else if (strcmp(key, "digest") == 0) { *ok = rd_str(j, a->digest, sizeof(a->digest)); }
  else if (strcmp(key, "size") == 0) { *ok = rd_int(j, &a->size); }
  else { return FALSE; }
  return TRUE;
}

typedef struct {
  SkimHdRelease *r;
  Asset          assets[MAX_ASSETS];
  guint          n;
} RelParse;

static gboolean rd_assets(SkimJsonR *j, RelParse *p) {
  if (j->p >= j->end || *j->p != '[') { return skim_json_skip(j, 1); }   /* null */
  j->p++;
  skim_json_ws(j);
  if (j->p < j->end && *j->p == ']') { j->p++; return TRUE; }
  for (;;) {
    Asset a = { .size = -1 };
    skim_json_ws(j);
    if (!rd_object(j, on_asset_key, &a)) { return FALSE; }
    if (p->n < MAX_ASSETS) { p->assets[p->n++] = a; }
    skim_json_ws(j);
    if (j->p >= j->end) { return FALSE; }
    if (*j->p == ',') { j->p++; continue; }
    if (*j->p == ']') { j->p++; return TRUE; }
    return FALSE;
  }
}

static gboolean on_release_key(SkimJsonR *j, const char *key, gpointer user, gboolean *ok) {
  RelParse *p = user;
  SkimHdRelease *r = p->r;
  if (strcmp(key, "tag_name") == 0) { *ok = rd_str(j, r->tag, sizeof(r->tag)); }
  else if (strcmp(key, "html_url") == 0) { *ok = rd_str(j, r->html_url, sizeof(r->html_url)); }
  else if (strcmp(key, "published_at") == 0) { *ok = rd_str(j, r->published, sizeof(r->published)); }
  else if (strcmp(key, "assets") == 0) { *ok = rd_assets(j, p); }
  else { return FALSE; }
  return TRUE;
}

static gboolean is_hex64(const char *s) {
  if (strlen(s) != 64) { return FALSE; }
  for (const char *p = s; *p; p++) {
    if (!g_ascii_isxdigit(*p)) { return FALSE; }
  }
  return TRUE;
}

gboolean skim_hd_update_parse_release(const char *body, gsize len, SkimHdRelease *r) {
  memset(r, 0, sizeof(*r));
  r->size = -1;
  RelParse *p = g_new0(RelParse, 1);
  p->r = r;
  SkimJsonR j = { body, body + len };
  gboolean ok = rd_object(&j, on_release_key, p);
  ok = ok && r->tag[0] == 'v' && skim_hd_update_version_ok(r->tag + 1);
  if (ok) {
    g_strlcpy(r->version, r->tag + 1, sizeof(r->version));
    g_snprintf(r->asset_name, sizeof(r->asset_name), "skimmer-headless-%s-x86_64.AppImage",
               r->version);
    for (guint i = 0; i < p->n; i++) {
      const Asset *a = &p->assets[i];
      if (strcmp(a->name, r->asset_name) != 0) { continue; }
      g_strlcpy(r->asset_url, a->url, sizeof(r->asset_url));
      r->size = a->size;
      if (g_str_has_prefix(a->digest, "sha256:") && is_hex64(a->digest + 7)) {
        char *low = g_ascii_strdown(a->digest + 7, -1);
        g_strlcpy(r->sha256, low, sizeof(r->sha256));
        g_free(low);
      }
      break;
    }
  }
  g_free(p);
  return ok;
}

/* --- the state file ------------------------------------------------------------
 * One GKeyFile, every read-modify-write under one lock: the worker's check
 * and the main thread's install, boot and marks share it. */

typedef struct {
  gint64 last_ok, last_try, result_at, installed_at;
  gint   fails, crashes;
  char  *result;
  char  *latest, *latest_url, *latest_published;
  char  *ready, *ready_sha;          /* the staged version, its digest          */
  char  *prev;                       /* the version of <appimage>.prev          */
  char  *skip;                       /* rolled back from: not tried again       */
  char  *pending_path, *pending_from, *pending_to;  /* installed, not accepted */
  char  *running;                    /* the AppImage of a run that has not ended */
} St;

static GRecMutex st_lock;

static void st_clear(St *s) {
  g_free(s->result);
  g_free(s->latest);
  g_free(s->latest_url);
  g_free(s->latest_published);
  g_free(s->ready);
  g_free(s->ready_sha);
  g_free(s->prev);
  g_free(s->skip);
  g_free(s->pending_path);
  g_free(s->pending_from);
  g_free(s->pending_to);
  g_free(s->running);
  memset(s, 0, sizeof(*s));
}

static char *kf_str(GKeyFile *kf, const char *k) {
  char *v = g_key_file_get_string(kf, GROUP, k, NULL);
  if (v && !v[0]) { g_clear_pointer(&v, g_free); }
  return v;
}

static gint64 kf_i64(GKeyFile *kf, const char *k) {
  return g_key_file_get_int64(kf, GROUP, k, NULL);
}

static void st_load(const SkimHdUpdateEnv *e, St *s) {
  memset(s, 0, sizeof(*s));
  GKeyFile *kf = g_key_file_new();
  if (g_key_file_load_from_file(kf, e->state_path, G_KEY_FILE_NONE, NULL)) {
    s->last_ok = kf_i64(kf, "last_ok");
    s->last_try = kf_i64(kf, "last_try");
    s->result_at = kf_i64(kf, "result_at");
    s->installed_at = kf_i64(kf, "installed_at");
    s->fails = (gint)kf_i64(kf, "fails");
    s->crashes = (gint)kf_i64(kf, "crashes");
    s->result = kf_str(kf, "result");
    s->latest = kf_str(kf, "latest");
    s->latest_url = kf_str(kf, "latest_url");
    s->latest_published = kf_str(kf, "latest_published");
    s->ready = kf_str(kf, "ready");
    s->ready_sha = kf_str(kf, "ready_sha");
    s->prev = kf_str(kf, "prev");
    s->skip = kf_str(kf, "skip");
    s->pending_path = kf_str(kf, "pending_path");
    s->pending_from = kf_str(kf, "pending_from");
    s->pending_to = kf_str(kf, "pending_to");
    s->running = kf_str(kf, "running");
  }
  g_key_file_free(kf);
}

static void kf_set(GKeyFile *kf, const char *k, const char *v) {
  if (v) { g_key_file_set_string(kf, GROUP, k, v); }
}

static void st_save(const SkimHdUpdateEnv *e, const St *s) {
  GKeyFile *kf = g_key_file_new();
  g_key_file_set_comment(kf, NULL, NULL,
                         " skimmer-headless's updates (src/headless/update.c) — kept by the program", NULL);
  g_key_file_set_int64(kf, GROUP, "last_ok", s->last_ok);
  g_key_file_set_int64(kf, GROUP, "last_try", s->last_try);
  g_key_file_set_int64(kf, GROUP, "fails", s->fails);
  g_key_file_set_int64(kf, GROUP, "result_at", s->result_at);
  kf_set(kf, "result", s->result);
  kf_set(kf, "latest", s->latest);
  kf_set(kf, "latest_url", s->latest_url);
  kf_set(kf, "latest_published", s->latest_published);
  kf_set(kf, "ready", s->ready);
  kf_set(kf, "ready_sha", s->ready_sha);
  kf_set(kf, "prev", s->prev);
  kf_set(kf, "skip", s->skip);
  kf_set(kf, "pending_path", s->pending_path);
  kf_set(kf, "pending_from", s->pending_from);
  kf_set(kf, "pending_to", s->pending_to);
  if (s->pending_path) { g_key_file_set_int64(kf, GROUP, "crashes", s->crashes); }
  if (s->installed_at) { g_key_file_set_int64(kf, GROUP, "installed_at", s->installed_at); }
  kf_set(kf, "running", s->running);
  char *dir = g_path_get_dirname(e->state_path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  GError *err = NULL;
  if (!g_key_file_save_to_file(kf, e->state_path, &err)) {
    g_warning("update: %s", err->message);
    g_error_free(err);
  }
  g_key_file_free(kf);
}

static void set_str(char **p, const char *v) {
  g_free(*p);
  *p = v && v[0] ? g_strdup(v) : NULL;
}

static gint64 env_now(const SkimHdUpdateEnv *e) {
  return e->now_s ? e->now_s(e->now_user) : g_get_real_time() / G_USEC_PER_SEC;
}

static void st_result(St *s, gint64 now, const char *text) {
  set_str(&s->result, text);
  s->result_at = now;
}

/* When the next scheduled check may run (0 = now). */
static gint64 st_due(const St *s, gint64 now) {
  gint64 due = s->fails > 0 ? s->last_try + (s->fails < SKIM_HD_UPDATE_RETRIES
                                             ? SKIM_HD_UPDATE_RETRY_S : SKIM_HD_UPDATE_SPACING_S)
                            : s->last_ok ? s->last_ok + SKIM_HD_UPDATE_SPACING_S : 0;
  if (due - now > SKIM_HD_UPDATE_SPACING_S) { due = now; }   /* a clock set back  */
  return due;
}

/* --- paths ----------------------------------------------------------------------- */

char *skim_hd_update_env_base_url(void) {
  const char *env = g_getenv("SKIM_UPDATE_URL");
  return env && env[0] ? g_strdup(env) : NULL;
}

char *skim_hd_update_state_path(void) {
  return g_build_filename(g_get_user_state_dir(), "skimmer-for-linux", "update.state", NULL);
}

/* <dir>/.<name>.new — beside the running file: the swap is one rename. */
static char *staged_path(const char *appimage, const char *suffix) {
  char *dir = g_path_get_dirname(appimage), *base = g_path_get_basename(appimage);
  char *name = g_strdup_printf(".%s.new%s", base, suffix);
  char *p = g_build_filename(dir, name, NULL);
  g_free(name);
  g_free(base);
  g_free(dir);
  return p;
}

static char *prev_path(const char *appimage) {
  return g_strconcat(appimage, ".prev", NULL);
}

char *skim_hd_update_why_not(const SkimHdUpdateEnv *e) {
  if (!e->appimage) {
    return g_strdup("not running from an AppImage — updates only report here");
  }
  if (e->build && e->build[0]) {
    return g_strdup_printf("a development build (%s%s) — never replaced by a release",
                           e->version, e->build);
  }
  if (!g_file_test(e->appimage, G_FILE_TEST_IS_REGULAR)) {
    return g_strdup_printf("%s is not a file", e->appimage);
  }
  char *dir = g_path_get_dirname(e->appimage);
  char *why = access(dir, W_OK) == 0 ? NULL
              : g_strdup_printf("%s is not writable — the new file goes beside the running one",
                                dir);
  g_free(dir);
  return why;
}

/* --- HTTP (libcurl, easy: a transfer is cancelled within its 1 s progress tick) - */

static gpointer global_init(gpointer unused) {
  (void)unused;
  signal(SIGPIPE, SIG_IGN);              /* CURLOPT_NOSIGNAL: see scp_update.c */
  return GINT_TO_POINTER(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
}

static gboolean curl_ready(void) {
  static GOnce once = G_ONCE_INIT;
  return GPOINTER_TO_INT(g_once(&once, global_init, NULL));
}

typedef struct {
  GByteArray   *mem;          /* into memory …                               */
  FILE         *file;         /* … or to disk, digested on the way          */
  GChecksum    *sum;
  guint64       got, cap;
  gboolean      too_big;
  GCancellable *cancel;
} Get;

static size_t on_body(char *ptr, size_t size, size_t n, void *user) {
  Get *g = user;
  const size_t len = size * n;
  if (g->got + len > g->cap) {
    g->too_big = TRUE;
    return 0;
  }
  if (g->file) {
    if (fwrite(ptr, 1, len, g->file) != len) { return 0; }
    g_checksum_update(g->sum, (const guchar *)ptr, (gssize)len);
  } else {
    g_byte_array_append(g->mem, (const guint8 *)ptr, (guint)len);
  }
  g->got += len;
  return len;
}

static int on_progress(void *user, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
  (void)dt; (void)dn; (void)ut; (void)un;
  Get *g = user;
  return g->cancel && g_cancellable_is_cancelled(g->cancel) ? 1 : 0;
}

/* The protocols a base allows: the API's https; a mirror's own. */
static const char *protos_of(const char *base) {
  return g_str_has_prefix(base, "file://") ? "file"
         : g_str_has_prefix(base, "http://") ? "http,https" : "https";
}

static gboolean http_get(const SkimHdUpdateEnv *e, const char *url, gboolean api, Get *g,
                         GError **error) {
  if (!curl_ready()) {
    g_set_error(error, ERR, 1, "libcurl would not start");
    return FALSE;
  }
  CURL *h = curl_easy_init();
  if (!h) {
    g_set_error(error, ERR, 1, "libcurl would not start");
    return FALSE;
  }
  const char *base = e->base_url ? e->base_url : SKIM_HD_UPDATE_API;
  char ua[96], cerr[CURL_ERROR_SIZE] = "";
  g_snprintf(ua, sizeof(ua), "skimmer-for-linux/%s", e->version ? e->version : SKIMMER_VERSION);
  struct curl_slist *hdr = NULL;
  if (api) {
    hdr = curl_slist_append(hdr, "Accept: application/vnd.github+json");
    hdr = curl_slist_append(hdr, "X-GitHub-Api-Version: 2022-11-28");
  }
  g->cancel = e->cancel;
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_USERAGENT, ua);
  if (hdr) { curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr); }
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1000L);    /* B/s for …        */
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);       /* … a minute: stalled */
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, protos_of(base));
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, protos_of(base));
  curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, g);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, g);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, cerr);
  if (e->ca_path) { curl_easy_setopt(h, CURLOPT_CAINFO, e->ca_path); }
  const CURLcode rc = curl_easy_perform(h);
  curl_easy_cleanup(h);
  curl_slist_free_all(hdr);
  if (rc == CURLE_OK) { return TRUE; }
  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  } else if (g->too_big) {
    g_set_error(error, ERR, 2, "%s: larger than expected", url);
  } else {
    g_set_error(error, ERR, 3, "%s: %s", url, cerr[0] ? cerr : curl_easy_strerror(rc));
  }
  return FALSE;
}

/* --- the preflight: the new program, run twice ----------------------------------- */

/* Runs exe args… (an AppImage extracts itself: no FUSE needed), at most
 * SKIM_HD_UPDATE_PREFLIGHT_S. *status = its exit code (-1 = killed or a
 * signal); out = stdout then stderr (caller frees). */
static gboolean run_capture(const char *exe, const char *const *args, int *status, char **out,
                            GError **error) {
  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer)exe);
  for (const char *const *a = args; *a; a++) { g_ptr_array_add(av, (gpointer)*a); }
  g_ptr_array_add(av, NULL);
  char **envp = g_get_environ();
  envp = g_environ_setenv(envp, "APPIMAGE_EXTRACT_AND_RUN", "1", TRUE);
  envp = g_environ_unsetenv(envp, "APPDIR");
  envp = g_environ_unsetenv(envp, "APPIMAGE");
  GPid pid = 0;
  int fo = -1, fe = -1;
  const gboolean spawned = g_spawn_async_with_pipes(NULL, (char **)av->pdata, envp,
                                                    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
                                                    NULL, &fo, &fe, error);
  g_strfreev(envp);
  g_ptr_array_free(av, TRUE);
  if (!spawned) { return FALSE; }
  GString *o = g_string_new(NULL), *er = g_string_new(NULL);
  const gint64 deadline = g_get_monotonic_time() + SKIM_HD_UPDATE_PREFLIGHT_S * G_USEC_PER_SEC;
  gboolean timed_out = FALSE;
  while (fo >= 0 || fe >= 0) {
    struct pollfd pf[2];
    int n = 0;
    if (fo >= 0) { pf[n++] = (struct pollfd){ .fd = fo, .events = POLLIN }; }
    if (fe >= 0) { pf[n++] = (struct pollfd){ .fd = fe, .events = POLLIN }; }
    const gint64 left = (deadline - g_get_monotonic_time()) / 1000;
    if (left <= 0) { timed_out = TRUE; break; }
    if (poll(pf, (nfds_t)n, (int)MIN(left, 1000)) < 0 && errno != EINTR) { break; }
    for (int i = 0; i < n; i++) {
      if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) { continue; }
      char buf[4096];
      const ssize_t r = read(pf[i].fd, buf, sizeof(buf));
      GString *dst = pf[i].fd == fo ? o : er;
      if (r > 0) {
        if (dst->len < 65536) { g_string_append_len(dst, buf, r); }
      } else if (r == 0 || errno != EINTR) {
        close(pf[i].fd);
        if (pf[i].fd == fo) { fo = -1; } else { fe = -1; }
      }
    }
  }
  if (fo >= 0) { close(fo); }
  if (fe >= 0) { close(fe); }
  int ws = 0;
  pid_t w = 0;
  while ((w = waitpid(pid, &ws, WNOHANG)) == 0 && !timed_out) {
    if (g_get_monotonic_time() > deadline) { timed_out = TRUE; break; }
    g_usleep(50 * 1000);
  }
  if (w == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &ws, 0);
  }
  g_spawn_close_pid(pid);
  *status = timed_out ? -1 : WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
  g_string_append(o, er->str);
  g_string_free(er, TRUE);
  *out = g_string_free(o, FALSE);
  if (timed_out) {
    g_set_error(error, ERR, 4, "%s did not finish in %d s", exe, SKIM_HD_UPDATE_PREFLIGHT_S);
    return FALSE;
  }
  return TRUE;
}

/* The last line with something on it, for a refusal's reason. */
static char *last_line(const char *text) {
  char **lines = g_strsplit(text, "\n", -1);
  const char *pick = "";
  for (char **l = lines; *l; l++) {
    if (g_strstrip(*l)[0]) { pick = *l; }
  }
  char *r = g_strndup(pick, 200);
  g_strfreev(lines);
  return r;
}

static gboolean preflight(const SkimHdUpdateEnv *e, const char *exe, const char *version,
                          GError **error) {
  int st = 0;
  char *out = NULL;
  const char *vargs[] = { "--version", NULL };
  if (!run_capture(exe, vargs, &st, &out, error)) {
    g_free(out);
    return FALSE;
  }
  char *want = g_strdup_printf("skimmer-headless %s", version);
  char *first = g_strndup(out, strcspn(out, "\n"));
  g_strstrip(first);
  const gboolean vok = st == 0 && strcmp(first, want) == 0;
  if (!vok) {
    g_set_error(error, ERR, 5, "the new program says \"%.80s\" (exit %d) to --version, "
                "not \"%s\"", first, st, want);
  }
  g_free(first);
  g_free(want);
  g_free(out);
  if (!vok || !e->config_path) { return vok; }
  const char *cargs[] = { "--check-config", "--config", e->config_path, NULL };
  out = NULL;
  if (!run_capture(exe, cargs, &st, &out, error)) {
    g_free(out);
    return FALSE;
  }
  if (st != 0) {
    char *why = last_line(out);
    g_set_error(error, ERR, 6, "the new version refuses this config (exit %d): %s", st, why);
    g_free(why);
  }
  g_free(out);
  return st == 0;
}

/* --- one check ------------------------------------------------------------------- */

static char *file_sha256(const char *path, GError **error) {
  GMappedFile *m = g_mapped_file_new(path, FALSE, error);
  if (!m) { return NULL; }
  char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                          (const guchar *)g_mapped_file_get_contents(m),
                                          g_mapped_file_get_length(m));
  g_mapped_file_unref(m);
  return sha;
}

/* The release downloaded to <staged>, its digest checked, run once.
 * permanent: the same try would fail the same way (a digest, a preflight). */
static gboolean stage(const SkimHdUpdateEnv *e, const SkimHdRelease *r, gboolean *permanent,
                      GError **error) {
  *permanent = TRUE;
  if (!r->asset_url[0]) {
    g_set_error(error, ERR, 7, "the release has no %s", r->asset_name);
    return FALSE;
  }
  if (!r->sha256[0]) {
    g_set_error(error, ERR, 8, "the release publishes no sha256 for %s — not installed",
                r->asset_name);
    return FALSE;
  }
  char *part = staged_path(e->appimage, ".part"), *dest = staged_path(e->appimage, "");
  FILE *f = g_fopen(part, "wb");
  if (!f) {
    g_set_error(error, ERR, 9, "%s: %s", part, g_strerror(errno));
    g_free(part);
    g_free(dest);
    return FALSE;
  }
  Get g = { .file = f, .sum = g_checksum_new(G_CHECKSUM_SHA256),
            .cap = r->size > 0 ? MIN((guint64)r->size, SKIM_HD_UPDATE_MAX_BYTES)
                               : SKIM_HD_UPDATE_MAX_BYTES };
  gboolean ok = http_get(e, r->asset_url, FALSE, &g, error);
  if (!ok) { *permanent = FALSE; }
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
    if (ok) { g_set_error(error, ERR, 9, "%s: %s", part, g_strerror(errno)); }
    ok = FALSE;
    *permanent = FALSE;
  }
  fclose(f);
  if (ok && r->size > 0 && g.got != (guint64)r->size) {
    g_set_error(error, ERR, 10, "%s: %" G_GUINT64_FORMAT " bytes, the release says %" G_GINT64_FORMAT,
                r->asset_name, g.got, r->size);
    ok = FALSE;
    *permanent = FALSE;
  }
  if (ok && strcmp(g_checksum_get_string(g.sum), r->sha256) != 0) {
    g_set_error(error, ERR, 11, "%s: sha256 %.8s…, the release says %.8s… — not installed",
                r->asset_name, g_checksum_get_string(g.sum), r->sha256);
    ok = FALSE;
  }
  g_checksum_free(g.sum);
  if (ok && g_chmod(part, 0755) != 0) {
    g_set_error(error, ERR, 9, "%s: %s", part, g_strerror(errno));
    ok = FALSE;
  }
  ok = ok && preflight(e, part, r->version, error);
  if (ok && g_rename(part, dest) != 0) {
    g_set_error(error, ERR, 9, "%s: %s", dest, g_strerror(errno));
    ok = FALSE;
    *permanent = FALSE;
  }
  if (!ok) { g_unlink(part); }
  g_free(part);
  g_free(dest);
  return ok;
}

static void out_set(SkimHdUpdateOutcome *out, SkimHdUpdateResult res, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);
static void out_set(SkimHdUpdateOutcome *out, SkimHdUpdateResult res, const char *fmt, ...) {
  out->result = res;
  va_list ap;
  va_start(ap, fmt);
  g_vsnprintf(out->detail, sizeof(out->detail), fmt, ap);
  va_end(ap);
}

/* A staged file that is no longer newer than what runs goes. */
static void drop_stale(const SkimHdUpdateEnv *e, St *s) {
  if (!s->ready || skim_hd_update_vercmp(s->ready, e->version) > 0) { return; }
  if (e->appimage) {
    char *p = staged_path(e->appimage, "");
    g_unlink(p);
    g_free(p);
  }
  set_str(&s->ready, NULL);
  set_str(&s->ready_sha, NULL);
}

static gboolean staged_ok(const SkimHdUpdateEnv *e, const St *s, const char *version) {
  if (!e->appimage || !s->ready || g_strcmp0(s->ready, version) != 0) { return FALSE; }
  char *p = staged_path(e->appimage, "");
  const gboolean ok = g_file_test(p, G_FILE_TEST_IS_REGULAR);
  g_free(p);
  return ok;
}

void skim_hd_update_check(const SkimHdUpdateEnv *e, gboolean force, gboolean download,
                          SkimHdUpdateOutcome *out) {
  memset(out, 0, sizeof(*out));
  const gint64 now = env_now(e);
  St s;
  g_rec_mutex_lock(&st_lock);
  st_load(e, &s);
  g_rec_mutex_unlock(&st_lock);
  const gint64 due = st_due(&s, now);
  const gint64 last_try = s.last_try;
  const gboolean too_soon = force ? now - last_try < FORCE_SPACING_S : due > now;
  st_clear(&s);
  if (too_soon) {
    out->wait_s = force ? FORCE_SPACING_S - (now - last_try) : due - now;
    out_set(out, SKIM_HD_UPDATE_NOT_DUE, force ? "checked a moment ago" : "not due");
    return;
  }

  char *url = g_strdup_printf("%s/repos/%s/releases/latest",
                              e->base_url ? e->base_url : SKIM_HD_UPDATE_API, SKIM_HD_UPDATE_REPO);
  Get g = { .mem = g_byte_array_new(), .cap = SKIM_HD_UPDATE_API_MAX };
  GError *err = NULL;
  SkimHdRelease r;
  gboolean got = http_get(e, url, TRUE, &g, &err);
  if (got && !skim_hd_update_parse_release((const char *)g.mem->data, g.mem->len, &r)) {
    g_set_error(&err, ERR, 12, "%s: not a release this program can read", url);
    got = FALSE;
  }
  g_byte_array_unref(g.mem);
  g_free(url);

  g_rec_mutex_lock(&st_lock);
  st_load(e, &s);
  s.last_try = now;
  if (!got) {
    const gboolean cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (!cancelled) { s.fails++; }
    out_set(out, cancelled ? SKIM_HD_UPDATE_CANCELLED : SKIM_HD_UPDATE_FAILED,
            "the check failed: %s", err->message);
    if (!cancelled) { st_result(&s, now, out->detail); }
    g_clear_error(&err);
  } else {
    s.last_ok = now;
    s.fails = 0;
    set_str(&s.latest, r.version);
    set_str(&s.latest_url, r.html_url);
    set_str(&s.latest_published, r.published);
    drop_stale(e, &s);
    g_strlcpy(out->version, r.version, sizeof(out->version));
    char *why = NULL;
    if (skim_hd_update_vercmp(r.version, e->version) <= 0) {
      out_set(out, SKIM_HD_UPDATE_UP_TO_DATE, "%s is the latest release — up to date",
              r.version);
    } else if (g_strcmp0(s.skip, r.version) == 0) {
      out_set(out, SKIM_HD_UPDATE_NEWER, "%s is out, but it was rolled back here once — "
              "waiting for the next release", r.version);
    } else if (!download) {
      out_set(out, SKIM_HD_UPDATE_NEWER, "%s is out (this is %s)", r.version, e->version);
    } else if ((why = skim_hd_update_why_not(e)) != NULL) {
      out_set(out, SKIM_HD_UPDATE_NEWER, "%s is out — not installed: %s", r.version, why);
    } else if (staged_ok(e, &s, r.version)) {
      out_set(out, SKIM_HD_UPDATE_READY, "%s is downloaded and checked", r.version);
    } else {
      st_save(e, &s);                        /* what the API said, kept …    */
      g_rec_mutex_unlock(&st_lock);          /* … while the download runs    */
      gboolean permanent = FALSE;
      const gboolean ok = stage(e, &r, &permanent, &err);
      char *sha = ok ? g_strdup(r.sha256) : NULL;
      g_rec_mutex_lock(&st_lock);
      st_clear(&s);
      st_load(e, &s);
      if (ok) {
        set_str(&s.ready, r.version);
        set_str(&s.ready_sha, sha);
        out_set(out, SKIM_HD_UPDATE_READY, "%s downloaded, sha256 and a trial run fine",
                r.version);
      } else if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        out_set(out, SKIM_HD_UPDATE_CANCELLED, "%s: download cancelled", r.version);
      } else {
        s.fails = permanent ? SKIM_HD_UPDATE_RETRIES : s.fails + 1;
        out_set(out, SKIM_HD_UPDATE_FAILED, "%s not installed: %s", r.version, err->message);
      }
      g_clear_error(&err);
      g_free(sha);
    }
    g_free(why);
    if (out->result != SKIM_HD_UPDATE_CANCELLED) { st_result(&s, now, out->detail); }
  }
  out->wait_s = MAX(st_due(&s, now) - now, 0);
  st_save(e, &s);
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
}

/* --- install, rollback, the marks ---------------------------------------------- */

/* cur.prev := cur (a hard link: cur is never missing; a copy where links
 * are not possible). */
static gboolean keep_prev(const char *cur, GError **error) {
  char *prev = prev_path(cur);
  g_unlink(prev);
  gboolean ok = link(cur, prev) == 0;
  if (!ok) {
    GFile *a = g_file_new_for_path(cur), *b = g_file_new_for_path(prev);
    ok = g_file_copy(a, b, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
    g_object_unref(a);
    g_object_unref(b);
  }
  g_free(prev);
  return ok;
}

gboolean skim_hd_update_install(const SkimHdUpdateEnv *e, char **version, GError **error) {
  char *why = skim_hd_update_why_not(e);
  if (why) {
    g_set_error(error, ERR, 20, "%s", why);
    g_free(why);
    return FALSE;
  }
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  gboolean ok = FALSE;
  char *staged = staged_path(e->appimage, "");
  if (!s.ready || !g_file_test(staged, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, ERR, 21, "no new version is downloaded");
  } else if (skim_hd_update_vercmp(s.ready, e->version) <= 0) {
    g_set_error(error, ERR, 22, "%s is not newer than %s", s.ready, e->version);
    drop_stale(e, &s);
    st_save(e, &s);
  } else {
    char *sha = file_sha256(staged, error);
    if (sha && g_strcmp0(sha, s.ready_sha) != 0) {
      g_set_error(error, ERR, 23, "%s changed since it was checked — downloaded again at the "
                  "next check", staged);
      g_unlink(staged);
      set_str(&s.ready, NULL);
      set_str(&s.ready_sha, NULL);
      st_save(e, &s);
    } else if (sha && keep_prev(e->appimage, error)) {
      if (g_rename(staged, e->appimage) != 0) {
        g_set_error(error, ERR, 24, "%s: %s", e->appimage, g_strerror(errno));
      } else {
        ok = TRUE;
        const gint64 now = env_now(e);
        *version = g_strdup(s.ready);
        set_str(&s.prev, e->version);
        set_str(&s.pending_path, e->appimage);
        set_str(&s.pending_from, e->version);
        set_str(&s.pending_to, s.ready);
        s.crashes = 0;
        s.installed_at = now;
        char *msg = g_strdup_printf("%s installed in place of %s (kept as %s.prev)", s.ready,
                                    e->version, e->appimage);
        st_result(&s, now, msg);
        g_free(msg);
        set_str(&s.ready, NULL);
        set_str(&s.ready_sha, NULL);
        st_save(e, &s);
      }
    }
    g_free(sha);
  }
  g_free(staged);
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
  return ok;
}

gboolean skim_hd_update_rollback(const SkimHdUpdateEnv *e, const char *why, GError **error) {
  if (!e->appimage) {
    g_set_error(error, ERR, 30, "not running from an AppImage");
    return FALSE;
  }
  char *prev = prev_path(e->appimage);
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  gboolean ok = FALSE;
  if (!g_file_test(prev, G_FILE_TEST_IS_REGULAR)) {
    g_set_error(error, ERR, 31, "no earlier version is kept (%s)", prev);
  } else if (g_rename(prev, e->appimage) != 0) {
    g_set_error(error, ERR, 32, "%s: %s", e->appimage, g_strerror(errno));
  } else {
    ok = TRUE;
    char *msg = g_strdup_printf("back to %s from %s: %s", s.prev ? s.prev : "the earlier version",
                                e->version, why);
    st_result(&s, env_now(e), msg);
    g_free(msg);
    set_str(&s.skip, e->version);
    set_str(&s.prev, NULL);
    set_str(&s.pending_path, NULL);
    set_str(&s.pending_from, NULL);
    set_str(&s.pending_to, NULL);
    set_str(&s.running, NULL);
    s.crashes = 0;
    s.installed_at = 0;
    st_save(e, &s);
  }
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
  g_free(prev);
  return ok;
}

static gboolean pending_here(const SkimHdUpdateEnv *e, const St *s) {
  return e->appimage && g_strcmp0(s->pending_path, e->appimage) == 0;
}

gboolean skim_hd_update_boot(const SkimHdUpdateEnv *e) {
  if (!e->appimage) { return FALSE; }
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  gboolean back = FALSE;
  if (pending_here(e, &s)) {
    if (g_strcmp0(s.pending_to, e->version) != 0) {
      /* another version runs from this path now (put there by hand) */
      set_str(&s.pending_path, NULL);
      set_str(&s.pending_from, NULL);
      set_str(&s.pending_to, NULL);
      s.crashes = 0;
    } else if (g_strcmp0(s.running, e->appimage) == 0) {
      s.crashes++;
      g_warning("update: %s did not stop in order last time (%d of %d before it goes back to %s)",
                e->version, s.crashes, SKIM_HD_UPDATE_CRASHES, s.pending_from);
      if (s.crashes >= SKIM_HD_UPDATE_CRASHES) {
        st_save(e, &s);
        char *why = g_strdup_printf("%s crashed %d times after the update", e->version, s.crashes);
        GError *err = NULL;
        back = skim_hd_update_rollback(e, why, &err);
        if (back) {
          g_warning("update: %s — going back", why);
        } else {
          g_warning("update: %s, and going back failed: %s", why, err->message);
          g_clear_error(&err);
        }
        g_free(why);
      }
    }
  }
  if (!back) {
    if (s.crashes >= SKIM_HD_UPDATE_CRASHES) {   /* the rollback failed: it   */
      st_clear(&s);                              /* saved its own result      */
      st_load(e, &s);
    }
    set_str(&s.running, e->appimage);
    st_save(e, &s);
  }
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
  return back;
}

void skim_hd_update_stopped(const SkimHdUpdateEnv *e) {
  if (!e->appimage) { return; }
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  if (g_strcmp0(s.running, e->appimage) == 0) {
    set_str(&s.running, NULL);
    st_save(e, &s);
  }
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
}

void skim_hd_update_accept(const SkimHdUpdateEnv *e) {
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  if (pending_here(e, &s) && g_strcmp0(s.pending_to, e->version) == 0) {
    g_message("update: %s has run %d min since the update — kept (%s stays as .prev)",
              e->version, SKIM_HD_UPDATE_ACCEPT_S / 60, s.pending_from);
    set_str(&s.pending_path, NULL);
    set_str(&s.pending_from, NULL);
    set_str(&s.pending_to, NULL);
    s.crashes = 0;
    st_save(e, &s);
  }
  st_clear(&s);
  g_rec_mutex_unlock(&st_lock);
}

void skim_hd_update_exec(const char *path, char *const *argv) {
  DIR *d = opendir("/proc/self/fd");
  if (d) {
    const int self = dirfd(d);
    for (struct dirent *de; (de = readdir(d)) != NULL;) {
      const int fd = atoi(de->d_name);
      if (fd > 2 && fd != self) {
        const int fl = fcntl(fd, F_GETFD);
        if (fl >= 0) { fcntl(fd, F_SETFD, fl | FD_CLOEXEC); }
      }
    }
    closedir(d);
  }
  g_unsetenv("APPDIR");
  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer)path);
  for (guint i = 1; argv && argv[0] && argv[i]; i++) { g_ptr_array_add(av, argv[i]); }
  g_ptr_array_add(av, NULL);
  fflush(stdout);
  fflush(stderr);
  execv(path, (char **)av->pdata);
  g_ptr_array_free(av, TRUE);
}

/* --- the page's view ------------------------------------------------------------ */

static void jstr(GString *o, const char *k, const char *v) {
  g_string_append_printf(o, ",\"%s\":", k);
  if (!v) {
    g_string_append(o, "null");
    return;
  }
  g_string_append_c(o, '"');
  skim_json_append(o, v);
  g_string_append_c(o, '"');
}

static char *state_json(const SkimHdUpdateEnv *e, SkimHdUpdateMode mode, guint hour,
                        gboolean busy) {
  g_rec_mutex_lock(&st_lock);
  St s;
  st_load(e, &s);
  g_rec_mutex_unlock(&st_lock);
  const gint64 now = env_now(e);
  char *why = skim_hd_update_why_not(e);
  char *prev = e->appimage ? prev_path(e->appimage) : NULL;
  const gboolean has_prev = prev && g_file_test(prev, G_FILE_TEST_IS_REGULAR);
  GString *o = g_string_new("{");
  g_string_append_printf(o, "\"mode\":\"%s\",\"hour_utc\":%u", skim_hd_update_mode_name(mode),
                         hour);
  jstr(o, "version", e->version);
  jstr(o, "build", e->build && e->build[0] ? e->build : NULL);
  jstr(o, "appimage", e->appimage);
  jstr(o, "why_not", why);
  g_string_append_printf(o, ",\"busy\":%s,\"now\":%" G_GINT64_FORMAT ",\"last_check\":%"
                         G_GINT64_FORMAT ",\"next_check\":", busy ? "true" : "false", now,
                         s.last_try);
  if (mode == SKIM_HD_UPDATE_OFF) {
    g_string_append(o, "null");
  } else {
    g_string_append_printf(o, "%" G_GINT64_FORMAT, MAX(st_due(&s, now), now));
  }
  jstr(o, "result", s.result);
  g_string_append_printf(o, ",\"result_at\":%" G_GINT64_FORMAT, s.result_at);
  jstr(o, "latest", s.latest);
  jstr(o, "latest_url", s.latest_url);
  jstr(o, "latest_published", s.latest_published);
  g_string_append_printf(o, ",\"newer\":%s",
                         s.latest && skim_hd_update_vercmp(s.latest, e->version) > 0 ? "true"
                                                                                      : "false");
  jstr(o, "ready", staged_ok(e, &s, s.ready) && skim_hd_update_vercmp(s.ready, e->version) > 0
                       ? s.ready : NULL);
  jstr(o, "prev", has_prev ? (s.prev ? s.prev : "?") : NULL);
  jstr(o, "skip", s.skip);
  jstr(o, "pending", pending_here(e, &s) ? s.pending_from : NULL);
  g_string_append_printf(o, ",\"installed_at\":%" G_GINT64_FORMAT "}", s.installed_at);
  g_free(prev);
  g_free(why);
  st_clear(&s);
  return g_string_free(o, FALSE);
}

/* --- the app's handle ------------------------------------------------------------ */

typedef struct Flight {
  gatomicrefcount     ref;
  SkimHdUpdater      *u;              /* main thread only; NULL once let go   */
  SkimHdUpdateEnv     env;            /* its own strings, below              */
  char               *s[7];
  gboolean            force, download, install_after;
  SkimHdUpdateOutcome out;
  GMutex              lock;
  GCond               landed_cond;
  gboolean            landed;
} Flight;

struct _SkimHdUpdater {
  SkimHdUpdateEnv      env;
  char                *s[7];          /* the env's strings, owned            */
  SkimHdUpdateMode     mode;
  guint                hour;
  SkimHdUpdaterRestart restart;
  gpointer             user;
  guint                check_timer, minute_timer, accept_timer;
  gboolean             armed;
  Flight              *flight;
};

/* env's strings copied into s[] and pointed at. */
static void env_copy(SkimHdUpdateEnv *dst, char *s[7], const SkimHdUpdateEnv *src) {
  *dst = *src;
  const char *in[7] = { src->appimage, src->version, src->build, src->config_path,
                        src->state_path, src->base_url, src->ca_path };
  for (guint i = 0; i < 7; i++) { s[i] = g_strdup(in[i]); }
  dst->appimage = s[0];
  dst->version = s[1];
  dst->build = s[2];
  dst->config_path = s[3];
  dst->state_path = s[4];
  dst->base_url = s[5];
  dst->ca_path = s[6];
}

static void flight_unref(Flight *f) {
  if (!g_atomic_ref_count_dec(&f->ref)) { return; }
  for (guint i = 0; i < 7; i++) { g_free(f->s[i]); }
  g_object_unref(f->env.cancel);
  g_mutex_clear(&f->lock);
  g_cond_clear(&f->landed_cond);
  g_free(f);
}

static void flight_thread(GTask *task, gpointer src, gpointer data, GCancellable *unused) {
  (void)src;
  (void)unused;
  Flight *f = data;
  skim_hd_update_check(&f->env, f->force, f->download, &f->out);
  g_mutex_lock(&f->lock);
  f->landed = TRUE;
  g_cond_broadcast(&f->landed_cond);
  g_mutex_unlock(&f->lock);
  g_task_return_boolean(task, TRUE);
}

static void check_arm(SkimHdUpdater *u, gint64 wait_s);

static guint utc_hour(const SkimHdUpdater *u) {
  const gint64 now = env_now(&u->env);
  GDateTime *t = g_date_time_new_from_unix_utc(now);
  const guint h = t ? (guint)g_date_time_get_hour(t) : 0;
  if (t) { g_date_time_unref(t); }
  return h;
}

static void do_install(SkimHdUpdater *u, const char *how) {
  char *version = NULL;
  GError *err = NULL;
  if (skim_hd_update_install(&u->env, &version, &err)) {
    g_message("update: %s installed (%s) — restarting into it", version, how);
    char *why = g_strdup_printf("the update to %s", version);
    if (u->restart) { u->restart(why, u->user); }
    g_free(why);
    g_free(version);
  } else {
    g_warning("update: not installed: %s", err->message);
    g_error_free(err);
  }
}

static void flight_done(GObject *src, GAsyncResult *res, gpointer data) {
  (void)src;
  (void)res;
  Flight *f = data;
  SkimHdUpdater *u = f->u;
  if (u) {
    u->flight = NULL;
    f->u = NULL;
    flight_unref(f);
    if (f->out.result != SKIM_HD_UPDATE_NOT_DUE) {
      if (f->out.result == SKIM_HD_UPDATE_FAILED) {
        g_warning("update: %s", f->out.detail);
      } else {
        g_message("update: %s", f->out.detail);
      }
    }
    if (f->out.result == SKIM_HD_UPDATE_READY &&
        (f->install_after ||
         (u->mode == SKIM_HD_UPDATE_AUTO && u->armed && utc_hour(u) == u->hour))) {
      do_install(u, f->install_after ? "asked for on the web page" : "the install hour");
    } else if (f->install_after && f->out.result != SKIM_HD_UPDATE_READY) {
      g_message("update: nothing to install (%s)", f->out.detail);
    }
    if (u->armed && u->mode != SKIM_HD_UPDATE_OFF) { check_arm(u, f->out.wait_s); }
  }
  flight_unref(f);
}

static void fly(SkimHdUpdater *u, gboolean force, gboolean download, gboolean install_after) {
  Flight *f = g_new0(Flight, 1);
  g_atomic_ref_count_init(&f->ref);
  g_atomic_ref_count_inc(&f->ref);             /* one for u->flight            */
  f->u = u;
  env_copy(&f->env, f->s, &u->env);
  f->env.cancel = g_cancellable_new();
  f->env.now_s = u->env.now_s;
  f->env.now_user = u->env.now_user;
  f->force = force;
  f->download = download;
  f->install_after = install_after;
  g_mutex_init(&f->lock);
  g_cond_init(&f->landed_cond);
  u->flight = f;
  GTask *t = g_task_new(NULL, NULL, flight_done, f);
  g_task_set_task_data(t, f, NULL);
  g_task_run_in_thread(t, flight_thread);
  g_object_unref(t);
}

static gboolean check_tick(gpointer data) {
  SkimHdUpdater *u = data;
  u->check_timer = 0;
  if (u->armed && !u->flight) { fly(u, FALSE, u->mode == SKIM_HD_UPDATE_AUTO, FALSE); }
  return G_SOURCE_REMOVE;
}

/* The limits are on the wall clock, a GLib timer on the monotonic one (it
 * stands still through a suspend): never longer than an hour, then the
 * worker says "not due" without a request. */
static void check_arm(SkimHdUpdater *u, gint64 wait_s) {
  if (u->check_timer) { g_source_remove(u->check_timer); }
  u->check_timer = g_timeout_add_seconds((guint)CLAMP(wait_s, 60, 3600) + 5, check_tick, u);
}

static gboolean minute_tick(gpointer data) {
  SkimHdUpdater *u = data;
  if (!u->armed || u->flight || utc_hour(u) != u->hour) { return G_SOURCE_CONTINUE; }
  St s;
  g_rec_mutex_lock(&st_lock);
  st_load(&u->env, &s);
  g_rec_mutex_unlock(&st_lock);
  const gboolean ready = staged_ok(&u->env, &s, s.ready) &&
                         skim_hd_update_vercmp(s.ready, u->env.version) > 0;
  st_clear(&s);
  if (ready) { do_install(u, "the install hour"); }
  return G_SOURCE_CONTINUE;
}

static gboolean accept_tick(gpointer data) {
  SkimHdUpdater *u = data;
  u->accept_timer = 0;
  skim_hd_update_accept(&u->env);
  return G_SOURCE_REMOVE;
}

SkimHdUpdater *skim_hd_updater_new(const SkimHdUpdateEnv *env, SkimHdUpdateMode mode,
                                   guint hour_utc, SkimHdUpdaterRestart restart, gpointer user) {
  curl_ready();                                /* on the main thread, once     */
  SkimHdUpdater *u = g_new0(SkimHdUpdater, 1);
  env_copy(&u->env, u->s, env);
  u->env.cancel = NULL;
  u->mode = mode;
  u->hour = hour_utc % 24;
  u->restart = restart;
  u->user = user;
  return u;
}

void skim_hd_updater_start(SkimHdUpdater *u, guint delay_s) {
  if (!u || u->armed) { return; }
  u->armed = TRUE;
  if (u->mode != SKIM_HD_UPDATE_OFF) {
    u->check_timer = g_timeout_add_seconds(MAX(delay_s, 1u), check_tick, u);
  }
  if (u->mode == SKIM_HD_UPDATE_AUTO) { u->minute_timer = g_timeout_add_seconds(60, minute_tick, u); }
  u->accept_timer = g_timeout_add_seconds(SKIM_HD_UPDATE_ACCEPT_S, accept_tick, u);
}

gboolean skim_hd_updater_busy(const SkimHdUpdater *u) {
  return u && u->flight != NULL;
}

char *skim_hd_updater_action(SkimHdUpdater *u, const char *action) {
  if (g_strcmp0(action, "rollback") == 0) {
    if (u->flight) { return g_strdup("a check is running — try again in a moment"); }
    GError *err = NULL;
    if (!skim_hd_update_rollback(&u->env, "asked for on the web page", &err)) {
      char *m = g_strdup(err->message);
      g_error_free(err);
      return m;
    }
    g_message("update: the earlier version is back in place — restarting into it");
    if (u->restart) { u->restart("back to the earlier version", u->user); }
    return NULL;
  }
  const gboolean install = g_strcmp0(action, "install") == 0;
  if (!install && g_strcmp0(action, "check") != 0) {
    return g_strdup_printf("unknown action \"%s\"", action ? action : "");
  }
  if (u->flight) { return g_strdup("a check is running already"); }
  if (install) {
    char *why = skim_hd_update_why_not(&u->env);
    if (why) { return why; }
    St s;
    g_rec_mutex_lock(&st_lock);
    st_load(&u->env, &s);
    g_rec_mutex_unlock(&st_lock);
    const gboolean ready = staged_ok(&u->env, &s, s.ready) &&
                           skim_hd_update_vercmp(s.ready, u->env.version) > 0;
    st_clear(&s);
    if (ready) {
      do_install(u, "asked for on the web page");
      return NULL;
    }
  }
  fly(u, TRUE, install || u->mode == SKIM_HD_UPDATE_AUTO, install);
  return NULL;
}

char *skim_hd_updater_json(const SkimHdUpdater *u) {
  return state_json(&u->env, u->mode, u->hour, u->flight != NULL);
}

#define TEARDOWN_GRACE_US (400 * 1000)

void skim_hd_updater_free(SkimHdUpdater *u) {
  if (!u) { return; }
  u->armed = FALSE;
  if (u->check_timer) { g_source_remove(u->check_timer); }
  if (u->minute_timer) { g_source_remove(u->minute_timer); }
  if (u->accept_timer) { g_source_remove(u->accept_timer); }
  Flight *f = u->flight;
  u->flight = NULL;
  if (f) {
    f->u = NULL;
    g_cancellable_cancel(f->env.cancel);
    const gint64 until = g_get_monotonic_time() + TEARDOWN_GRACE_US;
    g_mutex_lock(&f->lock);
    while (!f->landed && g_cond_wait_until(&f->landed_cond, &f->lock, until)) { }
    g_mutex_unlock(&f->lock);
    flight_unref(f);
  }
  for (guint i = 0; i < 7; i++) { g_free(u->s[i]); }
  g_free(u);
}
