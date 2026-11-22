/* decode_cw_v4.h — CW v4 ("v4.0 DeepCW"): e04's DeepCW v2 narrow network on
 * the channelizer's complex baseband, on the CPU (dcw4_kernel.h), read by
 * e04's posterior stitcher (dcw4_stitch.h), with the network's posteriors
 * turned into what the rest of the engine reads — text and one confidence
 * per event — plus call corrections (dcw4_callread.h) as FIX tokens.
 *
 * Per channel (250 Hz complex, the CW bank's):
 *  - front end: a 20-point periodic Hann DFT every 16 ms (4 samples), the
 *    channel's 11 bins (±62.5 Hz) in a frame ring; level, tone offset,
 *    the keyed-line gate (line over the floor, keyed duty) and SNR in
 *    500 Hz (OK1BR 4db0483, gh#27, against the band floor set_floor gives)
 *    as the DeepCW v1 backend and SDRIAK's v2 backend have them;
 *  - every step (3 s) on a keyed line, one window (≤ 12 s; 6 s at the
 *    default L 1 + G 2 + S 3) of the line's bin and its two neighbours,
 *    normalised (noise_q90_log1p), through the network on a worker thread
 *    (process-wide pool, one window per run, one in flight per channel);
 *    the gate holds the windows on through a hangover after the over so its
 *    tail commits;
 *  - the stitcher's committed characters gather into words; a word closes
 *    on a word gap, on 2.01 s of blank after it, or on a TX hold's final
 *    window. A closed word is read by the call reader (on its own, and
 *    joined to the word before it: the narrow model tears a call before a
 *    digit at ≤ 12 WPM) and goes out whole: "WORD ", or "WORD\x01CALL\x02 "
 *    when a dictionary call won (callsign.h FIX: the extractor takes CALL,
 *    the pane shows WORD). A word whose weakest character's posterior is
 *    under drop_p goes to the pane only (take_aux_text; off by default).
 *    `break_s` of blank after the last word ends the over: "· " (v3's over
 *    break, which the extractor's runs and CQ window key on);
 *  - confidence of an event: the mean posterior of its word's characters;
 *    speed: 12 × characters / s over a committed stretch (PARIS).
 *
 * Switches (all SKIM_DCW4_*): GATE_DB (6), STEP / LEFT / GUARD (3, 1, 2 s),
 * WORKERS (0 = skim_decode_cw_v4_set_workers, else half the CPUs), SYNC (0;
 * 1 = inference inline, deterministic: replays and the golden gate),
 * DRAFT (1), CALLREAD (1), CALL_SEP (2 nats), DROP_P (0 = off), BREAK
 * (3 s), DEBUG (0), ISA (dcw4_kernel.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include "decode.h"

G_BEGIN_DECLS

const SkimDecodeBackend *skim_decode_cw_v4(void);

/* The network loads (the built-in blob, an encoder this CPU runs). */
gboolean skim_decode_cw_v4_available(GError **error);

/* Inference threads, before the first channel: [decode] v4_workers (0 =
 * half the CPUs, at least 1). SKIM_DCW4_WORKERS wins. */
void  skim_decode_cw_v4_set_workers(guint n);
guint skim_decode_cw_v4_workers(void);

/* "avx2, 4 workers" — what runs; g_free. */
char *skim_decode_cw_v4_runtime_info(void);

/* Process-wide counters since the start (the max since the last call with
 * reset_max). The load signal: ticks a channel skipped because its last
 * window was still queued or running, and how long windows waited. */
typedef struct {
  guint64 windows;       /* run through the network                  */
  guint64 ticks;         /* due on a keyed line                      */
  guint64 skipped;       /* … skipped: the channel's window in flight */
  double  wait_max_ms;   /* longest a window waited for a worker      */
  guint64 cpu_ns;        /* the workers' CPU                          */
  guint   queued;        /* windows waiting now                       */
  guint   workers;
} SkimCwV4Stats;
void skim_decode_cw_v4_stats(SkimCwV4Stats *out, gboolean reset_max);

/* The compute check's measurement: windows a second the network runs on
 * `threads` threads at once for `secs` (a keyed 6 s window, CW v4's L1 +
 * G2 + S3). Each keyed channel takes one window a step (3 s), so the
 * channels carried in real time are this × step. 0 when the network is
 * not available. Blocks for secs. */
double skim_decode_cw_v4_bench(guint threads, double secs);
/* CW v4's step (s): a keyed channel's window cadence. */
double skim_decode_cw_v4_step_s(void);

G_END_DECLS
