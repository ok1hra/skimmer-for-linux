/* json.h — the one JSON string escape skimmer-headless's bodies use.
 *
 * Quote, backslash and control characters escaped, UTF-8 passed through
 * (g_strescape octal-escapes it — not JSON).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

static inline void skim_json_append(GString *o, const char *in) {
  for (const guchar *p = (const guchar *)(in ? in : ""); *p; p++) {
    if (*p == '"' || *p == '\\') { g_string_append_c(o, '\\'); g_string_append_c(o, (char)*p); }
    else if (*p < 0x20) { g_string_append_printf(o, "\\u%04x", *p); }
    else { g_string_append_c(o, (char)*p); }
  }
}

/* A newly allocated escaped copy (no quotes around it). */
static inline char *skim_json_str(const char *in) {
  GString *o = g_string_new(NULL);
  skim_json_append(o, in);
  return g_string_free(o, FALSE);
}
