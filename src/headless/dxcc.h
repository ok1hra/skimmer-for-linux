/* dxcc.h — a call's DXCC, as the web page's map finds it.
 *
 * The data is the map's own (web/map/, built into skimmer-headless as the
 * web GResource): DXCC_PREFIX_LABELS of map50.js (prefix → name) and
 * IP-rotator's prefix aliases (dxcc_prefix_aliases.js) — read once, on the
 * first lookup, so the server and the page name the same country for a call
 * (SkMap.locate in web/map.js is the same search). A binary without the web
 * resource knows no DXCC (every lookup is NULL).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The DXCC of a call: its label's prefix ("UA/E") and name ("European
 * Russia"), or FALSE when the call's prefix is not known. Thread-safe. */
gboolean skim_hd_dxcc_find(const char *call, const char **prefix, const char **name);

/* Does the DXCC's name — or one of its other names (Belarus: Belorussia;
 * Kaliningrad and Franz Josef Land: Russia) — contain the text, any case? */
gboolean skim_hd_dxcc_name_has(const char *call, const char *text_upper);

G_END_DECLS
