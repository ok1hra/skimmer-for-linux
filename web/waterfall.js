// waterfall.js — the Waterfall tab: one received band, live, as a waterfall
// the height of the page, a small frequency scale above it and, as an
// option, the spectrum's envelope above that. The calls the feed spotted
// stand upright in it where they were heard (frequency, and the row of the
// spot's time).
//
// The rows come over a WebSocket (/ws/waterfall, see src/headless/waterfall.h):
// the skimmer computes the band only while someone watches it and sends each
// viewer just its window, a byte a pixel at the rows a second it asks for;
// every row is acknowledged, so a slow link gets fewer rows, not a queue. The
// socket is open only while this tab is shown in a visible window.
// Per-viewer settings are kept in localStorage. The RTTY palette
// (rtty-palette.js) decodes the audio heard; RTTY in the bar opens it. The
// FT8 palette (ft8-palette.js) shows what the skimmer decodes on an FT8/FT4
// dial; FT8 in the bar opens it, and the view narrows to the dial's window.
'use strict';

const Wf = (() => {
  const SPANS = [0, 48000, 24000, 12000, 6000, 3000, 1500];   // 0 = the whole band
  const RATES = [1, 2, 5, 10, 20];
  const DEF = { band: '', span: 0, centre: 0, rate: 5, range: 50, floor: 0, spec: true, calls: true, abw: 250 };
  const BWS = [50, 100, 150, 200, 250, 300, 400, 500];
  const store = (() => { try { return JSON.parse(localStorage.getItem('skwf') || '{}'); } catch (e) { return {}; } })();
  if (store.v !== 2) { store.spec = true; store.v = 2; }        // v2: the spectrum on by default
  const opt = { ...DEF, ...store };
  const save = () => { try { localStorage.setItem('skwf', JSON.stringify(opt)); } catch (e) { /* private */ } };
  const $ = (s) => document.querySelector(s);
  // the RTTY palette (rtty-palette.js): while it is open the audio has its
  // passband, it gets every packet, and the markers stand on the scale
  const pal = () => typeof RttyPal !== 'undefined' && RttyPal.isOpen();
  const ft8 = () => typeof Ft8Pal !== 'undefined' && Ft8Pal.isOpen();
  // the digital palette open over the picture (FT8 or JS8, one at a time):
  // it draws its period lines and stamps the senders
  const dpal = () => (ft8() ? Ft8Pal : typeof Js8Pal !== 'undefined' && Js8Pal.isOpen() ? Js8Pal : null);
  const abw = () => (pal() ? RttyPal.bandwidth() : opt.abw);

  let ws = null, active = false, retry = 0, hello = null, band = -1, shownBand = '', leaveT = 0, away = false;
  let W = 0, H = 0, cv, g, sc, sg, sp, spg, rowCv, rowG;
  let view = { lo: 0, hi: 0 };                        // what the canvas shows now
  let floorB = null, envelope = null, rowTimes = [];
  let spots = [], spotsAt = 0, callHits = [];
  let bytes = 0, bytesAt = Date.now(), sendT = 0;

  // ---- colours: dark → blue → cyan → yellow → red → white ----------------------
  const LUT = (() => {
    const stops = [[0, [0, 0, 12]], [0.15, [0, 24, 90]], [0.35, [0, 80, 200]], [0.55, [0, 200, 200]],
                   [0.72, [210, 225, 0]], [0.86, [255, 120, 0]], [1, [255, 255, 255]]];
    const lut = new Uint8ClampedArray(256 * 3);
    for (let i = 0; i < 256; i++) {
      const t = i / 255;
      let k = 0;
      while (k < stops.length - 2 && t > stops[k + 1][0]) k++;
      const [t0, c0] = stops[k], [t1, c1] = stops[k + 1], f = (t - t0) / (t1 - t0);
      for (let j = 0; j < 3; j++) lut[i * 3 + j] = c0[j] + (c1[j] - c0[j]) * f;
    }
    return lut;
  })();

  // ---- the window ----------------------------------------------------------------
  function bandInfo() { return hello && band >= 0 ? hello.bands[band] : null; }
  // each band its IQ rate (the board's two IPs may run two); an older server: one
  function rateOf(b) { return (b && b.rate) || hello.rate; }
  function wanted() {
    const b = bandInfo();
    if (!b) return { lo: 0, hi: 0 };
    const half = rateOf(b) / 2;
    if (!opt.span || opt.span >= rateOf(b)) return { lo: b.centre_hz - half, hi: b.centre_hz + half };
    let c = opt.centre || b.centre_hz;
    c = Math.min(b.centre_hz + half - opt.span / 2, Math.max(b.centre_hz - half + opt.span / 2, c));
    opt.centre = c;
    return { lo: c - opt.span / 2, hi: c + opt.span / 2 };
  }
  const xOf = (hz) => ((hz - view.lo) / (view.hi - view.lo)) * W;
  const hzOf = (x) => view.lo + (x / W) * (view.hi - view.lo);

  // hist: ask for the picture so far, drawn anew at this window and width
  function send(hist) {
    if (!ws || ws.readyState !== 1 || band < 0) return;
    if (mini()) {                                      // away, listening: the pill's cut-out only
      const m = miniWin();
      miniAsked = true;
      ws.send(`band=${band}&lo=${m.lo.toFixed(1)}&hi=${m.hi.toFixed(1)}&w=${m.w}&rate=${opt.rate}`);
      return;
    }
    const v = wanted();
    ws.send(`band=${band}&lo=${v.lo.toFixed(1)}&hi=${v.hi.toFixed(1)}&w=${W}&rate=${opt.rate}${hist ? `&hist=${H}` : ''}`);
  }
  // A new window: while the wheel or the drag goes on, the picture is moved
  // along (a preview); 200 ms after the last change the skimmer sends the
  // picture so far again, at full resolution for the new window — the
  // stretched preview is replaced by a sharp one. A new band starts clean.
  function reframe(clear) {
    const v = wanted();
    if (clear || !view.hi) {
      g.fillStyle = '#00000c'; g.fillRect(0, 0, W, H);
      rowTimes = []; envelope = null;
      if (typeof Ft8Pal !== 'undefined') Ft8Pal.restamp();
      if (typeof Js8Pal !== 'undefined') Js8Pal.restamp();
    } else if (v.lo !== view.lo || v.hi !== view.hi) {
      const tmp = document.createElement('canvas'); tmp.width = W; tmp.height = H;
      tmp.getContext('2d').drawImage(cv, 0, 0);
      g.fillStyle = '#00000c'; g.fillRect(0, 0, W, H);
      const span = v.hi - v.lo, dx = ((view.lo - v.lo) / span) * W, dw = ((view.hi - view.lo) / span) * W;
      g.drawImage(tmp, dx, 0, dw, H);
    }
    view = v;
    drawScale();
    drawTop();
    spanUi();
    clearTimeout(sendT);
    sendT = setTimeout(() => send(true), 200);
    save();
  }

  // ---- WSPR --------------------------------------------------------------------------
  // While the skimmer decodes WSPR ([wspr], status.json "wspr"), each WSPR
  // frequency of the band shown is marked: the passband wsprd decodes
  // (dial + 1500 ± 150 Hz) dark red over the waterfall and the spectrum, and
  // "WSPR RX" where the calls stand (WSPR spots are not in the feed).
  function wsprHere() {
    const b = bandInfo(), w = typeof status !== 'undefined' && status && status.wspr;
    if (!b || !w || !w.on || !w.decoder || !w.dials) return [];
    return w.dials.filter((d) => d.band === b.name && d.lo_hz && d.hi_hz);
  }
  // [ft8] the same way, violet: each FT8/FT4 dial's window on the receiver
  // shown (status.json "ft8": dial + lo … hi Hz), "FT8 RX" / "FT4 RX"
  function ft8Here() {
    const f = typeof status !== 'undefined' && status && status.ft8;
    if (band < 0 || !f || !f.on || !f.decoder || !f.dials) return [];
    return f.dials.filter((d) => d.rx === band && d.lo_hz && d.hi_hz);
  }
  // [js8] gold: each JS8 dial's window (status.json "js8"), "JS8 RX"
  function js8Here() {
    const j = typeof status !== 'undefined' && status && status.js8;
    if (band < 0 || !j || !j.on || !j.decoder || !j.dials) return [];
    return j.dials.filter((d) => d.rx === band && d.lo_hz && d.hi_hz);
  }
  const PASS = { wspr: { fill: 'rgba(120,10,10,.45)', text: '#e05a5a', foot: '#8a1414' },
                 ft8: { fill: 'rgba(90,40,150,.26)', text: '#c59bff', foot: '#6b3fb0' },
                 js8: { fill: 'rgba(160,130,20,.26)', text: '#f2d46b', foot: '#a88a12' } };
  function passes() {
    return [...wsprHere().map((d) => ({ ...d, kind: 'wspr', label: 'WSPR RX' })),
            ...ft8Here().map((d) => ({ ...d, kind: 'ft8', label: `${d.mode} RX` })),
            ...js8Here().map((d) => ({ ...d, kind: 'js8', label: 'JS8 RX' }))];
  }
  function placeWspr() {
    const box = $('#wf-wspr');
    if (!box) return;
    const list = view.hi ? passes() : [], w = cv.clientWidth, span = view.hi - view.lo;
    while (box.children.length > list.length) box.lastChild.remove();
    while (box.children.length < list.length) box.appendChild(document.createElement('div'));
    list.forEach((d, i) => {
      const el = box.children[i];
      const x0 = ((d.lo_hz - view.lo) / span) * w, x1 = ((d.hi_hz - view.lo) / span) * w;
      el.hidden = x1 < 0 || x0 > w;
      el.className = d.kind;
      el.style.left = `${x0}px`; el.style.width = `${Math.max(2, x1 - x0)}px`;
    });
  }
  // over the strip: the passband and its label, upright like a call
  function drawWspr(w, h) {
    const list = passes();
    if (!list.length) return;
    const span = view.hi - view.lo;
    spg.font = '700 10px RC, sans-serif'; spg.textAlign = 'right'; spg.textBaseline = 'middle';
    for (const d of list) {
      const x0 = ((d.lo_hz - view.lo) / span) * w, x1 = ((d.hi_hz - view.lo) / span) * w;
      if (x1 < 0 || x0 > w) continue;
      spg.fillStyle = PASS[d.kind].fill;
      spg.fillRect(x0, 0, Math.max(2, x1 - x0), h);
      spg.fillStyle = PASS[d.kind].text;
      spg.save();
      spg.translate(Math.round((x0 + x1) / 2) + 0.5, 4);
      spg.rotate(-Math.PI / 2);
      spg.fillText(d.label, 0, 0);
      spg.restore();
    }
    spg.textAlign = 'start'; spg.textBaseline = 'alphabetic';
  }

  // ---- the scale and the spectrum --------------------------------------------------
  function drawScale() {
    const dpr = devicePixelRatio || 1, w = sc.clientWidth;
    sc.width = w * dpr; sc.height = 22 * dpr;
    sg.setTransform(dpr, 0, 0, dpr, 0, 0);
    sg.fillStyle = '#0a111a'; sg.fillRect(0, 0, w, 22);
    if (!view.hi) return;
    const span = view.hi - view.lo, px = w / span;
    const step = [50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000].find((s) => s * px >= 70) || 20000;
    sg.strokeStyle = '#4d5f76'; sg.fillStyle = '#9fb0c4'; sg.font = '11px RC, sans-serif'; sg.textAlign = 'center';
    for (let f = Math.ceil(view.lo / step) * step; f <= view.hi; f += step) {
      const x = (f - view.lo) * px;
      sg.beginPath(); sg.moveTo(x + 0.5, 15); sg.lineTo(x + 0.5, 22); sg.stroke();
      sg.fillText(fmtKhz(f / 1000, step < 1000 ? 1 : 0), x, 12);
    }
    for (let f = Math.ceil(view.lo / (step / 5)) * (step / 5); f <= view.hi; f += step / 5) {
      const x = (f - view.lo) * px;
      sg.beginPath(); sg.moveTo(x + 0.5, 19); sg.lineTo(x + 0.5, 22); sg.stroke();
    }
    // the audio's passband: a trapezoid, its base the passband, on the scale
    if (au.f) {
      const x0 = (au.f - abw() / 2 - view.lo) * px, x1 = (au.f + abw() / 2 - view.lo) * px;
      const inset = Math.min((x1 - x0) / 4, 6);
      sg.beginPath(); sg.moveTo(x0, 22); sg.lineTo(x0 + inset, 3); sg.lineTo(x1 - inset, 3); sg.lineTo(x1, 22); sg.closePath();
      sg.fillStyle = au.on ? 'rgba(255,196,64,.35)' : 'rgba(160,170,185,.25)'; sg.fill();
      sg.strokeStyle = au.on ? '#ffc440' : '#8796a8'; sg.lineWidth = 1; sg.stroke();
      // the palette's mark (green) and space (red), where its AFC has them
      if (pal()) {
        const m = RttyPal.markers();
        for (const [hz, c] of [[m.mark, m.reverse ? '#ff6b6b' : '#5ad18a'], [m.space, m.reverse ? '#5ad18a' : '#ff6b6b']]) {
          const x = Math.round((au.f + hz - view.lo) * px) + 0.5;
          sg.beginPath(); sg.moveTo(x, 3); sg.lineTo(x, 22); sg.strokeStyle = c; sg.lineWidth = 2; sg.stroke();
        }
        sg.lineWidth = 1;
      }
    }
    // WSPR's passbands dark red along the scale's foot, FT8/FT4's violet, JS8's gold
    for (const d of passes()) {
      sg.fillStyle = PASS[d.kind].foot;
      sg.fillRect((d.lo_hz - view.lo) * px, 19, Math.max(2, (d.hi_hz - d.lo_hz) * px), 3);
    }
    placePass();
    placeWspr();
  }
  // the passband over the waterfall too (an overlay, never in the picture)
  function placePass() {
    const el = $('#wf-pass');
    if (!el) return;
    el.hidden = !au.f || !view.hi;
    if (el.hidden) return;
    const w = cv.clientWidth, span = view.hi - view.lo;
    const x0 = ((au.f - abw() / 2 - view.lo) / span) * w, x1 = ((au.f + abw() / 2 - view.lo) / span) * w;
    el.style.left = `${x0}px`; el.style.width = `${Math.max(1, x1 - x0)}px`;
    el.classList.toggle('on', au.on);
  }

  // ---- the audio --------------------------------------------------------------------
  // A click in the waterfall tunes the skimmer's demodulator there; its audio
  // (µ-law, 8 kHz, ~8 kB/s) is played as it comes, a little ahead, the delay
  // kept short (a backlog is dropped). The speaker mutes: the skimmer stops.
  const au = { f: 0, on: false, ctx: null, next: 0 };
  let markT = 0;

  // ---- the S-meter ------------------------------------------------------------------
  // As an ICOM transceiver draws it: thin blocks along a scale S1 … S9, then
  // +20 … +60 dB in red. The level is the skimmer's: the passband's loudest
  // 10 ms of each audio packet, before the AGC, in dBFS (waterfall.h). dBm
  // = dBFS + SM_CAL: the Red Pitaya's ADC full scale (±1 V into 50 Ω) is
  // +10 dBm — an estimate, as a transceiver's meter is. S9 = −73 dBm, 6 dB
  // an S-unit; fast up, 12 dB/s down, the peak held a second.
  const SM_CAL = 10, SM_S0 = -127, SM_S9 = -73, SM_TOP = -13, SM_SPLIT = 0.56;
  const sm = { v: null, peak: null, peakT: 0, tipT: 0 };
  function smX(dbm, w) {
    const v = Math.max(SM_S0, Math.min(SM_TOP, dbm));
    return v <= SM_S9 ? ((v - SM_S0) / (SM_S9 - SM_S0)) * SM_SPLIT * w
      : (SM_SPLIT + ((v - SM_S9) / (SM_TOP - SM_S9)) * (1 - SM_SPLIT)) * w;
  }
  const smText = (dbm) => (dbm >= SM_S9 + 1 ? `S9+${Math.round(dbm - SM_S9)} dB` : `S${Math.max(0, Math.min(9, Math.round((dbm - SM_S0) / 6)))}`);
  function smDraw() {
    const c = $('#wf-smeter');
    if (!c) return;
    const dpr = devicePixelRatio || 1, w = c.clientWidth || 232, h = c.clientHeight || 34;
    if (c.width !== Math.round(w * dpr)) { c.width = Math.round(w * dpr); c.height = Math.round(h * dpr); }
    const x = c.getContext('2d');
    x.setTransform(dpr, 0, 0, dpr, 0, 0);
    x.fillStyle = '#05080c'; x.fillRect(0, 0, w, h);
    const L = 12, R = 22, bw = w - L - R;              // R: room for "+60dB"                  // the scale between its margins
    x.font = '700 10px RC, sans-serif'; x.textAlign = 'center'; x.textBaseline = 'alphabetic';
    x.fillStyle = '#e8eef6'; x.textAlign = 'left'; x.fillText('S', 1, 11); x.textAlign = 'center';
    for (const s of [1, 3, 5, 7, 9]) x.fillText(String(s), L + smX(SM_S0 + 6 * s, bw), 11);
    x.fillStyle = '#ff4a3d';
    for (const d of [20, 40, 60]) x.fillText(`+${d}`, L + smX(SM_S9 + d, bw), 11);
    x.textAlign = 'right'; x.font = '700 8px RC, sans-serif'; x.fillText('dB', w - 1, 11);
    // the scale's ticks: each S-unit, each 10 dB over S9
    x.fillStyle = '#7d8fa6';
    for (let s = 1; s <= 9; s++) x.fillRect(Math.round(L + smX(SM_S0 + 6 * s, bw)), 14, 1, s % 2 ? 3 : 2);
    x.fillStyle = '#a33';
    for (let d = 10; d <= 60; d += 10) x.fillRect(Math.round(L + smX(SM_S9 + d, bw)), 14, 1, d % 20 ? 2 : 3);
    // the blocks: 2 px wide, a pixel apart; lit to the level, the peak's one
    const lit = sm.v != null && au.on ? L + smX(sm.v, bw) : -1, pk = sm.peak != null && au.on ? L + smX(sm.peak, bw) : -1;
    const s9 = L + SM_SPLIT * bw;
    for (let px = L; px < L + bw - 1; px += 3) {
      const red = px >= s9, on = px < lit || (pk >= 0 && Math.abs(px - pk) < 1.5);
      x.fillStyle = on ? (red ? '#ff3b30' : '#f2f6fa') : (red ? '#3a1414' : '#1e2833');
      x.fillRect(px, 19, 2, 11);
    }
  }
  function smLevel(dbfs) {
    const dbm = dbfs + SM_CAL, now = Date.now();
    sm.v = sm.v == null || dbm > sm.v ? dbm : Math.max(dbm, sm.v - 1.2);   // a packet = 100 ms
    if (sm.peak == null || dbm >= sm.peak || now - sm.peakT > 1000) { sm.peak = Math.max(dbm, sm.v); sm.peakT = now; }
    smDraw();
    if (now - sm.tipT > 700) {
      sm.tipT = now;
      const c = $('#wf-smeter');
      if (c) c.title = `${smText(sm.v)} · ${Math.round(sm.v)} dBm (${Math.round(sm.v - SM_CAL)} dBFS in the passband; dBm = dBFS + ${SM_CAL}, an estimate — the Red Pitaya's full scale)`;
    }
  }
  function smOff() {
    sm.v = null; sm.peak = null;
    smDraw();
    const c = $('#wf-smeter');
    if (c) c.title = 'S-meter: the level in the audio\'s passband while listening';
  }
  const ULAW = (() => {
    const t = new Float32Array(256);
    for (let i = 0; i < 256; i++) {
      const u = ~i & 0xff, m = ((u & 0x0f) << 3) + 0x84, v = (m << ((u & 0x70) >> 4)) - 0x84;
      t[i] = ((u & 0x80) ? -v : v) / 32768;
    }
    return t;
  })();
  function audioSend() {
    if (!ws || ws.readyState !== 1) return;
    ws.send(au.on && au.f ? `audio=1&af=${au.f.toFixed(1)}&bw=${abw()}` : 'audio=0');
  }
  function audioUi() {
    const b = $('#wf-audio');
    b.classList.toggle('on', au.on);
    b.title = au.on ? `listening on ${fmtKhz(au.f / 1000, 2)} kHz — click to mute` : (au.f ? 'muted — click to listen' : 'click in the waterfall to listen there');
    const fi = $('#wf-freq');
    if (fi && document.activeElement !== fi) fi.value = au.f ? fmtKhz(au.f / 1000, 2) : '';
    if (fi) fi.classList.toggle('on', au.on);
    drawScale();
  }
  function audioTune(f) {
    au.f = f;
    au.on = true;
    if (pal()) RttyPal.tuned();
    if (!au.ctx) au.ctx = new (window.AudioContext || window.webkitAudioContext)();
    au.ctx.resume();
    au.next = 0;
    audioSend();
    audioUi();
  }
  // Tuning the frequency heard without a click (the wheel over the spectrum):
  // the marker moves, a listening demodulator follows a moment later (one
  // retune for a burst of notches), a muted one stays muted.
  let tuneT = 0;
  function audioMove(f) {
    au.f = f;
    if (pal()) RttyPal.tuned();
    if (au.on) { clearTimeout(tuneT); tuneT = setTimeout(audioSend, 60); }
    follow(f);
    audioUi();
  }
  // zoomed in, the view moves along when the frequency nears its edge
  function follow(f) {
    if (!opt.span || !view.hi) return;
    const m = (view.hi - view.lo) * 0.05;
    if (f < view.lo + m || f > view.hi - m) { opt.centre = f; reframe(false); }
  }
  // the palette moves the frequency heard (its click, AUTOTUNE)
  function retune(f) {
    const b = bandInfo();
    if (!b || !f) return;
    const half = rateOf(b) / 2;
    au.f = Math.max(b.centre_hz - half + 100, Math.min(b.centre_hz + half - 100, f));
    if (au.on) audioSend();
    if (mini()) { send(); awayUi(); }                  // the cut-out follows it
    follow(au.f);
    audioUi();
  }
  function state() {
    const b = bandInfo();
    return { band: b ? b.name : '', f: au.f, on: au.on, live: !!(ws && ws.readyState === 1 && hello) };
  }
  // opened: its passband, the audio on where it was tuned; closed: the
  // passband chosen here again
  function paletteChanged(open) {
    if (open && au.f && !au.on) audioTune(au.f);
    else if (au.on) audioSend();
    rttyUi();
    audioUi();
  }
  // ---- the FT8 palette's view -----------------------------------------------------
  // Opened: the view as it was is kept; a dial shown narrows it to the
  // dial's window on its receiver; closed: the view comes back.
  let ft8Prev = null;
  function ft8Opened() {
    ft8Prev = { band: opt.band, span: opt.span, centre: opt.centre };
  }
  function ft8Focus(d) {
    if (!hello || !hello.bands[d.rx]) return;
    const span = Math.round((d.hi_hz - d.lo_hz) * 1.08), centre = (d.lo_hz + d.hi_hz) / 2;
    if (d.rx !== band) {
      audioOff(true);
      band = d.rx;
      opt.band = hello.bands[band].name;
      shownBand = opt.band;
      $('#wf-band').value = String(band);
      opt.span = span; opt.centre = centre;
      bar();
      reframe(true);
    } else {
      opt.span = span; opt.centre = centre;
      bar();
      reframe(false);
    }
  }
  function ft8Closed() {
    const p = ft8Prev;
    ft8Prev = null;
    if (!p || !hello) return;
    const i = hello.bands.findIndex((b) => b.name === p.band);
    opt.span = p.span; opt.centre = p.centre;
    if (i >= 0 && i !== band) {
      band = i; opt.band = p.band; shownBand = p.band;
      $('#wf-band').value = String(band);
      bar();
      reframe(true);
    } else {
      bar();
      reframe(false);
    }
  }
  // the buttons: FT8 there only while the skimmer decodes FT8/FT4, JS8 while
  // it decodes JS8; on while its palette is open
  function ft8Ui() {
    const b = $('#wf-ft8'), j = $('#wf-js8');
    if (b) {
      b.hidden = typeof Ft8Pal === 'undefined' || !Ft8Pal.available();
      b.classList.toggle('on', ft8());
    }
    if (j) {
      j.hidden = typeof Js8Pal === 'undefined' || !Js8Pal.available();
      j.classList.toggle('on', typeof Js8Pal !== 'undefined' && Js8Pal.isOpen());
    }
  }
  function ft8Stamp() { const p = dpal(); if (p && g) p.stamp(g, rowTimes, xOf, W); }
  function rttyUi() {
    const b = $('#wf-rtty'), bw = $('#wf-bw');
    if (!b) return;
    b.classList.toggle('on', pal());
    // while it is open the list shows the palette's passband (one of its own if need be)
    const own = bw.querySelector('option[data-pal]');
    if (own) own.remove();
    if (pal() && !BWS.includes(abw())) bw.insertAdjacentHTML('beforeend', `<option data-pal value="${abw()}">${abw()} Hz</option>`);
    bw.value = String(abw());
    bw.disabled = pal();
    bw.title = pal() ? `the RTTY palette's passband, ${abw()} Hz (Settings → RTTY palette)` : "the audio's passband";
  }
  function audioOff(forget) {
    if (au.on) { au.on = false; audioSend(); }
    smOff();
    if (forget) au.f = 0;
    audioUi();
    if (away) { awayUi(); clearTimeout(leaveT); leaveT = setTimeout(close, 30000); }   // muted away: stop as away does
  }
  function onAudio(buf) {
    if (!au.on) return;
    const dv = new DataView(buf), n = dv.getUint16(2, true);
    if (dv.getUint8(1) !== band || !n) return;
    if (buf.byteLength >= 8 + n + 4) smLevel(dv.getFloat32(8 + n, true));   // the S-meter
    const u = new Uint8Array(buf, 8, n), d = new Float32Array(n);
    for (let i = 0; i < n; i++) d[i] = ULAW[u[i]];
    bytes += buf.byteLength;
    if (pal()) {
      RttyPal.samples(d);
      const t = Date.now();                            // its markers follow the AFC
      if (t - markT > 400) { markT = t; drawScale(); }
    }
    if (!au.ctx || (pal() && !RttyPal.sound())) return;
    const ab = au.ctx.createBuffer(1, n, 8000);
    ab.copyToChannel(d, 0);
    const now = au.ctx.currentTime;
    if (au.next < now + 0.05 || au.next > now + 0.8) au.next = now + 0.2;   // start, or drop a backlog
    const src = au.ctx.createBufferSource();
    src.buffer = ab; src.connect(au.ctx.destination); src.start(au.next);
    au.next += n / 8000;
  }
  // The strip over the scale: the spectrum's envelope (an option) and the
  // calls spotted on the band while the picture below was drawn (at most
  // 15 minutes), small, upright at their frequency, hanging from the top edge
  // (each fades and goes when its last spot leaves the picture). Shown while
  // either is on.
  let lastFloor = -130;
  function topShown() { return opt.spec || opt.calls; }
  function drawTop(dbFloor) {
    if (dbFloor != null) lastFloor = dbFloor;
    if (!topShown()) return;
    const dpr = devicePixelRatio || 1, w = sp.clientWidth, h = opt.spec ? 110 : 64;
    if (sp.width !== Math.round(w * dpr) || sp.height !== Math.round(h * dpr)) {
      sp.width = Math.round(w * dpr); sp.height = Math.round(h * dpr); sp.style.height = `${h}px`;
    }
    spg.setTransform(dpr, 0, 0, dpr, 0, 0);
    spg.fillStyle = '#05090e'; spg.fillRect(0, 0, w, h);
    if (opt.spec && envelope) {
      const lo = lastFloor - 10, hi = lastFloor + opt.range + 10, y = (db) => h - ((db - lo) / (hi - lo)) * h;
      spg.strokeStyle = '#1a283a'; spg.fillStyle = '#6a7d96'; spg.font = '10px RC, sans-serif'; spg.lineWidth = 1;
      for (let db = Math.ceil(lo / 10) * 10; db < hi; db += 10) {
        spg.beginPath(); spg.moveTo(0, y(db) + 0.5); spg.lineTo(w, y(db) + 0.5); spg.stroke();
        spg.fillText(`${db}`, 2, y(db) - 2);
      }
      // the envelope keeps the window it was made for: drawn where that
      // lies in the view now, it follows a zoom at once and never stops
      const ev = envelope.v, n = ev.length, es = (envelope.hi - envelope.lo) / n;
      const path = () => {
        spg.beginPath();
        for (let i = 0; i < n; i++) {
          const x = ((envelope.lo + (i + 0.5) * es - view.lo) / (view.hi - view.lo)) * w;
          if (i) spg.lineTo(x, y(ev[i])); else spg.moveTo(x, y(ev[i]));
        }
      };
      const xa = ((envelope.lo - view.lo) / (view.hi - view.lo)) * w, xb = ((envelope.hi - view.lo) / (view.hi - view.lo)) * w;
      path(); spg.lineTo(xb, h); spg.lineTo(xa, h); spg.closePath();
      spg.fillStyle = 'rgba(57,217,138,.18)'; spg.fill();
      path(); spg.strokeStyle = '#39d98a'; spg.lineWidth = 1.2; spg.stroke();
    }
    drawWspr(w, h);
    if (opt.calls) drawCalls(w);
    placeWspr();
  }

  // ---- a row -------------------------------------------------------------------
  // one row's bytes as colours (dB over the floor, the range wide)
  function rowImage(px, w) {
    if (rowCv.width !== w) rowCv.width = w;
    const dbFloor = (floorB || 0) / 2 - 160 - 3 + Number(opt.floor), scale = 255 / opt.range;
    const img = rowG.createImageData(w, 1), d = img.data;
    for (let i = 0; i < w; i++) {
      const c = px[i] ? Math.max(0, Math.min(255, Math.round((px[i] / 2 - 160 - dbFloor) * scale))) : 0;
      d[i * 4] = LUT[c * 3]; d[i * 4 + 1] = LUT[c * 3 + 1]; d[i * 4 + 2] = LUT[c * 3 + 2]; d[i * 4 + 3] = 255;
    }
    rowG.putImageData(img, 0, 0);
    return dbFloor;
  }
  function putRow(w, lo, hi, y) {
    g.imageSmoothingEnabled = false;
    g.drawImage(rowCv, 0, 0, w, 1, xOf(lo), y, xOf(hi) - xOf(lo), 1);
  }
  // the floor: a row's 20th percentile, smoothed
  function noteFloor(px) {
    const sorted = Array.from(px).filter((v) => v > 0).sort((a, b) => a - b);
    if (!sorted.length) return;
    const p20 = sorted[Math.floor(sorted.length * 0.2)];
    floorB = floorB == null ? p20 : floorB * 0.92 + p20 * 0.08;
  }
  function onRow(buf) {
    const dv = new DataView(buf), w = dv.getUint16(2, true), seq = dv.getUint32(4, true);
    const t = dv.getFloat64(8, true), lo = dv.getFloat64(16, true), hi = dv.getFloat64(24, true);
    if (ws && ws.readyState === 1) ws.send(`a=${seq}`);
    if (dv.getUint8(1) !== band) return;               // a row of the band left
    const px = new Uint8Array(buf, 32, w);
    if (mini()) { miniRow(px, w, lo, hi); bytes += buf.byteLength; return; }
    if (Math.abs((hi - lo) - MINI_SPAN) < 1 && Math.abs((hi - lo) - (view.hi - view.lo)) > 1) return;   // the cut-out's, late
    noteFloor(px);
    g.drawImage(cv, 0, 0, W, H - 1, 0, 1, W, H - 1);       // everything one row down
    g.fillStyle = '#00000c'; g.fillRect(0, 0, W, 1);
    const dbFloor = rowImage(px, w);
    putRow(w, lo, hi, 0);
    rowTimes.unshift(t);
    if (rowTimes.length > H) rowTimes.length = H;
    if (dpal()) dpal().row(g, W, t, rowTimes[1]);     // a cycle's start: a faint line
    // the envelope: its window's rows smoothed; a new window starts it anew
    if (opt.spec) {
      if (!envelope || envelope.lo !== lo || envelope.hi !== hi || envelope.v.length !== w) {
        envelope = { lo, hi, v: Array.from(px, (x) => x / 2 - 160) };
      } else {
        for (let i = 0; i < w; i++) envelope.v[i] = envelope.v[i] * 0.6 + (px[i] / 2 - 160) * 0.4;
      }
    }
    drawTop(dbFloor);
    bytes += buf.byteLength;
  }
  // The picture so far, from the skimmer's history at this window: each row
  // of the canvas takes the history row nearest its time (a row a 1/rate s,
  // the history a row a 0.2 s); a time with no history stays dark.
  function onHistory(buf) {
    const dv = new DataView(buf), w = dv.getUint16(2, true), count = dv.getUint32(4, true);
    const lo = dv.getFloat64(8, true), hi = dv.getFloat64(16, true);
    if (dv.getUint8(1) !== band || !count) return;
    const rows = [];
    for (let i = 0; i < count; i++) {
      const off = 24 + i * (8 + w);
      rows.push({ t: dv.getFloat64(off, true), px: new Uint8Array(buf, off + 8, w) });
    }
    g.fillStyle = '#00000c'; g.fillRect(0, 0, W, H);
    const t0 = rows[0].t;
    rowTimes = [];
    let k = 0;
    for (let y = 0; y < H; y++) {
      const ty = t0 - y / opt.rate;
      while (k + 1 < rows.length && Math.abs(rows[k + 1].t - ty) <= Math.abs(rows[k].t - ty)) k++;
      rowTimes.push(ty);
      if (Math.abs(rows[k].t - ty) > 0.35) continue;      // nothing then
      if (rows[k].img !== true) { rowImage(rows[k].px, w); rows[k].img = true; rows[k].cv = cloneRow(w); }
      g.imageSmoothingEnabled = false;
      g.drawImage(rows[k].cv, 0, 0, w, 1, xOf(lo), y, xOf(hi) - xOf(lo), 1);
    }
    if (dpal()) { dpal().restamp(); dpal().stamp(g, rowTimes, xOf, W); }   // the labels anew
    bytes += buf.byteLength;
  }
  function cloneRow(w) {
    const c = document.createElement('canvas'); c.width = w; c.height = 1;
    c.getContext('2d').drawImage(rowCv, 0, 0);
    return c;
  }

  // ---- the calls ------------------------------------------------------------------
  const CALL_KEEP_S = 15 * 60;                       // fetched; shown: what the picture covers
  // how far back the picture reaches: its oldest row once it is full (a slow
  // link brings fewer rows, so more time), else its height at the rows a
  // second asked for; a minute at least, 15 at most
  function shownS(now) {
    let old = 0;
    if (rowTimes.length >= H) for (let k = rowTimes.length - 1; k >= 0 && !old; k--) old = rowTimes[k];
    const s = old ? now - old : H / (opt.rate || 5);
    return Math.min(CALL_KEEP_S, Math.max(60, s));
  }
  let skew = 0;                                      // the skimmer's clock − ours (s)
  async function fetchSpots(force) {
    if (!force && Date.now() - spotsAt < 5000) return;
    spotsAt = Date.now();
    try {
      const r = await fetch(`api/spots?minutes=${CALL_KEEP_S / 60}`, { cache: 'no-store' });
      const d = await r.json();
      skew = d.now - Date.now() / 1000;
      spots = d.spots;
      drawTop();
    } catch (e) { /* the next try */ }
  }
  // the band's calls heard within the time the picture shows (each call a
  // trace still on screen), the newest spot of each, left to right, fading
  // with age; a call too close to its neighbour moves right of it (a tick
  // marks its true frequency)
  function drawCalls(w) {
    const b = bandInfo();
    if (!b || !view.hi) return;
    const now = Date.now() / 1000 + skew, last = new Map(), keep = shownS(now);
    for (const s of spots) {
      if (s.band !== b.name || now - s.t > keep) continue;
      const k = s.call;
      if (!last.has(k) || last.get(k).t < s.t) last.set(k, s);
    }
    const list = [...last.values()].map((s) => ({ s, x: ((s.khz * 1000 - view.lo) / (view.hi - view.lo)) * w }))
      .filter((c) => c.x >= 0 && c.x <= w).sort((a, b) => a.x - b.x);
    spg.font = '600 10px RC, sans-serif'; spg.textAlign = 'right'; spg.textBaseline = 'middle';
    let prev = -100;
    callHits = [];
    for (const c of list) {
      const x = Math.max(c.x, prev + 11);
      if (x > w - 4) break;
      prev = x;
      callHits.push({ x, s: c.s });
      spg.globalAlpha = 1 - 0.6 * Math.min(1, (now - c.s.t) / keep);
      spg.fillStyle = c.s.mode === 'RTTY' ? '#ffb88a' : c.s.mode === 'FT8' || c.s.mode === 'FT4' ? '#c59bff'
        : c.s.mode === 'JS8' ? '#f2d46b' : '#e8eef6';
      spg.fillRect(Math.round(c.x), 0, 1, 3);           // its frequency
      spg.save();
      spg.translate(Math.round(x) + 0.5, 4);
      spg.rotate(-Math.PI / 2);                         // reading upward, its end at the top edge
      spg.fillText(c.s.call, 0, 0);
      spg.restore();
    }
    spg.globalAlpha = 1; spg.textAlign = 'start'; spg.textBaseline = 'alphabetic';
  }

  // The tip at the mouse over the waterfall: the calls spotted near the
  // cursor's frequency in the last 15 min (the newest first — a trace still
  // on screen may be older than its last spot), WSPR RX inside a WSPR
  // passband, and the frequency there, small.
  function tip(e, f) {
    const el = $('#wf-tip'), b = bandInfo();
    if (!el) return;
    if (!b || !view.hi) { el.hidden = true; return; }
    const now = Date.now() / 1000 + skew, keep = CALL_KEEP_S;
    const tol = Math.max(60, ((view.hi - view.lo) / W) * 4), last = new Map();
    for (const s of spots) {
      if (s.band !== b.name || now - s.t > keep || Math.abs(s.khz * 1000 - f) >= tol) continue;
      if (!last.has(s.call) || last.get(s.call).t < s.t) last.set(s.call, s);
    }
    const near = [...last.values()].sort((a, c) => c.t - a.t);
    const wsprIn = wsprHere().some((d) => f >= d.lo_hz && f <= d.hi_hz);
    const ft8In = ft8Here().filter((d) => f >= d.lo_hz && f <= d.hi_hz).map((d) => `${d.mode} RX`);
    const js8In = js8Here().some((d) => f >= d.lo_hz && f <= d.hi_hz);
    el.innerHTML = near.slice(0, 4).map((s) =>
      `<b class="${s.mode === 'RTTY' ? 'rtty' : s.mode === 'FT8' || s.mode === 'FT4' ? 'ft8' : s.mode === 'JS8' ? 'js8' : ''}">${esc(s.call)}</b>`).join('') +
      (near.length > 4 ? `<span class="mut">+${near.length - 4}</span>` : '') +
      (wsprIn ? '<b class="wspr">WSPR RX</b>' : '') +
      ft8In.map((l) => `<b class="ft8">${l}</b>`).join('') +
      (js8In ? '<b class="js8">JS8 RX</b>' : '') +
      `<span class="f">${fmtKhz(f / 1000, 2)}</span>`;
    el.hidden = false;
    const box = $('#wf-box').getBoundingClientRect();
    let x = e.clientX - box.left + 14, y = e.clientY - box.top + 16;
    if (x + el.offsetWidth > box.width - 4) x = e.clientX - box.left - el.offsetWidth - 10;
    if (y + el.offsetHeight > box.height - 4) y = e.clientY - box.top - el.offsetHeight - 10;
    el.style.left = `${Math.max(0, x)}px`; el.style.top = `${Math.max(0, y)}px`;
  }

  // ---- the socket -----------------------------------------------------------------
  function netStatus(text, bad) {
    const el = $('#wf-net');
    el.textContent = text;
    el.classList.toggle('bad', !!bad);
  }
  function connect() {
    if (!active || ws) return;
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}${location.pathname.replace(/[^/]*$/, '')}ws/waterfall`;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    netStatus('connecting…');
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') {
        const kind = new DataView(e.data).getUint8(0);
        if (kind === 2) onAudio(e.data); else if (kind === 3) onHistory(e.data); else onRow(e.data);
        return;
      }
      let m = null;
      try { m = JSON.parse(e.data); } catch (err) { return; }
      if (m.type === 'audio' && m.error) { audioOff(false); netStatus(m.error, true); return; }
      if (m.type !== 'hello') return;
      hello = m;
      retry = 0;
      const sel = $('#wf-band');
      sel.innerHTML = m.bands.map((b, i) => `<option value="${i}">${b.name} · ${fmtKhz(b.centre_hz / 1000, 0)}</option>`).join('');
      const pi = pending ? m.bands.findIndex((b) => b.name === pending.name) : -1;
      if (pi >= 0) { opt.band = pending.name; if (opt.span) opt.centre = pending.hz; }
      const i = m.bands.findIndex((b) => b.name === opt.band);
      band = i >= 0 ? i : 0;
      sel.value = String(band);
      opt.band = m.bands[band] ? m.bands[band].name : '';
      // back after a pause: the picture stays, a line marks the gap
      const same = view.hi && shownBand === opt.band;
      reframe(!same);
      if (same) { g.fillStyle = 'rgba(125,143,166,.6)'; g.fillRect(0, 0, W, 1); rowTimes.unshift(0); }
      shownBand = opt.band;
      send(true);                                        // whatever the skimmer still keeps
      if (au.on) audioSend();                           // back: listening again
      if (pending) applyPending();                      // a row of the DX cluster sent us here
    };
    ws.onclose = (e) => {
      ws = null;
      if (!active) { netStatus(''); return; }
      netStatus(e.reason ? `closed: ${e.reason}` : 'reconnecting…', true);
      retry = Math.min(retry + 1, 6);
      setTimeout(connect, 1000 * retry);
    };
  }

  // ---- sizes and the bar ------------------------------------------------------------
  function size() {
    const box = $('#wf-box');
    if (!box.clientWidth) return false;               // the tab is not shown
    const w = Math.min(hello && hello.max_w ? hello.max_w : 2048, Math.max(200, Math.round(box.clientWidth)));
    const h = Math.max(200, Math.round(innerHeight - box.getBoundingClientRect().top - 12));
    if (w === W && h === H) return false;
    const keep = W ? (() => { const t = document.createElement('canvas'); t.width = W; t.height = H; t.getContext('2d').drawImage(cv, 0, 0); return t; })() : null;
    W = w; H = h;
    cv.width = W; cv.height = H; cv.style.height = `${H}px`;
    g.fillStyle = '#00000c'; g.fillRect(0, 0, W, H);
    if (keep) g.drawImage(keep, 0, 0, W, keep.height);
    if (rowTimes.length > H) rowTimes.length = H;
    return true;
  }
  // the span list: its steps, and the span the wheel left between them
  function spanUi() {
    const sel = $('#wf-span');
    if (!sel) return;
    const fmtS = (s) => (s ? `${fmtKhz(s / 1000, s < 10000 ? 1 : 0)} kHz` : 'whole band');
    const list = SPANS.includes(opt.span) ? SPANS : [...SPANS, opt.span].sort((a, b) => (a && b ? b - a : a ? 1 : -1));
    const html = list.map((s) => `<option value="${s}">${fmtS(s)}</option>`).join('');
    if (sel.dataset.html !== html) { sel.dataset.html = html; sel.innerHTML = html; }
    sel.value = String(opt.span);
  }
  function bar() {
    spanUi();
    $('#wf-rate').value = String(opt.rate);
    $('#wf-range').value = String(opt.range);
    $('#wf-floor').value = String(opt.floor);
    $('#wf-auto').classList.toggle('on', opt.range === DEF.range && opt.floor === DEF.floor);
    $('#wf-spec').checked = opt.spec;
    $('#wf-calls').checked = opt.calls;
    $('#wf-spec-cv').hidden = !topShown();
  }
  // another band: its own audio, the picture anew (centre: where to look)
  function setBand(i, centre) {
    audioOff(true);                                    // the audio stays with its band
    band = i;
    opt.band = hello.bands[band].name; opt.centre = centre || 0;
    shownBand = opt.band;
    $('#wf-band').value = String(band);
    reframe(true);
  }
  // Listen to hz on the band named (a row of the DX cluster, or the field):
  // that band, the view on it, the audio on. Before the hello it waits for it.
  let pending = null;
  function applyPending() {
    const p = pending;
    pending = null;
    if (!p || !hello) return;
    const i = hello.bands.findIndex((b) => b.name === p.name);
    if (i < 0) { netStatus(`${p.name} is not on this waterfall`, true); return; }
    // an FT8/FT4 spot: the FT8 palette on its dial (no audio: WSJT-X's tones)
    if ((p.mode === 'FT8' || p.mode === 'FT4') && typeof Ft8Pal !== 'undefined' && Ft8Pal.openAt(p.hz, p.mode)) return;
    // a JS8 spot: the JS8 palette on its dial (what the frames say)
    if (p.mode === 'JS8' && typeof Js8Pal !== 'undefined' && Js8Pal.openAt(p.hz)) return;
    if (i !== band) { setBand(i, p.hz); send(false); }   // the band first, then the audio in it
    else follow(p.hz);
    audioTune(p.hz);
    // an RTTY spot: the palette decodes it
    if (p.mode === 'RTTY' && typeof RttyPal !== 'undefined' && RttyPal.onSpot()) RttyPal.open();
  }
  function listen(name, hz, mode) {
    mount();
    // made now, in the click: a browser lets the audio start only from one
    try {
      if (!au.ctx) au.ctx = new (window.AudioContext || window.webkitAudioContext)();
      au.ctx.resume();
    } catch (e) { /* no audio here */ }
    pending = { name, hz: Math.round(hz), mode: mode || '' };
    if (hello && ws && ws.readyState === 1) applyPending();
  }
  // the field: kHz as shown; a number under 100 is MHz, from 100 000 Hz
  function parseFreq(text) {
    const v = Number(String(text).replace(/[\s\u00a0]/g, '').replace(',', '.'));
    if (!Number.isFinite(v) || v <= 0) return 0;
    return v < 100 ? v * 1e6 : v < 100000 ? v * 1000 : v;
  }
  function freqEntered(fi) {
    const hz = Math.round(parseFreq(fi.value));
    if (!hello || !hz) { fi.classList.add('bad'); return; }
    const inBand = (b) => Math.abs(hz - b.centre_hz) < rateOf(b) / 2;
    const i = bandInfo() && inBand(bandInfo()) ? band : hello.bands.findIndex(inBand);
    if (i < 0) { fi.classList.add('bad'); netStatus(`${fmtKhz(hz / 1000, 2)} kHz is in no band here`, true); return; }
    fi.classList.remove('bad');
    pending = { name: hello.bands[i].name, hz };
    applyPending();
    fi.value = fmtKhz(hz / 1000, 2);
  }

  let mounted = false;
  function mount() {
    if (mounted) return;
    mounted = true;
    cv = $('#wf-cv'); g = cv.getContext('2d');
    sc = $('#wf-scale'); sg = sc.getContext('2d');
    sp = $('#wf-spec-cv'); spg = sp.getContext('2d');
    rowCv = document.createElement('canvas'); rowCv.height = 1; rowG = rowCv.getContext('2d');
    spanUi();
    $('#wf-rate').innerHTML = RATES.map((r) => `<option value="${r}">${r} rows/s</option>`).join('');
    $('#wf-bw').innerHTML = BWS.map((b) => `<option value="${b}">${b} Hz</option>`).join('');
    $('#wf-bw').value = String(opt.abw);
    audioUi();
    smOff();
    bar();
    $('#wf-band').addEventListener('change', (e) => setBand(Number(e.target.value), 0));
    $('#wf-span').addEventListener('change', (e) => { opt.span = Number(e.target.value); reframe(false); });
    $('#wf-rate').addEventListener('change', (e) => { opt.rate = Number(e.target.value); save(); send(); });
    $('#wf-range').addEventListener('input', (e) => { opt.range = Number(e.target.value); save(); bar(); });
    $('#wf-floor').addEventListener('input', (e) => { opt.floor = Number(e.target.value); save(); bar(); });
    // auto: back to the default — 50 dB from black to white, the floor on the noise
    $('#wf-auto').addEventListener('click', () => { opt.range = DEF.range; opt.floor = DEF.floor; save(); bar(); });
    $('#wf-spec').addEventListener('change', (e) => {
      opt.spec = e.target.checked; envelope = null; save(); bar(); size(); drawTop(); send();
    });
    $('#wf-calls').addEventListener('change', (e) => { opt.calls = e.target.checked; save(); bar(); size(); drawTop(); });
    // the wheel zooms around the cursor, a drag pans, a double click centres
    const zoomAt = (x, dir) => {
      const b = bandInfo();
      if (!b) return;
      // smoothly: a notch of the wheel is a quarter in or out, 1 kHz at the least
      const f = hzOf(x), cur = opt.span || rateOf(b);
      const span = Math.max(1000, Math.min(rateOf(b), Math.round(cur * (dir > 0 ? 0.8 : 1.25))));
      opt.span = span >= rateOf(b) ? 0 : span;
      opt.centre = f - (x / W - 0.5) * span;          // the cursor's frequency stays put
      bar();
      reframe(false);
    };
    const xIn = (e) => ((e.clientX - cv.getBoundingClientRect().left) / cv.clientWidth) * W;
    for (const el of [cv, sc]) {
      el.addEventListener('wheel', (e) => { e.preventDefault(); zoomAt(xIn(e), e.deltaY < 0 ? 1 : -1); }, { passive: false });
    }
    // the wheel over the spectrum tunes: a notch a step of 10–100 Hz (finer
    // the closer the zoom), Shift 10 Hz; up is higher. Nothing tuned yet: it
    // starts at the cursor. A touchpad's small moves add up to a notch.
    let tuneAcc = 0;
    sp.addEventListener('wheel', (e) => {
      e.preventDefault();
      const b = bandInfo();
      if (!b || !view.hi) return;
      const d = e.deltaY || e.deltaX;                  // Shift turns the wheel sideways
      tuneAcc += e.deltaMode ? d * 33 : d;
      if (Math.abs(tuneAcc) < 30) return;
      const dir = tuneAcc < 0 ? 1 : -1;
      tuneAcc = 0;
      const hzPx = (view.hi - view.lo) / (sp.clientWidth || W);
      const step = e.shiftKey ? 10 : [10, 20, 50, 100].find((s) => s >= hzPx * 2) || 100;
      const x = ((e.clientX - sp.getBoundingClientRect().left) / sp.clientWidth) * W;
      const f = au.f ? Math.round((au.f + dir * step) / step) * step : Math.round(hzOf(x) / step) * step;
      const half = rateOf(b) / 2;
      audioMove(Math.max(b.centre_hz - half + 100, Math.min(b.centre_hz + half - 100, f)));
    }, { passive: false });
    const fi = $('#wf-freq');
    fi.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); freqEntered(fi); }
      if (e.key === 'Escape') { fi.classList.remove('bad'); fi.blur(); }
    });
    fi.addEventListener('focus', () => fi.select());
    fi.addEventListener('blur', () => { fi.classList.remove('bad'); audioUi(); });
    let drag = null;
    let down = null;                                   // a press: a click, or the start of a drag
    cv.addEventListener('pointerdown', (e) => {
      down = { x: xIn(e) };
      if (opt.span) drag = { x: xIn(e), c: opt.centre };
      try { cv.setPointerCapture(e.pointerId); } catch (err) { /* not a live pointer */ }
    });
    cv.addEventListener('pointermove', (e) => {
      const x = xIn(e);
      tip(e, hzOf(x));
      if (!drag) return;
      opt.centre = drag.c - ((x - drag.x) / W) * (view.hi - view.lo);
      reframe(false);
    });
    // a click (no drag): listen there
    cv.addEventListener('pointerup', (e) => {
      if (down && Math.abs(xIn(e) - down.x) < 4) audioTune(Math.round(hzOf(xIn(e))));
      down = null; drag = null;
    });
    cv.addEventListener('pointercancel', () => { down = null; drag = null; });
    cv.addEventListener('pointerleave', () => { $('#wf-tip').hidden = true; });
    $('#wf-rtty').addEventListener('click', () => {
      if (typeof RttyPal === 'undefined') return;
      // made in the click: a browser lets the audio start only from one
      try {
        if (!au.ctx) au.ctx = new (window.AudioContext || window.webkitAudioContext)();
        au.ctx.resume();
      } catch (e) { /* no audio here */ }
      RttyPal.toggle();
    });
    rttyUi();
    $('#wf-ft8').addEventListener('click', () => { if (typeof Ft8Pal !== 'undefined') Ft8Pal.toggleOn(band); });
    $('#wf-js8').addEventListener('click', () => { if (typeof Js8Pal !== 'undefined') Js8Pal.toggleOn(band); });
    ft8Ui();
    // a click on a call over the waterfall: listen there (RTTY: the palette too)
    sp.addEventListener('click', (e) => {
      const x = e.clientX - sp.getBoundingClientRect().left;
      let best = null;
      for (const h of callHits) if (Math.abs(h.x - x) <= 6 && (!best || Math.abs(h.x - x) < Math.abs(best.x - x))) best = h;
      if (!best || !opt.calls) return;
      pending = { name: best.s.band, hz: Math.round(best.s.khz * 1000), mode: best.s.mode };
      applyPending();
    });
    sp.addEventListener('mousemove', (e) => {
      const x = e.clientX - sp.getBoundingClientRect().left;
      sp.style.cursor = opt.calls && callHits.some((h) => Math.abs(h.x - x) <= 6) ? 'pointer' : '';
    });
    $('#wf-audio').addEventListener('click', () => {
      if (au.on) audioOff(false); else if (au.f) audioTune(au.f);
    });
    $('#wf-bw').addEventListener('change', (e) => {
      opt.abw = Number(e.target.value); save();
      if (au.on) audioSend();
      audioUi();
    });
    cv.addEventListener('dblclick', (e) => { if (opt.span) { opt.centre = hzOf(xIn(e)); reframe(false); } });
    addEventListener('resize', () => { if (active && size()) { drawScale(); send(); } });
    setInterval(() => {                              // kB/s, the spots
      if (!active) return;
      const now = Date.now();
      if (ws && ws.readyState === 1) netStatus(`${((bytes / 1024) / ((now - bytesAt) / 1000)).toFixed(1)} kB/s`);
      bytes = 0; bytesAt = now;
      if (opt.calls) fetchSpots();
      ft8Ui();                                       // [ft8] on or off: the status says
    }, 2000);
  }

  // Away (another tab, a hidden window) the stream stops after 30 s — the
  // skimmer then computes nothing for this page; back sooner, nothing stopped.
  // Not while listening: the station stays on the air until muted (a pill
  // in the corner says what is heard, mutes it, leads back), then 30 s.
  // Over the pill, a live cut-out of the waterfall: the frequency heard in
  // the middle, 1 kHz either side, two lines at the passband's edges — no
  // spectrum. Meanwhile the socket asks for this window alone (small, the
  // speed chosen); back on the tab, the full picture comes again, history
  // and all.
  const MINI_SPAN = 2000, MINI_H = 80;
  let miniAsked = false, miniFloor = null, mcv = null, mg = null;
  const mini = () => away && au.on && !!au.f;
  function miniWin() {
    const w = Math.max(120, Math.min(600, Math.round((mcv && mcv.clientWidth) || 260)));
    return { lo: au.f - MINI_SPAN / 2, hi: au.f + MINI_SPAN / 2, w };
  }
  function miniRow(px, w, lo, hi) {
    const m = miniWin();
    if (!mg || Math.abs(lo - m.lo) > 1 || Math.abs(hi - m.hi) > 1) return;   // another window's, late
    if (mcv.width !== m.w) { mcv.width = m.w; mcv.height = MINI_H; mg.fillStyle = '#00000c'; mg.fillRect(0, 0, m.w, MINI_H); }
    const sorted = Array.from(px).filter((v) => v > 0).sort((a, b) => a - b);
    if (sorted.length) { const p = sorted[Math.floor(sorted.length * 0.2)]; miniFloor = miniFloor == null ? p : miniFloor * 0.9 + p * 0.1; }
    const dbFloor = (miniFloor || 0) / 2 - 160 - 3 + Number(opt.floor), scale = 255 / opt.range;
    const img = mg.createImageData(w, 1), d = img.data;
    for (let i = 0; i < w; i++) {
      const c = px[i] ? Math.max(0, Math.min(255, Math.round((px[i] / 2 - 160 - dbFloor) * scale))) : 0;
      d[i * 4] = LUT[c * 3]; d[i * 4 + 1] = LUT[c * 3 + 1]; d[i * 4 + 2] = LUT[c * 3 + 2]; d[i * 4 + 3] = 255;
    }
    mg.drawImage(mcv, 0, 0, mcv.width, MINI_H - 1, 0, 1, mcv.width, MINI_H - 1);   // one row down
    const tmp = miniRow.cv || (miniRow.cv = document.createElement('canvas'));
    tmp.width = w; tmp.height = 1;
    tmp.getContext('2d').putImageData(img, 0, 0);
    mg.imageSmoothingEnabled = false;
    mg.drawImage(tmp, 0, 0, w, 1, 0, 0, mcv.width, 1);
  }
  function awayUi() {
    let el = $('#wf-away');
    if (!el) {
      el = document.createElement('div');
      el.id = 'wf-away';
      el.innerHTML = '<div class="wf-away-mini" title="the frequency heard in the middle, 1 kHz either side; the lines: the passband">' +
        '<canvas height="80"></canvas><i class="pb l"></i><i class="pb r"></i><i class="mid"></i>' +
        '<span class="lo">−1 kHz</span><span class="hi">+1 kHz</span></div>' +
        '<div class="wf-away-btns"><button type="button" class="wf-away-go" title="back to the waterfall"></button>' +
        '<button type="button" class="wf-away-mute" title="stop listening">mute</button></div>';
      el.querySelector('.wf-away-go').addEventListener('click', () => show('waterfall'));
      el.querySelector('.wf-away-mute').addEventListener('click', () => audioOff(false));
      document.body.appendChild(el);
      mcv = el.querySelector('canvas'); mg = mcv.getContext('2d');
    }
    el.hidden = !(away && au.on);
    const b = bandInfo();
    el.querySelector('.wf-away-go').textContent = `listening ${b ? `${b.name} · ` : ''}${fmtKhz(au.f / 1000, 2)} kHz`;
    // the passband's edges, in the cut-out's 2 kHz
    const half = Math.min(50, (abw() / 2 / MINI_SPAN) * 100);
    el.querySelector('.pb.l').style.left = `${50 - half}%`;
    el.querySelector('.pb.r').style.left = `${50 + half}%`;
  }
  function enter() {
    mount();
    const back = miniAsked;                           // the pill's cut-out was streaming
    miniAsked = false;
    away = false;
    awayUi();
    clearTimeout(leaveT);
    active = true;
    size();
    drawScale();
    fetchSpots(true);                                // the last 15 min at once
    connect();
    if (ws && ws.readyState === 1) send(back);        // after the cut-out: the picture so far again
  }
  function close() {
    away = false;
    active = false;
    audioOff(false);
    if (ws) { const s = ws; ws = null; s.close(1000, 'away'); }
  }
  function leave() {
    away = true;
    clearTimeout(leaveT);
    if (au.on) {
      awayUi();
      miniFloor = null;
      if (mg) { mg.fillStyle = '#00000c'; mg.fillRect(0, 0, mcv.width, mcv.height); }
      send();                                          // the cut-out's window
      return;
    }
    leaveT = setTimeout(close, 30000);
  }
  return { enter, leave, listen, retune, state, paletteChanged, ft8Opened, ft8Focus, ft8Closed, ft8Ui, ft8Stamp };
})();
