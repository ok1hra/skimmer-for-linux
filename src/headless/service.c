/* service.c — see service.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "service.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#define ERR (g_quark_from_static_string("skim-hd-service"))

static const char ENV_TEMPLATE[] =
  "# skimmer-headless environment for its systemd user service (EnvironmentFile).\n"
  "# One VAR=value per line, read at every start. `skimmer-headless --list-switches`\n"
  "# lists the SKIM_* switches; the ones set here are logged at start and shown\n"
  "# on the web page, and an unknown SKIM_* name is warned about.\n"
  "#\n"
  "# e.g. a system ONNX Runtime with CUDA for DeepCW:\n"
  "#SKIM_ORT_LIB=/usr/lib/x86_64-linux-gnu/libonnxruntime.so.1\n"
  "#SKIM_DEEPCW_DEVICE=cuda\n";

static char *unit_path(void) {
  return g_build_filename(g_get_user_config_dir(), "systemd", "user",
                          SKIM_HD_SERVICE ".service", NULL);
}

static char *env_path(void) {
  return g_build_filename(g_get_user_config_dir(), "skimmer-for-linux", "headless.env", NULL);
}

gboolean skim_hd_service_under_systemd(void) {
  const char *id = g_getenv("INVOCATION_ID");
  return id && id[0];
}

char *skim_hd_service_exe(void) {
  const char *ai = g_getenv("APPIMAGE");
  if (ai && g_path_is_absolute(ai)) { return g_strdup(ai); }
  char *self = g_file_read_link("/proc/self/exe", NULL);
  return self ? self : g_strdup("skimmer-headless");
}

/* One ExecStart word: double-quoted, systemd's escapes (\\ \" and %%). */
static void exec_word(GString *o, const char *w) {
  g_string_append_c(o, '"');
  for (const char *p = w; *p; p++) {
    if (*p == '"' || *p == '\\') { g_string_append_c(o, '\\'); }
    if (*p == '%') { g_string_append_c(o, '%'); }
    g_string_append_c(o, *p);
  }
  g_string_append_c(o, '"');
}

static char *escape_pct(const char *s) {
  GString *o = g_string_new(NULL);
  for (const char *p = s; *p; p++) {
    if (*p == '%') { g_string_append_c(o, '%'); }
    g_string_append_c(o, *p);
  }
  return g_string_free(o, FALSE);
}

char *skim_hd_service_unit(const char *exe, const char *config) {
  GString *ex = g_string_new(NULL);
  exec_word(ex, exe);
  /* the env file where --install-service puts it (an absolute path: the
   * unit's %h would miss a non-default XDG_CONFIG_HOME) */
  char *env = env_path();
  char *envf = escape_pct(env);
  if (config) {
    g_string_append(ex, " --config ");
    exec_word(ex, config);
  }
  char *u = g_strdup_printf(
      "# Written by `skimmer-headless --install-service`; --uninstall-service\n"
      "# removes it. Run --install-service again after moving the program.\n"
      "[Unit]\n"
      "Description=Skimmer for Linux — multi-band CW/RTTY skimmer (headless)\n"
      "Wants=network-online.target\n"
      "After=network-online.target\n"
      "# a radio that is busy or off is retried for as long as it takes\n"
      "StartLimitIntervalSec=0\n"
      "\n"
      "[Service]\n"
      "Type=simple\n"
      "ExecStart=%s\n"
      "EnvironmentFile=-%s\n"
      "# exit 3 (no free radio, stream lost) and crashes: again after a minute;\n"
      "# 2 (the config is wrong) stays down; 75 = a supervisor restart\n"
      "Restart=on-failure\n"
      "RestartSec=60\n"
      "RestartPreventExitStatus=2\n"
      "RestartForceExitStatus=75\n"
      "# SIGTERM stops it cleanly and frees the radio\n"
      "TimeoutStopSec=20\n"
      "\n"
      "[Install]\n"
      "WantedBy=default.target\n",
      ex->str, envf);
  g_string_free(ex, TRUE);
  g_free(envf);
  g_free(env);
  return u;
}

/* systemctl --user <args>: report, never fail the install over it. */
static void systemctl(const char *const *args) {
  GPtrArray *av = g_ptr_array_new();
  g_ptr_array_add(av, (gpointer) "systemctl");
  g_ptr_array_add(av, (gpointer) "--user");
  for (const char *const *a = args; *a; a++) { g_ptr_array_add(av, (gpointer)*a); }
  g_ptr_array_add(av, NULL);
  char *cmd = g_strjoinv(" ", (char **)av->pdata);
  int status = 0;
  GError *err = NULL;
  char *out = NULL;
  if (!g_spawn_sync(NULL, (char **)av->pdata, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                    &out, &status, &err)) {
    printf("  could not run %s (%s) — run it yourself\n", cmd, err->message);
    g_clear_error(&err);
  } else if (!g_spawn_check_wait_status(status, NULL)) {
    printf("  %s failed%s%s\n", cmd, out && out[0] ? ": " : "", out ? g_strstrip(out) : "");
  } else {
    printf("  %s\n", cmd);
  }
  g_free(out);
  g_free(cmd);
  g_ptr_array_free(av, TRUE);
}

static gboolean lingering(void) {
  char *p = g_build_filename("/var/lib/systemd/linger", g_get_user_name(), NULL);
  const gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
  g_free(p);
  return ok;
}

gboolean skim_hd_service_install(const char *exe, const char *config, GError **error) {
  char *unit = unit_path(), *env = env_path();
  char *dir = g_path_get_dirname(unit), *edir = g_path_get_dirname(env);
  char *text = skim_hd_service_unit(exe, config);
  gboolean ok = FALSE;
  const gboolean had = g_file_test(unit, G_FILE_TEST_EXISTS);
  if (g_mkdir_with_parents(dir, 0755) != 0 || g_mkdir_with_parents(edir, 0755) != 0) {
    g_set_error(error, ERR, 1, "%s: %s", dir, g_strerror(errno));
  } else if (g_file_set_contents(unit, text, -1, error)) {
    ok = TRUE;
    printf("service: %s %s\n  runs %s%s%s\n", had ? "updated" : "wrote", unit, exe,
           config ? " --config " : "", config ? config : "");
    if (!g_file_test(env, G_FILE_TEST_EXISTS)) {
      ok = g_file_set_contents(env, ENV_TEMPLATE, -1, error);
      if (ok) { printf("  environment: %s (new, all commented out)\n", env); }
    } else {
      printf("  environment: %s (kept)\n", env);
    }
  }
  if (ok) {
    const char *reload[] = { "daemon-reload", NULL };
    systemctl(reload);
    printf("\nStart it now and at every login:\n"
           "  systemctl --user enable --now " SKIM_HD_SERVICE "\n"
           "Its log:\n"
           "  journalctl --user -u " SKIM_HD_SERVICE " -f\n");
    if (!lingering()) {
      printf("To keep it running while you are logged out (once; this is not done for you):\n"
             "  loginctl enable-linger %s\n", g_get_user_name());
    }
  }
  g_free(text);
  g_free(dir);
  g_free(edir);
  g_free(unit);
  g_free(env);
  return ok;
}

gboolean skim_hd_service_uninstall(GError **error) {
  char *unit = unit_path(), *env = env_path();
  gboolean ok = TRUE;
  if (!g_file_test(unit, G_FILE_TEST_EXISTS)) {
    printf("service: no %s — nothing to remove\n", unit);
  } else {
    printf("service: removing %s\n", unit);
    const char *stop[] = { "disable", "--now", SKIM_HD_SERVICE, NULL };
    systemctl(stop);
    if (g_unlink(unit) != 0) {
      g_set_error(error, ERR, 2, "%s: %s", unit, g_strerror(errno));
      ok = FALSE;
    } else {
      const char *reload[] = { "daemon-reload", NULL };
      systemctl(reload);
      printf("  kept: %s and the config\n", env);
    }
  }
  g_free(unit);
  g_free(env);
  return ok;
}
