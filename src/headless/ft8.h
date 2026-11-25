/* ft8.h — FT8 and FT4 beside the skimmer: the FT8/FT4 dial frequencies that
 * fall inside the bands received are decoded every cycle (15 s / 7.5 s) by
 * WSJT-X's own decoders (skimmer-ft8d, vendor/wsjtx-ft); each station that
 * transmits goes to the telnet feed like a CW spot (at its own frequency,
 * once in 10 min per band and mode), and every decode of the last 15 min
 * is kept for the web page's FT8 palette.
 *
 * How (the source thread, then a small pool of decoder threads):
 *   - a dial (USB; the signals at dial + lo … hi Hz, 200–3500 by default)
 *     is taken when that window lies within 45 % of the rate around a
 *     receiver's centre — the first receiver that has it, once;
 *   - its IQ is shifted so the window's middle sits at 0, WDSP's resampler
 *     takes it to 12 kHz (a short filter: only what folds around ±12 kHz
 *     must go), a second, sharp one (12 k → 12 k) keeps the window and
 *     nothing below the dial; moved back up and its real part taken, that
 *     is the USB audio WSJT-X decodes, the dial at 0 Hz;
 *   - each UTC cycle (FT8 :00 :15 :30 :45, FT4 every 7.5 s) is captured
 *     whole (the wall clock, the blocks' arrival smoothed as for WSPR; the
 *     start may be up to 0.5 s late — zeros pad it) and handed to the pool,
 *     which writes it as a 12 kHz 16-bit WAV and runs skimmer-ft8d on it
 *     (nice 10); a cycle still waiting when its next one is complete is
 *     skipped (counted), so a busy machine falls behind by one cycle at most;
 *   - each line is a decode (kept 15 min); its sender — the CQ caller, the
 *     second call of a standard message — is a spot when it is a valid
 *     call (no hash "<…>", no AP-assisted decode), once a band and mode in
 *     10 min (SkimHdFt8SpotCb).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define SKIM_FT8_RATE        12000      /* the decoder's input, samples/s        */
#define SKIM_FT8_MAX_SLOTS   24
#define SKIM_FT8_KEEP_S      900        /* decodes kept for the palette (15 min) */
#define SKIM_FT8_FOLD_S      600        /* a sender spotted once a band+mode in this */
#define SKIM_FT8_RECENT_S    1800       /* status: the stations of the last 30 min */

typedef enum { SKIM_FT8_MODE_FT8 = 0, SKIM_FT8_MODE_FT4 = 1 } SkimFt8Mode;

/* "FT8" / "FT4" */
const char *skim_ft8_mode_name(SkimFt8Mode m);
/* the cycle, ms: 15000 / 7500 */
guint skim_ft8_period_ms(SkimFt8Mode m);

/* ---- the frequencies ------------------------------------------------------------ */

typedef struct {
  const char *band;                     /* "20m"                                */
  SkimFt8Mode mode;
  double      dial_hz;                  /* the dial frequency (USB)             */
} SkimFt8Dial;

extern const SkimFt8Dial SKIM_FT8_DIALS[];     /* ends with { NULL, 0, 0 }       */

/* TRUE when a receiver at centre (rate) takes the dial's whole window
 * dial + lo_hz … dial + hi_hz. */
gboolean skim_ft8_covers(double centre, double rate, double dial_hz, double lo_hz, double hi_hz);

/* ---- the DSP: one dial of one receiver ------------------------------------------ */

typedef struct _SkimFt8Rx SkimFt8Rx;
/* A finished cycle: its UTC start (unix ms) and n samples of 12 kHz
 * 16-bit USB audio (the dial at 0 Hz); the callee owns pcm. */
typedef void (*SkimFt8CycleCb)(gint64 start_ms, gint16 *pcm, guint n, gpointer user);

SkimFt8Rx *skim_ft8_rx_new(double rate, double centre, double dial_hz, SkimFt8Mode mode,
                           double lo_hz, double hi_hz, SkimFt8CycleCb cb, gpointer user);
void       skim_ft8_rx_free(SkimFt8Rx *rx);
/* n frames of interleaved I/Q, the last of them at real time t_end_us. */
void       skim_ft8_rx_feed(SkimFt8Rx *rx, const float *iq, guint n, gint64 t_end_us);

/* The cycle as the WAV jt9 reads (YYMMDD_HHMMSS.wav in dir: the name
 * carries the UTC). The path, or NULL + error. */
char *skim_ft8_write_wav(const char *dir, gint64 start_ms, const gint16 *pcm, guint n,
                         GError **error);

/* ---- a decode ------------------------------------------------------------------- */

typedef struct {
  gint64 t_ms;                          /* the cycle's start, unix ms            */
  guint  slot;                          /* the dial (the caller's)               */
  int    snr;                           /* dB in 2500 Hz (WSJT-X)                */
  double dt;                            /* s                                     */
  int    df;                            /* Hz above the dial                     */
  gboolean ap;                          /* an a-priori (aN) decode               */
  gboolean doubt;                       /* … that WSJT-X marks '?'               */
  char   msg[40];                       /* the message, as WSJT-X prints it      */
  char   call[16];                      /* its sender; "" = none to spot         */
  char   grid[8];                       /* … its locator when the message has it */
  gboolean cq;                          /* … calling CQ / QRZ                    */
} SkimFt8Decode;

/* One line of skimmer-ft8d's (jt9's) stdout, "HHMMSS snr dt freq ~|+ message
 * [aN]", to a decode (t_ms and slot are the caller's; call and cq from
 * skim_ft8_sender). FALSE for anything else (<DecodeFinished>, noise). */
gboolean skim_ft8_parse_line(const char *line, SkimFt8Decode *out);

/* The station that sent msg (a 77-bit FT8/FT4 message as WSJT-X unpacks
 * it): the caller of "CQ [DX|POTA|NA|…] CALL [GRID]", "QRZ CALL", "DE CALL",
 * the second call of "CALL1 CALL2 …" (also "TU; CALL1 CALL2 …"). FALSE when
 * there is none or it is no valid call: free text, telemetry, a hash
 * "<…>" (even a resolved one). *cq = a CQ/QRZ call. */
gboolean skim_ft8_sender(const char *msg, char *call, gsize call_size, gboolean *cq);

/* The sender's 4-character locator when msg carries one: "CQ … CALL GRID",
 * "CALL1 CALL2 GRID" (never RR73); "" otherwise. grid: 8 bytes. */
void skim_ft8_sender_grid(const char *msg, char grid[8]);

/* ---- the whole: every receiver's dials, the decoders, the spots ------------------ */

/* A station to spot (a decoder thread): mode "FT8"/"FT4", its RF, its
 * locator ("" unknown), the cycle's start (unix s). */
typedef void (*SkimHdFt8SpotCb)(const char *call, const char *mode, double freq_hz, int snr_db,
                                gboolean cq, const char *band, const char *grid, gint64 t,
                                gpointer user);

typedef struct {
  gboolean    ft8, ft4;                 /* which dials                           */
  guint       depth;                    /* 1–3 (WSJT-X's Fast / Normal / Deep)   */
  double      lo_hz, hi_hz;             /* the audio window above the dial       */
  guint       threads;                  /* decoder threads (≥ 1)                 */
  const char *const *bands;             /* the receivers (names) decoded; NULL = all */
  const char *dir;                      /* state: ft8/ (the decoder's work dir)  */
  const char *decoder;                  /* NULL = skim_ft8_find_decoder()        */
  const char *keep_wav;                 /* a dir: every cycle's WAV is left there
                                         * (<dir>/<band>-<mode>/YYMMDD_HHMMSS.wav,
                                         * the bench against WSJT-X's jt9); NULL  */
  SkimHdFt8SpotCb spot;                 /* NULL = no spots                       */
  gpointer    user;
} SkimHdFt8Config;

typedef struct _SkimHdFt8 SkimHdFt8;

/* The bands in receiver order (names, centres) at rate. Never NULL: with no
 * dial covered or no decoder it idles and says why (skim_hd_ft8_status_json). */
SkimHdFt8 *skim_hd_ft8_new(const SkimHdFt8Config *cfg, guint nb, const char *const *names,
                           const double *centres, const double *rates);
void       skim_hd_ft8_free(SkimHdFt8 *f);

/* Source thread: one IQ block of band b (returns at once without a dial). */
void skim_hd_ft8_push(SkimHdFt8 *f, guint b, const float *iq, guint n);
/* … the last frame at real time t_end_us (the gate's own clock). */
void skim_hd_ft8_push_at(SkimHdFt8 *f, guint b, const float *iq, guint n, gint64 t_end_us);

/* What a receiver's FT8/FT4 dials did since the last call (the minute's
 * history, stats.h): spots sent, senders that appeared (none from that
 * receiver in the 10 min before), distinct senders heard, the median SNR of
 * every decode (NaN: none). FALSE (and nothing counted) when the receiver
 * has no FT8/FT4 dial. Any thread; the counts start again. */
typedef struct {
  guint spots, appeared, heard;
  float snr;
} SkimHdFt8Minute;
gboolean skim_hd_ft8_minute(SkimHdFt8 *f, guint b, SkimHdFt8Minute *out);

/* Wait until every cycle handed over so far is decoded (the gate). */
void skim_hd_ft8_drain(SkimHdFt8 *f);

/* The dials taken, "40m FT8 7074.0, 40m FT4 7047.5" (for the log; "" = none). */
char *skim_hd_ft8_dials_text(SkimHdFt8 *f);

/* {"on":true,"decoder":…,"ft8":…,"ft4":…,"depth":…,"lo_hz":…,"hi_hz":…,
 *  "dials":[{"band","mode","dial_hz","rx","lo_hz","hi_hz"}…],
 *  "rx":[{"band","stations","active_ft8","active_ft4","last"}…],"cycles":…,"decodes":…,"last_cycle":…,
 *  "last_decodes":…,"skipped":…,"spots":…,"error":…} — any thread. */
char *skim_hd_ft8_status_json(SkimHdFt8 *f);

/* The decodes since since_ms (unix ms) of the dial dial_hz (0 = every
 * dial), oldest first: {"now":ms,"decodes":[{"t","dial","mode","band","snr",
 * "dt","df","msg","call","cq","ap"}…]} — any thread. */
char *skim_hd_ft8_decodes_json(SkimHdFt8 *f, double dial_hz, gint64 since_ms);

/* The helper next to this program (or the build tree's), else on PATH.
 * NULL = none. */
char *skim_ft8_find_decoder(void);

/* Run the decoder on a WAV (work dir dir) and parse what it prints;
 * decodes get t_ms and slot from the caller. FALSE + error when it could
 * not run or failed. */
gboolean skim_ft8_decode_file(const char *decoder, const char *dir, const char *wav,
                              SkimFt8Mode mode, guint depth, double lo_hz, double hi_hz,
                              gint64 t_ms, guint slot, GArray *decodes, GCancellable *cancel,
                              GError **error);

G_END_DECLS
