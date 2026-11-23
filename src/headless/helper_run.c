/* helper_run.c — a decoder helper run to its end, killable by a cancel.
 *
 * The cancellable is never handed to g_subprocess_communicate(): GLib (2.74,
 * the AppImage's, and 2.84 alike) corrupts its heap or deadlocks when two
 * threads run communicate with a GCancellable at once — FT8's two decoder
 * threads and WSPR's side by side did. Live: hra2 2026-11-22 23:41Z, 0.8.10
 * aborted after 3 h ("g_mutex_clear() called on uninitialised or locked
 * mutex"); a 4-thread loop of /bin/echo fails 5–8 runs of 10 with one, 0 of
 * 30 without. So a cancel force-exits the child instead, and communicate
 * returns as its pipes close.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "helper_run.h"

#include <unistd.h>

static void lower_priority(gpointer unused) {
  (void)unused;
  if (nice(10) == -1) { /* as it is */ }
}

static void kill_helper(GCancellable *c, gpointer p) {
  (void)c;
  g_subprocess_force_exit(p);
}

gboolean skim_hd_helper_run(const char *const *argv, const char *cwd, const char *name,
                            GCancellable *cancel, char **out, GError **error) {
  *out = NULL;
  if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) { return FALSE; }
  GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_set_cwd(l, cwd);
  g_subprocess_launcher_set_child_setup(l, lower_priority, NULL, NULL);
  GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, error);
  g_object_unref(l);
  if (!p) { return FALSE; }
  /* already cancelled: the handler runs at once */
  const gulong hid = cancel ? g_cancellable_connect(cancel, G_CALLBACK(kill_helper), p, NULL) : 0;
  char *o = NULL, *e = NULL;
  gboolean ok = g_subprocess_communicate_utf8(p, NULL, NULL, &o, &e, error);
  if (hid) { g_cancellable_disconnect(cancel, hid); }
  if (!ok) {
    g_subprocess_force_exit(p);
  } else if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) {
    ok = FALSE;
  } else if (!g_subprocess_get_if_exited(p) || g_subprocess_get_exit_status(p) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed%s%s", name,
                e && *e ? ": " : "", e ? g_strstrip(e) : "");
    ok = FALSE;
  }
  if (ok) {
    *out = o;
    o = NULL;
  }
  g_free(o);
  g_free(e);
  g_object_unref(p);
  return ok;
}
