/* service.h — skimmer-headless as a systemd user service.
 *
 *   --install-service    writes ~/.config/systemd/user/skimmer-headless.service
 *                        (ExecStart = this AppImage, or this binary) and an
 *                        empty ~/.config/skimmer-for-linux/headless.env, then
 *                        `systemctl --user daemon-reload`; it starts nothing
 *   --uninstall-service  `systemctl --user disable --now`, removes the unit,
 *                        daemon-reload; headless.env and the config stay
 *
 * The unit's restart policy follows the exit codes (headless_main.c):
 *   0  stopped (SIGTERM)          — stays stopped
 *   2  the config is wrong        — stays stopped (RestartPreventExitStatus)
 *   3  no free radio / stream lost — again after 60 s (Restart=on-failure)
 *   75 a supervisor restart        — again (RestartForceExitStatus); only
 *                                    with SKIM_SUPERVISED=1, which the unit
 *                                    does NOT set: a restart from the web page
 *                                    re-executes in place, at once, instead
 *                                    of waiting out RestartSec
 * A crash restarts after 60 s too. Lingering (running while logged out) is
 * the user's call: the install prints the command, it never runs it.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HD_SERVICE "skimmer-headless"

/* The unit text for exe (absolute) with config (NULL = the default). */
char *skim_hd_service_unit(const char *exe, const char *config);

/* The program to put in ExecStart: $APPIMAGE when running from one, else
 * this binary. Caller frees. */
char *skim_hd_service_exe(void);

/* Print what was done and what to run next on stdout; FALSE + error when
 * a file could not be written / removed. A missing or failing systemctl
 * is reported, not an error. */
gboolean skim_hd_service_install(const char *exe, const char *config, GError **error);
gboolean skim_hd_service_uninstall(GError **error);

/* Running as a systemd service (INVOCATION_ID set by systemd). */
gboolean skim_hd_service_under_systemd(void);

G_END_DECLS
