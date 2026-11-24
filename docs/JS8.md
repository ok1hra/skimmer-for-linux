# JS8 — decisions, numbers, rejected paths

`[js8]` decodes JS8 (JS8Call) beside CW/RTTY/FT8/WSPR. Dan's brief
(2026-11-24): put it in every statistic and bar like the other modes,
and design it for **deep detection below the noise on chosen bands,
without compromise**. Code: `src/headless/js8.c`, `js8_unpack.c`,
`digi_rx.c`, `vendor/js8call/`, `web/js8-palette.js`, `web/js8/`.

## The decoder: JS8Call-improved, through WIFILT's shim

- **Chosen:** the decoder core of JS8Call-improved (`JS8_Mode/JS8.cpp`,
  C++20). It is the maintained JS8Call, and its decoder is the most
  sensitive one: soft combining of repeated frames, LDPC feedback and
  rescue passes, SIC, frequency/timing tracking, whitening. It is built
  without Qt as WIFILT (ok1hra/wifilt) builds it natively and as WASM
  (`prototype/js8-core-prototype/decoder_shim`): the same pinned upstream
  subset (JS8.cpp sha256 `b72787f6…86bf1`), the same cut. Both projects
  read the same frames from the same air.
- **Rejected: js8call 2.x's Fortran** (`lib/js8/*.f90`, the old WSJT-X 1.9
  FT8 code, depth up to 4 with OSD). OSD over JS8's 12-bit CRC makes false
  frames — the reason JS8Call-improved left OSD out ("depth fixed at 2") —
  and a skimmer must not spot them. It would have fitted the gfortran
  toolchain, but its sensitivity is below the improved decoder's.
- **Rejected: decoding in the browser** (WIFILT's WASM decoder on the
  waterfall's audio, as the RTTY palette decodes). The waterfall audio is
  8 kHz µ-law with an AGC, only while a palette is open, only the band the
  audio carries; the server decodes the full IQ of every dial all the time.
  The browser gets raw frames and only reads them (WIFILT's
  js8-protocol.js + the JSC dictionary): no 0.9 MB WASM for visitors.

## Depth

- **Every submode, every period:** normal (15 s, 6.25 Hz tones), fast
  (10 s), turbo (6 s) and slow (30 s, 3.125 Hz — JS8Call's −28 dB). A
  skimmer cannot know which one a station uses; slow is the deepest and
  is decoded too. Ultra (4 s) is disabled in JS8Call itself — left out.
- **One front end a dial** (`digi_rx.c`): the shift, WDSP to 12 kHz, the
  sharp window filter, USB audio, cut into each submode's UTC periods.
  FT8 uses the same code with one period; its WAVs are byte-identical to
  before (`skimmer-ft8-replay --decoder /bin/true --keep` on golden 20 m).
- **The window:** 200–3500 Hz above the dial (as FT8's).
- **One decoder process a dial kept running** (`serve=true`): upstream's
  `DecodeMode` keeps its soft combiner between calls, so a repeated frame
  (a heartbeat every period) is combined across periods. It is also ten
  times cheaper than one run a period (the FFTW plans and tables are built
  once): golden g20m-day, 600 s, all four submodes — 14.4 s CPU kept
  running, 144 s once a period, the same 9 frames.
- **Not done (yet):** patching upstream's thresholds (ASYNCMIN, candidate
  counts) or an a-priori pass on known calls. Both change JS8.cpp, which
  stays byte-identical; to be measured on recordings with weak JS8 before.

## Deep frames and the feed

JS8 carries a 12-bit CRC only. The decoder reports quality = 1 − hard
errors/60 (bpdecode's mismatches against the codeword it found, of 174).

- `clean` (default **0.80** = up to 12 hard errors): from it a frame is
  clean and its sender goes to the feed at once.
- Under it the frame is **deep**: the call is on the page (spot log, DX
  cluster, map, stats, palette — faded with "?") at once, on the telnet
  feed and PSK Reporter only once **confirmed**: a second decode of the
  call in another period within 30 min, MASTER.SCP, or `js8-known.tsv`
  (calls confirmed on JS8 in the last 90 days, kept in the state dir).
- Golden 40 m night: deep frames at −19…−22 dB (SNR as the decoder
  reports it, in 2500 Hz) with quality 0.68–0.78 — M9RIP, KN4AM, VO1PAT:
  real calls, held for the page until a second decode or the SCP.

## Measured (golden recordings, 96 kHz, 600 s each, all four submodes)

| recording | frames | senders spotted | decoder CPU | DSP |
|---|---|---|---|---|
| g20m-day | 9 | 2 | 14.4 s | 10.6 s |
| g40m-night | 12 | 7 | 13.7 s | 10.2 s |
| g30m-day | 1 | 1 | 12.3 s | 10.5 s |

≈ 2.3 % of a core a band for the decoder, ≈ 1.8 % for the front end
(hra2, Intel, one thread each). Slow (submode 4) was in use on 40 m
(M0IQF). Synthetic CPFSK through skimmer-js8d alone (normal, two
heartbeats, white noise, SNR in 2500 Hz by the signal's power): 10/10 at
−16 dB, 8/10 at −18, 7/10 at −19; the decoder's own SNR reads ~7 dB
lower than that definition.

## The palette

`web/js8-palette.js` polls `/api/js8?dial=&since=` (the last 90 s again
each time: a slow period's frames come after a faster one's of a later
start) and feeds WIFILT's `Js8Protocol.ActivityStore`, which puts frames
back into messages (first/last flags, compound pairs, gaps, the directed
checksum) and unpacks Huffman and JSC text with `js8-jsc.bin` (JSC3,
262 144 words, 1.9 MB, fetched when the palette first opens). On golden
20 m: "F4LPU: DJ3EI INFO YAESY FT-891 (30W)- MFC-750 - JN15QS".

## Still open

- A live check on air (`needs-live-check`): frames, deep frames held and
  confirmed, CPU and temperature on hra2 with `[js8]` on 1–2 bands.
- A bench against JS8Call itself on the same audio (not installed here):
  replay `--keep` WAVs into JS8Call-improved through a PulseAudio null
  sink, compare frame by frame.
