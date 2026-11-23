/* helper_run_test.c — gate helper-run: the decoder helpers' runner.
 *   - four threads run helpers side by side with one shared cancellable
 *     (FT8's two decoder threads and WSPR's: GLib's own cancellable path
 *     corrupted the heap there — helper_run.c), every output read;
 *   - a failed exit names the helper and its stderr;
 *   - a cancel kills a running helper at once (G_IO_ERROR_CANCELLED);
 *   - a helper dies with the process that started it (the service unit
 *     signals the skimmer alone).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "headless/helper_run.h"

static int fails;
#define CHECK(c, ...) do { printf("  %s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); \
                           printf("\n"); if (!(c)) { fails++; } } while (0)

static GCancellable *shared;
static gint64 until;
static volatile gint runs, bad;

static gpointer worker(gpointer u) {
  (void)u;
  const char *argv[] = { "/bin/sh", "-c", "echo hi", NULL };
  while (g_get_monotonic_time() < until) {
    char *out = NULL;
    GError *e = NULL;
    if (!skim_hd_helper_run(argv, "/", "sh", shared, &out, &e) || g_strcmp0(out, "hi\n") != 0) {
      g_atomic_int_inc(&bad);
    }
    g_clear_error(&e);
    g_free(out);
    g_atomic_int_inc(&runs);
  }
  return NULL;
}

static gboolean quit_later(gpointer loop) {
  g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

static gboolean cancel_later(gpointer c) {
  g_cancellable_cancel(c);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GCancellable *c;
  gboolean ok;
  GError *e;
  gint64 took_us;
} Long;

static gpointer long_run(gpointer u) {
  Long *l = u;
  const char *argv[] = { "/bin/sleep", "20", NULL };   /* as the helpers: no shell */
  char *out = NULL;
  const gint64 t0 = g_get_monotonic_time();
  l->ok = skim_hd_helper_run(argv, "/", "sleep", l->c, &out, &l->e);
  l->took_us = g_get_monotonic_time() - t0;
  g_free(out);
  return NULL;
}

/* alive and not a zombie */
static gboolean running(int pid) {
  char *path = g_strdup_printf("/proc/%d/stat", pid), *st = NULL;
  gboolean on = g_file_get_contents(path, &st, NULL, NULL);
  const char *paren = on ? strrchr(st, ')') : NULL;
  on = paren && paren[1] == ' ' && paren[2] != 'Z' && paren[2] != 'X';
  g_free(st);
  g_free(path);
  return on;
}

/* A child process starts a helper and is killed: the helper goes too.
 * Before any thread of this process starts (the fork). */
static void dies_with_parent(void) {
  char *dir = g_dir_make_tmp("helper-run-XXXXXX", NULL);
  char *pidf = g_build_filename(dir, "pid", NULL);
  char *cmd = g_strdup_printf("echo $$ > %s.t && mv %s.t %s && exec /bin/sleep 20", pidf, pidf, pidf);
  const pid_t child = fork();
  if (child == 0) {
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    char *out = NULL;
    skim_hd_helper_run(argv, "/", "sleep", NULL, &out, NULL);
    _exit(0);
  }
  char *txt = NULL;
  for (int i = 0; i < 300 && !g_file_get_contents(pidf, &txt, NULL, NULL); i++) { g_usleep(10000); }
  const int helper = txt ? atoi(txt) : 0;
  const gboolean was = helper > 0 && running(helper);
  kill(child, SIGKILL);
  waitpid(child, NULL, 0);
  gboolean gone = FALSE;
  for (int i = 0; i < 200 && !gone; i++) {
    gone = !running(helper);
    if (!gone) { g_usleep(10000); }
  }
  if (!gone && helper > 0) { kill(helper, SIGKILL); }
  CHECK(was && gone, "a helper dies with the process that started it (pid %d: %s)", helper,
        !was ? "never ran" : gone ? "gone" : "still running");
  g_unlink(pidf);
  g_rmdir(dir);
  g_free(txt);
  g_free(cmd);
  g_free(pidf);
  g_free(dir);
}

int main(void) {
  printf("=== skimmer-helper-run-test ===\n");
  dies_with_parent();
  shared = g_cancellable_new();
  until = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
  GThread *t[4];
  for (int i = 0; i < 4; i++) { t[i] = g_thread_new("w", worker, NULL); }
  for (int i = 0; i < 4; i++) { g_thread_join(t[i]); }
  CHECK(runs > 40 && bad == 0, "4 threads side by side, one cancellable: %d runs, %d wrong", runs, bad);

  const char *fail_argv[] = { "/bin/sh", "-c", "echo broken >&2; exit 3", NULL };
  char *out = NULL;
  GError *e = NULL;
  gboolean ok = skim_hd_helper_run(fail_argv, "/", "skimmer-x", shared, &out, &e);
  CHECK(!ok && !out && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_FAILED) &&
        strcmp(e->message, "skimmer-x failed: broken") == 0,
        "a failed exit: \"%s\"", e ? e->message : "ok!");
  g_clear_error(&e);

  Long l = { .c = g_cancellable_new() };
  GThread *lt = g_thread_new("long", long_run, &l);
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(300, cancel_later, l.c);
  g_timeout_add(600, quit_later, loop);
  g_main_loop_run(loop);
  g_thread_join(lt);
  CHECK(!l.ok && g_error_matches(l.e, G_IO_ERROR, G_IO_ERROR_CANCELLED) && l.took_us < 3 * G_USEC_PER_SEC,
        "a cancel kills a running helper (%.1f s, %s)", l.took_us / 1e6, l.e ? l.e->message : "ok!");
  g_clear_error(&l.e);
  ok = skim_hd_helper_run(fail_argv, "/", "skimmer-x", l.c, &out, &e);
  CHECK(!ok && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED), "already cancelled: not started");
  g_clear_error(&e);
  g_object_unref(l.c);
  g_main_loop_unref(loop);
  g_object_unref(shared);
  printf("\n%s (%d failure%s)\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
