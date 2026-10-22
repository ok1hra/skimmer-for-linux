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
