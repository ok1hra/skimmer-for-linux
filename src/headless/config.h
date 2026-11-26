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
 *            rate    48000 / 96000 / 192000 / 384000 (4 receivers an IP at
 *                    384 kHz) — IP A's receivers; IP B's is [rp] b_rate
 *            clock_ppm  for the radios without @ppm
 *   [bands]  <name>=<centre Hz> [modes], in receiver order (RX1 first);
 *            modes a comma list of cw, cw:v3, cw:v4, cw:v2, cw:deepcw, rtty — none = cw
 *            with [decode] engine; off = the band is kept, not received;
 *            @name = received here, decoded by the worker "name" (hub.h);
 *            ip=b = from the board's second IP (mvl0, DDC 8–15; default
 *            ip=a, [radio] host); in=2 = from the ADC input IN2 (the
 *            skimmer's agent on the board only, docs/REDPITAYA.md) — 8
 *            bands an IP, 16 in all; ip=ab = from both inputs, phased
 *            (docs/PHASING.md): a receiver on IP A (IN1) and one on IP B
 *            (IN2) at the centre, combined per channel before decoding —
 *            counts on both IPs, needs the agent (0.4: paired streams), one
 *            rate on both IPs, no in=, no @worker; phase=mvdr|mrc|fixed|a|b
 *            overrides [phase] mode for the band
 *   [rp]     mode — p1 (default: one set of 16 DDCs, each band its input,
 *            in=) or extended (two antennas: every ip=a band on IN1, every
 *            ip=b band on IN2 — in= is then implied), b_host — the second IP
 *            (default: the agent on [radio] host names it), b_rate — its
 *            rate (default [radio] rate)
 *   [phase]  the phased bands' defaults (docs/PHASING.md): mode
 *            (mvdr | mrc | fixed | a | b, default mvdr), tc_s (1: the
 *            estimates' time constant), gate_db (6: a station sample stands
 *            this far over the noise), fixed_gain_db / fixed_phase_deg (B
 *            against A in mode fixed)
 *   [phase.<band>]  a phased band's own: fixed_gain_db, fixed_phase_deg, and
 *            the phasing panel's memories m1..m3 = "gain_db,phase_deg" (the
 *            panel's Save writes them, with the band's phase=)
 *   [decode] engine=v3|v4|v2|deepcw (the bands that name none), log=true|false,
 *            v4_workers (0 = half the CPUs): CW v4's inference threads,
 *            v4_guard (default true): the governor's v4 load guard,
 *            suppress_cw_on_rtty (default false: rtty_pairs.h),
 *            rtty_adaptive (default false), rtty_adaptive_spots (100),
 *            rtty_adaptive_minutes (2): the RTTY decoders on at each full
 *            hour and at start, off again until the next full hour when all
 *            bands spot no more than that in the first minutes (governor.h)
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
 *   [archive] dir — every spot the feed sends, a gzip file a UTC day
 *            (<dir>/YYYYMMDD-spots.gz, headless/archive.h); empty = off;
 *            ~/ is the home, a relative path lies next to this file
 *   [visitors] login (default true): the web page's visitors from outside
 *            free_from (default the private networks, loopback, link-local)
 *            or through a proxy sign in with their callsign; the visits and
 *            the refused sign-ins are logged (visitors.h)
 *   [thermal] enabled (default false), limit_c (40–110 °C; empty = the CPU
 *            sensor's critical temperature − 15 °C, else 80), average_min
 *            (5), restore_margin_c (3), probe_every_min (30; 0 = never),
 *            probe_min (5): the thermal guard (governor.h) — the band with
 *            the fewest spots paused while the CPU runs hot, a paused one
 *            tested in place of the weakest now and then
 *   [filter] call — only the calls that match go anywhere (empty = all):
 *            part of a call, any case; regex=true: a regular expression
 *            (PCRE, any case). Dropped where they are decoded — no station,
 *            spot, feed line, RBN line, map dot or count; peers' and
 *            workers' spots are held to it too
 *            mycall=true also drops this station's own call ([feed] call)
 *            and every call with up to mycall_diff changes against it
 *            (default 2), compared place by place: a letter other, or one
 *            missing at the end (a cut call), counts one — the letters keep
 *            their places, so a call read backwards differs in all; the
 *            busts the skimmer makes of its own transmissions; a call
 *            MASTER.SCP knows passes all the same (but the own call itself)
 *            bad — the bad calls, comma-separated, dropped the same way:
 *            a prefix (UA9, a call that starts with it), a whole call
 *            (OK1ABC, also with a /part), or a country's name or part of it
 *            (four letters or more, any case: the DXCC's name as the map
 *            has it, dxcc.h — Russia drops European and Asiatic Russia,
 *            Kaliningrad, Franz Josef Land and Belarus as Belorussia)
 *   [rtty_palette] the web page's RTTY palette over the waterfall (browser
 *            only — the engine never reads it): open_on_spot (true: a click
 *            on an RTTY spot opens it), bandwidth (400 Hz, 250–500: the
 *            audio's passband while it decodes), autotune (true: a steady
 *            pair is centred once by itself), afc (true), afc_max_hz (60,
 *            10–180), afc_rate (60 Hz a character, 5–85), reverse (false),
 *            usos (true), squelch_db (0 = off, 1–10), second_decoder (true),
 *            clean (false: the time-aligned tape), sound (true: heard too)
 *   [wspr]   enabled (default false): WSPR-2 decoded on every WSPR frequency
 *            inside a band received (wspr.h); upload (false): the spots to
 *            WSPRnet under call (empty = [feed] call) and locator (empty =
 *            [station] locator) — both needed then; upload_url (the gate's
 *            stand-in; not on the page)
 *   [ft8]    enabled (default false): FT8/FT4 decoded on every FT8 / FT4
 *            dial inside a band received (ft8.h, WSJT-X's decoders); ft8,
 *            ft4 (true): which dials; feed (true): each station that sends
 *            to the telnet feed (false: the web page only); depth (2: 1–3,
 *            WSJT-X's Fast/Normal/Deep); lo_hz, hi_hz (200, 3500): the audio
 *            window above the dial (lo 200–1000, hi 2000–4000); threads (2,
 *            1–8): decoders side by side; bands (empty = every band): the
 *            [bands] names whose FT8/FT4 dials are decoded, e.g. "40m, 20m";
 *            upload (false): the stations heard
 *            to PSK Reporter (pskreporter.h) under call (empty = [feed]
 *            call) and locator (empty = [station] locator) — both needed
 *            then; pskreporter (host:port, the gate's stand-in; not on the
 *            page). Not on a worker: its hub decodes.
 *   [js8]    enabled (default false): JS8 decoded on every JS8 dial inside a
 *            band received, in every submode (js8.h, JS8Call-improved's
 *            decoder); submodes ("normal,fast,turbo,slow"); feed (true):
 *            each station that sends to the telnet feed — a deep frame's
 *            call only once confirmed (false: the web page only); lo_hz,
 *            hi_hz (200, 3500, as [ft8]); threads (2, 1–8); bands (empty =
 *            every band); serve (true: one decoder process a dial kept
 *            running, its soft combining across periods; false: one run a
 *            period); clean (0.80, 0.50–1.00): the decoder's quality from
 *            which a frame is clean, under it deep; upload (false): the
 *            stations heard to PSK Reporter under [ft8] call/locator
 *   [update] mode auto (default) | notify | off: the daily look at the
 *            GitHub releases and what a newer one gets (update.h);
 *            hour_utc (0-23, default 3): when auto swaps it in
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

#include "engine/pipeline.h"

G_BEGIN_DECLS

#define SKIM_HD_MAX_BANDS 16          /* both IPs of a Red Pitaya, 8 each     */
#define SKIM_HD_IP_RX     8           /* sdr_receiver_hpsdr's receivers an IP */
#define SKIM_HD_IP_RX_384 4           /* … of them at 384 kHz (the board's ARM) */
#define SKIM_HD_MAX_LISTED 32         /* [bands] lines, the ones off included */
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
  guint8       ip;                    /* 0 = IP A ([radio] host: eth0, DDC
                                       * 0–7), 1 = IP B (mvl0, DDC 8–15)     */
  guint8       input;                 /* 0 = IN1, 1 = IN2 (the agent only)   */
  guint        rate;                  /* its IQ rate: its IP's ([radio] rate,
                                       * [rp] b_rate)                        */
  gboolean     phased;                /* ip=ab: IP A (IN1) + IP B (IN2), ip
                                       * stays 0 (A is the band's own)       */
  gint         phase_mode;            /* phase=: a SkimPhaseMode, -1 = [phase] */
  double       fixed_gain_db;         /* [phase.<band>]: mode fixed's weight  */
  double       fixed_phase_deg;       /* (NAN = [phase]'s)                     */
  double       mem[3][2];             /* m1..m3: gain dB, phase ° (NAN none)   */
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
  /* [rp] — the board's second IP (bands with ip=b) */
  gboolean         rp_split;          /* mode=extended: IN1 → IP A, IN2 → IP B */
  char            *rp_b_host;         /* NULL = the agent's discovery names it */
  guint            rp_b_rate;         /* its rate (default [radio] rate)      */
  guint            nb_ip[2];          /* receivers on each IP (a phased band
                                       * counts on both)                     */
  /* [phase] — the phased bands' combiner (docs/PHASING.md) */
  gint             phase_mode;        /* SkimPhaseMode (0 = mvdr)             */
  double           phase_tc_s;
  double           phase_gate_db;
  double           phase_fixed_gain_db;
  double           phase_fixed_phase_deg;
  guint            nb_phased;
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
  gboolean         rtty_adaptive;     /* RTTY only while a contest runs      */
  guint            rtty_adaptive_spots;   /* … more spots than this …        */
  guint            rtty_adaptive_minutes; /* … in this many minutes an hour  */
  guint            v4_workers;        /* CW v4's inference threads (0 auto)  */
  gboolean         v4_guard;          /* pause v4 bands when it is late      */
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
  char            *feed_address;      /* the telnet address the page header
                                       * shows (host:port or a telnet/http(s)
                                       * URL); NULL = none                   */
  /* [rbn] */
  gboolean         rbn_on;
  int              rbn_port;
  guint            rbn_delay_s;
  /* [station] */
  char             locator[8];        /* "JN18EU", "" = not set              */
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
  /* [archive] */
  char            *archive_dir;       /* absolute; NULL = off                */
  /* [visitors] (visitors.h) */
  gboolean         visitors_login;    /* outside free_from: sign in first    */
  char           **visitors_free;     /* NULL-terminated, never empty        */
  /* [thermal] (governor.h) */
  gboolean         thermal;
  double           thermal_limit_c;   /* 0 = the sensor's critical − 15 °C   */
  guint            thermal_average_min;
  double           thermal_margin_c;
  guint            thermal_probe_every_min;   /* 0 = no rotation              */
  guint            thermal_probe_min;
  /* [filter] */
  char            *filter_call;       /* as written; NULL = no filter        */
  gboolean         filter_regex;
  GRegex          *filter_re;         /* compiled (regex=true); else NULL    */
  char            *filter_up;         /* the text upper-cased (regex=false)  */
  gboolean         filter_mycall;     /* drop [feed] call and its near calls */
  guint            filter_mycall_diff;/* … within this many edits (0–3)      */
  char            *filter_bad;        /* bad calls as written; NULL = none   */
  char           **filter_bad_pfx;    /* … the prefixes and calls, upper     */
  char           **filter_bad_name;   /* … the country names, upper          */
  /* [rtty_palette] (the web page only) */
  struct {
    gboolean open_on_spot, autotune, afc, reverse, usos, second_decoder, clean, sound;
    guint    bandwidth, afc_max_hz, afc_rate, squelch_db;
  } rtty_palette;
  /* [wspr] (wspr.h) */
  gboolean         wspr;              /* decode WSPR beside                   */
  gboolean         wspr_upload;       /* … and post the spots to WSPRnet      */
  gboolean         wspr_deep;         /* wsprd -d: deeper candidate search    */
  gboolean         wspr_subtract;     /* wsprd without -s: subtract decoded
                                       * signals and decode again            */
  char            *wspr_call;         /* NULL = [feed] call                   */
  char             wspr_locator[8];   /* "" = [station] locator               */
  char            *wspr_upload_url;   /* NULL = wsprnet.org (the gate's own)  */
  /* [ft8] (ft8.h) */
  gboolean         ft8;               /* decode FT8/FT4 beside                */
  gboolean         ft8_ft8, ft8_ft4;  /* … on which dials                     */
  gboolean         ft8_feed;          /* the senders to the telnet feed       */
  guint            ft8_depth;         /* 1–3                                  */
  guint            ft8_lo_hz, ft8_hi_hz; /* the audio window above the dial   */
  guint            ft8_threads;       /* decoder threads                      */
  char           **ft8_bands;         /* the [bands] names decoded; NULL = all */
  gboolean         ft8_upload;        /* the stations heard to PSK Reporter   */
  char            *ft8_call;          /* NULL = [feed] call                   */
  char             ft8_locator[8];    /* "" = [station] locator               */
  char            *ft8_pskr_host;     /* NULL = report.pskreporter.info       */
  guint            ft8_pskr_port;     /* 0 = 4739                             */
  /* [js8] (js8.h) */
  gboolean         js8;               /* decode JS8 beside                    */
  guint            js8_submodes;      /* SKIM_JS8_M_* bits                    */
  gboolean         js8_feed;          /* the senders to the telnet feed       */
  guint            js8_lo_hz, js8_hi_hz; /* the audio window above the dial   */
  guint            js8_threads;       /* decoder threads                      */
  char           **js8_bands;         /* the [bands] names decoded; NULL = all */
  gboolean         js8_serve;         /* a decoder process kept a dial        */
  double           js8_clean;         /* quality from which a frame is clean  */
  gboolean         js8_upload;        /* the stations heard to PSK Reporter   */
  /* [update] (update.h) */
  guint            update_mode;       /* SkimHdUpdateMode: 0 off, 1 notify, 2 auto */
  guint            update_hour;       /* UTC hour auto installs at            */
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

/* [rtty_palette] as a JSON object, for the page (status.json). */
char *skim_hd_config_rtty_palette_json(const SkimHdConfig *cfg);

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

/* The phasing panel's Save (docs/PHASING.md): text (an ini) with band's
 * phase= set to mode (a SkimPhaseMode) in its [bands] line, and
 * [phase.<band>] fixed_gain_db, fixed_phase_deg and m1..m3 (mem[k][0] NAN
 * = none) — comments kept. NULL + error when the band has no line. */
char *skim_hd_config_set_phase(const char *text, gsize len, const char *band, gint mode,
                               double gain_db, double phase_deg, const double mem[3][2],
                               GError **error);

/* [filter] in force at all (a pattern, or the own call's filter on with a
 * call to compare with). */
gboolean skim_hd_config_filter_on(const SkimHdConfig *cfg);

/* Changes between two calls, any case, place by place (a letter other, or
 * missing at the end, counts one). */
guint skim_hd_call_distance(const char *a, const char *b);

/* The known calls (MASTER.SCP) for the own call's filter: a call a few
 * changes off it that they know is a real neighbour and passes. Set once,
 * before the pipelines run; NULL (the default) = none known. */
void skim_hd_config_set_known(gboolean (*known)(const char *call));

/* Does the call pass [filter]? (TRUE without one.) Thread-safe. */
gboolean skim_hd_config_call_pass(const SkimHdConfig *cfg, const char *call);

G_END_DECLS
