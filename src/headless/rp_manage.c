/* rp_manage.c — the skimmer's Red Pitaya over SSH (rp_manage.h).
 *
 * Test hooks (the rp-manage gate; never set in production):
 *   SKIM_RP_SSH      the ssh binary (a fake that runs the command here)
 *   SKIM_RP_ROOT     a prefix for every path on the board
 *   SKIM_RP_P1_PORT  the boards' Protocol 1 port (default 1024)
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "rp_manage.h"

#include <errno.h>
#include <math.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../engine/hpsdr_p1.h"
#include "json.h"

#define RP_CARD   "/media/mmcblk0p1"
#define RP_DIR    RP_CARD "/apps/skimmer_rp"
#define RP_CTL    "/run/skimmer-rp.sock"
#define RP_TAG    "skimmer-for-linux"

struct _SkimHdRp {
  GMutex  mu;
  char   *dir;        /* <state>/rp: key, known_hosts                     */
  char   *rundir;     /* <runtime>/skimmer-rp: the master, askpass sockets */
  char   *agent;      /* the ARM agent to install, NULL = none here        */
  char   *ssh, *keygen;
  char   *root;       /* SKIM_RP_ROOT ("" on a real board)                 */
  guint16 p1_port;
  GCancellable *cancel;

  /* under mu */
  char   *host;
  gboolean running;
  SkimHdRpJob job;
  char   *step, *error;
  gboolean need_password, connected;
  gboolean host_changed;  /* the board showed another host key than known  */
  GHashTable *board;  /* the probe's key=value                           */
  struct { char host[16]; int state; gboolean agent; } ip[2]; /* see IP_* */
  char   *status;     /* the agent's last `ctl status`                     */
  gint64  status_t;   /* g_get_monotonic_time() of it                      */
  SkimHdRpReadings rd;
  double  tx_prev[2]; /* the status before: each IP's tx bytes, when       */
  gint64  tx_prev_t;
  GThread *thread;
};

enum { IP_UNKNOWN = -1, IP_SILENT, IP_IDLE, IP_BUSY, IP_OURS };

static const char *job_name(SkimHdRpJob j) {
  switch (j) {
  case SKIM_HD_RP_CONNECT: return "connect";
  case SKIM_HD_RP_PROBE: return "probe";
  case SKIM_HD_RP_INSTALL: return "install";
  case SKIM_HD_RP_UNINSTALL: return "uninstall";
  case SKIM_HD_RP_SEL: return "sel";
  case SKIM_HD_RP_STATUS: return "status";
  }
  return "?";
}

/* --- small helpers ------------------------------------------------------------- */

static void set_step(SkimHdRp *rp, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_step(SkimHdRp *rp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_mutex_lock(&rp->mu);
  g_free(rp->step);
  rp->step = s;
  g_mutex_unlock(&rp->mu);
}

static char *self_exe(void) { return g_file_read_link("/proc/self/exe", NULL); }

char *skim_hd_rp_agent_path(void) {
  const char *env = g_getenv("SKIM_RP_AGENT");
  if (env && *env) { return g_file_test(env, G_FILE_TEST_IS_REGULAR) ? g_strdup(env) : NULL; }
  char *self = self_exe();
  if (!self) { return NULL; }
  char *dir = g_path_get_dirname(self);
  g_free(self);
  char *cand[] = {
    g_build_filename(dir, "skimmer-rp-armhf", NULL),
    g_build_filename(dir, "..", "share", "skimmer-for-linux", "skimmer-rp-armhf", NULL),
  };
  g_free(dir);
  char *found = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(cand); i++) {
    if (!found && g_file_test(cand[i], G_FILE_TEST_IS_REGULAR)) { found = cand[i]; }
    else { g_free(cand[i]); }
  }
  return found;
}

/* A shell single-quoted word. */
static char *sq(const char *s) {
  GString *o = g_string_new("'");
  for (const char *p = s; *p; p++) {
    if (*p == '\'') { g_string_append(o, "'\\''"); }
    else { g_string_append_c(o, *p); }
  }
  g_string_append_c(o, '\'');
  return g_string_free(o, FALSE);
}

/* --- running ssh and friends ------------------------------------------------------ */

typedef struct {
  GMutex mu;
  GCond  cond;
  gboolean done;
  GSubprocess *p;
  gint64 deadline;   /* monotonic µs */
} Watch;

static gpointer watch_thread(gpointer data) {
  Watch *w = data;
  g_mutex_lock(&w->mu);
  while (!w->done) {
    if (!g_cond_wait_until(&w->cond, &w->mu, w->deadline)) {
      if (!w->done) { g_subprocess_force_exit(w->p); }
      break;
    }
  }
  g_mutex_unlock(&w->mu);
  return NULL;
}

static void kill_on_cancel(GCancellable *c, gpointer p) {
  (void)c;
  g_subprocess_force_exit(p);
}

/* argv with input on its stdin (NULL = none), killed after timeout_s or on
 * the cancel. *code = its exit status (−1 = killed or did not exit). The
 * cancellable is never handed to communicate (helper_run.c says why). */
static gboolean run(SkimHdRp *rp, const char *const *argv, char **envp, GBytes *input,
                    guint timeout_s, char **out, char **err, int *code, GError **error) {
  *code = -1;
  if (out) { *out = NULL; }
  if (err) { *err = NULL; }
  if (g_cancellable_set_error_if_cancelled(rp->cancel, error)) { return FALSE; }
  GSubprocessLauncher *l = g_subprocess_launcher_new(
      (input ? G_SUBPROCESS_FLAGS_STDIN_PIPE : G_SUBPROCESS_FLAGS_STDIN_INHERIT) |
      G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  if (envp) { g_subprocess_launcher_set_environ(l, envp); }
  GSubprocess *p = g_subprocess_launcher_spawnv(l, argv, error);
  g_object_unref(l);
  if (!p) { return FALSE; }
  Watch w = { .p = p, .deadline = g_get_monotonic_time() + (gint64)timeout_s * G_USEC_PER_SEC };
  g_mutex_init(&w.mu);
  g_cond_init(&w.cond);
  GThread *wt = g_thread_new("rp-watch", watch_thread, &w);
  const gulong hid = g_cancellable_connect(rp->cancel, G_CALLBACK(kill_on_cancel), p, NULL);
  GBytes *o = NULL, *e = NULL;
  gboolean ok = g_subprocess_communicate(p, input, NULL, &o, &e, error);
  if (!ok) { g_subprocess_force_exit(p); }
  g_cancellable_disconnect(rp->cancel, hid);
  g_mutex_lock(&w.mu);
  w.done = TRUE;
  g_cond_signal(&w.cond);
  g_mutex_unlock(&w.mu);
  g_thread_join(wt);
  g_mutex_clear(&w.mu);
  g_cond_clear(&w.cond);
  if (ok) {
    g_subprocess_wait(p, NULL, NULL);
    if (g_subprocess_get_if_exited(p)) { *code = g_subprocess_get_exit_status(p); }
    /* an empty GBytes has NULL data: "" then, never NULL */
    if (out) {
      const char *d = o ? g_bytes_get_data(o, NULL) : NULL;
      *out = g_strndup(d ? d : "", d ? g_bytes_get_size(o) : 0);
    }
    if (err) {
      const char *d = e ? g_bytes_get_data(e, NULL) : NULL;
      *err = g_strstrip(g_strndup(d ? d : "", d ? g_bytes_get_size(e) : 0));
    }
    if (g_cancellable_set_error_if_cancelled(rp->cancel, error)) { ok = FALSE; }
  }
  if (o) { g_bytes_unref(o); }
  if (e) { g_bytes_unref(e); }
  g_object_unref(p);
  return ok;
}

/* ssh's argv: key only (BatchMode, through the kept master) or with a
 * password through the askpass. */
static GPtrArray *ssh_argv(SkimHdRp *rp, const char *host, gboolean password) {
  GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
  char *key = g_build_filename(rp->dir, "id_ed25519", NULL);
  char *kh = g_build_filename(rp->dir, "known_hosts", NULL);
  g_ptr_array_add(a, g_strdup(rp->ssh));
  const char *fixed[] = { "-F", "/dev/null", "-T", "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none",
                          "-o", "GlobalKnownHostsFile=/dev/null",
                          "-o", "StrictHostKeyChecking=accept-new", "-o", "ConnectTimeout=8",
                          "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
                          "-o", "LogLevel=ERROR" };
  for (guint i = 0; i < G_N_ELEMENTS(fixed); i++) { g_ptr_array_add(a, g_strdup(fixed[i])); }
  g_ptr_array_add(a, g_strdup("-i"));
  g_ptr_array_add(a, key);
  g_ptr_array_add(a, g_strdup("-o"));
  g_ptr_array_add(a, g_strdup_printf("UserKnownHostsFile=%s", kh));
  g_free(kh);
  if (password) {
    const char *pw[] = { "-o", "BatchMode=no", "-o", "PubkeyAuthentication=no",
                         "-o", "PreferredAuthentications=password,keyboard-interactive",
                         "-o", "NumberOfPasswordPrompts=1", "-o", "ControlMaster=no",
                         "-o", "ControlPath=none" };
    for (guint i = 0; i < G_N_ELEMENTS(pw); i++) { g_ptr_array_add(a, g_strdup(pw[i])); }
  } else {
    const char *k[] = { "-o", "BatchMode=yes", "-o", "PreferredAuthentications=publickey" };
    for (guint i = 0; i < G_N_ELEMENTS(k); i++) { g_ptr_array_add(a, g_strdup(k[i])); }
    if (rp->rundir) {
      const char *m[] = { "-o", "ControlMaster=auto", "-o", "ControlPersist=600" };
      for (guint i = 0; i < G_N_ELEMENTS(m); i++) { g_ptr_array_add(a, g_strdup(m[i])); }
      g_ptr_array_add(a, g_strdup("-o"));
      g_ptr_array_add(a, g_strdup_printf("ControlPath=%s/cm-%%C", rp->rundir));
    } else {
      g_ptr_array_add(a, g_strdup("-o"));
      g_ptr_array_add(a, g_strdup("ControlPath=none"));
    }
  }
  g_ptr_array_add(a, g_strdup("-l"));
  g_ptr_array_add(a, g_strdup("root"));
  g_ptr_array_add(a, g_strdup(host));
  return a;
}

/* A script on the board (sh -s, the script on stdin) or a command; key only. */
static gboolean ssh_run(SkimHdRp *rp, const char *host, const char *cmd, GBytes *input,
                        guint timeout_s, char **out, GError **error) {
  GPtrArray *a = ssh_argv(rp, host, FALSE);
  g_ptr_array_add(a, g_strdup(cmd));
  g_ptr_array_add(a, NULL);
  char *o = NULL, *e = NULL;
  int code = -1;
  gboolean ok = run(rp, (const char *const *)a->pdata, NULL, input, timeout_s, &o, &e, &code, error);
  g_ptr_array_unref(a);
  if (ok && code == -1) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "ssh to %s timed out", host);
    ok = FALSE;
  } else if (ok && code != 0) {
    const char *why = e && *e ? e : (o && *o ? o : "");
    if (code == 255) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ssh to %s failed%s%s", host,
                  *why ? ": " : "", why);
    } else {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "on the board: %s (exit %d)",
                  *why ? why : "failed", code);
    }
    ok = FALSE;
  }
  if (ok && out) { *out = o; o = NULL; }
  g_free(o);
  g_free(e);
  return ok;
}

static gboolean ssh_script(SkimHdRp *rp, const char *host, const char *script, guint timeout_s,
                           char **out, GError **error) {
  char *rq = sq(rp->root);
  char *full = g_strdup_printf("R=%s\nC=$R" RP_CARD "\nD=$R" RP_DIR "\nCTL=$R" RP_CTL "\n%s",
                               rq, script);
  g_free(rq);
  GBytes *in = g_bytes_new_take(full, strlen(full));
  gboolean ok = ssh_run(rp, host, "sh -s", in, timeout_s, out, error);
  g_bytes_unref(in);
  return ok;
}

/* --- the askpass ------------------------------------------------------------------ */

typedef struct {
  int fd;
  const char *pw;
  gint stop;
  gint served;
} Askpass;

static gpointer askpass_thread(gpointer data) {
  Askpass *a = data;
  while (!g_atomic_int_get(&a->stop)) {
    struct pollfd p = { .fd = a->fd, .events = POLLIN };
    if (poll(&p, 1, 100) <= 0) { continue; }
    int c = accept(a->fd, NULL, NULL);
    if (c < 0) { continue; }
    struct ucred cr;
    socklen_t n = sizeof(cr);
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &n) == 0 && cr.uid == getuid()) {
      const char *p0 = a->pw;
      size_t left = strlen(p0);
      while (left) {
        ssize_t w = write(c, p0, left);
        if (w <= 0) { break; }
        p0 += w;
        left -= (size_t)w;
      }
      g_atomic_int_inc(&a->served);
    }
    close(c);
  }
  return NULL;
}

void skim_hd_rp_askpass_main(int argc, char **argv) {
  const char *path = getenv("SKIM_ASKPASS_SOCK");
  if (!path || argc != 2) { return; }
  /* only a password prompt gets it — never a yes/no question */
  if (!strstr(argv[1], "assword")) { _exit(1); }
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof(sa.sun_path)) { _exit(1); }
  strcpy(sa.sun_path, path);
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) { _exit(1); }
  char buf[512];
  size_t n = 0;
  for (;;) {
    ssize_t r = read(s, buf + n, sizeof(buf) - 1 - n);
    if (r <= 0) { break; }
    n += (size_t)r;
    if (n >= sizeof(buf) - 1) { break; }
  }
  close(s);
  if (n == 0) { _exit(1); }
  buf[n++] = '\n';
  if (write(1, buf, n) != (ssize_t)n) { _exit(1); }
  memset(buf, 0, sizeof(buf));
  _exit(0);
}

/* A command with the password: ssh asks this binary, which asks the socket. */
static gboolean ssh_password(SkimHdRp *rp, const char *host, const char *pw, const char *script,
                             GError **error) {
  if (!rp->rundir) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no private directory for the askpass socket");
    return FALSE;
  }
  char *path = g_strdup_printf("%s/ask-%d-%08x.sock", rp->rundir, (int)getpid(), g_random_int());
  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  if (strlen(path) >= sizeof(sa.sun_path)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "socket path too long: %s", path);
    g_free(path);
    return FALSE;
  }
  strcpy(sa.sun_path, path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const mode_t old = umask(077);
  const gboolean bound = fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  umask(old);
  if (!bound || listen(fd, 2) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "askpass socket: %s",
                g_strerror(errno));
    if (fd >= 0) { close(fd); }
    g_free(path);
    return FALSE;
  }
  Askpass ask = { .fd = fd, .pw = pw };
  GThread *t = g_thread_new("rp-askpass", askpass_thread, &ask);

  char *self = self_exe();
  char **envp = g_get_environ();
  envp = g_environ_setenv(envp, "SSH_ASKPASS", self ? self : "/nonexistent", TRUE);
  envp = g_environ_setenv(envp, "SSH_ASKPASS_REQUIRE", "force", TRUE);
  envp = g_environ_setenv(envp, "SKIM_ASKPASS_SOCK", path, TRUE);
  if (!g_environ_getenv(envp, "DISPLAY")) { envp = g_environ_setenv(envp, "DISPLAY", ":0", TRUE); }
  envp = g_environ_unsetenv(envp, "SSH_AUTH_SOCK");
  g_free(self);

  GPtrArray *a = ssh_argv(rp, host, TRUE);
  g_ptr_array_add(a, g_strdup("sh -s"));
  g_ptr_array_add(a, NULL);
  char *rq = sq(rp->root);
  char *full = g_strdup_printf("R=%s\n%s", rq, script);
  g_free(rq);
  GBytes *in = g_bytes_new_take(full, strlen(full));
  char *o = NULL, *e = NULL;
  int code = -1;
  gboolean ok = run(rp, (const char *const *)a->pdata, envp, in, 30, &o, &e, &code, error);
  g_bytes_unref(in);
  g_ptr_array_unref(a);
  g_strfreev(envp);
  g_atomic_int_set(&ask.stop, 1);
  g_thread_join(t);
  close(fd);
  unlink(path);
  g_free(path);
  if (ok && code != 0) {
    const gboolean denied = e && (strstr(e, "denied") || strstr(e, "Permission"));
    g_set_error(error, G_IO_ERROR, denied ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                "%s%s%s", denied ? "the board refused the password" : "ssh failed",
                e && *e && !denied ? ": " : "", e && !denied ? e : "");
    ok = FALSE;
  }
  g_free(o);
  g_free(e);
  return ok;
}

/* --- the key ---------------------------------------------------------------------- */

static char *read_pub(SkimHdRp *rp) {
  char *pubp = g_build_filename(rp->dir, "id_ed25519.pub", NULL);
  char *pub = NULL;
  g_file_get_contents(pubp, &pub, NULL, NULL);
  g_free(pubp);
  if (pub) { g_strstrip(pub); }
  return pub;
}

static gboolean ensure_key(SkimHdRp *rp, GError **error) {
  char *key = g_build_filename(rp->dir, "id_ed25519", NULL);
  gboolean ok = TRUE;
  if (!g_file_test(key, G_FILE_TEST_EXISTS)) {
    if (!rp->keygen) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "ssh-keygen not found (install openssh-client)");
      g_free(key);
      return FALSE;
    }
    char host[64] = "";
    gethostname(host, sizeof(host) - 1);
    for (char *p = host; *p; p++) {
      if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-') { *p = '-'; }
    }
    char *comment = g_strdup_printf(RP_TAG "@%s", host[0] ? host : "skimmer");
    const char *argv[] = { rp->keygen, "-q", "-t", "ed25519", "-N", "", "-C", comment, "-f", key, NULL };
    char *e = NULL;
    int code = -1;
    ok = run(rp, argv, NULL, NULL, 20, NULL, &e, &code, error);
    if (ok && code != 0) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ssh-keygen failed: %s", e ? e : "");
      ok = FALSE;
    }
    g_free(e);
    g_free(comment);
  }
  g_free(key);
  return ok;
}

/* --- the jobs ---------------------------------------------------------------------- */

typedef struct {
  SkimHdRp *rp;
  SkimHdRpJob job;
  char *host;
  char *pw;
  gboolean take_over, persist, sel_given;
  guint8 sel_a, sel_b;
  char **ours;
  void (*done)(SkimHdRp *, SkimHdRpJob, gboolean, gpointer);
  gpointer user;
  gboolean ok;
} Job;

static void wipe(char *s) {
  if (s) {
    volatile char *p = s;
    while (*p) { *p++ = 0; }
    g_free(s);
  }
}

static gboolean key_works(SkimHdRp *rp, const char *host, GError **error) {
  return ssh_run(rp, host, "true", NULL, 20, NULL, error);
}

static const char *board(SkimHdRp *rp, const char *k) {
  return rp->board ? g_hash_table_lookup(rp->board, k) : NULL;
}

static gboolean probe(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  set_step(rp, "reading the board");
  static const char script[] =
      "hw=$(dd if=$R/sys/bus/i2c/devices/0-0050/eeprom bs=2048 skip=3 count=1 2>/dev/null"
      " | tr '\\000' '\\n' | sed -n 's/^hw_rev=//p' | head -n 1)\n"
      "echo \"hw=$hw\"\n"
      "echo \"arch=$(uname -m)\"\n"
      "echo \"alpine=$(cat $R/etc/alpine-release 2>/dev/null)\"\n"
      "[ -e $R/dev/xdevcfg ] && echo xdevcfg=1\n"
      "[ -f $C/apps/sdr_receiver_hpsdr_122_88/sdr_receiver_hpsdr_122_88.bit ] && echo pavel=1\n"
      "[ -f $C/start.sh.pavel ] && echo backup=1\n"
      "grep -q " RP_TAG " $C/start.sh 2>/dev/null && echo installed=1\n"
      "[ -x $D/skimmer-rp ] && echo \"agent=$($D/skimmer-rp ctl --ctl $CTL version 2>/dev/null)\"\n"
      "pidof sdr-receiver-hpsdr >/dev/null && echo stock=1\n"
      "sel=$(sed -n 's/^sel=//p' $C/start.sh 2>/dev/null | head -n 1); echo \"sel=$sel\"\n"
      "for i in eth0 mvl0; do echo \"$i=$(ip -4 -o addr show $i 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1)\"; done\n"
      "exit 0\n";
  char *out = NULL;
  if (!ssh_script(rp, j->host, script, 30, &out, error)) { return FALSE; }
  GHashTable *b = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  char **lines = g_strsplit(out, "\n", -1);
  for (char **l = lines; *l; l++) {
    char *eq = strchr(*l, '=');
    if (eq && eq > *l) { g_hash_table_replace(b, g_strndup(*l, (gsize)(eq - *l)), g_strdup(g_strstrip(eq + 1))); }
  }
  g_strfreev(lines);
  g_free(out);

  /* who streams from each IP (Protocol 1's busy byte) */
  const char *ips[2] = { g_hash_table_lookup(b, "eth0"), g_hash_table_lookup(b, "mvl0") };
  g_mutex_lock(&rp->mu);
  if (rp->board) { g_hash_table_unref(rp->board); }
  rp->board = b;
  rp->connected = TRUE;                        /* the key works: it just ran */
  g_mutex_unlock(&rp->mu);
  for (int k = 0; k < 2; k++) {
    int state = IP_UNKNOWN;
    gboolean agent = FALSE;
    char host[16] = "";
    if (ips[k] && *ips[k]) {
      g_strlcpy(host, ips[k], sizeof(host));
      SkimHpsdrInfo info;
      GError *e = NULL;
      if (skim_hpsdr_discover(host, rp->p1_port, 400, &info, &e)) {
        state = info.busy ? IP_BUSY : IP_IDLE;
        agent = info.agent;
        for (char **o = j->ours; state == IP_BUSY && o && *o; o++) {
          if (strcmp(*o, host) == 0) { state = IP_OURS; }
        }
      } else {
        state = IP_SILENT;
        g_clear_error(&e);
      }
    }
    g_mutex_lock(&rp->mu);
    g_strlcpy(rp->ip[k].host, host, sizeof(rp->ip[k].host));
    rp->ip[k].state = state;
    rp->ip[k].agent = agent;
    g_mutex_unlock(&rp->mu);
  }
  return TRUE;
}

static gboolean job_connect(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  set_step(rp, "the skimmer's key");
  if (!ensure_key(rp, error)) { return FALSE; }
  set_step(rp, "connecting with the key");
  GError *e = NULL;
  if (!key_works(rp, j->host, &e)) {
    /* Pavel's image makes new host keys at every boot (it runs from RAM)
     * until the install keeps them on the card: a changed key is then no
     * attack but a reboot — still, only the password (the manager's word
     * that this is the board) makes the skimmer trust the new one */
    const gboolean changed = e && (strstr(e->message, "IDENTIFICATION HAS CHANGED") ||
                                   strstr(e->message, "Host key verification failed"));
    const gboolean refused = e && strstr(e->message, "ermission denied");
    if (!refused && !changed) {
      g_propagate_error(error, e);
      return FALSE;
    }
    g_clear_error(&e);
    if (!j->pw || !*j->pw) {
      g_mutex_lock(&rp->mu);
      rp->need_password = TRUE;
      rp->host_changed = changed;
      rp->connected = FALSE;
      g_mutex_unlock(&rp->mu);
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s",
                  changed ? "the board shows another SSH host key than before (Pavel's image makes "
                            "new ones at every boot until the skimmer's agent is installed; or the "
                            "card was re-imaged) — type its root password to trust it"
                          : "the board does not know the skimmer's key yet — type its root password once");
      return FALSE;
    }
    if (changed) {                              /* the password vouches for the new key */
      set_step(rp, "forgetting the board's old host key");
      char *kh = g_build_filename(rp->dir, "known_hosts", NULL);
      const char *argv[] = { rp->keygen ? rp->keygen : "ssh-keygen", "-q", "-R", j->host, "-f", kh, NULL };
      int code = -1;
      run(rp, argv, NULL, NULL, 20, NULL, NULL, &code, NULL);
      g_free(kh);
    }
    set_step(rp, "putting the skimmer's key on the board");
    char *pub = read_pub(rp);
    if (!pub || !g_str_has_prefix(pub, "ssh-ed25519 ") || strchr(pub, '\'')) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the skimmer's public key is unreadable");
      g_free(pub);
      return FALSE;
    }
    char **parts = g_strsplit(pub, " ", 3);
    char *script = g_strdup_printf(
        "umask 077\n"
        "mkdir -p $R/root/.ssh\n"
        "touch $R/root/.ssh/authorized_keys\n"
        "grep -qF '%s' $R/root/.ssh/authorized_keys || echo '%s' >> $R/root/.ssh/authorized_keys\n",
        parts[1] ? parts[1] : pub, pub);
    g_strfreev(parts);
    g_free(pub);
    gboolean ok = ssh_password(rp, j->host, j->pw, script, error);
    g_free(script);
    if (!ok) { return FALSE; }
    set_step(rp, "connecting with the key");
    if (!key_works(rp, j->host, error)) { return FALSE; }
  }
  g_mutex_lock(&rp->mu);
  rp->need_password = FALSE;
  rp->host_changed = FALSE;
  rp->connected = TRUE;
  g_mutex_unlock(&rp->mu);
  return probe(j, error);
}

/* What the install needs from the board, or why not. */
static gboolean supported(SkimHdRp *rp, GError **error) {
  g_mutex_lock(&rp->mu);
  const char *hw = board(rp, "hw"), *arch = board(rp, "arch");
  const char *why = !hw || !g_str_has_prefix(hw, "STEM_122-16SDR")
                        ? "only the STEMlab 122.88-16 SDR is supported (EEPROM hw_rev)"
                  : !arch || !g_str_has_prefix(arch, "armv7") ? "not an ARMv7 board"
                  : !board(rp, "xdevcfg") ? "no /dev/xdevcfg — not Pavel Demin's image?"
                  : !board(rp, "pavel") ? "Pavel's sdr_receiver_hpsdr_122_88 is not on the card"
                                        : NULL;
  char *hwc = g_strdup(hw ? hw : "?");
  g_mutex_unlock(&rp->mu);
  if (why) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s (board: %s)", why, hwc); }
  g_free(hwc);
  return why == NULL;
}

/* Another client streams from one of the IPs: no install or uninstall
 * without take_over (CLAUDE.md: never cut another client's stream on our
 * own). */
static gboolean others_stream(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  if (j->take_over) { return FALSE; }
  GString *who = g_string_new(NULL);
  g_mutex_lock(&rp->mu);
  for (int k = 0; k < 2; k++) {
    if (rp->ip[k].state == IP_BUSY) {
      g_string_append_printf(who, "%s%s", who->len ? ", " : "", rp->ip[k].host);
    }
  }
  g_mutex_unlock(&rp->mu);
  const gboolean busy = who->len > 0;
  if (busy) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "another client streams from %s — confirm to cut it (it may reconnect on its own)",
                who->str);
  }
  g_string_free(who, TRUE);
  return busy;
}

static gboolean switch_and_wait(Job *j, gboolean expect_agent, GError **error) {
  SkimHdRp *rp = j->rp;
  set_step(rp, "starting %s", expect_agent ? "the agent" : "Pavel's servers");
  if (!ssh_script(rp, j->host, "setsid $C/start.sh </dev/null >/dev/null 2>&1 &\n", 20, NULL,
                  error)) {
    return FALSE;
  }
  for (int t = 0; t < 20; t++) {
    g_usleep(500 * 1000);
    if (g_cancellable_is_cancelled(rp->cancel)) { break; }
    char *out = NULL;
    GError *e = NULL;
    const char *check = expect_agent ? "$D/skimmer-rp ctl --ctl $CTL version"
                                     : "pidof sdr-receiver-hpsdr >/dev/null && echo stock";
    gboolean ok = ssh_script(rp, j->host, check, 10, &out, &e);
    g_clear_error(&e);
    const gboolean up = ok && out && (expect_agent ? g_str_has_prefix(out, "skimmer-rp ")
                                                   : g_str_has_prefix(out, "stock"));
    g_free(out);
    if (up) { return probe(j, error); }
  }
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s did not come up in 10 s",
              expect_agent ? "the agent" : "Pavel's servers");
  return FALSE;
}

static char *start_sh(guint8 a, guint8 b) {
  return g_strdup_printf(
      "#! /bin/sh\n"
      "# " RP_TAG ": the skimmer's agent on Pavel Demin's sdr_receiver_hpsdr_122_88\n"
      "# bitstream, in place of his two servers. His own start.sh is start.sh.pavel\n"
      "# beside this one; the skimmer's Uninstall puts it back.\n"
      "\n"
      "# the inputs of eth0's and mvl0's receivers (bit r set = receiver r on IN2)\n"
      "sel=0x%02x,0x%02x\n"
      "\n"
      "apps_dir=/media/mmcblk0p1/apps\n"
      "\n"
      "source $apps_dir/stop.sh\n"
      "\n"
      "cat $apps_dir/sdr_receiver_hpsdr_122_88/sdr_receiver_hpsdr_122_88.bit > /dev/xdevcfg\n"
      "\n"
      "address=`awk -F : '$5=\"FF\"' OFS=: /sys/class/net/eth0/address`\n"
      "ip link add mvl0 link eth0 address $address type macvlan mode passthru\n"
      "echo 1 > /proc/sys/net/ipv4/conf/all/arp_filter\n"
      "echo 2 > /proc/sys/net/ipv4/conf/all/rp_filter\n"
      "\n"
      "# the board's SSH host keys, kept on the card at the install: the image makes new\n"
      "# ones at every boot, and the skimmer would no longer know its board\n"
      "if ls $apps_dir/skimmer_rp/hostkeys/ssh_host_*_key >/dev/null 2>&1; then\n"
      "  if ! cmp -s $apps_dir/skimmer_rp/hostkeys/ssh_host_ed25519_key /etc/ssh/ssh_host_ed25519_key; then\n"
      "    cp $apps_dir/skimmer_rp/hostkeys/ssh_host_* /etc/ssh/ && chmod 600 /etc/ssh/ssh_host_*_key\n"
      "    /etc/init.d/sshd restart > /dev/null 2>&1\n"
      "  fi\n"
      "fi\n"
      "\n"
      "# the skimmer's key, kept on the card (the board runs from RAM)\n"
      "if [ -f $apps_dir/skimmer_rp/authorized_keys ]; then\n"
      "  mkdir -p /root/.ssh && chmod 700 /root/.ssh\n"
      "  cat $apps_dir/skimmer_rp/authorized_keys /root/.ssh/authorized_keys 2>/dev/null | sort -u > /root/.ssh/ak.new\n"
      "  mv /root/.ssh/ak.new /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys\n"
      "fi\n"
      "\n"
      "$apps_dir/skimmer_rp/skimmer-rp serve --sel $sel > /var/log/skimmer-rp.log 2>&1 &\n",
      a, b);
}

static gboolean job_install(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  if (!rp->agent) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "this build carries no Red Pitaya agent (skimmer-rp-armhf)");
    return FALSE;
  }
  if (!probe(j, error) || !supported(rp, error) || others_stream(j, error)) { return FALSE; }
  if (!j->sel_given) {
    /* a reinstall keeps the inputs the card's start.sh has */
    unsigned a = 0, b = 0;
    g_mutex_lock(&rp->mu);
    const char *sel = board(rp, "sel");
    if (sel && sscanf(sel, "%x,%x", &a, &b) == 2) {
      j->sel_a = (guint8)a;
      j->sel_b = (guint8)b;
    }
    g_mutex_unlock(&rp->mu);
  }

  set_step(rp, "copying the agent");
  char *bin = NULL;
  gsize len = 0;
  if (!g_file_get_contents(rp->agent, &bin, &len, error)) { return FALSE; }
  char *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bin, len);
  GBytes *in = g_bytes_new_take(bin, len);
  char *out = NULL;
  char *tmp = g_strdup_printf("%s/tmp/skimmer-rp.new", rp->root);
  char *tq = sq(tmp);
  char *cmd = g_strdup_printf("cat > %s && sha256sum %s", tq, tq);
  gboolean ok = ssh_run(rp, j->host, cmd, in, 120, &out, error);
  g_free(cmd);
  g_free(tq);
  g_free(tmp);
  g_bytes_unref(in);
  if (ok && (!out || !g_str_has_prefix(out, sum))) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "the agent arrived damaged (sha256)");
    ok = FALSE;
  }
  g_free(out);
  g_free(sum);
  if (!ok) { return FALSE; }

  set_step(rp, "writing the card");
  char *ss = start_sh(j->sel_a, j->sel_b);
  char *script = g_strdup_printf(
      "set -e\n"
      "ro() { mount -o remount,ro $C 2>/dev/null || true; }\n"
      "mount -o remount,rw $C\n"
      "trap ro EXIT\n"
      "mkdir -p $D\n"
      "cp $R/tmp/skimmer-rp.new $D/skimmer-rp.new\n"
      "chmod 755 $D/skimmer-rp.new 2>/dev/null || true\n"
      "mv $D/skimmer-rp.new $D/skimmer-rp\n"
      "printf '#! /bin/sh\\nkillall skimmer-rp 2>/dev/null\\nexit 0\\n' > $D/stop.sh\n"
      "chmod 755 $D/stop.sh 2>/dev/null || true\n"
      "[ -f $R/root/.ssh/authorized_keys ] && grep " RP_TAG " $R/root/.ssh/authorized_keys > $D/authorized_keys || true\n"
      "mkdir -p $D/hostkeys && cp $R/etc/ssh/ssh_host_*_key $R/etc/ssh/ssh_host_*_key.pub $D/hostkeys/ 2>/dev/null || true\n"
      "if [ ! -f $C/start.sh.pavel ] && ! grep -q " RP_TAG " $C/start.sh; then cp $C/start.sh $C/start.sh.pavel; fi\n"
      "cat > $C/start.sh.new <<'SKIMMER_EOF'\n%sSKIMMER_EOF\n"
      "chmod 755 $C/start.sh.new 2>/dev/null || true\n"
      "mv $C/start.sh.new $C/start.sh\n"
      "sync\n"
      "rm -f $R/tmp/skimmer-rp.new\n"
      "echo written\n",
      ss);
  g_free(ss);
  ok = ssh_script(rp, j->host, script, 60, NULL, error);
  g_free(script);
  return ok && switch_and_wait(j, TRUE, error);
}

static gboolean job_uninstall(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  if (!probe(j, error)) { return FALSE; }
  g_mutex_lock(&rp->mu);
  const gboolean backup = board(rp, "backup") != NULL;
  g_mutex_unlock(&rp->mu);
  if (!backup) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                "no start.sh.pavel on the card — nothing to put back");
    return FALSE;
  }
  if (others_stream(j, error)) { return FALSE; }
  set_step(rp, "putting Pavel's start.sh back");
  static const char script[] =
      "set -e\n"
      "ro() { mount -o remount,ro $C 2>/dev/null || true; }\n"
      "mount -o remount,rw $C\n"
      "trap ro EXIT\n"
      "cp $C/start.sh.pavel $C/start.sh.new\n"
      "mv $C/start.sh.new $C/start.sh\n"
      "rm -f $C/start.sh.pavel\n"
      "killall skimmer-rp 2>/dev/null || true\n"
      "rm -rf $D\n"
      "sync\n";
  return ssh_script(rp, j->host, script, 60, NULL, error) && switch_and_wait(j, FALSE, error);
}

static gboolean job_sel(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  set_step(rp, "setting the inputs");
  char *script = g_strdup_printf(
      "set -e\n"
      "$D/skimmer-rp ctl --ctl $CTL sel a 0x%02x\n"
      "$D/skimmer-rp ctl --ctl $CTL sel b 0x%02x\n"
      "if [ %d = 1 ]; then\n"
      "  ro() { mount -o remount,ro $C 2>/dev/null || true; }\n"
      "  mount -o remount,rw $C\n"
      "  trap ro EXIT\n"
      "  sed 's/^sel=.*/sel=0x%02x,0x%02x/' $C/start.sh > $C/start.sh.new\n"
      "  mv $C/start.sh.new $C/start.sh\n"
      "  sync\n"
      "fi\n",
      j->sel_a, j->sel_b, j->persist ? 1 : 0, j->sel_a, j->sel_b);
  gboolean ok = ssh_script(rp, j->host, script, 30, NULL, error);
  g_free(script);
  return ok && probe(j, error);
}

static gboolean job_status(Job *j, GError **error) {
  SkimHdRp *rp = j->rp;
  char *out = NULL;
  if (!ssh_script(rp, j->host, "$D/skimmer-rp ctl --ctl $CTL status", 15, &out, error)) { return FALSE; }
  g_strstrip(out);
  if (out[0] != '{') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "the agent's status is not JSON");
    g_free(out);
    return FALSE;
  }
  /* the readings: "fpga_c":X|null, "load":[1m,…], each inst's "tx_bytes" */
  SkimHdRpReadings rd = { NAN, NAN, { NAN, NAN } };
  const char *p = strstr(out, "\"fpga_c\":");
  if (p && p[9] != 'n') { rd.fpga_c = g_ascii_strtod(p + 9, NULL); }
  p = strstr(out, "\"load\":[");
  if (p) { rd.load1 = g_ascii_strtod(p + 8, NULL); }
  double tx[2] = { NAN, NAN };
  p = out;
  for (int k = 0; k < 2 && (p = strstr(p, "\"tx_bytes\":")); k++) {
    p += 11;
    if (*p != 'n' && *p != '-') { tx[k] = g_ascii_strtod(p, NULL); }
  }
  const gint64 now = g_get_monotonic_time();
  g_mutex_lock(&rp->mu);
  const double dt = (now - rp->tx_prev_t) / 1e6;
  double d[2] = { NAN, NAN };
  for (int k = 0; k < 2; k++) {
    if (rp->tx_prev_t && dt > 1 && dt < 120 && !isnan(tx[k]) && !isnan(rp->tx_prev[k])) {
      double x = tx[k] - rp->tx_prev[k];
      /* eth0's counter is 32 bits on the 32-bit ARM (net_device_stats is an
       * unsigned long): it wraps every ~8 min at 68 Mb/s; mvl0's is 64 */
      if (x < 0 && rp->tx_prev[k] < 4294967296.0) { x += 4294967296.0; }
      if (x >= 0) { d[k] = x; }
    }
    rp->tx_prev[k] = tx[k];
  }
  /* mvl0 is a passthru macvlan on eth0: its datagrams go out through eth0
   * and count there too (rp-f08908, 2026-11-25: eth0 68.7 Mb/s = A 27.5 +
   * B 41.2) — IP A's own is the difference */
  if (!isnan(d[0]) && !isnan(d[1]) && d[0] >= d[1]) { d[0] -= d[1]; }
  for (int k = 0; k < 2; k++) {
    if (!isnan(d[k])) { rd.mbps[k] = d[k] * 8 / dt / 1e6; }
    if (rd.mbps[k] > 1000) { rd.mbps[k] = NAN; }   /* a counter made anew (mvl0) */
  }
  rp->tx_prev_t = now;
  rp->rd = rd;
  g_free(rp->status);
  rp->status = out;
  rp->status_t = now;
  g_mutex_unlock(&rp->mu);
  return TRUE;
}

static void kv_bool(GString *o, const char *k, gboolean v, gboolean comma);
static void kv_str(GString *o, const char *k, const char *v, gboolean comma);

/* The raw value of "key" inside [p, end): a string's contents, a number,
 * true/false/null — "" when absent. */
static void jraw(const char *p, const char *end, const char *key, char *out, gsize room) {
  out[0] = 0;
  char pat[48];
  g_snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *k = g_strstr_len(p, end - p, pat);
  if (!k) { return; }
  const char *v = k + strlen(pat), *e = v;
  if (*v == '"') {
    v++;
    e = v;
    while (e < end && *e != '"') { e++; }
  } else {
    while (e < end && *e != ',' && *e != '}' && *e != ']') { e++; }
  }
  g_strlcpy(out, v, MIN(room, (gsize)(e - v) + 1));
}

static void out_num(GString *o, const char *k, const char *raw, gboolean comma) {
  g_string_append_printf(o, "%s\"%s\":%s", comma ? "," : "", k, raw[0] ? raw : "null");
}

static void out_dbl(GString *o, const char *k, double v) {
  if (isnan(v)) { g_string_append_printf(o, ",\"%s\":null", k); return; }
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(num, sizeof(num), "%.1f", v);
  g_string_append_printf(o, ",\"%s\":%s", k, num);
}

char *skim_hd_rp_status_block(SkimHdRp *rp) {
  g_mutex_lock(&rp->mu);
  if (!rp->board || !g_hash_table_size(rp->board)) {
    g_mutex_unlock(&rp->mu);
    return NULL;
  }
  GString *o = g_string_new("{");
  kv_str(o, "board", board(rp, "hw"), FALSE);
  const char *ag = board(rp, "agent");
  const gboolean agent = board(rp, "installed") && ag && g_str_has_prefix(ag, "skimmer-rp ");
  kv_str(o, "firmware", agent ? ag : board(rp, "stock") ? "Pavel Demin's servers" : "none running", TRUE);
  kv_bool(o, "managed", agent, TRUE);
  const gboolean fresh = rp->status && g_get_monotonic_time() - rp->status_t < 30 * G_USEC_PER_SEC;
  char v[64];
  if (fresh) {
    const char *st = rp->status, *end = st + strlen(st);
    const char *inst = strstr(st, "\"inst\":[");
    const char *top_end = inst ? inst : end;
    jraw(st, top_end, "mode", v, sizeof(v));
    kv_str(o, "mode", v, TRUE);
    out_dbl(o, "fpga_c", rp->rd.fpga_c);
    jraw(st, top_end, "uptime_s", v, sizeof(v));
    out_num(o, "uptime_s", v, TRUE);
    out_dbl(o, "rp_load", rp->rd.load1);
    jraw(st, top_end, "mem_avail_kb", v, sizeof(v));
    if (v[0]) { g_string_append_printf(o, ",\"rp_mem_mb\":%.0f", g_ascii_strtod(v, NULL) / 1024); }
    g_string_append_printf(o, ",\"age_s\":%.0f", (g_get_monotonic_time() - rp->status_t) / 1e6);
    g_string_append(o, ",\"ips\":[");
    const char *q = inst ? inst + 8 : end;
    for (int k = 0; k < 2 && q < end; k++) {
      const char *ob = strchr(q, '{');
      if (!ob) { break; }
      int depth = 0;
      const char *oe = ob;
      for (; oe < end; oe++) {
        if (*oe == '{' || *oe == '[') { depth++; }
        else if ((*oe == '}' || *oe == ']') && --depth == 0) { break; }
      }
      g_string_append_printf(o, "%s{", k ? "," : "");
      kv_str(o, "host", rp->ip[k].host[0] ? rp->ip[k].host : NULL, FALSE);
      jraw(ob, oe, "streaming", v, sizeof(v));
      out_num(o, "streaming", v, TRUE);
      char cl[48], cp[16];
      jraw(ob, oe, "client", cl, sizeof(cl));
      jraw(ob, oe, "client_port", cp, sizeof(cp));
      if (cl[0]) {
        char *c = g_strdup_printf("%s:%s", cl, cp);
        kv_str(o, "client", c, TRUE);
        g_free(c);
      } else {
        kv_str(o, "client", NULL, TRUE);
      }
      static const char *nums[] = { "rate", "receivers", "sel", "overflows", "orphan_stops", "link_mbps" };
      for (guint i = 0; i < G_N_ELEMENTS(nums); i++) {
        jraw(ob, oe, nums[i], v, sizeof(v));
        out_num(o, nums[i], v, TRUE);
      }
      out_dbl(o, "mbps", rp->rd.mbps[k]);
      g_string_append_c(o, '}');
      q = oe + 1;
    }
    g_string_append_c(o, ']');
  }
  g_mutex_unlock(&rp->mu);
  g_string_append_c(o, '}');
  return g_string_free(o, FALSE);
}

gboolean skim_hd_rp_readings(SkimHdRp *rp, SkimHdRpReadings *out) {
  g_mutex_lock(&rp->mu);
  const gboolean fresh = rp->status && g_get_monotonic_time() - rp->status_t < 30 * G_USEC_PER_SEC;
  *out = rp->rd;
  g_mutex_unlock(&rp->mu);
  if (!fresh) { *out = (SkimHdRpReadings){ NAN, NAN, { NAN, NAN } }; }
  return fresh;
}

static gboolean job_done_idle(gpointer data) {
  Job *j = data;
  if (j->done) { j->done(j->rp, j->job, j->ok, j->user); }
  g_free(j->host);
  g_strfreev(j->ours);
  g_free(j);
  return G_SOURCE_REMOVE;
}

static gpointer job_thread(gpointer data) {
  Job *j = data;
  SkimHdRp *rp = j->rp;
  GError *e = NULL;
  switch (j->job) {
  case SKIM_HD_RP_CONNECT: j->ok = job_connect(j, &e); break;
  case SKIM_HD_RP_PROBE: j->ok = probe(j, &e); break;
  case SKIM_HD_RP_INSTALL: j->ok = job_install(j, &e); break;
  case SKIM_HD_RP_UNINSTALL: j->ok = job_uninstall(j, &e); break;
  case SKIM_HD_RP_SEL: j->ok = job_sel(j, &e); break;
  case SKIM_HD_RP_STATUS: j->ok = job_status(j, &e); break;
  }
  wipe(j->pw);
  j->pw = NULL;
  g_mutex_lock(&rp->mu);
  rp->running = FALSE;
  g_free(rp->step);
  rp->step = NULL;
  /* a status poll that failed keeps the last good one and says nothing
   * unless the agent is gone for good — the page shows its age */
  if (!j->ok || j->job != SKIM_HD_RP_STATUS) {
    g_free(rp->error);
    rp->error = j->ok ? NULL : g_strdup(e ? e->message : "failed");
  }
  if (!j->ok && e && j->job != SKIM_HD_RP_CONNECT && strstr(e->message, "ssh to ")) {
    rp->connected = FALSE;
  }
  g_mutex_unlock(&rp->mu);
  g_clear_error(&e);
  g_main_context_invoke(NULL, job_done_idle, j);
  return NULL;
}

gboolean skim_hd_rp_start(SkimHdRp *rp, SkimHdRpJob job, const SkimHdRpArgs *args,
                          void (*done)(SkimHdRp *, SkimHdRpJob, gboolean, gpointer), gpointer user,
                          GError **error) {
  static const SkimHdRpArgs none = { 0 };
  if (!args) { args = &none; }
  if (!rp->ssh) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "ssh not found (install openssh-client)");
    return FALSE;
  }
  g_mutex_lock(&rp->mu);
  if (rp->running) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY, "busy: %s", job_name(rp->job));
    g_mutex_unlock(&rp->mu);
    return FALSE;
  }
  if (job == SKIM_HD_RP_CONNECT && args->host && *args->host) {
    if (!g_hostname_is_ip_address(args->host) &&
        strspn(args->host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") !=
            strlen(args->host)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "not a host: %s", args->host);
      g_mutex_unlock(&rp->mu);
      return FALSE;
    }
    if (!rp->host || strcmp(rp->host, args->host) != 0) {
      g_free(rp->host);
      rp->host = g_strdup(args->host);
      rp->connected = FALSE;
      if (rp->board) { g_hash_table_remove_all(rp->board); }
    }
  }
  if (!rp->host || !*rp->host || rp->host[0] == '-') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "no board address yet");
    g_mutex_unlock(&rp->mu);
    return FALSE;
  }
  GThread *old = rp->thread;
  rp->thread = NULL;
  rp->running = TRUE;
  rp->job = job;
  g_free(rp->step);
  rp->step = g_strdup("starting");
  Job *j = g_new0(Job, 1);
  j->rp = rp;
  j->job = job;
  j->host = g_strdup(rp->host);
  j->pw = args->password ? g_strdup(args->password) : NULL;
  j->take_over = args->take_over;
  j->persist = args->persist;
  j->sel_a = args->sel_a;
  j->sel_b = args->sel_b;
  j->sel_given = args->sel_given || job == SKIM_HD_RP_SEL;
  j->ours = args->ours ? g_strdupv((char **)args->ours) : NULL;
  j->done = done;
  j->user = user;
  g_mutex_unlock(&rp->mu);
  if (old) { g_thread_join(old); }
  GThread *t = g_thread_new("rp-job", job_thread, j);
  g_mutex_lock(&rp->mu);
  rp->thread = t;
  g_mutex_unlock(&rp->mu);
  return TRUE;
}

gboolean skim_hd_rp_managed(SkimHdRp *rp) {
  g_mutex_lock(&rp->mu);
  const char *ag = board(rp, "agent");
  const gboolean m = board(rp, "installed") && ag && g_str_has_prefix(ag, "skimmer-rp ");
  g_mutex_unlock(&rp->mu);
  return m;
}

static void kv_bool(GString *o, const char *k, gboolean v, gboolean comma) {
  g_string_append_printf(o, "%s\"%s\":%s", comma ? "," : "", k, v ? "true" : "false");
}

static void kv_str(GString *o, const char *k, const char *v, gboolean comma) {
  g_string_append_printf(o, "%s\"%s\":", comma ? "," : "", k);
  if (!v) {
    g_string_append(o, "null");
    return;
  }
  g_string_append_c(o, '"');
  skim_json_append(o, v);
  g_string_append_c(o, '"');
}

char *skim_hd_rp_json(SkimHdRp *rp) {
  static const char *ipstate[] = { "silent", "idle", "busy", "ours" };
  GString *o = g_string_new("{");
  g_mutex_lock(&rp->mu);
  kv_bool(o, "ssh", rp->ssh != NULL, FALSE);
  kv_bool(o, "agent_file", rp->agent != NULL, TRUE);
  kv_str(o, "host", rp->host, TRUE);
  char *key = g_build_filename(rp->dir, "id_ed25519", NULL);
  kv_bool(o, "key", g_file_test(key, G_FILE_TEST_EXISTS), TRUE);
  g_free(key);
  kv_bool(o, "connected", rp->connected, TRUE);
  kv_bool(o, "need_password", rp->need_password, TRUE);
  kv_bool(o, "host_key_changed", rp->host_changed, TRUE);
  kv_str(o, "job", rp->running ? job_name(rp->job) : NULL, TRUE);
  kv_str(o, "step", rp->step, TRUE);
  kv_str(o, "error", rp->error, TRUE);
  g_string_append(o, ",\"board\":{");
  static const char *keys[] = { "hw", "arch", "alpine", "agent", "sel", "eth0", "mvl0" };
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) { kv_str(o, keys[i], board(rp, keys[i]), i > 0); }
  static const char *flags[] = { "xdevcfg", "pavel", "backup", "installed", "stock" };
  for (guint i = 0; i < G_N_ELEMENTS(flags); i++) { kv_bool(o, flags[i], board(rp, flags[i]) != NULL, TRUE); }
  g_string_append(o, "},\"ips\":[");
  for (int k = 0; k < 2; k++) {
    g_string_append_printf(o, "%s{", k ? "," : "");
    kv_str(o, "host", rp->ip[k].host[0] ? rp->ip[k].host : NULL, FALSE);
    kv_str(o, "state", rp->ip[k].state >= 0 ? ipstate[rp->ip[k].state] : NULL, TRUE);
    kv_bool(o, "agent", rp->ip[k].agent, TRUE);
    g_string_append_c(o, '}');
  }
  g_string_append(o, "]");
  if (rp->status) {
    g_string_append_printf(o, ",\"status\":%s,\"status_age_s\":%.0f", rp->status,
                           (g_get_monotonic_time() - rp->status_t) / 1e6);
  }
  g_mutex_unlock(&rp->mu);
  g_string_append_c(o, '}');
  return g_string_free(o, FALSE);
}

/* A private dir for ssh's master socket ("cm-" + 40 hex) and the askpass's:
 * <runtime>/skimmer-rp, or — a socket path has 108 bytes at most — a
 * /tmp/skimmer-rp-<uid> of our own (0700, ours, no symlink); NULL: none
 * fits, ssh then runs without a kept master. */
static gboolean private_dir(const char *d) {
  g_mkdir_with_parents(d, 0700);
  struct stat st;
  if (lstat(d, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) { return FALSE; }
  return chmod(d, 0700) == 0;
}

static char *run_dir(void) {
  char *d = g_build_filename(g_get_user_runtime_dir(), "skimmer-rp", NULL);
  if (strlen(d) + 45 < sizeof(((struct sockaddr_un *)0)->sun_path) && private_dir(d)) { return d; }
  g_free(d);
  d = g_strdup_printf("/tmp/skimmer-rp-%u", (unsigned)getuid());
  if (private_dir(d)) { return d; }
  g_free(d);
  return NULL;
}

SkimHdRp *skim_hd_rp_new(const char *state_dir, const char *agent, const char *host) {
  SkimHdRp *rp = g_new0(SkimHdRp, 1);
  g_mutex_init(&rp->mu);
  rp->dir = g_build_filename(state_dir, "rp", NULL);
  g_mkdir_with_parents(rp->dir, 0700);
  g_chmod(rp->dir, 0700);
  rp->rundir = run_dir();
  rp->agent = agent ? g_strdup(agent) : skim_hd_rp_agent_path();
  const char *ssh = g_getenv("SKIM_RP_SSH");
  rp->ssh = ssh && *ssh ? g_strdup(ssh) : g_find_program_in_path("ssh");
  rp->keygen = g_find_program_in_path("ssh-keygen");
  const char *root = g_getenv("SKIM_RP_ROOT");
  rp->root = g_strdup(root ? root : "");
  const char *port = g_getenv("SKIM_RP_P1_PORT");
  rp->p1_port = port && atoi(port) > 0 ? (guint16)atoi(port) : 1024;
  rp->host = host && *host ? g_strdup(host) : NULL;
  rp->cancel = g_cancellable_new();
  rp->ip[0].state = rp->ip[1].state = IP_UNKNOWN;
  return rp;
}

void skim_hd_rp_free(SkimHdRp *rp) {
  if (!rp) { return; }
  g_cancellable_cancel(rp->cancel);
  g_mutex_lock(&rp->mu);
  GThread *t = rp->thread;
  rp->thread = NULL;
  g_mutex_unlock(&rp->mu);
  if (t) { g_thread_join(t); }
  /* a job's done callback may still be queued on the main context: it
   * holds rp — so the owner frees rp only at exit, after the loop */
  g_object_unref(rp->cancel);
  if (rp->board) { g_hash_table_unref(rp->board); }
  g_free(rp->dir);
  g_free(rp->rundir);
  g_free(rp->agent);
  g_free(rp->ssh);
  g_free(rp->keygen);
  g_free(rp->root);
  g_free(rp->host);
  g_free(rp->step);
  g_free(rp->error);
  g_free(rp->status);
  g_mutex_clear(&rp->mu);
  g_free(rp);
}
