# vendor/js8call — JS8Call-improved's JS8 decoder (→ skimmer-js8d)

A copy (not a submodule) of the decoder core of **JS8Call-improved** (GPL-3,
`COPYING`), built without Qt the way **WIFILT** (ok1hra/wifilt, GPL-3) builds
it natively: its `prototype/js8-core-prototype/decoder_shim/`. JS8 in
skimmer-headless (`src/headless/js8.c`) runs it as a helper program, like
skimmer-ft8d and skimmer-wsprd.

- Upstream subset as pinned in WIFILT `JS8Call-improved-master/`, commit
  `fd86b55057e44d2f650f163995517ee9e97e21cf` (2026-11-17), copied 2026-11-24.
- WIFILT's shim from the same commit.

## Why this decoder

JS8Call-improved is the maintained JS8Call and its decoder the most
sensitive one (soft combining of repeats, LDPC feedback/rescue passes, SIC,
frequency and timing tracking). It dropped the OSD of the Fortran js8call
2.x on purpose: OSD over JS8's 12-bit CRC makes false frames, and a skimmer
must not spot those. WIFILT already runs the same core (native and WASM), so
both projects read the same frames from the same air.

## What is here

`upstream/` — byte-identical files of WIFILT's pinned subset (sha256):

```
d7abc3090fcf5516b64435f78dfbd08dff9b01a56905afbe4d69fa4eb9c16048  JS8_Include/commons.h
d7054541dafdc93d54c04461b80d616210a90256b743a160d75622cb060dc1ae  JS8_Mode/FrequencyTracker.cpp
91a872349669353f6729e6526f178b5af9251318744ace8c735823c3d41b701d  JS8_Mode/FrequencyTracker.h
b72787f6e6473919872103997ef5dc9847810ff3e7783124a3961a2da2a86bf1  JS8_Mode/JS8.cpp
c9bb4ccc8f3def26952a29e97a88f4d912f7f79ed6006e843601b9d30fb974d0  JS8_Mode/ldpc_feedback.h
59a2c7dafab922a1aa93d7de7c57a50fa63d6fd900507c63233390de073218ea  JS8_Mode/soft_combiner.h
bcd61a557030caba012b501d3ea851ab0cfda16c8ec4d01ddc902bebec86fb45  JS8_Mode/whitening_processor.h
af01e01a85739cd52a69a6e2d3bc9fc4f7bd3a57002a697d97861b9b41fe11d0  LICENSE
```

`upstream/vendor/Eigen/` — the part of upstream's bundled Eigen 3.4.0
(MPL-2.0, see each file's header) that `<Eigen/Dense>` reaches: the module
headers Core, Dense, LU, Cholesky, QR, SVD, Geometry, Eigenvalues,
Householder, Jacobi and their `src/` directories plus `src/misc`,
`src/plugins`. JS8.cpp uses it for one small Vandermonde solve (the noise
baseline).

`shim/` — WIFILT's `decoder_shim/` files, unchanged: `JS8.h` (the public
header without Qt), `QDebug`, `QLoggingCategory`, `QtGlobal`, `qt_shim.cpp`
(the globals JS8.cpp expects), `decoder_c_api.h` + `decoder_wrapper.inc`
(the C API `js8_reference_decode()`).

Ours:
- `extract_core.py` — the same cut as WIFILT's
  `cmake/extract_reference_core.cmake`: JS8.cpp up to `// Worker` (the Qt
  worker is left out), its encoder from `// Public Interface - Encoding`,
  then `decoder_wrapper.inc`. Refuses a JS8.cpp whose sha256 is not the pin
  in `meson.build`.
- `js8d_main.cpp` — the program `skimmer-js8d` (one-shot over WAV files, or
  `--serve` with jobs on stdin; the line format is in its header).
- `js8code_main.cpp` — test-only `skimmer-js8code`: a frame's 79 tones from
  upstream's `JS8::encode`.
- `meson.build`.

Not taken: the rest of JS8Call-improved (Qt UI, Detector, Modulator,
JSC_map.cpp — the JSC text dictionary is unpacked in the browser by WIFILT's
`js8-protocol.js`, `web/js8/`), the Eigen modules Dense does not include.

## Rules

- Never edit `upstream/` or `shim/` here. A shim fix goes to WIFILT first
  and is copied back; a new upstream is reviewed, copied over and the
  sha256 pin in `meson.build` (and the table above) changes.
- Build needs a C++20 compiler and Boost's headers (crc, multi_index) at
  build time only; the helper links fftw3f and libstdc++.

## Updating

1. Copy the files above from a newer WIFILT (or JS8Call-improved), refresh
   the table, the pin in `meson.build`.
2. `meson test js8d js8` — the synthetic frames of every submode must still
   decode, and `skimmer-js8-replay` over the golden recordings should not
   lose frames.
