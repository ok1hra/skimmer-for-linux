/* switches.h — the SKIM_* environment switches a start runs with.
 *
 * The engine reads its experimental and debug switches straight from the
 * environment (getenv), and they stay that way: a switch is a start-time
 * choice, like the command line. What this module adds is visibility — the
 * switches set at start are logged and go into /status.json ("switches"),
 * so a measurement can tell a period run with SKIM_V2_EDGE=1 from one
 * without, and a SKIM_* name the engine does not know (a typo, a switch
 * since removed) is warned about instead of silently doing nothing.
 *
 * The table of known switches must follow the engine: the switches gate
 * compares it with every SKIM_ name the sources read.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKIM_SWITCH_BEHAVIOUR,     /* changes what is decoded or spotted          */
  SKIM_SWITCH_SYSTEM,        /* where things are: libraries, models, URLs   */
  SKIM_SWITCH_DEBUG,         /* traces and dumps, output only               */
} SkimSwitchKind;

/* NULL for a name the engine does not read. */
const char *skim_switch_kind_name(const char *name);

/* Every known switch, one per line ("NAME kind"), for the gate. */
void skim_switches_print_known(void);

/* Log the SKIM_* variables set now (unknown ones as a warning) and return
 * them as a JSON object body: {"SKIM_V2_EDGE":"1",…} ({} when none). */
char *skim_switches_scan(void);

G_END_DECLS
