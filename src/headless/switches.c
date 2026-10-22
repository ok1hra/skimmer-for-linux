/* switches.c — see switches.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "switches.h"

#include "json.h"

#include <stdio.h>
#include <string.h>

#define B SKIM_SWITCH_BEHAVIOUR
#define S SKIM_SWITCH_SYSTEM
#define D SKIM_SWITCH_DEBUG

static const struct { const char *name; SkimSwitchKind kind; } KNOWN[] = {
  /* callsign extraction */
  { "SKIM_CALL_DESPACE", B }, { "SKIM_CALL_EXCH", B }, { "SKIM_CALL_FUSED", B },
  { "SKIM_CALL_GLUE", B }, { "SKIM_CALL_TU", B }, { "SKIM_CALL_TU_STRICT", B },
  { "SKIM_DESPACE_DEBUG", D },
  /* CW engines */
  { "SKIM_CW_ENGINE", B }, { "SKIM_CW_V1", B }, { "SKIM_CW_READER", B },
  { "SKIM_CW_READER_RAW", B }, { "SKIM_CW_DEBUG", D }, { "SKIM_CW_DUMP_RUNS", D },
  { "SKIM_CW2_DEBUG", D }, { "SKIM_CW2_TRACE", D }, { "SKIM_RR_DBG", D },
  { "SKIM_V2_DAHMAX", B }, { "SKIM_V2_EDGE", B }, { "SKIM_V2_EDGE_WEAK", B },
  { "SKIM_V2_NARROW", B }, { "SKIM_V2_NEAREST", B }, { "SKIM_V2_RAYCAP", B },
  { "SKIM_V2_RAYFADE", B }, { "SKIM_V2_RAYSMEAR", B }, { "SKIM_V2_RELOCK_DB", B },
  { "SKIM_V2_RESCORE", B }, { "SKIM_V2_RESCORE_DEBUG", D },
  /* DeepCW */
  { "SKIM_DEEPCW_BATCH", B }, { "SKIM_DEEPCW_DRAFT", B }, { "SKIM_DEEPCW_DRAFT_MIN", B },
  { "SKIM_DEEPCW_DRAFT_P", B }, { "SKIM_DEEPCW_FLUSH", B }, { "SKIM_DEEPCW_FLUSH_K", B },
  { "SKIM_DEEPCW_FLUSH_P", B }, { "SKIM_DEEPCW_FLUSH_PAD", B },
  { "SKIM_DEEPCW_FLUSH_QUIET", B }, { "SKIM_DEEPCW_GAP_WORDS", B },
  { "SKIM_DEEPCW_GATE_DB", B }, { "SKIM_DEEPCW_MINCONF", B }, { "SKIM_DEEPCW_ONLY", B },
  { "SKIM_DEEPCW_SPACE_P", B }, { "SKIM_DEEPCW_SYNC", B }, { "SKIM_DEEPCW_TAIL", B },
  { "SKIM_DEEPCW_TICK", B }, { "SKIM_DEEPCW_WIN", B },
  { "SKIM_DEEPCW_DEVICE", S }, { "SKIM_DEEPCW_MODEL", S }, { "SKIM_DEEPCW_THREADS", S },
  { "SKIM_DEEPCW_WORKERS", S }, { "SKIM_ORT_LIB", S },
  { "SKIM_DEEPCW_DEBUG", D }, { "SKIM_DEEPCW_TAILDUMP", D },
  /* RTTY, channels, the tone splitter */
  { "SKIM_TONE_FOCUS", B }, { "SKIM_TONE_SPLIT", B }, { "SKIM_TS_RATIO", B },
  { "SKIM_TS_DEBUG", D }, { "SKIM_RTTY_DEBUG", D }, { "SKIM_FLOCK_DEBUG", D },
  { "SKIM_ST_DEBUG", D }, { "SKIM_DLOG_LANE", D }, { "SKIM_DUMP_DIR", D },
  { "SKIM_DUMP_CHAN", D },
  /* the RBN feed */
  { "SKIM_FEED_CONFIRM", B }, { "SKIM_GATE_DICT_MIN", B }, { "SKIM_GATE_NEAR", B },
  { "SKIM_GATE_NEAR_DICT", B }, { "SKIM_GATE_NEAR_TTL", B },
  { "SKIM_GATE_SETTLE_NODICT", B }, { "SKIM_FEED_TRACE", D }, { "SKIM_GATE_NEAR_DEBUG", D },
  /* MASTER.SCP */
  { "SKIM_SCP_URL", S },
};

#undef B
#undef S
#undef D

static const char *KIND[] = { "behaviour", "system", "debug" };

const char *skim_switch_kind_name(const char *name) {
  for (guint i = 0; i < G_N_ELEMENTS(KNOWN); i++) {
    if (strcmp(KNOWN[i].name, name) == 0) { return KIND[KNOWN[i].kind]; }
  }
  return NULL;
}

void skim_switches_print_known(void) {
  for (guint i = 0; i < G_N_ELEMENTS(KNOWN); i++) {
    printf("%s %s\n", KNOWN[i].name, KIND[KNOWN[i].kind]);
  }
}

static gint by_name(gconstpointer a, gconstpointer b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* GLib 2.74 is the floor (Debian 12, the AppImage's base): no
 * g_ptr_array_sort_values. */
char *skim_switches_scan(void) {
  char **env = g_get_environ();
  GPtrArray *names = g_ptr_array_new();
  for (char **e = env; *e; e++) {
    if (g_str_has_prefix(*e, "SKIM_")) { g_ptr_array_add(names, *e); }
  }
  g_ptr_array_sort(names, by_name);
  GString *js = g_string_new("{");
  for (guint i = 0; i < names->len; i++) {
    const char *kv = names->pdata[i];
    const char *eq = strchr(kv, '=');
    char *name = g_strndup(kv, eq ? (gsize)(eq - kv) : strlen(kv));
    const char *val = eq ? eq + 1 : "";
    const char *kind = skim_switch_kind_name(name);
    if (kind) {
      g_message("switch: %s=%s (%s)", name, val, kind);
    } else {
      g_warning("switch: %s=%s is not a switch the engine reads — a typo? "
                "It changes nothing", name, val);
    }
    char *jn = skim_json_str(name), *jv = skim_json_str(val);
    g_string_append_printf(js, "%s\"%s\":\"%s\"", i ? "," : "", jn, jv);
    g_free(jn);
    g_free(jv);
    g_free(name);
  }
  g_string_append_c(js, '}');
  g_ptr_array_free(names, TRUE);
  g_strfreev(env);
  return g_string_free(js, FALSE);
}
