/* logring.h — the last log lines, for the web page (/api/log).
 *
 * A GLib log writer that keeps the newest LOG_RING_LINES messages (time,
 * level, text) and passes every message on to the default writer, so the
 * console and the journal see exactly what they saw before.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_LOG_RING_LINES 200

/* Install the writer (once, at start). */
void skim_log_ring_install(void);

/* The lines as a JSON array body: [{"t":"HH:MM:SS","level":"message","text":"…"},…],
 * oldest first. */
char *skim_log_ring_json(void);

G_END_DECLS
