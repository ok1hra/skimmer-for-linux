# Skimmer for Linux — headless multi-band skimmer

> **This is a fork.** It is maintained by Dan, OK1HRA, from
> [OK1BR/skimmer-for-linux](https://github.com/OK1BR/skimmer-for-linux).
> Skimmer for Linux is the work of Richard Fakenberg, OK1BR. The fork has
> dropped the GTK application: it is a **multi-band skimmer without a GUI**,
> fed by a Red Pitaya. Please report problems with it here, not upstream.

`skimmer-headless` takes IQ from one **Red Pitaya** running Pavel Demin's
`sdr_receiver_hpsdr` (HPSDR Protocol 1), up to 8 receivers at once, one band
each. Every band runs its own pipeline: a channelizer splits the band into
125 Hz channels, each channel runs a CW decoder, and validated callsigns go
through a station table and a feed policy. The CQ spots of all bands go to
**one** telnet feed in the cluster dialect, the CW Skimmer Server model.
Status comes as a table on the console and as a web page with `/status.json`.

```
Red Pitaya ──UDP IQ, 1..8 receivers──► skimmer-headless ──► per band: channelizer ──► CW decode
                                               │                                         │
                                               └── telnet cluster feed ◄── callsigns ◄───┘
```

The skimmer is **read-only towards the operator's radio**. The Red Pitaya is
the skimmer's own receiver: it sets that receiver's DDC centres, rate and
receiver count, never keys, and never takes a receiver another client is
streaming from unless told to (`--take-over`).

## What it does

- **CW**, decoded by the soft-decision Viterbi decoder (v2, the default) or the
  neural **DeepCW** model when an ONNX Runtime and the model are installed.
  The decoder and the RBN feed are tuned on live air against CW Skimmer
  Server (v0.6.0): IQ recorded beside live decoding was replayed and scored on
  the same antenna, and the winners became the defaults. Method, numbers and
  what was rejected: [`docs/LIVE-AIR.md`](docs/LIVE-AIR.md).
- **RTTY** (45.45 Bd, 170 Hz shift) beside CW on the bands that ask for it
  (`20m=14045500 cw,rtty`): a second pipeline on the same IQ, 250 Hz
  channels, its spots on the same feed as `RTTY … 45 BPS` (`[feed]
  rtty=false` keeps them off it). It costs about 7.5 % of one core per band
  at 96 kHz.
- **The RBN feed** sends only validated CQ calls that were read twice (or are
  in MASTER.SCP), settled in the station table, and were read lately. A
  **learned feed gate** (a logistic model over what the pipeline knew when it
  spotted) is built in and runs in shadow: it scores every spot and reports,
  the hand gate decides. See [The feed gate](#the-feed-gate).
- **The Super Check Partial call list keeps itself current** in the background.
- **Learning and measuring:** a decode tap per band (`[feed] learn=true`), raw
  IQ recordings (`[record]`), and `skimmer-compare/`, which scores the feed
  against another skimmer with the Reverse Beacon Network as the referee.

An AppImage and a systemd user service are being built.

## Build

Requirements: GLib/GIO, libwebsockets, libcurl ≥ 7.85, FFTW (single and
double precision), `meson` and `ninja`. On Debian 13:

```sh
sudo apt install meson ninja-build libglib2.0-dev libwebsockets-dev \
                 libfftw3-dev libcurl4-openssl-dev
git clone https://github.com/ok1hra/skimmer-for-linux.git
cd skimmer-for-linux
meson setup build
meson compile -C build
meson test -C build            # offline gates, no radio needed
```

The build produces `build/skimmer-headless` and the tools below. The `golden`
gate replays recorded real air through the whole engine and needs inputs
that are not in git; without them it reports SKIP
([`tests/golden/README.md`](tests/golden/README.md)).

## Run

```sh
./build/skimmer-headless [--config FILE] [--take-over] [--status-every S]
                         [--http-port N] [--feed-gate FILE]
                         [--feed-gate-mode off|shadow|on]
                         [--check-config] [--list-switches]
```

`--check-config` reads and checks the config and the feed gate, prints what
would run and exits — no radio is touched. A config that does not parse
stops the start with exit 2 and names the bad line.

The first run writes `~/.config/skimmer-for-linux/headless.ini` with defaults.
Here is an example for seven bands at 96 kHz:

```ini
[radio]
# one radio, or a list tried in order: the first idle one is used;
# host@ppm gives that radio its own sampling-clock correction
host=192.168.1.21@3.81,192.168.1.71
# 48000 / 96000 / 192000, one rate for all receivers
rate=96000

[bands]
# name=centre Hz [modes], in receiver order (RX1 first), at most 8;
# modes: cw (default), cw:v2, cw:deepcw, rtty — e.g. 40m=7045500 cw,rtty
160m=1845500
80m=3545500
40m=7045500 cw,rtty
30m=10145500
20m=14045500 cw,rtty
17m=18113500
15m=21045500

[decode]
# v2 (classical) or deepcw (needs ONNX Runtime + model)
engine=v2
log=false

[feed]
# telnet spot feed, validated CQ spots only; port 0 = off
call=YOURCALL
port=7300

[status]
# console table every N s (0 = off); web page port (0 = off)
console_s=10
http_port=8073
```

Point a logger or a cluster client at `telnet <host> 7300`. Ctrl+C stops the
skimmer cleanly and frees the radio.

## The web page

`http://<host>:8073/` shows the skimmer: **Status** (radio, CPU, loss, the
feed, every band with its newest stations, the feed policy, the learned gate
and the `SKIM_*` switches), **Statistics** (seven days, one record a minute,
kept across restarts in `~/.local/state/skimmer-for-linux/stats.bin`: spots
per hour by band, active decoders, CPU, IQ loss, totals), **Settings** and
**Log**. It needs no internet; the JSON behind it is `/status.json`,
`/stats.json`, `/api/log` and `/api/config`.

**Settings** edits the radio, the bands (order, centre, CW engine, RTTY), the
feed's call and port, the status ports and the IQ recording. *Save and
restart* checks the new config with the same parser the start uses — a bad
value is refused and nothing is written — keeps the previous file as
`headless.ini.bak-<UTC>` (the last 10; *Earlier configs* restores any of
them) and restarts the skimmer. A start that does not get the radio
streaming within a minute of a change offers the previous config back. The
feed policy, the learned gate and `[web]` are not editable on the page.

Only the addresses in `[web] settings_from` (default `127.0.0.1,::1`) may
change settings; everyone else sees them read-only. From another machine
use a tunnel: `ssh -L 8073:127.0.0.1:8073 <host>`, then
`http://localhost:8073/`. A restart re-executes the skimmer; under a
supervisor that sets `SKIM_SUPERVISED=1` it exits with code 75 instead.

`[radio] type=mock` replaces the radio with synthetic CW on every band (the
headless gate uses it). When every listed radio is busy, or none answers within 30 s, the skimmer
says which and exits with code 3 — also when the stream is lost later; run it
under a supervisor that restarts it. Seven receivers at 96 kHz are about
35 Mb/s of UDP, so **use a wired link**. Over Wi-Fi, 192 kHz lost about
30 % of the packets.

## The feed gate

The learned gate's weights are part of the code
([`weights/feed-gate.ini`](weights/feed-gate.ini)); a new model is a new
commit of that file. A start may run another model or another mode:

| Switch | Config | Effect |
| --- | --- | --- |
| `--feed-gate FILE` | `[feed] gate=FILE` | Run this model instead of the built-in one. It must name every feature, or the start stops (exit 2). |
| `--feed-gate-mode shadow` | `[feed] gate_mode=` | Default: score every spot and report, the hand gate decides. |
| `--feed-gate-mode on` | | Hold back the spots the model scores below its threshold. |
| `--feed-gate-mode off` | | No learned gate. |

The command line wins over the config. The log and `/status.json`
(`feed_gate.id`) name the model as `builtin:feed-gate.ini#<sha8>` or
`file:<name>#<sha8>`, so a measurement can tell its periods apart.

## Tools

| Binary | What |
| --- | --- |
| `skimmer-hpsdr-probe` | Live check of a receiver: discovery, packet loss, effective rate, the strongest peaks at absolute frequencies, optional IQ dump |
| `skimmer-replay` | Decode a recorded `.cf32` offline through the whole pipeline, with the feed traced |
| `skimmer-tap-replay` | Replay a decode tap through the feed policy and the learned gate |
| `skimmer-cw-bench` | CW decoder bench on synthetic keying |
| `skimmer-tci-probe` | IQ ingest from a TCI server (the engine still has a TCI client) |

## Compare two skimmers (`skimmer-compare/`)

```sh
cd skimmer-compare
./capture-spots.sh -c YOURCALL -r <cw-skimmer-host>:<port>   # records local, remote and RBN feeds into logs/
./compare-web.py                                             # live comparison at http://localhost:8074/
```

`compare.ini` sets the labels, the remote's receive windows and the matching
defaults. `test-capture.py` and `test-compare.py` are offline self-tests.
It needs bash, curl and Python 3, standard library only.

## Files

| Path | What |
| --- | --- |
| `~/.config/skimmer-for-linux/headless.ini` | Settings |
| `~/.config/skimmer-for-linux/master.scp` | Super Check Partial call list (`.state` records the update rate limits) |
| `~/.local/share/skimmer-for-linux/headless/` | Decode logs (`[decode] log=true`), decode taps (`learn/`) |
| `~/.local/share/skimmer-for-linux/models/deepcw/` | DeepCW model, if installed |

## Environment variables

`--list-switches` prints every `SKIM_*` switch the engine reads. The ones set
at start are logged and listed in `/status.json` (`switches`); a `SKIM_*`
name the engine does not read is warned about.

| Variable | Effect |
| --- | --- |
| `SKIM_CW_V1=1` | Use the classical CW decoder |
| `SKIM_CW_ENGINE=v1\|v2\|deepcw` | Pick the CW engine for this run |
| `SKIM_TONE_SPLIT=1` | Decode two stations in one channel separately — opt-in, awaiting its live validation |
| `SKIM_TONE_FOCUS=1` | Narrow the channel filter onto a lone carrier (implies the splitter) — same status |
| `SKIM_ORT_LIB=<path>` | ONNX Runtime library for DeepCW |
| `SKIM_SCP_URL=<url>` | Take the call list from somewhere else |

The decoder and the feed have more `SKIM_*` switches for the live-air work;
[`docs/LIVE-AIR.md`](docs/LIVE-AIR.md) describes them.

## Credits

- **Richard Fakenberg, OK1BR** ([rifak.cz](https://rifak.cz)) wrote Skimmer
  for Linux: the engine, the channelizer, the CW and RTTY decoders, callsign
  validation, the spot pipeline. This fork only builds on his work. Many
  thanks.
- **Pavel Demin** wrote [red-pitaya-notes](https://github.com/pavel-demin/red-pitaya-notes)
  and its `sdr_receiver_hpsdr`, the receiver this fork talks to.
- **VE3NEA** wrote CW Skimmer Server, the reference used for the comparisons.
- The fork's changes were written by Dan, OK1HRA, with the help of Claude Code.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
