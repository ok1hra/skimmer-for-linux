project('skimmer-for-linux', 'c',
  version : '0.8.10',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')
# GLib 2.74 is the floor (Debian 12 — the AppImage's base): newer API is a
# compile error, not a crash on the target.
add_project_arguments('-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_74',
                      '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_74',
                      '-Werror=deprecated-declarations', language : 'c')
add_project_arguments('-DSKIMMER_VERSION="@0@"'.format(meson.project_version()),
                      '-DSKIMMER_BUILD="@0@"'.format(get_option('build_suffix')),
                      language : 'c')

# --- Dependencies -----------------------------------------------------------
# GLib only — no GUI: the engine (src/engine/) and skimmer-headless
# (src/headless/ + src/headless_main.c). libwebsockets is the TCI client's
# transport, fftw3f the channelizer FFT (+ the vendored WDSP resampler).
cc        = meson.get_compiler('c')
gnome     = import('gnome')  # GResource: the built-in weights
glib_dep  = dependency('glib-2.0', version : '>=2.74')  # headless engine
gio_dep   = dependency('gio-2.0', version : '>=2.74')   # sockets, GResource
m_dep     = cc.find_library('m', required : false)
lws_dep   = dependency('libwebsockets')       # M1: TCI client transport
fftw3f_dep  = dependency('fftw3f')            # M2: channelizer FFT (float)
fftw3_dep   = dependency('fftw3')             # WDSP fir.c min-phase path (double)
threads_dep = dependency('threads')
curl_dep  = dependency('libcurl', version : '>=7.85')  # MASTER.SCP updater
soup_dep  = dependency('libsoup-3.0', version : '>=3.0') # the web page

# WDSP subset (FIR designer + resampler) — declares wdsp_dep. VENDOR.md there
# records provenance and the subset rationale.
subdir('vendor/wdsp')
# WSJT-X's wsprd (the WSPR decoder, a helper program) — VENDOR.md there.
subdir('vendor/wsprd')
# WSJT-X's FT8/FT4 decoders (Fortran, a helper program) — VENDOR.md there.
subdir('vendor/wsjtx-ft')

engine_inc = include_directories('src/engine', 'vendor/onnxruntime')
# ONNX Runtime is NEVER linked: decode_deepcw dlopen()s it at run time
# (vendor/onnxruntime/VENDOR.md) — only dlopen/dlsym are needed.
dl_dep = cc.find_library('dl', required : false)

# --- Engine (headless, GLib-only) -------------------------------------------
# Grows milestone by milestone. M0: skeleton modules that build
# and expose the architecture; no pipeline wired yet.
engine_sources = [
  'src/engine/tci_client.c',   # M1: WS client, IQ ingest, conjugate, outgoing SPOT
  'src/engine/hpsdr_p1.c',     # HPSDR Protocol 1 IQ source (Red Pitaya), no TCI
  'src/engine/channelizer.c',  # M2: polyphase filter bank → complex channels
  'src/engine/fftw_lock.c',    # FFTW planner is not thread-safe: one lock
  'src/engine/decode_cw.c',    # M3: CW backend (implements decode.h)
  'src/engine/decode_cw_v2.c', # CW v2: semi-Markov Viterbi (QSB-proof)
  'src/engine/decode_cw_v3.c', # CW v3: matched front end, calibrated LLR, kws reader
  'src/engine/decode_rtty.c',  # M7: RTTY backend (Baudot FSK)
  'src/engine/rtty_pairs.c',   # RTTY pairs held → the band's CW stays off them
  'src/engine/ort_shim.c',     # ONNX Runtime C API behind dlopen (optional)
  'src/engine/decode_deepcw.c',# DeepCW v1 neural CW backend (Conformer + CTC)
  'src/engine/decode_cw_v4.c', # CW v4: DeepCW v2 narrow on the CPU (dcw4_*)
  'src/engine/dcw4_stitch.c',  # CW v4: e04's posterior stitcher
  'src/engine/dcw4_callread.c',# CW v4: CTC call reader against MASTER.SCP
  'src/engine/cw_reader.c',    # neural over re-reader (TCN+CTC, pane-only)
  'src/engine/tone_split.c',   # per-channel carrier splitter (multi-signal)
  'src/engine/snr_meter.c',    # shadow SNR against the band floor
  'src/engine/station.c',      # M5: per-frequency station tracker
  'src/engine/callsign.c',     # M4: extraction + validation
  'src/engine/spot_out.c',     # M5/M6: spot policy — dedup, rate, sinks
  'src/engine/rbn_feed.c',     # M6: RBN telnet server (CW Skimmer dialect)
  'src/engine/pipeline.c',     # M5: the engine assembled end to end
  'src/engine/gatelog.c',      # feed-gate learning logs (decode tap + rows)
  'src/engine/feed_gate.c',    # the learned feed gate (shadow mode)
  'src/engine/feed_fold.c',    # RBN feed: busted twins of a sent call
  'src/engine/oracle.c',       # measurement only: R's stations → v2 (SKIM_ORACLE)
]
engine_deps = [glib_dep, gio_dep, lws_dep, wdsp_dep, fftw3f_dep, m_dep, dl_dep]

# CW v4's network (src/engine/dcw4_kernel.h): e04's DeepCW v2 narrow model
# as a hand-written forward pass, its weights built in (weights/dcw4.*).
# The encoder (dcw4_encoder.inc) is compiled once per instruction set and
# picked at run time: AVX2 + FMA in a library of its own (only that file
# may hold AVX2 code), SSE2 (x86-64's baseline) and plain arrays beside the
# loader. -ffp-contract=off: the front end must round as ONNX Runtime does.
dcw4_res = gnome.compile_resources('skimmer-dcw4-weights',
  'weights/dcw4.gresource.xml',
  source_dir : 'weights',
  c_name : 'skim_dcw4_weights',
)
dcw4_x86_64 = host_machine.cpu_family() == 'x86_64'
dcw4_avx2_lib = static_library('dcw4-avx2',
  'src/engine/dcw4_enc_avx2.c',
  c_args : dcw4_x86_64 ? ['-mavx2', '-mfma'] : [],
)
dcw4_lib = static_library('dcw4',
  ['src/engine/dcw4_kernel.c', 'src/engine/dcw4_enc_sse.c',
   'src/engine/dcw4_enc_generic.c', dcw4_res],
  c_args : ['-ffp-contract=off'],
  dependencies : [glib_dep, gio_dep, m_dep],
  link_with : dcw4_avx2_lib,
)

engine_lib = static_library('skimmer-engine',
  engine_sources,
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : dcw4_lib,
)

# --- Gates (offline tests + live probes, sdr-for-linux house style) ---------
# M1: offline gate — mock TCI server + skim_tci_client round-trip (handshake,
# Stream reassembly, conjugate orientation, spot plumbing).
skimmer_tci_test = executable('skimmer-tci-test',
  ['src/tci_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('tci-client', skimmer_tci_test, timeout : 60)

# M1: live gate — IQ ingest + orientation eyeball check against a running
# sdr-for-linux (ws://host:40001).
executable('skimmer-tci-probe',
  ['src/tci_probe_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# Offline gate — mock Red Pitaya sdr-receiver-hpsdr (UDP) + skim_hpsdr_client:
# discovery, EP2 bytes (rate, one RX, ppm-corrected word), RF-inverted wire
# conjugated on ingest, gap zero-fill, busy/take-over etiquette, stop, watchdog.
skimmer_hpsdr_test = executable('skimmer-hpsdr-test',
  ['src/hpsdr_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('hpsdr-client', skimmer_hpsdr_test, timeout : 60)

# Live gate — discovery + IQ ingest straight from an HPSDR Protocol 1
# receiver (Red Pitaya sdr_receiver_hpsdr); records cf32 + .meta for replay.
executable('skimmer-hpsdr-probe',
  ['src/hpsdr_probe_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# M3 A/B harness: offline decode of a probe-recorded cf32 sample through the
# full pipeline — decoder versions diff on the same off-air corpus.
skimmer_replay = executable('skimmer-replay',
  ['src/replay_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# Feed-gate learning: replay a decode tap (headless [feed] tap=) through the
# extractor → station table → RBN gate, offline, on the tap's own clock.
skimmer_tap_replay = executable('skimmer-tap-replay',
  ['src/tap_replay_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# Decoder benchmark: the CW backend alone on dumped channelizer channels
# (skimmer-replay SKIM_DUMP_CHAN) — skimmer-compare/cqww/corpus.py.
executable('skimmer-cw-bench',
  ['src/cw_bench_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# M2: WDSP subset smoke — headers compile against our c11, fir_bandpass +
# create_resample link and behave (the sdrfl-wdsp-smoke counterpart).
skimmer_wdsp_smoke = executable('skimmer-wdsp-smoke',
  ['src/wdsp_smoke.c'],
  dependencies : [wdsp_dep, m_dep],
  # includes WDSP's comm.h, which needs the GNU dialect (see vendor/wdsp/meson.build)
  override_options : ['c_std=gnu11'],
)
test('wdsp-smoke', skimmer_wdsp_smoke, timeout : 30)

# M2: offline gate — polyphase channelizer on synthetic tones (isolation,
# alias rejection, phase preservation, CPU headroom).
skimmer_chan_test = executable('skimmer-chan-test',
  ['src/chan_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('channelizer', skimmer_chan_test, timeout : 120)

# M3: offline gate — CW backend on synthetic keying (speeds, SNR, jitter,
# QSB, speed change, squelch-on-noise, channelizer integration).
skimmer_cw_test = executable('skimmer-cw-test',
  ['src/cw_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('cw-decode', skimmer_cw_test, timeout : 120)

# M7: offline gate — RTTY backend on synthetic Baudot FSK (ITA2 bit-vector
# truth, offsets, figures/UOS, reversed polarity, AWGN/QSB/selective fade,
# squelch on noise/CW/two-carrier, wide-passband channelizer integration).
skimmer_rtty_test = executable('skimmer-rtty-test',
  ['src/rtty_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('rtty-decode', skimmer_rtty_test, timeout : 120)

# DeepCW: offline gate — tile builder (bin seating, +30 Hz offset sign,
# level, gate on keyed vs noise), the pure commit rule on synthetic CTC
# matrices, and — only when SKIM_ORT_LIB / SKIM_DEEPCW_MODEL resolve — the
# real model on a synthetic keyed tone (exit 77 = SKIP otherwise).
skimmer_deepcw_test = executable('skimmer-deepcw-test',
  ['src/deepcw_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
# SKIM_DEEPCW_SYNC=1: the backend batches inference on worker threads, so in
# async mode it is the scheduler that decides whether a character was already
# committed when the TX hold cuts in -- the "flush" checks then compare two
# snapshots that both move, and the gate failed 17 runs in 20. Inference
# inline makes the gate a test of the decode logic, which is what it asserts.
test('deepcw-decode', skimmer_deepcw_test, timeout : 300,
  env : ['SKIM_DEEPCW_SYNC=1'])

# CW v4's network: the built-in blob and its pin, every encoder this CPU
# runs against the plain-array one (|Δ logp| ≤ 1e-4, the same greedy text),
# a keyed tile read back; with SKIM_DCW4_ONNX=<model_narrow.onnx> and an
# ONNX Runtime also against the model itself (said SKIP otherwise).
skimmer_dcw4_kernel_test = executable('skimmer-dcw4-kernel-test',
  ['src/dcw4_kernel_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('dcw4-kernel', skimmer_dcw4_kernel_test, timeout : 120)

# CW v4 from the network's output on: the normalisation, the posterior
# stitcher on synthetic CTC streams, the CTC call reader against a test
# MASTER.SCP, the backend on synthetic keyed IQ (inference inline).
skimmer_dcw4_stream_test = executable('skimmer-dcw4-stream-test',
  ['src/dcw4_stream_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('dcw4-stream', skimmer_dcw4_stream_test, timeout : 120)

# What CW v4's network costs here: windows/s per encoder on 1 and N threads,
# keyed channels carried, CPU per channel-second.
executable('skimmer-dcw4-bench',
  ['src/dcw4_bench_main.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)

# Tone splitter: offline gate — sideband immunity, Δf 50/30 Hz splits with
# both texts copied, Δf 15 Hz contested, slot TTL collapse/re-engage, and the
# WHOLE offline pipeline with SKIM_TONE_SPLIT=1 (both calls, no mutations).
skimmer_split_test = executable('skimmer-split-test',
  ['src/split_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('tone-split', skimmer_split_test, timeout : 120)

# Shadow SNR meter: a keyed carrier of known SNR_2500 in white noise
# through the CW channelizer reads back — -10..+40 dB, off centre, busy band.
skimmer_snr_test = executable('skimmer-snr-test',
  ['src/snr_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('snr-meter', skimmer_snr_test, timeout : 120)

# M4: offline gate — callsign validity (ITU allocation), labelled-corpus
# precision/recall, fragment feeds, dictionary boost, noise fuzz.
skimmer_call_test = executable('skimmer-call-test',
  ['src/call_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('callsign', skimmer_call_test, timeout : 60)

# Learned feed gate: a known ini gives a known p, bad inis are refused,
# the tracker's window/neighbour features, likeness as skimcmp's similar().
# The learned weights built into skimmer-headless (weights/: a new model is a
# new commit of its ini; --feed-gate overrides it for one start).
weights_res = gnome.compile_resources('skimmer-weights',
  'weights/weights.gresource.xml',
  source_dir : 'weights',
  c_name : 'skim_weights',
)

skimmer_feed_gate_test = executable('skimmer-feed-gate-test',
  ['src/feed_gate_test.c', weights_res],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('feed-gate', skimmer_feed_gate_test, timeout : 60)

# RBN feed twin fold: a glued/cut twin of a call sent lately on the
# frequency is held back unless it is the better-attested call.
skimmer_feed_fold_test = executable('skimmer-feed-fold-test',
  ['src/feed_fold_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('feed-fold', skimmer_feed_fold_test, timeout : 60)

# M5: offline gate — station tracker units, spot policy units, and the WHOLE
# pipeline over a real WebSocket against a mock TCI server (spots come back).
skimmer_spot_test = executable('skimmer-spot-test',
  ['src/spot_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('spot-pipeline', skimmer_spot_test, timeout : 120)

# M6: offline gate — the RBN telnet feed: login handshake, cluster line
# format, multi-client broadcast; then the OFFLINE pipeline against a local
# telnet sink (CQ-only etiquette, dedup, exact kHz, no unvalidated calls).
skimmer_rbn_test = executable('skimmer-rbn-test',
  ['src/rbn_test.c'],
  include_directories : include_directories('src'),
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('rbn-feed', skimmer_rbn_test, timeout : 120)

# headless.ini parsing: defaults, band modes, host@ppm, refusals with reasons.
skimmer_config_test = executable('skimmer-config-test',
  ['src/config_test.c', 'src/headless/config.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('config', skimmer_config_test, timeout : 30)

# [peers]: other skimmers' feeds read, their CQ spots handed on; the line parser.
skimmer_peers_test = executable('skimmer-peers-test',
  ['src/peers_test.c', 'src/headless/peers.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('peers', skimmer_peers_test, timeout : 60)

# The hub's side of the hub ↔ worker link: handshake, int24 IQ, lines, lag.
skimmer_hub_test = executable('skimmer-hub-test',
  ['src/hub_test.c', 'src/headless/hub.c', 'src/headless/config.c', 'src/headless/web.c',
   'src/headless/visitors.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps + [soup_dep],
)
test('hub', skimmer_hub_test, timeout : 60)

# PSK Reporter: the datagram byte for byte against pskreporter.info/pskdev.html,
# split under the limit; the uploader against a local UDP listener.
skimmer_pskreporter_test = executable('skimmer-pskreporter-test',
  ['src/pskreporter_test.c', 'src/headless/pskreporter.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('pskreporter', skimmer_pskreporter_test, timeout : 60)

# The 7-day history: buckets, reopen with re-ordered bands, a foreign file.
skimmer_stats_test = executable('skimmer-stats-test',
  ['src/stats_test.c', 'src/headless/stats.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('stats', skimmer_stats_test, timeout : 30)

# The spot archive: a gzip file a UTC day, one member a flush, a refused disk.
skimmer_archive_test = executable('skimmer-archive-test',
  ['src/archive_test.c', 'src/headless/archive.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('archive', skimmer_archive_test, timeout : 30)

# The page's spots across a restart: saved, read back, old ones and bad lines left out.
skimmer_spotlog_test = executable('skimmer-spotlog-test',
  ['src/spotlog_test.c', 'src/headless/spotlog.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('spotlog', skimmer_spotlog_test, timeout : 30)

# The waterfall's DSP: a tone lands in its pixel at its level, zoom, edges.
skimmer_waterfall_test = executable('skimmer-waterfall-test',
  ['src/waterfall_test.c', 'src/headless/waterfall.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps + [soup_dep],
  link_with : engine_lib,
)
test('waterfall', skimmer_waterfall_test, timeout : 30)

# WSPR: the dials, wsprd's lines, the WSPRnet form; two synthetic WSPR
# signals at 48 and 192 kHz through the shift, the resamplers, the capture,
# the .c2 file and skimmer-wsprd; the decoder thread, the stations of 24 h,
# the upload to a local stand-in for WSPRnet.
skimmer_wspr_test = executable('skimmer-wspr-test',
  ['src/wspr_test.c', 'src/headless/wspr.c'],
  include_directories : [include_directories('src'), engine_inc, wsprd_enc_inc],
  dependencies : engine_deps + [soup_dep],
  link_with : wsprd_enc_lib,
)
test('wspr', skimmer_wspr_test, args : [skimmer_wsprd], timeout : 120)

# FT8/FT4: skimmer-ft8d (WSJT-X's decoders, vendor/wsjtx-ft) on synthetic
# air — tones from upstream's ft8code/ft4code (so our CRC-14 stub is checked
# by the decoder's Fortran one), two messages a cycle with their SNR, DT and
# frequency, two cycles in one run, noise alone, a WAV at the wrong rate.
if ft8_enabled
  # The whole [ft8]: the dials, the lines and each message's sender; FT8 and
  # FT4 in IQ at 48 and 192 kHz through the DSP, the cycles, the WAV and
  # skimmer-ft8d — decodes, no mirror from below the dial, the spots folded;
  # a decoder slower than the cycles skips the waiting ones.
  skimmer_ft8_test = executable('skimmer-ft8-test',
    ['src/ft8_test.c', 'src/headless/ft8.c'],
    include_directories : [include_directories('src'), engine_inc],
    dependencies : engine_deps,
    link_with : engine_lib,
  )
  test('ft8', skimmer_ft8_test,
       args : [skimmer_ft8d, skimmer_ft8code, skimmer_ft4code], timeout : 240)
  # The bench on real air: a recorded cf32 through the whole [ft8] (and with
  # --keep the cycles' WAVs, for WSJT-X's own jt9).
  executable('skimmer-ft8-replay', ['src/ft8_replay_main.c', 'src/headless/ft8.c'],
    include_directories : [include_directories('src'), engine_inc],
    dependencies : engine_deps,
    link_with : engine_lib,
  )
  skimmer_ft8d_test = executable('skimmer-ft8d-test', 'src/ft8d_test.c',
    dependencies : [glib_dep, m_dep])
  test('ft8d', skimmer_ft8d_test,
       args : [skimmer_ft8d, skimmer_ft8code, skimmer_ft4code], timeout : 120)
endif

# The governor: adaptive RTTY's hourly test (and the thermal guard).
skimmer_governor_test = executable('skimmer-governor-test',
  ['src/governor_test.c', 'src/headless/governor.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('governor', skimmer_governor_test, timeout : 30)

# The visitors: the callsign check, the signed cookie, visits and what they
# watched, the refused sign-ins and their cap, the web's sign-in gate.
skimmer_visitors_test = executable('skimmer-visitors-test',
  ['src/visitors_test.c', 'src/headless/visitors.c', 'src/headless/web.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps + [soup_dep],
)
test('visitors', skimmer_visitors_test, timeout : 60)

# SkimSrv.ini under wine: found, the newest wins, only the band keys leave.
skimmer_skimsrv_test = executable('skimmer-skimsrv-test',
  ['src/skimsrv_test.c', 'src/headless/skimsrv.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps,
)
test('skimsrv', skimmer_skimsrv_test, timeout : 30)

# Golden gate: real-air IQ + decode-tap slices through the whole engine with
# no SKIM_* switches — every output byte-identical to tests/golden/
# expected.sha256. Inputs live outside git (tests/golden/README.md); without
# them the gate SKIPs.
test('golden', find_program('tests/golden/run.sh'),
  args : [skimmer_replay, skimmer_tap_replay],
  timeout : 1800,     # CW v4 on the generic encoder: ~10 min a slice
)

# gh#15: offline gate — the MASTER.SCP updater against a mock HTTP server:
# the good path, every way a server or a download can be wrong (the old file
# and the loaded dictionary must survive each), the persisted rate limits,
# and the app handle (main loop never blocked, teardown never waits).
skimmer_scp_test = executable('skimmer-scp-test',
  ['src/scp_test.c', 'src/headless/scp_update.c'],      # MASTER.SCP updater
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps + [curl_dep],
  link_with : engine_lib,
)
test('scp-update', skimmer_scp_test, timeout : 180)

# [update]: versions, the release JSON (a real API answer), a file:// mirror
# with a fake AppImage — the swap and .prev, every refusal (digest, none,
# size, the preflight's version and config), the install hour, a crash loop
# rolled back, the rollback asked for.
skimmer_update_test = executable('skimmer-update-test',
  ['src/update_test.c', 'src/headless/update.c'],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : [glib_dep, gio_dep, curl_dep],
)
test('update', skimmer_update_test,
  args : [meson.current_source_dir() / 'tests' / 'update'], timeout : 120)

# Multi-band skimmer without a GUI: one HPSDR P1 receiver (Red Pitaya) with
# up to 8 RX, one pipeline per band, one telnet feed, console + web status.
# The product layer (src/headless/): config, IQ sources, recording, switches.
headless_sources = files(
  'src/headless/config.c',          # headless.ini, parsed and checked
  'src/headless/deepcw_model.c',    # the DeepCW model: check, fetch
  'src/headless/discovery.c',       # instances finding each other on the LAN
  'src/headless/governor.c',        # adaptive RTTY, the thermal guard
  'src/headless/hub.c',             # the hub: bands passed to workers on other PCs
  'src/headless/logring.c',         # the last log lines (/api/log)
  'src/headless/archive.c',         # [archive]: every spot, a gzip file a day
  'src/headless/spotlog.c',         # the page's spots, kept across a restart
  'src/headless/peers.c',           # [peers]: other skimmers' feeds, aggregated
  'src/headless/record.c',          # [record]: a band's IQ to disk
  'src/headless/scp_update.c',      # MASTER.SCP updater
  'src/headless/service.c',         # --install-service: the systemd user unit
  'src/headless/skimsrv.c',         # CW Skimmer Server's bands, from its SkimSrv.ini
  'src/headless/source.c',          # IQ sources: the interface
  'src/headless/source_hpsdr_p1.c', # … a Red Pitaya (HPSDR P1)
  'src/headless/source_hub.c',      # … a worker: the bands its hub passes on
  'src/headless/source_mock.c',     # … synthetic CW (the headless gate)
  'src/headless/stats.c',           # the 7-day per-minute history
  'src/headless/update.c',          # [update]: a newer release swapped in
  'src/headless/switches.c',        # the SKIM_* switches a start runs with
  'src/headless/visitors.c',        # the page's visitors: sign-in, their log
  'src/headless/waterfall.c',       # the page's waterfall (only while watched)
  'src/headless/web.c',             # the web page (libsoup 3)
  'src/headless/wspr.c',            # [wspr]: WSPR decoded beside, WSPRnet
  'src/headless/ft8.c',             # [ft8]: FT8/FT4 decoded beside, to the feed
  'src/headless/pskreporter.c',     # [ft8] upload: to PSK Reporter (IPFIX/UDP)
)

# The web page's static files (web/), built in.
web_res = gnome.compile_resources('skimmer-web',
  'web/web.gresource.xml',
  source_dir : 'web',
  c_name : 'skim_web',
)

skimmer_headless = executable('skimmer-headless',
  ['src/headless_main.c', headless_sources, weights_res, web_res],
  include_directories : [include_directories('src'), engine_inc],
  dependencies : engine_deps + [curl_dep, soup_dep],
  link_with : engine_lib,
)

# Headless gate: the whole skimmer-headless on the mock source (no radio) —
# config, two bands, telnet spot, /status.json, SIGTERM, a refused config.
test('headless-mock', find_program('tests/headless-mock.sh'),
  args : [skimmer_headless], timeout : 120)

# Hub gate: a hub and a worker on this machine (mock source) — a band
# passed on, decoded there, spotted on the hub's feed; the peer duplicate;
# a reassignment the worker follows by itself.
test('hub-mock', find_program('tests/hub-mock.sh'),
  args : [skimmer_headless], timeout : 240)

# The known SKIM_* switches (src/headless/switches.c) = the ones the sources read.
test('switches', find_program('tests/switches.sh'),
  args : [skimmer_headless, meson.project_source_root()], timeout : 30)
