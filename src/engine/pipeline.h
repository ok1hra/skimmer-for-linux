/* pipeline.h — the headless engine assembled end to end.
 *
 * TCI client → polyphase channelizer → per-channel CW decoders → callsign
 * extractors → station tracker → spot feeder. Runs on its own engine thread
 * (the TCI client's LWS thread only queues IQ blocks). GLib-only — the GTK
 * app sits on top via callbacks and must marshal them to its main loop
 * itself (they fire on the engine thread).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_PIPELINE_H
#define SKIMMER_PIPELINE_H

#include <glib.h>
#include "decode.h"
#include "dup_query.h"
#include "rbn_feed.h"
#include "station.h"

G_BEGIN_DECLS

/* Decoded mode: one pipeline decodes ONE mode segment at a time (the CW
 * Skimmer model — the IQ stream covers a mode subband). The mode picks the
 * decode backend, the bank geometry (CW: 125 Hz channels; RTTY: 250 Hz
 * spacing with a wide ±225 Hz passband so both FSK tones of a station
 * anywhere between channel centres stay in one channel) and the mode string
 * on stations/spots/dup queries. */
typedef enum {
  SKIM_PIPELINE_MODE_CW = 0,
  SKIM_PIPELINE_MODE_RTTY,
} SkimPipelineMode;

/* CW decode ENGINE (CW mode only; RTTY has one backend). The classical
 * v2 Viterbi is the default; v1 is the historical fallback; DeepCW is the
 * neural Conformer/CTC backend (decode_deepcw.c) that needs ONNX Runtime
 * and a model file at run time — when either is missing the pipeline
 * falls back to v2 and says so. Env override for replays and probes:
 * SKIM_CW_ENGINE=v1|v2|deepcw (SKIM_CW_V1=1 still means v1). */
typedef enum {
  SKIM_CW_ENGINE_V2 = 0,
  SKIM_CW_ENGINE_V1,
  SKIM_CW_ENGINE_DEEPCW,
} SkimCwEngine;

/* Where the IQ comes from. TCI: a radio's TCI server (it owns the tuning;
 * spots go back to its panadapter, a station click tunes it). HPSDR: an
 * HPSDR Protocol 1 receiver the skimmer drives itself (Red Pitaya — see
 * hpsdr_p1.h): the pipeline sets the DDC centre; no panadapter, no VFO,
 * no TX state — spots reach the telnet/RBN sinks only. */
typedef enum {
  SKIM_PIPELINE_SOURCE_TCI = 0,
  SKIM_PIPELINE_SOURCE_HPSDR,
  /* Live, but the OWNER feeds the IQ with skim_pipeline_push() — one
   * multi-receiver radio link serving several pipelines (skimmer-headless:
   * one Red Pitaya, one pipeline per band). No radio state, no panadapter;
   * spots reach the telnet/RBN sinks. cfg.host is only a label. */
  SKIM_PIPELINE_SOURCE_EXTERNAL,
} SkimPipelineSource;

typedef struct {
  SkimPipelineSource source;  /* default TCI                                 */
  const char *host;           /* TCI server (default 127.0.0.1) / HPSDR radio */
  guint16     port;           /* default 40001 (TCI) / 1024 (HPSDR)          */
  guint       iq_rate;        /* TCI 48/96/192/384 kHz, 0 = keep device rate;
                               * HPSDR 48/96/192 kHz, 0 = 192               */
  double      center_hz;      /* HPSDR: DDC centre (TRUE, after correction)  */
  double      clock_ppm;      /* HPSDR: sampling-clock error, ppm            */
  gboolean    take_over;      /* HPSDR: start even when the radio is busy    */
  SkimPipelineMode mode;      /* decoded mode (default CW)                   */
  SkimCwEngine cw_engine;     /* CW engine (default v2; see SkimCwEngine)    */
  double      chan_bw_hz;     /* channel spacing (default 125 Hz CW,
                               * 250 Hz RTTY)                                */
  const char *dict_path;      /* optional MASTER.SCP; NULL = none            */
  const char *decode_log_path; /* append raw decodes here; NULL = no log     */

  /* M6 — RBN telnet feed (borrowed; the app owns it so aggregator sessions
   * survive TCI reconnects). NULL = no RBN. The feed is ALWAYS CQ-only and
   * gated at rbn_min_score (0 = default 0.85): stricter than the 0.70 that
   * puts a label on the local panadapter — the network wants certainty.
   * The score alone is not enough: ONE copy keyed after "CQ DE" scores 0.90,
   * and so does a garble of it. A call reaches the feed only once it was
   * read rbn_min_hearings times (0 = default 2; 1 = no such gate) or the
   * dictionary knows it, and only after it stayed in the station table for
   * rbn_settle_s (0 = default 8 s; < 0 = send at once): a torn call ("IZ3N"
   * one token before "YG") is folded away by the table within that time,
   * but a telnet line cannot be taken back (skimmer-compare vs RBN,
   * 2026-10-01: 36 % of the feed's calls were never confirmed, VE3NEA's
   * CW Skimmer Server 2 %). And the call must have been READ within the
   * last rbn_fresh_s (0 = default 120 s; < 0 = no such gate): a candidate
   * outlives its station, and noise on the quiet channel reports it again
   * an hour later (2026-10-02: 21 of the 48 unconfirmed spots left after
   * the hearings gate were such ghosts of real stations). */
  SkimRbnFeed *rbn;
  /* Every line the RBN policy sends — with or without .rbn: an offline
   * bench (skimmer-sweep) reads the feed's decisions without a socket.
   * Called on the feeding thread (offline) or the engine thread (live). */
  void       (*rbn_cb)(const char *call, double freq_hz, double snr_db,
                       double speed, gpointer user);
  gpointer     rbn_user;
  double       rbn_min_score;
  guint        rbn_min_hearings;
  double       rbn_settle_s;
  double       rbn_fresh_s;
} SkimPipelineConfig;

typedef struct _SkimPipeline SkimPipeline;

/* Station/text callbacks fire on the ENGINE thread; state/vfo callbacks may
 * also fire on the network thread (connection loss, radio retune) or the
 * caller's thread (start/stop). Marshal accordingly. */
typedef void (*SkimPipelineStationCb)(const SkimStation *st, gpointer user);
/* A station left the tracker (TTL, or its frequency was taken over). */
typedef void (*SkimPipelineStationGoneCb)(const SkimStation *st, gpointer user);
typedef void (*SkimPipelineTextCb)(double freq_hz, const char *text, gpointer user);
/* Phase B hybrid pane op (OPEN/SET/CLOSE — APPENDs ride the text cb): the
 * live over region at freq_hz becomes `text`; its first final_len bytes are
 * reader-final, the rest live draft. OPEN takes back `erase` bytes of
 * already-delivered draft first. Display only — never the extractor. */
typedef void (*SkimPipelineOverCb)(double freq_hz, SkimPaneOpKind kind,
                                   guint erase, const char *text,
                                   guint final_len, gpointer user);
typedef void (*SkimPipelineStateCb)(gboolean connected, const char *detail, gpointer user);
/* The radio's tuned frequency (vfo:0,0) changed. */
typedef void (*SkimPipelineVfoCb)(double vfo_hz, gpointer user);
/* M8 waterfall: one spectrum row of the raw IQ band (spectrum.h — nbins
 * bytes, fftshifted, byte = dBFS + 200). center_hz is the stream centre the
 * row was taken at; row index i sits at center_hz + (i − nbins/2) · bin_hz.
 * Fires only while enabled (display-only work — no FFT for a hidden view). */
typedef void (*SkimPipelineSpectrumCb)(const guint8 *row, guint nbins,
                                       double center_hz, double bin_hz,
                                       gpointer user);

SkimPipeline *skim_pipeline_new(const SkimPipelineConfig *cfg);
void          skim_pipeline_free(SkimPipeline *p);

void skim_pipeline_set_station_cb(SkimPipeline *p, SkimPipelineStationCb cb, gpointer user);
void skim_pipeline_set_station_gone_cb(SkimPipeline *p, SkimPipelineStationGoneCb cb, gpointer user);
void skim_pipeline_set_text_cb(SkimPipeline *p, SkimPipelineTextCb cb, gpointer user);
void skim_pipeline_set_over_cb(SkimPipeline *p, SkimPipelineOverCb cb, gpointer user);
void skim_pipeline_set_state_cb(SkimPipeline *p, SkimPipelineStateCb cb, gpointer user);
void skim_pipeline_set_vfo_cb(SkimPipeline *p, SkimPipelineVfoCb cb, gpointer user);
void skim_pipeline_set_spectrum_cb(SkimPipeline *p, SkimPipelineSpectrumCb cb, gpointer user);
/* Thread-safe switch for the spectrum tap (default off). */
void     skim_pipeline_set_spectrum_enabled(SkimPipeline *p, gboolean on);
gboolean skim_pipeline_spectrum_enabled(const SkimPipeline *p);

/* Connect + start decoding. Blocks for the TCI handshake / the first HPSDR
 * packet. An HPSDR radio streaming to another client fails with
 * SKIM_HPSDR_ERROR_BUSY (hpsdr_p1.h) unless cfg.take_over. */
gboolean skim_pipeline_start(SkimPipeline *p, GError **error);
void     skim_pipeline_stop(SkimPipeline *p);

/* Offline mode (the .cf32 replayer / M3 A/B gate): no TCI, no engine thread —
 * the caller feeds IQ synchronously and callbacks fire on the caller's
 * thread. The decode log is stamped with STREAM time (deterministic across
 * runs). host/port/iq_rate in the config are ignored. */
/* The decode engine actually in use ("cw-v2", "cw-v1", "deepcw", "rtty") —
 * resolved at skim_pipeline_new from the config, the env override and the
 * DeepCW availability check (About, replay header, logs). */
const char *skim_pipeline_cw_engine_name(const SkimPipeline *p);

gboolean skim_pipeline_start_offline(SkimPipeline *p, GError **error);

/* SOURCE_EXTERNAL: queue one IQ block for the engine thread — any thread,
 * never blocks (a full queue drops the block and counts it, like the TCI
 * ingest). A no-op before start / after stop. */
void     skim_pipeline_push(SkimPipeline *p, const float *iq, guint nframes,
                            double rate, double center_hz);
void     skim_pipeline_feed(SkimPipeline *p, const float *iq, guint nframes,
                            double rate, double center_hz);

/* TX hold: TRUE while the operator's own radio
 * transmits. Live it is driven automatically from the TCI trx/tune
 * broadcasts (sdr-for-linux ≥ cc470af reports the real keyed state); the
 * offline harness/gates drive it by hand. While held the pipeline swallows
 * IQ blocks — every decoder/extractor/squelch time constant freezes instead
 * of adapting to the self-deafened band — and resumes with a bit-level
 * resync ~300 ms after release, so the answering station decodes from its
 * first characters. A ~30 s cap unfreezes a stuck hold. Thread-safe. */
void skim_pipeline_set_tx_hold(SkimPipeline *p, gboolean tx);

/* The radio's tuned frequency (0 until the first vfo broadcast lands). */
double skim_pipeline_vfo_hz(const SkimPipeline *p);

/* Tune the radio to freq_hz — only ever on an explicit user action (a no-op
 * while disconnected). The vfo broadcast confirms the retune. */
void   skim_pipeline_tune(SkimPipeline *p, double freq_hz);

/* Announce a user's click on a decoded callsign (clicked_on_spot over TCI) —
 * the server relays it to its other clients so a logger prefills. Explicit
 * user action only; a no-op while disconnected. */
void   skim_pipeline_spot_clicked(SkimPipeline *p, const char *call,
                                  double freq_hz);

/* The logbook's cached dup verdict for call (UNKNOWN when the logbook is
 * closed or has not answered yet). Never blocks — safe from the GTK thread;
 * the app tints the decode-pane highlight with it. */
SkimDupVerdict skim_pipeline_dup_verdict(SkimPipeline *p, const char *call,
                                         double freq_hz);

/* Spot policy: when TRUE, only stations heard CALLING (CQ/TEST/QRZ context)
 * reach the spot sinks — S&P answers stay off the panadapter/RBN. The
 * station list still tracks everything. Runtime-switchable, thread-safe. */
void   skim_pipeline_set_spot_cq_only(SkimPipeline *p, gboolean cq_only);

/* Snap outgoing spot frequencies (panadapter AND telnet feed) to a grid;
 * 0/1 = exact. Applies live. */
void   skim_pipeline_set_spot_round_hz(SkimPipeline *p, guint hz);

/* The RBN feed policy in force, defaults resolved: score threshold, hearings
 * needed (1 = no such gate), settle time in s (0 = sends at once), freshness
 * in s (0 = no such gate). All 0 when the pipeline has no RBN feed. */
void skim_pipeline_rbn_policy(const SkimPipeline *p, double *min_score,
                              guint *min_hearings, double *settle_s,
                              double *fresh_s);

/* Counters for the status line / gates. */
guint64 skim_pipeline_frames(const SkimPipeline *p);
guint64 skim_pipeline_spots(const SkimPipeline *p);
guint64 skim_pipeline_rbn_spots(const SkimPipeline *p);
guint   skim_pipeline_stations(const SkimPipeline *p);
guint64 skim_pipeline_dropped_blocks(const SkimPipeline *p);
/* Decoder channels in the bank (0 until the first block sized it). */
guint   skim_pipeline_channels(const SkimPipeline *p);
/* HPSDR source: frames zero-filled for lost UDP packets (0 for TCI). */
guint64 skim_pipeline_lost_frames(const SkimPipeline *p);

G_END_DECLS

#endif /* SKIMMER_PIPELINE_H */
