// ft8-palette.js — the FT8 palette over the waterfall: what the skimmer
// decodes on an FT8 or FT4 dial ([ft8], src/headless/ft8.c — WSJT-X's own
// decoders, every cycle, the whole window), shown as WSJT-X's Band Activity:
// each cycle's messages with UTC, dB, DT and the audio frequency. It decodes
// nothing itself: the waterfall's audio is a 500 Hz slot without time, and
// the skimmer decodes the dial's whole 3 kHz anyway.
//
// Opening it narrows the waterfall to the dial's window (dial + 200 … 3500
// Hz) on its band, marks each cycle's start with a line and stands each
// message's sender upright on its trace, in the cycle it was sent; closing
// it brings the waterfall's view back. Opened by FT8 in the waterfall's bar,
// a click on an FT8/FT4 spot (the DX cluster's speaker, a call over the
// waterfall). Where it stands and how big it is is kept per browser.
'use strict';

const Ft8Pal = (() => {
  const KEY = 'skft8';
  const MIN_W = 380, MIN_H = 220;
  const KEEP_MS = 15 * 60 * 1000;                    // the skimmer keeps 15 min (ft8.h)
  const mem = (() => { try { return JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { return {}; } })();
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(mem)); } catch (e) { /* private */ } };
  const wf = () => (typeof Wf !== 'undefined' ? Wf : null);
  const escH = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  let el = null, open = false, dial = null, list = [], since = 0, pollT = 0, busy = false;
  let hl = '', stamped = new Set(), stuck = true, err = '';

  // the dials the skimmer decodes (status.json "ft8", app.js keeps it)
  function dials() {
    const f = typeof status !== 'undefined' && status && status.ft8;
    return f && f.on && f.decoder && Array.isArray(f.dials) ? f.dials : [];
  }
  const period = (d) => (d && d.mode === 'FT4' ? 7500 : 15000);
  const keyOf = (x) => `${x.t}|${x.df}|${x.msg}`;
  // a signal's middle above its tone 0: 8 tones of 6.25 Hz (FT8), 4 of 20.8 (FT4)
  const half = (d) => (d && d.mode === 'FT4' ? 31 : 22);

  // ---- where it stands (as the RTTY palette) --------------------------------------
  function clampBox() {
    const vw = innerWidth, vh = innerHeight;
    mem.w = Math.max(MIN_W, Math.min(vw - 8, Math.round(mem.w || 440)));
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
    el.className = 'ft8-pal';
    el.hidden = true;
    el.innerHTML = `
      <div class="fp-head">
        <span class="fp-title">FT8</span>
        <select class="fp-dial" title="the dial shown (every one is decoded all the time)"></select>
        <span class="fp-state"></span>
        <button type="button" class="fp-close" title="close (the waterfall's view comes back)" aria-label="close">×</button>
      </div>
      <div class="fp-cols"><span>UTC</span><span>dB</span><span>DT</span><span>Hz</span><span>message</span></div>
      <div class="fp-list" aria-live="polite"></div>`;
    document.querySelector('#tab-waterfall').appendChild(el);
    mountDrag(el.querySelector('.fp-head'));
    el.querySelector('.fp-close').addEventListener('click', () => setOpen(false));
    el.querySelector('.fp-dial').addEventListener('change', (e) => {
      const d = dials().find((x) => String(x.dial_hz) === e.target.value);
      if (d) setDial(d);
    });
    const lst = el.querySelector('.fp-list');
    // a call in a message: its rows marked (again: unmarked)
    lst.addEventListener('click', (e) => {
      const c = e.target.closest('[data-call]');
      if (!c) return;
      hl = hl === c.dataset.call ? '' : c.dataset.call;
      render();
    });
    // scrolled up: it stays there; at the bottom: it follows the newest
    lst.addEventListener('scroll', () => { stuck = lst.scrollTop + lst.clientHeight >= lst.scrollHeight - 4; });
    new ResizeObserver(() => {
      if (!open) return;
      mem.w = el.offsetWidth; mem.h = el.offsetHeight;
      save();
    }).observe(el);
    addEventListener('resize', () => { if (open) place(); });
  }

  // ---- the dial -------------------------------------------------------------------
  function dialText(d) {
    return `${d.band} ${d.mode} ${typeof fmtKhz === 'function' ? fmtKhz(d.dial_hz / 1000, 1) : (d.dial_hz / 1000).toFixed(1)}`;
  }
  function dialsUi() {
    const sel = el.querySelector('.fp-dial'), ds = dials();
    const html = ds.map((d) => `<option value="${d.dial_hz}">${escH(dialText(d))}</option>`).join('');
    if (sel.dataset.html !== html) { sel.dataset.html = html; sel.innerHTML = html; }
    if (dial) sel.value = String(dial.dial_hz);
    el.querySelector('.fp-title').textContent = dial ? dial.mode : 'FT8';
  }
  function setDial(d) {
    dial = d;
    list = []; since = 0; stamped = new Set(); hl = ''; stuck = true;
    mem.dial = d.dial_hz; save();
    dialsUi();
    render();
    const w = wf();
    if (w) w.ft8Focus(d);
    poll();
  }

  // ---- the decodes ----------------------------------------------------------------
  async function poll() {
    if (!open || !dial || busy) return;
    busy = true;
    try {
      const r = await fetch(`api/ft8?dial=${dial.dial_hz}&since=${since}`, { cache: 'no-store' });
      const d = await r.json();
      if (!r.ok || d.off) throw new Error(d.off ? 'FT8 is off' : (d.error || `HTTP ${r.status}`));
      err = '';
      const have = new Set(list.map(keyOf));
      for (const x of d.decodes || []) {
        if (x.dial !== dial.dial_hz || have.has(keyOf(x))) continue;
        list.push(x);
        since = Math.max(since, x.t);              // a cycle comes whole: the next ask from it on
      }
      const old = (d.now || Date.now()) - KEEP_MS;
      list = list.filter((x) => x.t >= old).sort((a, b) => a.t - b.t || a.df - b.df);
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
  // a message's calls clickable (each token that looks like a call)
  function msgHtml(x) {
    return x.msg.split(' ').map((w) => {
      const plain = w.replace(/^<|>$/g, '');
      const isCall = /^[A-Z0-9/]{3,}$/.test(plain) && /\d/.test(plain) && /[A-Z]/.test(plain) && !/^R?[+-]\d+$/.test(plain) &&
        !/^[A-R]{2}\d{2}$/.test(plain) && plain !== 'RR73';
      return isCall ? `<b data-call="${escH(plain)}" class="${plain === hl ? 'hl' : ''}${plain === x.call ? ' from' : ''}">${escH(w)}</b>` : escH(w);
    }).join(' ');
  }
  function render() {
    if (!el) return;
    const lst = el.querySelector('.fp-list');
    if (!dial) { lst.innerHTML = '<p class="fp-empty">no FT8/FT4 dial decoded (Settings → FT8 / FT4)</p>'; state(); return; }
    let html = '', cyc = -1;
    for (const x of list) {
      if (x.t !== cyc) {
        cyc = x.t;
        const n = list.filter((y) => y.t === cyc).length;
        html += `<div class="fp-cyc">${utc(cyc).replace(/(\d\d)(\d\d)(\d\d)/, '$1:$2:$3')} · ${n}</div>`;
      }
      const cls = [x.cq ? 'cq' : '', hl && x.msg.split(' ').some((w) => w.replace(/^<|>$/g, '') === hl) ? 'hl' : '', x.ap ? 'ap' : '']
        .filter(Boolean).join(' ');
      html += `<div class="fp-row${cls ? ` ${cls}` : ''}" title="${escH(`${dial.band} ${dial.mode}: ${((dial.dial_hz + x.df) / 1000).toFixed(2)} kHz${x.ap ? ' · an a-priori decode (not spotted)' : ''}`)}">` +
        `<span>${utc(x.t)}</span><span>${x.snr}</span><span>${Number(x.dt).toFixed(1)}</span><span>${x.df}</span>` +
        `<span class="m">${msgHtml(x)}</span></div>`;
    }
    lst.innerHTML = html || '<p class="fp-empty">nothing decoded yet — a cycle takes 15 s (FT4 7.5 s), its decodes a few s more</p>';
    if (stuck) lst.scrollTop = lst.scrollHeight;
    state();
  }
  function state() {
    if (!el) return;
    const s = el.querySelector('.fp-state');
    const last = list.length ? list[list.length - 1].t : 0;
    const n = last ? list.filter((x) => x.t === last).length : 0;
    s.textContent = err ? err : dial ? `${list.length} in 15 min${last ? ` · ${n} at ${utc(last).slice(0, 4)}` : ''}${hl ? ` · ${hl}` : ''}` : '';
    s.classList.toggle('bad', !!err);
  }

  // ---- on the waterfall (waterfall.js calls these) --------------------------------
  // A row drawn at y = 0 at time t (s; prev: the row's before it): the
  // cycle's start is a faint line.
  function row(g, w, t, prev) {
    if (!open || !dial || !t || !prev) return;
    const p = period(dial) / 1000;
    if (Math.floor(t / p) === Math.floor(prev / p)) return;
    g.fillStyle = 'rgba(197,155,255,.45)';
    for (let x = 0; x < w; x += 6) g.fillRect(x, 0, 3, 1);
  }
  // Each decode not yet in the picture: its sender (else its first word)
  // upright on its trace, in the middle of its cycle's rows.
  function stamp(g, rowTimes, xOf, w) {
    if (!open || !dial || !rowTimes.length) return;
    g.save();
    g.font = '700 11px RC, sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle';
    for (const x of list) {
      const k = keyOf(x);
      if (stamped.has(k)) continue;
      const mid = (x.t + period(dial) / 2) / 1000;
      let y = -1;
      for (let i = 0; i < rowTimes.length; i++) {
        if (!rowTimes[i]) continue;                    // a gap
        if (rowTimes[i] <= mid) { y = i; break; }
      }
      if (y < 0 || rowTimes[y] < mid - 2) continue;   // not in the picture (yet, or any more)
      stamped.add(k);
      const fx = xOf(dial.dial_hz + x.df + half(dial));
      if (fx < -10 || fx > w + 10) continue;
      // the sender; else the first word that looks like a call (a CQ of a
      // call the skimmer does not take, a hash), else the first word
      const words = x.msg.split(' ');
      const label = x.call || words.find((w) => /\d/.test(w) && /[A-Z]/.test(w) && !/^R?[+-]\d+$/.test(w) &&
                                                !/^[A-R]{2}\d{2}$/.test(w)) || words[0] || '';
      g.save();
      g.translate(Math.round(fx) + 0.5, y);
      g.rotate(-Math.PI / 2);
      g.lineWidth = 3; g.strokeStyle = 'rgba(0,0,12,.85)'; g.strokeText(label, 0, 0);
      g.fillStyle = x.cq ? '#7dffb0' : '#e4d4ff';
      g.fillText(label, 0, 0);
      g.restore();
    }
    g.restore();
  }
  // the picture drawn anew (another window, the history): every label again
  function restamp() { stamped = new Set(); }

  // ---- open / close ---------------------------------------------------------------
  function setOpen(v, want) {
    if (!!v === open && !want) return;
    const was = open;
    // one digital palette at a time: the JS8 one closes (its view comes back first)
    if (v && !was && typeof Js8Pal !== 'undefined' && Js8Pal.isOpen()) Js8Pal.close();
    open = !!v;
    if (open && !el) build();
    el.hidden = !open;
    const w = wf();
    if (open) {
      place();
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
  // a spot heard at hz in mode (FT8/FT4): the dial whose window has it
  function openAt(hz, mode) {
    const ds = dials();
    const d = ds.find((x) => x.mode === mode && hz >= x.lo_hz - 50 && hz <= x.hi_hz + 50) ||
              ds.find((x) => hz >= x.lo_hz - 50 && hz <= x.hi_hz + 50);
    if (!d) return false;
    setOpen(true, d);
    return true;
  }
  // the bar's button: the band shown's first dial, else any
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
