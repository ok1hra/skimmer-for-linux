// skimmer-headless web page — status, statistics, settings, log.
// Plain JS, no libraries: the page is served by the skimmer itself and must
// work on a LAN with no internet. Everything it shows comes from JSON
// (/status.json, /stats.json, /api/log, /api/config).
'use strict';

const $ = (sel) => document.querySelector(sel);
// a listener on an element that may not be on the page (Settings and Log are
// served only to the addresses that may change settings)
const on = (sel, ev, fn, o) => { const e = document.querySelector(sel); if (e) e.addEventListener(ev, fn, o); };
const esc = (s) => String(s).replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fmt = (v, d = 0) => (v === undefined || v === null || Number.isNaN(v)) ? '–'
  : Number(v).toLocaleString('en-US', { minimumFractionDigits: d, maximumFractionDigits: d });

// A frequency in kHz the way the shack reads it: a space after the MHz
// ("14 025.3"), everywhere on the page (map.js and dxc.js use it too).
// sep: a no-break space for display, a plain one for an input to edit.
function fmtKhz(khz, d = 1, sep = '\u00a0') {
  if (khz === undefined || khz === null || !Number.isFinite(Number(khz))) return '–';
  const [i, f] = (d === null ? String(Number(khz)) : Number(khz).toFixed(d)).split('.');
  return `${i.replace(/\B(?=(\d{3})+(?!\d))/g, sep)}${f !== undefined ? `.${f}` : ''}`;
}

async function getJSON(url) {
  const r = await fetch(url, { cache: 'no-store' });
  if (r.status === 401 && url !== 'api/visitor') {           // a visitor from outside: sign in first
    visitor.need = true;
    visitor.call = null;
    showVisitor();
    throw new Error('sign in first');
  }
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

// ---- visitors from outside the LAN sign in with their call (visitors.h) ----------
// need: this browser must sign in before it gets any data; call: as whom it
// is signed in (null for a client on the LAN, who needs none)
const visitor = { need: false, call: null };
async function visitorCheck() {
  try {
    const v = await getJSON('api/visitor');
    visitor.need = !!v.need;
    visitor.call = v.call || null;
  } catch (e) { /* an old skimmer: no sign-in */ }
  showVisitor();
}
function showVisitor() {
  const box = $('#signin'), was = box.hidden;
  box.hidden = !visitor.need;
  if (visitor.need && was) setTimeout(() => $('#signin-call').focus(), 0);
  const el = $('#visitor');
  el.hidden = !visitor.call;
  el.innerHTML = visitor.call ? `you: <b>${esc(visitor.call)}</b><button type="button" id="visitor-out" ` +
    'title="sign out, or in with another call">change</button>' : '';
}
async function visitorPost(fields) {
  const r = await fetch('api/visitor', { method: 'POST', body: new URLSearchParams(fields),
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' } });
  let j = {};
  try { j = await r.json(); } catch (e) { /* none */ }
  return { ok: r.ok, ...j };
}
on('#signin-form', 'submit', async (e) => {
  e.preventDefault();
  const err = $('#signin-err');
  // the skimmer checks the call (and logs a refusal) — the page only shows why
  const r = await visitorPost({ call: $('#signin-call').value });
  if (!r.ok) {
    err.hidden = false;
    err.textContent = r.error === 'no callsign entered' || /callsign has|not a callsign/.test(r.error || '')
      ? `That is not a callsign — ${r.error}.` : (r.error || 'not signed in');
    $('#signin-call').select();
    return;
  }
  err.hidden = true;
  visitor.need = !!r.need;
  visitor.call = r.call || null;
  showVisitor();
  poll();
  if (tab !== 'status' && tabs[tab] && tabs[tab].enter) tabs[tab].enter();
});
on('#visitor', 'click', async (e) => {
  if (e.target.id !== 'visitor-out') return;
  const r = await visitorPost({ logout: '1' });
  visitor.call = null;
  visitor.need = !!r.need;
  $('#signin-call').value = '';
  showVisitor();
});

// ---- tabs ------------------------------------------------------------------------
const tabs = {};
let tab = 'status';
function show(name) {
  if (tab !== name && tabs[tab] && tabs[tab].leave) tabs[tab].leave();
  tab = name;
  for (const b of document.querySelectorAll('nav button')) b.classList.toggle('on', b.dataset.tab === name);
  for (const s of document.querySelectorAll('main > section')) s.hidden = s.id !== `tab-${name}`;
  try { localStorage.setItem('skimmer-tab', name); } catch (e) { /* private window */ }
  if (tabs[name] && tabs[name].enter) tabs[name].enter();
}
for (const b of document.querySelectorAll('nav button')) b.addEventListener('click', () => show(b.dataset.tab));

// ---- status ----------------------------------------------------------------------
function age(s) {
  return s < 60 ? `${s} s` : s < 3600 ? `${Math.floor(s / 60)} min` : `${Math.floor(s / 3600)} h`;
}
function uptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d} d ${h} h` : `${h}:${String(m).padStart(2, '0')}`;
}
function tile(k, v, unit = '') {
  return `<div class="tile"><div class="k">${esc(k)}</div><div class="v">${v}${unit ? ` <small>${esc(unit)}</small>` : ''}</div></div>`;
}

// The bands as decoded, low band first (Settings may list them in any
// order): a band a worker decodes ("worker") is shown with that worker's own
// numbers (its status comes with the hub's), or as waiting.
function bandsDecoded(d) {
  const workers = (d.hub && d.hub.workers) || [];
  return [...d.bands].sort((a, b) => a.centre_hz - b.centre_hz).map((b) => {
    if (!b.worker) return b;
    const w = workers.find((x) => x.name === b.worker);
    const wb = w && w.status && (w.status.bands || []).find((x) => x.band === b.band);
    if (wb) return { ...wb, centre_hz: b.centre_hz, ksps: b.ksps, worker: b.worker };
    const empty = { decoders: 0, active: 0, stations: 0, drops: 0, recent: [], engine: '' };
    return { ...empty, band: b.band, centre_hz: b.centre_hz, ksps: b.ksps, worker: b.worker,
             waiting: true, cw: b.worker_cw, spots: b.remote_spots,
             rtty: b.worker_rtty ? { ...empty, spots: b.remote_spots_rtty } : null };
  });
}

// The bands the map leaves out (the eye in the ladder), per browser.
const mapHidden = new Set((() => {
  try { const v = JSON.parse(localStorage.getItem('skimmer-map-hidden') || '[]'); return Array.isArray(v) ? v : []; }
  catch (e) { return []; }
})());
const EYE_SVG = (cross) => '<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true" fill="none" stroke="currentColor" ' +
  'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/>' +
  `<circle cx="12" cy="12" r="3"/>${cross ? '<path d="M4 4l16 16"/>' : ''}</svg>`;
const EYE_ON = EYE_SVG(false), EYE_OFF = EYE_SVG(true);

// the CPU temperature's colour: fine, warm, hot
const tempClass = (c) => (c >= 85 ? 'hot' : c >= 70 ? 'warm' : 'cool');

let status = null;
function renderStatus(d) {
  status = d;
  $('#call').textContent = d.call || 'SKIMMER';
  $('#version').textContent = d.locator || (d.version ? `v${d.version}` : '');
  $('#hdr-stats').innerHTML = `<span>CPU <b>${fmt(d.cpu_pct)}%</b>${d.cpu_temp_c != null ? ` <b class="temp ${tempClass(d.cpu_temp_c)}" title="the hottest CPU sensor: under 70 °C fine, 70–85 °C warm, 85 °C and more hot">${fmt(d.cpu_temp_c)} °C</b>` : ''}</span> <span>lost <b>${fmt(d.lost_pct, 3)}%</b></span> ` +
    `<span>feed <b>${d.feed_port ? fmt(d.feed_clients) : 'off'}</b></span> <span>${esc(d.engine || '')}</span> <span>v${esc(d.version || '')}</span>`;
  document.title = `${d.call ? `${d.call} · ` : ''}Skimmer for Linux`;
  const dot = $('#radio .dot');
  dot.className = `dot ${d.streaming ? 'ok' : 'bad'}`;
  $('#radio-text').textContent = d.radio;
  // a band's modes: its CW at the top level (when it decodes CW), its RTTY in .rtty
  const modes = bandsDecoded(d).flatMap((b) => [
    ...(b.cw !== false ? [{ ...b, mode: 'CW' }] : []),
    ...(b.rtty ? [{ ...b.rtty, band: b.band, centre_hz: b.centre_hz, ksps: b.ksps, mode: 'RTTY',
                    worker: b.worker, waiting: b.waiting }] : []),
  ]);
  const spots = modes.reduce((a, b) => a + b.spots, 0);
  const workers = (d.hub && d.hub.workers) || [];
  // beside the map: the bands as a ladder — CW and RTTY activity, the spots of
  // the last half hour and the last call — then one line of readouts
  const decoded = bandsDecoded(d);
  const maxAct = Math.max(1, ...decoded.map((b) => Math.max(b.cw !== false ? b.active : 0, b.rtty ? b.rtty.active : 0)));
  const per = (band) => window.spots ? window.spots.list.filter((s) => s.band === band) : [];
  const maxSpots = Math.max(1, ...decoded.map((b) => per(b.band).length));
  $('#ladder').innerHTML = decoded.map((b) => {
    const sp = per(b.band), cwOn = b.cw !== false, rt = b.rtty;
    const col = typeof SkMap !== 'undefined' ? SkMap.bandColor(b.band) : '';   // the band's own colour
    const i = (frac) => `<i style="width:${(100 * frac).toFixed(0)}%;${col ? `background:${col}` : ''}"></i>`;
    const bar = (on, act, dec, cls, paused) => paused
      ? `<td class="bar off" title="${esc(pausedText(paused))}"></td><td class="n mut" title="${esc(pausedText(paused))}">off</td>`
      : on
      ? `<td class="bar ${cls}" title="${fmt(act)} of ${fmt(dec)} decoders active">${i(act / maxAct)}</td><td class="n">${fmt(act)}</td>`
      : '<td class="bar off"></td><td class="n mut">–</td>';
    const off = mapHidden.has(b.band);
    return `<tr${off ? ' class="off-map"' : ''}><td class="eye"><button type="button" data-band="${esc(b.band)}" ` +
      `title="${off ? 'not on the map — click: show' : 'on the map — click: hide'} ${esc(b.band)}" ` +
      `aria-pressed="${off ? 'false' : 'true'}">${off ? EYE_OFF : EYE_ON}</button></td>` +
      `<td class="b" style="color:${col}">${esc(b.band)}</td>` +
      bar(cwOn, b.active, b.decoders, 'cw', cwOn && b.paused) +
      bar(!!rt, rt ? rt.active : 0, rt ? rt.decoders : 0, 'rtty', rt && rt.paused) +
      `<td class="bar spots" title="${fmt(sp.length)} spots in 30 min">${i(sp.length / maxSpots)}</td>` +
      `<td class="n">${fmt(sp.length)}</td><td class="last">${sp.length ? esc(sp[0].call) : ''}</td>` +
      `<td class="w mut">${b.worker ? `${b.waiting ? 'waiting for ' : 'on '}${esc(b.worker)}` : ''}</td></tr>`;
  }).join('') + '<tr class="legend-row"><td colspan="2" class="mut">map</td><td colspan="2" class="mut">CW</td><td colspan="2" class="mut">RTTY</td><td colspan="2" class="mut">spots</td><td colspan="2"></td></tr>';
  const sumDec = (f) => decoded.reduce((a, b) => a + (f(b) || 0), 0);
  const ro = (k, v) => `<span>${k} <b>${v}</b></span>`;
  // every number says over what: now, the last half hour, or since the start
  const anyRtty = decoded.some((b) => b.rtty);
  const in30 = window.spots ? window.spots.list.length : 0;
  $('#readouts').innerHTML = [
    ro('decoders now', `${fmt(sumDec((b) => (b.cw !== false ? b.decoders : 0)))} CW` +
       `${anyRtty ? ` · ${fmt(sumDec((b) => (b.rtty ? b.rtty.decoders : 0)))} RTTY` : ''}`),
    ro('spots in 30 min', fmt(in30)),
    ro(`since the start ${uptime(d.uptime_s)} ago`, `${fmt(spots)} spots` +
       `${d.feed_port ? ` · ${fmt(d.feed_lines)} telnet lines out` : ''}`),
    ...(d.feed_port ? [ro('telnet now', `${fmt(d.feed_clients)} client${d.feed_clients === 1 ? '' : 's'} on :${d.feed_port}`)] : []),
    ro('packets', `${fmt(d.packets_per_s)}/s`),
    ro('MASTER.SCP', `${fmt(d.dict_calls)} calls`),
    ...(d.rtty_adaptive ? [ro('adaptive RTTY', rttyAdaptiveText(d.rtty_adaptive))] : []),
    ...(d.thermal ? [ro('thermal guard', thermalText(d.thermal))] : []),
    ...(d.rbn ? [ro('RBN now', `${fmt(d.rbn.clients)} on :${d.rbn.port} · ${fmt(d.rbn.lines)} sent since the start${d.rbn.delay_s ? ` · held ${d.rbn.delay_s} s` : ''}`)] : []),
  ].join('');

  const head = '<tr><th>band</th><th class="l">mode</th><th>centre kHz</th><th>engine</th><th>decoders</th><th>active</th><th>stations</th><th>spots</th><th>drops</th><th>kS/s</th></tr>';
  // a band that asks for DeepCW and runs v2 (no model or no runtime) says so
  const dc = d.deepcw || {};
  const engineCell = (b) => b.worker
    ? `<td class="${b.waiting ? 'bad' : ''}" title="decoded by the worker ${esc(b.worker)}">` +
      `${b.waiting ? 'waiting for' : `${esc(b.engine)} · on`} <b>${esc(b.worker)}</b></td>`
    : b.mode === 'CW' && b.engine_wanted === 'deepcw' && b.engine !== 'deepcw'
    ? `<td class="bad" title="DeepCW not available: ${esc(dc.fallback || '')}">${esc(b.engine)} · DeepCW not available</td>`
    : b.paused
    ? `<td class="mut" title="${esc(pausedText(b.paused))}">${esc(b.engine || d.engine)} · paused</td>`
    : `<td>${esc(b.engine || d.engine)}</td>`;
  $('#bands').innerHTML = head + modes.map((b) =>
    `<tr><td>${esc(b.band)}</td><td class="l">${b.mode}</td><td>${fmtKhz(b.centre_hz / 1000, 1)}</td>${engineCell(b)}` +
    `<td>${fmt(b.decoders)}</td><td>${fmt(b.active)}</td><td>${fmt(b.stations)}</td>` +
    `<td>${fmt(b.spots)}</td><td>${fmt(b.drops)}</td><td>${fmt(b.ksps)}</td></tr>`).join('');

  $('#workers-box').hidden = !workers.length;
  if (workers.length) {
    $('#workers').innerHTML = '<tr><th>worker</th><th class="l">address</th><th class="l">bands</th><th>Mb/s</th>' +
      '<th>dropped</th><th>spots in</th><th>CPU</th></tr>' + workers.map((w) =>
        `<tr><td>${esc(w.name)}</td><td class="l">${esc(w.addr)}</td><td class="l">${w.assigned
          ? esc(w.bands.map((i) => d.bands[i] ? d.bands[i].band : `#${i}`).join(', '))
          : '<span class="mut">none assigned — Settings → Bands → decoded by</span>'}</td>` +
        `<td>${fmt(w.mbps, 1)}</td><td class="${w.dropped ? 'bad' : ''}">${fmt(w.dropped)}</td><td>${fmt(w.spots)}</td>` +
        `<td>${w.status ? `${fmt(w.status.cpu_pct)} %` : '–'}</td></tr>`).join('');
  }
  const peers = d.peers || [];
  $('#peers-box').hidden = !peers.length;
  if (peers.length) {
    $('#peers').innerHTML = '<tr><th>feed</th><th class="l">state</th><th>CQ spots in</th><th>lines</th></tr>' +
      peers.map((p) => `<tr><td>${esc(p.host)}</td><td class="l ${p.connected ? 'ok' : 'bad'}">${esc(p.state)}</td>` +
        `<td>${fmt(p.spots)}</td><td>${fmt(p.lines)}</td></tr>`).join('') +
      `<tr><td colspan="4" class="mut">${fmt(d.external_dups)} of their spots dropped as duplicates of what the feed sent already</td></tr>`;
  }

  const p = d.feed_policy, g = d.feed_gate;
  const sw = Object.entries(d.switches || {});
  $('#policy').innerHTML = '<dl class="kv">' +
    `<dt>feed policy</dt><dd>score ≥ ${fmt(p.min_score, 2)}, read ${p.min_hearings}×, settle ${fmt(p.settle_s)} s ` +
    `(unknown calls ${fmt(p.settle_nodict_s)} s), fresh ${fmt(p.fresh_s)} s, fold ${fmt(p.fold_s)} s — ${fmt(p.folded)} folded` +
    `${d.bands.some((b) => b.rtty) ? `; RTTY ${d.feed_rtty ? 'on the feed too' : 'kept off the feed'}` : ''}</dd>` +
    (d.bands.some((b) => b.rtty && b.cw !== false)
      ? `<dt>CW on RTTY</dt><dd>${d.suppress_cw_on_rtty
        ? `kept off the RTTY stations copied now (${fmt(d.bands.reduce((a, b) => a + (b.rtty ? b.rtty.pairs : 0), 0))}) — ` +
          `${fmt(d.bands.reduce((a, b) => a + (b.rtty_suppressed || 0), 0))} CW decodes dropped`
        : `decoded as anywhere else (${fmt(d.bands.reduce((a, b) => a + (b.rtty ? b.rtty.pairs : 0), 0))} RTTY stations copied now)`}</dd>`
      : '') +
    `<dt>learned gate</dt><dd>${g ? `${esc(g.id)} · ${esc(g.mode)} · threshold ${fmt(g.threshold, 2)} · ` +
      `${fmt(g.below)} of ${fmt(g.spots)} below · CW only` : 'off'}</dd>` +
    `<dt>switches</dt><dd>${sw.length ? sw.map(([k, v]) => `${esc(k)}=${esc(v)}`).join('<br>') : 'none'}</dd>` +
    '</dl>';
  $('#updated').textContent = `updated ${d.time}`;

  const fb = d.bands.filter((b) => b.cw !== false && b.engine_wanted === 'deepcw' && b.engine !== 'deepcw');
  const warn = $('#dcw-warn');
  warn.hidden = !fb.length;
  if (fb.length) {
    warn.innerHTML = `<b class="bad">DeepCW not available</b> — ${fb.map((b) => esc(b.band)).join(', ')} ` +
      `${fb.length === 1 ? 'runs' : 'run'} the v2 decoder instead: ${esc(dc.fallback || '')}. ` +
      (dc.model_ok ? 'The model is in place now — a restart puts it to work.'
                   : 'The model can be fetched under Settings → Decoding.');
  }
  renderDcw(dc);
  renderArchive(d.archive);
  if (tab === 'stats') renderDecTile(d);
}

// The spot archive (Settings): where today's spots go, what was written.
function renderArchive(a) {
  const box = $('#archive-state');
  if (!box) return;
  box.innerHTML = !a ? '<span class="mut">off</span>'
    : !a.dir ? `<span class="bad">not writing: ${esc(a.error || '')}</span>`
    : `<span class="${a.error ? 'bad' : 'ok'}">${a.error ? 'not writing' : 'writing'}</span> ` +
      `<span class="mut">${esc(a.dir)}/</span><b>${esc(a.file)}</b>` +
      `<small class="mut"> · ${fmt(a.written)} spots written since the start${a.waiting ? ` · ${fmt(a.waiting)} waiting` : ''}</small>` +
      (a.error ? `<div class="bad">${esc(a.error)}</div>` : '');
}

// The DeepCW model (Settings → Decoding): installed or not, the runtime,
// a fetch with its progress, and the restart that puts a new model to work.
function renderDcw(dc) {
  const box = $('#dcw');
  if (!box || !dc || dc.model === undefined) return;
  const may = cfg.data && cfg.data.may_change;
  const f = dc.fetch || {};
  const short = (dc.commit || '').slice(0, 7);
  let html = '<div class="k">DeepCW model <span class="warn">· not tested with this skimmer</span></div>';
  if (f.state === 'running') {
    const pct = f.total ? Math.floor((100 * f.got) / f.total) : 0;
    html += `<div>fetching… ${pct} % <span class="mut">(${fmt(f.got / 1e6, 1)} of ${fmt(f.total / 1e6, 1)} MB)</span></div>` +
      `<progress max="100" value="${pct}"></progress>`;
  } else if (dc.model_ok) {
    html += `<div><span class="ok">installed</span> <span class="mut">· e04/deepcw-engine ${esc(short)} · AGPL-3.0</span></div>`;
    if (dc.wanted && dc.fallback) {
      html += `<div>the bands that ask for it still run v2 — <button type="button" class="small" id="dcw-restart"${may ? '' : ' disabled'}>Restart</button></div>`;
    }
  } else {
    html += `<div><span class="${dc.wanted ? 'bad' : 'mut'}">not installed</span> ` +
      `<button type="button" class="small" id="dcw-fetch"${may ? '' : ' disabled'}>Download (15 MB)</button></div>` +
      `<small>by e04, AGPL-3.0, from <a href="https://github.com/e04/deepcw-engine" target="_blank" rel="noopener">github.com/e04/deepcw-engine</a> ` +
      `at ${esc(short)}, checked against its sha256 · without it a band set to DeepCW runs v2</small>`;
    if (f.state === 'failed') html += `<div class="bad">download failed: ${esc(f.error || '')}</div>`;
  }
  html += `<small class="mut">runtime: ${dc.runtime ? esc(dc.runtime) : (dc.wanted && dc.model_ok && dc.fallback ? esc(dc.fallback) : 'loaded when a band uses DeepCW')}</small>`;
  if (box.dataset.html === html) return;          // no flicker, keep focus
  box.dataset.html = html;
  box.innerHTML = html;
  const go = $('#dcw-fetch');
  if (go) {
    go.addEventListener('click', async () => {
      go.disabled = true;
      try { renderDcw({ ...dc, fetch: { state: 'running', got: 0, total: 0 } }); await post('api/deepcw', new URLSearchParams()); }
      catch (err) { renderDcw({ ...dc, fetch: { state: 'failed', error: err.message } }); }
    });
  }
  const rs = $('#dcw-restart');
  if (rs) rs.addEventListener('click', () => $('#set-restart').click());
}

// The map (and the DX cluster): the spots the feed sent in the last half hour,
// fetched every 10 s while the status page is open. Their age runs on between
// fetches (on the skimmer's clock), and a spot older than 30 min leaves the
// map, the cluster and the ladder at the next poll — also when a fetch fails
// or the page was in another tab for a while.
const SPOT_MAX_AGE_S = 30 * 60;
const spots = { raw: [], list: [], at: 0, skew: 0, locator: '', call: '' };
window.spots = spots;
let mapMounted = false;
function ageSpots(fresh) {
  const now = Date.now() / 1000 + spots.skew;
  const n = spots.list.length;
  spots.list = spots.raw.map((s) => ({ ...s, age: now - s.t })).filter((s) => s.age <= SPOT_MAX_AGE_S);
  if (fresh || spots.list.length !== n) renderSpots();
}
async function refreshSpots(force) {
  if (!force && Date.now() - spots.at < 10000) { ageSpots(false); return; }
  spots.at = Date.now();
  const d = await getJSON(`api/spots?minutes=${SPOT_MAX_AGE_S / 60}`);
  spots.locator = d.locator;
  spots.call = d.call;
  spots.skew = d.now - Date.now() / 1000;
  spots.raw = d.spots;
  ageSpots(true);
  if (status) renderStatus(status);   // the ladder and readouts count these spots
}
// The DX cluster filters the spots; the map draws what the filters leave, the
// row clicked marked on it.
// The DX cluster: under the band ladder beside the map on a wide page, under
// the map on a narrow one.
const wideDxc = matchMedia('(min-width: 1400px)');
function placeDxc() {
  const el = $('#dxc'), side = $('.st-side'), top = $('.st-top');
  if (wideDxc.matches) {
    if (el.parentNode !== side) { side.appendChild(el); el.classList.remove('pad'); el.classList.add('side'); }
  } else if (el.parentNode === side) {
    top.after(el); el.classList.add('pad'); el.classList.remove('side');
  }
}
wideDxc.addEventListener('change', placeDxc);
placeDxc();

// The line between the map and the rest: dragged, it sets the map's width
// (kept in the browser).
(() => {
  const top = $('.st-top'), handle = $('#st-split');
  const set = (px) => {
    const w = Math.round(Math.max(300, Math.min(top.clientWidth - 360, px)));
    top.style.gridTemplateColumns = `${w}px 1fr`;
    return w;
  };
  let kept = 0;
  try { kept = Number(localStorage.getItem('skimmer-mapw')) || 0; } catch (e) { /* private */ }
  if (kept) set(kept);
  let dragging = false, raf = 0;
  handle.addEventListener('pointerdown', (e) => { dragging = true; handle.classList.add('on'); handle.setPointerCapture(e.pointerId); e.preventDefault(); });
  handle.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    const w = set(e.clientX - top.getBoundingClientRect().left);
    cancelAnimationFrame(raf);
    raf = requestAnimationFrame(() => { if (typeof SkMap !== 'undefined') SkMap.redraw(); });
    try { localStorage.setItem('skimmer-mapw', String(w)); } catch (err) { /* private */ }
  });
  const end = () => { dragging = false; handle.classList.remove('on'); };
  handle.addEventListener('pointerup', end);
  handle.addEventListener('pointercancel', end);
  handle.addEventListener('dblclick', () => {                   // back to the default
    top.style.gridTemplateColumns = '';
    try { localStorage.removeItem('skimmer-mapw'); } catch (err) { /* private */ }
    if (typeof SkMap !== 'undefined') SkMap.redraw();
  });
})();

// What the DX cluster shows goes to the map, less the bands its eye in the
// ladder hides (kept in the browser).
const mapShown = { list: [], marked: null };
function mapDraw() {
  if (typeof SkMap === 'undefined') return;
  SkMap.update(mapShown.list.filter((s) => !mapHidden.has(s.band)).map((s) => ({ ...s, hi: s.call === mapShown.marked })),
               { locator: spots.locator, call: spots.call, visitor: visitor.call });
}
on('#ladder', 'click', (e) => {
  const b = e.target.closest('button[data-band]');
  if (!b) return;
  const band = b.dataset.band;
  if (mapHidden.has(band)) mapHidden.delete(band); else mapHidden.add(band);
  try { localStorage.setItem('skimmer-map-hidden', JSON.stringify([...mapHidden])); } catch (err) { /* private window */ }
  if (status) renderStatus(status);
  mapDraw();
});
function renderSpots() {
  if (typeof SkMap === 'undefined' || typeof Dxc === 'undefined') return;
  if (!mapMounted) {
    SkMap.mount($('#skmap'));
    Dxc.mount($('#dxc'), (shown, marked) => { mapShown.list = shown; mapShown.marked = marked; mapDraw(); });
    mapMounted = true;
    SkMap.update([], { locator: spots.locator, call: spots.call, visitor: visitor.call });   // the centre before the rows
  }
  const bands = status ? status.bands.map((b) => b.band) : [];
  const workers = status && status.hub ? status.hub.workers.map((w) => w.name) : [];
  Dxc.update(spots.list, bands, workers);
}

tabs.status = {
  enter() { if (status) refreshSpots(true).catch(() => ageSpots(false)); },
  async refresh() {
    renderStatus(await getJSON('status.json'));
    if (tab === 'status') refreshSpots().catch(() => ageSpots(false));
  },
};

// ---- statistics ------------------------------------------------------------------
const stats = { hours: 24, data: null, at: 0, end: null };   // end: unix s, null = now
// the spots chart's y-axis, linear or log (kept in the browser)
stats.logY = (() => { try { return localStorage.getItem('skimmer-spots-y') === 'log'; } catch (e) { return false; } })();
function spotsYButtons() {
  for (const b of document.querySelectorAll('#spots-y button')) b.classList.toggle('on', (b.dataset.y === 'log') === stats.logY);
}
spotsYButtons();
// the night under the charts (the grey line at the station), on unless turned off
// (charts.js loads deferred, after this file: handed over in renderStats)
stats.night = (() => { try { return localStorage.getItem('skimmer-stats-night') !== 'off'; } catch (e) { return true; } })();
$('#stats-night').classList.toggle('on', stats.night);
on('#stats-night', 'click', () => {
  stats.night = !stats.night;
  $('#stats-night').classList.toggle('on', stats.night);
  try { localStorage.setItem('skimmer-stats-night', stats.night ? 'on' : 'off'); } catch (err) { /* private */ }
  renderStats();
});
on('#spots-y', 'click', (e) => {
  const y = e.target.dataset && e.target.dataset.y;
  if (!y) return;
  stats.logY = y === 'log';
  try { localStorage.setItem('skimmer-spots-y', y); } catch (err) { /* private window */ }
  spotsYButtons();
  renderStats();
});

// The CW + RTTY decoders tile: every channel decoder running now, summed over
// the bands, CW beside RTTY (live, from the status — the history has no count).
function renderDecTile(d) {
  const el = $('#stats-dec');
  if (!el || !d) return;
  const all = bandsDecoded(d);
  const cw = all.filter((b) => b.cw !== false);
  const rt = all.filter((b) => b.rtty);
  const n = (list, f) => list.reduce((a, b) => a + (f(b) || 0), 0);
  const part = (label, cls, list, f) => `<div class="part ${cls}"><div class="n">${list.length ? fmt(n(list, (b) => f(b).decoders)) : '–'}</div>` +
    `<div class="l">${label}<small>${list.length ? `${fmt(n(list, (b) => f(b).active))} active · ${list.length} band${list.length === 1 ? '' : 's'}` : 'no band'}</small></div></div>`;
  el.innerHTML = '<div class="k">CW + RTTY decoders <small>running now</small></div><div class="parts">' +
    part('CW', 'cw', cw, (b) => b) + part('RTTY', 'rtty', rt, (b) => b.rtty) + '</div>';
}
// why the skimmer paused a band's mode on its own (status.json "paused")
function pausedText(why) {
  return why === 'rtty-adaptive' ? 'off: adaptive RTTY found no contest — on again for a test at the next full hour'
    : why === 'thermal' ? 'off: the CPU runs hot — the band with the fewest spots is paused until it cools'
    : `paused (${why})`;
}
// the thermal guard, one line: the mean against the limit, the bands paused
// (each with the drop its pause brought and the mean it waits for)
function thermalText(t) {
  const m = t.mean_c == null ? '–' : `${fmt(t.mean_c, 1)} °C`;
  const shed = (t.shed || []).map((s) => `${esc(s.band)} off${s.drop_c == null ? ''
    : ` (−${fmt(s.drop_c, 1)} °C)`}, back ≤ ${fmt(s.back_at_c, 1)} °C`).join(' · ');
  return `${m} / ${fmt(t.limit_c, 1)} °C, ${t.average_min}-min mean` +
    (t.state === 'hot' ? ' — <span class="bad">over the limit</span>' : '') + (shed ? ` · ${shed}` : '');
}
// adaptive RTTY's state, one line: testing, on (a contest) or off till the hour
function rttyAdaptiveText(a) {
  const hm = (t) => { const x = new Date(t * 1000); return `${String(x.getUTCHours()).padStart(2, '0')}:${String(x.getUTCMinutes()).padStart(2, '0')}`; };
  const last = a.last == null ? '' : ` · last test ${fmt(a.last)} spots`;
  return a.state === 'test' ? `testing till ${hm(a.until)} UTC: ${fmt(a.count)} of > ${fmt(a.spots)} spots`
    : a.state === 'on' ? `on — a contest${last} · next test ${hm(a.until)} UTC`
    : `off — no contest${last} (> ${fmt(a.spots)} in ${a.minutes} min) · next test ${hm(a.until)} UTC`;
}
function sum(a) { return a.reduce((x, v) => x + (v || 0), 0); }
// CW and RTTY keep one colour each across Statistics (CSS --cw, --rtty)
function modeColor(m) { return getComputedStyle(document.documentElement).getPropertyValue(`--${m}`).trim(); }
function mean(a) { const v = a.filter((x) => x != null); return v.length ? sum(v) / v.length : null; }

function renderStats() {
  const d = stats.data;
  if (!d) return;
  const t = Array.from({ length: d.n }, (_, i) => d.t0 + i * d.step_min * 60);
  const stepH = d.step_min / 60;
  const label = (i) => {
    const a = new Date(t[i] * 1000), b = new Date((t[i] + d.step_min * 60) * 1000);
    const f = (x) => `${String(x.getUTCHours()).padStart(2, '0')}:${String(x.getUTCMinutes()).padStart(2, '0')}`;
    const day = (x) => `${x.getUTCDate()}.${x.getUTCMonth() + 1}.${stats.hours > 2160 ? x.getUTCFullYear() : ''}`;
    return d.step_min >= 1440 || a.getUTCDate() !== b.getUTCDate() && d.step_min >= 360
      ? `${day(a)} ${f(a)} – ${day(b)} ${f(b)} UTC` : `${day(a)} ${f(a)}–${f(b)} UTC`;
  };
  // the night at the station (the grey line): where the map has its centre
  const site = status && typeof SkMap !== 'undefined' ? SkMap.site(status.locator, status.call) : null;
  stats.sited = !!site;
  Charts.setNight(stats.night);
  const x = { t, spanH: stats.hours, label, step: d.step_min * 60, site,
              sun: typeof SkMap !== 'undefined' ? { at: SkMap.subsolar, alt: SkMap.sunAlt } : null };
  renderTimebar(d);
  const per = d.step_min % 1440 === 0 ? `${d.step_min / 1440}-day` : d.step_min % 60 === 0
    ? `${d.step_min / 60}-hour` : `${d.step_min}-minute`;
  $('#stats-note').textContent = `${per} buckets · times in UTC` +
    (d.res_min > 1 ? ` · kept as ${d.res_min === 60 ? 'hourly' : `${d.res_min}-minute`} records for this range` : '');

  // colour follows the band (config order), never its rank; CW up, RTTY down
  const perHour = (mm) => d.bands.map((b, i) => ({
    name: b, color: Charts.seriesColor(i),
    values: mm.spots[b].map((v) => v == null ? null : v / stepH),
  }));
  const series = perHour(d.cw);
  $('#legend-spots').replaceChildren(...series.map((s) => {
    const sp = document.createElement('span'); const i = document.createElement('i');
    i.style.background = s.color; sp.append(i, document.createTextNode(s.name)); return sp;
  }));
  const loading = (id, on) => $(id).classList.toggle('loading', on);
  Charts.mirror($('#chart-spots'), x,
    [{ name: 'CW', series, color: modeColor('cw') },
     ...(d.rtty ? [{ name: 'RTTY', series: perHour(d.rtty), color: modeColor('rtty') }] : [])],
    (v) => fmt(v, v < 10 && v % 1 ? 1 : 0), 160, stats.logY);
  // a band's block: CW over RTTY (a mode the band does not decode: an empty
  // strip), both in one ramp — the strip's place says the mode, each its own scale
  const decoded = (mm, b) => mm && mm.active[b] && mm.active[b].some((v) => v != null) ? mm.active[b] : null;
  const amax = Charts.heat($('#chart-active'), x, d.bands.map((b) => ({ name: b, parts: [
    { label: 'CW', values: decoded(d.cw, b), lo: '--seq-lo', hi: '--seq-hi' },
    ...(d.rtty ? [{ label: 'RTTY', values: decoded(d.rtty, b), lo: '--seq-lo', hi: '--seq-hi' }] : []),
  ] })), (v) => fmt(v, 1), 'active decoders');
  const span = (l) => (amax[l] ? ` <span class="mut">0–${fmt(amax[l], amax[l] < 10 ? 1 : 0)}</span>` : '');
  $('#legend-active').innerHTML = '<span><i class="ramp" style="background:linear-gradient(90deg,var(--seq-lo),var(--seq-hi))"></i>' +
    `${d.rtty ? 'upper strip ' : ''}CW${span('CW')}${d.rtty ? ` · lower strip RTTY${span('RTTY')}` : ''}</span>`;
  // CPU: the CW and RTTY pipelines' threads and the rest of the process, stacked
  // under the total (history before the split has the total line only)
  const g = d.global, cpuCw = g.cpu_cw || g.cpu.map(() => null), cpuRt = g.cpu_rtty || g.cpu.map(() => null);
  const anyRt = cpuRt.some((v) => v != null);
  const layers = [
    { name: 'CW decoders', color: modeColor('cw'), values: cpuCw },
    ...(anyRt ? [{ name: 'RTTY decoders', color: modeColor('rtty'), values: cpuRt }] : []),
    { name: 'rest (IQ in, feed, web)', color: getComputedStyle(document.documentElement).getPropertyValue('--axis').trim(),
      values: g.cpu.map((v, i) => v == null || cpuCw[i] == null ? null
        : Math.max(0, v - cpuCw[i] - (anyRt ? cpuRt[i] || 0 : 0))) },
  ];
  const lostColor = Charts.seriesColor(7);
  $('#legend-cpu').innerHTML = layers.map((l) => `<span><i style="background:${l.color}"></i>${esc(l.name)}</span>`).join('') +
    `<span><i class="line"></i>all</span><span><i class="line" style="border-color:${lostColor};opacity:1"></i>IQ lost (right)</span>`;
  Charts.area($('#chart-cpu'), x, layers, g.cpu, (v) => `${fmt(v)} %`, 'CPU, all, % of a core',
    { name: 'IQ lost, worst minute', values: g.lost, color: lostColor,
      fmtV: (v) => `${fmt(v, 3)} %`, fmtTick: (v) => `${+v.toPrecision(2)} %` });
  // the CPU temperature: the hottest minute of each bucket (from v0.8.2 on);
  // the line coloured by its height — green while fine, amber from 70 °C, red
  // at 85 °C and above (the header's colours)
  const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const tempRamp = [[60, css('--ok')], [70, css('--ok')], [78, css('--warn')], [85, css('--bad')]];
  const nowT = status && status.cpu_temp_c != null ? status.cpu_temp_c : null;
  $('#legend-temp').innerHTML = '<span><i class="ramp" style="background:linear-gradient(90deg,var(--ok),var(--ok) 35%,var(--warn) 65%,var(--bad))"></i>' +
    `CPU temperature, the hottest minute — green fine, amber from 70 °C, red from 85 °C${nowT != null ? ` · now <b class="temp ${tempClass(nowT)}">${fmt(nowT)} °C</b>` : ''}</span>` +
    (status && status.thermal ? `<span><i class="line dash"></i>the thermal guard's limit (on the ${status.thermal.average_min}-min mean)</span>` : '');
  const guard = status && status.thermal ? status.thermal.limit_c : null;
  Charts.line($('#chart-temp'), x, g.cpu_temp || g.cpu.map(() => null),
    { name: 'CPU temperature, the hottest minute', color: css('--ok'), ramp: tempRamp, fmtV: (v) => `${fmt(v)} °C`,
      fmtTick: (v) => `${fmt(v)} °C`, height: 110, minSpan: 20,
      mark: guard == null ? null : { value: guard, label: `thermal guard ${fmt(guard)} °C (on the mean)` } });
  ['#chart-spots', '#chart-active', '#chart-cpu', '#chart-temp'].forEach((id) => loading(id, false));

  // CW and RTTY one under the other — in the tiles and in every table cell
  const modes = [['CW', d.cw], ...(d.rtty ? [['RTTY', d.rtty]] : [])];
  const all = (mm, k) => d.bands.reduce((a, b) => a + sum(mm[k][b]), 0);
  const tileM = (k, f, unit = '', title = '') => `<div class="tile"${title ? ` title="${esc(title)}"` : ''}><div class="k">${esc(k)}</div>` +
    modes.map(([l, mm]) => `<div class="v ml ${l.toLowerCase()}">${f(mm)} <small>${l}${unit ? ` · ${esc(unit)}` : ''}</small></div>`).join('') + '</div>';
  const streamed = mean(d.global.streaming);
  renderDecTile(status);
  // telnet clients: now (live), and the range's mean and busiest bucket
  const cl = d.global.clients.filter((v) => v != null);
  const clNow = status && status.feed_port ? status.feed_clients : null;
  $('#stats-tiles').innerHTML = [
    tileM('spots', (mm) => fmt(all(mm, 'spots')), `in ${stats.hours} h`),
    tileM('calls decoded', (mm) => fmt(all(mm, 'stations')), '',
          'calls that came into a band\'s station list in the range — every call the decoder read and ' +
          'validated, spotted or not (a call heard again after it dropped out counts again)'),
    `<div class="tile" title="clients on the telnet feed (the skimmer server's port)"><div class="k">telnet clients</div>` +
      `<div class="v">${clNow == null ? '<span class="mut">feed off</span>' : fmt(clNow)} <small>now</small></div>` +
      `<div class="v ml">${cl.length ? fmt(mean(cl), 1) : '–'} <small>mean · busiest ${cl.length ? fmt(Math.max(...cl), 1) : '–'}</small></div></div>`,
    tile('radio streamed', streamed == null ? '–' : fmt(100 * streamed),
         streamed == null ? 'no data' : '% of the recorded time'),
  ].join('');

  const hrs = (d.n * d.step_min) / 60;
  const has = (mm, b) => ['spots', 'active'].some((k) => mm[k][b].some((v) => v != null));
  // the columns, each with what it counts (the note under the table too) and
  // its number for the bar beside the value (longest bar = the column's most)
  const cols = [
    ['spots', 'spots sent to the telnet feed in the range', (b, mm) => sum(mm.spots[b]), (v) => fmt(v)],
    ['spots per hour', 'the spots divided by the hours of the range', (b, mm) => sum(mm.spots[b]) / hrs, (v) => fmt(v, 1)],
    ['calls decoded', 'calls that came into the band\'s station list — read and validated, spotted or not',
      (b, mm) => sum(mm.stations[b]), (v) => fmt(v)],
    ['busy decoders', 'channel decoders reading running text, on average over the range', (b, mm) => mean(mm.active[b]),
      (v) => fmt(v, 1)],
    ['SNR', 'the typical SNR of the spots: the median of each minute\'s spots, averaged',
      (b, mm) => mean(mm.snr[b]), (v) => `${fmt(v)} dB`],
  ];
  const top = cols.map(([, , f]) => Math.max(0, ...d.bands.flatMap((b) => modes.map(([, mm]) => (has(mm, b) ? f(b, mm) || 0 : 0)))));
  const cell = (b, c) => modes.map(([l, mm]) => {
    const v = has(mm, b) ? cols[c][2](b, mm) : null;
    const w = v == null || !(top[c] > 0) ? 0 : Math.max(0, (100 * v) / top[c]);
    return `<div class="ml ${l.toLowerCase()} tb"><span class="trk"><i style="width:${w.toFixed(1)}%"></i></span>` +
      `<span class="tv">${v == null ? '–' : cols[c][3](v)}</span></div>`;
  }).join('');
  $('#stats-table').innerHTML = '<tr><th>band</th><th class="l">mode</th>' +
    `${cols.map(([n, t]) => `<th title="${esc(t)}">${n}</th>`).join('')}</tr>` +
    d.bands.map((b) => `<tr><td>${esc(b)}</td>` +
      `<td class="l">${modes.map(([l]) => `<div class="ml ${l.toLowerCase()}">${l}</div>`).join('')}</td>` +
      cols.map((_, c) => `<td>${cell(b, c)}</td>`).join('') + '</tr>').join('');
  $('#stats-cols').innerHTML = cols.map(([n, t]) => `<b>${n}</b> = ${esc(t)}`).join(' · ');
}

tabs.stats = {
  enter() { this.refresh(true); },
  async refresh(force) {
    // drawn before the first status came: again now that the station's place is known
    if (!force && stats.data && !stats.sited && status) renderStats();
    // history moves by the minute: refetch at most every 30 s
    if (!force && (stats.end || Date.now() - stats.at < 30000)) return;   // a past window stays
    ['#chart-spots', '#chart-active', '#chart-cpu', '#chart-temp'].forEach((id) => $(id).classList.add('loading'));
    stats.data = await getJSON(`stats.json?hours=${stats.hours}${stats.end ? `&end=${stats.end}` : ''}`);
    stats.at = Date.now();
    renderStats();
  },
};
// The time slider: shown while the range is shorter than the history kept;
// right = now, left = as far back as the oldest record. All charts follow.
// Above it, the whole history in small (spots per bucket, CW + RTTY) with the
// window the charts show as a frame that moves with the slider while it is
// dragged — and the frame can be dragged (or the history clicked) directly.
function sliderStep() { return stats.hours <= 24 ? 1 : stats.hours <= 168 ? 6 : stats.hours <= 2160 ? 24 : 24 * 7; }
const over = { key: '', at: 0, data: null, maxOff: 0, busy: false };
function renderTimebar(d) {
  const bar = $('#timebar'), inp = $('#t-end');
  const nowS = Math.floor(Date.now() / 1000);
  const back = d.first ? Math.floor((nowS - d.first) / 3600) - stats.hours : 0;
  bar.hidden = back < sliderStep();
  if (bar.hidden) return;
  const step = sliderStep(), maxOff = Math.floor(back / step) * step;
  inp.min = String(-maxOff); inp.max = '0'; inp.step = String(step);
  const off = stats.end ? Math.round((nowS - stats.end) / 3600) : 0;
  if (document.activeElement !== inp && !over.drag) inp.value = String(-Math.min(off, maxOff));
  over.maxOff = maxOff;
  timeLabel(-Number(inp.value));
  // the history the slider spans: fetched once per range, again after 5 min
  const key = `${maxOff + stats.hours}`;
  if ((over.key !== key || Date.now() - over.at > 300000) && !over.busy) {
    over.busy = true;
    getJSON(`stats.json?hours=${Math.min(17520, maxOff + stats.hours)}`)
      .then((o) => { over.data = o; over.key = key; over.at = Date.now(); drawOver(); })
      .catch(() => {})
      .finally(() => { over.busy = false; });
  }
  drawOver();
}
// the overview's time axis: from the slider's leftmost window start to now
function overSpan() {
  const nowS = Math.floor(Date.now() / 1000);
  return { t0: nowS - (over.maxOff + stats.hours) * 3600, t1: nowS };
}
function drawOver() {
  const box = $('#t-over');
  if (!box || $('#timebar').hidden) return;
  const W = Math.max(200, box.clientWidth), H = 48;
  const { t0, t1 } = overSpan(), xt = (t) => ((t - t0) / (t1 - t0)) * W;
  let bars = '';
  const o = over.data;
  if (o) {
    const tot = Array.from({ length: o.n }, (_, i) => {
      let v = 0, any = false;
      for (const mm of [o.cw, o.rtty]) {
        if (!mm) continue;
        for (const b of o.bands) { const x = mm.spots[b][i]; if (x != null) { v += x; any = true; } }
      }
      return any ? v : null;
    });
    const mx = Math.max(1, ...tot.filter((v) => v != null));
    const bw = Math.max(1, xt(o.t0 + o.step_min * 60) - xt(o.t0));
    tot.forEach((v, i) => {
      if (!v) return;
      const x = xt(o.t0 + i * o.step_min * 60), h = Math.max(1, (H - 4) * Math.sqrt(v / mx));
      if (x + bw < 0 || x > W) return;
      bars += `M${x.toFixed(1)},${H}v${(-h).toFixed(1)}h${bw.toFixed(1)}v${h.toFixed(1)}Z`;
    });
  }
  const off = -Number($('#t-end').value), end = t1 - off * 3600, start = end - stats.hours * 3600;
  const a = Math.max(0, xt(start)), b = Math.min(W, xt(end));
  // the history dim, the charts' window bright inside a green frame
  box.innerHTML = `<svg viewBox="0 0 ${W} ${H}" height="${H}" preserveAspectRatio="none">` +
    `<defs><clipPath id="t-win"><rect x="${a.toFixed(1)}" y="0" width="${Math.max(2, b - a).toFixed(1)}" height="${H}"/></clipPath></defs>` +
    `<path d="${bars}" fill="var(--axis)" opacity=".7"/>` +
    `<rect class="win" x="${a.toFixed(1)}" y="1" width="${Math.max(3, b - a).toFixed(1)}" height="${H - 2}"/>` +
    `<path d="${bars}" fill="var(--fg)" opacity=".8" clip-path="url(#t-win)"/>` +
    `${o ? '' : `<text x="${W / 2}" y="${H / 2 + 4}" text-anchor="middle" font-size="11" fill="var(--tick)">loading the history…</text>`}</svg>`;
}
function timeLabel(offH) {
  const nowS = Math.floor(Date.now() / 1000), end = nowS - offH * 3600, start = end - stats.hours * 3600;
  const f = (t) => { const x = new Date(t * 1000); return `${x.getUTCDate()}.${x.getUTCMonth() + 1}.${stats.hours > 2160 ? x.getUTCFullYear() : ''} ${String(x.getUTCHours()).padStart(2, '0')}:00`; };
  $('#t-label').textContent = offH ? `${f(start)} – ${f(end)} UTC` : `the last ${stats.hours >= 48 ? `${stats.hours / 24} days` : `${stats.hours} h`}, up to now`;
  $('#t-now').disabled = !offH;
}
on('#t-end', 'input', (e) => { timeLabel(-Number(e.target.value)); drawOver(); });
on('#t-end', 'change', (e) => {
  const off = -Number(e.target.value);
  stats.end = off ? Math.floor(Date.now() / 1000) - off * 3600 : null;
  tabs.stats.refresh(true);
});
// the overview: press or drag puts the window's middle under the pointer
function overPick(e) {
  const box = $('#t-over'), r = box.getBoundingClientRect(), { t0, t1 } = overSpan();
  const t = t0 + ((e.clientX - r.left) / r.width) * (t1 - t0) + (stats.hours * 3600) / 2;
  const step = sliderStep();
  const off = Math.min(over.maxOff, Math.max(0, Math.round((t1 - t) / 3600 / step) * step));
  $('#t-end').value = String(-off);
  timeLabel(off);
  drawOver();
}
on('#t-over', 'pointerdown', (e) => {
  over.drag = true;
  e.currentTarget.setPointerCapture(e.pointerId);
  overPick(e);
});
on('#t-over', 'pointermove', (e) => { if (over.drag) overPick(e); });
const overDone = () => {
  if (!over.drag) return;
  over.drag = false;
  $('#t-end').dispatchEvent(new Event('change'));
};
on('#t-over', 'pointerup', overDone);
on('#t-over', 'pointercancel', overDone);
on('#t-now', 'click', () => { stats.end = null; tabs.stats.refresh(true); });

for (const b of document.querySelectorAll('#range button')) {
  b.addEventListener('click', () => {
    stats.hours = Number(b.dataset.h);
    stats.end = null;
    for (const o of document.querySelectorAll('#range button')) o.classList.toggle('on', o === b);
    tabs.stats.refresh(true);
  });
}
let resizeT = 0;
window.addEventListener('resize', () => {
  clearTimeout(resizeT);
  resizeT = setTimeout(() => { if (tab === 'stats') renderStats(); }, 150);
});

// ---- settings --------------------------------------------------------------------
const cfg = { data: null, dirty: false };
// An edit not saved yet: a bar in the header on every tab, and the browser
// asks before the page is left.
function setDirty(v) {
  cfg.dirty = v;
  const bar = $('#unsaved');
  if (bar) bar.hidden = !v;
}
addEventListener('beforeunload', (e) => {
  if (!cfg.dirty) return;
  e.preventDefault();
  e.returnValue = '';
});
on('#unsaved-show', 'click', () => show('settings'));
on('#unsaved-save', 'click', () => { show('settings'); $('#settings').requestSubmit(); });
on('#unsaved-discard', 'click', () => {
  if (tab === 'settings') tabs.settings.refresh(true); else setDirty(false);
});
// A call and a locator in capitals, whatever is typed (the caret stays put).
const upperFields = () => document.querySelectorAll('#settings input.upper');
on('#settings', 'input', (e) => {
  const t = e.target;
  if (!t.classList || !t.classList.contains('upper')) return;
  const u = t.value.toUpperCase();
  if (u === t.value) return;
  const a = t.selectionStart, b = t.selectionEnd;
  t.value = u;
  try { t.setSelectionRange(a, b); } catch (err) { /* not a text field */ }
});
const MAX_BANDS = 8;          // receivers: the bands on
const MAX_LISTED = 16;        // [bands] lines, the ones off included

// Read-only settings (an address outside [web] settings_from) stay readable:
// a choice list still opens to show its options, a text field can be selected
// and copied — only nothing takes a change. Ticks and buttons are disabled.
function applyLock(root, may) {
  for (const inp of root.querySelectorAll('input, select, button')) {
    if (inp.tagName === 'SELECT') {
      inp.disabled = false;
      inp.dataset.locked = may ? '' : '1';
      inp.dataset.v = inp.value;
    } else if (inp.tagName === 'BUTTON' || inp.type === 'checkbox' || inp.type === 'file') {
      inp.disabled = !may;
    } else {
      inp.disabled = false;
      inp.readOnly = !may;
    }
  }
}
// a locked list snaps back to its value before anything sees the change
for (const ev of ['input', 'change']) {
  on('#settings', ev, (e) => {
    const t = e.target;
    if (t.tagName === 'SELECT' && t.dataset.locked === '1') {
      t.value = t.dataset.v;
      e.stopImmediatePropagation();
    }
  }, true);
}

function bandRow(b, i, n, may, rx) {
  const tr = document.createElement('tr');
  const dis = may ? '' : ' disabled';
  tr.classList.toggle('off', !b.on);
  tr.innerHTML =
    `<td>${b.on ? `RX${rx}` : '<span class="mut">off</span>'}</td>` +
    `<td><input type="checkbox" data-k="on"${b.on ? ' checked' : ''}${dis} title="received"></td>` +
    `<td><input data-k="name" value="${esc(b.name)}" size="6"${dis}></td>` +
    `<td><input data-k="khz" value="${esc(b.khz)}" inputmode="decimal" size="9"${dis}></td>` +
    `<td><input type="checkbox" data-k="cw"${b.cw ? ' checked' : ''}${dis}></td>` +
    `<td><select data-k="engine"${dis}><option value="v3">v3.1</option><option value="v2">v2</option><option value="deepcw" title="not tested with this skimmer">DeepCW (not tested)</option></select></td>` +
    `<td><input type="checkbox" data-k="rtty"${b.rtty ? ' checked' : ''}${dis}></td>` +
    `<td><input data-k="worker" value="${esc(b.worker || '')}" list="worker-names" size="8" placeholder="this PC"${dis}></td>` +
    `<td><button type="button" data-a="up" title="earlier receiver"${i && may ? '' : ' disabled'}>↑</button>` +
    `<button type="button" data-a="down" title="later receiver"${i < n - 1 && may ? '' : ' disabled'}>↓</button>` +
    `<button type="button" data-a="del" title="remove"${n > 1 && may ? '' : ' disabled'}>✕</button></td>`;
  tr.querySelector('select').value = b.engine;
  if (!may) applyLock(tr, false);               // the ↑ ↓ ✕ keep their own limits when may
  return tr;
}
function readBands() {
  return [...document.querySelectorAll('#band-rows tr')].map((tr) => ({
    name: tr.querySelector('[data-k=name]').value.trim(),
    khz: tr.querySelector('[data-k=khz]').value.trim(),
    cw: tr.querySelector('[data-k=cw]').checked,
    engine: tr.querySelector('[data-k=engine]').value,
    rtty: tr.querySelector('[data-k=rtty]').checked,
    on: tr.querySelector('[data-k=on]').checked,
    worker: tr.querySelector('[data-k=worker]').value.trim(),
  }));
}
function drawBands(list) {
  const may = cfg.data && cfg.data.may_change;
  let rx = 0;
  $('#band-rows').replaceChildren(...list.map((b, i) => bandRow(b, i, list.length, may, b.on ? ++rx : 0)));
  // the workers this hub knows: named in a band, connected, or found on the LAN
  const names = new Set(list.map((b) => b.worker).filter(Boolean));
  for (const w of (status && status.hub && status.hub.workers) || []) names.add(w.name);
  for (const n of cfg.lanWorkers || []) names.add(n);
  $('#worker-names').innerHTML = [...names].map((n) => `<option value="${esc(n)}">`).join('');
  const off = list.length - rx;
  $('#band-count').textContent = `${rx} of ${MAX_BANDS} receivers${off ? ` · ${off} off` : ''}`;
  $('#band-count').classList.toggle('bad', rx > MAX_BANDS);
  $('#band-add').disabled = !may || list.length >= MAX_LISTED;
}
// ticking a band on or off renumbers the receivers
on('#band-rows', 'change', (e) => {
  if (e.target.dataset && e.target.dataset.k === 'on') drawBands(readBands());
});
on('#band-rows', 'click', (e) => {
  const a = e.target.dataset && e.target.dataset.a;
  if (!a) return;
  const list = readBands(), i = [...$('#band-rows').children].indexOf(e.target.closest('tr'));
  if (a === 'del') list.splice(i, 1);
  if (a === 'up' && i > 0) [list[i - 1], list[i]] = [list[i], list[i - 1]];
  if (a === 'down' && i < list.length - 1) [list[i + 1], list[i]] = [list[i], list[i + 1]];
  drawBands(list);
  setDirty(true);
});
on('#band-add', 'click', () => {
  const list = readBands();
  const on = list.filter((b) => b.on).length < MAX_BANDS;   // a ninth line starts off
  list.push({ name: '', khz: '', cw: true, engine: $('[name="decode.engine"]').value || 'v3', rtty: false, on });
  drawBands(list);
  setDirty(true);
});
on('#settings', 'input', () => setDirty(true));

// The callsign filter, as IP-rotator's DX filter (its DXC page): plain text
// (part of a call, any case) or, with Regex, a regular expression ignoring
// case; ticking Regex on an empty field puts in its default — every call but
// the Russian and Belarusian ones. dxc.js's DX menu does the same.
const DX_FILTER_DEFAULT = '^(?!^(?:R[A-Z]?|U[A-I0]|E[UVW])[0-9]{1,3}[A-Z]{1,4}(?:/[A-Z0-9]+)?$).+';
function filterCheck() {
  const inp = $('#filter-call'), re = $('#filter-regex').checked;
  let ok = true;
  if (re && inp.value.trim()) { try { new RegExp(inp.value.trim(), 'i'); } catch (e) { ok = false; } }
  inp.classList.toggle('bad', !ok);
  $('#filter-bad').hidden = ok;
  return ok;
}
on('#filter-call', 'input', filterCheck);
on('#filter-regex', 'change', (e) => {
  if (e.target.checked && !$('#filter-call').value.trim()) $('#filter-call').value = DX_FILTER_DEFAULT;
  filterCheck();
});
// the own call's filter names the call it compares with (Station and feed → Call)
function mycallShow() {
  const c = $('[name="feed.call"]').value.trim().toUpperCase();
  $('#filter-mycall-call').textContent = c ? `(${c})` : '(no call set)';
  const d = $('#filter-mycall-diff').value.trim();
  const ok = d === '' || /^[0-3]$/.test(d);
  $('#filter-mycall-diff').classList.toggle('bad', !ok);
  return ok;
}
on('[name="feed.call"]', 'input', mycallShow);
on('#filter-mycall-diff', 'input', mycallShow);
on('#filter-clear', 'click', () => {
  $('#filter-call').value = '';
  $('#filter-regex').checked = false;
  setDirty(true);
  filterCheck();
});

function fillSettings(d) {
  cfg.data = d;
  setDirty(false);
  const f = $('#settings');
  for (const [k, v] of Object.entries(d.values)) {
    const inp = f.elements[k];
    if (!inp) continue;
    if (inp.type === 'checkbox') inp.checked = v === '' ? inp.dataset.default === 'true' : v === 'true' || v === '1';
    else if (inp.tagName === 'SELECT') inp.value = v || inp.dataset.default || inp.options[0].value;
    else inp.value = v;
  }
  for (const inp of upperFields()) inp.value = inp.value.toUpperCase();
  applyLock(f, d.may_change);
  f.classList.toggle('locked', !d.may_change);
  $('#set-export').disabled = false;                 // anyone may read the file
  $('#lan-find').disabled = false;                   // a search changes nothing
  drawBands(d.bands.map((b) => ({ name: b.name, khz: fmtKhz(b.centre_hz / 1000, null, ' '), cw: b.cw,
                                  engine: b.engine, rtty: b.rtty, on: b.on !== false,
                                  worker: b.worker || '' })));
  const lock = $('#set-lock');
  lock.hidden = d.may_change;
  if (!d.may_change) {
    const host = location.hostname, port = location.port || '80';
    lock.innerHTML = `Settings are read-only from this address — only ${d.settings_from.map(esc).join(', ')} ` +
      'may change them (<code>[web] settings_from</code> in the config file). From another machine, open a tunnel: ' +
      `<code>ssh -L ${esc(port)}:127.0.0.1:${esc(port)} ${esc(host)}</code> and use <code>http://localhost:${esc(port)}/</code>.`;
  }
  const err = $('#set-error');
  err.hidden = !d.file_error;
  if (d.file_error) err.textContent = `The config file does not parse: ${d.file_error}`;
  $('#set-ro').innerHTML = Object.entries(d.readonly).map(([k, v]) =>
    `<dt>${esc(k)}</dt><dd>${esc(v) || '<span class="mut">(empty)</span>'}</dd>`).join('') ||
    '<dt>—</dt><dd class="mut">nothing beyond the form</dd>';
  $('#set-path').textContent = `config file: ${d.path} · start switches (--feed-gate, --feed-gate-mode, SKIM_*) win over it`;
  $('#set-baks').innerHTML = '<tr><th>saved</th><th>file</th><th></th></tr>' + (d.backups.length ? d.backups.map((b) =>
    `<tr><td>${esc(new Date(b.time * 1000).toISOString().replace('T', ' ').slice(0, 19))} UTC</td><td>${esc(b.name)}</td>` +
    `<td><button type="button" class="small" data-restore="${esc(b.name)}"${d.may_change ? '' : ' disabled'}>Restore</button></td></tr>`).join('')
    : '<tr><td colspan="3" class="mut">none yet — every save keeps the previous file</td></tr>');
  $('#set-msg').textContent = '';
  filterCheck();
  mycallShow();
  if (status) renderDcw(status.deepcw);
}

function formBody() {
  const f = $('#settings'), p = new URLSearchParams();
  for (const k of Object.keys(cfg.data.values)) {
    const inp = f.elements[k];
    if (!inp) continue;
    p.set(k, inp.type === 'checkbox' ? String(inp.checked) : inp.value.trim());
  }
  const dflt = f.elements['decode.engine'].value;
  const lines = [];
  for (const b of readBands()) {
    if (!b.name && !b.khz) continue;
    const hz = Math.round(parseFloat(b.khz.replace(/\s/g, '').replace(',', '.')) * 1000);
    const modes = [];
    if (b.cw) modes.push(b.engine === dflt ? 'cw' : `cw:${b.engine}`);
    if (b.rtty) modes.push('rtty');
    if (!b.on) modes.push('off');
    if (b.worker) modes.push(`@${b.worker}`);
    lines.push(`${b.name}=${Number.isFinite(hz) ? hz : b.khz}${modes.length ? ` ${modes.join(',')}` : ''}`);
  }
  p.set('bands', lines.join('\n'));
  return p;
}

async function post(url, body) {
  const r = await fetch(url, { method: 'POST', body, cache: 'no-store' });
  const j = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw new Error(j.error || `HTTP ${r.status}`);
  return j;
}

// After a restart: wait for a NEW process (uptime falls), then carry on.
async function waitRestart(what, newPort) {
  const ov = $('#overlay');
  $('#overlay-text').textContent = `${what} — restarting…`;
  $('#overlay-sub').textContent = '';
  ov.hidden = false;
  const base = newPort && String(newPort) !== (location.port || '80')
    ? `${location.protocol}//${location.hostname}:${newPort}/` : '';
  const old = status ? status.started_us : 0, t0 = Date.now();
  while (Date.now() - t0 < 60000) {
    await new Promise((r) => setTimeout(r, 1000));
    try {
      const d = await getJSON(`${base}status.json`);
      if (d.started_us !== old) {
        if (base) { location.href = base; return; }
        ov.hidden = true;
        renderStatus(d);
        if (tab === 'settings') tabs.settings.refresh(true);
        return;
      }
    } catch (e) { $('#overlay-sub').textContent = 'waiting for the skimmer…'; }
  }
  $('#overlay-text').textContent = 'The skimmer has not come back within 60 s.';
  $('#overlay-sub').innerHTML = 'Check its log on the machine; an earlier config can be restored under Settings once it answers.';
}

// Saving: every field checked first. A wrong one turns red with what is
// wrong under it (and the bar on top says how many); a refusal from the
// skimmer names its key ("[hub] port: …") or its band, and that field turns
// red the same way. An empty field whose hint is the default takes the hint
// (the skimmer reads a blank value as the default too), so a part that is
// not used — the hub, the RBN — never asks to be filled in.
const HINT_IS_DEFAULT = ['radio.host', 'radio.clock_ppm', 'feed.port', 'rbn.port', 'rbn.delay_s',
  'hub.port', 'status.http_port', 'filter.mycall_diff', 'record.minutes'];
const int = (lo, hi, what) => (v) => (/^\d+$/.test(v) && +v >= lo && +v <= hi ? '' : what);
const FIELD_CHECK = {
  'radio.clock_ppm': (v) => (/^[+-]?\d+([.,]\d+)?$/.test(v) && Math.abs(parseFloat(v.replace(',', '.'))) <= 200
    ? '' : 'ppm: a number, e.g. 3.81 or -1.2'),
  'feed.port': int(0, 65535, '0 (off) to 65535'),
  'rbn.port': int(1, 65535, '1 to 65535'),
  'rbn.delay_s': int(0, 99, 'whole seconds, 0 to 99'),
  'hub.port': int(0, 65535, '0 (no hub) to 65535'),
  'status.console_s': int(0, 86400, 'whole seconds, 0 = quiet'),
  'status.http_port': int(1, 65535, '1 to 65535'),
  'filter.mycall_diff': int(0, 3, '0 to 3'),
  'station.locator': (v) => (/^[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?$/.test(v) ? '' : 'a Maidenhead locator: JN18 or JN18EU'),
  'hub.name': (v) => (/^[A-Za-z0-9._-]+$/.test(v) ? '' : 'letters, digits, - _ . only'),
  'record.minutes': (v) => (/^\d+([.,]\d+)?$/.test(v) && parseFloat(v.replace(',', '.')) > 0 ? '' : 'minutes, more than 0'),
  'record.start_utc': (v) => (/^([01]?\d|2[0-3]):[0-5]\d$/.test(v) ? '' : 'HH:MM in UTC'),
};
function fieldBad(inp, msg) {
  const lab = inp.closest('label, .filter-row') || inp.parentNode;
  let note = lab.querySelector(':scope > .ferr');
  inp.classList.toggle('bad', !!msg);
  if (!msg) { if (note) note.remove(); return; }
  if (!note) { note = document.createElement('small'); note.className = 'ferr'; lab.appendChild(note); }
  note.textContent = msg;
}
function clearBad() {
  for (const n of document.querySelectorAll('#settings .ferr')) n.remove();
  for (const i of document.querySelectorAll('#settings .bad')) if (i.tagName === 'INPUT' || i.tagName === 'SELECT') i.classList.remove('bad');
}
// the form's own checks: the wrong fields, marked
function checkFields() {
  const f = $('#settings'), wrong = [];
  for (const [k, test] of Object.entries(FIELD_CHECK)) {
    const inp = f.elements[k];
    if (!inp) continue;
    const v = inp.value.trim(), msg = v === '' ? '' : test(v);
    fieldBad(inp, msg);
    if (msg) wrong.push(inp);
  }
  for (const tr of document.querySelectorAll('#band-rows tr')) {
    const name = tr.querySelector('[data-k=name]'), khz = tr.querySelector('[data-k=khz]');
    if (!name.value.trim() && !khz.value.trim()) continue;
    const kv = parseFloat(khz.value.replace(/\s/g, '').replace(',', '.'));
    const nm = name.value.trim() ? '' : 'a name';
    const km = Number.isFinite(kv) && kv > 0 ? '' : 'kHz';
    name.classList.toggle('bad', !!nm); khz.classList.toggle('bad', !!km);
    if (nm) wrong.push(name);
    if (km) wrong.push(khz);
  }
  return wrong;
}
// the skimmer's refusal names a key or a band: mark that field
function markRefusal(msg) {
  const f = $('#settings');
  const b = /band (\S+?):/.exec(msg);             // a band's line first ("band 20m: … [hub] port is 0")
  if (b) {
    const tr = [...document.querySelectorAll('#band-rows tr')]
      .find((r) => r.querySelector('[data-k=name]').value.trim() === b[1]);
    if (tr) { for (const i of tr.querySelectorAll('input:not([type=checkbox])')) i.classList.add('bad'); return tr; }
  }
  const k = /\[(\w+)\] (\w+)/.exec(msg);
  if (k && f.elements[`${k[1]}.${k[2]}`]) {
    const inp = f.elements[`${k[1]}.${k[2]}`];
    fieldBad(inp, msg.slice(k.index + k[0].length).replace(/^[:\s]+/, '') || 'refused');
    return inp;
  }
  return null;
}
function notSaved(text, first) {
  const box = $('#set-error');
  box.hidden = false;
  box.textContent = `Not saved: ${text}`;
  $('#set-msg').textContent = '';
  if (first) { first.scrollIntoView({ block: 'center', behavior: 'smooth' }); if (first.focus) first.focus({ preventScroll: true }); }
}
on('#settings', 'input', (e) => {                 // typing in a red field clears it
  const t = e.target;
  if (t.classList && t.classList.contains('bad') && t.name) fieldBad(t, '');
  else if (t.classList) t.classList.remove('bad');
});

on('#settings', 'submit', async (e) => {
  e.preventDefault();
  if (!cfg.data || !cfg.data.may_change) return;
  clearBad();
  $('#set-error').hidden = true;
  const wrong = checkFields();
  if (!filterCheck()) wrong.push($('#filter-call'));      // its own note under it
  if (wrong.length) {
    notSaved(`${wrong.length === 1 ? 'one field is' : `${wrong.length} fields are`} not right — marked red below.`, wrong[0]);
    return;
  }
  const f = $('#settings');
  for (const k of HINT_IS_DEFAULT) {              // an empty field takes its hint
    const inp = f.elements[k];
    if (inp && !inp.value.trim() && inp.placeholder) inp.value = inp.placeholder;
  }
  const body = formBody();
  $('#set-msg').textContent = 'checking…';
  try {
    const j = await post('api/config', body);
    setDirty(false);                               // saved: leaving is fine now
    if (!j.changed) { $('#set-msg').textContent = 'nothing changed'; return; }
    const port = body.get('status.http_port');
    await waitRestart('Saved', port || '8073');
  } catch (err) {
    const at = markRefusal(err.message);
    notSaved(`${err.message}${at ? ' — marked red below' : ''}`, at && at.tagName === 'TR' ? at.querySelector('input') : at);
  }
});
on('#set-reset', 'click', () => tabs.settings.refresh(true));
on('#set-restart', 'click', async () => {
  if (cfg.dirty && !confirm('Restart without the unsaved changes in the form? They are lost.')) return;
  setDirty(false);
  try { await post('api/restart', new URLSearchParams()); await waitRestart('Restart'); }
  catch (err) { $('#set-msg').textContent = err.message; }
});
// Export: the file on disk as it is (what a restart would run), named after
// the day. Import: an exported file as the config — the skimmer keeps its own
// [web] settings_from, checks the file with the start's parser and restarts.
on('#set-export', 'click', async () => {
  try {
    const c = await getJSON('api/config');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([c.ini], { type: 'text/plain' }));
    a.download = `headless-${new Date().toISOString().slice(0, 10)}.ini`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  } catch (err) { $('#set-msg').textContent = `Not exported: ${err.message}`; }
});
on('#set-import', 'click', () => $('#set-import-file').click());
on('#set-import-file', 'change', async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file || !cfg.data || !cfg.data.may_change) return;
  if (cfg.dirty && !confirm('Discard the unsaved changes in the form?')) return;
  if (!confirm(`Use ${file.name} as the config and restart? The current file is kept as a backup ` +
               '(Earlier configs), and [web] settings_from stays as it is on this machine.')) return;
  try {
    const text = await file.text();
    const j = await post('api/config/import', new URLSearchParams({ ini: text }));
    if (!j.changed) { $('#set-msg').textContent = 'the imported file is the config already'; return; }
    const port = /^\s*http_port\s*=\s*(\d+)/m.exec(text);   // the page may move with it
    await waitRestart('Imported', port && port[1] !== '0' ? port[1] : '8073');
  } catch (err) {
    const box = $('#set-error');
    box.hidden = false;
    box.textContent = `Not imported: ${err.message}`;
  }
});

async function restore(name) {
  if (!confirm(`Put ${name} back as the config and restart?`)) return;
  try { await post('api/rollback', new URLSearchParams({ name })); await waitRestart('Restored'); }
  catch (err) { alert(`Not restored: ${err.message}`); }
}
on('#set-baks', 'click', (e) => { if (e.target.dataset.restore) restore(e.target.dataset.restore); });

// Skimmers on the LAN: a search, and what each found one can be used for here
async function findSkimmers() {
  $('#lan-msg').textContent = 'searching…';
  $('#lan-find').disabled = true;
  try {
    const d = await getJSON('api/skimmers');
    $('#lan-self').textContent = `${d.self.name}${d.self.call ? ` (${d.self.call})` : ''}`;
    const may = cfg.data && cfg.data.may_change;
    const peersIn = $('[name="peers.hosts"]');
    const have = new Set(peersIn.value.split(/[\s,]+/).filter(Boolean));
    $('#lan-list').innerHTML = '<tr><th>address</th><th class="l">name</th><th class="l">role</th><th class="l">call</th>' +
      '<th class="l">bands</th><th></th></tr>' + (d.found.length ? d.found.map((s, i) => {
        const feed = s.feed_port ? `${s.addr}:${s.feed_port}` : '';
        const bands = (s.bands || []).map((b) => `${b.name}${b.modes && b.modes !== 'cw' ? ` ${b.modes}` : ''}`).join(', ');
        return `<tr><td><a href="http://${esc(s.addr)}:${esc(s.http_port)}/" target="_blank" rel="noopener">${esc(s.addr)}:${esc(s.http_port)}</a></td>` +
          `<td class="l">${esc(s.name)}</td><td class="l">${esc(s.role)}</td><td class="l">${esc(s.call || '–')}</td>` +
          `<td class="l">${esc(bands)}</td><td>` +
          (feed ? `<button type="button" class="small" data-peer="${esc(feed)}"${may && !have.has(feed) ? '' : ' disabled'}>` +
                  `${have.has(feed) ? 'a peer already' : 'Add as peer'}</button>` : '<span class="mut">no feed</span>') +
          (s.hub_port && s.role !== 'worker'
            ? ` <button type="button" class="small" data-hub="${esc(`${s.addr}:${s.hub_port}`)}"${may ? '' : ' disabled'} ` +
              `title="this skimmer becomes its worker: it decodes the bands that hub assigns to ${esc(d.self.name)}">Use as hub</button>` : '') +
          '</td></tr>';
      }).join('') : '<tr><td colspan="6" class="mut">no other skimmer answered</td></tr>');
    $('#lan-msg').textContent = `${d.found.length} found`;
    cfg.lanWorkers = d.found.filter((s) => s.role === 'worker').map((s) => s.name);
    if (cfg.data) drawBands(readBands());
  } catch (err) {
    $('#lan-msg').textContent = err.message;
  } finally {
    $('#lan-find').disabled = false;
  }
}
on('#lan-find', 'click', findSkimmers);
on('#lan', 'toggle', () => { if ($('#lan').open) findSkimmers(); });
on('#lan-list', 'click', (e) => {
  const hub = e.target.dataset && e.target.dataset.hub;
  if (hub) {
    $('[name="radio.type"]').value = 'hub';
    $('[name="radio.host"]').value = hub;
    setDirty(true);
    e.target.disabled = true;
    e.target.textContent = 'set as the hub — Save to apply';
    return;
  }
  const peer = e.target.dataset && e.target.dataset.peer;
  if (!peer) return;
  const inp = $('[name="peers.hosts"]');
  inp.value = [...inp.value.split(/[\s,]+/).filter(Boolean), peer].join(', ');
  setDirty(true);
  e.target.disabled = true;
  e.target.textContent = 'added — Save to apply';
});

tabs.settings = {
  enter() { if (!cfg.dirty) this.refresh(true); },   // an edit not saved stays
  async refresh(force) {
    if (!force) return;                        // never overwrite the user's edits
    $('#set-error').hidden = true;
    fillSettings(await getJSON('api/config'));
  },
};

// A start that saved a new config and does not stream a minute later:
// offer the config it replaced.
let rbChecked = 0;
async function checkRollback(d) {
  const box = $('#rollback');
  if (d.streaming || d.uptime_s < 60) { box.hidden = true; return; }
  if (Date.now() - rbChecked < 15000) return;
  rbChecked = Date.now();
  try {
    const c = await getJSON('api/config');
    const b = c.backups[0];
    if (!b || b.time < c.started - 120) { box.hidden = true; return; }
    box.hidden = false;
    box.innerHTML = `The radio has not streamed since this start, right after the config was changed. ` +
      (c.may_change ? `<button type="button" class="small" id="rb-go">Restore the previous config</button> (${esc(b.name)})`
                    : `The previous config is ${esc(b.name)} — restore it under Settings from an allowed address.`);
    const go = $('#rb-go');
    if (go) go.addEventListener('click', () => restore(b.name));
  } catch (e) { /* the next poll tries again */ }
}

// ---- waterfall -------------------------------------------------------------------
// waterfall.js loads after this file (deferred): reach it when the tab opens.
tabs.waterfall = {
  enter() { if (typeof Wf !== 'undefined') Wf.enter(); else setTimeout(() => tab === 'waterfall' && Wf.enter(), 300); },
  leave() { if (typeof Wf !== 'undefined') Wf.leave(); },
};
// a hidden window watches nothing: the skimmer stops computing for it
document.addEventListener('visibilitychange', () => {
  if (tab !== 'waterfall' || typeof Wf === 'undefined') return;
  if (document.hidden) Wf.leave(); else Wf.enter();
});

// ---- log -------------------------------------------------------------------------
tabs.log = {
  enter() { this.refresh(); },
  async refresh() {
    const lines = await getJSON('api/log');
    const el = $('#log');
    const atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 8;
    el.innerHTML = lines.map((l) =>
      `<span class="${esc(l.level)}">${esc(l.t)} ${esc(l.text)}</span>`).join('\n');
    if (atEnd) el.scrollTop = el.scrollHeight;
    if (Date.now() - visitorsAt > 15000) { visitorsAt = Date.now(); renderVisitors(await getJSON('api/visitors')); }
  },
};
let visitorsAt = 0;
// the visitors' log for the managers: who is here now, the last visits, the refusals
function renderVisitors(v) {
  $('#visitors-box').hidden = !!v.off;
  if (v.off) return;
  const t = (s) => esc(String(s || '').replace('T', ' ').replace('Z', ''));
  const freqs = (f) => esc(f || '').replace(/,/g, ', ').replace(/=(\d+)/g, ' $1 s');
  const dur = (s) => { s = Number(s) || 0; return s >= 3600 ? `${Math.floor(s / 3600)} h ${Math.floor(s / 60) % 60} min` : s >= 60 ? `${Math.floor(s / 60)} min ${s % 60} s` : `${s} s`; };
  const addr = (r) => `${esc(r.addr)}${r.fwd ? ` <span class="mut">for ${esc(r.fwd)}</span>` : ''}`;
  $('#visitors').innerHTML = '<tr><th class="l">start UTC</th><th>call</th><th>time</th><th class="l">address</th><th class="l">watched on the waterfall (10 s or more)</th></tr>' +
    (v.open || []).map((r) => `<tr class="ok"><td class="l">${t(r.start)} <b>now</b></td><td>${esc(r.call)}</td><td>${dur(r.secs)}</td><td class="l">${addr(r)}</td><td class="l">${freqs(r.freqs)}</td></tr>`).join('') +
    (v.visits || []).map((r) => `<tr><td class="l">${t(r.start)}</td><td>${esc(r.call)}</td><td>${dur(r.secs)}</td><td class="l">${addr(r)}</td><td class="l">${freqs(r.freqs)}</td></tr>`).join('') +
    (!(v.open || []).length && !(v.visits || []).length ? '<tr><td colspan="5" class="mut">no visitor yet</td></tr>' : '');
  $('#visitors-failed').innerHTML = '<tr><th class="l">time UTC</th><th class="l">address</th><th class="l">typed</th><th class="l">why</th></tr>' +
    (v.failed || []).map((r) => `<tr><td class="l">${t(r.time)}</td><td class="l">${addr(r)}</td><td class="l">${esc(r.typed)}</td><td class="l">${esc(r.why)}</td></tr>`).join('') +
    (!(v.failed || []).length ? '<tr><td colspan="4" class="mut">none</td></tr>' : '');
}

// ---- the loop --------------------------------------------------------------------
async function poll() {
  if (visitor.need) return;                     // nothing until the visitor signs in
  try {
    await tabs.status.refresh();
    checkRollback(status);
    if (tab !== 'status' && tabs[tab] && tabs[tab].refresh) await tabs[tab].refresh();
  } catch (e) {
    if (visitor.need) return;
    $('#radio .dot').className = 'dot bad';
    $('#radio-text').textContent = `no answer from the skimmer (${e.message})`;
  }
}

let saved = 'status';
try { saved = localStorage.getItem('skimmer-tab') || 'status'; } catch (e) { /* private window */ }
show(document.getElementById(`tab-${saved}`) ? saved : 'status');
visitorCheck().then(poll);
setInterval(poll, 2000);
