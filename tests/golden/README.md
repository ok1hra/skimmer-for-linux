# Golden gate

`meson test golden` proves that a change did not move the CW decoder, the
RTTY decoder or the RBN feed by one byte. It replays real air recorded on hra2 through the whole
engine with an empty environment (no `SKIM_*` switches, code defaults only):

| Input | What it runs |
|---|---|
| `g20m-day`, `g20m-eve`, `g30m-day`, `g40m-night`, `g40m-rtty` — 10 min of 96 kHz IQ each | `skimmer-replay` in CW with the feed on: decode log, feed lines, station table |
| `g40m-rtty` (JARTS WW RTTY contest, 30 stations, 107 feed lines), `g20m-eve` (an RTTY net, no CQ) | the same in RTTY (`rtty-<slice>/`) |
| `tap-<band>.log` — the first hour of the 2026-10-17 20:17Z decode tap, 7 bands | `skimmer-tap-replay` with the learned gate in shadow: gate rows, faithfulness |
| `tap-40m-rtty.log` — the RTTY decode tap of `g40m-rtty` (`SKIM_REPLAY_TAP`) | `skimmer-tap-replay` in RTTY (the header says `engine=rtty`), no learned gate |
| `scp/master.scp`, `feed-gate-20261006.ini` | frozen, so the reference cannot drift with the SCP updater |

The inputs (2.4 GB) are not in git. `inputs.sha256` pins them,
`expected.sha256` pins the outputs. They live in `/var/tmp/skimmer-iq/golden`
on hra2 (`SKIM_GOLDEN_DIR` overrides). Without them the gate reports SKIP.
CI therefore skips it, and the gate is run on hra2.

A mismatch prints the outputs that moved and the head of each diff against
the full reference in `<golden dir>/ref/`.

**A refactor must leave the gate green.** A deliberate behaviour change
regenerates the reference in its own commit and says why:

    tests/golden/run.sh builddir/skimmer-replay builddir/skimmer-tap-replay --update

The slices came from these recordings (`dd bs=768000`, one second a block):

| Slice | Source | Offset | UTC |
|---|---|---|---|
| g20m-day | hd-20m-20261016-124545Z | +3600 s | 2026-10-16 13:45:45 |
| g20m-eve | hd-20m-20261017-201737Z | +1800 s | 2026-10-17 20:47:37 |
| g30m-day | hd-30m-20261016-124545Z | +3600 s | 2026-10-16 13:45:45 |
| g40m-night | hd-40m-20261015-210146Z | +3600 s | 2026-10-15 22:01:46 |
| g40m-rtty | hd-40m-20261017-201737Z | +3300 s | 2026-10-17 21:12:37 |
