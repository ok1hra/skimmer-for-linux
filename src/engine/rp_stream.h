/* rp_stream.h — SKRS: the skimmer's own stream from its Red Pitaya agent.
 *
 * On the skimmer's own bitstream (docs/FPGA.md) the agent (rp/agent,
 * 0.6 on) serves Protocol 1 as before AND, on the port its discovery names
 * (SkimHpsdrInfo.agent_skrs_port, 1025), SKRS: up to every DDC slot of the
 * board, each with its own frequency, rate (48/96/192/384 kHz) and input —
 * what P1 cannot (one rate an IP, 8 receivers). One datagram carries one
 * stream's samples with the epoch (the board's syncs) and the HARDWARE
 * index of its first sample: two streams at one rate share that timeline,
 * so one index is one instant on both (a phased pair, docs/PHASING.md).
 * The wire is RF-inverted as P1's; this client conjugates, so the IQ it
 * hands on is in true orientation. Receive-only: it never keys.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_RP_STREAM_H
#define SKIMMER_RP_STREAM_H

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_RP_STREAM_MAX 32

#define SKIM_RP_STREAM_ERROR (g_quark_from_static_string("skim-rp-stream-error"))
typedef enum {
  SKIM_RP_STREAM_ERROR_FAILED = 0,   /* socket / resolve / bad argument        */
  SKIM_RP_STREAM_ERROR_NO_REPLY,     /* no answer to the start                 */
  SKIM_RP_STREAM_ERROR_BUSY,         /* refused: no free slots, another client */
  SKIM_RP_STREAM_ERROR_NO_IQ,        /* started, but a stream did not come     */
} SkimRpStreamError;

typedef struct {
  double centre_hz;      /* TRUE centre (the clock correction is applied)  */
  guint  rate;           /* 48000, 96000, 192000 or 384000                 */
  guint  input;          /* 1 = IN1, 2 = IN2                               */
} SkimRpStreamSpec;

typedef struct _SkimRpStream SkimRpStream;

/* Stream id's IQ: nframes interleaved I/Q in TRUE orientation, the block's
 * first frame at hardware index idx of epoch; within an epoch the frames
 * are contiguous (a lost datagram zero-filled by the index). Fires on the
 * client's receive thread. */
typedef void (*SkimRpStreamCb)(guint id, const float *iq, guint nframes, double rate,
                               double centre_hz, guint32 epoch, guint64 idx, gpointer user);
/* The stream stopped arriving (2 s without a datagram). Once, on the
 * receive thread; not on skim_rp_stream_stop(). */
typedef void (*SkimRpStreamClosedCb)(gpointer user);

SkimRpStream *skim_rp_stream_new(const char *host, guint16 port);
void          skim_rp_stream_free(SkimRpStream *s);
void          skim_rp_stream_set_cb(SkimRpStream *s, SkimRpStreamCb cb, gpointer user);
void          skim_rp_stream_set_closed_cb(SkimRpStream *s, SkimRpStreamClosedCb cb, gpointer user);

/* Start n streams (id = their order in specs); the frequency word sent is
 * centre / (1 + clock_ppm·1e-6), as the P1 client's. Returns once the
 * agent acknowledged and every stream's first datagram is in (≤ 2 s), or
 * FALSE (the board refused: not enough free slots, another SKRS client). */
gboolean skim_rp_stream_start(SkimRpStream *s, const SkimRpStreamSpec *specs, guint n,
                              double clock_ppm, GError **error);
/* Stop (SKRC stop) and join the receive thread. Idempotent. */
void     skim_rp_stream_stop(SkimRpStream *s);

guint64  skim_rp_stream_datagrams(SkimRpStream *s, guint id);
guint64  skim_rp_stream_lost_frames(SkimRpStream *s, guint id);
/* The board's ADC clipped (IN1 bit 0, IN2 bit 1) since its last sync. */
guint    skim_rp_stream_clips(SkimRpStream *s);
const char *skim_rp_stream_device(SkimRpStream *s);   /* "Red Pitaya 192.168.1.21 (SKRS)" */

G_END_DECLS

#endif /* SKIMMER_RP_STREAM_H */
