/* deepcw_model.h — the DeepCW model on disk: is it the pinned one, and
 * fetching it on request (--fetch-deepcw-model, the web page's button).
 *
 * The model (model.onnx, by e04, AGPL-3.0-only) is not part of this
 * program and is not shipped with it: it comes from its own repository at
 * ONE fixed commit, every file checked against its sha256 here, into the
 * place the engine looks (skim_decode_deepcw_model_path(): $SKIM_DEEPCW_MODEL,
 * else ~/.local/share/skimmer-for-linux/models/deepcw/model.onnx), with
 * the metadata, the licence and a NOTICE saying where it came from beside
 * it. A download is held in memory until every file passed its check, and
 * each file lands by an atomic rename — a failed or cancelled fetch leaves
 * what was there untouched.
 *
 * GLib + libcurl; blocking — run it on a worker thread where a main loop
 * must not wait.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SKIM_DEEPCW_UPSTREAM  "https://raw.githubusercontent.com/e04/deepcw-engine"
#define SKIM_DEEPCW_COMMIT    "8e264d243bbd4467bd19f3f28292219405b47e0e"
#define SKIM_DEEPCW_MODEL_SHA "ef120799457bca042d4690944f0faf93268eb4654e7f50f28784ad63bdc1fe02"

typedef void (*SkimDeepcwProgress)(guint64 got, guint64 total, gpointer user);

typedef struct {
  const char        *model_path;  /* where model.onnx goes (the engine's path) */
  const char        *base_url;    /* NULL = SKIM_DEEPCW_UPSTREAM; files are at
                                   * <base>/<commit>/<name> (http:// and
                                   * file:// only when the base says so — a
                                   * gate's mirror)                           */
  const char        *ca_path;     /* NULL = libcurl's trust store              */
  GCancellable      *cancel;
  SkimDeepcwProgress progress;    /* NULL = quiet; called from the fetching
                                   * thread                                   */
  gpointer           user;
} SkimDeepcwFetch;

/* The base URL the environment asks for ($SKIM_DEEPCW_URL), NULL = the
 * upstream. Read it on the main thread. Caller frees. */
char *skim_deepcw_env_base_url(void);

/* The model at model_path is the pinned one (its sha256). FALSE + a reason
 * a person can act on: missing, or a different file. */
gboolean skim_deepcw_model_check(const char *model_path, GError **error);

/* Fetch, check and install the pinned model and its companions. */
gboolean skim_deepcw_model_fetch(const SkimDeepcwFetch *f, GError **error);

G_END_DECLS
