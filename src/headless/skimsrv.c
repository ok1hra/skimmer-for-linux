/* skimsrv.c — see skimsrv.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "skimsrv.h"

#include "json.h"

#include <glib/gstdio.h>
#include <string.h>

static const char *const KEYS[] = {
  "Rate", "CenterFreqs48", "CenterFreqs96", "CenterFreqs192",
  "SegmentSel48", "SegmentSel96", "SegmentSel192", "FreqCalibration",
};

/* a value that may leave: digits, commas, one kind of dot, nothing else */
static gboolean value_ok(const char *v) {
  if (!*v || strlen(v) > 1024) { return FALSE; }
  for (const char *p = v; *p; p++) {
    if (!g_ascii_isdigit(*p) && *p != ',' && *p != '.') { return FALSE; }
  }
  return TRUE;
}

/* The [Skimmer] keys of one file, as ini text; NULL when it has none. */
static char *skimmer_keys(const char *path) {
  char *text = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &text, &len, NULL)) { return NULL; }
  GString *o = g_string_new("[Skimmer]\n");
  guint n = 0;
  gboolean in = FALSE;
  char **lines = g_strsplit(text, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *s = g_strstrip(*l);                         /* the \r of a Windows file too */
    if (s[0] == '[') { in = g_ascii_strncasecmp(s, "[Skimmer]", 9) == 0; continue; }
    if (!in) { continue; }
    char *eq = strchr(s, '=');
    if (!eq) { continue; }
    *eq = '\0';
    const char *k = g_strstrip(s), *v = g_strstrip(eq + 1);
    for (guint i = 0; i < G_N_ELEMENTS(KEYS); i++) {
      if (strcmp(k, KEYS[i]) == 0 && value_ok(v)) {
        g_string_append_printf(o, "%s=%s\n", KEYS[i], v);
        n++;
      }
    }
  }
  g_strfreev(lines);
  g_free(text);
  if (!n) { g_string_free(o, TRUE); return NULL; }
  return g_string_free(o, FALSE);
}

/* every <prefix>/drive_c/users/<user>/<appdata>/Afreet/Products/SkimSrv/SkimSrv.ini */
static void candidates(const char *prefix, GPtrArray *out, GPtrArray *looked) {
  static const char *const APPDATA[] = { "Application Data", "AppData/Roaming" };
  char *users = g_build_filename(prefix, "drive_c", "users", NULL);
  g_ptr_array_add(looked, g_build_filename(users, "*", "…", "Afreet", "Products", "SkimSrv",
                                           "SkimSrv.ini", NULL));
  GDir *d = g_dir_open(users, 0, NULL);
  for (const char *u; d && (u = g_dir_read_name(d));) {
    for (guint i = 0; i < G_N_ELEMENTS(APPDATA); i++) {
      char *p = g_build_filename(users, u, APPDATA[i], "Afreet", "Products", "SkimSrv", "SkimSrv.ini", NULL);
      if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) { g_ptr_array_add(out, p); } else { g_free(p); }
    }
  }
  if (d) { g_dir_close(d); }
  g_free(users);
}

char *skim_hd_skimsrv_json(const char *home, const char *wineprefix) {
  GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *looked = g_ptr_array_new_with_free_func(g_free);
  if (wineprefix && *wineprefix) { candidates(wineprefix, found, looked); }
  char *dflt = home ? g_build_filename(home, ".wine", NULL) : NULL;
  if (dflt && g_strcmp0(dflt, wineprefix) != 0) { candidates(dflt, found, looked); }
  g_free(dflt);
  /* the newest file that has the keys */
  const char *best = NULL;
  char *ini = NULL;
  gint64 best_t = G_MININT64;
  for (guint i = 0; i < found->len; i++) {
    GStatBuf st;
    if (g_stat(found->pdata[i], &st) != 0 || (gint64)st.st_mtime <= best_t) { continue; }
    char *k = skimmer_keys(found->pdata[i]);
    if (!k) { continue; }
    g_free(ini);
    ini = k;
    best = found->pdata[i];
    best_t = (gint64)st.st_mtime;
  }
  GString *o = g_string_new(NULL);
  if (best) {
    g_string_append(o, "{\"found\":true,\"path\":\"");
    skim_json_append(o, best);
    g_string_append(o, "\",\"ini\":\"");
    skim_json_append(o, ini);
    g_string_append(o, "\"}");
  } else {
    g_string_append(o, "{\"found\":false,\"looked\":[");
    for (guint i = 0; i < looked->len; i++) {
      g_string_append(o, i ? ",\"" : "\"");
      skim_json_append(o, looked->pdata[i]);
      g_string_append_c(o, '"');
    }
    g_string_append(o, "]}");
  }
  g_free(ini);
  g_ptr_array_unref(found);
  g_ptr_array_unref(looked);
  return g_string_free(o, FALSE);
}
