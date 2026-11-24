/* waterfall.h — the web page's waterfall: a band's spectrum, live, to the
 * viewers that watch it (WebSocket /ws/waterfall).
 *
 * Nothing is computed for a band nobody watches: the source thread's push
 * checks one counter and returns. A watched band is windowed (4-term
 * Blackman-Harris) and transformed in FFTs of SKIM_WF_FFT bins (twice that
 * at 192 kHz: ~11.7 Hz a bin), half overlapping; the power of the last
 * frames is kept in a short ring. Each viewer asks for its own window of
 * the band (lo–hi Hz), its width in pixels and its rows a second; each row
 * averages the frames since the viewer's last one and goes out as one byte
 * a pixel (the strongest bin under it, 0.5 dB a step from -160 dBFS).
 *
 * Over the link (the remote viewer in mind):
 *   viewer → skimmer, text: "band=I&lo=HZ&hi=HZ&w=PX&rate=ROWS_PER_S" (any
 *     time; the next row follows it), "a=SEQ" (row SEQ arrived);
 *   skimmer → viewer, text once: {"type":"hello","rate":Hz,"fft":N,
 *     "bands":[{"name":…,"centre_hz":…},…]};
 *   binary, a row: u8 version (1), u8 band, u16 w, u32 seq, f64 unix time,
 *     f64 lo Hz, f64 hi Hz, then w bytes — little-endian.
 * Audio (a CW signal heard in the browser): "audio=1&af=HZ&bw=HZ" tunes
 *   the viewer's own demodulator to af with a passband of bw (50–500 Hz) —
 *   the band shifted, WDSP's resampler to 8 kHz, a second one at 8 kHz as
 *   the narrow filter, a 600 Hz BFO, an AGC — and "audio=0" stops it; the
 *   audio goes out every 100 ms as binary: u8 version (2), u8 band, u16 n,
 *   u32 seq, n bytes of G.711 µ-law at 8 kHz (8 kB/s), then f32 the
 *   passband's level in dBFS (the loudest 10 ms of the packet, before the
 *   AGC: the page's S-meter). It runs only while on;
 *   SKIM_WF_MAX_LISTENERS at once.
 * History: the band keeps a full-resolution row every 0.2 s for 4 minutes
 *   while watched; "hist=ROWS" (with a new window) asks for the picture so
 *   far at the new window and width — binary: u8 version (3), u8 band, u16
 *   w, u32 count, f64 lo, f64 hi, count × (f64 unix time, w bytes), newest
 *   first — so a zoom or a move is drawn sharp, never stretched.
 * A row goes out only while fewer than about two seconds' worth are
 * unacknowledged: a slow link gets fewer, longer-averaged rows instead of
 * a growing queue. At most SKIM_WF_MAX_VIEWERS viewers at once.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define SKIM_WF_FFT          8192     /* bins at 48 and 96 kHz (192: twice)  */
#define SKIM_WF_MAX_VIEWERS  8
#define SKIM_WF_MAX_W        2048     /* pixels a row                         */
#define SKIM_WF_DB_FLOOR     (-160.0) /* byte 0                               */
#define SKIM_WF_AUDIO_RATE   8000     /* the audio's sample rate               */
#define SKIM_WF_AUDIO_BFO    600.0    /* Hz: the tone a carrier is heard at   */
#define SKIM_WF_MAX_LISTENERS 4

typedef struct _SkimHdWaterfall SkimHdWaterfall;

/* names/centres: the bands in receiver order; rate: the IQ rate. */
SkimHdWaterfall *skim_hd_waterfall_new(guint nb, const char *const *names,
                                       const double *centres, double rate);
void             skim_hd_waterfall_free(SkimHdWaterfall *w);

/* Source thread: one IQ block (interleaved float I/Q, n frames) of band b.
 * Returns at once while nobody watches the band. */
void skim_hd_waterfall_push(SkimHdWaterfall *w, guint b, const float *iq, guint n,
                            double rate, double centre);

/* Main thread: a new viewer (the waterfall takes its own reference);
 * visitor: its sign-in token (visitors.h), NULL for a free client. */
void  skim_hd_waterfall_add(SkimHdWaterfall *w, SoupWebsocketConnection *c, const char *peer,
                            const char *visitor);

/* What a signed-in visitor's viewer watches, at each of its requests (band
 * NULL: it left) — conn tells its viewers apart. Main thread. */
typedef void (*SkimWfViewCb)(const char *visitor, gconstpointer conn, const char *band, double lo,
                             double hi, gboolean audio, double af, gpointer user);
void  skim_hd_waterfall_set_view_cb(SkimHdWaterfall *w, SkimWfViewCb cb, gpointer user);
guint skim_hd_waterfall_viewers(SkimHdWaterfall *w);
guint skim_hd_waterfall_listeners(SkimHdWaterfall *w);

/* ---- the DSP, for the gate ---------------------------------------------------- */

guint skim_hd_wf_fft_size(double rate);

typedef struct _SkimWfAnalyser SkimWfAnalyser;
typedef void (*SkimWfFrameCb)(const float *power, guint n, gpointer user);
SkimWfAnalyser *skim_wf_analyser_new(guint n);
void            skim_wf_analyser_free(SkimWfAnalyser *a);
/* Frames in; each completed FFT (hop n/2) gives its power, FFT order,
 * normalised so a full-scale complex tone on a bin is 1 (0 dBFS). */
void skim_wf_analyser_feed(SkimWfAnalyser *a, const float *iq, guint n, SkimWfFrameCb cb,
                           gpointer user);

/* Power (FFT order, n bins of a band at centre, rate) to w bytes covering
 * lo–hi Hz: the strongest bin under each pixel; 0 outside the band. */
void skim_hd_wf_render(const float *power, guint n, double rate, double centre, double lo,
                       double hi, guint w, guint8 *out);

/* A history row (n bytes, display order) to w bytes covering lo–hi: the
 * strongest under each pixel. */
void skim_hd_wf_render_bytes(const guint8 *disp, guint n, double rate, double centre,
                             double lo, double hi, guint w, guint8 *out);

/* One listener's CW demodulator (see Audio above): IQ of a band, the
 * tuned frequency's offset from its centre, the passband → µ-law at 8 kHz. */
typedef struct _SkimWfAudio SkimWfAudio;
SkimWfAudio *skim_wf_audio_new(double rate, double offset, double bw);
void         skim_wf_audio_free(SkimWfAudio *a);
void         skim_wf_audio_feed(SkimWfAudio *a, const float *iq, guint n);
GByteArray  *skim_wf_audio_take(SkimWfAudio *a);         /* the caller unrefs */
/* the passband's level, dBFS: its loudest 10 ms since the last call */
double       skim_wf_audio_level(SkimWfAudio *a);

G_END_DECLS
