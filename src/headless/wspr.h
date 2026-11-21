/* wspr.h — WSPR beside the skimmer: the WSPR-2 frequencies that fall inside
 * the bands received are decoded every 2 minutes, the stations heard are
 * kept for 24 h (the page's Statistics) and, if asked for, sent to WSPRnet.
 *
 * How (the source thread, then one decoder thread):
 *   - a WSPR frequency (dial, USB; the signals at dial + 1400–1600 Hz) is
 *     taken when dial + 1500 ± 150 Hz lies within 45 % of the rate around
 *     a receiver's centre — the first receiver that has it, once;
 *   - its IQ is shifted so dial + 1500 Hz sits at 0, WDSP's resampler takes
 *     it to 12 kHz, a second one to 375 Hz (flat to ±150 Hz, the stopband
 *     from ~225 Hz) — wsprd's own .c2 input;
 *   - each even UTC minute starts a capture of 114 s (the wall clock, the
 *     blocks' arrival smoothed; the start may be up to 0.5 s late — zeros
 *     pad it); the cycle goes to the decoder thread, which writes it as a
 *     .c2 file and runs the vendored wsprd (skimmer-wsprd, vendor/wsprd/:
 *     "-C 500" as WSJT-X does, nice 10) on it — one band after the other;
 *   - its decodes are the spots: kept 24 h in memory and in
 *     <state>/wspr.tsv, and with upload on, posted to WSPRnet one by one
 *     (function=wspr, as WSJT-X does) from the same thread; a refused or
 *     unreachable server keeps them for the next cycle (24 h at most).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SKIM_WSPR_RATE        375       /* the decoder's input, samples/s        */
#define SKIM_WSPR_C2_N        45000     /* a .c2 file's samples (120 s)          */
#define SKIM_WSPR_CAPTURE_N   42750     /* … of them captured (114 s)            */
#define SKIM_WSPR_BFO         1500.0    /* Hz above the dial: the band's centre  */
#define SKIM_WSPR_HALF_SPAN   150.0     /* Hz each side of it (wsprd -w)         */
#define SKIM_WSPR_URL         "http://wsprnet.org/post/"
#define SKIM_WSPR_MAX_SLOTS   16

/* ---- the frequencies ------------------------------------------------------------ */

typedef struct {
  const char *band;                     /* "20m"                                */
  double      dial_hz;                  /* the WSPR-2 dial frequency (USB)      */
} SkimWsprDial;

extern const SkimWsprDial SKIM_WSPR_DIALS[];   /* ends with { NULL, 0 }          */

/* TRUE when a receiver at centre (rate) takes the dial's whole window. */
gboolean skim_wspr_covers(double centre, double rate, double dial_hz);

/* ---- the DSP: one dial of one receiver ------------------------------------------ */

typedef struct _SkimWsprRx SkimWsprRx;
/* A finished capture: the even minute it began (unix s) and
 * SKIM_WSPR_C2_N complex samples at 375 Hz (I, Q interleaved; true
 * orientation, centred on dial + 1500 Hz), the callee owns them. */
typedef void (*SkimWsprCycleCb)(gint64 start_s, float *iq, gpointer user);

SkimWsprRx *skim_wspr_rx_new(double rate, double centre, double dial_hz, SkimWsprCycleCb cb,
                             gpointer user);
void        skim_wspr_rx_free(SkimWsprRx *rx);
/* n frames of interleaved I/Q, the last of them at real time t_end_us. */
void        skim_wspr_rx_feed(SkimWsprRx *rx, const float *iq, guint n, gint64 t_end_us);

/* The cycle as wsprd's .c2 file (YYMMDD_HHMM.c2 in dir: the name carries
 * the date and time). The path, or NULL + error. */
char *skim_wspr_write_c2(const char *dir, gint64 start_s, double dial_hz, const float *iq,
                         GError **error);

/* ---- a decode ------------------------------------------------------------------- */

typedef struct {
  gint64 t;                             /* the cycle's start, unix s             */
  char   band[16];
  double dial_hz, freq_hz;              /* the dial; the signal heard (RF)       */
  int    snr;                           /* dB in 2500 Hz (wsprd)                 */
  double dt;                            /* s                                     */
  int    drift;                         /* Hz a minute                           */
  char   call[16];
  char   grid[8];                       /* "" = none sent (type 2)               */
  int    dbm;
} SkimWsprSpot;

/* One line of wsprd's stdout ("HHMM snr dt MHz drift message") to a spot
 * (t, band, dial_hz are the caller's). FALSE for anything else — the end
 * marker, an unresolved hash "<...>". */
gboolean skim_wspr_parse_line(const char *line, SkimWsprSpot *out);

/* The WSPRnet form body (application/x-www-form-urlencoded) for a spot. */
char *skim_wspr_post_body(const SkimWsprSpot *s, const char *rcall, const char *rgrid,
                          const char *version);

/* ---- the whole: every receiver's dials, the decoder, the stations heard ---------- */

typedef struct {
  const char *call;                     /* the receiver's call (WSPRnet rcall)  */
  const char *locator;                  /* its locator (rgrid)                  */
  gboolean    upload;                   /* post the spots to WSPRnet            */
  const char *upload_url;               /* NULL = SKIM_WSPR_URL                 */
  const char *dir;                      /* state: wspr.tsv, wspr/ (the decoder's
                                         * work dir and its hash table)          */
  const char *decoder;                  /* NULL = skim_wspr_find_decoder()      */
} SkimHdWsprConfig;

typedef struct _SkimHdWspr SkimHdWspr;

/* The bands in receiver order (names, centres) at rate. Never NULL: with no
 * dial covered or no decoder it idles and says why (skim_hd_wspr_status_json). */
SkimHdWspr *skim_hd_wspr_new(const SkimHdWsprConfig *cfg, guint nb, const char *const *names,
                             const double *centres, double rate);
void        skim_hd_wspr_free(SkimHdWspr *w);

/* Source thread: one IQ block of band b (returns at once without a dial). */
void skim_hd_wspr_push(SkimHdWspr *w, guint b, const float *iq, guint n);
/* … the last frame at real time t_end_us (the gate's own clock). */
void skim_hd_wspr_push_at(SkimHdWspr *w, guint b, const float *iq, guint n, gint64 t_end_us);

/* The dials taken, "40m 7038.6, 30m 10138.7" (for the log; "" = none). */
char *skim_hd_wspr_dials_text(SkimHdWspr *w);

/* {"on":true,"upload":…,"call":…,"locator":…,"decoder":…,"dials":[…],
 *  "cycles":…,"decodes":…,"uploaded":…,"queued":…,"error":…} — any thread. */
char *skim_hd_wspr_status_json(SkimHdWspr *w);

/* The stations heard in the last hours (any thread): {"hours":H,"spots":N,
 * "stations":[{"call","grid","dbm","bands","n","snr","best","last","freq",
 * "drift"}…]} newest first. now: unix s. */
char *skim_hd_wspr_stations_json(SkimHdWspr *w, guint hours, gint64 now);

/* The helper next to this program (or the build tree's), else on PATH
 * (skimmer-wsprd, then WSJT-X's wsprd). NULL = none. */
char *skim_wspr_find_decoder(void);

/* Run the decoder on a .c2 file (work dir dir) and parse what it prints;
 * spots get t, band and dial_hz from the caller. FALSE + error when it
 * could not run or failed. */
gboolean skim_wspr_decode_file(const char *decoder, const char *dir, const char *c2,
                               gint64 t, const char *band, double dial_hz, GArray *spots,
                               GCancellable *cancel, GError **error);

G_END_DECLS
