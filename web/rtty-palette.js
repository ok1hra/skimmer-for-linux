// rtty-palette.js — the RTTY palette over the waterfall: what the waterfall's
// audio brings (the skimmer's demodulator, 8 kHz, the tuned frequency at a
// 600 Hz tone) decoded in the browser, as WIFILT's QRPlog palette does with
// the radio's audio (ok1hra/wifilt data/log-rtty-panel.js): its small audio
// scope with the mark/space markers, the two-decoder tape, AFC and AUTOTUNE.
// The decoding and drawing modules are WIFILT's, unchanged (web/rtty/,
// VENDOR.md); this file is what differs here — the audio comes from the
// waterfall (waterfall.js hands every packet over), tuning moves the
// waterfall's listening frequency instead of a radio's dial, and the
// settings are the config's [rtty_palette] (Settings → RTTY palette, in
// status.json), not a page of their own.
//
// Opened by a click on an RTTY spot (the DX cluster's speaker, a call over
// the waterfall) or by RTTY in the waterfall's bar. It lives inside the
// Waterfall tab, so another tab hides it. Where it stands and how big it is
// is kept per browser (localStorage), and so is CLEAN.
'use strict';

const RttyPal = (() => {
  const RATE = 8000;                 // the audio's sample rate (waterfall.h SKIM_WF_AUDIO_RATE)
  const TONE = 600;                  // where the tuned frequency is heard (SKIM_WF_AUDIO_BFO)
  const SHIFT = 170;
  const DEF = { open_on_spot: true, bandwidth: 400, autotune: true, afc: true, afc_max_hz: 60,
                afc_rate: 60, reverse: false, usos: true, squelch_db: 0, second_decoder: true,
                clean: false, sound: true };
  const KEY = 'skrtty';
  const MIN_W = 360, MIN_H = 260;
  const mem = (() => { try { return JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { return {}; } })();
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(mem)); } catch (e) { /* private */ } };

  let el = null, open = false;
  let decoder = null, decoder2 = null, dec2On = false, rxLog = null, scope = null, afc = null, autotune = null;
  let effective = null, cfgSeen = '', rev = null;     // rev: REV pressed in this palette (null = the config's)
  let autoDone = false, lastAutoValues = null, flash = null, flashT = 0, stateT = 0;

  // the config's [rtty_palette], as the last status.json brought it (app.js)
  function cfg() {
    const s = typeof status !== 'undefined' && status && status.rtty_palette;
    return { ...DEF, ...(s || {}) };
  }
  const clean = () => (typeof mem.clean === 'boolean' ? mem.clean : cfg().clean);
  const wf = () => (typeof Wf !== 'undefined' ? Wf : null);

  // WIFILT's settings shape, as its modules read it; the tone is the demodulator's
  function settings() {
    const c = cfg();
    return {
      toneHz: TONE, txPolarity: 'normal', reverse: rev === null ? c.reverse : rev,
      squelchDb: c.squelch_db, usos: c.usos, afcEnabled: c.afc, afcRateHzPerChar: c.afc_rate,
      afcMaxDeviationHz: c.afc_max_hz, secondDecoder: c.second_decoder, cleanText: clean(),
    };
  }
  function apply() {
    effective = settings();
    cfgSeen = JSON.stringify(cfg());
    if (!decoder) return;
    for (const d of [decoder, decoder2]) {
      d.setReverse(effective.reverse);
      d.setSquelchDb(effective.squelchDb);
      d.setUsos(effective.usos);
      d.setToneOffset(TONE + (afc ? afc.offsetHz() : 0));
    }
    // DEC 2 that sat out starts clean, on DEC 1's sample clock (the tape lines them up by it)
    if (effective.secondDecoder && !dec2On) { decoder2.reset(); decoder2.totalSamples = decoder.totalSamples; }
    dec2On = !!effective.secondDecoder;
    rxLog.setDual(dec2On);
    rxLog.setClean(effective.cleanText);
    pills();
    if (scope) scope.drawOverlay();
  }

  // ---- the audio, from the waterfall ------------------------------------------------
  function samples(f32) {
    if (!open || !decoder) return;
    decoder.pushSamples(f32);
    if (dec2On) decoder2.pushSamples(f32);
    scope.ingest(f32);
  }

  // ---- tuning: the waterfall's frequency, not a dial --------------------------------
  // The audio heard at TONE + x Hz is the tuned frequency + x.
  function retuneBy(dHz) {
    const w = wf(), st = w && w.state();
    if (!st || !st.f) return false;
    w.retune(Math.round(st.f + dHz));
    afc.reset();
    autotune.clear(RttyAfc.AUTOTUNE.holdoffMs);
    return true;
  }
  // a click in the palette's scope: the pair's lower tone (space) where it was clicked
  function onScopeTune(lowHz) { retuneBy(lowHz + SHIFT / 2 - TONE); }
  // the waterfall was tuned (a click, a spot, the wheel): what AFC and AUTOTUNE knew is stale
  function tuned() {
    autoDone = false;
    if (!afc) return;
    afc.reset();
    autotune.clear(RttyAfc.AUTOTUNE.holdoffMs);
  }

  // AUTOTUNE: the pair finder over the scope's live spectrum, a sample each
  // 0.5 s; five that agree are a sync. With [rtty_palette] autotune it is
  // applied once by itself after each tuning; the pill applies the last sync.
  function finderRange() { return Math.max(20, Math.round(cfg().bandwidth / 2 - SHIFT / 2 - 10)); }
  function autoTick() {
    if (!autotune) return;
    const st = wf() && wf().state();
    if (!st || !st.on) { pillAuto(); return; }
    const values = scope.waterfall.state().liveValues;
    if (values && values !== lastAutoValues) {
      lastAutoValues = values;
      autotune.observe(RttyAfc.findOffset(values, {
        lowHz: scope.waterfall.lowHz, highHz: scope.waterfall.highHz,
        markHz: TONE + SHIFT / 2, spaceHz: TONE - SHIFT / 2, maxDeviationHz: finderRange(), prominenceDb: 8,
      }));
    }
    if (!autoDone && cfg().autotune) {
      const s = autotune.summary();
      if (s.synced) {
        autoDone = true;
        if (Math.abs(s.syncHz) >= RttyAfc.AUTOTUNE.deadBandHz) {
          const d = Math.round(s.syncHz);
          retuneBy(d);
          say(`autotune ${d > 0 ? '+' : '−'}${Math.abs(d)} Hz`);
        }
      }
    }
    pillAuto();
  }
  function runAutotune() {
    const d = autotune.decide();
    if (d.action === 'none') { say('no steady pair', true); return; }
    if (d.action === 'on-mark') { say('on the markers'); return; }
    const hz = Math.round(d.offsetHz);
    if (retuneBy(hz)) say(`autotune ${hz > 0 ? '+' : '−'}${Math.abs(hz)} Hz`);
  }
  let autoLook = '';
  function pillAuto() {
    const p = el && el.querySelector('.rp-auto');
    if (!p) return;
    const s = autotune.summary();
    const look = `${s.synced}|${s.lastSyncHz === null ? '' : Math.round(s.lastSyncHz)}`;
    if (look === autoLook) return;
    autoLook = look;
    p.classList.toggle('on', s.synced);
    p.title = 'AUTOTUNE: move the listening frequency so the mark/space pair sits on the markers. ' +
      (s.synced ? 'Green: a steady pair.' : s.lastSyncHz !== null
        ? `A press applies the last one (${Math.round(s.lastSyncHz) > 0 ? '+' : '−'}${Math.abs(Math.round(s.lastSyncHz))} Hz).`
        : 'No steady pair yet.');
  }

  // ---- the head --------------------------------------------------------------------------
  function say(text, bad) {
    flash = { text, bad: !!bad };
    clearTimeout(flashT);
    flashT = setTimeout(() => { flash = null; state(); }, 3000);
    state();
  }
  function state() {
    if (!el) return;
    const w = wf(), st = w ? w.state() : null, s = el.querySelector('.rp-state');
    let text, bad = false;
    if (flash) ({ text, bad } = flash);
    else if (!st || !st.live) { text = 'no waterfall'; bad = true; }
    else if (!st.f) text = 'click in the waterfall to tune';
    else if (!st.on) { text = `${fmtKhz(st.f / 1000, 2)} kHz — the speaker is off`; bad = true; }
    else text = `${st.band} · ${fmtKhz(st.f / 1000, 2)} kHz${cfg().sound ? '' : ' · silent'}`;
    if (s.textContent !== text) s.textContent = text;
    s.classList.toggle('bad', bad);
  }
  function pills() {
    if (!el) return;
    const c = el.querySelector('.rp-clean'), r = el.querySelector('.rp-rev');
    c.classList.toggle('on', effective.cleanText);
    c.setAttribute('aria-pressed', String(effective.cleanText));
    r.classList.toggle('on', effective.reverse);
    r.setAttribute('aria-pressed', String(effective.reverse));
  }
  function zoom(pct) {
    scope.setZoom(pct);
    for (const b of el.querySelectorAll('.rp-zoom button')) b.classList.toggle('on', Number(b.dataset.zoom) === pct);
  }

  // ---- where it stands ------------------------------------------------------------------
  function clampBox() {
    const vw = innerWidth, vh = innerHeight;
    mem.w = Math.max(MIN_W, Math.min(vw - 8, Math.round(mem.w || 460)));
    mem.h = Math.max(MIN_H, Math.min(vh - 8, Math.round(mem.h || 330)));
    if (typeof mem.x !== 'number' || typeof mem.y !== 'number') {
      // first opening: the top right of the waterfall
      const box = document.querySelector('#wf-box'), r = box ? box.getBoundingClientRect() : null;
      mem.x = r ? r.right - mem.w - 8 : vw - mem.w - 16;
      mem.y = r ? Math.max(60, r.top + 8) : 80;
    }
    mem.x = Math.max(0, Math.min(vw - mem.w, mem.x));
    mem.y = Math.max(0, Math.min(vh - mem.h, mem.y));
  }
  function place() {
    clampBox();
    el.style.left = `${mem.x}px`; el.style.top = `${mem.y}px`;
    el.style.width = `${mem.w}px`; el.style.height = `${mem.h}px`;
  }
  function mountDrag(head) {
    let d = null;
    head.addEventListener('pointerdown', (e) => {
      if (e.target.closest('button')) return;
      d = { dx: e.clientX - el.offsetLeft, dy: e.clientY - el.offsetTop };
      try { head.setPointerCapture(e.pointerId); } catch (err) { /* not a live pointer */ }
      e.preventDefault();
    });
    head.addEventListener('pointermove', (e) => {
      if (!d) return;
      mem.x = e.clientX - d.dx; mem.y = e.clientY - d.dy;
      clampBox();
      el.style.left = `${mem.x}px`; el.style.top = `${mem.y}px`;
    });
    const end = () => { if (d) { d = null; save(); } };
    head.addEventListener('pointerup', end);
    head.addEventListener('pointercancel', end);
  }

  // ---- build -------------------------------------------------------------------------------
  function build() {
    el = document.createElement('div');
    el.className = 'rtty-pal';
    el.hidden = true;
    el.innerHTML = `
      <div class="rp-head">
        <span class="rp-title">RTTY</span>
        <span class="rp-zoom" role="group" aria-label="zoom" title="zoom the audio scope around the tone">
          <button type="button" data-zoom="100" class="on">100%</button><button type="button" data-zoom="200">200%</button></span>
        <button type="button" class="rp-pill rp-rev" aria-pressed="false" title="REV: mark and space swapped (this palette, until it closes)">REV</button>
        <button type="button" class="rp-pill rp-clean" aria-pressed="false" title="CLEAN: each decoder's text in the order decoded, no time slots (kept in this browser). Off: the time-aligned tape">CLEAN</button>
        <button type="button" class="rp-pill rp-auto">AUTOTUNE</button>
        <span class="rp-state"></span>
        <button type="button" class="rp-close" title="close (the audio stays as it is)" aria-label="close">×</button>
      </div>
      <div class="rp-scope" title="the audio, 45.45 Bd / 170 Hz: green mark, red space; click to put a pair on the markers">
        <div class="rp-live"><canvas width="1024" height="81"></canvas></div>
        <div class="rp-wf"><canvas width="320" height="64"></canvas></div>
        <canvas class="rp-overlay"></canvas>
      </div>
      <div class="rp-rx" aria-live="polite"></div>`;
    document.querySelector('#tab-waterfall').appendChild(el);
    const c = effective = settings();
    decoder = new RttyCodec.Decoder(RATE, { toneHz: TONE, reverse: c.reverse, squelchDb: c.squelchDb, usos: c.usos });
    // DEC 2: the same decoder averaging over each bit, with the character rhythm (WIFILT §21)
    decoder2 = new RttyCodec.Decoder(RATE, { toneHz: TONE, reverse: c.reverse, squelchDb: c.squelchDb, usos: c.usos,
                                             bitDecision: 'integrate', dpll: true });
    dec2On = !!c.secondDecoder;
    rxLog = RttyRxLog.create({
      el: el.querySelector('.rp-rx'), maxChars: 8000, floorRgb: RttyRxLog.floorRgbFrom(el),
      dual: dec2On, clean: c.cleanText, onToken: () => {},
    });
    decoder.onChar((ch, meta) => rxLog.pushChar(ch, { ...meta, stream: 1 }));
    decoder2.onChar((ch, meta) => rxLog.pushChar(ch, { ...meta, stream: 2 }));
    decoder.onEvent((e) => { if (e.type === 'shift') rxLog.pushShift({ stream: 1, t: e.t }); });
    decoder2.onEvent((e) => { if (e.type === 'shift') rxLog.pushShift({ stream: 2, t: e.t }); });
    afc = RttyAfc.createTracker({
      settings: () => effective,
      liveValues: () => scope.waterfall.state().liveValues,
      window: () => ({ lowHz: scope.waterfall.lowHz, highHz: scope.waterfall.highHz }),
      markSpace: () => RttyScope.expectedMarkSpaceHz(effective),
      squelchOpen: () => decoder.squelchOpen,
      onOffset: (off) => { decoder.setToneOffset(TONE + off); decoder2.setToneOffset(TONE + off); },
      charDurationMs: RttyCodec.CHAR_DURATION_MS,
    });
    autotune = RttyAfc.createAutotune();
    scope = RttyScope.create({
      scopeEl: el.querySelector('.rp-scope'),
      liveCanvas: el.querySelector('.rp-live canvas'), liveContainer: el.querySelector('.rp-live'),
      waterfallCanvas: el.querySelector('.rp-wf canvas'), waterfallContainer: el.querySelector('.rp-wf'),
      overlayCanvas: el.querySelector('.rp-overlay'),
      sampleRate: RATE, baseLowHz: TONE - 350, baseHighHz: TONE + 350,   // the passband is ≤ 500 Hz
      settings: () => effective, afcOffsetHz: () => afc.offsetHz(), showFskLabel: false,
      onFrame: () => { afc.tick(); autoTick(); }, onTune: onScopeTune,
    });

    mountDrag(el.querySelector('.rp-head'));
    el.querySelector('.rp-close').addEventListener('click', () => setOpen(false));
    el.querySelector('.rp-zoom').addEventListener('click', (e) => {
      const b = e.target.closest('button');
      if (b) zoom(Number(b.dataset.zoom));
    });
    el.querySelector('.rp-clean').addEventListener('click', () => { mem.clean = !clean(); save(); apply(); });
    el.querySelector('.rp-rev').addEventListener('click', () => { rev = !effective.reverse; apply(); });
    el.querySelector('.rp-auto').addEventListener('click', runAutotune);
    // the browser's own resize handle: every pixel more is decoded text
    new ResizeObserver(() => {
      if (!open) return;
      mem.w = el.offsetWidth; mem.h = el.offsetHeight;
      scope.resize();
      save();
    }).observe(el);
    addEventListener('resize', () => { if (open) { place(); scope.resize(); } });
  }

  // ---- open / close -----------------------------------------------------------------------
  function setOpen(v) {
    if (!!v === open) return;
    open = !!v;
    if (open && !el) build();
    el.hidden = !open;
    if (open) {
      rev = null;                                       // REV is for the station it was pressed on
      apply();
      place();
      scope.resize();
      scope.start();
      tuned();
      state();
      stateT = setInterval(() => {
        if (JSON.stringify(cfg()) !== cfgSeen) apply();   // Settings saved: the next status brings it
        state();
      }, 500);
    } else {
      scope.stop();
      clearInterval(stateT);
    }
    const w = wf();
    if (w) w.paletteChanged(open);                     // its passband, the audio on, the button
  }

  return {
    open: () => setOpen(true),
    close: () => setOpen(false),
    toggle: () => setOpen(!open),
    isOpen: () => open,
    onSpot: () => cfg().open_on_spot,
    bandwidth: () => cfg().bandwidth,
    sound: () => cfg().sound,
    // the markers on the waterfall's scale: Hz from the tuned frequency
    markers: () => ({ mark: SHIFT / 2 + (afc ? afc.offsetHz() : 0), space: -SHIFT / 2 + (afc ? afc.offsetHz() : 0),
                      reverse: !!(effective && effective.reverse) }),
    samples, tuned,
  };
})();
