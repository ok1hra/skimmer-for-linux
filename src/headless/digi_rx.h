/* digi_rx — one dial's USB audio at 12 kHz, cut into UTC periods: the front
 * end FT8/FT4 (ft8.c) and JS8 (js8.c) share.
 *
 * The IQ of a band is shifted so the dial's window (lo_hz…hi_hz above the
 * dial) sits at 0 Hz, resampled to 12 kHz (WDSP, 240 taps), filtered sharp
 * to the window (800 taps — nothing below the dial, whose mirror USB would
 * hear), shifted back up and its real part taken: USB audio with the dial at
 * 0 Hz. The arrival clock is smoothed as wspr.c does. Each period (one or
 * more, e.g. JS8's 15/10/6/30 s from one dial) captures its own UTC cycles:
 * a capture starts no later than 0.5 s into its cycle (the late start is
 * zeros) and, once full, goes to the callback as int16 at RMS 1500 — the
 * callee owns the samples. GLib + WDSP. */
#ifndef SKIM_DIGI_RX_H
#define SKIM_DIGI_RX_H

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_DIGI_RATE        12000
#define SKIM_DIGI_MAX_PERIODS 8

typedef struct _SkimDigiRx SkimDigiRx;

/* `period` is the index into the constructor's period_ms[]. */
typedef void (*SkimDigiCycleCb)(guint period, gint64 start_ms, gint16 *pcm, guint n,
                                gpointer user);

SkimDigiRx *skim_digi_rx_new(double rate, double centre, double dial_hz, double lo_hz,
                             double hi_hz, const guint *period_ms, guint nperiods,
                             SkimDigiCycleCb cb, gpointer user);
void        skim_digi_rx_free(SkimDigiRx *rx);
/* n IQ frames (interleaved float), the last one arrived at t_end_us (wall
 * clock, µs since the epoch). */
void        skim_digi_rx_feed(SkimDigiRx *rx, const float *iq, guint n, gint64 t_end_us);

/* <dir>/YYMMDD_HHMMSS.wav (the cycle's UTC start) as 12 kHz mono PCM16;
 * NULL + error when the file cannot be written. Caller frees the path. */
char       *skim_digi_write_wav(const char *dir, gint64 start_ms, const gint16 *pcm, guint n,
                                GError **error);

G_END_DECLS

#endif
