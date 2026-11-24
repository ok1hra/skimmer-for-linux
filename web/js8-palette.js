// js8-palette.js — the JS8 palette over the waterfall: what the skimmer
// decodes on a JS8 dial ([js8], src/headless/js8.c — JS8Call-improved's own
// decoder, every submode every period, the whole window), shown as JS8Call
// shows it: the frames put back together into messages, each with its
// last period's UTC, submode, dB and audio offset. The skimmer sends raw
// frames (/api/js8); WIFILT's Js8Protocol (web/js8/js8-protocol.js, its
// ActivityStore and the JSC dictionary js8-jsc.bin) reads them — the
// compressed text too. It decodes no audio itself.
//
// A deep frame whose call is not confirmed yet (js8.h: under [js8] clean,
// not decoded twice, not in MASTER.SCP, never confirmed on JS8) shows
// faded with a "?": on the page, not yet on the telnet feed.
//
// Opening it narrows the waterfall to the dial's window on its band (as
// the FT8 palette, which it closes) and stands each frame's sender upright
// on its trace in the middle of its period; closing it brings the view
// back. Opened by JS8 in the waterfall's bar, a click on a JS8 spot (the DX
// cluster's speaker, a call over the waterfall). Where it stands and how
// big it is is kept per browser.
'use strict';

const Js8Pal = (() => {
  const KEY = 'skjs8';
  const MIN_W = 420, MIN_H = 220;
  const KEEP_MS = 15 * 60 * 1000;                    // the skimmer keeps 15 min (js8.h)
  const AGAIN_MS = 90 * 1000;                        // each poll asks the last 90 s again:
                                                     // a slow period's frames come after a
                                                     // faster one's of a later start
  // JS8's submodes by its own number: letter, period, tone spacing
  const SUB = { 0: ['N', 15000, 6.25, 'normal'], 1: ['F', 10000, 10, 'fast'], 2: ['T', 6000, 20, 'turbo'],
                4: ['S', 30000, 3.125, 'slow'] };
  const mem = (() => { try { return JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { return {}; } })();
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(mem)); } catch (e) { /* private */ } };
  const wf = () => (typeof Wf !== 'undefined' ? Wf : null);
  const escH = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  let el = null, open = false, dial = null, frames = [], have = new Set(), pollT = 0, busy = false;
  let hl = '', stamped = new Set(), stuck = true, err = '', serverNow = 0;
  let dict = null, dictState = '', store = null;

  function dials() {
    const j = typeof status !== 'undefined' && status && status.js8;
    return j && j.on && j.decoder && Array.isArray(j.dials) ? j.dials : [];
  }
  const keyOf = (x) => `${x.t}|${x.sub}|${x.df}|${x.raw}`;
  const sub = (n) => SUB[n] || ['?', 15000, 6.25, '?'];

  // ---- where it stands (as the FT8 palette) --------------------------------------
  function clampBox() {
    const vw = innerWidth, vh = innerHeight;
    mem.w = Math.max(MIN_W, Math.min(vw - 8, Math.round(mem.w || 520)));
    mem.h = Math.max(MIN_H, Math.min(vh - 8, Math.round(mem.h || 360)));
    if (typeof mem.x !== 'number' || typeof mem.y !== 'number') {
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
      if (e.target.closest('button, select')) return;
      d = { dx: e.clientX - el.offsetLeft, dy: e.clientY - el.offsetTop };
      try { head.setPointerCapture(e.pointerId); } catch (err2) { /* not a live pointer */ }
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

  // ---- build ----------------------------------------------------------------------
  function build() {
    el = document.createElement('div');
    el.className = 'ft8-pal js8-pal';
    el.hidden = true;
    el.innerHTML = `
      <div class="fp-head">
        <span class="fp-title">JS8</span>
        <select class="fp-dial" title="the dial shown (every one is decoded all the time, in every submode)"></select>
        <span class="fp-state"></span>
        <button type="button" class="fp-close" title="close (the waterfall's view comes back)" aria-label="close">×</button>
      </div>
      <div class="fp-cols js8"><span>UTC</span><span title="submode: N normal 15 s, F fast 10 s, T turbo 6 s, S slow 30 s">sub</span><span>dB</span><span>Hz</span><span>message</span></div>
      <div class="fp-list" aria-live="polite"></div>`;
    document.querySelector('#tab-waterfall').appendChild(el);
    mountDrag(el.querySelector('.fp-head'));
    el.querySelector('.fp-close').addEventListener('click', () => setOpen(false));
    el.querySelector('.fp-dial').addEventListener('change', (e) => {
      const d = dials().find((x) => String(x.dial_hz) === e.target.value);
      if (d) setDial(d);
    });
    const lst = el.querySelector('.fp-list');
    lst.addEventListener('click', (e) => {
      const c = e.target.closest('[data-call]');
      if (!c) return;
      hl = hl === c.dataset.call ? '' : c.dataset.call;
      render();
    });
    lst.addEventListener('scroll', () => { stuck = lst.scrollTop + lst.clientHeight >= lst.scrollHeight - 4; });
    new ResizeObserver(() => {
      if (!open) return;
      mem.w = el.offsetWidth; mem.h = el.offsetHeight;
      save();
    }).observe(el);
    addEventListener('resize', () => { if (open) place(); });
  }

  // ---- the JSC dictionary (the compressed text), once --------------------------------
  function loadDict() {
    if (dictState || typeof Js8Protocol === 'undefined') return;
    dictState = 'loading';
    fetch('js8/js8-jsc.bin').then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return r.arrayBuffer();
    }).then((buf) => {
      dict = new Js8Protocol.JscDictionary(new Uint8Array(buf));
      dictState = 'ok';
      rebuild();
      render();
    }).catch((e) => { dictState = `the JSC dictionary: ${e.message || e}`; render(); });
  }
  // the messages anew from the frames held (a dial, the dictionary come)
  function rebuild() {
    if (typeof Js8Protocol === 'undefined') { store = null; return; }
    store = new Js8Protocol.ActivityStore(dict);
    for (const x of frames) store.push(toFrame(x));
  }
  const toFrame = (x) => ({ raw: x.raw, frameType: x.type, slotUtcMs: x.t, submode: x.sub, offsetHz: x.df,
                            snr: x.snr, dtMs: x.dt * 1000, quality: x.q });

  // ---- the dial -------------------------------------------------------------------
  function dialText(d) {
    return `${d.band} JS8 ${typeof fmtKhz === 'function' ? fmtKhz(d.dial_hz / 1000, 1) : (d.dial_hz / 1000).toFixed(1)}`;
  }
  function dialsUi() {
    const sel = el.querySelector('.fp-dial'), ds = dials();
    const html = ds.map((d) => `<option value="${d.dial_hz}">${escH(dialText(d))}</option>`).join('');
    if (sel.dataset.html !== html) { sel.dataset.html = html; sel.innerHTML = html; }
    if (dial) sel.value = String(dial.dial_hz);
  }
  function setDial(d) {
    dial = d;
    frames = []; have = new Set(); stamped = new Set(); hl = ''; stuck = true; serverNow = 0;
    rebuild();
    mem.dial = d.dial_hz; save();
    dialsUi();
    render();
    const w = wf();
    if (w) w.ft8Focus(d);
    poll();
  }

  // ---- the frames -----------------------------------------------------------------
  async function poll() {
    if (!open || !dial || busy) return;
    busy = true;
    try {
      const since = serverNow ? serverNow - AGAIN_MS : 0;
      const r = await fetch(`api/js8?dial=${dial.dial_hz}&since=${Math.max(0, since)}`, { cache: 'no-store' });
      const d = await r.json();
      if (!r.ok || d.off) throw new Error(d.off ? 'JS8 is off' : (d.error || `HTTP ${r.status}`));
      err = '';
      serverNow = d.now || Date.now();
      const fresh = (d.frames || []).filter((x) => x.dial === dial.dial_hz && !have.has(keyOf(x)))
        .sort((a, b) => a.t - b.t || a.sub - b.sub || a.df - b.df);
      for (const x of fresh) {
        have.add(keyOf(x));
        frames.push(x);
        if (store) store.push(toFrame(x));
      }
      const old = serverNow - KEEP_MS;
      if (frames.length && frames[0].t < old) {
        frames = frames.filter((x) => x.t >= old);
        have = new Set(frames.map(keyOf));
        rebuild();
      }
      if (store) store.expire(serverNow);
      render();
      const w = wf();
      if (w) w.ft8Stamp();
    } catch (e) {
      err = e.message || String(e);
      state();
    } finally {
      busy = false;
    }
  }
  function utc(ms) {
    const d = new Date(ms);
    const p = (n) => String(n).padStart(2, '0');
    return `${p(d.getUTCHours())}${p(d.getUTCMinutes())}${p(d.getUTCSeconds())}`;
  }
  // the calls the page has but the feed not yet: every frame of theirs deep
  // and unconfirmed
  function unconfirmed() {
    const ok = new Set(), not = new Set();
    for (const x of frames) {
      if (!x.call) continue;
      (x.ok ? ok : not).add(x.call);
    }
    return new Set([...not].filter((c) => !ok.has(c)));
  }
  function textHtml(text, from, unconf) {
    return escH(text).replace(/\b([A-Z0-9]{1,3}\/)?[A-Z0-9]{1,3}\d[A-Z]{1,4}(\/[A-Z0-9]{1,4})?\b/g, (c) =>
      `<b data-call="${c}" class="${c === hl ? 'hl' : ''}${c === from ? ' from' : ''}${unconf.has(c) ? ' q' : ''}">${c}${unconf.has(c) ? '?' : ''}</b>`);
  }
  function render() {
    if (!el) return;
    const lst = el.querySelector('.fp-list');
    if (!dial) { lst.innerHTML = '<p class="fp-empty">no JS8 dial decoded (Settings → JS8)</p>'; state(); return; }
    if (!store) { lst.innerHTML = '<p class="fp-empty">the JS8 protocol module is missing</p>'; state(); return; }
    const snap = store.snapshot(), unconf = unconfirmed();
    // the finished messages and those still coming in, oldest first
    const items = [...snap.messages.map((m) => ({ m, live: false })), ...snap.channels.map((m) => ({ m, live: true }))]
      .sort((a, b) => a.m.lastSlotUtcMs - b.m.lastSlotUtcMs || a.m.offsetHz - b.m.offsetHz);
    let html = '';
    for (const { m, live } of items) {
      const s = sub(m.submode);
      const from = (m.directed && m.directed.from) || m.compoundFrom || (m.callsigns || [])[0] || '';
      const cq = /@ALLCALL CQ|: @HB/.test(m.text);
      const q = from && unconf.has(from);
      const marks = hl && (m.callsigns || []).includes(hl);
      const cls = [cq ? 'cq' : '', marks ? 'hl' : '', q ? 'ap' : ''].filter(Boolean).join(' ');
      const tip = `${dial.band} JS8 ${s[3]}: ${((dial.dial_hz + m.offsetHz) / 1000).toFixed(2)} kHz · ${m.frameCount} frame${m.frameCount === 1 ? '' : 's'}` +
        `${live ? ' · still coming in' : m.complete ? '' : ' · its end frame missing'}${m.gaps && m.gaps.length ? ' · frames missing inside' : ''}` +
        `${m.checksumOk === false ? ' · its checksum fails' : ''}${q ? ' · a deep decode: on the page, not on the telnet feed until its call is confirmed' : ''}`;
      const text = (m.headerMissing ? '… ' : '') + (m.text || '(compressed text — the dictionary is loading)') +
        (live ? ' …' : m.complete ? '' : ' …');
      html += `<div class="fp-row js8${cls ? ` ${cls}` : ''}" title="${escH(tip)}">` +
        `<span>${utc(m.lastSlotUtcMs)}</span><span>${s[0]}</span><span>${m.snr != null ? m.snr : ''}</span>` +
        `<span>${Math.round(m.offsetHz)}</span><span class="m">${textHtml(text, from, unconf)}</span></div>`;
    }
    lst.innerHTML = html || '<p class="fp-empty">nothing decoded yet — a period takes 6 to 30 s by submode, its frames a few s more</p>';
    if (stuck) lst.scrollTop = lst.scrollHeight;
    state();
  }
  function state() {
    if (!el) return;
    const s = el.querySelector('.fp-state');
    const calls = new Set(frames.filter((x) => x.call).map((x) => x.call));
    s.textContent = err ? err : dial ? `${frames.length} frames · ${calls.size} stations in 15 min` +
      `${dictState && dictState !== 'ok' && dictState !== 'loading' ? ` · ${dictState}` : ''}${hl ? ` · ${hl}` : ''}` : '';
    s.classList.toggle('bad', !!err);
  }

  // ---- on the waterfall (waterfall.js calls these) --------------------------------
  // JS8's submodes overlap in time: no period lines.
  function row() {}
  // Each frame not yet in the picture that names its sender: the call
  // upright on its trace, in the middle of its period's rows.
  function stamp(g, rowTimes, xOf, w) {
    if (!open || !dial || !rowTimes.length) return;
    g.save();
    g.font = '700 11px RC, sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle';
    for (const x of frames) {
      const k = keyOf(x);
      if (stamped.has(k) || !x.call) continue;
      const s = sub(x.sub);
      const mid = (x.t + s[1] / 2) / 1000;
      let y = -1;
      for (let i = 0; i < rowTimes.length; i++) {
        if (!rowTimes[i]) continue;
        if (rowTimes[i] <= mid) { y = i; break; }
      }
      if (y < 0 || rowTimes[y] < mid - 2) continue;
      stamped.add(k);
      const fx = xOf(dial.dial_hz + x.df + 3.5 * s[2]);   // the middle of its 8 tones
      if (fx < -10 || fx > w + 10) continue;
      g.save();
      g.translate(Math.round(fx) + 0.5, y);
      g.rotate(-Math.PI / 2);
      g.lineWidth = 3; g.strokeStyle = 'rgba(0,0,12,.85)'; g.strokeText(x.call, 0, 0);
      g.fillStyle = !x.ok ? 'rgba(242,212,107,.55)' : /@HB|@ALLCALL/.test(x.text || '') ? '#7dffb0' : '#f2d46b';
      g.fillText(x.call, 0, 0);
      g.restore();
    }
    g.restore();
  }
  function restamp() { stamped = new Set(); }

  // ---- open / close ---------------------------------------------------------------
  function setOpen(v, want) {
    if (!!v === open && !want) return;
    const was = open;
    if (v && !was && typeof Ft8Pal !== 'undefined' && Ft8Pal.isOpen()) Ft8Pal.close();
    open = !!v;
    if (open && !el) build();
    el.hidden = !open;
    const w = wf();
    if (open) {
      place();
      loadDict();
      const ds = dials();
      const d = want || (dial && ds.find((x) => x.dial_hz === dial.dial_hz)) ||
                ds.find((x) => x.dial_hz === mem.dial) || ds[0] || null;
      if (!was && w) w.ft8Opened();
      if (d) setDial(d); else { dial = null; dialsUi(); render(); }
      clearInterval(pollT);
      pollT = setInterval(() => { if (el && !el.hidden) { dialsUi(); poll(); } }, 3000);
    } else {
      clearInterval(pollT);
      if (w) w.ft8Closed();
    }
    if (w) w.ft8Ui();
  }
  // a spot heard at hz: the dial whose window has it
  function openAt(hz) {
    const d = dials().find((x) => hz >= x.lo_hz - 50 && hz <= x.hi_hz + 50);
    if (!d) return false;
    setOpen(true, d);
    return true;
  }
  function toggleOn(rx) {
    if (open) { setOpen(false); return; }
    const ds = dials();
    setOpen(true, ds.find((x) => x.rx === rx) || ds[0] || null);
  }

  return {
    isOpen: () => open,
    available: () => dials().length > 0,
    dial: () => dial,
    close: () => setOpen(false),
    openAt, toggleOn, row, stamp, restamp,
  };
})();
