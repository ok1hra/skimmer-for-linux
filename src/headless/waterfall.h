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

typedef struct _SkimHdWaterfall SkimHdWaterfall;

/* names/centres: the bands in receiver order; rate: the IQ rate. */
SkimHdWaterfall *skim_hd_waterfall_new(guint nb, const char *const *names,
                                       const double *centres, double rate);
void             skim_hd_waterfall_free(SkimHdWaterfall *w);

/* Source thread: one IQ block (interleaved float I/Q, n frames) of band b.
 * Returns at once while nobody watches the band. */
void skim_hd_waterfall_push(SkimHdWaterfall *w, guint b, const float *iq, guint n,
                            double rate, double centre);

/* Main thread: a new viewer (the waterfall takes its own reference). */
void  skim_hd_waterfall_add(SkimHdWaterfall *w, SoupWebsocketConnection *c, const char *peer);
guint skim_hd_waterfall_viewers(SkimHdWaterfall *w);

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

G_END_DECLS
