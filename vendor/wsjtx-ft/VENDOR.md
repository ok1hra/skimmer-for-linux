# Vendored WSJT-X FT8/FT4 decoders — SUBSET

The FT8 and FT4 decoders of K1JT's WSJT-X (Fortran). **We do not
reimplement the decoders and we do not modify these sources** (warnings are
silenced in `meson.build`, never patched). `skimmer-headless` writes one
dial's 15 s (FT8) or 7.5 s (FT4) cycle as a 12 kHz mono 16-bit WAV and runs
`skimmer-ft8d` on it, as WSJT-X's `jt9` would run from disk.

Why WSJT-X and not ft8_lib (C, MIT): on a busy 20 m band ft8_lib decodes
about half of what WSJT-X does, and RBN's FT8 nodes run WSJT-X (CWSL_DIGI
→ jt9). Decided with Dan OK1HRA, 2026-11-22.

## Source of truth

| | |
|---|---|
| Authors | Joe Taylor K1JT, Steven Franke K9AN, Bill Somerville G4WJS and the WSJT-X team |
| Upstream | WSJT-X 3.0.2, https://sourceforge.net/projects/wsjt/ (`lib/`) |
| Taken from | Debian's source tarball `wsjtx_3.0.2+dfsg.orig.tar.xz`, https://deb.debian.org/debian/pool/main/w/wsjtx/, sha256 `df0fd7d0fef2cf6a7316b78638138c32e6b7f0f0c9d4c207ebef733ab36749c4` (the same one as `vendor/wsprd`) |
| Vendored on | 2026-11-22 |
| License | GPL-3 (`COPYING`, WSJT-X's) — compatible with this project's GPL-3.0-or-later |

Each `.f90` / `.f03` here is byte-identical to that tarball's `lib/` at the
same relative path (`ft8/`, `ft4/`, `ft2/`, `77bit/` kept as subdirectories,
because the sources `include` each other by those paths). COPYING is the
tarball's top level. Two files are ours:

- **`ft8d_main.f90`**: the program `skimmer-ft8d`. It stands in for `jt9` +
  `multimode_decoder` (`lib/jt9.f90`, `lib/decoder.f90`), which would pull
  in JT65/JT9/Q65/FST4/SuperFox and Qt's shared memory. It drives
  `ft8_decode` / `ft4_decode` exactly as `jt9` does from disk (FT8: passes
  at nzhsym 41, 47 and 50, FT4 once; AP off, no mycall/hiscall) and prints
  `jt9`'s own lines, so the two can be compared on the same WAV.
- **`wsjtx_stubs.c`**: `crc14/crc13/crc10` (+ `_check`) in place of
  `lib/crc1?.cpp` (boost's `augmented_crc`, the same polynomials), and
  `shmem_lock/unlock` in place of `lib/shmem.cpp` (Qt). So the build needs
  no C++ compiler, no boost and no Qt; the AppImage gains only libgfortran.

The classic single-threaded FT8 decoder is used, not 3.0's multi-threaded
`ft8var` (JTDX's "ft8md", OpenMP): jt9 also defaults to the classic one
(`multift8 = .false.`), and skimmer-headless already runs many bands.

## What we build

- **`wsjtx-ft`** (static): the decoders, `packjt77` and their helpers.
- **`skimmer-ft8d`** = `ft8d_main.f90` + `wsjtx-ft`, linked with fftw3f;
  installed and put in the AppImage beside `skimmer-wsprd`.
- **`skimmer-ft8code`**, **`skimmer-ft4code`** (upstream `ft8/ft8code.f90`,
  `ft4/ft4code.f90`): test-only, they print a message's channel tones, so the
  `ft8` gates synthesize signals without an encoder of our own.

Built only with a Fortran compiler (`-Dft8=auto`, the default; `enabled`
makes a missing gfortran an error, `disabled` skips it).

Not taken: everything else in `lib/` — the other modes, `jt9*.f90`,
`decoder.f90`, `ft8var/`, the C++ and Qt sources, the simulators.

## Updating

Replace the files from a newer WSJT-X `lib/` (same paths), update the table
above, check that `ft8_decode`'s / `ft4_decode`'s `decode` arguments still
match `ft8d_main.f90`, run `meson test ft8d ft8`. A fix belongs upstream
(wsjt-devel), not here.
