/* config.h — skimmer-headless's settings (headless.ini), parsed and checked.
 *
 * One parser for every consumer: the start reads the file with it, and a
 * settings page validates a new ini with the same call before it is written,
 * so the file a restart reads has already been through the start's checks.
 *
 *   [radio]  type    hpsdr-p1 (default) | mock (synthetic CW, for the gates)
 *            host    one radio or a list (comma separated, tried in order),
 *                    each optionally host@ppm: that radio's own clock error
 *            rate    48000 / 96000 / 192000 — one rate for all receivers
 *            clock_ppm  for the radios without @ppm
 *   [bands]  <name>=<centre Hz> [modes], in receiver order (RX1 first);
 *            modes a comma list of cw, cw:v2, cw:deepcw, rtty — none = cw
 *            with [decode] engine
 *   [decode] engine=v2|deepcw (the bands that name none), log=true|false
 *   [feed]   call, port (0 = off), min_score, min_hearings, settle_s,
 *            settle_nodict_s, fresh_s, fold_s, learn, gate, gate_mode
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

typedef enum {
  SKIM_HD_SOURCE_HPSDR_P1 = 0,
  SKIM_HD_SOURCE_MOCK,
} SkimHdSourceType;

typedef struct {
  char         name[16];
  double       centre_hz;
  gboolean     cw;                    /* decode CW on this band              */
  SkimCwEngine cw_engine;             /* … with this engine                  */
  gboolean     rtty;                  /* decode RTTY beside it               */
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
  /* [bands] */
  SkimHdBand       band[SKIM_HD_MAX_BANDS];
  guint            nb;
  /* [decode] */
  SkimCwEngine     engine;            /* default for bands that name none    */
  gboolean         decode_log;
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
  char            *feed_gate;         /* absolute path; NULL = built in      */
  char            *feed_gate_mode;    /* as written; NULL = not set          */
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

/* Write the commented default ini to path (its directory is created). */
gboolean skim_hd_config_write_default(const char *path, GError **error);

/* Parse an ini held in memory; path names it in messages and anchors its
 * relative paths. FALSE + error (and cfg cleared) on a bad host, a band that
 * is no centre in Hz or names an unknown mode, more than SKIM_HD_MAX_BANDS
 * bands, no band at all, a rate other than 48/96/192 kHz or an unknown
 * radio type. */
gboolean skim_hd_config_parse(SkimHdConfig *cfg, const char *text, gsize len,
                              const char *path, GError **error);

/* The same from the file. */
gboolean skim_hd_config_load(SkimHdConfig *cfg, const char *path, GError **error);

void skim_hd_config_clear(SkimHdConfig *cfg);

/* "cw", "cw:deepcw,rtty", … — the band's modes as the ini writes them. */
char *skim_hd_band_modes(const SkimHdBand *b, SkimCwEngine dflt);

G_END_DECLS
