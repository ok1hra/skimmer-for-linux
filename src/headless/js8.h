/* js8.h — JS8 beside the skimmer ([js8]).
 *
 * Every JS8 dial (JS8Call's frequency list) that a received band holds is
 * decoded in all JS8 submodes at once — normal (15 s), fast (10 s), turbo
 * (6 s), slow (30 s) — from one front end (digi_rx.c: the dial's 12 kHz USB
 * audio cut into each submode's UTC periods). Each period goes as a WAV to
 * skimmer-js8d (JS8Call-improved's decoder, vendor/js8call): one process a
 * dial kept running (--serve), so the decoder's soft combining sees the
 * dial's earlier periods, or one run a period. A frame that names its
 * sender (heartbeat/CQ, compound, directed — js8_unpack.h) is a spot, once
 * a call and band in 10 min, at dial + its audio frequency.
 *
 * JS8 carries only a 12-bit CRC, so a frame is "deep" when the decoder
 * read it with many hard errors (quality = 1 − errors/60 under
 * clean_quality). A deep frame's call goes to the page (spot log, stats,
 * palette) at once but to the telnet feed only once it is confirmed: a
 * second decode of it in another period within 30 min, MASTER.SCP, or the
 * calls confirmed on JS8 in the last 90 days (<dir>/js8-known.tsv). A clean
 * frame's call is confirmed by itself.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <gio/gio.h>

#include "js8_unpack.h"

G_BEGIN_DECLS

#define SKIM_JS8_RATE        12000
#define SKIM_JS8_MAX_SLOTS   16         /* dials                                 */
#define SKIM_JS8_NSUB        4          /* normal, fast, turbo, slow             */
#define SKIM_JS8_KEEP_S      900        /* frames kept for the page (15 min)     */
#define SKIM_JS8_FOLD_S      600        /* a call+band spotted once in 10 min    */
#define SKIM_JS8_RECENT_S    1800       /* status: stations of the last 30 min   */
#define SKIM_JS8_CONFIRM_S   1800       /* a deep call's second decode within    */
#define SKIM_JS8_KNOWN_DAYS  90         /* js8-known.tsv keeps a call this long  */

/* The submodes by index 0..3 (the config's bits) and JS8's own number
 * (0 normal, 1 fast, 2 turbo, 4 slow — skimmer-js8d's -m). */
#define SKIM_JS8_M_NORMAL (1u << 0)
#define SKIM_JS8_M_FAST   (1u << 1)
#define SKIM_JS8_M_TURBO  (1u << 2)
#define SKIM_JS8_M_SLOW   (1u << 3)
#define SKIM_JS8_M_ALL    0xfu
int         skim_js8_sub_number(guint index);          /* index → JS8's number */
int         skim_js8_sub_index(int number);            /* -1: not one of the 4  */
const char *skim_js8_sub_name(guint index);            /* "normal" …            */
guint       skim_js8_sub_period_ms(guint index);
/* "normal,fast,turbo,slow" → bits; FALSE on an unknown word or none. */
gboolean    skim_js8_parse_submodes(const char *text, guint *bits);

typedef struct {
  const char *band;
  double      dial_hz;
} SkimJs8Dial;

/* JS8Call's default dials, NULL-terminated. */
extern const SkimJs8Dial SKIM_JS8_DIALS[];

/* The dial's window (dial + lo…hi) lies inside 45 % of the rate around
 * the receiver's centre. */
gboolean skim_js8_covers(double centre, double rate, double dial_hz, double lo_hz, double hi_hz);

typedef struct {
  gint64   t_ms;                       /* the period's UTC start                */
  guint    slot;                       /* the dial (skim_hd_js8's)              */
  guint    sub;                        /* submode index 0..3                    */
  int      snr;                        /* the decoder's, dB in 2500 Hz          */
  double   dt;                         /* s after the nominal start             */
  double   df;                         /* audio frequency (tone 0), Hz          */
  int      type;                       /* transmission bits (1 first, 2 last, 4 fast) */
  float    quality;                    /* 1 − hard errors / 60                  */
  char     raw[13];                    /* the frame, 12 alphabet72 characters   */
  SkimJs8Frame f;                      /* what it says                          */
  char     call[20];                   /* its sender, valid, or ""              */
  gboolean deep;                       /* quality < clean_quality               */
  gboolean confirmed;                  /* the call may go to the feed           */
} SkimJs8Decode;

/* One line of skimmer-js8d ("HHMMSS SUB SNR DT FREQ TYPE QUALITY RAW"):
 * fills snr/dt/df/type/quality/raw/sub and f (unpacked); FALSE for
 * anything else (<DecodeFinished>, junk, a submode not of the 4). */
gboolean skim_js8_parse_line(const char *line, SkimJs8Decode *out);

/* The sender of a frame as a spot names it: a valid callsign (portable
 * designators kept) — not "<....>", not a group (@…). */
gboolean skim_js8_sender(const SkimJs8Frame *f, char *call, gsize size);

/* A spot: `confirmed` FALSE = the page only (a deep call not confirmed
 * yet); `cq` = heartbeat/CQ, else DE. Runs on a decoder thread. */
typedef void (*SkimHdJs8SpotCb)(const char *call, double freq_hz, int snr_db, gboolean cq,
                                const char *band, const char *grid, gint64 t,
                                gboolean confirmed, gpointer user);

typedef struct {
  guint       submodes;                /* SKIM_JS8_M_* bits                     */
  double      lo_hz, hi_hz;            /* the window above each dial            */
  guint       threads;                 /* decoder threads                       */
  gboolean    serve;                   /* a helper kept running a dial          */
  double      clean_quality;           /* quality ≥ this = a clean frame        */
  const char *const *bands;            /* [js8] bands: NULL = all received      */
  const char *dir;                     /* state dir; work = <dir>/js8           */
  const char *decoder;                 /* NULL = skim_js8_find_decoder()        */
  const char *keep_wav;                /* <dir>/<band>-<sub>/…wav, bench only   */
  gboolean    no_known;                /* no js8-known.tsv (the gates)          */
  SkimHdJs8SpotCb spot;
  gpointer    user;
} SkimHdJs8Config;

typedef struct _SkimHdJs8 SkimHdJs8;

/* names[]/centres[] of the nb receivers at `rate`. */
SkimHdJs8 *skim_hd_js8_new(const SkimHdJs8Config *cfg, guint nb, const char *const *names,
                           const double *centres, double rate);
void       skim_hd_js8_free(SkimHdJs8 *j);
void       skim_hd_js8_push(SkimHdJs8 *j, guint b, const float *iq, guint n);
void       skim_hd_js8_push_at(SkimHdJs8 *j, guint b, const float *iq, guint n, gint64 t_end_us);
/* Waits until every handed-over period is decoded (the bench, the gates). */
void       skim_hd_js8_drain(SkimHdJs8 *j);

/* The minute's history of receiver b, reset: spots, senders that
 * appeared (none from it in 10 min), senders heard, median SNR (NaN when
 * nothing). FALSE when b decodes no JS8. */
typedef struct {
  guint spots, appeared, heard;
  float snr;
} SkimHdJs8Minute;
gboolean   skim_hd_js8_minute(SkimHdJs8 *j, guint b, SkimHdJs8Minute *out);

char      *skim_hd_js8_dials_text(SkimHdJs8 *j);
char      *skim_hd_js8_status_json(SkimHdJs8 *j);
/* The frames of one dial (0 = all) since since_ms, for the palette:
 * {"now":ms,"frames":[{t,dial,band,sub,snr,dt,df,type,q,raw,call,deep,ok,text}…]}. */
char      *skim_hd_js8_frames_json(SkimHdJs8 *j, double dial_hz, gint64 since_ms);

/* The kept-running decoders' CPU now (clock ticks, /proc) — the part of
 * the helpers' CPU the children's times do not hold yet. */
guint64    skim_hd_js8_helper_ticks(SkimHdJs8 *j);

char      *skim_js8_find_decoder(void);

G_END_DECLS
