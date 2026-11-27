# fpga/ — from Pavel Demin's red-pitaya-notes

- Upstream: https://github.com/pavel-demin/red-pitaya-notes
- Pinned commit: 71c3402a ("fix model in iir_test"), taken 2026-11-27
- Licence: MIT (`LICENSE.red-pitaya-notes`)

Copied unchanged: `cores/` (all of it — `scripts/project.tcl` adds
`cores/*.v`, an unused module costs nothing), `modules/`, `cfg/`,
`scripts/project.tcl`, `scripts/bitstream.tcl`, and
`projects/sdr_receiver_hpsdr_122_88/` without its `server/` and `app/`
(the skimmer's agent replaces the server: `rp/agent/`). That project is
the stock bitstream the managed board runs, kept to build it from this
tree and compare.

Ours: `Makefile` (the bitstream targets of his, nothing else),
`build.sh`, `scripts/report.tcl`, `projects/skimmer_rp_122_88/`, the
blocks named in `docs/FPGA.md`.

A newer upstream: copy the same paths again, note the commit here, and
rebuild `sdr_receiver_hpsdr_122_88` first — it must still build with
timing met before anything of ours is rebuilt on it.
