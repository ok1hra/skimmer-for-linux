/* helper_run.h — run a decoder helper (skimmer-ft8d, skimmer-wsprd) and
 * read what it prints.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* argv[0] in cwd at nice 10; *out = its stdout once it exited 0. A cancel
 * kills it (G_IO_ERROR_CANCELLED); a failed exit is G_IO_ERROR_FAILED with
 * its stderr, named `name`. */
gboolean skim_hd_helper_run(const char *const *argv, const char *cwd, const char *name,
                            GCancellable *cancel, char **out, GError **error);

/* A helper kept running between jobs (skimmer-js8d --serve: its decoder
 * state carries from one period to the next). Started on the first ask at
 * nice 10 in cwd, dies with the skimmer; ask writes one line to its stdin
 * and returns (in *out) the lines it prints up to a line equal to `end`,
 * on the calling thread. A helper that died (EOF) is gone, the next ask
 * starts it again. A cancel kills it: the ask in progress fails with
 * G_IO_ERROR_CANCELLED. One ask at a time — the caller serialises. */
typedef struct _SkimHdHelperServer SkimHdHelperServer;
SkimHdHelperServer *skim_hd_helper_server_new(const char *const *argv, const char *cwd,
                                              const char *name, GCancellable *cancel);
gboolean            skim_hd_helper_server_ask(SkimHdHelperServer *s, const char *line,
                                              const char *end, char **out, GError **error);
void                skim_hd_helper_server_free(SkimHdHelperServer *s);
/* The running helper's CPU (utime + stime, clock ticks) from /proc, 0 when
 * none runs; any thread — a kept helper's CPU reaches the children's
 * times only once it ends. */
guint64             skim_hd_helper_server_ticks(SkimHdHelperServer *s);

G_END_DECLS
