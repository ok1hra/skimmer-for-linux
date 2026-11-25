/* rp_manage_test.c — the rp-manage gate: rp_manage.c against a fake board.
 *
 * A fake `ssh` (SKIM_RP_SSH) runs each command on this machine with every
 * board path under SKIM_RP_ROOT, checks the key or asks the askpass (this
 * very binary) for the password; shims for mount, ip, pidof, killall and
 * uname stand in for the board's. "Switching" to the agent starts the real
 * agent (skimmer-rp-host --sim) on 127.0.0.4/.5, so `ctl status`, `sel` and
 * Protocol 1's busy byte are real.
 *
 *   skimmer-rp-manage-test PATH/TO/skimmer-rp-host
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "headless/rp_manage.h"

static int failed;

static void check(const char *what, gboolean ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) { failed++; }
}

static char *T, *R;

static char *rpath(const char *rel) { return g_build_filename(R, rel, NULL); }

static void put(const char *rel, const char *text, int mode) {
  char *p = rpath(rel);
  char *d = g_path_get_dirname(p);
  g_mkdir_with_parents(d, 0755);
  g_file_set_contents(p, text, -1, NULL);
  if (mode) { g_chmod(p, mode); }
  g_free(d);
  g_free(p);
}

static char *get(const char *rel) {
  char *p = rpath(rel), *s = NULL;
  g_file_get_contents(p, &s, NULL, NULL);
  g_free(p);
  return s;
}

static gboolean exists(const char *rel) {
  char *p = rpath(rel);
  gboolean e = g_file_test(p, G_FILE_TEST_EXISTS);
  g_free(p);
  return e;
}

static void shim(const char *name, const char *body) {
  char *p = g_build_filename(T, "bin", name, NULL);
  char *s = g_strdup_printf("#!/bin/sh\nR=\"$SKIM_RP_ROOT\"\n%s", body);
  g_file_set_contents(p, s, -1, NULL);
  g_chmod(p, 0755);
  g_free(s);
  g_free(p);
}

static const char fake_ssh[] =
    "key= pwmode=0 host=\n"
    "while [ $# -gt 0 ]; do\n"
    "  case \"$1\" in\n"
    "    -o) [ \"$2\" = PubkeyAuthentication=no ] && pwmode=1; shift 2;;\n"
    "    -i) key=$2; shift 2;;\n"
    "    -l|-F) shift 2;;\n"
    "    -T) shift;;\n"
    "    *) host=$1; shift; break;;\n"
    "  esac\n"
    "done\n"
    "echo \"$host $pwmode $*\" >> \"$R/ssh.log\"\n"
    "[ \"$host\" = \"$FAKE_HOST\" ] || { echo \"ssh: connect to host $host port 22: No route to host\" >&2; exit 255; }\n"
    "if [ -f \"$R/hostkey_changed\" ] && [ $pwmode = 0 ]; then\n"
    "  echo '@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@' >&2; echo 'Host key verification failed.' >&2; exit 255\n"
    "fi\n"
    "if [ $pwmode = 1 ]; then\n"
    "  rm -f \"$R/hostkey_changed\"\n"
    "  pw=$(\"$SSH_ASKPASS\" \"root@$host's password: \") || { echo 'Permission denied' >&2; exit 255; }\n"
    "  [ \"$pw\" = \"$FAKE_PW\" ] || { echo 'Permission denied, please try again.' >&2; exit 255; }\n"
    "else\n"
    "  body=$(cut -d' ' -f2 \"$key.pub\")\n"
    "  grep -qF \"$body\" \"$R/root/.ssh/authorized_keys\" 2>/dev/null || "
    "{ echo \"root@$host: Permission denied (publickey,password).\" >&2; exit 255; }\n"
    "fi\n"
    "if [ \"$*\" = 'sh -s' ]; then\n"
    "  script=$(cat)\n"
    "  case \"$script\" in\n"
    "    *setsid*start.sh*)\n"
    "      for f in agent stock; do [ -f \"$R/$f.pid\" ] && kill $(cat \"$R/$f.pid\") 2>/dev/null; rm -f \"$R/$f.pid\"; done\n"
    "      rm -f \"$R/stock\"\n"
    "      sleep 0.3\n"
    "      if grep -q skimmer-for-linux \"$R/media/mmcblk0p1/start.sh\"; then\n"
    "        sel=$(sed -n 's/^sel=//p' \"$R/media/mmcblk0p1/start.sh\")\n"
    "        \"$R/media/mmcblk0p1/apps/skimmer_rp/skimmer-rp\" serve --sim --sel \"$sel\" "
    "--bind 127.0.0.4:$SKIM_RP_P1_PORT,127.0.0.5:$SKIM_RP_P1_PORT --ctl \"$R/run/skimmer-rp.sock\" "
    "--orphan-s 2 </dev/null >\"$R/agent.log\" 2>&1 &\n"
    "        echo $! > \"$R/agent.pid\"\n"
    "      else\n"
    "        touch \"$R/stock\"\n"
    "      fi\n"
    "      exit 0;;\n"
    "  esac\n"
    "  printf '%s\\n' \"$script\" | sh\n"
    "  exit $?\n"
    "fi\n"
    "exec sh -c \"$*\"\n";

static gboolean job_ok;
static gboolean job_over;

static void on_done(SkimHdRp *rp, SkimHdRpJob job, gboolean ok, gpointer user) {
  (void)rp; (void)job; (void)user;
  job_ok = ok;
  job_over = TRUE;
}

static gboolean run_job(SkimHdRp *rp, SkimHdRpJob job, const SkimHdRpArgs *a) {
  job_over = FALSE;
  GError *e = NULL;
  if (!skim_hd_rp_start(rp, job, a, on_done, NULL, &e)) {
    printf("     start refused: %s\n", e->message);
    g_error_free(e);
    return FALSE;
  }
  const gint64 end = g_get_monotonic_time() + 40 * G_USEC_PER_SEC;
  while (!job_over && g_get_monotonic_time() < end) { g_main_context_iteration(NULL, FALSE); g_usleep(10000); }
  if (!job_over) { printf("     job did not end\n"); return FALSE; }
  if (!job_ok) {
    char *j = skim_hd_rp_json(rp);
    printf("     job failed: %s\n", j);
    g_free(j);
  }
  return job_ok;
}

static gboolean has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }

/* A raw Protocol 1 start from a socket of ours: the IP is then busy. */
static int busy_stream(const char *ip, guint16 port) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
  inet_pton(AF_INET, ip, &a.sin_addr);
  unsigned char start[64] = { 0xef, 0xfe, 0x04, 0x01 };
  sendto(s, start, sizeof(start), 0, (struct sockaddr *)&a, sizeof(a));
  return s;
}

int main(int argc, char **argv) {
  skim_hd_rp_askpass_main(argc, argv);
  if (argc != 2) {
    fprintf(stderr, "usage: %s PATH/TO/skimmer-rp-host\n", argv[0]);
    return 2;
  }
  char *agent = g_canonicalize_filename(argv[1], NULL);
  T = g_dir_make_tmp("rp-manage-XXXXXX", NULL);
  R = g_build_filename(T, "root", NULL);
  char *state = g_build_filename(T, "state", NULL);
  char *runtime = g_build_filename(T, "run", NULL);
  g_mkdir_with_parents(runtime, 0700);
  char *bin = g_build_filename(T, "bin", NULL);
  g_mkdir_with_parents(bin, 0755);
  const guint16 port = (guint16)(21000 + getpid() % 20000);
  char portstr[8];
  g_snprintf(portstr, sizeof(portstr), "%u", port);

  /* the board: Pavel's card, the EEPROM, Alpine */
  static const char pavel_start[] =
      "#! /bin/sh\n\napps_dir=/media/mmcblk0p1/apps\n\nsource $apps_dir/stop.sh\n"
      "$apps_dir/sdr_receiver_hpsdr_122_88/sdr-receiver-hpsdr eth0 1 1 1 1 1 1 1 1 &\n";
  put("media/mmcblk0p1/start.sh", pavel_start, 0755);
  put("media/mmcblk0p1/apps/sdr_receiver_hpsdr_122_88/sdr_receiver_hpsdr_122_88.bit", "bit", 0);
  put("media/mmcblk0p1/apps/stop.sh", "#! /bin/sh\n", 0755);
  put("etc/alpine-release", "3.12.0\n", 0);
  put("dev/xdevcfg", "", 0);
  put("run/.keep", "", 0);
  put("tmp/.keep", "", 0);
  put("root/.keep", "", 0);
  put("etc/ssh/ssh_host_ed25519_key", "HOSTKEY-PRIVATE\n", 0600);
  put("etc/ssh/ssh_host_ed25519_key.pub", "ssh-ed25519 HOSTKEY board\n", 0);
  put("stock", "", 0);
  {
    GString *ee = g_string_sized_new(8192);
    for (int i = 0; i < 6144; i++) { g_string_append_c(ee, '\xff'); }
    g_string_append(ee, "dummy=1");
    g_string_append_c(ee, '\0');
    g_string_append(ee, "hw_rev=STEM_122-16SDR_v1.1");
    g_string_append_c(ee, '\0');
    char *p = rpath("sys/bus/i2c/devices/0-0050/eeprom");
    char *d = g_path_get_dirname(p);
    g_mkdir_with_parents(d, 0755);
    g_file_set_contents(p, ee->str, (gssize)ee->len, NULL);
    g_free(d);
    g_free(p);
    g_string_free(ee, TRUE);
  }
  shim("ssh-fake", fake_ssh);
  shim("mount", "echo \"mount $*\" >> \"$R/mount.log\"\n");
  shim("uname", "echo armv7l\n");
  shim("pidof", "[ \"$1\" = sdr-receiver-hpsdr ] && [ -f \"$R/stock\" ] && exit 0\nexit 1\n");
  shim("killall", "[ \"$1\" = skimmer-rp ] && [ -f \"$R/agent.pid\" ] && kill $(cat \"$R/agent.pid\") && rm -f \"$R/agent.pid\"\nexit 0\n");
  shim("ip", "case \"$5\" in eth0) echo '2: eth0    inet 127.0.0.4/8 scope host eth0';; "
             "mvl0) echo '4: mvl0    inet 127.0.0.5/8 scope host mvl0';; esac\n");

  char *ssh = g_build_filename(bin, "ssh-fake", NULL);
  char *path = g_strdup_printf("%s:%s", bin, g_getenv("PATH"));
  g_setenv("PATH", path, TRUE);
  g_setenv("SKIM_RP_SSH", ssh, TRUE);
  g_setenv("SKIM_RP_ROOT", R, TRUE);
  g_setenv("SKIM_RP_P1_PORT", portstr, TRUE);
  g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
  g_setenv("FAKE_HOST", "127.0.0.4", TRUE);
  g_setenv("FAKE_PW", "s3cret pass", TRUE);
  g_unsetenv("SSH_AUTH_SOCK");

  SkimHdRp *rp = skim_hd_rp_new(state, agent, NULL);
  char *j = skim_hd_rp_json(rp);
  check("fresh: ssh found, agent file, no key, no host",
        has(j, "\"ssh\":true") && has(j, "\"agent_file\":true") && has(j, "\"key\":false") &&
            has(j, "\"host\":null"));
  g_free(j);

  GError *e = NULL;
  check("a job without a board address is refused",
        !skim_hd_rp_start(rp, SKIM_HD_RP_PROBE, NULL, NULL, NULL, &e));
  g_clear_error(&e);
  SkimHdRpArgs bad = { .host = "-oProxyCommand=x" };
  check("a host that would be an ssh option is refused",
        !skim_hd_rp_start(rp, SKIM_HD_RP_CONNECT, &bad, NULL, NULL, &e));
  g_clear_error(&e);

  SkimHdRpArgs a = { .host = "127.0.0.4" };
  check("connect without the key on the board and no password fails", !run_job(rp, SKIM_HD_RP_CONNECT, &a));
  j = skim_hd_rp_json(rp);
  check("… asks for the password, the key was made", has(j, "\"need_password\":true") && has(j, "\"key\":true"));
  g_free(j);
  char *kp = g_build_filename(state, "rp", "id_ed25519", NULL);
  struct stat st;
  check("the private key is 0600 in a 0700 dir",
        g_stat(kp, &st) == 0 && (st.st_mode & 0777) == 0600);
  char *kdir = g_path_get_dirname(kp);
  check("… the dir 0700", g_stat(kdir, &st) == 0 && (st.st_mode & 0777) == 0700);

  a.password = "wrong";
  check("a wrong password fails", !run_job(rp, SKIM_HD_RP_CONNECT, &a));
  j = skim_hd_rp_json(rp);
  check("… says the board refused it", has(j, "refused the password"));
  g_free(j);
  check("… and put no key on the board", !exists("root/.ssh/authorized_keys"));

  a.password = "s3cret pass";
  check("the right password connects", run_job(rp, SKIM_HD_RP_CONNECT, &a));
  char *ak = get("root/.ssh/authorized_keys");
  char *pubp = g_strconcat(kp, ".pub", NULL), *pub = NULL;
  g_file_get_contents(pubp, &pub, NULL, NULL);
  check("the skimmer's key is on the board", pub && ak && has(ak, g_strstrip(pub)));
  j = skim_hd_rp_json(rp);
  check("probe: the board, Pavel's image, stock servers, both IPs",
        has(j, "\"connected\":true") && has(j, "\"hw\":\"STEM_122-16SDR_v1.1\"") &&
            has(j, "\"arch\":\"armv7l\"") && has(j, "\"pavel\":true") && has(j, "\"stock\":true") &&
            has(j, "\"installed\":false") && has(j, "\"eth0\":\"127.0.0.4\"") &&
            has(j, "\"mvl0\":\"127.0.0.5\""));
  g_free(j);

  a.password = NULL;
  check("the next connect needs no password", run_job(rp, SKIM_HD_RP_CONNECT, &a));
  put("hostkey_changed", "", 0);                 /* the board rebooted: a new host key */
  check("a changed host key: the key alone is not enough", !run_job(rp, SKIM_HD_RP_CONNECT, &a));
  j = skim_hd_rp_json(rp);
  check("… asks for the password, and says why", has(j, "\"host_key_changed\":true") &&
        has(j, "\"need_password\":true") && has(j, "another SSH host key"));
  g_free(j);
  a.password = "s3cret pass";
  check("… the password trusts the new one", run_job(rp, SKIM_HD_RP_CONNECT, &a));
  j = skim_hd_rp_json(rp);
  check("… and the flags clear", has(j, "\"host_key_changed\":false") && has(j, "\"connected\":true"));
  g_free(j);
  a.password = NULL;
  char *log = get("ssh.log");
  check("the password never reached ssh's command line", log && !has(log, "s3cret"));
  g_free(log);
  {
    gchar *argvg[] = { "grep", "-rl", "s3cret", state, runtime, NULL };
    gint code = 0;
    g_spawn_sync(NULL, argvg, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
                 NULL, NULL, &code, NULL);
    check("… nor any file of the skimmer's", WIFEXITED(code) && WEXITSTATUS(code) == 1);
  }

  /* askpass: only a password prompt, and only with the socket */
  {
    char *self = g_file_read_link("/proc/self/exe", NULL);
    gchar *av[] = { self, "Are you sure you want to continue connecting (yes/no)?", NULL };
    gchar *envp[] = { "SKIM_ASKPASS_SOCK=/nonexistent", NULL };
    gchar *out = NULL;
    gint code = 0;
    g_spawn_sync(NULL, av, envp, 0, NULL, NULL, &out, NULL, &code, NULL);
    check("askpass answers no yes/no question", WIFEXITED(code) && WEXITSTATUS(code) == 1 && out && !*out);
    g_free(out);
    g_free(self);
  }

  /* another client streams from mvl0 (a stand-in stock server) */
  char *standin = g_strdup_printf("127.0.0.5:%u", port);
  gchar *sv[] = { agent, "serve", "--sim", "--bind", standin, "--ctl", NULL, NULL };
  char *sock2 = g_build_filename(T, "standin.sock", NULL);
  sv[6] = sock2;
  GPid spid = 0;
  g_spawn_async(NULL, sv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                &spid, NULL);
  char *pidtxt = g_strdup_printf("%d\n", (int)spid);
  put("stock.pid", pidtxt, 0);
  g_free(pidtxt);
  g_usleep(300 * 1000);
  int client = busy_stream("127.0.0.5", port);
  g_usleep(300 * 1000);

  SkimHdRpArgs in = { .sel_a = 0x00, .sel_b = 0x02, .sel_given = TRUE };
  check("install refused while another client streams", !run_job(rp, SKIM_HD_RP_INSTALL, &in));
  j = skim_hd_rp_json(rp);
  check("… names its IP, and nothing was written",
        has(j, "127.0.0.5") && has(j, "another client") && !exists("media/mmcblk0p1/start.sh.pavel"));
  check("… the IP reads busy", has(j, "{\"host\":\"127.0.0.5\",\"state\":\"busy\""));
  g_free(j);

  const char *ours[] = { "127.0.0.5", NULL };
  in.ours = ours;
  check("… but not when that stream is this skimmer's own", run_job(rp, SKIM_HD_RP_INSTALL, &in));
  close(client);
  in.ours = NULL;
  {
    int status = 0;
    check("the stand-in stock server was stopped by the switch", waitpid(spid, &status, 0) == spid);
  }
  char *ss = get("media/mmcblk0p1/start.sh"), *bak = get("media/mmcblk0p1/start.sh.pavel");
  check("start.sh is the skimmer's, with the inputs",
        has(ss, "skimmer-for-linux") && has(ss, "sel=0x00,0x02") && has(ss, "skimmer-rp serve --sel $sel"));
  check("Pavel's start.sh kept as start.sh.pavel", bak && strcmp(bak, pavel_start) == 0);
  g_free(ss);
  char *ak2 = get("media/mmcblk0p1/apps/skimmer_rp/authorized_keys");
  check("the key kept on the card", ak2 && pub && has(ak2, pub));
  g_free(ak2);
  check("the board's host keys kept on the card, and start.sh puts them back",
        exists("media/mmcblk0p1/apps/skimmer_rp/hostkeys/ssh_host_ed25519_key") &&
            exists("media/mmcblk0p1/apps/skimmer_rp/hostkeys/ssh_host_ed25519_key.pub"));
  ss = get("media/mmcblk0p1/start.sh");
  check("… start.sh restores them and restarts sshd only when they differ",
        has(ss, "cmp -s $apps_dir/skimmer_rp/hostkeys/ssh_host_ed25519_key") && has(ss, "/etc/init.d/sshd restart"));
  g_free(ss);
  check("the agent and its stop.sh on the card",
        exists("media/mmcblk0p1/apps/skimmer_rp/skimmer-rp") && exists("media/mmcblk0p1/apps/skimmer_rp/stop.sh"));
  check("the upload left nothing in /tmp", !exists("tmp/skimmer-rp.new"));
  log = get("mount.log");
  check("the card remounted rw, then ro", has(log, "remount,rw") && has(log, "remount,ro"));
  g_free(log);
  check("managed", skim_hd_rp_managed(rp));
  j = skim_hd_rp_json(rp);
  check("both IPs answer as the agent, idle",
        has(j, "{\"host\":\"127.0.0.4\",\"state\":\"idle\",\"agent\":true}") &&
            has(j, "{\"host\":\"127.0.0.5\",\"state\":\"idle\",\"agent\":true}") &&
            has(j, "\"agent\":\"skimmer-rp 0.3\"") && has(j, "\"stock\":false"));
  g_free(j);

  check("status read", run_job(rp, SKIM_HD_RP_STATUS, NULL));
  j = skim_hd_rp_json(rp);
  check("… the agent's JSON, inst B on IN2 for RX2",
        has(j, "\"status\":{\"agent\":\"0.3\"") && has(j, "\"status_age_s\":") && has(j, "\"sel\":2"));
  g_free(j);
  SkimHdRpReadings rd;
  check("… its readings: the load (the sim has no FPGA sensor: NaN)",
        skim_hd_rp_readings(rp, &rd) && !isnan(rd.load1) && isnan(rd.fpga_c));

  SkimHdRpArgs sel = { .sel_a = 0x05, .sel_b = 0x00, .persist = TRUE };
  check("inputs set live and kept", run_job(rp, SKIM_HD_RP_SEL, &sel));
  run_job(rp, SKIM_HD_RP_STATUS, NULL);
  j = skim_hd_rp_json(rp);
  check("… the agent took them", has(j, "\"sel\":5") && !has(j, "\"sel\":2"));
  g_free(j);
  ss = get("media/mmcblk0p1/start.sh");
  check("… and start.sh says sel=0x05,0x00", has(ss, "sel=0x05,0x00") && has(ss, "skimmer-for-linux"));
  g_free(ss);

  SkimHdRpArgs again = { 0 };
  check("a second install", run_job(rp, SKIM_HD_RP_INSTALL, &again));
  ss = get("media/mmcblk0p1/start.sh");
  check("… keeps the card's inputs when given none", has(ss, "sel=0x05,0x00"));
  g_free(ss);
  char *bak2 = get("media/mmcblk0p1/start.sh.pavel");
  check("… keeps Pavel's start.sh, not ours, as the backup", bak2 && strcmp(bak2, pavel_start) == 0);
  g_free(bak2);

  check("uninstall", run_job(rp, SKIM_HD_RP_UNINSTALL, NULL));
  ss = get("media/mmcblk0p1/start.sh");
  check("Pavel's start.sh is back, the backup and the agent gone",
        ss && strcmp(ss, pavel_start) == 0 && !exists("media/mmcblk0p1/start.sh.pavel") &&
            !exists("media/mmcblk0p1/apps/skimmer_rp"));
  g_free(ss);
  check("the agent stopped", !exists("agent.pid"));
  j = skim_hd_rp_json(rp);
  check("the board reads stock again", has(j, "\"stock\":true") && has(j, "\"installed\":false"));
  g_free(j);
  check("not managed", !skim_hd_rp_managed(rp));
  check("a second uninstall has nothing to put back", !run_job(rp, SKIM_HD_RP_UNINSTALL, NULL));

  skim_hd_rp_free(rp);
  if (exists("agent.pid")) {
    char *p = get("agent.pid");
    kill(atoi(p), SIGTERM);
    g_free(p);
  }
  if (!failed) {
    gchar *rm[] = { "rm", "-rf", T, NULL };
    g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  } else {
    printf("left for a look: %s\n", T);
  }
  printf("%s\n", failed ? "rp-manage: FAILED" : "rp-manage: all passed");
  return failed ? 1 : 0;
}
