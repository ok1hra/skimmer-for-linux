# One station, several PCs: hub, workers and peers

One skimmer-headless can be the station's single point of contact — one
telnet feed, one RBN port, one call — while the decoding runs on several
PCs, and while other skimmers' spots join in.

```
 Red Pitaya ──UDP ~40 Mb/s──► HUB ─── telnet :7300 / RBN :7550  (one point)
                               │ decodes its own bands
                               │ TCP :7380 ── IQ, int24 ──► WORKER "pc2"
                               │           ◄─ SPOT / STAT ─┘
                               └─ telnet client ◄── PEER (another skimmer,
                                                    CW Skimmer Server…)
```

## Why a hub has to pass the IQ on

A Red Pitaya running `sdr_receiver_hpsdr` (HPSDR Protocol 1) streams to
**one** client at a time; a second client gets "busy" (`docs/HPSDR-P1.md`).
So two PCs cannot each take their own bands from one radio. The PC that has
the radio — the hub — receives all of its receivers and passes the IQ of the
bands it does not decode itself to the worker that does.

## Setting it up

On the hub (the PC with the radio), name the worker after a band:

```ini
[bands]
40m=7045500 cw,rtty            ; decoded here
20m=14045500 cw,rtty @pc2      ; received here, decoded by the worker pc2
[hub]
name=central
port=7380                      ; 0 = no hub server
#allow=10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,127.0.0.0/8,::1   (the default)
```

On the worker:

```ini
[radio]
type=hub
host=auto                      ; or the hub's address[:port]
[hub]
name=pc2                       ; default: the host name
```

The worker needs no `[bands]`: everything that decides what is decoded —
the bands, the rate, `[decode]` and the feed policy — comes from the hub
(`skim_hd_config_worker_fragment`), merged over the worker's own file
(`skim_hd_config_worker_merge`), so it is set in one place. The worker
keeps its own type, host, name, ports and page.

On the web page: Settings → Bands → *decoded by* on the hub; on the worker,
Settings → Skimmers on the LAN → *Use as hub*. A worker connected with
nothing assigned is listed on the hub's Status page.

Changing the assignment restarts the hub (every settings change does); the
link drops, the worker connects again, sees other bands than it was built
on and restarts itself. A worker without a hub runs idle and keeps looking
(every 5 s).

## Bandwidth

The IQ goes as 24-bit integers — the Red Pitaya's own samples, so the
worker decodes exactly what the hub would have (a float block from the
radio is n / 2²³). At 96 kHz that is **~4.6 Mb/s per band** (measured
4.64 Mb/s on the gate). The hub's link carries the radio's ~40 Mb/s in and
the workers' bands out — full duplex, so even all 8 bands out (~37 Mb/s)
fit a 100 Mb/s link. Use wired Ethernet.

A worker that cannot keep up loses blocks, never the radio: each link holds
at most 2 s of IQ, the rest is dropped and counted (Status → Workers →
dropped), and the frame sequence numbers tell the worker how much it lost.

## Peers

`[peers] hosts=` lists other skimmers' telnet feeds (host or host:port,
7300 by default) — ours, or CW Skimmer Server. The hub logs in with its
own call, reads their CQ spots and sends them on. **They go out under the
hub's call** (Dan's choice, 2026-11-17): right for skimmers on this
station's antennas, not for someone else's — the RBN counts a spotter as
one station.

## Duplicates

Every spot the feed sends is noted (call, frequency, time). A spot from a
peer or a worker is dropped when the same call went out within 300 Hz in
the last 600 s — the pipeline's own re-spot interval (`RBN_RESPOT_S`) and a
little more than its QSY tolerance, since two skimmers read one carrier a
few Hz apart. A local spot is dropped only when another skimmer's spot of
the call went first; without peers or workers the feed sends exactly what
it did before (`skim_rbn_feed_spot_external`).

## Finding each other

Every instance answers a UDP broadcast on port 8075 with its name, role
(solo, hub, worker), call, ports and bands (`headless/discovery.h`).
`skimmer-headless --find-skimmers` lists them, and so does Settings →
Skimmers on the LAN. A worker with `host=auto` takes the hub that names it
in its bands, else the first instance that runs a hub server.

## The link protocol

TCP, `[hub] port` (7380). The worker says
`SKIMHUB 1 HELLO name=<name> version=<v>`; the hub answers
`SKIMHUB 1 CONFIG <bytes>` + the ini fragment, `SKIMHUB 1 NONE` (nothing
assigned — the link stays, idle) or `SKIMHUB 1 REFUSED <why>` (an address
outside `allow`, a second link under one name). Then IQ frames: a 32-byte
little-endian header — `u32` magic `SKIQ`, `u8` band (index in the
fragment's `[bands]`), `u8` flags, `u16` 0, `u32` frames, `u32` rate, `u64`
seq (the band's frames before this block, dropped ones included), `f64`
centre Hz — then frames × (I, Q) as 24-bit little-endian integers. The
worker sends lines: `SPOT <call> <mode> <Hz> <snr> <speed>`,
`STAT <band> <cw|rtty> <decoders> <active> <stations> <spots>` and
`STATUS <its /status.json>` (every 10 s). There is no authentication;
`[hub] allow` limits who may connect.

The gates: `hub` (the hub's side against a raw socket), `peers`,
`rbn-feed` (the dedup), `config` (fragment and merge) and `hub-mock` (a hub
and a worker on one machine, end to end).
