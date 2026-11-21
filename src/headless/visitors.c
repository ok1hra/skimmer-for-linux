/* visitors.c — see visitors.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "visitors.h"

#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define KEY_BYTES 32

typedef struct {
  char        *key;                    /* "wf:20m@14.026" / "audio:20m@14.0253" */
  gint64       secs;
} Dwell;

typedef struct {
  char         band[16];
  char        *wkey, *akey;            /* what it watches now (akey NULL: no audio) */
  gint64       since;
} View;

typedef struct {
  char         call[SKIM_VIS_CALL_MAX + 1];
  char        *addr, *fwd;
  gint64       first, last;
  GHashTable  *dwell;                  /* key → gint64 seconds (boxed)        */
  GHashTable  *views;                  /* conn → View                          */
} Visit;

struct _SkimHdVisitors {
  char        *dir, *log, *failed;
  guint8       key[KEY_BYTES];
  GHashTable  *visits;                 /* token → Visit                        */
  gint64       fail_min;               /* the minute counted                   */
  guint        fail_n, fail_dropped;
};

/* ---- the callsign ------------------------------------------------------------- */

gboolean skim_hd_visitor_call_ok(const char *in, char *out, const char **why) {
  char buf[SKIM_VIS_CALL_MAX + 1];
  guint n = 0, digits = 0, letters = 0;
  out[0] = '\0';
  for (const char *p = in ? in : ""; *p; p++) {
    if (g_ascii_isspace(*p)) { continue; }
    const char c = g_ascii_toupper(*p);
    if (!g_ascii_isalnum(c) && c != '/') {
      *why = "a callsign has letters, digits and / only";
      return FALSE;
    }
    if (n == SKIM_VIS_CALL_MAX) {
      *why = "a callsign is 15 characters at most";
      return FALSE;
    }
    digits += g_ascii_isdigit(c) != 0;
    letters += g_ascii_isalpha(c) != 0;
    buf[n++] = c;
  }
  buf[n] = '\0';
  if (n < 3) {
    *why = n ? "a callsign has 3 characters at least" : "no callsign entered";
    return FALSE;
  }
  if (!digits || !letters) {
    *why = "a callsign has a digit and a letter at least";
    return FALSE;
  }
  if (buf[0] == '/' || buf[n - 1] == '/' || strstr(buf, "//")) {
    *why = "not a callsign";
    return FALSE;
  }
  memcpy(out, buf, n + 1);
  return TRUE;
}

/* ---- the key and the token ------------------------------------------------------ */

static gboolean key_load(SkimHdVisitors *v, GError **error) {
  char *path = g_build_filename(v->dir, "visitor.key", NULL);
  char *hex = NULL;
  gboolean ok = FALSE;
  if (g_file_get_contents(path, &hex, NULL, NULL) && strlen(g_strstrip(hex)) == 2 * KEY_BYTES) {
    ok = TRUE;
    for (int i = 0; i < KEY_BYTES && ok; i++) {
      const int hi = g_ascii_xdigit_value(hex[2 * i]), lo = g_ascii_xdigit_value(hex[2 * i + 1]);
      ok = hi >= 0 && lo >= 0;
      v->key[i] = (guint8)(hi * 16 + lo);
    }
  }
  g_free(hex);
  if (!ok) {                                         /* a new one             */
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ok = fd >= 0 && read(fd, v->key, KEY_BYTES) == KEY_BYTES;
    if (fd >= 0) { close(fd); }
    if (!ok) {
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_IO, "no random bytes for the visitors' key");
    } else {
      GString *h = g_string_new(NULL);
      for (int i = 0; i < KEY_BYTES; i++) { g_string_append_printf(h, "%02x", v->key[i]); }
      g_string_append_c(h, '\n');
      const int kfd = g_open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      ok = kfd >= 0 && write(kfd, h->str, h->len) == (ssize_t)h->len;
      if (kfd >= 0) { close(kfd); }
      if (!ok) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s: %s", path,
                    g_strerror(errno));
      }
      g_string_free(h, TRUE);
    }
  }
  g_free(path);
  return ok;
}

static char *sign(const SkimHdVisitors *v, const char *msg) {
  return g_compute_hmac_for_string(G_CHECKSUM_SHA256, v->key, KEY_BYTES, msg, -1);
}

char *skim_hd_visitors_token(SkimHdVisitors *v, const char *call, gint64 now) {
  char *msg = g_strdup_printf("%s.%" G_GINT64_FORMAT, call, now);
  char *mac = sign(v, msg);
  char *t = g_strdup_printf("%s.%s", msg, mac);
  g_free(mac);
  g_free(msg);
  return t;
}

gboolean skim_hd_visitors_check(SkimHdVisitors *v, const char *token, gint64 now, char *call) {
  if (!token || strlen(token) > 128) { return FALSE; }
  const char *d2 = strrchr(token, '.');
  if (!d2 || d2 == token) { return FALSE; }
  char *msg = g_strndup(token, (gsize)(d2 - token));
  char *mac = sign(v, msg);
  /* the same length, then every byte compared (no early way out) */
  gboolean ok = strlen(mac) == strlen(d2 + 1);
  guint diff = 0;
  for (gsize i = 0; ok && mac[i]; i++) { diff |= (guint)(mac[i] ^ d2[1 + i]); }
  ok = ok && diff == 0;
  g_free(mac);
  const char *d1 = ok ? strrchr(msg, '.') : NULL;
  if (d1) {
    char *end = NULL;
    const gint64 at = g_ascii_strtoll(d1 + 1, &end, 10);
    const char *why;
    char c[SKIM_VIS_CALL_MAX + 1];
    char *cs = g_strndup(msg, (gsize)(d1 - msg));
    ok = end && !*end && now - at < SKIM_VIS_MAX_AGE_S && at - now < 86400 &&
         skim_hd_visitor_call_ok(cs, c, &why) && strcmp(c, cs) == 0;
    if (ok) { g_strlcpy(call, c, SKIM_VIS_CALL_MAX + 1); }
    g_free(cs);
  } else {
    ok = FALSE;
  }
  g_free(msg);
  return ok;
}

/* ---- the files ------------------------------------------------------------------ */

static void iso(gint64 t, char out[24]) {
  GDateTime *d = g_date_time_new_from_unix_utc(t);
  char *s = g_date_time_format(d, "%Y-%m-%dT%H:%M:%SZ");
  g_strlcpy(out, s, 24);
  g_free(s);
  g_date_time_unref(d);
}

/* One line onto path; at SKIM_VIS_ROLL_BYTES the file becomes path.1 first. */
static void append(const char *path, const char *line) {
  GStatBuf st;
  if (g_stat(path, &st) == 0 && st.st_size >= SKIM_VIS_ROLL_BYTES) {
    char *old = g_strconcat(path, ".1", NULL);
    g_rename(path, old);
    g_free(old);
  }
  FILE *f = g_fopen(path, "a");
  if (!f) {
    g_warning("visitors: %s: %s", path, g_strerror(errno));
    return;
  }
  fputs(line, f);
  fclose(f);
}

/* No tab, line break or control character in a field; a hyphen for none. */
static char *field(const char *s) {
  if (!s || !*s) { return g_strdup("-"); }
  GString *o = g_string_new(NULL);
  for (const guchar *p = (const guchar *)s; *p; p++) {
    if (*p < 0x20 || *p == 0x7f || *p == '\\') { g_string_append_printf(o, "\\x%02x", *p); }
    else { g_string_append_c(o, (char)*p); }
  }
  return g_string_free(o, FALSE);
}

/* ---- visits ------------------------------------------------------------------------ */

static void view_free(gpointer p) {
  View *w = p;
  g_free(w->wkey);
  g_free(w->akey);
  g_free(w);
}

static void visit_free(gpointer p) {
  Visit *s = p;
  g_free(s->addr);
  g_free(s->fwd);
  g_hash_table_unref(s->dwell);
  g_hash_table_unref(s->views);
  g_free(s);
}

static void dwell_add(Visit *s, const char *key, gint64 secs) {
  if (!key || secs <= 0) { return; }
  gint64 *t = g_hash_table_lookup(s->dwell, key);
  if (!t) {
    t = g_new0(gint64, 1);
    g_hash_table_insert(s->dwell, g_strdup(key), t);
  }
  *t += secs;
}

/* What a view has watched up to now goes to its visit's totals. */
static void view_account(Visit *s, View *w, gint64 now) {
  dwell_add(s, w->wkey, now - w->since);
  dwell_add(s, w->akey, now - w->since);
  w->since = now;
}

static gint dwell_cmp(gconstpointer a, gconstpointer b) {
  const Dwell *x = a, *y = b;
  return x->secs < y->secs ? 1 : x->secs > y->secs ? -1 : strcmp(x->key, y->key);
}

/* The frequencies watched long enough, the longest first: "k=s,k=s". */
static char *dwell_list(Visit *s, gint64 now) {
  GHashTableIter it;
  gpointer k, w;
  g_hash_table_iter_init(&it, s->views);
  while (g_hash_table_iter_next(&it, &k, &w)) { view_account(s, w, now); }
  GArray *a = g_array_new(FALSE, FALSE, sizeof(Dwell));
  gpointer t;
  g_hash_table_iter_init(&it, s->dwell);
  while (g_hash_table_iter_next(&it, &k, &t)) {
    if (*(gint64 *)t >= SKIM_VIS_DWELL_S) {
      const Dwell d = { k, *(gint64 *)t };
      g_array_append_val(a, d);
    }
  }
  g_array_sort(a, dwell_cmp);
  GString *o = g_string_new(NULL);
  for (guint i = 0; i < a->len && i < 50; i++) {
    const Dwell *d = &g_array_index(a, Dwell, i);
    g_string_append_printf(o, "%s%s=%" G_GINT64_FORMAT, i ? "," : "", d->key, d->secs);
  }
  g_array_free(a, TRUE);
  return g_string_free(o, FALSE);
}

static void visit_log(SkimHdVisitors *v, Visit *s, gint64 now) {
  char a[24], b[24];
  iso(s->first, a);
  iso(s->last, b);
  char *fr = dwell_list(s, now);
  char *ad = field(s->addr), *fw = field(s->fwd), *fq = field(fr);
  char *line = g_strdup_printf("%s\t%s\t%" G_GINT64_FORMAT "\t%s\t%s\t%s\t%s\n", a, b,
                               s->last - s->first, s->call, ad, fw, fq);
  append(v->log, line);
  g_message("visitors: %s from %s left after %" G_GINT64_FORMAT " s", s->call, s->addr,
            s->last - s->first);
  g_free(line);
  g_free(ad); g_free(fw); g_free(fq); g_free(fr);
}

SkimHdVisitors *skim_hd_visitors_new(const char *dir, GError **error) {
  SkimHdVisitors *v = g_new0(SkimHdVisitors, 1);
  v->dir = g_strdup(dir);
  g_mkdir_with_parents(dir, 0755);
  if (!key_load(v, error)) {
    g_free(v->dir);
    g_free(v);
    return NULL;
  }
  v->log = g_build_filename(dir, "visitors.tsv", NULL);
  v->failed = g_build_filename(dir, "visitors-failed.tsv", NULL);
  v->visits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, visit_free);
  return v;
}

void skim_hd_visitors_free(SkimHdVisitors *v) {
  if (!v) { return; }
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  GHashTableIter it;
  gpointer k, s;
  g_hash_table_iter_init(&it, v->visits);
  while (g_hash_table_iter_next(&it, &k, &s)) { visit_log(v, s, MAX(now, ((Visit *)s)->last)); }
  g_hash_table_unref(v->visits);
  g_free(v->dir);
  g_free(v->log);
  g_free(v->failed);
  g_free(v);
}

const char *skim_hd_visitors_path(const SkimHdVisitors *v) { return v->log; }

static Visit *visit_of(SkimHdVisitors *v, const char *token, const char *call, const char *addr,
                       const char *fwd, gint64 now) {
  Visit *s = g_hash_table_lookup(v->visits, token);
  if (!s) {
    s = g_new0(Visit, 1);
    g_strlcpy(s->call, call, sizeof(s->call));
    s->addr = g_strdup(addr);
    s->fwd = g_strdup(fwd);
    s->first = now;
    s->dwell = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->views = g_hash_table_new_full(NULL, NULL, NULL, view_free);
    g_hash_table_insert(v->visits, g_strdup(token), s);
    g_message("visitors: %s from %s%s%s", call, addr ? addr : "?", fwd ? ", forwarded for " : "",
              fwd ? fwd : "");
  }
  s->last = MAX(s->last, now);
  return s;
}

void skim_hd_visitors_seen(SkimHdVisitors *v, const char *token, const char *call,
                           const char *addr, const char *fwd, gint64 now) {
  visit_of(v, token, call, addr, fwd, now);
}

void skim_hd_visitors_leave(SkimHdVisitors *v, const char *token, gint64 now) {
  Visit *s = token ? g_hash_table_lookup(v->visits, token) : NULL;
  if (!s) { return; }
  s->last = MAX(s->last, now);
  visit_log(v, s, now);
  g_hash_table_remove(v->visits, token);
}

void skim_hd_visitors_view(SkimHdVisitors *v, const char *token, gconstpointer conn,
                           const char *band, double lo, double hi, gboolean audio, double af,
                           gint64 now) {
  Visit *s = token ? g_hash_table_lookup(v->visits, token) : NULL;
  if (!s) { return; }                                /* not a visit (yet)     */
  s->last = MAX(s->last, now);
  View *w = g_hash_table_lookup(s->views, conn);
  if (w) { view_account(s, w, now); }
  if (!band) {
    g_hash_table_remove(s->views, conn);
    return;
  }
  if (!w) {
    w = g_new0(View, 1);
    g_hash_table_insert(s->views, (gpointer)conn, w);
  }
  g_strlcpy(w->band, band, sizeof(w->band));
  g_free(w->wkey);
  g_free(w->akey);
  char mhz[G_ASCII_DTOSTR_BUF_SIZE];
  w->wkey = g_strdup_printf("wf:%s@%s", band,
                            g_ascii_formatd(mhz, sizeof(mhz), "%.3f", round((lo + hi) / 2 / 1e3) / 1e3));
  w->akey = audio ? g_strdup_printf("audio:%s@%s", band,
                                    g_ascii_formatd(mhz, sizeof(mhz), "%.4f", round(af / 100) / 1e4))
                  : NULL;
  w->since = now;
}

void skim_hd_visitors_failed(SkimHdVisitors *v, gint64 now, const char *addr, const char *fwd,
                             const char *typed, const char *why) {
  if (now / 60 != v->fail_min) {
    if (v->fail_dropped) {
      char t[24];
      iso(v->fail_min * 60 + 59, t);
      char *line = g_strdup_printf("%s\t-\t-\t-\t%u more refused sign-ins that minute, not logged\n",
                                   t, v->fail_dropped);
      append(v->failed, line);
      g_free(line);
    }
    v->fail_min = now / 60;
    v->fail_n = 0;
    v->fail_dropped = 0;
  }
  if (v->fail_n >= SKIM_VIS_FAILS_MIN) {
    v->fail_dropped++;
    return;
  }
  v->fail_n++;
  char t[24];
  iso(now, t);
  char *cut = g_strndup(typed ? typed : "", 32);
  char *ad = field(addr), *fw = field(fwd), *ty = field(cut), *wy = field(why);
  char *line = g_strdup_printf("%s\t%s\t%s\t%s\t%s\n", t, ad, fw, ty, wy);
  append(v->failed, line);
  g_free(line);
  g_free(ad); g_free(fw); g_free(ty); g_free(wy); g_free(cut);
}

void skim_hd_visitors_tick(SkimHdVisitors *v, gint64 now) {
  GHashTableIter it;
  gpointer k, p;
  g_hash_table_iter_init(&it, v->visits);
  while (g_hash_table_iter_next(&it, &k, &p)) {
    Visit *s = p;
    if (now - s->last >= SKIM_VIS_IDLE_S && g_hash_table_size(s->views) == 0) {
      visit_log(v, s, s->last);
      g_hash_table_iter_remove(&it);
    }
  }
}

/* ---- the managers' view ---------------------------------------------------------- */

/* The last n lines of path (and its .1 when path has fewer), newest first. */
static GPtrArray *tail(const char *path, guint n) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  char *old = g_strconcat(path, ".1", NULL);
  const char *files[] = { path, old };
  for (int f = 0; f < 2 && out->len < n; f++) {
    char *text = NULL;
    if (!g_file_get_contents(files[f], &text, NULL, NULL)) { continue; }
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = (int)g_strv_length(lines) - 1; i >= 0 && out->len < n; i--) {
      if (lines[i][0]) { g_ptr_array_add(out, g_strdup(lines[i])); }
    }
    g_strfreev(lines);
    g_free(text);
  }
  g_free(old);
  return out;
}

static void json_row(GString *o, const char *line, const char *const *names, guint nn) {
  char **f = g_strsplit(line, "\t", -1);
  g_string_append_c(o, '{');
  for (guint i = 0; i < nn; i++) {
    const char *x = i < g_strv_length(f) ? f[i] : "";
    g_string_append_printf(o, "%s\"%s\":\"", i ? "," : "", names[i]);
    skim_json_append(o, strcmp(x, "-") == 0 ? "" : x);
    g_string_append_c(o, '"');
  }
  g_string_append_c(o, '}');
  g_strfreev(f);
}

/* A log line's end (its second field) in unix s, 0 when it does not parse. */
static gint64 line_end(const char *line) {
  const char *t = strchr(line, '\t');
  if (!t) { return 0; }
  char *e = g_strndup(t + 1, strcspn(t + 1, "\t"));
  GDateTime *d = g_date_time_new_from_iso8601(e, NULL);
  g_free(e);
  if (!d) { return 0; }
  const gint64 u = g_date_time_to_unix(d);
  g_date_time_unref(d);
  return u;
}

char *skim_hd_visitors_public_json(SkimHdVisitors *v, guint hours, gint64 now) {
  static const char *const VN[] = { "start", "end", "secs", "call" };
  GString *o = g_string_new(NULL);
  g_string_append_printf(o, "{\"public\":true,\"hours\":%u,\"open\":[", hours);
  GHashTableIter it;
  gpointer k, p;
  guint c = 0;
  g_hash_table_iter_init(&it, v->visits);
  while (g_hash_table_iter_next(&it, &k, &p)) {
    Visit *s = p;
    char a[24];
    iso(s->first, a);
    g_string_append_printf(o, "%s{\"start\":\"%s\",\"secs\":%" G_GINT64_FORMAT ",\"call\":\"%s\"}",
                           c++ ? "," : "", a, s->last - s->first, s->call);
  }
  g_string_append(o, "],\"visits\":[");
  /* the files are in end order: newest first, until one ended too long ago */
  GPtrArray *t = tail(v->log, 20000);
  const gint64 since = now - (gint64)hours * 3600;
  c = 0;
  for (guint i = 0; i < t->len; i++) {
    const gint64 end = line_end(t->pdata[i]);
    if (end && end < since) { break; }
    if (c++) { g_string_append_c(o, ','); }
    json_row(o, t->pdata[i], VN, G_N_ELEMENTS(VN));
  }
  g_ptr_array_unref(t);
  g_string_append(o, "]}");
  return g_string_free(o, FALSE);
}

char *skim_hd_visitors_json(SkimHdVisitors *v, guint n, gint64 now) {
  static const char *const VN[] = { "start", "end", "secs", "call", "addr", "fwd", "freqs" };
  static const char *const FN[] = { "time", "addr", "fwd", "typed", "why" };
  GString *o = g_string_new("{\"open\":[");
  GHashTableIter it;
  gpointer k, p;
  guint c = 0;
  g_hash_table_iter_init(&it, v->visits);
  while (g_hash_table_iter_next(&it, &k, &p)) {
    Visit *s = p;
    char a[24];
    iso(s->first, a);
    char *fr = dwell_list(s, now);
    g_string_append_printf(o, "%s{\"start\":\"%s\",\"secs\":%" G_GINT64_FORMAT ",\"call\":\"%s\","
                           "\"addr\":\"", c++ ? "," : "", a, s->last - s->first, s->call);
    skim_json_append(o, s->addr);
    g_string_append(o, "\",\"fwd\":\"");
    skim_json_append(o, s->fwd);
    g_string_append(o, "\",\"freqs\":\"");
    skim_json_append(o, fr);
    g_string_append(o, "\",\"watching\":");
    g_string_append(o, g_hash_table_size(s->views) ? "true}" : "false}");
    g_free(fr);
  }
  const char *const paths[] = { v->log, v->failed };
  const char *const keys[] = { "visits", "failed" };
  for (int f = 0; f < 2; f++) {
    g_string_append_printf(o, "],\"%s\":[", keys[f]);
    GPtrArray *t = tail(paths[f], n);
    for (guint i = 0; i < t->len; i++) {
      if (i) { g_string_append_c(o, ','); }
      json_row(o, t->pdata[i], f ? FN : VN, f ? G_N_ELEMENTS(FN) : G_N_ELEMENTS(VN));
    }
    g_ptr_array_unref(t);
  }
  g_string_append(o, "]}");
  return g_string_free(o, FALSE);
}
