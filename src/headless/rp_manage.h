/* rp_manage.h — the skimmer runs its own Red Pitaya over SSH: its key on the
 * board, the agent installed in place of Pavel's two stock servers, the
 * inputs set, the board's state read for the page (docs/REDPITAYA.md).
 *
 * Everything goes through the system's `ssh` and `ssh-keygen` (no SSH
 * library): the skimmer's own ed25519 key in <state>/rp/, the board's host
 * key in <state>/rp/known_hosts (accepted on the first connect, checked
 * after), one ControlMaster connection kept 10 min in the runtime dir.
 *
 * The password is typed once, on the first connect, and never stored: ssh
 * asks for it through SSH_ASKPASS = this very binary, which fetches it from
 * a unix socket the job listens on for that run (0600, in a 0700 dir, the
 * peer checked to be this user) — never in argv, the environment or a file.
 * That run appends the skimmer's public key to the board's
 * /root/.ssh/authorized_keys (in RAM: Pavel's Alpine runs from RAM); the
 * install keeps a copy on the SD card that the new start.sh puts back at
 * each boot. No `lbu commit` — it would also save every other change
 * pending in the board's /etc.
 *
 * Install (a manager's explicit action): the agent to
 * apps/skimmer_rp/ on the card, Pavel's start.sh kept once as
 * start.sh.pavel, a start.sh that loads the same bitstream and starts the
 * agent, then that start.sh run. A stream from another client on either IP
 * refuses the install unless take_over. Uninstall puts start.sh.pavel back
 * and runs it.
 *
 * Jobs run one at a time on their own thread; the state is a JSON for
 * GET /api/redpitaya.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimHdRp SkimHdRp;

typedef enum {
  SKIM_HD_RP_CONNECT,    /* key (password once if the key is not there yet), probe */
  SKIM_HD_RP_PROBE,      /* what the board is and runs                              */
  SKIM_HD_RP_INSTALL,    /* the agent in, Pavel's start.sh kept                     */
  SKIM_HD_RP_UNINSTALL,  /* Pavel's start.sh back                                   */
  SKIM_HD_RP_SEL,        /* the inputs: live, and in start.sh when persist          */
  SKIM_HD_RP_STATUS,     /* the agent's `ctl status`                                */
} SkimHdRpJob;

typedef struct {
  const char *host;      /* CONNECT: the board's address (eth0's); NULL = the last one */
  const char *password;  /* CONNECT: only when the key is not on the board yet       */
  gboolean    take_over; /* INSTALL/UNINSTALL: even while another client streams     */
  guint8      sel_a, sel_b; /* SEL, INSTALL: input masks (bit r = receiver r on IN2) */
  gboolean    sel_given; /* INSTALL: use sel_a/b — else the card's start.sh's (or 0) */
  gboolean    persist;   /* SEL: also into start.sh                                  */
  gint        fpga;      /* INSTALL: 0 = the bitstream the card's start.sh loads now
                          * (a first install: Pavel's), 1 = the skimmer's own
                          * (docs/FPGA.md), 2 = Pavel's stock one                     */
  /* INSTALL/UNINSTALL: the addresses this skimmer streams from itself (its
   * own stream is no reason to refuse), NULL-terminated or NULL */
  const char *const *ours;
} SkimHdRpArgs;

/* state_dir: <state>/skimmer-for-linux. agent: the ARM agent to install
 * (NULL = skim_hd_rp_agent_path()). host: the board from the config (may be
 * NULL until a connect names one). */
SkimHdRp *skim_hd_rp_new(const char *state_dir, const char *agent, const char *host);
void      skim_hd_rp_free(SkimHdRp *rp);

/* Start a job; FALSE (error says why) when one already runs or the args
 * are wrong. The password is copied and wiped once used. done (may be
 * NULL) runs on the main context when the job ends. */
gboolean  skim_hd_rp_start(SkimHdRp *rp, SkimHdRpJob job, const SkimHdRpArgs *args,
                           void (*done)(SkimHdRp *rp, SkimHdRpJob job, gboolean ok,
                                        gpointer user),
                           gpointer user, GError **error);

/* The state for the page: {"ssh","agent_file","host","key","connected","job",
 * "step","error","board":{…},"busy":{…},"status":<the agent's JSON>,
 * "status_age_s"}. */
char     *skim_hd_rp_json(SkimHdRp *rp);

/* The board's readings from the agent's last status (NaN where it has
 * none): FPGA °C, the 1-minute load, Mb/s each IP sent (from two statuses'
 * tx bytes). FALSE when no status came in the last 30 s. */
typedef struct {
  double fpga_c, load1, mbps[2];
} SkimHdRpReadings;
gboolean  skim_hd_rp_readings(SkimHdRp *rp, SkimHdRpReadings *out);

/* What the board's statuses said since the last call (the stats' minute),
 * NaN where none did: FIFO overflows (both IPs' instances, and the skimmer
 * bitstream's ring overruns) and, on that bitstream, the ADC's loudest peak
 * each input (dBFS, of the 5-ms windows the statuses sampled) and the
 * 5.3-ms windows each input clipped in (clip_n1/2: an agent built since
 * 2026-11-27 counts them).
 * The counters are differences of the agent's totals; one made anew (an
 * agent restart) counts from zero. */
typedef struct {
  double overflows, peak_db[2], clips[2];
} SkimHdRpMinute;
void      skim_hd_rp_take_minute(SkimHdRp *rp, SkimHdRpMinute *out);

/* The board for status.json ("redpitaya"), NULL before any probe: the
 * model, the agent's version (or Pavel's servers), the mode, FPGA °C,
 * uptime, rp_load and rp_mem_mb, and each IP: host, streaming, client,
 * rate, receivers, inputs, overflows, orphan stops, Mb/s, link speed. The
 * caller's status_public scrubs the addresses and the readings for others. */
char     *skim_hd_rp_status_block(SkimHdRp *rp);

/* TRUE once a probe found the agent installed and running. */
gboolean  skim_hd_rp_managed(SkimHdRp *rp);

/* The ARM agent shipped with this build: $SKIM_RP_AGENT, else
 * skimmer-rp-armhf beside the executable (a build dir) or in
 * ../share/skimmer-for-linux/ (the AppImage);
 * NULL when there is none. */
char     *skim_hd_rp_agent_path(void);
/* The skimmer's own bitstream to install (skimmer_rp_122_88.bit beside the
 * binary, ../share/skimmer-for-linux, or SKIM_RP_BIT); NULL = none here. */
char     *skim_hd_rp_bit_path(void);

/* Call first in main(): when this process is ssh's askpass (SKIM_ASKPASS_SOCK
 * set and one argument, a password prompt), it prints the password from the
 * socket and exits; otherwise it returns at once. */
void      skim_hd_rp_askpass_main(int argc, char **argv);

G_END_DECLS
