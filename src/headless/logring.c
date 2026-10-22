/* logring.c — see logring.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "logring.h"

#include "json.h"

#include <string.h>

typedef struct {
  gint64 t_us;
  char   level[8];
  char  *text;
} Line;

static GMutex lock;
static Line   ring[SKIM_LOG_RING_LINES];
static guint  head, count;

static const char *level_name(GLogLevelFlags l) {
  if (l & G_LOG_LEVEL_ERROR) { return "error"; }
  if (l & G_LOG_LEVEL_CRITICAL) { return "crit"; }
  if (l & G_LOG_LEVEL_WARNING) { return "warn"; }
  if (l & G_LOG_LEVEL_MESSAGE) { return "message"; }
  if (l & G_LOG_LEVEL_INFO) { return "info"; }
  return "debug";
}

static GLogWriterOutput writer(GLogLevelFlags level, const GLogField *fields, gsize n,
                               gpointer user) {
  (void)user;
  if (!(level & G_LOG_LEVEL_DEBUG)) {           /* debug stays on the console */
    const char *msg = NULL;
    for (gsize i = 0; i < n; i++) {
      if (strcmp(fields[i].key, "MESSAGE") == 0) { msg = fields[i].value; break; }
    }
    if (msg) {
      g_mutex_lock(&lock);
      Line *l = &ring[head];
      g_free(l->text);
      l->text = g_strdup(msg);
      l->t_us = g_get_real_time();
      g_strlcpy(l->level, level_name(level), sizeof(l->level));
      head = (head + 1) % SKIM_LOG_RING_LINES;
      if (count < SKIM_LOG_RING_LINES) { count++; }
      g_mutex_unlock(&lock);
    }
  }
  return g_log_writer_default(level, fields, n, user);
}

void skim_log_ring_install(void) { g_log_set_writer_func(writer, NULL, NULL); }

char *skim_log_ring_json(void) {
  GString *o = g_string_new("[");
  g_mutex_lock(&lock);
  for (guint k = 0; k < count; k++) {
    const Line *l = &ring[(head + SKIM_LOG_RING_LINES - count + k) % SKIM_LOG_RING_LINES];
    GDateTime *t = g_date_time_new_from_unix_utc(l->t_us / G_USEC_PER_SEC);
    char *ts = g_date_time_format(t, "%H:%M:%S");
    g_date_time_unref(t);
    g_string_append_printf(o, "%s{\"t\":\"%sZ\",\"level\":\"%s\",\"text\":\"", k ? "," : "",
                           ts, l->level);
    skim_json_append(o, l->text);
    g_string_append(o, "\"}");
    g_free(ts);
  }
  g_mutex_unlock(&lock);
  g_string_append_c(o, ']');
  return g_string_free(o, FALSE);
}
