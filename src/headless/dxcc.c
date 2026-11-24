/* dxcc.c — a call's DXCC from the web page's map data (see dxcc.h).
 *
 * A port of SkMap.locate (web/map.js) and canonicalizeDxccPrefixAlias
 * (web/map/dxcc_prefix_aliases.js, IP-rotator's): the call's longest part
 * (less /P, /M, /MM, /AM, /QRP, /digit), its alias (the special cases by
 * hand, as the JS has them, then the longest alias prefix), and the label
 * of that prefix, or of its first 4, 3, 2, 1 characters; the same for the
 * part itself. The tables are parsed from the resource's JS once. A new
 * special case in dxcc_prefix_aliases.js needs its line here too.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "headless/dxcc.h"

#include <gio/gio.h>
#include <string.h>

#define RES "/skimmer-for-linux/web/map/"

typedef struct { char *prefix, *name, *also; } Dxcc;

static GHashTable *pfx;      /* prefix → Dxcc (the first label of a prefix) */
static GHashTable *alias;    /* alias prefix → canonical prefix             */
static guint alias_max;      /* the longest alias prefix                    */

/* Other names a DXCC goes by, for the bad-calls filter's country names. */
static const struct { const char *prefix, *also; } ALSO[] = {
  { "EU", "BELORUSSIA BYELORUSSIA" },
  { "UA2", "RUSSIA" },
  { "R1FJ", "RUSSIA" },
};

static char *res_text(const char *file) {
  GBytes *b = g_resources_lookup_data(file, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (!b) { return NULL; }
  gsize n = 0;
  const char *d = g_bytes_get_data(b, &n);
  char *s = g_strndup(d, n);
  g_bytes_unref(b);
  return s;
}

static void load_labels(void) {
  char *js = res_text(RES "map50.js");
  if (!js) { return; }
  const char *p = strstr(js, "DXCC_PREFIX_LABELS");
  const char *end = p ? strstr(p, "];") : NULL;
  if (p && end) {
    char *sect = g_strndup(p, (gsize)(end - p));
    GRegex *re = g_regex_new("\\[-?\\d+,-?\\d+,\"([^\"]+)\",-?\\d+,\"([^\"]*)\"\\]", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    g_regex_match(re, sect, 0, &mi);
    while (g_match_info_matches(mi)) {
      char *pf = g_match_info_fetch(mi, 1);
      if (!g_hash_table_contains(pfx, pf)) {
        Dxcc *d = g_new0(Dxcc, 1);
        d->prefix = pf;
        d->name = g_match_info_fetch(mi, 2);
        for (guint i = 0; i < G_N_ELEMENTS(ALSO); i++) {
          if (strcmp(ALSO[i].prefix, pf) == 0) { d->also = g_strdup(ALSO[i].also); }
        }
        g_hash_table_insert(pfx, d->prefix, d);
      } else {
        g_free(pf);
      }
      g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    g_free(sect);
  }
  g_free(js);
}

static void load_aliases(void) {
  char *js = res_text(RES "dxcc_prefix_aliases.js");
  if (!js) { return; }
  GRegex *re = g_regex_new("addAliases\\(aliasMap,\\s*\"([^\"]+)\",\\s*\\[([^\\]]*)\\]\\)", 0, 0, NULL);
  GRegex *q = g_regex_new("\"([^\"]+)\"", 0, 0, NULL);
  GMatchInfo *mi = NULL;
  g_regex_match(re, js, 0, &mi);
  while (g_match_info_matches(mi)) {
    char *canon = g_match_info_fetch(mi, 1), *list = g_match_info_fetch(mi, 2);
    GMatchInfo *mq = NULL;
    g_regex_match(q, list, 0, &mq);
    while (g_match_info_matches(mq)) {
      char *a = g_match_info_fetch(mq, 1);
      char *up = g_ascii_strup(a, -1);
      g_free(a);
      alias_max = MAX(alias_max, (guint)strlen(up));
      g_hash_table_replace(alias, up, g_strdup(canon));   /* a later one wins, as aliasMap[] = */
      g_match_info_next(mq, NULL);
    }
    g_match_info_free(mq);
    g_free(canon);
    g_free(list);
    g_match_info_next(mi, NULL);
  }
  g_match_info_free(mi);
  g_regex_unref(q);
  g_regex_unref(re);
  g_free(js);
}

static gpointer load(gpointer unused) {
  (void)unused;
  pfx = g_hash_table_new(g_str_hash, g_str_equal);
  alias = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  load_labels();
  load_aliases();
  return NULL;
}

static gboolean starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* resolveSpecialCases + resolveAlias: the canonical prefix (newly allocated) */
static char *canonical(const char *s) {
  static const char *const EXACT[] = { "JD1/M", "JD1/O", "EA6", "EA8", "EA9", "CT3", "CU", "IS0", "3B9" };
  if (!*s) { return g_strdup(""); }
  for (guint i = 0; i < G_N_ELEMENTS(EXACT); i++) {
    if (starts(s, EXACT[i])) { return g_strdup(EXACT[i]); }
  }
  if ((s[0] == 'A' || s[0] == 'N' || s[0] == 'W') && s[1] == 'H' && g_ascii_isdigit(s[2])) {
    return g_strdup_printf("KH%c", s[2]);
  }
  if (s[0] == 'L' && s[1] >= '2' && s[1] <= '9') { return g_strdup("LU"); }
  static const char *const UK[][4] = {
    { "GI", "2I", "MI", "GI" }, { "GM", "2M", "MM", "GM" }, { "GW", "2W", "MW", "GW" },
    { "GD", "2D", "MD", "GD" }, { "GJ", "2J", "MJ", "GJ" }, { "GU", "2U", "MU", "GU" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(UK); i++) {
    if (starts(s, UK[i][0]) || starts(s, UK[i][1]) || starts(s, UK[i][2])) { return g_strdup(UK[i][3]); }
  }
  if (s[0] == 'M' || starts(s, "2E") || starts(s, "GB") || starts(s, "GX") || starts(s, "MX")) {
    return g_strdup("G");
  }
  /* Russia: R0…, RA0…, UA0…, UAA0… — the first digit 0, 8, 9 is Asia */
  const gboolean r = s[0] == 'R', u = s[0] == 'U' && s[1] >= 'A' && s[1] <= 'I';
  if ((r && (g_ascii_isdigit(s[1]) || (g_ascii_isupper(s[1]) && g_ascii_isdigit(s[2])))) ||
      (u && (g_ascii_isdigit(s[2]) || (g_ascii_isupper(s[2]) && g_ascii_isdigit(s[3]))))) {
    if (starts(s, "UA2")) { return g_strdup("UA2"); }
    const char *d = s;
    while (*d && !g_ascii_isdigit(*d)) { d++; }
    return g_strdup(*d == '0' || *d == '8' || *d == '9' ? "UA/AS" : "UA/E");
  }
  const guint n = (guint)strlen(s);
  for (guint k = MIN(n, alias_max); k >= 1; k--) {
    char *head = g_strndup(s, k);
    const char *c = g_hash_table_lookup(alias, head);
    g_free(head);
    if (c) { return g_strdup(c); }
  }
  return g_strdup(s);
}

static const Dxcc *by_prefix(const char *cand) {
  const Dxcc *d = g_hash_table_lookup(pfx, cand);
  if (d) { return d; }
  for (guint n = MIN(4, (guint)strlen(cand)); n >= 1; n--) {
    char *head = g_strndup(cand, n);
    d = g_hash_table_lookup(pfx, head);
    g_free(head);
    if (d) { return d; }
  }
  return NULL;
}

static const Dxcc *locate(const char *call) {
  static GOnce once = G_ONCE_INIT;
  g_once(&once, load, NULL);
  if (!g_hash_table_size(pfx)) { return NULL; }
  char *up = g_ascii_strup(call, -1);
  char **parts = g_strsplit(up, "/", -1);
  const char *base = "";
  for (char **p = parts; *p; p++) {
    const char *x = *p;
    if (!*x || strcmp(x, "P") == 0 || strcmp(x, "M") == 0 || strcmp(x, "MM") == 0 ||
        strcmp(x, "AM") == 0 || strcmp(x, "QRP") == 0 || (g_ascii_isdigit(x[0]) && !x[1])) {
      continue;
    }
    if (strlen(x) > strlen(base)) { base = x; }   /* the first of the longest, as a stable sort */
  }
  /* normalize: A-Z, 0-9 and / only */
  GString *b = g_string_new(NULL);
  for (const char *c = base; *c; c++) {
    if (g_ascii_isupper(*c) || g_ascii_isdigit(*c) || *c == '/') { g_string_append_c(b, *c); }
  }
  char *canon = canonical(b->str);
  const Dxcc *d = *canon ? by_prefix(canon) : NULL;
  if (!d && *base) { d = by_prefix(base); }
  g_free(canon);
  g_string_free(b, TRUE);
  g_strfreev(parts);
  g_free(up);
  return d;
}

gboolean skim_hd_dxcc_find(const char *call, const char **prefix, const char **name) {
  const Dxcc *d = call ? locate(call) : NULL;
  if (prefix) { *prefix = d ? d->prefix : NULL; }
  if (name) { *name = d ? d->name : NULL; }
  return d != NULL;
}

static gboolean has_caseless(const char *hay, const char *needle_upper) {
  if (!hay) { return FALSE; }
  char *up = g_ascii_strup(hay, -1);
  const gboolean ok = strstr(up, needle_upper) != NULL;
  g_free(up);
  return ok;
}

gboolean skim_hd_dxcc_name_has(const char *call, const char *text_upper) {
  const Dxcc *d = call ? locate(call) : NULL;
  return d && (has_caseless(d->name, text_upper) || has_caseless(d->also, text_upper));
}
