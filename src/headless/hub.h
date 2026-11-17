/* hub.h — a hub passes bands of its radio to workers on other PCs.
 *
 * A Red Pitaya streams to ONE client, so when several PCs share its
 * decoding, one of them — the hub — receives every band and passes the IQ
 * of the bands it does not decode itself ("@worker" in its [bands]) to the
 * worker named. The worker decodes them and sends its spots back; the hub
 * sends them on its own feed (and the RBN port) as its own, so the station
 * has one point to connect to. docs/HUB.md has the whole story.
 *
 * Protocol (TCP, SKIM_HD_HUB_PORT): the worker says
 *   "SKIMHUB 1 HELLO name=<name> version=<v>\n"
 * and the hub answers
 *   "SKIMHUB 1 CONFIG <bytes>\n" + an ini fragment (the bands assigned to
 *       that name, the rate, [decode], the feed policy —
 *       skim_hd_config_worker_fragment), then IQ frames, or
 *   "SKIMHUB 1 NONE\n" (nothing assigned: the link stays, idle — the hub
 *       lists the worker as available), or
 *   "SKIMHUB 1 REFUSED <why>\n" and closes.
 * An IQ frame: a 32-byte header (little endian: u32 magic "SKIQ", u8 band —
 * the index in the fragment's [bands], u8 flags, u16 0, u32 frames, u32
 * rate, u64 seq — the band's frame count before this block, dropped ones
 * included, f64 centre Hz), then frames × (I, Q) as 24-bit little-endian
 * integers: the Red Pitaya's own samples, lossless (a float block that came
 * off the wire is n / 2^23 exactly).
 * The worker sends lines: "SPOT <call> <mode> <Hz> <snr dB> <speed>",
 * "STAT <band> <cw|rtty> <decoders> <active> <stations> <spots>" and
 * "STATUS <its /status.json>".
 *
 * A worker that cannot keep up loses blocks, never the radio: each link has
 * a queue of WORKER_QUEUE_S seconds; what does not fit is dropped and
 * counted (the seq tells the worker). Only addresses in allow may connect.
 *
 * Threading: a thread per worker plus the accept thread; push() from the
 * source's thread, json() and the rest from the main loop.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HD_HUB_PROTO    "SKIMHUB 1"
#define SKIM_HD_HUB_MAGIC    0x51494B53u      /* "SKIQ" little endian        */
#define SKIM_HD_HUB_HDR      32
#define SKIM_HD_HUB_FULLSCALE 8388608.0f      /* 2^23: int24 ↔ float          */

typedef struct _SkimHdHub SkimHdHub;

/* The fragment for worker (newly allocated), NULL = nothing assigned. */
typedef char *(*SkimHdHubAssign)(const char *worker, gpointer user);
/* A worker's spot (its link's thread). */
typedef void (*SkimHdHubSpot)(const char *worker, const char *call, const char *mode,
                              double freq_hz, double snr_db, double speed, gpointer user);

/* Listen on port (0 = an ephemeral one, for the gate). band_worker: per hub
 * band, the worker name that decodes it or "" (nb entries). */
SkimHdHub *skim_hd_hub_new(guint16 port, const char *const *allow,
                           const char *const *band_worker, guint nb, SkimHdHubAssign assign,
                           SkimHdHubSpot spot, gpointer user, GError **error);
void       skim_hd_hub_free(SkimHdHub *h);
guint16    skim_hd_hub_port(const SkimHdHub *h);

/* One IQ block of hub band `band` (the source's thread): to the worker that
 * decodes it, if one is connected. */
void skim_hd_hub_push(SkimHdHub *h, guint band, const float *iq, guint nframes, double rate,
                      double centre_hz);

/* {"port":N,"workers":[{"name","addr","state","bands":[…],"mbps","dropped",
 *   "spots","status":<its status.json>|null}, …]} — call it once a second. */
char *skim_hd_hub_json(SkimHdHub *h);

/* The active decoders a worker last reported for hub band `band`, mode 0 =
 * CW, 1 = RTTY; -1 when none did. */
int skim_hd_hub_band_active(SkimHdHub *h, guint band, guint mode);

/* Workers connected with nothing assigned (newly allocated names). */
char **skim_hd_hub_idle_workers(SkimHdHub *h);

G_END_DECLS
