// skimmer-headless web page — status, statistics, settings, log.
// Plain JS, no libraries: the page is served by the skimmer itself and must
// work on a LAN with no internet. Everything it shows comes from JSON
// (/status.json, /stats.json, /api/log, /api/config).
'use strict';

const $ = (sel) => document.querySelector(sel);
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
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

// ---- tabs ------------------------------------------------------------------------
const tabs = {};
let tab = 'status';
function show(name) {
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

// The bands as decoded: a band a worker decodes ("worker") is shown with
// that worker's own numbers (its status comes with the hub's), or as waiting.
function bandsDecoded(d) {
  const workers = (d.hub && d.hub.workers) || [];
  return d.bands.map((b) => {
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

let status = null;
function renderStatus(d) {
  status = d;
  $('#call').textContent = d.call || 'SKIMMER';
  $('#version').textContent = d.locator || (d.version ? `v${d.version}` : '');
  $('#hdr-stats').innerHTML = `<span>CPU <b>${fmt(d.cpu_pct)}%</b></span> <span>lost <b>${fmt(d.lost_pct, 3)}%</b></span> ` +
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
  $('#ladder').innerHTML = decoded.map((b) => {
    const sp = per(b.band), cwOn = b.cw !== false, rt = b.rtty;
    const bar = (on, act, dec, cls) => on
      ? `<td class="bar ${cls}" title="${fmt(act)} of ${fmt(dec)} decoders active"><i style="width:${(100 * act / maxAct).toFixed(0)}%"></i></td><td class="n">${fmt(act)}</td>`
      : '<td class="bar off"></td><td class="n mut">–</td>';
    return `<tr><td class="b" style="color:${typeof SkMap !== 'undefined' ? SkMap.bandColor(b.band) : ''}">${esc(b.band)}</td>` +
      bar(cwOn, b.active, b.decoders, 'cw') + bar(!!rt, rt ? rt.active : 0, rt ? rt.decoders : 0, 'rtty') +
      `<td class="n">${fmt(sp.length)}</td><td class="last">${sp.length ? esc(sp[0].call) : ''}</td>` +
      `<td class="w mut">${b.worker ? `${b.waiting ? 'waiting for ' : 'on '}${esc(b.worker)}` : ''}</td></tr>`;
  }).join('') + '<tr class="legend-row"><td></td><td colspan="2" class="mut">CW</td><td colspan="2" class="mut">RTTY</td><td class="mut n">spots</td><td colspan="2"></td></tr>';
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
  if (tab === 'stats') renderDecTile(d);
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
const spots = { raw: [], list: [], at: 0, skew: 0, locator: '' };
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
  spots.skew = d.now - Date.now() / 1000;
  spots.raw = d.spots;
  ageSpots(true);
}
// The DX cluster filters the spots; the map draws what the filters leave, the
// row clicked marked on it.
function renderSpots() {
  if (typeof SkMap === 'undefined' || typeof Dxc === 'undefined') return;
  if (!mapMounted) {
    SkMap.mount($('#skmap'));
    Dxc.mount($('#dxc'), (shown, marked) =>
      SkMap.update(shown.map((s) => ({ ...s, hi: s.call === marked })), spots.locator || 'JO60'));
    mapMounted = true;
    SkMap.update([], spots.locator || 'JO60');              // the centre before the rows
  }
  $('#loc-warn').hidden = !!spots.locator;
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
function sum(a) { return a.reduce((x, v) => x + (v || 0), 0); }
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
  const x = { t, spanH: stats.hours, label };
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
    [{ name: 'CW', series }, ...(d.rtty ? [{ name: 'RTTY', series: perHour(d.rtty) }] : [])],
    (v) => fmt(v, v < 10 && v % 1 ? 1 : 0));
  // a band's block: CW over RTTY (a mode the band does not decode: an empty strip)
  const decoded = (mm, b) => mm && mm.active[b] && mm.active[b].some((v) => v != null) ? mm.active[b] : null;
  const amax = Charts.heat($('#chart-active'), x, d.bands.map((b) => ({ name: b, parts: [
    { label: 'CW', values: decoded(d.cw, b), lo: '--seq-lo', hi: '--seq-hi' },
    ...(d.rtty ? [{ label: 'RTTY', values: decoded(d.rtty, b), lo: '--seq2-lo', hi: '--seq2-hi' }] : []),
  ] })), (v) => fmt(v, 1), 'active decoders');
  $('#legend-active').innerHTML = [['CW', '--seq'], ...(d.rtty ? [['RTTY', '--seq2']] : [])].map(([l, v]) =>
    `<span><i class="ramp" style="background:linear-gradient(90deg,var(${v}-lo),var(${v}-hi))"></i>` +
    `${l}${amax[l] ? ` <span class="mut">0–${fmt(amax[l], amax[l] < 10 ? 1 : 0)}</span>` : ''}</span>`).join('');
  // CPU: the CW and RTTY pipelines' threads and the rest of the process, stacked
  // under the total (history before the split has the total line only)
  const g = d.global, cpuCw = g.cpu_cw || g.cpu.map(() => null), cpuRt = g.cpu_rtty || g.cpu.map(() => null);
  const anyRt = cpuRt.some((v) => v != null);
  const layers = [
    { name: 'CW decoders', color: Charts.seriesColor(0), values: cpuCw },
    ...(anyRt ? [{ name: 'RTTY decoders', color: Charts.seriesColor(1), values: cpuRt }] : []),
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
  ['#chart-spots', '#chart-active', '#chart-cpu'].forEach((id) => loading(id, false));

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
  const stack = (b, f) => modes.map(([l, mm]) =>
    `<div class="ml ${l.toLowerCase()}">${has(mm, b) ? f(mm) : '–'}</div>`).join('');
  // the columns, each with what it counts (the note under the table too)
  const cols = [
    ['spots', 'spots sent to the telnet feed in the range', (b, mm) => fmt(sum(mm.spots[b]))],
    ['spots per hour', 'the spots divided by the hours of the range', (b, mm) => fmt(sum(mm.spots[b]) / hrs, 1)],
    ['calls decoded', 'calls that came into the band\'s station list — read and validated, spotted or not',
      (b, mm) => fmt(sum(mm.stations[b]))],
    ['busy decoders', 'channel decoders reading running text, on average over the range', (b, mm) => fmt(mean(mm.active[b]), 1)],
    ['SNR', 'the typical SNR of the spots: the median of each minute\'s spots, averaged',
      (b, mm) => (mean(mm.snr[b]) == null ? '–' : `${fmt(mean(mm.snr[b]))} dB`)],
  ];
  $('#stats-table').innerHTML = '<tr><th>band</th><th class="l">mode</th>' +
    `${cols.map(([n, t]) => `<th title="${esc(t)}">${n}</th>`).join('')}</tr>` +
    d.bands.map((b) => `<tr><td>${esc(b)}</td>` +
      `<td class="l">${modes.map(([l]) => `<div class="ml ${l.toLowerCase()}">${l}</div>`).join('')}</td>` +
      cols.map(([, , f]) => `<td>${stack(b, (mm) => f(b, mm))}</td>`).join('') + '</tr>').join('');
  $('#stats-cols').innerHTML = cols.map(([n, t]) => `<b>${n}</b> = ${esc(t)}`).join(' · ');
}

tabs.stats = {
  enter() { this.refresh(true); },
  async refresh(force) {
    // history moves by the minute: refetch at most every 30 s
    if (!force && (stats.end || Date.now() - stats.at < 30000)) return;   // a past window stays
    ['#chart-spots', '#chart-active', '#chart-cpu'].forEach((id) => $(id).classList.add('loading'));
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
  const W = Math.max(200, box.clientWidth), H = 40;
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
  box.innerHTML = `<svg viewBox="0 0 ${W} ${H}" height="${H}" preserveAspectRatio="none">` +
    `<path d="${bars}" fill="var(--axis)"/>` +
    `<rect x="0" y="0" width="${a.toFixed(1)}" height="${H}" fill="var(--panel)" opacity=".55"/>` +
    `<rect x="${b.toFixed(1)}" y="0" width="${(W - b).toFixed(1)}" height="${H}" fill="var(--panel)" opacity=".55"/>` +
    `<rect class="win" x="${a.toFixed(1)}" y="0.5" width="${Math.max(2, b - a).toFixed(1)}" height="${H - 1}"/>` +
    `${o ? '' : `<text x="${W / 2}" y="${H / 2 + 4}" text-anchor="middle" font-size="11" fill="var(--tick)">loading the history…</text>`}</svg>`;
}
function timeLabel(offH) {
  const nowS = Math.floor(Date.now() / 1000), end = nowS - offH * 3600, start = end - stats.hours * 3600;
  const f = (t) => { const x = new Date(t * 1000); return `${x.getUTCDate()}.${x.getUTCMonth() + 1}.${stats.hours > 2160 ? x.getUTCFullYear() : ''} ${String(x.getUTCHours()).padStart(2, '0')}:00`; };
  $('#t-label').textContent = offH ? `${f(start)} – ${f(end)} UTC` : `the last ${stats.hours >= 48 ? `${stats.hours / 24} days` : `${stats.hours} h`}, up to now`;
  $('#t-now').disabled = !offH;
}
$('#t-end').addEventListener('input', (e) => { timeLabel(-Number(e.target.value)); drawOver(); });
$('#t-end').addEventListener('change', (e) => {
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
$('#t-over').addEventListener('pointerdown', (e) => {
  over.drag = true;
  e.currentTarget.setPointerCapture(e.pointerId);
  overPick(e);
});
$('#t-over').addEventListener('pointermove', (e) => { if (over.drag) overPick(e); });
const overDone = () => {
  if (!over.drag) return;
  over.drag = false;
  $('#t-end').dispatchEvent(new Event('change'));
};
$('#t-over').addEventListener('pointerup', overDone);
$('#t-over').addEventListener('pointercancel', overDone);
$('#t-now').addEventListener('click', () => { stats.end = null; tabs.stats.refresh(true); });

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
const MAX_BANDS = 8;          // receivers: the bands on
const MAX_LISTED = 16;        // [bands] lines, the ones off included

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
$('#band-rows').addEventListener('change', (e) => {
  if (e.target.dataset && e.target.dataset.k === 'on') drawBands(readBands());
});
$('#band-rows').addEventListener('click', (e) => {
  const a = e.target.dataset && e.target.dataset.a;
  if (!a) return;
  const list = readBands(), i = [...$('#band-rows').children].indexOf(e.target.closest('tr'));
  if (a === 'del') list.splice(i, 1);
  if (a === 'up' && i > 0) [list[i - 1], list[i]] = [list[i], list[i - 1]];
  if (a === 'down' && i < list.length - 1) [list[i + 1], list[i]] = [list[i], list[i + 1]];
  drawBands(list);
  cfg.dirty = true;
});
$('#band-add').addEventListener('click', () => {
  const list = readBands();
  const on = list.filter((b) => b.on).length < MAX_BANDS;   // a ninth line starts off
  list.push({ name: '', khz: '', cw: true, engine: $('[name="decode.engine"]').value || 'v3', rtty: false, on });
  drawBands(list);
  cfg.dirty = true;
});
$('#settings').addEventListener('input', () => { cfg.dirty = true; });

function fillSettings(d) {
  cfg.data = d;
  cfg.dirty = false;
  const f = $('#settings');
  for (const [k, v] of Object.entries(d.values)) {
    const inp = f.elements[k];
    if (!inp) continue;
    if (inp.type === 'checkbox') inp.checked = v === '' ? inp.dataset.default === 'true' : v === 'true' || v === '1';
    else if (inp.tagName === 'SELECT') inp.value = v || inp.dataset.default || inp.options[0].value;
    else inp.value = v;
  }
  for (const inp of f.querySelectorAll('input, select, button')) inp.disabled = !d.may_change;
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

$('#settings').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (!cfg.data || !cfg.data.may_change) return;
  const body = formBody();
  const delay = body.get('rbn.delay_s');
  if (delay !== '' && !(/^\d+$/.test(delay) && Number(delay) <= 99)) {
    const box = $('#set-error');
    box.hidden = false;
    box.textContent = 'Not saved: the RBN delay is whole seconds, 0 to 99.';
    return;
  }
  $('#set-msg').textContent = 'checking…';
  try {
    const j = await post('api/config', body);
    if (!j.changed) { $('#set-msg').textContent = 'nothing changed'; cfg.dirty = false; return; }
    const port = body.get('status.http_port');
    await waitRestart('Saved', port || '8073');
  } catch (err) {
    $('#set-msg').textContent = '';
    const box = $('#set-error');
    box.hidden = false;
    box.textContent = `Not saved: ${err.message}`;
  }
});
$('#set-reset').addEventListener('click', () => tabs.settings.refresh(true));
$('#set-restart').addEventListener('click', async () => {
  try { await post('api/restart', new URLSearchParams()); await waitRestart('Restart'); }
  catch (err) { $('#set-msg').textContent = err.message; }
});
// Export: the file on disk as it is (what a restart would run), named after
// the day. Import: an exported file as the config — the skimmer keeps its own
// [web] settings_from, checks the file with the start's parser and restarts.
$('#set-export').addEventListener('click', async () => {
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
$('#set-import').addEventListener('click', () => $('#set-import-file').click());
$('#set-import-file').addEventListener('change', async (e) => {
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
$('#set-baks').addEventListener('click', (e) => { if (e.target.dataset.restore) restore(e.target.dataset.restore); });

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
$('#lan-find').addEventListener('click', findSkimmers);
$('#lan').addEventListener('toggle', () => { if ($('#lan').open) findSkimmers(); });
$('#lan-list').addEventListener('click', (e) => {
  const hub = e.target.dataset && e.target.dataset.hub;
  if (hub) {
    $('[name="radio.type"]').value = 'hub';
    $('[name="radio.host"]').value = hub;
    cfg.dirty = true;
    e.target.disabled = true;
    e.target.textContent = 'set as the hub — Save to apply';
    return;
  }
  const peer = e.target.dataset && e.target.dataset.peer;
  if (!peer) return;
  const inp = $('[name="peers.hosts"]');
  inp.value = [...inp.value.split(/[\s,]+/).filter(Boolean), peer].join(', ');
  cfg.dirty = true;
  e.target.disabled = true;
  e.target.textContent = 'added — Save to apply';
});

tabs.settings = {
  enter() { this.refresh(true); },
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
  },
};

// ---- the loop --------------------------------------------------------------------
async function poll() {
  try {
    await tabs.status.refresh();
    checkRollback(status);
    if (tab !== 'status' && tabs[tab] && tabs[tab].refresh) await tabs[tab].refresh();
  } catch (e) {
    $('#radio .dot').className = 'dot bad';
    $('#radio-text').textContent = `no answer from the skimmer (${e.message})`;
  }
}

let saved = 'status';
try { saved = localStorage.getItem('skimmer-tab') || 'status'; } catch (e) { /* private window */ }
show(document.getElementById(`tab-${saved}`) ? saved : 'status');
poll();
setInterval(poll, 2000);
