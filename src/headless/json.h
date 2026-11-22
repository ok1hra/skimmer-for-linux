/* json.h — the one JSON string escape skimmer-headless's bodies use, and a
 * small bounded reader for the JSON it fetches (no JSON library).
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

/* --- reading: no JSON library ---------------------------------------------------
 * Tolerant of order, whitespace and unknown keys; anything it cannot follow
 * (an error page, a truncated body, nesting past the limit) is a FALSE, never
 * a crash: every read is bounded by `end`. */

typedef struct { const char *p, *end; } SkimJsonR;

static inline void skim_json_ws(SkimJsonR *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' ||
                           *j->p == '\r')) {
    j->p++;
  }
}

/* A string value → out (truncated to fit; escapes flattened — nothing we
 * compare holds one). */
static inline gboolean skim_json_string(SkimJsonR *j, char *out, gsize outsz) {
  if (j->p >= j->end || *j->p != '"') { return FALSE; }
  j->p++;
  gsize n = 0;
  while (j->p < j->end && *j->p != '"') {
    char c = *j->p++;
    if (c == '\\') {
      if (j->p >= j->end) { return FALSE; }
      char e = *j->p++;
      if (e == 'u') {
        if (j->end - j->p < 4) { return FALSE; }
        j->p += 4;
        c = '?';
      } else {
        c = (e == 'n' || e == 'r' || e == 't' || e == 'b' || e == 'f') ? ' ' : e;
      }
    }
    if (out && n + 1 < outsz) { out[n++] = c; }
  }
  if (j->p >= j->end) { return FALSE; }        /* no closing quote           */
  j->p++;
  if (out && outsz) { out[n] = '\0'; }
  return TRUE;
}

static inline gboolean skim_json_skip(SkimJsonR *j, int depth) {
  skim_json_ws(j);
  if (j->p >= j->end || depth > 16) { return FALSE; }
  char c = *j->p;
  if (c == '"') { return skim_json_string(j, NULL, 0); }
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    j->p++;
    skim_json_ws(j);
    if (j->p < j->end && *j->p == close) { j->p++; return TRUE; }
    for (;;) {
      if (c == '{') {
        skim_json_ws(j);
        if (!skim_json_string(j, NULL, 0)) { return FALSE; }
        skim_json_ws(j);
        if (j->p >= j->end || *j->p++ != ':') { return FALSE; }
      }
      if (!skim_json_skip(j, depth + 1)) { return FALSE; }
      skim_json_ws(j);
      if (j->p >= j->end) { return FALSE; }
      if (*j->p == ',') { j->p++; continue; }
      if (*j->p == close) { j->p++; return TRUE; }
      return FALSE;
    }
  }
  const char *start = j->p;                    /* number, true, false, null  */
  while (j->p < j->end && (g_ascii_isalnum(*j->p) || *j->p == '-' ||
                           *j->p == '+' || *j->p == '.')) {
    j->p++;
  }
  return j->p > start;
}
