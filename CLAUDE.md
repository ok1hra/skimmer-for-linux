# Skimmer for Linux — project context

Instructions and context for Claude Code working in this repo. (Richard's global
`~/.claude/CLAUDE.md` rules also apply — consent before major/irreversible
changes, work in Czech with Richard, etc.)

## What this is

**This is Dan OK1HRA's fork** (ok1hra/skimmer-for-linux) of Richard OK1BR's
GTK4 skimmer. The fork has **dropped the GUI** (2026-10-22): it is
`skimmer-headless`, a multi-band skimmer without a GUI — a Red Pitaya (HPSDR
Protocol 1) streams up to 8 receivers, one pipeline per band decodes CW (v2,
or DeepCW) and feeds one telnet cluster feed; status on the console and a web
page. The plan for turning it into the product (web settings and statistics,
RTTY beside CW, AppImage, systemd service) is
`~/.claude/plans/headless-product.md` on hra2. The engine is still Richard's
and still has its TCI client.

## Ground rules

- **Language: C** (GLib only — no GUI since 2026-10-22), built with **meson**. All of OK1BR's Linux apps are C so DSP/render code is shared
  across projects. Do not introduce Rust/Go/Python as a primary language.
- **Don't reimplement DSP — reuse WDSP** (in-tree `vendor/wdsp`, same policy as
  `sdr-for-linux`: a copy, not a submodule; `VENDOR.md` records upstream +
  pinned commit). Use its FFT + `create_resample`.
- **Engine is headless and GLib-only** (`src/engine/`), so every milestone has an
  offline gate binary. The product layer is `src/headless/` +
  `src/headless_main.c`.
- **The golden gate** (`meson test golden`, `tests/golden/`) replays recorded
  real air (CW, and RTTY from the JARTS contest) through the whole engine with
  no `SKIM_*` switches. A refactor must
  leave it byte-identical; a deliberate behaviour change regenerates the
  reference (`--update`) in its own commit and says why.
- **The channelizer is complex/phase-preserving and mode-agnostic** from the
  start (RTTY/PSK need phase). Decode backends implement the `decode.h` interface.
- **Read-only to the radio — except an explicit user tune.** The skimmer
  consumes IQ and sends spots; it never keys and never changes radio state on
  its own. The single deliberate write is `vfo:0,0,<hz>` when the USER
  activates a station row (added 2026-07-15 at Richard's request).
  The HPSDR P1 source (Red Pitaya, `docs/HPSDR-P1.md`) is not the operator's
  radio but the skimmer's own receiver: it sets that receiver's DDC centre,
  rate and RX count, never keys, and never touches a receiver another
  client is streaming from unless the user presses "Take over". The one
  stream it stops by itself is its own orphan — still coming to the port of
  a run of ours that died, which nothing on this machine holds any more
  (`docs/HPSDR-P1.md`).
- **RBN feed must never emit unvalidated callsigns** — M4 (validation) gates M6
  (RBN).
- **Every commit and tag is GPG-signed** (`commit.gpgsign=true`, key
  35AE58A8…B33931; gpg signs without a prompt). Never override with
  `-c commit.gpgsign=false` / `--no-gpg-sign`; verify `git log --format=%G? -1`
  = `G` after each commit. Richard, 2026-09-13, after one unsigned commit
  (60fe31f) slipped in — that one stays as is, on his word.

## Work queue — GitHub Issues (since 2026-09-18)

Bugs, ideas, debt and anything still waiting for a live check are **GitHub
Issues** (`gh issue list -R OK1BR/skimmer-for-linux`), not a file in `docs/`.
`docs/BACKLOG.md` (SKM-1…SKM-20) left the tree on 2026-09-18: eight items were
done, the rest became #3–#16 — every "BACKLOG" / "SKM-N" mention below refers
to its last version, commit 96606bf. SKM-3's measured record (DeepCW) moved to
`docs/DEEPCW.md`. Richard's reason: finished and long-verified items kept
sitting there as "open", and nobody could see what was really left.

- Labels: type `bug` / `enhancement` / `debt`; `severity: high` = wrong data
  or something that leaves the machine wrong, `medium` = gets in the
  operator's way, `low` = cosmetic or log noise; `needs-live-check` = done in
  code, gates green, but the issue **stays open until the behaviour was seen
  live**; `at-the-radio` = the check needs the rig; `deferred` = parked on
  purpose.
- A commit closes its issue with `Fixes #N` only when nothing is left to
  verify live — otherwise `Refs #N`, and the issue is closed by hand once the
  check passed. Before filing a "needs live check", look for evidence that
  real operation already proved it (logs, contest data).
- Issue text is public and goes out under Richard's name: English, never
  hard-wrapped, shown to him (with a Czech translation) before it is posted.
- `docs/` keeps only what can have value in the future (Richard, 2026-09-18):
  binding rules, decisions with their rationale, hard-won protocol/DSP facts,
  measured numbers that justify a choice, rejected approaches, plans for work
  still ahead. Build diaries, gate counts, "state at the end of the day"
  blocks and old contest notes do not belong there — git keeps them. Notes
  from live operation may start in `docs/CONTEST-NOTES-<date>.md`, are triaged
  into issues, and then the file leaves the tree.

## TCI facts that matter (from sdr-for-linux `docs/TCI-SCOPE.md`)

- Server: `ws://<host>:40001`, `PROTOCOL:ExpertSDR3,1.9`.
- IQ: `iq_samplerate:{48,96,192,384};` + `iq_start:0;`. Binary Stream header
  `type=0`, float32, 2 ch, `length = frames×2`.
- **The wire carries TRUE spectrum orientation — do NOT conjugate on ingest.**
  The server conjugates its RF-inverted raw HPSDR DDC feed on send (that is the
  ExpertSDR convention SDC/CW Skimmer consume as-is). The TCI-SCOPE line
  "a +12 kHz DDC tone lands at −12 kHz on the wire" is relative to the raw DDC
  feed, *not* to RF. A client-side conjugate mirrors every frequency around the
  DDC centre — live-caught 2026-07-15 (spots landed out of band).
- `iq_samplerate` is device-global radio state, announced in the init block.
- Spots back: `SPOT:call,mode,freq,ARGB,text;` / `SPOT_DELETE:call` /
  `SPOT_CLEAR`. A click on the radio issues `rx_clicked_on_spot:0,0,call,hz`.

## Current state

What the tree does today. Anything still to DO is a GitHub Issue, not a line
here; what was done and why is in git.

- **Milestones M0–M7 are in the engine:** TCI ingest, channelizer, CW (v1
  classical, v2 semi-Markov Viterbi), callsign extraction/validation, the spot
  pipeline, the local telnet feed, RTTY (45.45 Bd). M8 (the waterfall) went
  with the GUI, and so did the engine's spectrum tap.
- **CW backend: v3.1 is the default (v0.8.1).** v3 = v2 verbatim for stations
  above 8 dB SNR500 plus a weak view (carrier detector, matched filter,
  calibrated LLR) and an audio-confirmed call reader (FIX tokens to the
  extractor); v3.1 = v3 with the precision rules on (`SKIM_V3_KR_RIVAL`,
  `SKIM_V3_KR_TWO`, `SKIM_GATE_HEAD` for CW v3 only; `=0` = v0.8.0's v3),
  reported as `cw-v3.1`; `docs/CW-V3.md`. `cw:v2` / `[decode] engine=v2` keep v2, and
  the golden gate runs v2 and v3 jobs side by side. `SKIM_CW_V1=1` selects
  the classical v1. DeepCW (neural, ONNX Runtime via `dlopen`, model outside
  git), is chosen per band (`cw:deepcw`) or with `[decode] engine=deepcw`;
  without the runtime or the model the pipeline falls back to v3 and the
  status says why. The model (AGPL) is never shipped: `--fetch-deepcw-model`
  / the web page fetch e04/deepcw-engine at a pinned commit, sha256-checked
  (`src/headless/deepcw_model.c` — a new model = a new pin there).
- **CW v4 ("v4.0 DeepCW", `[decode] engine=v4` / `cw:v4`, 2026-11-22 at
  Dan's request; default stays v3.1)**: e04's DeepCW v2 narrow network
  (MIT) as a C forward pass on the CPU — a bit-exact port of SDRIAK's
  fused kernel, AVX2/SSE2/generic picked at run time, the weights built in
  (`weights/deepcw-v4-narrow.bin`, `dcw4_kernel.c`); e04's posterior
  stitcher at L1/G2/S3 (`dcw4_stitch.c`); words go out whole with a CTC
  call reader against MASTER.SCP as FIX tokens (`dcw4_callread.c`), the
  over break, frequency and SNR per keyed window (`decode_cw_v4.c`). A
  compute check (Settings, `/api/v4bench`, cached in the state dir) warns,
  `[decode] v4_guard` pauses the quietest v4 band when the worker pool
  (`v4_workers`, 0 = half the CPUs) falls behind. The old DeepCW backend is
  shown as "DeepCW v1" (key `deepcw`). `docs/DEEPCW-V4.md`; needs a live
  check on air.
- **IQ sources: TCI, or HPSDR Protocol 1 direct** (Red Pitaya
  `sdr_receiver_hpsdr`, 48/96/192 kHz, clock ppm correction; the wire is
  RF-inverted and ingest conjugates — `docs/HPSDR-P1.md`). `skimmer-headless`
  runs up to 8 receivers (one pipeline per band and mode — CW, and RTTY
  beside it on the same IQ where `[bands]` says `rtty`; one telnet feed,
  console + web status). `[radio] host=auto` (the default) takes the
  first idle Red Pitaya a broadcast discovery finds; `--find-radios` lists
  them. RTTY spots skip the learned gate (a CW model).
  The RTTY pipeline publishes the pairs it copies (`rtty_pairs.c`); with
  `[decode] suppress_cw_on_rtty` (default off) the band's CW drops decodes
  on them — without it the CW pipeline is byte-for-byte unchanged. The FFTW planner is not thread-safe:
  plan create/destroy goes through `skim_fftw_lock()`.
- **`meson test` = 37 gates** (DeepCW skips without the model, golden skips
  without its inputs in `/var/tmp/skimmer-iq/golden`; `ft8d` and `ft8` exist
  only with gfortran). `headless-mock` runs
  the whole skimmer-headless on `[radio] type=mock` (no radio).
- **Hub, workers, peers** (`docs/HUB.md`): the PC with the radio passes the
  IQ of `@name` bands as int24 over TCP :7380 (`src/headless/hub.c`) to
  workers (`[radio] type=hub`, `source_hub.c`) whose bands and policy come
  from the hub's config; `[peers]` reads other feeds (`peers.c`); all of it
  leaves on the hub's feed under its call, deduplicated
  (`skim_rbn_feed_spot_external`); UDP 8075 discovery (`discovery.c`).
- **The web page** (libsoup 3, `src/headless/web.c`, static files in `web/`
  built in as a GResource, plain JS/SVG — no libraries, it must work
  offline; the look of IP-rotator/wifilt, Roboto Condensed built in):
  status = an azimuthal map of the last 30 min of spots (`web/map.js`, data
  `web/map/` from IP-rotator) + the DX cluster with column filters
  (`web/dxc.js`), both from `/api/spots`, + the bands table (10 m on top:
  two stacked bars a band, spots of the map's age and active decoders, each
  mode — CW, RTTY, FT8, FT4, WSPR — its part in its colour; FT8/FT4 active =
  the senders of the last minute, WSPR = its newest cycle's stations) with
  a heat map of the continents each band is open to (the modes counted,
  the map's age; how open, not how busy — two loud stations are an open
  band; `web/cont.js`: a call's continent from the map's DXCC labels). One
  mode filter and one age (5/10/15/30 min) for the page: the legend's eyes
  and the age buttons over the map set the DX cluster's Mode and UTC
  filters (`Dxc.setMode`/`setAge`, kept in `skdxc`); a mode left out stays
  grey in the bars and leaves the numbers, the heat map, the map and the
  cluster; the table's status column says what a worker or a guard does
  with the band; statistics (`stats.c`, three
  rings: 1 min × 7 d, 15 min × 90 d, 1 h × 2 y, saved in place; file
  version 6 = three modes, CW, RTTY and FT8 (FT8 + FT4: spots, senders
  that appeared, as "active" the senders heard a minute, SNR), and the
  helper programs' CPU (`cpu_helpers`: skimmer-ft8d, skimmer-wsprd —
  outside the process's own); version 5 added the thermal guard's pauses per
  band (`thermal`, the share of the bucket off — red in "active decoders by
  band"); a version 3, 4 or 5 file is carried over and kept as
  `stats.bin.v3` / `.v4` / `.v5` for an older build; ≈ 32 MB), settings (one parser:
  `skim_hd_config_apply_form` → `skim_hd_config_parse` → save with a
  backup → restart: exit 75 under `SKIM_SUPERVISED=1`, else exec), log.
  Managers only (settings_from, no proxy headers, a Host that is this
  machine — DNS rebinding): Settings and Log (cut out of index.html for the
  others), /api/config, /api/log, /api/skimmers, every POST; viewers get a
  status.json without addresses, paths and switches; the locator leaves
  the skimmer as its square only (4 characters, everyone — the map's
  centre, else the call's DXCC). A blank config value is the default (the
  form sends empty fields; `given()` in config.c). Settings → Bands lists
  the Rate's segments as CW Skimmer has them (45.5/91/182 kHz from each
  band's low edge, `segments()` in app.js; a second one is `80m-2`), up to
  8 on, plus bands of the user's own; off segments with nothing set are no
  [bands] line. *From CW Skimmer* takes SkimSrv's rate and ticked segments
  (`/api/skimsrv`, `skimsrv.c`: its SkimSrv.ini under wine on this PC, the
  band keys of [Skimmer] only; else a file chosen in the browser). Rules and the audit:
  `docs/WEB-SECURITY.md` — keep them when adding an endpoint. Frequencies show
  with a space after the MHz (`fmtKhz` in app.js); the charts share one plot
  frame and one crosshair.
- **The waterfall** (`src/headless/waterfall.c`, `web/waterfall.js`, WS
  `/ws/waterfall`): `rx_iq_cb` pushes every band's IQ, which returns at once
  while nobody watches it; a watched band runs FFTs of 8192 (16384 at
  192 kHz), half overlapping, into a 32-frame ring; each viewer asks for
  band/lo/hi/w/rate and gets bytes (0.5 dB from -160 dBFS), acks each row
  (no ack → fewer rows). Gate `waterfall`: a tone's pixel and level. While WSPR decodes, each
  WSPR passband of the band shown is dark red with "WSPR RX" (status.json
  `wspr.dials[].lo_hz/hi_hz`); the calls near the cursor show in a tip at
  the mouse.
- **The RTTY palette** (`web/rtty-palette.js`, `[rtty_palette]`): decodes
  the waterfall's audio (8 kHz µ-law, the tuned frequency at the 600 Hz BFO,
  so mark = +85 Hz) in the browser with WIFILT's RTTY-ICOM modules, copied
  unchanged into `web/rtty/` (`VENDOR.md`: a fix goes to ok1hra/wifilt
  first). A click on an RTTY spot (the DX cluster's speaker, a call over the
  waterfall) or RTTY in the bar opens it; while open the audio takes its
  passband, AUTOTUNE centres a steady pair once after each tuning, AFC
  follows. Its settings reach the page in status.json (`rtty_palette`);
  the engine never reads them.
- **The spot archive** (`[archive] dir=`, `src/headless/archive.c`): every
  spot the feed tap sees, a gzip file a UTC day (`YYYYMMDD-spots.gz`, TSV),
  one more gzip member a minute from the main loop; a refused disk keeps
  the lines for the next try.
- **The callsign filter** (`[filter] call=`, `regex=`; IP-rotator's DX
  filter, `data/dxc.html` of ok1hra/IP-rotator, with its RU/BY-out default
  regex: substring any case, or a caseless GRegex): a call that
  does not match is dropped in `take_text` right after the extractor names
  it (`SkimPipelineConfig.call_pass`, NULL = byte-identical), so it never
  reaches the station table, the stats, the map or any feed; `peer_spot` and
  `hub_spot` hold peers' and workers' spots to it.
- **Exit codes are the service contract:** 0 stopped, 2 bad config, 3 no
  free radio / stream lost, 75 restart for a `SKIM_SUPERVISED=1`
  supervisor. `--install-service` (`src/headless/service.c`) writes the
  systemd user unit on them (RestartSec=60 for 3 and crashes, 2 stays
  down); a web restart execs in place even under systemd, so it never
  waits out RestartSec.
- **GLib 2.74 is the floor** (Debian 12, the AppImage's base):
  `GLIB_VERSION_MAX_ALLOWED` makes newer API a compile error. It does NOT relink
  every binary — `ninja -C builddir` first.
- **The learned feed gate** is built in (`weights/feed-gate.ini`, GResource)
  and runs in shadow; `--feed-gate FILE` / `--feed-gate-mode off|shadow|on`
  override it per start.
- Dan's live instance on hra2 is the systemd **user service**
  `skimmer-headless` (since 2026-11-22; `--install-service`, linger on, so it
  starts at boot; a drop-in `skimmer-headless.service.d/limits.conf` sets
  LimitNOFILE=65536). It runs the released AppImage under a stable name,
  `~/skimmer-v0.7.0/skimmer-headless-x86_64.AppImage`, with `[update]
  mode=auto` (03 UTC): a new release swaps itself in, the old file stays as
  `.prev`. Log: `journalctl --user -u skimmer-headless -f` (tmux main:1 shows
  it); `systemctl --user restart|stop skimmer-headless` (from a Claude shell
  export `XDG_RUNTIME_DIR=/run/user/1000` first). A build by hand: stop the
  service, copy it to the stable name, start it — never a second instance
  beside it (`start.sh` refuses while the service runs). Dan's how-to:
  `~/skimmer-for-linux-navod.txt`. Work on the product goes in a worktree,
  never in the main clone's `builddir`. Recorded IQ fixtures live in
  `/var/tmp/skimmer-iq/` (a fresh one takes minutes:
  `skimmer-tci-probe <host> <port> <rate> <secs> <dump.cf32>`).
- **Decoder and feed defaults (v0.6.0) come from live air** against CW
  Skimmer Server: 45 Hz carrier envelope, word re-read against MASTER.SCP,
  fused/torn run reading, TU blesses the next call only, min_score 0.90,
  unknown calls held 60 s, fresh 120 s, fold 600 s; edge model / Rayleigh
  fade off. Numbers, bench and rejected tries: `docs/LIVE-AIR.md`.
- **Visitors sign in** (`[visitors] login`, default on; `src/headless/visitors.c`,
  the gate in `web.c`): a client that is neither a manager nor from
  `free_from` (default the private nets) without a proxy gets the page but
  401 on status/stats/spots and a closed waterfall until it signs in with a
  call (3–15 of A–Z0–9/, a digit and a letter) — an HMAC-signed HttpOnly
  cookie, 30 days, key in the state dir. Visits (call, address, time, the
  waterfall windows/audio watched ≥ 10 s) go to `visitors.tsv`, refusals to
  `visitors-failed.tsv` (≤ 30/min); the Log tab shows both, and Statistics
  lists the visitors under its table: whole at home (LAN, manager), from
  outside the last 24 h's calls and how long they stayed only. The visitor's
  call is in the header and, from another DXCC, a star on the map that AUTO
  zooms to. From outside (signed in or not) the status and history come
  without the machine readings — CPU load, IQ lost; the CPU temperature
  and the thermal guard go to everyone since 2026-11-21 (`SkimHdWebWho`
  PUBLIC / LOCAL / MANAGER in the GET hooks). `[feed] address` (host:port or
  a telnet/http(s) URL) is the feed's address in the header, for everyone.
  Rules: `docs/WEB-SECURITY.md`.
- **Adaptive RTTY** (`[decode] rtty_adaptive`, default off;
  `src/headless/governor.c`): the RTTY decoders are on at each full UTC
  hour and at start for `rtty_adaptive_minutes` (2); more than
  `rtty_adaptive_spots` (100) RTTY spots on all bands keeps them on, else
  off till the next hour. A paused mode gets no IQ (`Dec.paused` in
  `rx_iq_cb`; its engine thread idles) and is NaN in the stats.
- **The thermal guard** (`[thermal] enabled`, default off; `governor.c`):
  the 5-min mean of the hottest CPU sensor over `limit_c` (empty = the
  sensor's crit − 15 °C, 85 on coretemp; else 80) now and for 3 of the
  last 5 min (counted in a window — a dip under the limit does not restart
  it; counted on end, 2026-11-20 it held one band off for 70 min at minute
  peaks of 85–100 °C and never paused the next) pauses the
  local band with the fewest spots a minute (CW + RTTY, over the minutes
  of the last 30 it decoded), 10 min later the
  next; one always stays. The drop each pause brought (10 min after) is
  its hysteresis: it comes back (LIFO) once the mean holds ≤ limit −
  max(drop, 5) − `restore_margin_c` (3) for 20 min. hra2 (2026-11-20):
  mean ~78 °C, minute peaks up to 100 — so the mean, never the peak.
  The rotation (2026-11-22 at Dan's request; `probe_every_min` 30, 0 =
  off, `probe_min` 5): a paused band has no spots, so on a machine hot
  all day it never came back; one paused that long is on for 5 min in
  place of the weakest band decoding (same CPU) and stays if it spotted
  more a minute (≥ 2 spots), else each goes back. 5 min, because an open
  band spots in its first minute (hra2 11-22 after a restart). The
  Status table marks it TEST; pause/restore wait during a test.
- **WSPR** (`[wspr] enabled`, default off; `src/headless/wspr.c`,
  2026-11-21 at Dan's request): every WSPR-2 dial (WSJT-X's list) whose
  dial + 1500 ± 150 Hz lies inside 45 % of the rate around a received
  band's centre (the first receiver once) — at 96 kHz from the low edge
  that leaves out 20/15/10 m. `rx_iq_cb` → shift, WDSP to 12 kHz, then 375 Hz;
  a 114 s capture from each even UTC minute (the wall clock, the arrival
  smoothed) → a `.c2` file → `skimmer-wsprd` (WSJT-X 3.0.2's wsprd,
  `vendor/wsprd/`, C only: the Fortran OSD is a stub, never called; `-C 500
  -w`, `deep=true` adds -d, `subtract=false` adds -s; nice 10) on one thread, band after band. Spots 24 h in memory +
  `<state>/wspr.tsv`; `/api/wspr` = the stations (Statistics, under the
  bands' table); `upload=true` posts each spot to WSPRnet
  (`function=wspr`, as WSJT-X) under `[wspr] call`/`locator` (empty = the
  feed's call, the station's locator — needed then); a failed post waits
  for the next cycle. The `.c2` stores Q negated (wsprd's convention) —
  the `wspr` gate decodes two synthetic signals at 48 and 192 kHz, so a
  mirrored spectrum fails it. Needs a live check on air.
- **FT8/FT4** (`[ft8] enabled`, default off; `src/headless/ft8.c`,
  2026-11-22 at Dan's request): every FT8/FT4 dial (WSJT-X's list) whose
  dial + `lo_hz`…`hi_hz` (200–3500) lies inside 45 % of the rate around a
  received band's centre (`bands=`: only the [bands] names listed —
  2026-11-22, Dan: one band first, hra2 runs hot). `rx_iq_cb` → shift, WDSP to 12 kHz (240 taps),
  a sharp 12 k → 12 k filter (800 taps: nothing below the dial, or USB's
  mirror hears it), back up and the real part = USB audio, the dial at 0 Hz;
  each UTC cycle (15 s / 7.5 s) → a WAV → `skimmer-ft8d` (WSJT-X 3.0.2's
  classic ft8_decode / ft4_decode in Fortran, `vendor/wsjtx-ft/`, driven as
  jt9 does from disk; not ft8_lib — half the decodes on a busy band) on a
  pool of `threads` (2), nice 10; a cycle still waiting when its dial's next
  one is done is skipped (counted). Every station that sends (CQ caller, the
  2nd call of a standard message; no hash `<…>`, no AP decode) is a spot
  once a call+band+mode in 10 min, at dial + its audio frequency, on the
  telnet feed as `FT8  -12 dB  CQ` / `DE` (`skim_rbn_feed_spot_digital`, the
  CW line byte-identical) — never on the RBN Aggregator's port (RBN takes
  FT8 its own way); `feed=false` = the page only. Off on a worker (the hub
  decodes its IQ), off with the thermal guard's band. Decodes of 15 min for
  the page (`skim_hd_ft8_decodes_json`); status.json `ft8`. Real air
  (golden, depth 2): busy 20m ≈ 41 FT8 decodes a cycle, the decoder ≈ 16 %
  of a core for 20m FT8+FT4, the DSP ≈ 3 % a band. `skimmer-ft8-replay
  FILE.cf32 [--keep DIR]` is the bench. The page: Settings → FT8 / FT4
  (the dials the bands take, live), FT8 and FT4 parts in the
  bands' ladder, FT8/FT4 in the heat map (weighed as WSPR) and the DX
  cluster's mode filter (its dB floor −30 since FT8), the windows violet on
  the waterfall ("FT8 RX"); the FT8 palette (`web/ft8-palette.js`; FT8 in
  the bar, a click on an FT8/FT4 spot) is WSJT-X's Band Activity from
  `/api/ft8?dial=&since=` — it narrows the waterfall to the dial's window,
  draws each cycle's start and stamps each sender upright on its trace into
  the picture, and gives the view back on close. `upload=true`
  (`src/headless/pskreporter.c`) reports the stations heard to PSK Reporter
  as WSJT-X does: IPFIX over UDP to report.pskreporter.info:4739 (the page's
  templates: receiver R1, sender with sNR/iMD/locator), one source port, a
  datagram every 5 min (not on the clock's minute) or when full (1400 B),
  the templates in the first three and hourly, each call once an hour a
  band and mode, its locator from any message it sent; under `[ft8]
  call`/`locator` (empty = [feed] call, [station] locator — needed then);
  `pskreporter=host:port` is the gates' stand-in (a datagram a second).
  Needs a live check on air.
- **Updates** (`[update]`, default `mode=auto`, `hour_utc=3`;
  `src/headless/update.c`, 2026-11-22 at Dan's request): once a day the
  GitHub API's latest release of ok1hra/skimmer-for-linux; a newer one is
  downloaded beside `$APPIMAGE`, checked against the asset's `digest`
  (none = refused) and run once (`--version`, `--check-config`), then at
  the hour renamed over the running file (old one `.prev`) and exec'd
  (`skim_hd_update_exec`: every fd close-on-exec, so the old FUSE mount
  goes). Three crashes before 15 min of running roll back and skip that
  version. Only a release AppImage updates — `-Dbuild_suffix` (build.sh
  sets `-g<commit>`) marks a dev build, which only reports. Managers only:
  GET/POST `/api/update`. Gate `update` (file:// mirror, fake AppImage).
- Opt-in and without a live verdict: the tone splitter (`SKIM_TONE_SPLIT=1`)
  and the narrow focus slot (`SKIM_TONE_FOCUS=1`, implies the splitter).

## Releasing

One artifact: the AppImage (`packaging/appimage/`; deb, rpm and the AUR
package went with the GUI, 2026-10-22). Running skimmers update themselves
from it (`[update]`): the asset must be named exactly
`skimmer-headless-X.Y.Z-x86_64.AppImage` and the release must be neither
a draft nor a prerelease (a prerelease = testing by hand). Tag first; `gh release create
--verify-tag --notes-file` right after the tag push, so CI (build.yml) builds
the AppImage in debian:12, smoke-tests it in a clean debian:12 and attaches
it to the curated release instead of an empty one. Release titles carry no
epithet: "Skimmer for Linux 0.7.0", tag message "release: v0.7.0". A new
pinned download (linuxdeploy, ONNX Runtime) = new URL + sha256 in
`packaging/appimage/build.sh`.

## Layout

```
src/engine/   headless, GLib-only:
  tci_client   WS client, IQ ingest (true orientation), outgoing SPOT
  hpsdr_p1     HPSDR Protocol 1 UDP client (Red Pitaya) — the non-TCI IQ source
  fftw_lock    the one lock around FFTW plan creation/destruction
  channelizer  polyphase filter bank → complex baseband per channel
  decode.h     backend interface: channel → { text, confidence, freq, wpm/baud }
  decode_cw    CW backend (phase 1: v1 classical + v2 Viterbi)
  decode_rtty  RTTY backend (M7: Baudot FSK 45.45 Bd); later decode_psk
  decode_deepcw  DeepCW neural CW backend (Conformer + CTC via ort_shim/dlopen)
  station      per-frequency station tracker
  callsign     extraction + validation (RBN-grade)
  spot_out     TCI SPOT feed + RBN telnet feed
src/headless/ the product layer: config (headless.ini, one parser), source
              (IQ source interface) + source_hpsdr_p1 / source_mock /
              source_hub (a worker), hub (bands to workers), peers (other
              feeds), discovery (UDP 8075), record ([record] IQ to disk),
              switches (SKIM_* visibility), stats (7-day history), web
              (libsoup server), logring, scp_update (MASTER.SCP updater),
              wspr ([wspr]: WSPR beside, WSPRnet), ft8 ([ft8]: FT8/FT4
              beside, to the feed), update ([update]: a
              newer release swapped in)
web/          the web page (HTML, CSS, JS — built in as a GResource)
src/headless_main.c  skimmer-headless: multi-band, no GUI (HPSDR P1 → N pipelines)
weights/      learned weights built into skimmer-headless (feed-gate.ini)
tests/golden/ the golden gate: real-air replay, byte for byte
vendor/wdsp/  in-tree WDSP copy (FFT + resampler)
vendor/wsprd/ WSJT-X's wsprd (the WSPR decoder → skimmer-wsprd, a helper)
vendor/wsjtx-ft/ WSJT-X's FT8/FT4 decoders (Fortran → skimmer-ft8d, a helper)
vendor/onnxruntime/  ONNX Runtime C API header only (MIT) — dlopen at run time
```
