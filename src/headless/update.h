/*
 * update — skimmer-headless keeps itself current from its GitHub releases
 * ([update], Settings → Updates).
 *
 * Once a day (the time of the last check is persisted: a restart sends no
 * extra request) it asks the GitHub API for the latest release of
 * SKIM_HD_UPDATE_REPO — drafts and prereleases are never "latest". A newer
 * version than the one running is, in mode auto:
 *
 *   1. downloaded beside the running AppImage ($APPIMAGE), streamed to disk
 *      and checked against the sha256 GitHub publishes for the asset (a
 *      release without that digest is refused), then
 *   2. run once as a preflight — `--version` must name the release and
 *      `--check-config --config <this config>` must pass — and only then
 *   3. swapped in for the running file at [update] hour_utc (Install now: at
 *      once): the old file stays as <file>.prev, the new one is renamed over
 *      the running path (never a moment without a file), and the skimmer
 *      restarts into it.
 *
 * Every failure before the swap leaves the running version and its file as
 * they were. After it, a version that crashes SKIM_HD_UPDATE_CRASHES times
 * before it ran SKIM_HD_UPDATE_ACCEPT_S (only a supervisor or the systemd
 * unit starts it again after a crash) is swapped back out and not tried again
 * — the next release is.
 *
 * Only a release build running from an AppImage in a writable directory is
 * updated; a development build (SKIMMER_BUILD "-g…") or a binary from a
 * build tree checks and reports, it never replaces anything.
 *
 * GLib + libcurl, no web or engine code: the `update` gate runs it against a
 * file:// mirror (SKIM_UPDATE_URL) with a fake AppImage.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>
#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HD_UPDATE_REPO        "ok1hra/skimmer-for-linux"
#define SKIM_HD_UPDATE_API         "https://api.github.com"
#define SKIM_HD_UPDATE_SPACING_S   (24 * 3600)   /* between completed checks  */
#define SKIM_HD_UPDATE_RETRY_S     3600          /* after a failed one …      */
#define SKIM_HD_UPDATE_RETRIES     4             /* … this many, then a day   */
#define SKIM_HD_UPDATE_API_MAX     (1u << 20)    /* the API answer (~10 kB)   */
#define SKIM_HD_UPDATE_MAX_BYTES   (100u << 20)  /* an AppImage (~19 MB)      */
#define SKIM_HD_UPDATE_PREFLIGHT_S 60            /* each preflight run        */
#define SKIM_HD_UPDATE_CRASHES     3             /* crashes → back to .prev   */
#define SKIM_HD_UPDATE_ACCEPT_S    (15 * 60)     /* up this long = accepted   */

typedef enum {
  SKIM_HD_UPDATE_OFF,      /* never asks GitHub on its own                      */
  SKIM_HD_UPDATE_NOTIFY,   /* asks once a day, shows a newer version            */
  SKIM_HD_UPDATE_AUTO,     /* … and installs it at hour_utc                     */
} SkimHdUpdateMode;

const char *skim_hd_update_mode_name(SkimHdUpdateMode m);

/* Versions are dotted numbers ("0.8.10"); a suffix after the numbers is
 * ignored. <0 / 0 / >0 like strcmp; a string without a leading number sorts
 * below every version. */
int      skim_hd_update_vercmp(const char *a, const char *b);
gboolean skim_hd_update_version_ok(const char *v);   /* digits and dots only */

/* The latest release, as far as an update needs it. */
typedef struct {
  char   tag[40];          /* "v0.8.9"                                         */
  char   version[32];      /* "0.8.9"                                          */
  char   html_url[256];    /* the release page                                 */
  char   published[32];    /* "2026-11-22T00:40:26Z"                           */
  char   asset_name[128];  /* skimmer-headless-<version>-x86_64.AppImage       */
  char   asset_url[512];   /* "" = the release has no such asset               */
  char   sha256[65];       /* the asset's digest, hex; "" = none published     */
  gint64 size;             /* the asset's, -1 = unknown                        */
} SkimHdRelease;

/* Reads the API's answer for one release. FALSE = not a release this can
 * use (no tag of the form v<version>); a release without the AppImage
 * asset parses, with asset_url "". */
gboolean skim_hd_update_parse_release(const char *body, gsize len, SkimHdRelease *r);

/* What one process knows about itself — the environment of every call. */
typedef struct {
  const char   *appimage;     /* the running AppImage; NULL = not one          */
  const char   *version;      /* SKIMMER_VERSION                               */
  const char   *build;        /* SKIMMER_BUILD: "" = a release build           */
  const char   *config_path;  /* the preflight checks the new version on it    */
  const char   *state_path;   /* update.state                                  */
  const char   *base_url;     /* NULL = SKIM_HD_UPDATE_API; a file:// one lets
                               * every download be file:// (the gate)          */
  const char   *ca_path;      /* NULL = libcurl's own trust store              */
  GCancellable *cancel;       /* NULL = not cancellable                        */
  gint64      (*now_s)(gpointer user);   /* NULL = the wall clock             */
  gpointer      now_user;
} SkimHdUpdateEnv;

/* SKIM_UPDATE_URL (a mirror, the gate) — read on the main thread. NULL = the
 * API. Caller frees. */
char *skim_hd_update_env_base_url(void);
/* <state dir>/skimmer-for-linux/update.state. Caller frees. */
char *skim_hd_update_state_path(void);

/* NULL when this process may replace its file, else why not (caller frees). */
char *skim_hd_update_why_not(const SkimHdUpdateEnv *e);

typedef enum {
  SKIM_HD_UPDATE_UP_TO_DATE,  /* nothing newer than the running version        */
  SKIM_HD_UPDATE_NEWER,       /* newer, not downloaded (notify, can't, skipped) */
  SKIM_HD_UPDATE_READY,       /* newer, downloaded, checked: install can swap  */
  SKIM_HD_UPDATE_NOT_DUE,     /* the limits say not yet (no request sent)      */
  SKIM_HD_UPDATE_FAILED,      /* network, server, digest, preflight            */
  SKIM_HD_UPDATE_CANCELLED,
} SkimHdUpdateResult;

typedef struct {
  SkimHdUpdateResult result;
  char   detail[512];         /* one line for the log and the page            */
  char   version[32];         /* NEWER / READY: the release                   */
  gint64 wait_s;              /* until the limits allow the next check        */
} SkimHdUpdateOutcome;

/* One check, blocking (a worker thread, the gate). force = never mind the
 * daily limit (Check now). download = stage a newer version (mode auto,
 * Install now) — only where skim_hd_update_why_not() is NULL. */
void skim_hd_update_check(const SkimHdUpdateEnv *e, gboolean force, gboolean download,
                          SkimHdUpdateOutcome *out);

/* The staged version swapped in for e->appimage (its digest checked again
 * first). TRUE = the file at e->appimage is now `*version` (caller frees):
 * restart into it. */
gboolean skim_hd_update_install(const SkimHdUpdateEnv *e, char **version, GError **error);

/* <appimage>.prev put back; the version it replaces is not tried again.
 * TRUE = restart into e->appimage. */
gboolean skim_hd_update_rollback(const SkimHdUpdateEnv *e, const char *why, GError **error);

/* At start, before the radio. Counts a crash of a freshly installed version
 * (it did not stop in order last time) and, at SKIM_HD_UPDATE_CRASHES, rolls
 * back: TRUE = exec e->appimage again now (skim_hd_update_exec). Then marks
 * this run as running. */
gboolean skim_hd_update_boot(const SkimHdUpdateEnv *e);
/* An orderly end (every exit code but a crash's): the mark goes. */
void     skim_hd_update_stopped(const SkimHdUpdateEnv *e);
/* Up long enough: a freshly installed version is accepted. */
void     skim_hd_update_accept(const SkimHdUpdateEnv *e);

/* exec path with argv[1..] (argv[0] = path): every descriptor above stderr
 * closed on the way (the old AppImage's mount lets go), APPDIR dropped.
 * Returns only when exec failed. */
void skim_hd_update_exec(const char *path, char *const *argv);

/* --- the app's handle: a daily timer, a worker thread, the install hour ------ */
typedef struct _SkimHdUpdater SkimHdUpdater;

/* Main context. The file at env->appimage is a new version (or the old one
 * back): restart into it. */
typedef void (*SkimHdUpdaterRestart)(const char *why, gpointer user);

/* env's strings are copied; its cancel and clock are the updater's own. */
SkimHdUpdater *skim_hd_updater_new(const SkimHdUpdateEnv *env, SkimHdUpdateMode mode,
                                   guint hour_utc, SkimHdUpdaterRestart restart,
                                   gpointer user);
/* Arm: the first check `delay_s` from now (when due), then daily; the
 * install hour each minute (auto); the acceptance of a fresh version. */
void     skim_hd_updater_start(SkimHdUpdater *u, guint delay_s);
gboolean skim_hd_updater_busy(const SkimHdUpdater *u);
/* POST /api/update: "check", "install" (a staged version at once, else a
 * check that downloads and then installs), "rollback". NULL = started or
 * done; else why not (caller frees). */
char    *skim_hd_updater_action(SkimHdUpdater *u, const char *action);
/* GET /api/update (managers only). */
char    *skim_hd_updater_json(const SkimHdUpdater *u);
void     skim_hd_updater_free(SkimHdUpdater *u);

G_END_DECLS
