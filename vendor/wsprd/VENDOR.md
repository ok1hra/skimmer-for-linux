# Vendored wsprd (WSJT-X) — SUBSET

The WSPR decoder of K1JT's WSJT-X, `lib/wsprd/` — **we do not reimplement
the decoder and we do not modify these sources** (warnings are silenced in
`meson.build`, never patched). `skimmer-headless` writes one band's
2-minute cycle as a `.c2` file (complex, 375 samples/s, centred on dial +
1500 Hz) and runs `skimmer-wsprd` (this `wsprd.c`, built here) on it.

## Source of truth

| | |
|---|---|
| Authors | Joe Taylor K1JT, Steven Franke K9AN (wsprd); Phil Karn KA9Q (fano.c); Bob Jenkins (nhash.c) |
| Upstream | WSJT-X 3.0.2, https://sourceforge.net/projects/wsjt/ (`lib/wsprd/`) |
| Taken from | Debian's source tarball `wsjtx_3.0.2+dfsg.orig.tar.xz`, https://deb.debian.org/debian/pool/main/w/wsjtx/, sha256 `df0fd7d0fef2cf6a7316b78638138c32e6b7f0f0c9d4c207ebef733ab36749c4` |
| Vendored on | 2026-11-21 |
| License | GPL-3 (`COPYING`, WSJT-X's) — compatible with this project's GPL-3.0-or-later |

Each file here is byte-identical to that tarball's `lib/wsprd/` (COPYING is
the tarball's top level), except `osd_stub.c`, which is ours.

## What we build

- **`skimmer-wsprd`** = `wsprd.c` + `wsprd_utils.c`, `wsprsim_utils.c`,
  `nhash.c`, `tab.c`, `fano.c`, `jelinek.c` (`metric_tables.c` is
  `#include`d by wsprd.c), linked with fftw3f.
- **`osd_stub.c`** (ours) stands in for `osdwspr.f90` (+ `../indexx.f90`):
  the ordered-statistics decoder is Fortran, and wsprd calls it only with
  `-o N` — off by default, and skimmer-headless never passes it. So the
  build needs no Fortran compiler and the AppImage no libgfortran. The cost
  is the OSD's few extra decodes of calls already in the hash table.
- **`wsprd-enc`** (a static library of the same utils) gives the `wspr`
  gate wsprsim's encoder (`get_wspr_channel_symbols`) for its synthetic
  signal.

Not taken: the Fortran (`*.f90`), `wsprsim.c`, `gran.c`, `mettab.c`, the
obsolete unpacker, Makefiles other than the reference `Makefile`, the local
`fftw3.h` (the system's is used).

## Updating

Replace the files from a newer WSJT-X `lib/wsprd/`, update the table above,
run `meson test wspr`. A fix belongs upstream (wsjt-devel), not here.
