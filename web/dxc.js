// dxc.js — the status page's DX cluster: the feed's spots of the last half
// hour as a table, in the manner of IP-rotator's and wifilt's DXC page.
//
// Every column has its filter in its header (▾): the bands, the call (text or
// a regular expression), distance, bearing, dB, speed, mode, country, who
// made the spot. Columns can be switched off. dB is a meter coloured by the
// strength. The rows the filters leave are what the map draws too; a click on
// a row marks the station on the map, a click on the map marks its row.
// Filters and columns are kept in the browser.
'use strict';

const Dxc = (() => {
  const COLS = [
    ['time', 'UTC'], ['khz', 'kHz'], ['call', 'DX'], ['km', 'km'], ['az', '°'], ['db', 'dB'],
    ['speed', 'WPM'], ['mode', 'Mode'], ['band', 'Band'], ['country', 'Country'], ['src', 'From'],
  ];
  const DEF = { bands: {}, call: '', regex: false, kmMin: 0, kmMax: 20000, azMin: 0, azMax: 360,
                dbMin: -10, dbMax: 60, spMin: 0, spMax: 99, modes: { CW: true, RTTY: true }, country: '',
                src: { here: true, worker: true, peer: true }, age: 30, hidden: { src: true } };
  const load = () => { try { return JSON.parse(localStorage.getItem('skdxc') || '{}'); } catch (e) { return {}; } };
  const F = { ...JSON.parse(JSON.stringify(DEF)), ...load() };
  const save = () => { try { localStorage.setItem('skdxc', JSON.stringify(F)); } catch (e) { /* private window */ } };
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  let el = null, rows = [], bandsKnown = [], workers = new Set(), onChange = () => {}, marked = '', menu = null;

  // dB: the strength as a green from dark to bright (wifilt's ramp) and a meter
  const tone = (db) => `hsl(131,70%,${(22 + Math.max(0, Math.min(1, (db + 5) / 40)) * 54).toFixed(0)}%)`;
  const meter = (db) => '|'.repeat(Math.max(1, Math.min(8, Math.round((db + 5) / 5))));
  const ARR = ['↑', '↗', '→', '↘', '↓', '↙', '←', '↖'];
  const utc = (t) => { const d = new Date(t * 1000); return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`; };
  const srcKind = (s) => (!s.src ? 'here' : workers.has(s.src) ? 'worker' : 'peer');

  function callRe() {
    if (!F.call) return null;
    if (!F.regex) return F.call.toUpperCase();
    try { return new RegExp(F.call, 'i'); } catch (e) { return false; }
  }
  function pass(s) {
    const re = callRe();
    if (re && re !== false && (typeof re === 'string' ? !s.call.includes(re) : !re.test(s.call))) return false;
    if (s.band && F.bands[s.band] === false) return false;
    if (F.modes[s.mode] === false) return false;
    if (s.age > F.age * 60) return false;
    if (s.snr < F.dbMin || s.snr > F.dbMax) return false;
    if (s.speed && (s.speed < F.spMin || s.speed > F.spMax)) return false;
    if (s.km != null && (s.km < F.kmMin || s.km > F.kmMax)) return false;
    if (s.az != null && (F.azMin <= F.azMax ? (s.az < F.azMin || s.az > F.azMax) : (s.az < F.azMin && s.az > F.azMax))) return false;
    if (F.country && !(s.where && s.where.name.toUpperCase().includes(F.country.toUpperCase()))) return false;
    if (F.src[srcKind(s)] === false) return false;
    return true;
  }
  const active = {
    khz: () => Object.values(F.bands).some((v) => v === false),
    call: () => !!F.call, km: () => F.kmMin > 0 || F.kmMax < 20000, az: () => F.azMin > 0 || F.azMax < 360,
    db: () => F.dbMin > DEF.dbMin || F.dbMax < DEF.dbMax, speed: () => F.spMin > 0 || F.spMax < 99,
    mode: () => !F.modes.CW || !F.modes.RTTY, country: () => !!F.country, src: () => Object.values(F.src).some((v) => !v),
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
        return `<div class="fm-t">DX call</div><input data-f="call" value="${esc(F.call)}" placeholder="${F.regex ? '^(OK|OL)|/P$' : 'part of a call'}" class="${bad ? 'bad' : ''}">` +
          `<label><input type="checkbox" data-flag="regex"${F.regex ? ' checked' : ''}> regular expression</label>` +
          (bad ? '<div class="bad">not a valid expression — ignored</div>' : '');
      }
      case 'km': return `<div class="fm-t">Distance</div>${range('kmMin', 'kmMax', 0, 20000, 100, 'km')}`;
      case 'az': return `<div class="fm-t">Bearing (a sector; 300 to 60 crosses north)</div>${range('azMin', 'azMax', 0, 360, 5, '°')}`;
      case 'db': return `<div class="fm-t">Signal</div>${range('dbMin', 'dbMax', -10, 60, 1, 'dB')}`;
      case 'speed': return `<div class="fm-t">Speed (WPM; RTTY: baud)</div>${range('spMin', 'spMax', 0, 99, 1, '')}`;
      case 'mode': return `<div class="fm-t">Mode</div>${checks('modes', ['CW', 'RTTY'])}`;
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
    const keys = { khz: ['bands'], band: ['bands'], call: ['call', 'regex'], km: ['kmMin', 'kmMax'], az: ['azMin', 'azMax'],
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
      case 'khz': return `<td class="c-khz">${s.khz.toFixed(1)}</td>`;
      case 'call': return `<td class="c-call">${esc(s.call)}</td>`;
      case 'km': return `<td class="c-km">${s.km != null ? Math.round(s.km).toLocaleString('en-US') : ''}</td>`;
      case 'az': return `<td class="c-az" title="${s.az != null ? `${Math.round(s.az)}°` : ''}">${s.az != null ? `${ARR[Math.round(s.az / 45) % 8]} ${Math.round(s.az)}` : ''}</td>`;
      case 'db': return `<td class="c-db" style="color:${tone(s.snr)}"><span class="m">${meter(s.snr)}</span>${s.snr}</td>`;
      case 'speed': return `<td class="c-speed">${s.speed || ''}</td>`;
      case 'mode': return `<td class="c-mode">${s.mode}</td>`;
      case 'band': return `<td class="c-band" style="color:${SkMap.bandColor(s.band)}">${esc(s.band)}</td>`;
      case 'country': return `<td class="c-country">${s.where ? esc(s.where.name) : ''}</td>`;
      case 'src': return `<td class="c-src">${s.src ? esc(s.src) : '—'}</td>`;
      default: return '<td></td>';
    }
  }
  function draw() {
    if (!el) return;
    const vis = COLS.filter(([c]) => !F.hidden[c]);
    const shown = rows.filter(pass);
    el.querySelector('.dxc-count').textContent = `${shown.length} of ${rows.length}`;
    el.querySelector('.dxc-clear').disabled = !Object.values(active).some((f) => f());
    el.querySelector('thead').innerHTML = `<tr>${vis.map(([c, n]) =>
      `<th data-c="${c}" class="c-${c}"><span>${n}</span><button type="button" class="fbtn${active[c] && active[c]() ? ' on' : ''}" title="filter">▾</button></th>`).join('')}</tr>`;
    el.querySelector('tbody').innerHTML = shown.length ? shown.map((s) =>
      `<tr data-call="${esc(s.call)}" class="${s.age > 900 ? 'old' : ''}${s.call === marked ? ' hi' : ''}">${vis.map(([c]) => cell(c, s)).join('')}</tr>`).join('')
      : `<tr><td colspan="${vis.length}" class="mut">${rows.length ? 'the filters leave nothing' : 'no spots in the last half hour'}</td></tr>`;
    el.querySelector('.dxc-cols').innerHTML = COLS.map(([c, n]) =>
      `<label><input type="checkbox" data-col="${c}"${F.hidden[c] ? '' : ' checked'}> ${n === '°' ? 'bearing' : n}</label>`).join('');
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
    if (!menu) draw(); else onChange(rows.filter(pass), marked);   // keep an open menu's focus
  }

  return { mount, update };
})();
