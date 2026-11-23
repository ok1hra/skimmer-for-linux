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

G_END_DECLS
