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

let status = null;
function renderStatus(d) {
  status = d;
  $('#version').textContent = d.version ? `headless ${d.version}` : '';
  const dot = $('#radio .dot');
  dot.className = `dot ${d.streaming ? 'ok' : 'bad'}`;
  $('#radio-text').textContent = d.radio;
  const spots = d.bands.reduce((a, b) => a + b.spots, 0);
  $('#tiles').innerHTML = [
    tile('uptime', uptime(d.uptime_s)),
    tile('CPU', fmt(d.cpu_pct), '% of a core'),
    tile('packets', fmt(d.packets_per_s), '/s'),
    tile('lost', fmt(d.lost_pct, 3), '%'),
    tile('feed', d.feed_port ? fmt(d.feed_clients) : 'off', d.feed_port ? `client${d.feed_clients === 1 ? '' : 's'} · :${d.feed_port}` : ''),
    tile('spots', fmt(spots), `${fmt(d.feed_lines)} line${d.feed_lines === 1 ? '' : 's'} to clients`),
    tile('MASTER.SCP', fmt(d.dict_calls), 'calls'),
  ].join('');

  const head = '<tr><th>band</th><th>centre kHz</th><th>engine</th><th>decoders</th><th>active</th><th>stations</th><th>spots</th><th>drops</th><th>kS/s</th></tr>';
  $('#bands').innerHTML = head + d.bands.map((b) =>
    `<tr><td>${esc(b.band)}</td><td>${fmt(b.centre_hz / 1000, 1)}</td><td>${esc(b.engine || d.engine)}</td>` +
    `<td>${fmt(b.decoders)}</td><td>${fmt(b.active)}</td><td>${fmt(b.stations)}</td>` +
    `<td>${fmt(b.spots)}</td><td>${fmt(b.drops)}</td><td>${fmt(b.ksps)}</td></tr>`).join('');

  $('#recent').innerHTML = d.bands.map((b) => {
    const rows = b.recent.length ? b.recent.map((s) =>
      `<tr><td${s.cq ? ' class="cq"' : ''}>${esc(s.call)}</td><td>${fmt(s.freq_hz / 1000, 1)}</td>` +
      `<td>${fmt(s.wpm)}</td><td>${fmt(s.snr_db)}</td><td>${age(s.age_s)}</td></tr>`).join('')
      : '<tr><td colspan="5" class="mut">nothing yet</td></tr>';
    return `<div class="card"><h2>${esc(b.band)} <span class="mut">· ${fmt(b.stations)} station${b.stations === 1 ? '' : 's'}</span></h2>` +
      `<table><tr><th>call</th><th>kHz</th><th>wpm</th><th>dB</th><th>heard</th></tr>${rows}</table></div>`;
  }).join('');

  const p = d.feed_policy, g = d.feed_gate;
  const sw = Object.entries(d.switches || {});
  $('#policy').innerHTML = '<dl class="kv">' +
    `<dt>feed policy</dt><dd>score ≥ ${fmt(p.min_score, 2)}, read ${p.min_hearings}×, settle ${fmt(p.settle_s)} s ` +
    `(unknown calls ${fmt(p.settle_nodict_s)} s), fresh ${fmt(p.fresh_s)} s, fold ${fmt(p.fold_s)} s — ${fmt(p.folded)} folded</dd>` +
    `<dt>learned gate</dt><dd>${g ? `${esc(g.id)} · ${esc(g.mode)} · threshold ${fmt(g.threshold, 2)} · ` +
      `${fmt(g.below)} of ${fmt(g.spots)} below` : 'off'}</dd>` +
    `<dt>switches</dt><dd>${sw.length ? sw.map(([k, v]) => `${esc(k)}=${esc(v)}`).join('<br>') : 'none'}</dd>` +
    '</dl>';
  $('#updated').textContent = `updated ${d.time}`;
}

tabs.status = {
  async refresh() { renderStatus(await getJSON('status.json')); },
};

// ---- statistics ------------------------------------------------------------------
const stats = { hours: 24, mode: 'cw', data: null, at: 0 };
function sum(a) { return a.reduce((x, v) => x + (v || 0), 0); }
function mean(a) { const v = a.filter((x) => x != null); return v.length ? sum(v) / v.length : null; }

function renderStats() {
  const d = stats.data;
  if (!d) return;
  const m = d[stats.mode] || d.cw;
  const t = Array.from({ length: d.n }, (_, i) => d.t0 + i * d.step_min * 60);
  const stepH = d.step_min / 60;
  const label = (i) => {
    const a = new Date(t[i] * 1000), b = new Date((t[i] + d.step_min * 60) * 1000);
    const f = (x) => `${String(x.getUTCHours()).padStart(2, '0')}:${String(x.getUTCMinutes()).padStart(2, '0')}`;
    return `${a.getUTCDate()}.${a.getUTCMonth() + 1}. ${f(a)}–${f(b)} UTC`;
  };
  const x = { t, spanH: stats.hours, label };
  $('#mode').hidden = !d.rtty;
  $('#stats-note').textContent = `${d.step_min}-minute buckets · times in UTC`;

  // colour follows the band (config order), never its rank
  const series = d.bands.map((b, i) => ({
    name: b, color: Charts.seriesColor(i),
    values: m.spots[b].map((v) => v == null ? null : v / stepH),   // per hour
  }));
  $('#legend-spots').replaceChildren(...series.map((s) => {
    const sp = document.createElement('span'); const i = document.createElement('i');
    i.style.background = s.color; sp.append(i, document.createTextNode(s.name)); return sp;
  }));
  const loading = (id, on) => $(id).classList.toggle('loading', on);
  Charts.stacked($('#chart-spots'), x, series, (v) => fmt(v, v < 10 && v % 1 ? 1 : 0));
  Charts.heat($('#chart-active'), x, d.bands.map((b) => ({ name: b, values: m.active[b] })), (v) => fmt(v, 1));
  Charts.line($('#chart-cpu'), x, d.global.cpu, Charts.seriesColor(0), (v) => `${fmt(v)} %`, 'CPU, % of a core');
  Charts.line($('#chart-lost'), x, d.global.lost, Charts.seriesColor(1), (v) => `${fmt(v, 3)} %`, 'lost, worst minute');
  ['#chart-spots', '#chart-active', '#chart-cpu', '#chart-lost'].forEach((id) => loading(id, false));

  const totSpots = d.bands.reduce((a, b) => a + sum(m.spots[b]), 0);
  const streamed = mean(d.global.streaming);
  $('#stats-tiles').innerHTML = [
    tile('spots', fmt(totSpots), `in ${stats.hours} h`),
    tile('stations appeared', fmt(d.bands.reduce((a, b) => a + sum(m.stations[b]), 0))),
    tile('folded', fmt(d.bands.reduce((a, b) => a + sum(m.folded[b]), 0)), 'twins held back'),
    tile('gate below', fmt(d.bands.reduce((a, b) => a + sum(m.gate_below[b]), 0)), 'learned gate'),
    tile('radio streamed', streamed == null ? '–' : fmt(100 * streamed),
         streamed == null ? 'no data' : '% of the recorded time'),
  ].join('');

  const head = '<tr><th>band</th><th>spots</th><th>spots / h</th><th>stations appeared</th><th>active (mean)</th>' +
    '<th>folded</th><th>gate below</th><th>SNR (mean of medians)</th></tr>';
  const hrs = (d.n * d.step_min) / 60;
  $('#stats-table').innerHTML = head + d.bands.map((b) => {
    const sp = sum(m.spots[b]);
    return `<tr><td>${esc(b)}</td><td>${fmt(sp)}</td><td>${fmt(sp / hrs, 1)}</td><td>${fmt(sum(m.stations[b]))}</td>` +
      `<td>${fmt(mean(m.active[b]), 1)}</td><td>${fmt(sum(m.folded[b]))}</td><td>${fmt(sum(m.gate_below[b]))}</td>` +
      `<td>${mean(m.snr[b]) == null ? '–' : `${fmt(mean(m.snr[b]))} dB`}</td></tr>`;
  }).join('');
}

tabs.stats = {
  enter() { this.refresh(true); },
  async refresh(force) {
    // history moves by the minute: refetch at most every 30 s
    if (!force && Date.now() - stats.at < 30000) return;
    ['#chart-spots', '#chart-active', '#chart-cpu', '#chart-lost'].forEach((id) => $(id).classList.add('loading'));
    stats.data = await getJSON(`stats.json?hours=${stats.hours}`);
    stats.at = Date.now();
    renderStats();
  },
};
for (const b of document.querySelectorAll('#range button')) {
  b.addEventListener('click', () => {
    stats.hours = Number(b.dataset.h);
    for (const o of document.querySelectorAll('#range button')) o.classList.toggle('on', o === b);
    tabs.stats.refresh(true);
  });
}
for (const b of document.querySelectorAll('#mode button')) {
  b.addEventListener('click', () => {
    stats.mode = b.dataset.m;
    for (const o of document.querySelectorAll('#mode button')) o.classList.toggle('on', o === b);
    renderStats();
  });
}
let resizeT = 0;
window.addEventListener('resize', () => {
  clearTimeout(resizeT);
  resizeT = setTimeout(() => { if (tab === 'stats') renderStats(); }, 150);
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
  },
};

// ---- the loop --------------------------------------------------------------------
async function poll() {
  try {
    await tabs.status.refresh();
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
