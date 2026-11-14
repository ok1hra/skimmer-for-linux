/* snr_meter.h — a band-referenced SNR per channel (shadow measurement).
 *
 * The decoders' snr_db is env_hi/env_lo of one channel: a peak tracker
 * over the channel's own "below mid" mean. Against CW Skimmer Server on
 * 44k identical spots (2026-11-05) it is compressed — R's 3..45 dB maps
 * to our 15..30 — because noise peaks lift the bottom and keying edges
 * drag env_lo up with the signal at the top.
 *
 * This meter measures in the power domain, independent of any decoder:
 * signal = the channel's mark power (mean of the samples near its peak)
 * minus the noise; noise = a low percentile of the neighbouring channels'
 * mean power — the band's floor around the station, which a strong
 * signal's keying cannot lift. It only measures; nothing decides on it.
 *
 * The scale is SNR_2500: key-down carrier power over the noise in 2500 Hz,
 * P_key_down / (N0 · 2500 Hz) — not the mean over marks and spaces. Its
 * per-dot twin is Es/N0_dot = SNR_2500 + 10·log10(2500 · T_dot), with
 * T_dot = 1.2 / WPM s (PARIS): +21.8 dB at 20 WPM.
 *
 * Threading: single-threaded (the pipeline's engine thread).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_SNR_METER_H
#define SKIMMER_SNR_METER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _SkimSnrMeter SkimSnrMeter;

/* nchan channels in the channelizer's cyclic order (index ±k mod nchan
 * are frequency neighbours), each sampled at rate Hz, each with a noise
 * bandwidth of enbw_hz (SKIM_SNR_CW_ENBW for the CW geometry). */
SkimSnrMeter *skim_snr_meter_new(guint nchan, double rate, double enbw_hz);
void          skim_snr_meter_free(SkimSnrMeter *m);

/* One block of channel chan's complex baseband (interleaved I/Q). */
void   skim_snr_meter_push(SkimSnrMeter *m, guint chan, const float *iq,
                           guint nframes);

/* Noise bandwidth of a CW channel (skim_channelizer_new, default
 * prototype) per Hz of channel spacing: 106.4 Hz at 125 Hz, measured with
 * white noise against a centred carrier (0 dB; -0.1 dB at 25 Hz off). */
#define SKIM_SNR_CW_ENBW 0.851

/* The band floor around every channel, refreshed at once (the percentile
 * over the neighbours is too dear per block): the pipeline calls this about
 * once a second, then hands each channel's floor to its decoder (decode.h
 * set_floor). */
void   skim_snr_meter_refresh(SkimSnrMeter *m);

/* Channel chan's floor from the last refresh: the mean power of a
 * noise-only sample of the channel, linear; NAN before the first one. */
double skim_snr_meter_noise(const SkimSnrMeter *m, guint chan);

/* Channel chan's SNR_2500 in dB; NAN until the meter has seen a couple
 * of seconds. */
double skim_snr_meter_db(SkimSnrMeter *m, guint chan);

/* Es/N0_dot in dB from SNR_2500 and the keying speed (NAN without one). */
double skim_snr_esn0_dot(double snr2500_db, double wpm);

G_END_DECLS

#endif /* SKIMMER_SNR_METER_H */
