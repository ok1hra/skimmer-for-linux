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
 * A helper dies with the process that started it (PR_SET_PDEATHSIG): a
 * stop under systemd signals the skimmer alone (service.c).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "helper_run.h"

#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

/* nice 10, and the helper dies with the skimmer: the service unit sends
 * its SIGTERM to the skimmer alone (KillMode=process, service.c), so a
 * helper running then is not killed by systemd */
static void child_setup(gpointer unused) {
  (void)unused;
  if (nice(10) == -1) { /* as it is */ }
  prctl(PR_SET_PDEATHSIG, SIGKILL);
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
  g_subprocess_launcher_set_child_setup(l, child_setup, NULL, NULL);
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

/* ---- a helper kept running ------------------------------------------------ */

struct _SkimHdHelperServer {
  char        **argv;
  char         *cwd, *name;
  GCancellable *cancel;
  GSubprocess  *p;
  GOutputStream *in;
  GDataInputStream *out;
  gulong        hid;
};

static void server_stop(SkimHdHelperServer *s) {
  if (!s->p) { return; }
  if (s->hid) { g_cancellable_disconnect(s->cancel, s->hid); s->hid = 0; }
  g_subprocess_force_exit(s->p);
  g_clear_object(&s->out);
  s->in = NULL;                                  /* owned by the subprocess */
  g_clear_object(&s->p);
}

SkimHdHelperServer *skim_hd_helper_server_new(const char *const *argv, const char *cwd,
                                              const char *name, GCancellable *cancel) {
  SkimHdHelperServer *s = g_new0(SkimHdHelperServer, 1);
  s->argv = g_strdupv((char **)argv);
  s->cwd = g_strdup(cwd);
  s->name = g_strdup(name);
  s->cancel = cancel ? g_object_ref(cancel) : NULL;
  return s;
}

static gboolean server_start(SkimHdHelperServer *s, GError **error) {
  /* a helper that died while we write to it must not take us along */
  signal(SIGPIPE, SIG_IGN);
  GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                     G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  g_subprocess_launcher_set_cwd(l, s->cwd);
  g_subprocess_launcher_set_child_setup(l, child_setup, NULL, NULL);
  s->p = g_subprocess_launcher_spawnv(l, (const char *const *)s->argv, error);
  g_object_unref(l);
  if (!s->p) { return FALSE; }
  s->in = g_subprocess_get_stdin_pipe(s->p);
  s->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(s->p));
  g_data_input_stream_set_newline_type(s->out, G_DATA_STREAM_NEWLINE_TYPE_LF);
  if (s->cancel) { s->hid = g_cancellable_connect(s->cancel, G_CALLBACK(kill_helper), s->p, NULL); }
  return TRUE;
}

gboolean skim_hd_helper_server_ask(SkimHdHelperServer *s, const char *line, const char *end,
                                   char **out, GError **error) {
  *out = NULL;
  if (s->cancel && g_cancellable_set_error_if_cancelled(s->cancel, error)) { return FALSE; }
  if (!s->p && !server_start(s, error)) { return FALSE; }
  char *req = g_strconcat(line, "\n", NULL);
  const gboolean wrote = g_output_stream_write_all(s->in, req, strlen(req), NULL, NULL, error) &&
                         g_output_stream_flush(s->in, NULL, error);
  g_free(req);
  GString *got = g_string_new(NULL);
  gboolean ok = wrote;
  while (ok) {
    gsize n = 0;
    GError *e = NULL;
    char *ln = g_data_input_stream_read_line_utf8(s->out, &n, NULL, &e);
    if (!ln) {
      if (e) {
        g_propagate_error(error, e);
      } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s ended", s->name);
      }
      ok = FALSE;
      break;
    }
    const gboolean done = strcmp(ln, end) == 0;
    if (!done) { g_string_append(got, ln); g_string_append_c(got, '\n'); }
    g_free(ln);
    if (done) { break; }
  }
  if (!ok) {
    server_stop(s);                               /* the next ask starts it again */
    if (s->cancel && g_cancellable_is_cancelled(s->cancel)) {
      g_clear_error(error);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    }
    g_string_free(got, TRUE);
    return FALSE;
  }
  *out = g_string_free(got, FALSE);
  return TRUE;
}

void skim_hd_helper_server_free(SkimHdHelperServer *s) {
  if (!s) { return; }
  server_stop(s);
  g_strfreev(s->argv);
  g_free(s->cwd);
  g_free(s->name);
  g_clear_object(&s->cancel);
  g_free(s);
}
