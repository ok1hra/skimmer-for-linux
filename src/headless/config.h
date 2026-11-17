/* config.h — skimmer-headless's settings (headless.ini), parsed and checked.
 *
 * One parser for every consumer: the start reads the file with it, and a
 * settings page validates a new ini with the same call before it is written,
 * so the file a restart reads has already been through the start's checks.
 *
 *   [radio]  type    hpsdr-p1 (default) | mock (synthetic CW, for the gates) |
 *                    hub (a worker: IQ, bands and decoding policy from a hub —
 *                    host=the hub[:port] or auto; [bands] not needed)
 *            host    one radio or a list (comma separated, tried in order),
 *                    each optionally host@ppm: that radio's own clock error;
 *                    auto (or none) = the radios a broadcast discovery finds
 *            rate    48000 / 96000 / 192000 — one rate for all receivers
 *            clock_ppm  for the radios without @ppm
 *   [bands]  <name>=<centre Hz> [modes], in receiver order (RX1 first);
 *            modes a comma list of cw, cw:v3, cw:v2, cw:deepcw, rtty — none = cw
 *            with [decode] engine; off = the band is kept, not received;
 *            @name = received here, decoded by the worker "name" (hub.h)
 *   [decode] engine=v3|v2|deepcw (the bands that name none), log=true|false,
 *            suppress_cw_on_rtty (default false: rtty_pairs.h)
 *   [feed]   call, port (0 = off), min_score, min_hearings, settle_s,
 *            settle_nodict_s, fresh_s, fold_s, learn, gate, gate_mode,
 *            rtty (RTTY spots on the feed too; default true)
 *   [rbn]    enabled (default false), port (default 7550), delay_s (0–99,
 *            default 0): a second telnet port for the RBN Aggregator, which
 *            relays to the Reverse Beacon Network — the feed's spots, each
 *            held delay_s seconds (the feed's call; needs [feed] call)
 *   [station] locator — the station's Maidenhead locator (4 or 6 characters):
 *            the centre of the page's map, the distances and bearings
 *   [hub]    name — this instance on the LAN (discovery, and the name a worker
 *            asks its hub for bands under; default the host name);
 *            port (7380, 0 = no hub), allow — who may connect as a worker
 *            (networks, default the private ranges and loopback)
 *   [peers]  hosts — other skimmers' telnet feeds (host or host:port, comma
 *            separated) whose CQ spots this one sends on as its own,
 *            deduplicated (headless/peers.h)
 *   [status] console_s (0 = quiet), http_port (0 = no web page)
 *   [web]    settings_from — the addresses / networks that may change the
 *            settings from the web page (a.b.c.d, a.b.c.d/n, ::1), default
 *            127.0.0.1,::1; editable here only, never from the page
 *   [record] bands, minutes, dir, start_utc (HH:MM)
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

#include "engine/pipeline.h"

G_BEGIN_DECLS

#define SKIM_HD_MAX_BANDS 8           /* sdr_receiver_hpsdr's receiver count */
#define SKIM_HD_MAX_LISTED 16         /* [bands] lines, the ones off included */
#define SKIM_HD_HOST_AUTO "auto"      /* [radio] host: what a broadcast finds */

typedef enum {
  SKIM_HD_SOURCE_HPSDR_P1 = 0,
  SKIM_HD_SOURCE_MOCK,
  SKIM_HD_SOURCE_HUB,                 /* a worker: IQ from a hub             */
} SkimHdSourceType;

#define SKIM_HD_HUB_PORT 7380

typedef struct {
  char         name[16];
  double       centre_hz;
  gboolean     cw;                    /* decode CW on this band              */
  SkimCwEngine cw_engine;             /* … with this engine                  */
  gboolean     rtty;                  /* decode RTTY beside it               */
  gboolean     off;                   /* listed, not received ("off")        */
  char         worker[32];            /* "@name": decoded there; "" = here   */
} SkimHdBand;

typedef struct {
  char            *path;              /* the file read (relative paths in it
                                       * resolve next to it)                 */
  /* [radio] */
  SkimHdSourceType source;
  char           **hosts;             /* in search order, NULL-terminated    */
  double          *host_ppm;          /* each radio's clock_ppm              */
  char            *hosts_text;        /* "a, b" for messages                 */
  guint            rate;
  double           ppm;               /* clock_ppm for the radios w/o @ppm   */
  /* [bands] — band[]: the ones received, in receiver order; listed[]:
   * every line in file order, the ones off included (the settings page) */
  SkimHdBand       band[SKIM_HD_MAX_BANDS];
  guint            nb;
  SkimHdBand       listed[SKIM_HD_MAX_LISTED];
  guint            nlisted;
  /* [decode] */
  SkimCwEngine     engine;            /* default for bands that name none    */
  gboolean         decode_log;
  gboolean         suppress_cw_on_rtty; /* CW off the RTTY pairs copied     */
  /* [feed] — 0 = the pipeline's default, < 0 = off, as SkimPipelineConfig */
  char            *feed_call;
  int              feed_port;
  double           feed_min_score;
  guint            feed_min_hearings;
  double           feed_settle_s;
  double           feed_settle_nodict_s;
  double           feed_fresh_s;
  double           feed_fold_s;
  gboolean         feed_learn;
  gboolean         feed_rtty;         /* RTTY spots go to the feed too       */
  char            *feed_gate;         /* absolute path; NULL = built in      */
  char            *feed_gate_mode;    /* as written; NULL = not set          */
  /* [rbn] */
  gboolean         rbn_on;
  int              rbn_port;
  guint            rbn_delay_s;
  /* [station] */
  char             locator[8];        /* "JO60WA", "" = not set              */
  /* [hub] */
  char            *hub_name;          /* never NULL                          */
  int              hub_port;          /* 0 = no hub server                   */
  char           **hub_allow;         /* NULL-terminated, never empty        */
  /* [peers] */
  char           **peers;             /* NULL-terminated, maybe empty        */
  /* [status] — -1 = not in the file */
  int              console_s;
  int              http_port;
  /* [web] */
  char           **settings_from;     /* NULL-terminated, never empty list  */
  /* [record] */
  char           **rec_bands;         /* NULL = no recording                 */
  double           rec_minutes;
  char            *rec_dir;
  gint64           rec_at_us;         /* real time to start, 0 = at once     */
} SkimHdConfig;

/* Write the commented default ini to path (its directory is created).
 * radios: comment lines naming the radios found on the LAN, or NULL. */
gboolean skim_hd_config_write_default(const char *path, const char *radios, GError **error);

/* Parse an ini held in memory; path names it in messages and anchors its
 * relative paths. FALSE + error (and cfg cleared) on a bad host, a band that
 * is no centre in Hz or names an unknown mode, more than SKIM_HD_MAX_BANDS
 * bands on (SKIM_HD_MAX_LISTED in all), no band on at all, a rate other than 48/96/192 kHz or an unknown
 * radio type. */
gboolean skim_hd_config_parse(SkimHdConfig *cfg, const char *text, gsize len,
                              const char *path, GError **error);

/* The same from the file. */
gboolean skim_hd_config_load(SkimHdConfig *cfg, const char *path, GError **error);

void skim_hd_config_clear(SkimHdConfig *cfg);

/* ---- editing (the settings page) ---------------------------------------------- */

/* The keys a settings form may set, "group.key" (bands: one "name=centre
 * [modes]" per line, the whole [bands] group in receiver order). Everything
 * else in the file — comments, the feed policy, the gate, [web] — stays. */
extern const char *const SKIM_HD_EDITABLE[];

/* The ini text with form's fields applied (a field not in the form keeps
 * its value). NULL + error on a key that is not editable or a value with a
 * line break. The result is NOT checked — skim_hd_config_parse() it. */
char *skim_hd_config_apply_form(const char *text, gsize len, GHashTable *form,
                                GError **error);

/* An ini imported from a file (the settings page): in_text with the
 * current file's [web] group in place of its own — who may change the
 * settings is set in the file on this machine only, never by an upload.
 * NULL + error when in_text is no key file. The result is NOT checked —
 * skim_hd_config_parse() it. */
char *skim_hd_config_import(const char *cur, gsize cur_len, const char *in_text, gsize in_len,
                            GError **error);

/* Write text as the config at path, the current file first copied to
 * <path>.bak-<UTC> (*backup: its name); at most SKIM_HD_BACKUPS kept. */
#define SKIM_HD_BACKUPS 10
gboolean skim_hd_config_save(const char *path, const char *text, char **backup,
                             GError **error);

/* The backups of path, newest first (basenames; free with g_strfreev). */
char **skim_hd_config_backups(const char *path);

/* Put backup (a basename from skim_hd_config_backups) back as the config;
 * the current file becomes a backup itself, so a restore can be undone. */
gboolean skim_hd_config_restore(const char *path, const char *backup, GError **error);

/* ---- hub and worker ------------------------------------------------------------ */

/* What a hub tells the worker "worker": the bands assigned to it ("@worker",
 * on), the rate, [decode] and the feed policy of hub_text — an ini
 * fragment; NULL when nothing is assigned to it. */
char *skim_hd_config_worker_fragment(const char *hub_text, gsize len, const char *worker);

/* The worker's own ini with a hub's fragment over it: [bands] and [decode]
 * from the fragment, its [feed] keys and [radio] rate set; everything else
 * (radio type/host, [hub], [status], [web], its feed port…) stays the
 * worker's. NULL + error when either is no key file. */
char *skim_hd_config_worker_merge(const char *own, gsize own_len, const char *frag,
                                  gsize frag_len, GError **error);

/* "cw", "cw:deepcw,rtty", "cw,off" … — the band's modes as the ini writes them. */
char *skim_hd_band_modes(const SkimHdBand *b, SkimCwEngine dflt);

G_END_DECLS
