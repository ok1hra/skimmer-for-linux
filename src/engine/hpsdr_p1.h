/* hpsdr_p1.h — HPSDR Protocol 1 (Metis/Hermes) receive-only IQ client.
 *
 * Pulls ONE receiver's IQ straight from a Protocol 1 radio — written for, and
 * verified against, Pavel Demin's Red Pitaya `sdr_receiver_hpsdr` server
 * (discovery name "R_PITAYA"; the same wire Quisk's "Red Pitaya" profile and
 * CW Skimmer's HermesIntf speak). The IQ source used when there is no TCI
 * server in front of the receiver (docs/HPSDR-P1.md).
 *
 * The receiver belongs to the skimmer: the client sets the DDC centre, the
 * sample rate and the receiver count itself. It never keys (no TX frames),
 * and once its stream is gone (another client took the radio over) it goes
 * silent — no control refresh, no stop — because the server accepts both
 * from anyone and would retune/stop the new owner's stream.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_HPSDR_P1_H
#define SKIMMER_HPSDR_P1_H

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HPSDR_DEFAULT_PORT 1024
#define SKIM_HPSDR_MAX_RX       8     /* sdr_receiver_hpsdr's receiver count */

#define SKIM_HPSDR_ERROR (g_quark_from_static_string("skim-hpsdr-error"))
typedef enum {
  SKIM_HPSDR_ERROR_FAILED = 0,   /* socket / resolve / bad argument           */
  SKIM_HPSDR_ERROR_NO_REPLY,     /* discovery unanswered                      */
  SKIM_HPSDR_ERROR_BUSY,         /* the radio streams to another client       */
  SKIM_HPSDR_ERROR_NO_IQ,        /* started, but no IQ packet arrived         */
} SkimHpsdrError;

/* Discovery reply (EF FE 02/03 …). busy = the server is streaming to SOME
 * client — Protocol 1 does not say which one (it may be our own crashed
 * session). nrx is byte 0x13 (Red Pitaya / HL2 fill it; 0 elsewhere). */
typedef struct {
  gboolean busy;
  guint8   mac[6];
  guint8   code_version;
  guint8   board_id;
  guint    nrx;
  char     name[9];              /* "R_PITAYA" (bytes 0x0B..0x12), NUL-ended  */
} SkimHpsdrInfo;

/* Unicast discovery to host:port; blocks up to timeout_ms. */
gboolean skim_hpsdr_discover(const char *host, guint16 port, guint timeout_ms,
                             SkimHpsdrInfo *out, GError **error);

typedef struct _SkimHpsdrClient SkimHpsdrClient;

/* Same shape as SkimTciIqCb: nframes interleaved I/Q pairs in TRUE spectrum
 * orientation, blocks of 2048 frames. Fires on the client's receive thread. */
typedef void (*SkimHpsdrIqCb)(const float *iq, guint nframes,
                              double sample_rate, double center_hz,
                              gpointer user_data);

/* The stream stopped arriving (2 s without a packet — another client took
 * the radio, or it went off the network). Fires once, on the receive
 * thread; not on skim_hpsdr_client_stop(). */
typedef void (*SkimHpsdrClosedCb)(gpointer user_data);

SkimHpsdrClient *skim_hpsdr_client_new(const char *host, guint16 port);
void             skim_hpsdr_client_free(SkimHpsdrClient *c);

/* RX1's IQ (= set_rx_iq_cb(c, 0, …)). */
void skim_hpsdr_client_set_iq_cb(SkimHpsdrClient *c, SkimHpsdrIqCb cb, gpointer user_data);
/* Receiver rx's IQ (0-based, < the nrx given to start_multi). */
void skim_hpsdr_client_set_rx_iq_cb(SkimHpsdrClient *c, guint rx, SkimHpsdrIqCb cb,
                                    gpointer user_data);
void skim_hpsdr_client_set_closed_cb(SkimHpsdrClient *c, SkimHpsdrClosedCb cb, gpointer user_data);

/* Discover, configure (rate, one receiver, DDC centre) and start the stream;
 * returns once the first IQ packet is in (≤ 2 s). rate ∈ {48000, 96000,
 * 192000} (0 = 192000). center_hz is the TRUE centre: the frequency word
 * sent is center_hz / (1 + clock_ppm·1e-6), so a sampling clock that runs
 * clock_ppm fast still lands the DDC on center_hz. A busy radio fails with
 * SKIM_HPSDR_ERROR_BUSY unless take_over. */
gboolean skim_hpsdr_client_start(SkimHpsdrClient *c, guint rate, double center_hz,
                                 double clock_ppm, gboolean take_over,
                                 GError **error);
/* Several receivers at once (multi-band skimming): nrx ≤ 8 and ≤ what the
 * radio's discovery reports, each at centers_hz[rx], all at one rate — the
 * server has a single rate for every receiver. The link carries
 * nrx × rate × 6 bytes/s plus framing (6 × 96 kHz ≈ 30 Mb/s): wired. */
gboolean skim_hpsdr_client_start_multi(SkimHpsdrClient *c, guint rate,
                                       const double *centers_hz, guint nrx,
                                       double clock_ppm, gboolean take_over,
                                       GError **error);
/* Stop the stream (EF FE 04 00 — only while it is still ours) and join the
 * receive thread. Idempotent. */
void     skim_hpsdr_client_stop(SkimHpsdrClient *c);

const char *skim_hpsdr_client_device(SkimHpsdrClient *c);   /* "Red Pitaya 192.168.1.21 (HPSDR P1)" */
double      skim_hpsdr_client_center_hz(SkimHpsdrClient *c);
guint       skim_hpsdr_client_rate(SkimHpsdrClient *c);
guint32     skim_hpsdr_client_freq_word(SkimHpsdrClient *c); /* RX1: Hz actually sent */
guint32     skim_hpsdr_client_rx_freq_word(SkimHpsdrClient *c, guint rx);
guint       skim_hpsdr_client_nrx(SkimHpsdrClient *c);
guint64     skim_hpsdr_client_packets(SkimHpsdrClient *c);
/* Frames zero-filled for sequence gaps (lost UDP packets). */
guint64     skim_hpsdr_client_lost_frames(SkimHpsdrClient *c);

G_END_DECLS

#endif /* SKIMMER_HPSDR_P1_H */
