// dxc.js — the status page's DX cluster: the feed's spots of the last half
// hour as a table, in the manner of IP-rotator's and wifilt's DXC page.
//
// Every column has its filter in its header (▾): the bands, the call (text or
// a regular expression), distance, bearing, dB, speed, mode, country, who
// made the spot. Columns can be switched off. dB is a meter coloured by the
// strength. The rows the filters leave are what the map draws too; a click on
// a row marks the station on the map, a click on the map marks its row.
// The speaker between kHz and DX opens the waterfall there, listening (a
// peer's spot has none: its band is not received here); an RTTY spot opens
// the RTTY palette over it too ([rtty_palette] open_on_spot).
// Filters and columns are kept in the browser. The Mode and age filters are
// the page's own: the band ladder's legend (its eyes) and the map's age
// buttons set the same ones (modeOn/setMode, setAge; 'sk-filter' on a change).
'use strict';

const Dxc = (() => {
  // wifilt's DX cluster's columns, in its order and alignment (UTC, kHz, DX,
  // km with the bearing's arrow, spotter, mode, dB, WPM — the eye reads the
  // same table in both), then this skimmer's own: band, country
  const COLS = [
    ['time', 'UTC'], ['khz', 'kHz'], ['play', ''], ['call', 'DX'], ['km', 'km'], ['src', 'From'], ['mode', 'Mode'],
    ['db', 'dB'], ['speed', 'WPM'], ['band', 'Band'], ['country', 'Country'],
  ];
  const DEF = { bands: {}, call: '', regex: false, kmMin: 0, kmMax: 20000, azMin: 0, azMax: 360,
                dbMin: -30, dbMax: 60, spMin: 0, spMax: 99, modes: { CW: true, RTTY: true, FT8: true, FT4: true, WSPR: true }, country: '',
                src: { here: true, worker: true, peer: true }, age: 30, hidden: { src: true } };
  const load = () => { try { return JSON.parse(localStorage.getItem('skdxc') || '{}'); } catch (e) { return {}; } };
  const F = { ...JSON.parse(JSON.stringify(DEF)), ...load() };
  if (!(F.age > 0 && F.age <= 30)) F.age = 30;            // the status page keeps half an hour
  // v3: the band ladder's eyes are this band filter — the bands they hid
  // from the map alone (skimmer-map-hidden, until v0.8.12) are out of it
  if (!(F.v >= 3)) {
    try {
      const h = JSON.parse(localStorage.getItem('skimmer-map-hidden') || '[]');
      if (Array.isArray(h)) for (const b of h) F.bands[b] = false;
      localStorage.removeItem('skimmer-map-hidden');
    } catch (e) { /* private window */ }
  }
  // v2: FT8/FT4 (WSJT-X's SNR goes down to −24 dB) — the old floor of −10 dB
  // was the default, not a choice: it moves down with it
  if (!(F.v >= 2)) {
    if (F.dbMin === -10) F.dbMin = -30;
    F.modes = { ...DEF.modes, ...(F.modes || {}) };
  }
  F.v = 3;
  // every change is the user's: the ladder and the map's age buttons follow
  const save = () => {
    try { localStorage.setItem('skdxc', JSON.stringify(F)); } catch (e) { /* private window */ }
    window.dispatchEvent(new Event('sk-filter'));
  };
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  let el = null, rows = [], bandsKnown = [], workers = new Set(), onChange = () => {}, marked = '', menu = null;

  // As wifilt's DX cluster (ok1hra/wifilt data/dxc.html, valueTone and
  // meterBars): distance and dB as a green from dark to bright, scaled from
  // the lowest to the highest value among the rows shown (so the farthest
  // and the loudest on screen are the brightest), and dB's meter of 6 bars
  // on the same scale; one value, or none apart, keeps the plain colour.
  const ratio = (v, r) => (r && r.hi > r.lo ? Math.max(0, Math.min(1, (v - r.lo) / (r.hi - r.lo))) : null);
  const tone = (v, r) => { const x = ratio(v, r); return x == null ? '' : ` style="color:hsl(131,70%,${(22 + x * 54).toFixed(0)}%)"`; };
  const meter = (v, r) => { const x = ratio(v, r); return '|'.repeat(x == null ? 6 : Math.max(1, Math.round(x * 6))); };
  const spanOf = (vals) => (vals.length ? { lo: Math.min(...vals), hi: Math.max(...vals) } : null);
  let kmR = null, dbR = null;                        // the rows shown, set by draw()
  const thousands = (v) => String(v).replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
  const utc = (t) => { const d = new Date(t * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
  const SPK = '<svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true"><path d="M3 7.5h3.2L10.5 4v12L6.2 12.5H3z" fill="currentColor"/>' +
    '<path d="M13.2 7.2a4 4 0 0 1 0 5.6M15.6 5a7.2 7.2 0 0 1 0 10" stroke="currentColor" stroke-width="1.6" fill="none" stroke-linecap="round"/></svg>';
  const srcKind = (s) => (!s.src ? 'here' : workers.has(s.src) ? 'worker' : 'peer');

  function callRe() {
    if (!F.call) return null;
    if (!F.regex) return F.call.toUpperCase();
    try { return new RegExp(F.call, 'i'); } catch (e) { return false; }
  }
  // the filters a spot passes; skip names the ones left to the caller (the
  // ladder shows a band or a mode left out its own way)
  function pass(s, skip) {
    const no = (k) => !(skip && skip.has(k));
    const re = no('call') && callRe();
    if (re && re !== false && (typeof re === 'string' ? !s.call.includes(re) : !re.test(s.call))) return false;
    if (no('band') && s.band && F.bands[s.band] === false) return false;
    if (no('mode') && F.modes[s.mode] === false) return false;
    if (no('age') && s.age > F.age * 60) return false;
    if (no('db') && (s.snr < F.dbMin || s.snr > F.dbMax)) return false;
    if (no('speed') && s.speed && (s.speed < F.spMin || s.speed > F.spMax)) return false;
    if (s.km != null && (s.km < F.kmMin || s.km > F.kmMax)) return false;
    if (s.az != null && (F.azMin <= F.azMax ? (s.az < F.azMin || s.az > F.azMax) : (s.az < F.azMin && s.az > F.azMax))) return false;
    if (F.country && !(s.where && s.where.name.toUpperCase().includes(F.country.toUpperCase()))) return false;
    if (no('src') && F.src[srcKind(s)] === false) return false;
    return true;
  }
  // a spot (or a WSPR station) from elsewhere on the page, with its place
  const placeCache = new Map();
  function placed(s) {
    if (s.where !== undefined) return s;
    let p = placeCache.get(s.call);
    if (!p) {
      const where = SkMap.locate(s.call), g = where ? SkMap.geo(SkMap.center(), where.lat, where.lon) : null;
      p = { where, km: g ? g.km : null, az: g ? g.az : null };
      if (placeCache.size > 5000) placeCache.clear();
      placeCache.set(s.call, p);
    }
    return { ...s, ...p };
  }
  const active = {
    khz: () => Object.values(F.bands).some((v) => v === false),
    call: () => !!F.call, km: () => F.kmMin > 0 || F.kmMax < 20000 || F.azMin > 0 || F.azMax < 360,
    db: () => F.dbMin > DEF.dbMin || F.dbMax < DEF.dbMax, speed: () => F.spMin > 0 || F.spMax < 99,
    mode: () => Object.values(F.modes).some((v) => v === false), country: () => !!F.country, src: () => Object.values(F.src).some((v) => !v),
    band: () => Object.values(F.bands).some((v) => v === false), time: () => F.age !== 30,
  };

  // ---- the filter menus ---------------------------------------------------------------
  function menuHtml(c) {
    const range = (a, b, lo, hi, step, unit) => `<div class="fm-range"><input type="number" data-f="${a}" value="${F[a]}" min="${lo}" max="${hi}" step="${step}">` +
      `<span>to</span><input type="number" data-f="${b}" value="${F[b]}" min="${lo}" max="${hi}" step="${step}"><span>${unit}</span></div>`;
    const checks = (obj, keys, label = (k) => k) => keys.map((k) =>
      `<label><input type="checkbox" data-obj="${obj}" data-key="${esc(k)}"${F[obj][k] !== false ? ' checked' : ''}> ${esc(label(k))}</label>`).join('');
    switch (c) {
      case 'khz': case 'band':
        return `<div class="fm-t">Bands</div><div class="fm-grid">${checks('bands', bandsKnown)}</div>` +
          '<div class="fm-act"><button type="button" data-all="bands">all</button><button type="button" data-none="bands">none</button></div>';
      case 'call': {
        const bad = callRe() === false;
        return `<div class="fm-t">DX filter</div><input data-f="call" value="${esc(F.call)}" placeholder="Callsign, text, or regex" spellcheck="false" autocomplete="off" class="${bad ? 'bad' : ''}">` +
          `<label><input type="checkbox" data-flag="regex"${F.regex ? ' checked' : ''}> Regex</label>` +
          (bad ? '<div class="bad">not a valid expression — ignored</div>' : '') +
          '<div class="fm-note">Without Regex, this matches plain text. With Regex enabled, it uses a JavaScript ' +
          'regular expression and ignores letter case.</div>';
      }
      case 'km': return `<div class="fm-t">Distance</div>${range('kmMin', 'kmMax', 0, 20000, 100, 'km')}` +
        `<div class="fm-t">Bearing (a sector; 300 to 60 crosses north)</div>${range('azMin', 'azMax', 0, 360, 5, '°')}`;
      case 'db': return `<div class="fm-t">Signal (FT8/FT4: WSJT-X's SNR)</div>${range('dbMin', 'dbMax', -30, 60, 1, 'dB')}`;
      case 'speed': return `<div class="fm-t">Speed (WPM; RTTY: baud)</div>${range('spMin', 'spMax', 0, 99, 1, '')}`;
      case 'mode': return `<div class="fm-t">Mode</div>${checks('modes', ['CW', 'RTTY', 'FT8', 'FT4', 'WSPR'])}` +
        '<div class="fm-note">The same switches as the eyes in the Bands legend: a mode left out is out of the ' +
        'bands\' counts, the heat map, the map and this table. WSPR has no rows here.</div>';
      case 'country': return `<div class="fm-t">Country contains</div><input data-f="country" value="${esc(F.country)}" placeholder="Japan, Russia…">`;
      case 'src': return `<div class="fm-t">Made by</div>${checks('src', ['here', 'worker', 'peer'], (k) => ({ here: 'this skimmer', worker: 'a worker', peer: 'a peer' }[k]))}`;
      case 'time': return `<div class="fm-t">Age</div><div class="fm-act">${[5, 10, 15, 30].map((a) => `<button type="button" data-age="${a}" class="${F.age === a ? 'on' : ''}">${a} min</button>`).join('')}</div>`;
      default: return '';
    }
  }
  function openMenu(c, btn) {
    closeMenu();
    menu = document.createElement('div');
    menu.className = 'fmenu';
    menu.dataset.c = c;
    menu.innerHTML = `${menuHtml(c)}<div class="fm-act"><button type="button" data-reset="${c}">reset</button><button type="button" data-close>close</button></div>`;
    document.body.appendChild(menu);
    const r = btn.getBoundingClientRect();
    menu.style.left = `${Math.max(8, Math.min(innerWidth - menu.offsetWidth - 8, r.left + scrollX))}px`;
    menu.style.top = `${r.bottom + scrollY + 4}px`;
    const first = menu.querySelector('input:not([type=checkbox])');
    if (first) first.focus();
  }
  function closeMenu() { if (menu) { menu.remove(); menu = null; } }
  function resetCol(c) {
    const keys = { khz: ['bands'], band: ['bands'], call: ['call', 'regex'], km: ['kmMin', 'kmMax', 'azMin', 'azMax'],
                   db: ['dbMin', 'dbMax'], speed: ['spMin', 'spMax'], mode: ['modes'], country: ['country'], src: ['src'], time: ['age'] }[c] || [];
    for (const k of keys) F[k] = JSON.parse(JSON.stringify(DEF[k]));
  }
  function menuEvent(e) {
    if (!menu || !menu.contains(e.target)) return;
    const t = e.target, c = menu.dataset.c;
    if (t.dataset.close !== undefined) { closeMenu(); return; }
    if (t.dataset.reset) resetCol(t.dataset.reset);
    if (t.dataset.all) bandsKnown.forEach((b) => { F.bands[b] = true; });
    if (t.dataset.none) bandsKnown.forEach((b) => { F.bands[b] = false; });
    if (t.dataset.age) F.age = Number(t.dataset.age);
    if (t.dataset.obj) F[t.dataset.obj][t.dataset.key] = t.checked;
    if (t.dataset.flag) F[t.dataset.flag] = t.checked;
    if (t.dataset.f) F[t.dataset.f] = t.type === 'number' ? Number(t.value) : t.value.trim();
    save();
    if (e.type === 'click' && (t.dataset.reset || t.dataset.all || t.dataset.none || t.dataset.age)) {
      const btn = el.querySelector(`th[data-c="${c}"] .fbtn`);
      openMenu(c, btn);
    }
    draw();
    if (t.dataset.flag || (t.dataset.f === 'call')) {     // the regex state shows at once
      const inp = menu.querySelector('[data-f=call]');
      if (inp) inp.classList.toggle('bad', callRe() === false);
    }
  }

  // ---- the table ------------------------------------------------------------------------
  function cell(c, s) {
    switch (c) {
      case 'time': return `<td class="c-time">${utc(s.t)}</td>`;
      case 'khz': return `<td class="c-khz">${fmtKhz(s.khz)}</td>`;
      case 'play': return `<td class="c-play">${s.band && srcKind(s) !== 'peer'
        ? `<button type="button" class="dxc-play" data-band="${esc(s.band)}" data-khz="${s.khz}" data-mode="${esc(s.mode)}" title="${s.mode === 'FT8' || s.mode === 'FT4' ? `the FT8 palette: ${esc(s.mode)} at ${fmtKhz(s.khz)} kHz, its dial on the waterfall` : `listen on the waterfall: ${fmtKhz(s.khz)} kHz${s.mode === 'RTTY' ? ' — the RTTY palette decodes it' : ''}`}" aria-label="listen">${SPK}</button>` : ''}</td>`;
      case 'call': return `<td class="c-call">${esc(s.call)}</td>`;
      // as wifilt: the distance in thousands, the bearing as an arrow turned to it
      case 'km': return s.km != null ? `<td class="c-km"${tone(s.km, kmR)} title="${Math.round(s.az)}°"><span class="km">${thousands(Math.round(s.km))}</span>` +
        `<span class="arr" style="transform:rotate(${Math.round(s.az)}deg)">↑</span></td>` : '<td class="c-km"></td>';
      case 'db': return `<td class="c-db"${tone(s.snr, dbR)}><span class="m">${meter(s.snr, dbR)}</span><span class="v">${s.snr}</span></td>`;
      case 'speed': return `<td class="c-speed">${s.speed || ''}</td>`;
      case 'mode': return `<td class="c-mode">${esc(s.mode)}</td>`;
      case 'band': return `<td class="c-band">${esc(s.band)}</td>`;
      case 'country': return `<td class="c-country">${s.where ? esc(s.where.name) : ''}</td>`;
      case 'src': return `<td class="c-src">${s.src ? esc(s.src) : '—'}</td>`;
      default: return '<td></td>';
    }
  }
  function draw() {
    if (!el) return;
    const vis = COLS.filter(([c]) => !F.hidden[c]);
    const shown = rows.filter((s) => pass(s));
    kmR = spanOf(shown.filter((s) => s.km != null).map((s) => s.km));
    dbR = spanOf(shown.filter((s) => Number.isFinite(s.snr)).map((s) => s.snr));
    el.querySelector('.dxc-count').textContent = `${shown.length} of ${rows.length}`;
    el.querySelector('.dxc-clear').disabled = !Object.values(active).some((f) => f());
    el.querySelector('thead').innerHTML = `<tr>${vis.map(([c, n]) => c === 'play' ? '<th class="c-play"></th>' :
      `<th data-c="${c}" class="c-${c}"><span>${n}</span><button type="button" class="fbtn${active[c] && active[c]() ? ' on' : ''}" title="filter">▾</button></th>`).join('')}</tr>`;
    el.querySelector('tbody').innerHTML = shown.length ? shown.map((s) =>
      `<tr data-call="${esc(s.call)}" class="${s.age > 900 ? 'old' : ''}${s.call === marked ? ' hi' : ''}">${vis.map(([c]) => cell(c, s)).join('')}</tr>`).join('')
      : `<tr><td colspan="${vis.length}" class="mut">${rows.length ? 'the filters leave nothing' : 'no spots in the last half hour'}</td></tr>`;
    el.querySelector('.dxc-cols').innerHTML = COLS.map(([c, n]) =>
      `<label><input type="checkbox" data-col="${c}"${F.hidden[c] ? '' : ' checked'}> ${c === 'play' ? 'listen' : c === 'km' ? 'km, bearing' : n}</label>`).join('');
    onChange(shown, marked);
  }

  function mount(container, cb) {
    el = container;
    onChange = cb || onChange;
    el.innerHTML = `
      <div class="dxc-bar"><h2>DX cluster <span class="mut">this skimmer's spots · ▾ in a column filters it</span></h2>
        <span class="dxc-count mut"></span>
        <button type="button" class="small dxc-clear">clear filters</button>
        <details class="dxc-colmenu"><summary class="small">columns</summary><div class="dxc-cols"></div></details></div>
      <div class="wrap"><table class="dxc"><thead></thead><tbody></tbody></table></div>`;
    el.querySelector('thead').addEventListener('click', (e) => {
      const b = e.target.closest('.fbtn');
      if (!b) return;
      e.stopPropagation();
      const c = b.closest('th').dataset.c;
      if (menu && menu.dataset.c === c) closeMenu(); else openMenu(c, b);
    });
    el.querySelector('tbody').addEventListener('click', (e) => {
      const play = e.target.closest('.dxc-play');
      if (play) {                                      // to the waterfall (app.js), still in the click
        window.dispatchEvent(new CustomEvent('sk-listen', { detail: { band: play.dataset.band, hz: Number(play.dataset.khz) * 1000, mode: play.dataset.mode } }));
        return;
      }
      const tr = e.target.closest('tr[data-call]');
      if (!tr) return;
      marked = marked === tr.dataset.call ? '' : tr.dataset.call;
      draw();
    });
    el.querySelector('.dxc-clear').addEventListener('click', () => {
      const hidden = F.hidden;
      Object.assign(F, JSON.parse(JSON.stringify(DEF)), { hidden });
      save(); closeMenu(); draw();
    });
    el.querySelector('.dxc-cols').addEventListener('change', (e) => {
      const c = e.target.dataset.col;
      if (!c) return;
      F.hidden[c] = !e.target.checked;
      save(); draw();
    });
    document.addEventListener('click', (e) => {
      if (menu && !menu.contains(e.target) && !e.target.closest('.fbtn')) closeMenu();
    });
    document.addEventListener('click', menuEvent);
    document.addEventListener('change', menuEvent);
    document.addEventListener('input', (e) => { if (e.target.dataset && e.target.dataset.f && e.target.type !== 'number') menuEvent(e); });
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMenu(); });
    window.addEventListener('skmap-spot', (e) => {
      marked = e.detail;
      draw();
      const tr = el.querySelector(`tr[data-call="${CSS.escape(marked)}"]`);
      if (tr) tr.scrollIntoView({ block: 'nearest' });
    });
  }

  // spots: from /api/spots, with age; bands: the skimmer's; workerNames: its workers
  function update(spots, bands, workerNames) {
    bandsKnown = bands;
    workers = new Set(workerNames || []);
    const c = SkMap.center();
    rows = spots.map((s) => {
      const where = SkMap.locate(s.call), g = where ? SkMap.geo(c, where.lat, where.lon) : null;
      return { ...s, where, km: g ? g.km : null, az: g ? g.az : null };
    });
    if (!menu) draw(); else onChange(rows.filter((s) => pass(s)), marked);   // keep an open menu's focus
  }

  // the page's other inputs for the same filters
  function setMode(m, on) {
    F.modes[m] = !!on;
    if (menu && menu.dataset.c === 'mode') closeMenu();
    save();
    if (el) draw();
  }
  function setBand(b, on) {
    F.bands[b] = !!on;
    if (menu && (menu.dataset.c === 'khz' || menu.dataset.c === 'band')) closeMenu();
    save();
    if (el) draw();
  }
  function setAge(a) {
    if (![5, 10, 15, 30].includes(a)) return;
    F.age = a;
    if (menu && menu.dataset.c === 'time') closeMenu();
    save();
    if (el) draw();
  }
  // the spots other parts of the page count (the band ladder, its heat map)
  // pass the same filters — skip: the ones they show their own way
  const passes = (s, skip) => pass(placed(s), skip);
  return { mount, update, age: () => F.age, modeOn: (m) => F.modes[m] !== false, setMode, setAge,
           bandOn: (b) => F.bands[b] !== false, setBand, passes };
})();
