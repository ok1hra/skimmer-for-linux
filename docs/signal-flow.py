#!/usr/bin/env python3
"""docs/signal-flow.py OUT.svg — writes docs/signal-flow.svg (README): the way
the signal takes through skimmer-headless, from the Red Pitaya to the telnet
feed, each step linked to its code. Edit here, run, commit both."""
import sys
from xml.sax.saxutils import escape as esc

BASE = 'https://github.com/ok1hra/skimmer-for-linux/blob/main/'
W = 1000
out = []
A = out.append

C = dict(bg='#0b1420', panel='#111d2b', ln='#2f4560', fg='#e6edf5', mut='#9fb0c4', faint='#6a7d96',
         hot='#39d98a', cw='#8fb9ff', rtty='#ffb88a', warn='#f2b33d', link='#5fc8ff', ext='#1a283a')


def box(x, y, w, h, title, lines, files, color=C['ln'], fill=C['panel'], num=None, title_color=None):
    A(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{fill}" stroke="{color}" stroke-width="1.4"/>')
    tx = x + 14
    if num is not None:
        A(f'<circle cx="{x + 18}" cy="{y + 19}" r="10" fill="{color}"/>')
        A(f'<text x="{x + 18}" y="{y + 23}" text-anchor="middle" class="n">{num}</text>')
        tx = x + 36
    tc = f' style="fill:{title_color}"' if title_color else ''
    A(f'<text x="{tx}" y="{y + 24}" class="t"{tc}>{esc(title)}</text>')
    yy = y + 44
    for ln in lines:
        A(f'<text x="{x + 14}" y="{yy}" class="d">{esc(ln)}</text>')
        yy += 17
    xx = x + 14
    for f in files:                                  # path#function: the file, the function named
        path, _, fn = f.partition('#')
        label = f'{path} — {fn}()' if fn else path
        A(f'<a href="{BASE}{path}" target="_blank"><text x="{xx}" y="{yy}" class="f">{esc(label)}</text></a>')
        xx += len(label) * 7.6 + 18                  # several files: side by side


def arrow(x1, y1, x2, y2, label=None, color=C['mut'], lx=None, ly=None, anchor='start'):
    A(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="1.8" marker-end="url(#ah)"/>')
    if label:
        A(f'<text x="{lx if lx is not None else x1 + 10}" y="{ly if ly is not None else (y1 + y2) / 2 + 4}" '
          f'class="l" text-anchor="{anchor}">{esc(label)}</text>')


# ---- the main column ------------------------------------------------------------------------
MX, MW = 40, 600            # main column
SX, SW = 680, 285           # side column

H = 1500
A(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
  'font-family="Roboto Condensed, Arial Narrow, Helvetica, Arial, sans-serif">')
A('<title>Skimmer for Linux — the signal from the Red Pitaya to the telnet feed</title>')
A('<defs><marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
  f'<path d="M0,0 L10,5 L0,10 z" fill="{C["mut"]}"/></marker></defs>')
A('<style>'
  f'.t{{font-size:16px;font-weight:700;fill:{C["fg"]}}}'
  f'.d{{font-size:13px;fill:{C["mut"]}}}'
  f'.f{{font-size:12.5px;fill:{C["link"]};font-family:"DejaVu Sans Mono",Menlo,Consolas,monospace}}'
  f'.l{{font-size:12px;fill:{C["faint"]}}}'
  f'.n{{font-size:12px;font-weight:700;fill:{C["bg"]}}}'
  f'.h{{font-size:12px;font-weight:700;letter-spacing:2px;fill:{C["hot"]}}}'
  f'.k{{font-size:12px;fill:{C["faint"]}}}'
  '</style>')
A(f'<rect width="{W}" height="{H}" fill="{C["bg"]}"/>')
A(f'<text x="{MX}" y="30" class="h">THE SIGNAL, FROM THE ANTENNA TO THE TELNET FEED</text>')
A(f'<text x="{MX}" y="48" class="k">each box names the code it runs in — open this picture on its own to follow the links</text>')

y = 66
box(MX, y, MW, 62, 'Red Pitaya — sdr_receiver_hpsdr (Pavel Demin)',
    ['the skimmer\'s own receiver: up to 8 DDCs, one band each, 48 / 96 / 192 kHz'], [],
    color=C['faint'], fill=C['ext'])
arrow(MX + 60, y + 62, MX + 60, y + 100, 'UDP, HPSDR Protocol 1 — 24-bit IQ of every receiver, ~4400 packets/s',
      lx=MX + 74)
y += 100
box(MX, y, MW, 96, 'IQ in',
    ['finds an idle radio, sets centres / rate / receivers, never keys; the wire is',
     'RF-inverted, so it is conjugated here; clock correction (ppm)'],
    ['src/engine/hpsdr_p1.c', 'src/headless/source_hpsdr_p1.c'], color=C['hot'], num=1)
arrow(MX + 60, y + 96, MX + 60, y + 130)
y += 130
box(MX, y, MW, 80, 'Bands out — one IQ block of each receiver',
    ['to the pipelines of its band (CW, and RTTY beside it on the same IQ)'],
    ['src/headless_main.c#rx_iq_cb'], color=C['hot'], num=2)
fan_y = y + 40
# side branches of the IQ
sy = y - 128
box(SX, sy, SW, 76, 'Waterfall + listening',
    ['FFT 8192 while someone watches; CW audio'], ['src/headless/waterfall.c'], color=C['ln'])
box(SX, sy + 88, SW, 76, 'Hub → worker PCs',
    ['a band\'s IQ to another PC (int24, TCP)'], ['src/headless/hub.c'], color=C['ln'])
box(SX, sy + 176, SW, 76, 'IQ recording', ['raw IQ to disk, for replays'], ['src/headless/record.c'], color=C['ln'])
for k, yy in enumerate((sy + 38, sy + 88 + 38, sy + 176 + 38)):
    A(f'<path d="M{MX + MW},{fan_y} C{MX + MW + 20},{fan_y} {SX - 20},{yy} {SX - 2},{yy}" fill="none" '
      f'stroke="{C["faint"]}" stroke-width="1.4" stroke-dasharray="4 3" marker-end="url(#ah)"/>')

arrow(MX + 60, y + 80, MX + 60, y + 118)
y += 118

# ---- the pipeline -------------------------------------------------------------------------------
py0 = y
PH = 0                      # set once the policy box is placed
frame_at = len(out)          # the dashed frame goes in here once its height is known
A(f'<text x="{MX}" y="{py0 + 22}" class="h">ONE PIPELINE PER BAND AND MODE — ITS OWN THREAD</text>')
A(f'<a href="{BASE}src/engine/pipeline.c" target="_blank"><text x="{MX + 404}" y="{py0 + 22}" class="f">src/engine/pipeline.c</text></a>')
y = py0 + 36
box(MX, y, MW, 96, 'Channelizer',
    ['a polyphase filter bank: the band in narrow complex channels, phase kept —',
     'CW 125 Hz apart (768 channels in 96 kHz), RTTY 250 Hz (384)'],
    ['src/engine/channelizer.c'], color=C['hot'], num=3)
arrow(MX + 60, y + 96, MX + 60, y + 128, 'every channel, also into its noise floor and SNR —', lx=MX + 74, ly=y + 116)
A(f'<a href="{BASE}src/engine/snr_meter.c" target="_blank"><text x="{MX + 352}" y="{y + 116}" class="f">src/engine/snr_meter.c</text></a>')
y += 150
# the decoders, side by side
dw = (MW - 24) // 3
dec = [
    ('CW v3.1 (default)', ['v2\'s Viterbi + a weak-', 'signal path, call reader'],
     ['src/engine/decode_cw_v3.c', 'src/engine/cw_reader.c'], C['cw']),
    ('CW v2 · DeepCW', ['semi-Markov Viterbi;', 'or the neural model'],
     ['src/engine/decode_cw_v2.c', 'src/engine/decode_deepcw.c'], C['cw']),
    ('RTTY 45.45 Bd', ['Baudot FSK, 170 Hz', 'shift, both polarities'],
     ['src/engine/decode_rtty.c'], C['rtty']),
]
A(f'<circle cx="{MX + 18}" cy="{y - 4}" r="10" fill="{C["hot"]}"/>'
  f'<text x="{MX + 18}" y="{y}" text-anchor="middle" class="n">4</text>')
A(f'<text x="{MX + 36}" y="{y + 1}" class="t">Decoders — one per channel, text with its confidence</text>')
y += 14
for k, (t, ls, fs, col) in enumerate(dec):
    x = MX + k * (dw + 12)
    A(f'<rect x="{x}" y="{y}" width="{dw}" height="112" rx="6" fill="{C["panel"]}" stroke="{col}" stroke-width="1.4"/>')
    A(f'<text x="{x + 12}" y="{y + 22}" class="t" fill="{col}" style="fill:{col}">{esc(t)}</text>')
    yy = y + 42
    for ln in ls:
        A(f'<text x="{x + 12}" y="{yy}" class="d">{esc(ln)}</text>'); yy += 16
    for f in fs:
        A(f'<a href="{BASE}{f}" target="_blank"><text x="{x + 12}" y="{yy + 2}" class="f">{esc(f.replace("src/engine/", ""))}</text></a>'); yy += 15
arrow(MX + 60, y + 112, MX + 60, y + 140, 'a station heard in two channels at once: the weaker one dropped (ghosts)',
      lx=MX + 74)
y += 140
box(MX, y, MW, 96, 'Callsigns read and validated',
    ['calls out of the running text: structure, prefixes, MASTER.SCP; torn and',
     'fused reads mended; CQ / TU / exchange context — RBN-grade'],
    ['src/engine/callsign.c'], color=C['hot'], num=5)
arrow(MX + 60, y + 96, MX + 60, y + 124)
y += 124
box(MX, y, MW, 80, 'Callsign filter (Settings)',
    ['a call that does not pass is gone here: no spot, no map dot, no statistics'],
    ['src/headless/config.c#skim_hd_config_call_pass'], color=C['hot'], num=6)
arrow(MX + 60, y + 80, MX + 60, y + 108)
y += 108
box(MX, y, MW, 80, 'Station table',
    ['one record per station: frequency snapped at 100 Hz, held 30 s, SNR, WPM'],
    ['src/engine/station.c'], color=C['hot'], num=7)
arrow(MX + 60, y + 80, MX + 60, y + 108)
y += 108
box(MX, y, MW, 96, 'Feed policy',
    ['CQ only; heard enough times; unknown calls wait 60 s; a near-call bust folded',
     'into the real call; the learned gate scores every line'],
    ['src/engine/feed_fold.c', 'src/engine/feed_gate.c'], color=C['hot'], num=8)
pe = y + 96 + 14
out.insert(frame_at, f'<rect x="{MX - 14}" y="{py0}" width="{MW + 28}" height="{pe - py0}" rx="10" fill="none" '
           f'stroke="{C["ln"]}" stroke-width="1.4" stroke-dasharray="6 4"/>')
arrow(MX + 60, y + 96, MX + 60, pe + 36, 'the CQ spots of every band and mode', lx=MX + 74, ly=pe + 24)
y = pe + 36

# ---- the feed --------------------------------------------------------------------------------
box(MX, y, MW, 96, 'The feed',
    ['one feed for all bands: spots of other skimmers (peers, workers) merged,',
     'duplicates dropped, a station re-spotted only after 10 min or a QSY'],
    ['src/engine/rbn_feed.c', 'src/headless/peers.c'], color=C['hot'], num=9)
fy = y
y += 96
ow = (MW - 12) // 2
arrow(MX + ow // 2, y, MX + ow // 2, y + 34)
arrow(MX + ow + 12 + ow // 2, y, MX + ow + 12 + ow // 2, y + 34)
y += 34
for k, (t, d, port) in enumerate((('Telnet cluster feed', 'DX de CALL-#: … CW 25 dB 22 WPM CQ', ':7300'),
                                  ('RBN Aggregator', 'to reversebeacon.net, under your call', ':7550'))):
    x = MX + k * (ow + 12)
    A(f'<rect x="{x}" y="{y}" width="{ow}" height="66" rx="6" fill="#0f2a1e" stroke="{C["hot"]}" stroke-width="2"/>')
    A(f'<text x="{x + 14}" y="{y + 25}" class="t" style="fill:{C["hot"]}">{esc(t)} <tspan class="d">{port}</tspan></text>')
    A(f'<text x="{x + 14}" y="{y + 47}" class="d">{esc(d)}</text>')
# the tap: web, statistics, archive
ty = fy - 10
box(SX, ty, SW, 76, 'Web page — map, DX cluster', ['the last 30 min (/api/spots)'], ['src/headless/web.c'], color=C['ln'])
box(SX, ty + 88, SW, 76, 'Statistics', ['per band and mode, two years'], ['src/headless/stats.c'], color=C['ln'])
box(SX, ty + 176, SW, 76, 'Spot archive', ['a gzip file a UTC day'], ['src/headless/archive.c'], color=C['ln'])
tap_y = fy + 48
for yy in (ty + 38, ty + 88 + 38, ty + 176 + 38):
    A(f'<path d="M{MX + MW},{tap_y} C{MX + MW + 20},{tap_y} {SX - 20},{yy} {SX - 2},{yy}" fill="none" '
      f'stroke="{C["faint"]}" stroke-width="1.4" stroke-dasharray="4 3" marker-end="url(#ah)"/>')
A(f'<text x="{SX}" y="{ty - 10}" class="l">every spot sent, as it goes out (the feed\'s tap)</text>')
A('</svg>')
open(sys.argv[1], 'w').write('\n'.join(out) + '\n')
