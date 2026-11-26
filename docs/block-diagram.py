#!/usr/bin/env python3
"""docs/block-diagram.py [OUT.svg] — writes docs/block-diagram.svg: the whole
receiver as blocks, from the two antennas through the Red Pitaya (its target
state: the skimmer's own FPGA, being built) and one Ethernet cable into
skimmer-headless — phasing, the decoders, the spot path, the band-level
consumers and the product layer, each block named after the code it runs in.
Edit here, run, commit both. Fonts: web/fonts (Roboto Condensed), embedded."""
import base64
import math
import os
import sys
from xml.sax.saxutils import escape as esc

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 1970, 1210
out = []
A = out.append

C = dict(bg='#0b1420', panel='#111d2b', ln='#2f4560', fg='#e6edf5', mut='#9fb0c4', faint='#6a7d96',
         a='#8fb9ff', b='#ffb88a', y='#39d98a', warn='#f2b33d', digi='#c39bff', link='#5fc8ff',
         ext='#1a283a', pcb='#0e1d17', pcbln='#d0505a', fpga='#14261f')


def font_face():
    css = ''
    for weight, name in ((400, 'RC-R.ttf'), (700, 'RC-B.ttf')):
        p = os.path.join(HERE, '..', 'web', 'fonts', name)
        with open(p, 'rb') as f:
            data = base64.b64encode(f.read()).decode()
        css += (f'@font-face{{font-family:"RC";font-weight:{weight};'
                f'src:url(data:font/ttf;base64,{data}) format("truetype")}}')
    return css


def text(x, y, s, cls='d', anchor='start', color=None, extra=''):
    st = f' style="fill:{color}"' if color else ''
    A(f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}"{st}{extra}>{esc(s)}</text>')


def rect(x, y, w, h, stroke=C['ln'], fill=C['panel'], rx=6, dash=False, sw=1.4):
    d = ' stroke-dasharray="5 4"' if dash else ''
    A(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" '
      f'stroke-width="{sw}"{d}/>')


def box(x, y, w, h, title, lines=(), code=None, stroke=C['ln'], fill=C['panel'], tcolor=None,
        dash=False, stack=0):
    """A block: title, lines of text, the code it runs in at the bottom.
    stack = how many cards peek out behind it (one per band and mode)."""
    for k in range(stack, 0, -1):
        rect(x + 6 * k, y - 6 * k, w, h, stroke=stroke, fill=fill, dash=dash)
    rect(x, y, w, h, stroke=stroke, fill=fill, dash=dash)
    text(x + 12, y + 21, title, 't', color=tcolor)
    yy = y + 40
    for ln in lines:
        if ln.startswith('!'):                       # a highlighted line
            text(x + 12, yy, ln[1:], 'd', color=C['fg'])
        else:
            text(x + 12, yy, ln, 'd')
        yy += 16
    if code:
        text(x + 12, y + h - 9, code, 'f')


def line(pts, color=C['mut'], sw=1.8, dash=False, head=True, marker=None):
    d = 'M' + ' L'.join(f'{px},{py}' for px, py in pts)
    da = ' stroke-dasharray="6 4"' if dash else ''
    m = f' marker-end="url(#{marker or "h_" + color.lstrip("#")})"' if head else ''
    A(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{sw}"{da}{m}/>')


def label(x, y, s, anchor='start', color=None):
    text(x, y, s, 'l', anchor, color)


def antenna(x, y, name):
    """A dipole-ish mast at x whose feed line ends at height y."""
    top = y - 70
    A(f'<path d="M{x},{y} L{x},{top} M{x - 16},{top - 22} L{x},{top} L{x + 16},{top - 22}" '
      f'fill="none" stroke="{C["fg"]}" stroke-width="2"/>')
    text(x, top - 30, name, 't', 'middle')


# ---- canvas ---------------------------------------------------------------------------------
A(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
  'font-family="RC, Roboto Condensed, Arial Narrow, sans-serif">')
A('<title>Skimmer for Linux — block diagram: two antennas, the Red Pitaya, skimmer-headless</title>')
A('<defs>')
for col in (C['mut'], C['a'], C['b'], C['y'], C['digi'], C['warn'], C['faint'], C['pcbln']):
    A(f'<marker id="h_{col.lstrip("#")}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
      f'markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{col}"/></marker>')
A('</defs>')
A('<style>' + font_face() +
  f'.t{{font-size:15px;font-weight:700;fill:{C["fg"]}}}'
  f'.d{{font-size:12.5px;fill:{C["mut"]}}}'
  f'.f{{font-size:11.5px;fill:{C["link"]};font-family:"DejaVu Sans Mono",Menlo,Consolas,monospace}}'
  f'.l{{font-size:12px;fill:{C["faint"]}}}'
  f'.h{{font-size:13px;font-weight:700;letter-spacing:2px;fill:{C["y"]}}}'
  f'.big{{font-size:20px;font-weight:700;fill:{C["fg"]}}}'
  f'.k{{font-size:13px;fill:{C["faint"]}}}'
  '</style>')
A(f'<rect width="{W}" height="{H}" fill="{C["bg"]}"/>')
text(40, 34, 'SKIMMER FOR LINUX — BLOCK DIAGRAM', 'h')
text(40, 54, 'two antennas, one Red Pitaya, one PC. The board is drawn in its target state (the skimmer\'s own '
     'FPGA, being built); everything right of the cable is the code as it runs (docs/PHASING.md, '
     'docs/REDPITAYA.md).', 'k')

# legend
LX = 1220
for i, (col, s) in enumerate(((C['a'], 'antenna A: IN1 → IP A'), (C['b'], 'antenna B: IN2 → IP B'),
                              (C['y'], 'phased: A and B combined'), (C['digi'], 'digital modes'))):
    xx = LX + i * 180
    A(f'<line x1="{xx}" y1="30" x2="{xx + 24}" y2="30" stroke="{col}" stroke-width="3"/>')
    label(xx + 32, 34, s)
A(f'<line x1="{LX}" y1="50" x2="{LX + 24}" y2="50" stroke="{C["mut"]}" stroke-width="1.8" '
  'stroke-dasharray="6 4"/>')
label(LX + 32, 54, 'control, feedback, side taps')

# ---- the Red Pitaya: the board as it is (STEMlab 122.88-16 SDR, 60 × 107 mm, upright) ---------
# The SMA edge is a convex arc, the four SMAs on it splay out (OUT2, OUT1, IN2, IN1 from the
# left); Ethernet, USB and the microSD card are on the bottom edge, power and console on the
# right one. Left out: the heatsink and the E1 / E2 headers (GPIO).
RX, RY, RW, RH = 60, 250, 420, 750
PCB, PCBLN, SILK, CHIP, GOLD = '#3d1518', '#d0505a', '#f5d5d5', '#260b0d', '#c9a227'
ACX, AR = RX + RW / 2, ((RW - 40) ** 2 / 4 + 44 ** 2) / 88      # the arc: centre x, radius
A(f'<path d="M{RX},{RY + RH - 12} L{RX},{RY + 60} Q{RX},{RY + 44} {RX + 20},{RY + 44} '
  f'A{AR:.1f},{AR:.1f} 0 0 1 {RX + RW - 20},{RY + 44} Q{RX + RW},{RY + 44} {RX + RW},{RY + 60} '
  f'L{RX + RW},{RY + RH - 12} Q{RX + RW},{RY + RH} {RX + RW - 12},{RY + RH} L{RX + 12},{RY + RH} '
  f'Q{RX},{RY + RH} {RX},{RY + RH - 12} Z" fill="{PCB}" stroke="{PCBLN}" stroke-width="2"/>')
for hx, hy in ((RX + 20, RY + 64), (RX + RW - 20, RY + 64), (RX + 20, RY + RH - 20), (RX + RW - 20, RY + RH - 20)):
    A(f'<circle cx="{hx}" cy="{hy}" r="8" fill="{C["bg"]}" stroke="{GOLD}" stroke-width="3"/>')
text(RX + 50, RY + 84, 'Red Pitaya', 'big', color=SILK)
text(RX + 50, RY + 100, 'STEMlab 122.88-16 SDR', 'k', color='#d9a0a4')

def sma(dx, name, col):
    """An SMA on the arc dx from its middle, splayed along the radius; returns its tip."""
    px, py = ACX + dx, RY + AR - math.sqrt(AR * AR - dx * dx)
    th = math.degrees(math.asin(dx / AR))
    A(f'<g transform="translate({px:.1f},{py:.1f}) rotate({th:.1f})">'
      f'<rect x="-15" y="4" width="5" height="14" fill="{GOLD}"/><rect x="10" y="4" width="5" height="14" fill="{GOLD}"/>'
      f'<rect x="-18" y="-6" width="36" height="12" rx="2" fill="{GOLD}" stroke="#7a6416"/>'
      f'<rect x="-11" y="-20" width="22" height="14" fill="#d8bd4a" stroke="#7a6416"/>'
      f'<rect x="-7" y="-54" width="14" height="34" fill="{GOLD}" stroke="#7a6416"/>'
      + ''.join(f'<line x1="-7" y1="{-50 + 5 * k}" x2="7" y2="{-48 + 5 * k}" stroke="#7a6416"/>' for k in range(7))
      + f'<text x="0" y="34" text-anchor="middle" class="{"t" if col else "l"}" '
      f'style="fill:{col or "#8a6a6c"}">{name}</text></g>')
    r = math.radians(th)
    return px, py, px + 54 * math.sin(r), py - 54 * math.cos(r)


sma(-135, 'OUT2', None)
sma(-45, 'OUT1', None)
_, IN2Y, T2X, T2Y = sma(45, 'IN2', C['b'])
_, IN1Y, T1X, T1Y = sma(135, 'IN1', C['a'])
for tx, ty, nm, col in ((T1X, T1Y, 'ANT A', C['a']), (T2X, T2Y, 'ANT B', C['b'])):
    antenna(tx, 175, nm)
    line([(tx, ty), (tx, 175)], col, sw=2.5, head=False)

# the clock and the dual ADC, under the SMAs
COL_W = 146
CBX, CAX = RX + 52, RX + RW - 52 - COL_W          # lane B on the left, lane A on the right (as IN2, IN1)
LBX, LAX = CBX + COL_W / 2, CAX + COL_W / 2       # the lanes' middles
ADX, ADY, ADW, ADH = RX + 150, RY + 118, RW - 202, 62
A(f'<rect x="{ADX}" y="{ADY}" width="{ADW}" height="{ADH}" rx="3" fill="{CHIP}" stroke="#777"/>')
for k in range(14):
    for py_ in (ADY - 4, ADY + ADH):
        A(f'<rect x="{ADX + 8 + k * 15}" y="{py_}" width="6" height="4" fill="#aaa"/>')
text(ADX + 10, ADY + 20, 'dual ADC · 16 bit · 122.88 MS/s', 't', color=SILK)
text(ADX + 10, ADY + 40, 'ch 2 ← IN2', 'd')
text(ADX + ADW - 10, ADY + 40, 'ch 1 ← IN1', 'd', 'end')
text(ADX + 10, ADY + 55, 'peak / overload per input', 'l')
line([(ACX + 45, IN2Y + 4), (ACX + 45, ADY - 4)], C['b'], sw=2.5)
line([(ACX + 135, IN1Y + 4), (ACX + 135, ADY - 4)], C['a'], sw=2.5)
CKX, CKY = RX + 50, ADY
A(f'<rect x="{CKX}" y="{CKY}" width="86" height="{ADH}" rx="3" fill="#151517" stroke="#888"/>')
text(CKX + 8, CKY + 22, '122.88 MHz', 't', color=SILK)
text(CKX + 8, CKY + 40, 'one clock', 'l')
text(CKX + 8, CKY + 54, 'for all', 'l')
line([(CKX + 86, CKY + 31), (ADX - 2, CKY + 31)], C['y'], sw=1.4, dash=True)

# the Zynq: FPGA (PL) and ARM (PS)
PX0, PX1 = RX + 44, RX + RW - 44
PLY0, PLY1 = RY + 194, RY + 404
PSY0, PSY1 = RY + 414, RY + 552
for y0, y1, t0, s0 in ((PLY0, PLY1, 'FPGA (PL) — own bitstream', 'Zynq-7020 · base: Pavel Demin (MIT)'),
                       (PSY0, PSY1, 'ARM (PS) · skimmer-rp agent', 'Protocol 1 for CW Skimmer, Quisk; ctl')):
    rect(PX0, y0, PX1 - PX0, y1 - y0, stroke=C['y'], fill='none', dash=True)
    text(PX0 + 22, y0 + 18, t0, 't', color=C['y'])
    label(PX0 + 22, y0 + 32, s0)
DY0 = RY + 262
for cx0, name, col, inp in ((CAX, 'A', C['a'], 1), (CBX, 'B', C['b'], 2)):
    for k in range(2, 0, -1):
        rect(cx0 + 4 * k, DY0 - 4 * k, COL_W, 84, stroke=col, fill=CHIP)
    box(cx0, DY0, COL_W, 84, f'DDC {name}1 … {name}n', ('NCO + CIC + FIR', 'own rate, 48 k – 3.07 M',
                                                       f'~11 slots on IN{inp}'), stroke=col, fill=CHIP)
line([(CAX + COL_W - 8, ADY + ADH + 4), (CAX + COL_W - 8, DY0 - 8)], C['a'], sw=2.5)
line([(ADX + 16, ADY + ADH + 4), (ADX + 16, ADY + ADH + 8), (CBX + 8, ADY + ADH + 8), (CBX + 8, DY0 - 8)],
     C['b'], sw=2.5)
# one reset for both: what makes A and B coherent (the lanes pass it on the outside)
RSX0, RSX1, RSY = CBX + 16, CAX + COL_W - 16, DY0 + 96
A(f'<rect x="{RSX0}" y="{RSY}" width="{RSX1 - RSX0}" height="38" rx="5" fill="{CHIP}" stroke="{C["y"]}"/>')
text(RSX0 + 10, RSY + 16, 'A and B NCOs reset together,', 'd', color=C['y'])
text(RSX0 + 10, RSY + 31, 'a hardware sample counter', 'd', color=C['y'])
line([(CKX, CKY + 31), (RX + 40, CKY + 31), (RX + 40, RSY + 19), (RSX0, RSY + 19)], C['y'], sw=1.4, dash=True)
# ARM: one IP per channel
IPY = PSY0 + 44
for cx0, ip, ifc, col, lx in ((CAX, 'IP A', 'eth0', C['a'], CAX + COL_W - 8), (CBX, 'IP B', 'mvl0', C['b'], CBX + 8)):
    box(cx0, IPY, COL_W, 82, f'{ip} · {ifc}', ('DMA ring ' + ip[-1], 'UDP: epoch +', '64-bit sample index'),
        stroke=col, fill=CHIP)
    line([(lx, DY0 + 84), (lx, IPY)], col, sw=2.5)
for k in range(10):                                   # the LED column on the left edge
    A(f'<rect x="{RX + 12}" y="{RY + 450 + k * 11}" width="7" height="5" '
      f'fill="{("#39d98a", "#f2b33d", "#ff5a5a")[k % 3] if k < 3 else "#5a3030"}"/>')

# the bottom edge: PHY, RJ45, USB, microSD; power and console on the right edge
PHX, PHY0 = RX + 66, RY + 572
A(f'<rect x="{PHX}" y="{PHY0}" width="64" height="42" rx="3" fill="{CHIP}" stroke="#777"/>')
text(PHX + 32, PHY0 + 26, 'PHY', 't', 'middle', color=SILK)
line([(CBX + 30, IPY + 82), (CBX + 30, PHY0)], C['b'], sw=2.5)
line([(LAX, IPY + 82), (LAX, PHY0 + 21), (PHX + 64, PHY0 + 21)], C['a'], sw=2.5)
JX0, JY0, JW, JH = RX + 46, RY + 646, 104, 116    # RJ45, standing out of the bottom edge
A(f'<line x1="{PHX + 30}" y1="{PHY0 + 42}" x2="{PHX + 30}" y2="{JY0}" stroke="{C["a"]}" stroke-width="2"/>')
A(f'<line x1="{PHX + 35}" y1="{PHY0 + 42}" x2="{PHX + 35}" y2="{JY0}" stroke="{C["b"]}" stroke-width="2"/>')
A(f'<rect x="{JX0}" y="{JY0}" width="{JW}" height="{JH}" rx="3" fill="#b9bec6" stroke="#e4e7eb" stroke-width="1.5"/>')
A(f'<rect x="{JX0 + 22}" y="{JY0 + 58}" width="{JW - 44}" height="50" rx="2" fill="#15181d"/>')
text(JX0 + JW / 2, JY0 + 24, 'RJ45', 't', 'middle', color='#1d2026')
text(JX0 + JW / 2, JY0 + 42, '1 GbE', 'd', 'middle', color='#33373e')
A(f'<rect x="{RX + 160}" y="{RY + 686}" width="46" height="76" rx="2" fill="#9aa0a8" stroke="#d0d4da"/>')
A(f'<rect x="{RX + 168}" y="{RY + 700}" width="30" height="14" fill="#2a2d33"/>')
text(RX + 183, RY + 678, 'USB', 'l', 'middle', color='#d9a0a4')
A(f'<rect x="{RX + 236}" y="{RY + 672}" width="104" height="64" rx="3" fill="none" stroke="#d9a0a4" '
  'stroke-dasharray="4 3"/>')
text(RX + 246, RY + 696, 'microSD', 'l', color='#d9a0a4')
text(RX + 246, RY + 712, 'Alpine + agent', 'l', color='#d9a0a4')
for uy, nm in ((RY + 572, 'power'), (RY + 626, 'console')):
    A(f'<rect x="{RX + RW - 20}" y="{uy}" width="26" height="40" rx="2" fill="#5b6068" stroke="#9aa0a8"/>')
    text(RX + RW - 26, uy + 24, nm, 'l', 'end', color='#8a6a6c')

# below the board: the target and today
box(RX, RY + RH + 44, RW, 150, 'Target (Vivado 2025.2, being built)', (
    '• ~22 DDC slots on two channels, each its own rate: all HF + 6 m,',
    '   CW and digital, every band from both antennas',
    '• A and B NCOs reset together, a hardware sample counter',
    '• a DMA ring per channel, ADC peak / overload per input',
    '• in P1 mode (CW Skimmer, Quisk) a slot takes either input',
    '!Today: Pavel\'s bitstream, 16 DDC (8 an IP), one rate an IP ≤ 384 k;',
    '!   agent 0.5 counts samples since a paired FIFO reset'), stroke=PCBLN, fill='#1d0c0e')

# ---- the cable ------------------------------------------------------------------------------
BX, BY, BW, BH = 570, 90, 1290, 1070            # skimmer-headless
C1, W1 = 590, 222          # IQ in
C2, W2 = 850, 310          # phasing
C3, W3 = 1252, 290         # decoders, calls, stations
C4, W4 = 1574, 272         # spot policy, feeds
TOP = 150
CY = TOP + 150
CX0, CBOT, CV = JX0 + JW / 2, RY + RH + 26, 526   # out of the RJ45, under the board, up the gap


def cable_pts(s):
    return [(CX0 - s, JY0 + JH), (CX0 - s, CBOT + s), (CV + s, CBOT + s), (CV + s, CY + s), (C1, CY + s)]


A('<path d="M' + ' L'.join(f'{x},{y}' for x, y in cable_pts(0)) + '" fill="none" stroke="#3a4656" '
  'stroke-width="12" stroke-linejoin="round"/>')
for s, col in ((-2.5, C['a']), (2.5, C['b'])):
    A('<path d="M' + ' L'.join(f'{x},{y}' for x, y in cable_pts(s)) + f'" fill="none" stroke="{col}" '
      'stroke-width="2"/>')
A(f'<text x="{CV - 12}" y="{(CY + CBOT) / 2}" class="l" text-anchor="middle" '
  f'transform="rotate(-90 {CV - 12} {(CY + CBOT) / 2})">one Ethernet cable, two IPs</text>')

# ---- skimmer-headless -----------------------------------------------------------------------
rect(BX, BY, BW, BH, stroke=C['mut'], fill='#0d1826', rx=12, sw=2)
text(BX + 20, BY + 28, 'skimmer-headless — the PC', 'big')
text(BX + 270, BY + 28, 'src/headless_main.c · engine in src/engine/ (GLib only) · product layer in '
     'src/headless/ · one pipeline per band and mode, up to 16 bands', 'k')

# C1 — the IQ source
box(C1, TOP, W1, 250, 'HPSDR P1 clients', (
    '!IP A and IP B, a paired start',
    'discovery (SKRP marker) finds',
    'the board and its second IP;',
    'rate, slots and centres per IP;',
    'conjugate (the wire is RF-',
    'inverted), clock ppm;',
    'IP B busy or silent: retried',
    'every 3 s, A runs on alone',
    '',
    'blocks stamped with epoch',
    '+ sample index, loss filled'), 'engine/hpsdr_p1.c', stroke=C['mut'])
box(C1, TOP + 270, W1, 140, 'Pairer, one per band', ('A and B matched by epoch +', 'sample index → one block pair',
                                                     'ip=ab: A + B from both IPs', 'in=12: IN1 + IN2 on one IP',
                                                     'B missing / late → A alone'),
    'headless/source_hpsdr_p1.c', stroke=C['y'])
line([(C1 + 70, TOP + 250), (C1 + 70, TOP + 270)], C['a'], sw=2.2)
line([(C1 + 150, TOP + 250), (C1 + 150, TOP + 270)], C['b'], sw=2.2)
box(C1, TOP + 440, W1, 80, 'Recorder [record]', ('IQ to disk, a pair: x.cf32 + x-b.cf32',),
    'headless/record.c', dash=True)
box(C1, TOP + 534, W1, 86, 'Hub :7380', ('int24 IQ of @worker bands to', 'other PCs (unphased bands)'),
    'headless/hub.c', dash=True)
box(C1, TOP + 640, W1, 62, 'Other sources', ('TCI, a hub\'s worker, mock',), dash=True)
line([(C1 + 20, TOP + 410), (C1 + 20, TOP + 440)], C['mut'], sw=1.4, dash=True)
line([(C1 + 20, TOP + 426), (C1 - 10, TOP + 426), (C1 - 10, TOP + 577), (C1, TOP + 577)], C['mut'], sw=1.4,
     dash=True)
label(C1 + 30, TOP + 430, 'raw A (and B)')

# C2 — phasing
rect(C2 - 10, TOP - 8, W2 + 20, 630, stroke=C['y'], fill='#0e1f1a', rx=10, sw=1.8)
text(C2 + 4, TOP + 14, 'PHASING — two antennas, one band', 't', color=C['y'])
label(C2 + 4, TOP + 30, 'docs/PHASING.md · off = antenna A, byte for byte')
box(C2, TOP + 42, W2, 88, 'Channelizer A + channelizer B', (
    'polyphase bank, WDSP FFT: CW 125 Hz channels',
    '(768 at 96 kHz), RTTY 250 Hz; B only if phased'), 'engine/channelizer.c', stroke=C['y'])
box(C2, TOP + 144, W2, 132, 'Combiner, per channel', (
    '!weights for every channel, from the data:',
    'MVDR (default) lifts the station, nulls the',
    'interference; MRC · Fixed (Linrad) · A · B.',
    'No calibration: cables, NCO phase and antenna',
    'delays fall out of the estimate.'), 'engine/combiner.c', stroke=C['y'])
box(C2, TOP + 290, W2, 70, 'Meter', ('gain G, floor drop ΔF, coherence per channel',), 'engine/phase_meter.c',
    stroke=C['y'])
box(C2, TOP + 374, W2, 108, 'Band combine W(f)', (
    'WOLA √Hann on the band\'s IQ; the combiner\'s',
    'weights (1/s), interpolated to every bin',
    '!→ the band\'s own IQ for everything below'), 'engine/band_combine.c', stroke=C['y'])
# pairer → channelizers and band combine
PA, PB = TOP + 330, TOP + 346
line([(C1 + W1, PA), (C2 - 30, PA), (C2 - 30, TOP + 80), (C2, TOP + 80)], C['a'], sw=2.2)
line([(C1 + W1, PB), (C2 - 20, PB), (C2 - 20, TOP + 100), (C2, TOP + 100)], C['b'], sw=2.2)
line([(C2 - 30, PA), (C2 - 30, TOP + 420), (C2, TOP + 420)], C['a'], sw=2.2)
line([(C2 - 20, PB), (C2 - 20, TOP + 440), (C2, TOP + 440)], C['b'], sw=2.2)
line([(C2 + 160, TOP + 130), (C2 + 160, TOP + 144)], C['y'], sw=2.2)
line([(C2 + 160, TOP + 276), (C2 + 160, TOP + 290)], C['y'], sw=1.4, dash=True)
line([(C2 + W2, TOP + 250), (C2 + W2 + 7, TOP + 250), (C2 + W2 + 7, TOP + 400), (C2 + W2, TOP + 400)],
     C['y'], sw=1.4, dash=True)
# phasing's controls
box(C2, TOP + 506, W2, 104, 'Phasing controls', (
    '[phase] mode, phase= per band, Fixed memories;',
    'Φ on / off and the panel in the waterfall,',
    'POST /api/phase — live, no restart'), 'headless_main.c', stroke=C['y'], dash=True)

# C3 — decoders, calls, stations (one stack per band and mode)
box(C3, TOP + 12, W3, 196, 'Decoders, one per channel', (
    '!CW v3.1 (default): v2 Viterbi above 8 dB',
    '   + weak view, audio-confirmed call reader',
    'CW v4: DeepCW v2 network in C, worker pool',
    'DeepCW v1 (ONNX via dlopen) · v2 · v1',
    '!RTTY 45.45 Bd Baudot FSK, on the same IQ',
    'rtty_pairs: CW may drop what RTTY copies',
    'TX hold; a paused band gets no IQ'), 'engine/decode_cw_v3.c …', stroke=C['mut'], stack=2)
box(C3, TOP + 228, W3, 116, 'Callsign extraction + validation', (
    'calls from the text, CQ / DE / TU context,',
    're-read against MASTER.SCP; [filter] call,',
    'regex, bad calls (DXCC names)',
    '!an unvalidated call never reaches a feed'), 'engine/callsign.c · dxcc.c', stroke=C['mut'], stack=2)
box(C3, TOP + 364, W3, 100, 'Station table', (
    'one station a frequency: snap 100 Hz,',
    'hold 30 s, hearings, WPM, SNR (A, B, gain)'), 'engine/station.c', stroke=C['mut'], stack=2)
label(C3 + W3, TOP + 484, '× every band and mode (CW, RTTY)', 'end')
line([(C2 + W2, TOP + 200), (C3, TOP + 200)], C['y'], sw=2.6)
label(C2 + W2 + 14, TOP + 192, 'a channel\'s')
label(C2 + W2 + 14, TOP + 220, 'signal')
line([(C3, TOP + 150), (C2 + W2 + 24, TOP + 150), (C2 + W2 + 24, TOP + 170), (C2 + W2, TOP + 170)],
     C['y'], sw=1.4, dash=True)
label(C2 + W2 + 14, TOP + 140, 'mark_prob')
label(C2 + W2 + 14, TOP + 128, 'layer 2:')
for y0, y1 in ((TOP + 208, TOP + 228), (TOP + 344, TOP + 364)):
    line([(C3 + W3 / 2, y0), (C3 + W3 / 2, y1)], C['mut'], sw=2)

# C4 — spot policy and the feeds
box(C4, TOP + 12, W4, 136, 'Spot policy', (
    'CQ only, min score 0.90, unknown call',
    'held 60 s, fresh 120 s, twin fold 600 s,',
    'respot / QSY rules; the learned feed gate',
    'peers\' and workers\' spots, deduplicated'),
    'engine/spot_out.c · feed_fold.c', stroke=C['mut'])
box(C4, TOP + 168, W4, 104, 'Telnet feed :7300', (
    'CW Skimmer\'s format: DX de CALL-#: …',
    '!CW · RTTY · FT8 / FT4 · JS8'), 'engine/rbn_feed.c', stroke=C['warn'], tcolor=C['warn'])
box(C4, TOP + 292, W4, 80, 'RBN Aggregator :7550', ('CW and RTTY only, [rbn] delay',),
    'engine/rbn_feed.c', stroke=C['warn'], tcolor=C['warn'])
line([(C3 + W3, TOP + 414), (C3 + W3 + 20, TOP + 414), (C3 + W3 + 20, TOP + 80), (C4, TOP + 80)],
     C['mut'], sw=2)
line([(C4 + 100, TOP + 148), (C4 + 100, TOP + 168)], C['mut'], sw=2)
line([(C4 + 30, TOP + 148), (C4 + 30, TOP + 158), (C4 - 10, TOP + 158), (C4 - 10, TOP + 332), (C4, TOP + 332)],
     C['mut'], sw=2)

# ---- the band's own IQ: waterfall, digital modes --------------------------------------------
R2 = 760
BUS = R2 + 30
line([(C2 + 160, TOP + 506), (C2 + 160, TOP + 482)], C['y'], sw=1.4, dash=True)
A(f'<line x1="{C2 + W2 + 4}" y1="{TOP + 440}" x2="{C2 + W2 + 30}" y2="{TOP + 440}" stroke="{C["y"]}" '
  'stroke-width="2.6"/>')
A(f'<line x1="{C2 + W2 + 30}" y1="{TOP + 440}" x2="{C2 + W2 + 30}" y2="{BUS}" stroke="{C["y"]}" '
  'stroke-width="2.6"/>')
A(f'<line x1="{C2 + 118}" y1="{BUS}" x2="{C2 + 358}" y2="{BUS}" stroke="{C["y"]}" stroke-width="2.6"/>')
label(C2 + W2 + 40, BUS - 8, 'the band\'s own IQ (A alone when the band is not phased)')
BW1, BW2, BW3, BW4, G = 236, 216, 256, 250, 14
X1 = C2
X2 = X1 + BW1 + G
X3 = X2 + BW2 + G
X4 = X3 + BW3 + G
box(X1, R2 + 50, BW1, 150, 'Waterfall + audio', (
    'FFT 8192 of a watched band,',
    'a ring per viewer; 8 kHz audio,',
    'S-meter; Φ and the gain line',
    'WebSocket /ws/waterfall'), 'headless/waterfall.c', stroke=C['y'])
box(X2, R2 + 50, BW2, 150, 'Dial audio', (
    'a dial\'s 12 kHz USB audio',
    '(WDSP resampler) in UTC',
    'periods: FT8 15 s, FT4 7.5 s,',
    'JS8 6–30 s; WSPR its own way:',
    '375 Hz, 114 s each even minute'), 'digi_rx.c · wspr.c', stroke=C['digi'])
box(X3, R2 + 50, BW3, 150, 'Helpers (own processes)', (
    '!skimmer-ft8d: WSJT-X FT8 / FT4',
    '!skimmer-js8d: JS8Call, kept running',
    '!skimmer-wsprd: WSJT-X wsprd',
    'JS8 deep frames: only confirmed',
    'nice 10, a pool of threads'), 'ft8.c · js8.c · wspr.c', stroke=C['digi'])
box(X4, R2 + 50, BW4, 150, 'Digital spots out', (
    'FT8 / FT4 / JS8 → telnet feed',
    'PSK Reporter (IPFIX, UDP 4739)',
    'WSPRnet (WSPR spots)',
    'never on the RBN port'), 'pskreporter.c · wspr.c', stroke=C['digi'])
for xx in (X1 + 118, X2 + 108):
    line([(xx, BUS), (xx, R2 + 50)], C['y'], sw=2.2)
line([(X2 + BW2, R2 + 125), (X3, R2 + 125)], C['digi'], sw=2.2)
line([(X3 + BW3, R2 + 125), (X4, R2 + 125)], C['digi'], sw=2.2)
# digital spots → the telnet feed (up the right side of the feeds)
line([(X4 + BW4 - 40, R2 + 50), (X4 + BW4 - 40, R2 + 10), (C4 + W4 + 8, R2 + 10), (C4 + W4 + 8, TOP + 250),
       (C4 + W4, TOP + 250)], C['digi'], sw=1.8)

# ---- the product layer ----------------------------------------------------------------------
R3 = 990
text(C1, R3 + 4, 'PRODUCT LAYER — src/headless/', 'h')
PW, PG = 170, 8
prod = [
    ('Red Pitaya manager', ('over ssh: install the', 'agent, inputs, status;', 'password never stored'),
     'rp_manage.c', C['pcbln']),
    ('Statistics', ('7 d / 90 d / 2 y rings,', 'the board\'s series;', 'Phasing: coarse history'),
     'stats.c · phase_hist.c', C['mut']),
    ('Governor', ('thermal guard, adaptive', 'RTTY, v4 load guard', '→ pauses bands'),
     'governor.c', C['mut']),
    ('Archive, spot log', ('every spot, a gzip file', 'a UTC day'),
     'archive.c · spotlog.c', C['mut']),
    ('Peers, discovery', ('other skimmers\' feeds,', 'workers; UDP 8075'),
     'peers.c · discovery.c', C['mut']),
    ('Config, updates', ('headless.ini, one parser;', 'visitors\' sign-in;', 'self-update, service'),
     'config.c · update.c', C['mut']),
    ('Web :8073', ('Status: map, DX cluster,', 'bands; Statistics,', 'Waterfall, Settings, Log'),
     'web.c · web/', C['mut']),
]
for i, (t, ls, code, col) in enumerate(prod):
    box(C1 + i * (PW + PG), R3 + 20, PW, 130, t, ls, code, stroke=col)
# the manager reaches the board over the same cable
line([(C1, R3 + 60), (CV + 22, R3 + 60), (CV + 22, CY + 30)], PCBLN, sw=1.6, dash=True)
A(f'<text x="{CV + 36}" y="{R3 - 120}" class="l" text-anchor="middle" '
  f'transform="rotate(-90 {CV + 36} {R3 - 120})">ssh over the same cable: install the agent, inputs, ctl status</text>')

# ---- out of the PC --------------------------------------------------------------------------
OX = BX + BW + 6
outs = ((TOP + 210, 'telnet clients,', 'DX clusters', C['warn'], C4 + W4),
        (TOP + 332, 'RBN', 'Aggregator', C['warn'], C4 + W4),
        (R2 + 125, 'PSK Reporter,', 'WSPRnet', C['digi'], X4 + BW4),
        (R3 + 85, 'browser', '', C['mut'], C1 + 7 * (PW + PG) - PG))
for y, s1, s2, col, x0 in outs:
    line([(x0, y), (OX + 6, y)], col, sw=2)
    label(OX + 12, y - 2, s1)
    label(OX + 12, y + 13, s2)

A('</svg>')
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'block-diagram.svg')
with open(path, 'w') as f:
    f.write('\n'.join(out) + '\n')
