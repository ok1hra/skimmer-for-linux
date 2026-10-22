// app.js — the page for compare-web.py. Plain DOM + hand-drawn SVG; every
// string from the data goes in through textContent, never innerHTML.
'use strict';

const $ = (s) => document.querySelector(s);
const SVGNS = 'http://www.w3.org/2000/svg';
let W = 720;      // chart width in CSS px: set from the page on every render

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  setAttrs(e, attrs);
  for (const k of kids.flat()) {
    if (k == null || k === false) continue;
    e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
  return e;
}
function sv(tag, attrs, text) {
  const e = document.createElementNS(SVGNS, tag);
  setAttrs(e, attrs);
  if (text != null) e.textContent = String(text);
  return e;
}
function setAttrs(e, attrs) {
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k === 'cls' ? 'class' : k, v === true ? '' : v);
  }
}
function svgRoot(h) {
  return sv('svg', { viewBox: `0 0 ${W} ${h}`, role: 'img' });
}

// ---- formatting ---------------------------------------------------------------
const pct = (p) => (p == null ? '—' : (100 * p).toFixed(1) + ' %');
const pctShort = (p) => (p == null ? '—' : Math.round(100 * p) + '%');
const fmtP = (p) => (p == null ? '—' : p < 0.001 ? 'p < 0.001' : 'p = ' + p.toPrecision(2));
const iso = (t) => new Date(t * 1000).toISOString();
const utc = (t) => iso(t).slice(5, 16).replace('T', ' ');
const hhmm = (t) => iso(t).slice(11, 16);
const hms = (t) => iso(t).slice(11, 19);
function dur(s) {
  s = Math.round(s || 0);
  if (s < 3600) return Math.round(s / 60) + ' min';
  const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
  return h >= 48 ? (s / 86400).toFixed(1) + ' d' : `${h} h ${m} min`;
}
const signed = (v, unit) => (v == null ? '—' : (v > 0 ? '+' : v < 0 ? '−' : '±') + Math.abs(v) + unit);
const CAT = {
  'match': 'match', 'only-L': 'only L', 'only-R': 'only R', 'nonCQ': 'R heard, not CQ',
  'bust': 'bust pair',
};
const VERD = { rbn: 'RBN', scp: 'SCP', none: 'unverified', agree: 'both agree' };

// ---- tooltip ------------------------------------------------------------------
const tip = $('#tip');
function tipShow(ev, head, rows) {
  tip.replaceChildren(el('div', { cls: 'h' }, head));
  for (const [color, value, label] of rows) {
    tip.append(el('div', { cls: 'r' },
      color ? el('span', { cls: 'key', style: `background:${color}` }) : null,
      el('b', null, value), el('span', null, label)));
  }
  tip.hidden = false;
  let x = ev.clientX, y = ev.clientY;
  if (x == null) { const r = ev.target.getBoundingClientRect(); x = r.left + r.width / 2; y = r.top; }
  const w = tip.offsetWidth, h = tip.offsetHeight;
  tip.style.left = Math.max(8, Math.min(window.innerWidth - w - 8, x + 14)) + 'px';
  tip.style.top = Math.max(8, Math.min(window.innerHeight - h - 8, y + 14)) + 'px';
}
function tipHide() { tip.hidden = true; }
function hover(node, fn) {
  node.setAttribute('tabindex', '0');
  const show = (e) => { const [h, r] = fn(); tipShow(e, h, r); };
  node.addEventListener('pointermove', show);
  node.addEventListener('focus', show);
  node.addEventListener('pointerleave', tipHide);
  node.addEventListener('blur', tipHide);
}
const cL = 'var(--L)', cR = 'var(--R)';

// ---- shapes ---------------------------------------------------------------------
// a bar with a 4px rounded data end, square at the baseline (up or down)
function bar(x, base, w, h, color, down) {
  if (h <= 0.5) return null;
  const r = Math.min(4, w / 2, h);
  const d = down
    ? `M${x},${base}V${base + h - r}Q${x},${base + h} ${x + r},${base + h}H${x + w - r}Q${x + w},${base + h} ${x + w},${base + h - r}V${base}Z`
    : `M${x},${base}V${base - h + r}Q${x},${base - h} ${x + r},${base - h}H${x + w - r}Q${x + w},${base - h} ${x + w},${base - h + r}V${base}Z`;
  return sv('path', { d, fill: color });
}
function yGrid(g, m, H, y, ticks, fmt) {
  for (const v of ticks) {
    g.append(sv('line', { cls: 'grid', x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }));
    g.append(sv('text', { x: m.l - 6, y: y(v) + 4, 'text-anchor': 'end' }, fmt(v)));
  }
}
function niceMax(v) {
  if (v <= 0) return 1;
  const e = Math.pow(10, Math.floor(Math.log10(v)));
  for (const k of [1, 2, 2.5, 5, 10]) if (k * e >= v) return k * e;
  return 10 * e;
}

// ---- state + URL --------------------------------------------------------------------
const url = new URLSearchParams(location.search);
const st = {
  S: null, E: [], shown: 200, open: new Set(), last: 0, took: 0, timer: null, inflight: false,
  f: { band: url.get('band') || '', cat: url.get('cat') || 'diff', v: url.get('v') || '',
       call: url.get('call') || '' },
  hourUnit: url.get('hunit') === 'calls' ? 'calls' : 'ep',
};
function apiQuery() {
  const q = new URLSearchParams();
  for (const [k, v] of url) if (!['band', 'cat', 'v', 'call', 'nolive'].includes(k)) q.set(k, v);
  return q.toString();
}
function pushUrl() {
  const q = new URLSearchParams(url);
  for (const [k, v] of Object.entries(st.f)) {
    if (v && !(k === 'cat' && v === 'diff')) q.set(k, v); else q.delete(k);
  }
  history.replaceState(null, '', '?' + q.toString());
}

async function refresh() {
  if (st.inflight) return;
  st.inflight = true;
  $('#main').classList.add('busy');
  const t0 = Date.now();
  try {
    const q = apiQuery();
    const [S, E] = await Promise.all([
      fetch('/api/summary?' + q).then((r) => r.json()),
      fetch('/api/events?' + q).then((r) => r.json()),
    ]);
    if (S.error || E.error) throw new Error(S.error || E.error);
    st.S = S; st.E = E.events;
    st.last = Date.now();
    st.took = st.last - t0;
    render();
  } catch (e) {
    const S = st.S;
    $('#updated').textContent = (S ? `${S.range.label} · updated ${hms(S.now)} UTC · ` : '') +
      'last update failed: ' + e.message;
  } finally {
    st.inflight = false;
    $('#main').classList.remove('busy');
  }
}
// New data arrives every few seconds (RBN), but the numbers move slowly:
// redraw at most once a minute, and a summary that takes long to compute
// (a long range) waits 15× as long as it took. A hidden tab waits until it is
// shown again, and a range that ended in the past never changes.
const REFRESH_MIN = 60000;
function every() { return Math.max(REFRESH_MIN, 15 * (st.took || 0)); }
function closed() { return st.S && st.S.range.to < st.S.now - 120; }
function soon() {
  if (st.timer || document.hidden || closed()) return;
  const wait = Math.max(0, every() - (Date.now() - st.last));
  st.timer = setTimeout(() => { st.timer = null; if (!document.hidden) refresh(); }, wait);
}
document.addEventListener('visibilitychange', () => { if (!document.hidden && st.last) soon(); });

// ---- render -------------------------------------------------------------------------
function engLabel(S, key) {
  const e = S.engines.find((x) => x.key === key);
  return e ? e.label : key;
}

function render() {
  const S = st.S;
  W = Math.max(320, Math.round($('#ci').clientWidth || 720));
  renderStatus(S);
  renderAbout(S);
  renderEngineSelect(S);
  renderRange(S);
  renderParams(S);
  renderNotice(S);
  renderVerdict(S);
  renderDiffs();
  renderBands(S);
  renderRtty(S);
  renderTimeline(S);
  renderBins('#snr', S.snr.bins, ' dB');
  const curve = (S.snr.curve || []).map(([x, d]) => `${x}→${signed(d, '')}`).join(' ');
  $('#snr-title').textContent = `Recall by SNR (R's scale; L-only mapped by R − L at L's SNR: ${curve} dB, ${S.snr.offset_n} matched pairs)`;
  renderBins('#wpm', S.wpm.bins, ' WPM');
  renderLatency(S.latency);
  renderHists(S);
  renderSessions(S);
  $('#updated').textContent = `${S.range.label} · updated ${hms(S.now)} UTC · `
    + (closed() ? 'closed range, no auto-update' : `auto-update every ${Math.round(every() / 1000)} s`);
}

// The chosen comparison may have no compared time in the range — typically
// the local skimmer switched decoder and the URL still names the old one.
// Say so loudly instead of showing an empty page.
function renderNotice(S) {
  const box = $('#notice');
  const cur = S.overview.find((o) => o.key === S.engine);
  const others = S.overview.filter((o) => o.key !== S.engine && o.common_s > 0);
  if (!cur || cur.common_s > 0) { box.hidden = true; return; }
  box.replaceChildren(
    el('span', null, el('b', null, `Nothing compared for ${engLabel(S, S.engine)} in ${S.range.label}.`),
      others.length ? ' In this range the local skimmer ran:' : ' Pick a wider range.'),
    ...others.map((o) => el('button', { type: 'button', onclick: () => setEngine(o.key) },
      `${o.label} (${dur(o.common_s)})`)));
  box.hidden = false;
}

function renderStatus(S) {
  const box = $('#status');
  box.replaceChildren();
  const F = S.feeds || {};
  for (const [k, name] of [['local', 'L feed'], ['remote', 'R feed'], ['rbn', 'RBN']]) {
    const f = F[k];
    const cls = !f || !f.recorded ? 'off' : f.up ? 'up' : 'down';
    const txt = !f || !f.recorded ? 'not recorded' : f.up ? 'up' : 'down';
    box.append(el('span', { cls: 'pill', title: f && f.since ? 'since ' + utc(f.since) + ' UTC' : '' },
      el('span', { cls: 'dot ' + cls }), el('b', null, name), txt));
  }
  if (F.engine_label) box.append(el('span', { cls: 'pill' }, 'L decoder now ', el('b', null, F.engine_label)));
  if (F.session) box.append(el('span', { cls: 'pill' }, 'session ', el('b', null, F.session),
    F.stopped ? ' stopped' : ''));
}

function renderAbout(S) {
  const N = S.names, eng = S.engines.find((x) => x.key === S.engine);
  const who = $('#who');
  who.replaceChildren(el('span', { cls: 'key key-L' }), `L = ${N.local}, ${eng ? eng.label : S.engine}`,
    ' · ', el('span', { cls: 'key key-R' }), `R = ${N.remote}`);
  $('#sides').replaceChildren(
    el('div', { cls: 'side' },
      el('div', { cls: 'n' }, el('span', { cls: 'key key-L' }), `L — local: ${N.local}`),
      el('div', { cls: 'd' }, 'decoder: ', el('b', null, eng ? eng.label : S.engine),
        eng && eng.what ? ` (${eng.what})` : ''),
      el('div', { cls: 'a' }, N.local_about)),
    el('div', { cls: 'side' },
      el('div', { cls: 'n' }, el('span', { cls: 'key key-R' }), `R — remote: ${N.remote}`),
      el('div', { cls: 'a' }, N.remote_about)));

  const t = $('#overview');
  const head = ['Comparison', 'Compared', 'Stations', 'Recall L', 'Recall R', 'McNemar',
    'Bust L', 'Bust R', 'First spot R − L', 'Better'];
  t.replaceChildren(el('thead', null, el('tr', null, head.map((h, i) => el('th', { cls: i > 0 && i < 9 ? 'num' : '' }, h)))));
  const tb = el('tbody');
  for (const o of S.overview) {
    const wins = [['recall', o.recall.winner], ['precision', o.bust.winner], ['speed', o.latency.winner]]
      .filter(([, w]) => w).map(([m, w]) => `${w}: ${m}`);
    const tr = el('tr', { cls: o.key === S.engine ? 'sel' : '', tabindex: 0,
                          title: 'show this comparison' },
      el('td', { cls: 'cmp' }, el('b', null, `${o.label} vs ${N.remote.replace(/ \(.*\)$/, '')}`),
        el('div', { cls: 'muted' }, `${N.local} vs ${N.remote}`)),
      ...[dur(o.common_s), o.n, pct(o.recall.L.p), pct(o.recall.R.p), fmtP(o.recall.p),
        pct(o.bust.L.p), pct(o.bust.R.p),
        o.latency.median == null ? '—' : signed(Math.round(o.latency.median), ' s')]
        .map((v) => el('td', { cls: 'num' }, v)),
      el('td', null, wins.length ? wins.join(', ') : 'undecided'));
    const pick = () => setEngine(o.key);
    tr.addEventListener('click', pick);
    tr.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') pick(); });
    tb.append(tr);
  }
  t.append(tb);
  $('#overview-note').textContent = S.overview.length > 1
    ? 'Click a row to show that comparison below. Each decoder is compared only over the time the local skimmer ran it.'
    : `Only one decoder so far. When skimmer-headless runs another one ([decode] engine= in headless.ini, then a restart), its time becomes a comparison of its own here, next to this one.`;
}

function renderEngineSelect(S) {
  const sel = $('#engine');
  sel.replaceChildren(...S.engines.map((e) => el('option', { value: e.key, selected: e.key === S.engine },
    `${S.names.local.replace(/ \(.*\)$/, '')} ${e.label} vs ${S.names.remote.replace(/ \(.*\)$/, '')}`)));
}
function setEngine(k) {
  url.set('engine', k);
  st.open.clear();
  pushUrl();
  refresh();
}

function renderRange(S) {
  const sel = $('#range');
  const cur = url.get('range') || 'all';
  const opts = [['all', 'All data'], ['1h', 'Last hour'], ['6h', 'Last 6 hours'],
    ['24h', 'Last 24 hours'], ['today', 'Today (UTC)'], ['session', 'Current session']];
  for (const s of [...S.sessions].reverse()) {
    opts.push(['session:' + s.T, `Session ${s.T} (${dur(s.common_s)} compared)`]);
  }
  opts.push(['custom', 'Custom…']);
  sel.replaceChildren(...opts.map(([v, t]) => el('option', { value: v, selected: v === cur }, t)));
  $('#custom').hidden = cur !== 'custom';
}

let paramsBuilt = false;
function renderParams(S) {
  if (paramsBuilt) return;
  paramsBuilt = true;
  const g = $('#params-grid');
  for (const [k, d] of Object.entries(S.param_defs)) {
    g.append(el('label', null, el('span', null, k),
      el('input', { type: 'number', id: 'p-' + k, step: 'any', min: d.min, max: d.max,
                    value: url.get(k) ?? S.params[k] }),
      el('span', { cls: 'help' }, d.help + ` (default ${d.default})`)));
  }
}

function tile(label, value, sub, key) {
  return el('div', { cls: 'tile' },
    el('div', { cls: 'l' }, key ? el('span', { cls: 'key key-' + key }) : null, label),
    el('div', { cls: 'v' }, value), el('div', { cls: 's' }, sub));
}

function renderVerdict(S) {
  const V = $('#verdicts');
  V.replaceChildren();
  for (const v of S.verdicts) {
    const b = v.winner
      ? el('span', { cls: 'badge' }, el('span', { cls: 'key key-' + v.winner }), v.winner + ' better')
      : el('span', { cls: 'badge none' }, 'undecided');
    V.append(el('div', { cls: 'verdict' }, el('span', { cls: 'm' }, v.metric), b,
      el('span', null, v.winner ? v.text : v.text.replace(/^undecided — /, ''))));
  }
  const r = S.recall, b = S.bust, l = S.latency, t = S.time;
  const ci = (x) => (x.lo == null ? '' : `95 % CI ${pctShort(x.lo)}–${pctShort(x.hi)}`);
  $('#tiles').replaceChildren(
    tile('Real stations', r.n, `${r.b} only L · ${r.c} only R`),
    tile('Recall', pct(r.L.p), ci(r.L), 'L'),
    tile('Recall', pct(r.R.p), ci(r.R), 'R'),
    tile('Recall difference L − R', r.diff == null ? '—' : signed(+(100 * r.diff).toFixed(1), ' pp'),
      `McNemar ${fmtP(r.p)}`),
    tile('Bust rate', pct(b.L.p), `${b.L.k} of ${b.L.n} episodes`, 'L'),
    tile('Bust rate', pct(b.R.p), `${b.R.k} of ${b.R.n} episodes`, 'R'),
    tile('First spot, median R − L', l.median == null ? '—' : signed(Math.round(l.median), ' s'),
      `L first ${l.L_first} · R first ${l.R_first} · tie ${l.ties}`),
    tile('Compared time', dur(t.common_s), `of ${dur(t.span_s)} recorded`),
    tile('RBN referee', t.span_s ? pctShort(t.rbn_s / t.span_s) : '—',
      `of recorded time · ${S.counts.rbn_spots.toLocaleString('en')} spots indexed`),
    tile('Spots compared', `${S.counts.spots_L} / ${S.counts.spots_R}`,
      `L / R CQ (R all ${S.counts.spots_R_all})`),
  );
  const C = S.cats, cats = $('#cats');
  cats.replaceChildren();
  const chips = [['match', 'match'], ['only-L', 'only L'], ['only-R', 'only R'],
    ['bust', 'bust pair'], ['nonCQ', 'R heard, not CQ'], ['only-L-unv', 'only L unverified', 'only-L unverified'],
    ['only-R-unv', 'only R unverified', 'only-R unverified'], ['bust', 'bust unresolved', 'bust unresolved']];
  for (const [f, label, key] of chips) {
    const n = C[key || f] || 0;
    cats.append(el('button', { type: 'button', onclick: () => setCat(f) }, label, el('b', null, n)));
  }
  renderBreakdown(S);
  renderCI(S);
}

// One bar per side and unit (episodes, unique calls), each in % of its U.
// Left of the dashed line is the universe of real stations U (the same for
// both sides): found, busted (a bust pair the referee settled against this
// side: the station counts as missed) and missed. Right of it are the side's
// episodes recall never sees: calls nobody confirms (lone busts), unsettled
// bust pairs and L's calls R heard only as non-CQ.
// found + lost + missed = recall's n; lost + unverified = the bust rate's k.
const BD = ['found', 'lost', 'missed', 'unverified', 'unresolved', 'nonCQ'];
const BD_IN_U = new Set(['found', 'lost', 'missed']);
const BD_TEXT = {
  ep: {
    found: ['found', 'real station spotted with the right call'],
    lost: ['busted → missed', 'the other side had the confirmed call; counts as a miss and as a bust'],
    missed: ['missed', 'real station the other side found, this one never reported'],
    unverified: ['unverified call', 'nobody confirms the call: a bust, but no real station behind it, so recall cannot see it'],
    unresolved: ['bust pair, unsettled', 'similar calls at one spot, the referee cannot tell which is right'],
    nonCQ: ['R heard, not as CQ', 'L spotted a real call R reported only without CQ'],
  },
  calls: {
    found: ['found', 'real call reported right at least once'],
    lost: ['busted, never right', 'a real call this side only ever reported busted'],
    missed: ['missed', 'real call the other side found, this one never reported'],
    unverified: ['unconfirmed call', 'a call string nobody confirms: this side\'s busts'],
    unresolved: ['unsettled only', 'a call string seen only in bust pairs the referee cannot settle'],
    nonCQ: ['verified, not as CQ by R', 'a verified call outside U: R heard it only without CQ'],
  },
};
function bdFill(k, side) {
  const c = side === 'L' ? cL : cR;
  return {
    found: { fill: c },
    lost: { fill: 'url(#bd-lost)', stroke: 'var(--bad)' },
    missed: { fill: 'var(--out)' },
    unverified: { fill: 'url(#bd-unv)', stroke: 'var(--bad)' },
    unresolved: { fill: 'url(#bd-unres)', stroke: 'var(--muted)' },
    nonCQ: { fill: side === 'L' ? 'var(--L-wash)' : 'var(--R-wash)', stroke: c },
  }[k];
}
function bdDefs(svg, pre) {
  const defs = sv('defs');
  for (const [id, angle, color, gap] of [['lost', 45, 'var(--bad)', 4], ['unv', 135, 'var(--bad)', 5],
    ['unres', 135, 'var(--muted)', 5]]) {
    const p = sv('pattern', { id: pre + id, width: gap, height: gap, patternUnits: 'userSpaceOnUse',
      patternTransform: `rotate(${angle})` });
    p.append(sv('line', { x1: 0, y1: 0, x2: 0, y2: gap, stroke: color, 'stroke-width': 1.5 }));
    defs.append(p);
  }
  svg.append(defs);
}
// a breakdown of one side (episodes) or of its unique calls, with the totals
// the percentages are taken of
function bdPart(B, side, unit) {
  const d = B[side];
  return unit === 'calls'
    ? { v: d.calls, n: B.calls_real, own: d.calls.own, nWhat: 'real calls', ownWhat: `${side} unique calls` }
    : { v: d, n: B.n, own: d.episodes, nWhat: 'real stations', ownWhat: `${side} episodes` };
}
function bdSegmentTip(side, unit, k, p) {
  const [label, what] = BD_TEXT[unit][k], v = p.v[k], f = (x) => x.toLocaleString('en');
  return [`${side} · ${label}`, [
    [null, f(v), BD_IN_U.has(k) ? `${pct(p.n ? v / p.n : null)} of ${f(p.n)} ${p.nWhat}`
      : `${pct(p.own ? v / p.own : null)} of ${f(p.own)} ${p.ownWhat}`],
    [null, '', what]]];
}

function renderBreakdown(S) {
  const B = S.breakdown, box = $('#breakdown'), txt = $('#breakdown-text');
  const t = S.time;
  $('#breakdown-range').textContent = t.common_from == null
    ? `${S.range.label}: nothing compared`
    : `${S.range.label} · ${engLabel(S, S.engine)} · compared ${utc(t.common_from)} – ${utc(t.common_to)} UTC, `
      + `${dur(t.common_s)} of common time`;
  if (!B) { box.replaceChildren(); txt.replaceChildren(); return; }
  // One chart, one axis: every bar in % of its own U, so the U line is the
  // same place for episodes and unique calls; the found segment reads as
  // recall and what lies past 100 % is what the side spots on top of U.
  const total = (v) => BD.reduce((a, k) => a + v[k], 0);
  const rows = [['ep', 'L'], ['ep', 'R'], ['calls', 'L'], ['calls', 'R']]
    .map(([unit, side]) => ({ unit, side, p: bdPart(B, side, unit) }));
  const m = { l: 92, r: 16, t: 34, b: 24 }, rowH = 30, barH = 18, groupGap = 14;
  const H = m.t + rows.length * rowH + groupGap + m.b;
  const svg = svgRoot(H);
  bdDefs(svg, 'bd-');
  const max = Math.max(1.25, ...rows.map((r) => (r.p.n ? total(r.p.v) / r.p.n : 0)));
  const top = Math.ceil(max * 4) / 4;
  const x = (f) => m.l + f / top * (W - m.l - m.r);
  const rowY = (i) => m.t + i * rowH + (i >= 2 ? groupGap : 0);
  const yEnd = rowY(rows.length - 1) + rowH;
  for (let f = 0; f <= top + 1e-9; f += top > 2 ? 0.5 : 0.25) {
    svg.append(sv('line', { cls: 'grid', x1: x(f), x2: x(f), y1: m.t - 4, y2: yEnd }));
    svg.append(sv('text', { x: x(f), y: H - 8, 'text-anchor': 'middle' }, pctShort(f)));
  }
  // brackets: what recall counts, and what lies outside it
  const brace = (a, b, label) => {
    if (x(b) - x(a) < 4) return;
    const y = m.t - 12;
    svg.append(sv('path', { d: `M${x(a) + 1},${y + 6}V${y}H${x(b) - 1}V${y + 6}`, fill: 'none', stroke: 'var(--axis)' }));
    svg.append(sv('text', { cls: 'lbl', x: (x(a) + x(b)) / 2, y: y - 4, 'text-anchor': 'middle' }, label));
  };
  brace(0, 1, x(1) - x(0) > 260 ? 'U = 100 % of real stations / calls — recall' : 'U = 100 %');
  brace(1, max, x(max) - x(1) > 120 ? 'outside recall, in % of U' : '');
  rows.forEach((r, i) => {
    const { unit, side, p } = r, y = rowY(i) + (rowH - barH) / 2;
    svg.append(sv('text', { cls: 'lbl', x: 0, y: y + barH / 2 + 4 },
      `${side} ${unit === 'calls' ? 'unique calls' : 'episodes'}`));
    if (!p.n) return;
    let acc = 0;
    for (const k of BD) {
      const v = p.v[k];
      if (!v) continue;
      const x0 = x(acc / p.n), x1 = x((acc + v) / p.n);
      acc += v;
      const w = x1 - x0 - 2;              // 2px surface gap between segments
      if (w < 0.5) continue;
      const f = bdFill(k, side);
      const g = sv('g');
      g.append(sv('rect', { x: x0 + 1, y, width: w, height: barH, rx: Math.min(2, w / 2),
        fill: f.fill, stroke: f.stroke, 'stroke-width': f.stroke ? 1 : null }));
      g.append(sv('rect', { cls: 'hit', x: x0, y: y - 4, width: Math.max(6, x1 - x0), height: barH + 8 }));
      hover(g, () => {
        const [h, rr] = bdSegmentTip(side, unit, k, p);
        if (!BD_IN_U.has(k)) rr.splice(1, 0, [null, pct(v / p.n), `of U (${p.n.toLocaleString('en')} ${p.nWhat})`]);
        return [h, rr];
      });
      svg.append(g);
    }
  });
  // the edge of U: one place for all four bars
  svg.append(sv('line', { x1: x(1), x2: x(1), y1: m.t - 6, y2: yEnd, stroke: 'var(--ink-2)', 'stroke-dasharray': '3 3' }));
  box.replaceChildren(svg);

  const fmt = (v) => v.toLocaleString('en');
  const spots = { L: `${fmt(S.counts.spots_L)} CQ spots`,
    R: `${fmt(S.counts.spots_R)} CQ spots (${fmt(S.counts.spots_R_all)} all)` };
  txt.replaceChildren(...['L', 'R'].map((side) => {
    const d = B[side], c = d.calls, ok = c.found + c.nonCQ;
    return el('p', null, el('span', { cls: 'key key-' + side }),
      el('b', null, side), ` ${spots[side]} → ${fmt(d.episodes)} episodes · recall `,
      el('b', null, pct(S.recall[side].p)), ` (${fmt(d.found)} of ${fmt(B.n)}) · bust rate `,
      el('b', null, pct(S.bust[side].p)),
      ` = ${fmt(d.lost)} busted a real station (lowers recall) + ${fmt(d.unverified)} unverified (bust rate only)`,
      ' · unique calls ', el('b', null, fmt(c.own)), ', verified ',
      el('b', null, pct(c.own ? ok / c.own : null)), ` (${fmt(ok)})`,
      ', found ', el('b', null, pct(B.calls_real ? c.found / B.calls_real : null)),
      ` of ${fmt(B.calls_real)} real calls`);
  }));
  renderHourly(S);
}

// The same partition per UTC hour: per hour an L and an R column on one count
// axis, U (found, busted, missed) above the baseline, the side's calls outside
// recall below it. Wider than the card when there are many hours: the box
// scrolls sideways and opens at the newest hour.
function renderHourly(S) {
  const box = $('#hourly'), Hs = S.hourly || [];
  const unit = st.hourUnit;
  for (const b of document.querySelectorAll('#hourly-unit button')) {
    b.setAttribute('aria-pressed', String(b.dataset.unit === unit));
  }
  if (!Hs.length) {
    $('#hourly-axis').replaceChildren();
    box.replaceChildren(el('p', { cls: 'muted' }, 'no compared hour in this range'));
    return;
  }
  const P = Hs.map((h) => ({ h, L: bdPart(h, 'L', unit), R: bdPart(h, 'R', unit) }));
  const upMax = Math.max(1, ...P.map((p) => p.L.n)), tick = niceMax(upMax / 4);
  const up = Math.ceil(upMax / tick) * tick;
  const out = (v) => v.unverified + v.unresolved + v.nonCQ;
  const down = Math.max(...P.map((p) => Math.max(out(p.L.v), out(p.R.v))));
  const dn = down ? Math.ceil(down / tick) * tick : 0;
  // the count axis sits in its own box left of the scrolling plot
  const AX = 62, m = { l: 4, r: 12, t: 10, b: 28 }, H = 280;
  const slot = Math.max(12, (W - AX - m.l - m.r) / P.length);
  const Wd = Math.round(m.l + m.r + slot * P.length);
  const svg = sv('svg', { viewBox: `0 0 ${Wd} ${H}`, role: 'img' });
  if (Wd > W - AX) svg.setAttribute('style', `width:${Wd}px`);
  const axis = sv('svg', { viewBox: `0 0 ${AX} ${H}`, role: 'img', style: `width:${AX}px` });
  bdDefs(svg, 'hr-');
  const plotH = H - m.t - m.b;
  const k = plotH / (up + dn);                 // px per count, one scale both ways
  const y0 = m.t + up * k;
  const x = (i) => m.l + i * slot;
  P.forEach((p, i) => {
    if (p.h.common_s === 0) {
      svg.append(sv('rect', { x: x(i), y: m.t, width: slot, height: plotH, fill: 'var(--out)' }));
    }
  });
  for (let v = tick; v <= up; v += tick) {
    svg.append(sv('line', { cls: 'grid', x1: m.l, x2: Wd - m.r, y1: y0 - v * k, y2: y0 - v * k }));
    axis.append(sv('text', { x: AX - 6, y: y0 - v * k + 4, 'text-anchor': 'end' }, Math.round(v)));
  }
  if (dn) {
    svg.append(sv('line', { cls: 'grid', x1: m.l, x2: Wd - m.r, y1: y0 + dn * k, y2: y0 + dn * k }));
    axis.append(sv('text', { x: AX - 6, y: y0 + dn * k + 4, 'text-anchor': 'end' }, Math.round(dn)));
  }
  axis.append(sv('text', { x: AX - 6, y: y0 + 4, 'text-anchor': 'end' }, 0));
  // brackets, as on the bars above: U above the line, outside recall below;
  // the longest label that fits the bracket, none if nothing does
  const vbrace = (ya, yb, labels) => {
    if (yb - ya < 6) return;
    const bx = 18;
    axis.append(sv('path', { d: `M${bx + 5},${ya + 1}H${bx}V${yb - 1}H${bx + 5}`, fill: 'none', stroke: 'var(--axis)' }));
    const label = labels.find((l) => l.length * 6 + 6 <= yb - ya);
    if (label) {
      const cy = (ya + yb) / 2;
      axis.append(sv('text', { cls: 'lbl', x: bx - 6, y: cy, 'text-anchor': 'middle',
        transform: `rotate(-90 ${bx - 6} ${cy})` }, label));
    }
  };
  vbrace(y0 - up * k, y0 - 2, [unit === 'calls' ? 'U (real calls) · recall' : 'U (real stations) · recall',
    'U · recall', 'U']);
  if (dn) vbrace(y0 + 2, y0 + dn * k, ['outside recall', 'outside']);
  // an L and an R column, 1px apart, the pair centred with air between hours
  const cw = Math.max(2, Math.min(18, (slot * 0.8 - 1) / 2)), pad = (slot - 2 * cw - 1) / 2;
  P.forEach((p, i) => {
    const g = sv('g');
    ['L', 'R'].forEach((side, j) => {
      const cx = x(i) + pad + j * (cw + 1);
      let yu = y0, yd = y0;
      for (const kk of BD) {
        const v = p[side].v[kk];
        if (!v) continue;
        const h = v * k, f = bdFill(kk, side), gap = h >= 4 ? 1 : 0;
        let yy;
        if (BD_IN_U.has(kk)) { yu -= h; yy = yu; } else { yy = yd; yd += h; }
        g.append(sv('rect', { x: cx, y: yy + (BD_IN_U.has(kk) ? gap : 0), width: cw, height: Math.max(0.5, h - gap),
          fill: f.fill, stroke: f.stroke, 'stroke-width': f.stroke && cw > 3 ? 1 : null }));
      }
    });
    g.append(sv('rect', { cls: 'hit', x: x(i), y: m.t, width: slot, height: plotH }));
    hover(g, () => {
      const rows = [];
      for (const side of ['L', 'R']) {
        const q = p[side], v = q.v, c = side === 'L' ? cL : cR;
        const busts = unit === 'calls' ? v.unverified : v.lost + v.unverified;
        rows.push([c, pct(q.n ? v.found / q.n : null), `${side} recall · ${v.found} of ${q.n} ${q.nWhat}`]);
        rows.push([null, pct(q.own ? busts / q.own : null),
          unit === 'calls' ? `${side} unconfirmed calls · ${busts} of ${q.own}`
            : `${side} bust rate · ${v.lost} busted real + ${v.unverified} unverified of ${q.own}`]);
      }
      rows.push([null, dur(p.h.common_s), 'compared']);
      return [`${utc(p.h.t)} – ${hhmm(p.h.t + 3600)} UTC · ${unit === 'calls' ? 'unique calls' : 'episodes'}`, rows];
    });
    svg.append(g);
  });
  svg.append(sv('line', { cls: 'axis', x1: m.l, x2: Wd - m.r, y1: y0, y2: y0 }));
  // labels at least ~70 px apart, on whole hours of the day
  const every = [1, 2, 3, 6, 12, 24].find((s) => s * slot >= 70) || 24;
  P.forEach((p, i) => {
    const hr = (p.h.t / 3600) % 24;
    if (hr % every || x(i) < 16 || x(i) > Wd - 16) return;
    svg.append(sv('line', { cls: 'grid', x1: x(i), x2: x(i), y1: H - m.b, y2: H - m.b + 4 }));
    svg.append(sv('text', { x: x(i), y: H - 10, 'text-anchor': 'middle' },
      hr === 0 ? iso(p.h.t).slice(5, 10) : hhmm(p.h.t)));
  });
  $('#hourly-axis').replaceChildren(axis);
  box.replaceChildren(svg);
  box.scrollLeft = box.scrollWidth;
}

function renderCI(S) {
  const rows = [['Recall', S.recall], ['Bust rate', S.bust]];
  const m = { l: 90, r: 16, t: 8, b: 24 }, rowH = 44, H = m.t + rows.length * rowH + m.b;
  const svg = svgRoot(H);
  const x = (p) => m.l + p * (W - m.l - m.r);
  for (const v of [0, .25, .5, .75, 1]) {
    svg.append(sv('line', { cls: 'grid', x1: x(v), x2: x(v), y1: m.t, y2: H - m.b }));
    svg.append(sv('text', { x: x(v), y: H - 8, 'text-anchor': 'middle' }, pctShort(v)));
  }
  rows.forEach(([name, blk], i) => {
    const y0 = m.t + i * rowH;
    svg.append(sv('text', { cls: 'lbl', x: 0, y: y0 + rowH / 2 + 4 }, name));
    for (const [side, dy, color] of [['L', 14, cL], ['R', 30, cR]]) {
      const d = blk[side];
      if (d.p == null) continue;
      const y = y0 + dy;
      const g = sv('g');
      g.append(sv('line', { x1: x(d.lo), x2: x(d.hi), y1: y, y2: y, stroke: color, 'stroke-width': 2, 'stroke-linecap': 'round' }));
      g.append(sv('circle', { cx: x(d.p), cy: y, r: 5, fill: color, stroke: 'var(--surface)', 'stroke-width': 2 }));
      g.append(sv('rect', { cls: 'hit', x: x(d.lo) - 8, y: y - 12, width: Math.max(24, x(d.hi) - x(d.lo) + 16), height: 24 }));
      hover(g, () => [`${name} · ${side}`, [[color, pct(d.p), `${d.k} of ${d.n}`],
        [null, `${pct(d.lo)} – ${pct(d.hi)}`, '95 % CI']]]);
      svg.append(g);
    }
  });
  $('#ci').replaceChildren(svg);
}

// ---- differences ---------------------------------------------------------------------
function setCat(c) {
  st.f.cat = c; $('#f-cat').value = c; st.shown = 200; pushUrl(); renderDiffs();
  $('#diff-card').scrollIntoView({ behavior: 'smooth', block: 'start' });
}
function passes(e) {
  const f = st.f;
  if (f.band && e.band !== f.band) return false;
  if (f.v && e.v !== f.v && !(e.L && e.L.v === f.v) && !(e.R && e.R.v === f.v)) return false;
  if (f.call) {
    const c = f.call.toUpperCase();
    if (![e.call, e.L && e.L.call, e.R && e.R.call].some((x) => x && x.includes(c))) return false;
  }
  switch (f.cat) {
    case 'all': return true;
    case 'diff': return e.cat !== 'match';
    case 'only-L': case 'only-R': return e.cat === f.cat && e.inU;
    case 'only-L-unv': return e.cat === 'only-L' && !e.inU;
    case 'only-R-unv': return e.cat === 'only-R' && !e.inU;
    default: return e.cat === f.cat;
  }
}
function referee(e) {
  if (e.cat === 'bust') {
    const p = (s) => (s && s.v ? (VERD[s.v] || s.v) + (s.rbn ? ' ×' + s.rbn : '') : '—');
    return `L: ${p(e.L)} · R: ${p(e.R)}`;
  }
  return (VERD[e.v] || e.v) + (e.rbn ? ' ×' + e.rbn : '');
}
function callCell(side, e) {
  const s = e[side];
  if (!s) return el('td', { cls: 'call muted' }, '—');
  const busted = side === 'L' ? e.bL : e.bR;
  return el('td', { cls: 'call' + (busted ? ' busted' : ''), title: `${s.n} spot(s), ${hhmm(s.t0)}–${hhmm(s.t1)} UTC` }, s.call);
}
function catCell(e) {
  const keys = { 'only-L': ['L'], 'only-R': ['R'], 'match': ['L', 'R'], 'nonCQ': ['L'], 'bust': ['L', 'R'] };
  let label = CAT[e.cat] || e.cat;
  if ((e.cat === 'only-L' || e.cat === 'only-R') && !e.inU) label += ', unverified';
  if (e.cat === 'bust' && !e.inU) label += ', unresolved';
  return el('td', null, el('span', { cls: 'cat' },
    ...(keys[e.cat] || []).map((k) => el('span', { cls: 'key key-' + k })), label));
}
function noteText(e) {
  const parts = [];
  if (e.note) parts.push(e.note);
  if (e.cat === 'match' && e.lat != null) parts.push(`R − L first spot ${signed(e.lat, ' s')}`);
  if (e.hint && e.hint.length) parts.push('heard there: ' + e.hint.map(([s, c, n]) => `${s} ${c} ×${n}`).join(', '));
  return parts.join(' · ');
}
function renderDiffs() {
  const tb = $('#diffs tbody');
  const list = st.E.filter(passes).sort((a, b) => b.t0 - a.t0);
  $('#diff-count').textContent = `${list.length} event(s)`;
  const bands = [...new Set(st.E.map((e) => e.band))].sort((a, b) => parseFloat(b) - parseFloat(a));
  const bs = $('#f-band');
  bs.replaceChildren(el('option', { value: '' }, 'all'),
    ...bands.map((b) => el('option', { value: b, selected: b === st.f.band }, b)));
  tb.replaceChildren();
  for (const e of list.slice(0, st.shown)) {
    const key = `${e.band}|${e.call}|${e.t0}`;
    const tr = el('tr', { cls: 'ev' + (st.open.has(key) ? ' open' : ''), tabindex: 0 },
      el('td', { cls: 'num' }, utc(e.t0)), el('td', null, e.band), el('td', { cls: 'num' }, e.f.toFixed(1)),
      catCell(e), callCell('L', e), callCell('R', e), el('td', null, referee(e)),
      el('td', { cls: 'num' }, e.snr ?? '—'), el('td', { cls: 'num' }, e.wpm ?? '—'),
      el('td', { cls: 'note' }, noteText(e)));
    const toggle = () => {
      if (st.open.has(key)) st.open.delete(key); else st.open.add(key);
      renderDiffs();
    };
    tr.addEventListener('click', toggle);
    tr.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') toggle(); });
    tb.append(tr);
    if (st.open.has(key)) {
      const td = el('td', { colspan: 10 }, el('div', { cls: 'raw' }, 'loading…'));
      tb.append(el('tr', null, td));
      loadRaw(e, td.firstChild);
    }
  }
  $('#more').hidden = list.length <= st.shown;
}
async function loadRaw(e, box) {
  const calls = [e.L && e.L.call, e.R && e.R.call, e.call].filter(Boolean);
  const q = new URLSearchParams(apiQuery());
  q.set('band', e.band); q.set('f', e.f); q.set('t0', e.t0); q.set('t1', e.t1);
  q.set('calls', [...new Set(calls)].join(','));
  try {
    const R = await fetch('/api/raw?' + q).then((r) => r.json());
    const block = (title, key, lines) => {
      const pre = el('pre');
      if (!lines.length) pre.textContent = '(nothing)';
      for (const ln of lines) pre.append(el('span', { cls: ln.mine ? 'mine' : '' }, ln.text), '\n');
      return el('div', null, el('h4', null, key ? el('span', { cls: 'key key-' + key }) : null, title), pre);
    };
    box.replaceChildren(
      block('L — local feed, ±15 min, same call or within 1 kHz', 'L',
        R.L.map(([t, line, mine]) => ({ text: hms(t) + '  ' + line, mine }))),
      block('R — remote feed (all comment types), ±15 min, same call or within 1 kHz', 'R',
        R.R.map(([t, line, mine]) => ({ text: hms(t) + '  ' + line, mine }))),
      block('RBN — other skimmers: this call nearby in time, and anything at this frequency', null,
        R.rbn.map(([t, sp, f, c, snr]) => ({
          text: `${utc(t)}  ${sp.padEnd(12)} ${f.toFixed(1).padStart(8)}  ${c.padEnd(12)} ${String(snr).padStart(3)} dB`,
          mine: calls.includes(c) })))
    );
  } catch (err) {
    box.textContent = 'cannot load raw lines: ' + err.message;
  }
}

// ---- bands + timeline ---------------------------------------------------------------
function renderBands(S) {
  const t = $('#bands');
  const head = ['Band', 'kHz', 'Stations', 'Recall L', 'Recall R', 'Only L', 'Only R',
    'Bust L', 'Bust R', 'R non-CQ', 'McNemar', 'R − L median'];
  t.replaceChildren(el('thead', null, el('tr', null, head.map((h, i) => el('th', { cls: i > 1 ? 'num' : '' }, h)))));
  const tb = el('tbody');
  for (const b of S.bands) {
    tb.append(el('tr', null,
      el('td', null, b.band), el('td', null, `${b.lo.toFixed(1)}–${b.hi.toFixed(1)}`),
      ...[b.n, pct(b.L.p), pct(b.R.p), b.onlyL, b.onlyR,
        `${b.bustL} / ${b.epsL}`, `${b.bustR} / ${b.epsR}`, b.nonCQ, fmtP(b.p),
        b.lat == null ? '—' : signed(Math.round(b.lat), ' s')].map((v) => el('td', { cls: 'num' }, v))));
  }
  t.append(tb);
}

function renderRtty(S) {
  const R = S.rtty;
  $('#rtty-card').hidden = !R || !R.time_s;
  if (!R || !R.time_s) return;
  const t = $('#rtty');
  const head = ['Band', 'RBN stations', 'L spotted', 'Recall', 'Recall strong', 'L stations',
    'RBN knows', 'SCP only', 'Bust'];
  t.replaceChildren(el('thead', null, el('tr', null, head.map((h, i) => el('th', { cls: i ? 'num' : '' }, h)))));
  const tb = el('tbody');
  const row = (name, b) => el('tr', null, el('td', null, name),
    ...[b.rbn, b.matched, pct(b.recall), `${pct(b.recall_strong)} (${b.matched_strong} / ${b.rbn_strong})`,
      b.local, b.local_rbn, b.local_scp, `${pct(b.bust)} (${b.local_none})`].map((v) => el('td', { cls: 'num' }, v)));
  for (const [name, b] of Object.entries(R.bands)) tb.append(row(name, b));
  tb.append(row(`all · ${dur(R.time_s)}`, R.total));
  t.append(tb);
  const bl = R.busts;
  $('#rtty-busts').replaceChildren(bl.length
    ? el('table', null, el('tbody', null, bl.slice(-200).reverse().map((x) => el('tr', null,
      el('td', null, `${utc(x.t0)}`), el('td', null, x.band), el('td', { cls: 'num' }, x.f.toFixed(2)),
      el('td', null, x.call), el('td', { cls: 'num' }, `${x.spots} spot${x.spots === 1 ? '' : 's'}`)))))
    : el('p', { cls: 'muted' }, 'none'));
}

function timeTicks(a, b) {
  const span = b - a;
  const steps = [600, 1800, 3600, 3 * 3600, 6 * 3600, 12 * 3600, 86400, 2 * 86400, 7 * 86400];
  const step = steps.find((s) => span / s <= 7) || 7 * 86400;
  const out = [];
  for (let t = Math.ceil(a / step) * step; t <= b; t += step) out.push(t);
  return { out, fmt: step >= 86400 ? (t) => iso(t).slice(5, 10) : span > 86400 ? utc : hhmm };
}

function renderTimeline(S) {
  const T = S.timeline, ta = S.range.from, tb = Math.min(S.range.to, S.now);
  const m = { l: 44, r: 12, t: 14, b: 26 }, H = 230;
  const svg = svgRoot(H);
  const x = (t) => m.l + ((t - ta) / Math.max(1, tb - ta)) * (W - m.l - m.r);
  const y = (p) => m.t + (1 - p) * (H - m.t - m.b);
  for (const [a, b] of [...T.outages, ...(T.dead || []).map((d) => [d.t0, d.t1])]) {
    svg.append(sv('rect', { x: x(a), y: m.t, width: Math.max(1, x(b) - x(a)), height: H - m.t - m.b, fill: 'var(--out)' }));
  }
  for (const [a, b] of T.other || []) {
    svg.append(sv('rect', { x: x(a), y: m.t, width: Math.max(1, x(b) - x(a)), height: H - m.t - m.b, fill: 'var(--L-wash)' }));
  }
  yGrid(svg, m, H, y, [0, .25, .5, .75, 1], pctShort);
  const ticks = timeTicks(ta, tb);
  for (const t of ticks.out) svg.append(sv('text', { x: x(t), y: H - 8, 'text-anchor': 'middle' }, ticks.fmt(t)));
  for (const s of T.silences) {
    const r = sv('rect', { x: x(Math.max(ta, s.t0)), y: 2, height: 6, rx: 2,
      width: Math.max(3, x(Math.min(tb, s.t1)) - x(Math.max(ta, s.t0))), fill: s.side === 'L' ? cL : cR });
    hover(r, () => [`${s.side} silent on ${s.band}`, [[null, `${hhmm(s.t0)}–${hhmm(s.t1)}`, 'UTC'],
      [s.side === 'L' ? cR : cL, s.other, `spots by ${s.side === 'L' ? 'R' : 'L'} meanwhile`]]]);
    svg.append(r);
  }
  const mid = (r) => r.t + T.step / 2;
  for (const [side, color] of [['L', cL], ['R', cR]]) {
    let d = '', pen = false;
    for (const r of T.rows) {
      if (!r.n || mid(r) < ta || mid(r) > tb) { pen = false; continue; }
      d += (pen ? 'L' : 'M') + x(mid(r)).toFixed(1) + ',' + y(r[side] / r.n).toFixed(1);
      pen = true;
    }
    svg.append(sv('path', { d, fill: 'none', stroke: color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    for (const r of T.rows) {
      if (!r.n || mid(r) < ta || mid(r) > tb) continue;
      svg.append(sv('circle', { cx: x(mid(r)), cy: y(r[side] / r.n), r: 4, fill: color, stroke: 'var(--surface)', 'stroke-width': 2 }));
    }
  }
  // crosshair: snaps to the nearest interval
  const cross = sv('line', { cls: 'cross', y1: m.t, y2: H - m.b, visibility: 'hidden' });
  svg.append(cross);
  const ov = sv('rect', { x: m.l, y: m.t, width: W - m.l - m.r, height: H - m.t - m.b, fill: 'transparent' });
  ov.addEventListener('pointermove', (ev) => {
    const box = svg.getBoundingClientRect();
    const t = ta + ((ev.clientX - box.left) * (W / box.width) - m.l) / (W - m.l - m.r) * (tb - ta);
    const rows = T.rows.filter((r) => mid(r) >= ta && mid(r) <= tb);
    if (!rows.length) return;
    const r = rows.reduce((a, b) => (Math.abs(mid(b) - t) < Math.abs(mid(a) - t) ? b : a));
    cross.setAttribute('x1', x(mid(r))); cross.setAttribute('x2', x(mid(r)));
    cross.setAttribute('visibility', 'visible');
    tipShow(ev, `${utc(r.t)} – ${hhmm(r.t + T.step)} UTC`, [
      [cL, r.n ? pct(r.L / r.n) : '—', `L ${r.L} of ${r.n}`],
      [cR, r.n ? pct(r.R / r.n) : '—', `R ${r.R} of ${r.n}`],
      [null, `${r.bustL} / ${r.bustR}`, 'busts L / R'],
      [null, dur(r.common_s), 'compared']]);
  });
  ov.addEventListener('pointerleave', () => { cross.setAttribute('visibility', 'hidden'); tipHide(); });
  svg.append(ov);
  $('#timeline').replaceChildren(svg);

  // volume: real stations per interval (one series, neutral)
  const H2 = 120, m2 = { l: 44, r: 12, t: 8, b: 22 };
  const v = svgRoot(H2);
  const max = niceMax(Math.max(1, ...T.rows.map((r) => r.n)));
  const y2 = (n) => m2.t + (1 - n / max) * (H2 - m2.t - m2.b);
  yGrid(v, m2, H2, y2, [0, max / 2, max], (n) => Math.round(n));
  const bw = Math.max(2, Math.min(24, x(ta + T.step) - x(ta) - 2));
  for (const r of T.rows) {
    if (mid(r) < ta || mid(r) > tb) continue;
    const g = sv('g');
    const b = bar(x(mid(r)) - bw / 2, y2(0), bw, y2(0) - y2(r.n), 'var(--muted)');
    if (b) g.append(b);
    g.append(sv('rect', { cls: 'hit', x: x(mid(r)) - Math.max(12, bw / 2), y: m2.t, width: Math.max(24, bw), height: H2 - m2.t - m2.b }));
    hover(g, () => [`${utc(r.t)} UTC`, [[null, r.n, 'real stations'], [null, dur(r.common_s), 'compared']]]);
    v.append(g);
  }
  v.append(sv('line', { cls: 'axis', x1: m2.l, x2: W - m2.r, y1: y2(0), y2: y2(0) }));
  for (const t of ticks.out) v.append(sv('text', { x: x(t), y: H2 - 6, 'text-anchor': 'middle' }, ticks.fmt(t)));
  $('#volume').replaceChildren(v);

  const box = $('#silences');
  const rows = [
    ...T.outages.map(([a, b]) => ['not compared', '', a, b, '']),
    ...(T.other || []).map(([a, b]) => ['another decoder', '', a, b, '']),
    ...(T.dead || []).map((d) => [`${d.side} feed dead`, 'all', d.t0, d.t1, 'not compared: no spot at all while the other side spotted']),
    ...T.silences.map((s) => [`${s.side} silent`, s.band, s.t0, s.t1, `${s.other} spots by the other side`]),
  ].sort((p, q) => p[2] - q[2]);
  box.replaceChildren(rows.length ? el('table', null,
    el('thead', null, el('tr', null, ['What', 'Band', 'From (UTC)', 'To', 'Length', ''].map((h) => el('th', null, h)))),
    el('tbody', null, rows.map(([w, b, a, z, n]) => el('tr', null, el('td', null, w), el('td', null, b),
      el('td', null, utc(a)), el('td', null, utc(z)), el('td', null, dur(z - a)), el('td', null, n)))))
    : el('p', { cls: 'muted' }, 'none in this range'));
}

// ---- sensitivity: grouped bars ---------------------------------------------------------
function binLabel(b, unit) {
  if (b.lo == null) return `< ${b.hi}`;
  if (b.hi == null) return `≥ ${b.lo}`;
  return `${b.lo}–${b.hi}`;
}
function renderBins(sel, bins, unit) {
  const m = { l: 44, r: 12, t: 10, b: 40 }, H = 220;
  const svg = svgRoot(H);
  const y = (p) => m.t + (1 - p) * (H - m.t - m.b);
  yGrid(svg, m, H, y, [0, .25, .5, .75, 1], pctShort);
  const n = bins.length, band = (W - m.l - m.r) / n, w = Math.min(24, band * 0.3);
  bins.forEach((b, i) => {
    const cx = m.l + band * (i + 0.5);
    const g = sv('g');
    if (b.n) {
      const l = bar(cx - w - 1, y(0), w, y(0) - y(b.L.p), cL);
      const r = bar(cx + 1, y(0), w, y(0) - y(b.R.p), cR);
      if (l) g.append(l);
      if (r) g.append(r);
      if (b.LofR && b.LofR.n)
        g.append(sv('line', { cls: 'lofr', x1: cx - w - 4, x2: cx + w + 4, y1: y(b.LofR.p), y2: y(b.LofR.p) }));
    }
    g.append(sv('text', { cls: 'lbl', x: cx, y: H - 24, 'text-anchor': 'middle' }, binLabel(b) + unit));
    g.append(sv('text', { x: cx, y: H - 10, 'text-anchor': 'middle' }, 'n = ' + b.n));
    g.append(sv('rect', { cls: 'hit', x: cx - band / 2 + 2, y: m.t, width: band - 4, height: H - m.t - m.b }));
    hover(g, () => [`${binLabel(b)}${unit} · ${b.n} stations`, [
      [cL, pct(b.L.p), `L ${b.L.k} of ${b.n}`], [cR, pct(b.R.p), `R ${b.R.k} of ${b.n}`],
      [null, b.LofR && b.LofR.n ? pct(b.LofR.p) : '—', `L caught ${b.LofR ? b.LofR.k : 0} of R's ${b.LofR ? b.LofR.n : 0} (line)`],
      [null, `${b.b} / ${b.c}`, 'only L / only R'], [null, fmtP(b.p), 'McNemar']]]);
    svg.append(g);
  });
  svg.append(sv('line', { cls: 'axis', x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }));
  $(sel).replaceChildren(svg);
}

function renderLatency(L) {
  const m = { l: 44, r: 12, t: 10, b: 40 }, H = 200;
  const svg = svgRoot(H);
  const max = niceMax(Math.max(1, ...L.hist.map((h) => h.n)));
  const y = (n) => m.t + (1 - n / max) * (H - m.t - m.b);
  yGrid(svg, m, H, y, [0, max / 2, max], (n) => Math.round(n));
  const n = L.hist.length, band = (W - m.l - m.r) / n, w = Math.min(24, band - 4);
  L.hist.forEach((h, i) => {
    const cx = m.l + band * (i + 0.5);
    // R − L < 0: R spotted first
    const color = h.hi != null && h.hi <= -3 ? cR : h.lo != null && h.lo >= 3 ? cL : 'var(--tie)';
    const label = h.lo == null ? `< ${h.hi}` : h.hi == null ? `≥ ${h.lo}` : h.lo === -3 ? '±3' : `${h.lo}…${h.hi}`;
    const g = sv('g');
    const b = bar(cx - w / 2, y(0), w, y(0) - y(h.n), color);
    if (b) g.append(b);
    g.append(sv('text', { cls: 'lbl', x: cx, y: H - 24, 'text-anchor': 'middle' }, label));
    g.append(sv('rect', { cls: 'hit', x: cx - band / 2 + 1, y: m.t, width: band - 2, height: H - m.t - m.b }));
    hover(g, () => [`R − L ${label} s`, [[color, h.n, 'matched stations']]]);
    svg.append(g);
  });
  svg.append(sv('text', { x: m.l, y: H - 6 }, '← R first'));
  svg.append(sv('text', { x: W - m.r, y: H - 6, 'text-anchor': 'end' }, 'L first →'));
  svg.append(sv('line', { cls: 'axis', x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }));
  $('#latency').replaceChildren(svg);
}

// ---- frequency butterfly per band ---------------------------------------------------------
function renderHists(S) {
  const box = $('#hists');
  box.replaceChildren();
  for (const h of S.hist) {
    const m = { l: 44, r: 12, t: 14, b: 22 }, H = 170;
    const svg = svgRoot(H);
    const x = (f) => m.l + ((f - h.lo) / (h.hi - h.lo)) * (W - m.l - m.r);
    const max = niceMax(Math.max(1, ...h.L, ...h.R));
    const mid = m.t + (H - m.t - m.b) / 2, half = (H - m.t - m.b) / 2;
    const yv = (n) => (n / max) * half;
    for (const [c0, c1] of h.parts || [h.common]) {
      svg.append(sv('rect', { x: x(c0), y: m.t, width: x(c1) - x(c0), height: H - m.t - m.b, fill: 'var(--surface-2)', 'fill-opacity': 0.6 }));
    }
    for (const v of [max, max / 2]) {
      for (const s of [-1, 1]) {
        svg.append(sv('line', { cls: 'grid', x1: m.l, x2: W - m.r, y1: mid - s * yv(v), y2: mid - s * yv(v) }));
      }
      const t = Number.isInteger(v) ? v : v.toFixed(1);
      svg.append(sv('text', { x: m.l - 6, y: mid - yv(v) + 4, 'text-anchor': 'end' }, t));
      svg.append(sv('text', { x: m.l - 6, y: mid + yv(v) + 4, 'text-anchor': 'end' }, t));
    }
    const bw = Math.max(1, x(h.lo + 1) - x(h.lo) - 2);
    for (let i = 0; i < h.L.length; i++) {
      const f = h.lo + i, g = sv('g');
      const a = bar(x(f) + 1, mid - 1, bw, yv(h.L[i]), cL);
      const b = bar(x(f) + 1, mid + 1, bw, yv(h.R[i]), cR, true);
      if (a) g.append(a);
      if (b) g.append(b);
      g.append(sv('rect', { cls: 'hit', x: x(f), y: m.t, width: x(f + 1) - x(f), height: H - m.t - m.b }));
      hover(g, () => [`${h.band} · ${f}–${f + 1} kHz`, [[cL, h.L[i], 'L CQ spots'], [cR, h.R[i], 'R CQ spots'],
        [null, h.Rall[i], 'R spots, any comment']]]);
      svg.append(g);
    }
    svg.append(sv('line', { cls: 'axis', x1: m.l, x2: W - m.r, y1: mid, y2: mid }));
    for (const [side, [lo, hi], color] of [['L', h.local, cL], ['R', h.remote, cR]]) {
      for (const f of [lo, hi]) {
        const xx = x(Math.max(h.lo, Math.min(h.hi, f)));
        svg.append(sv('line', { x1: xx, x2: xx, y1: m.t, y2: H - m.b, stroke: color, 'stroke-width': 1.5 }));
      }
      svg.append(sv('text', { x: x(Math.max(h.lo, lo)) + 4, y: side === 'L' ? m.t - 3 : H - m.b - 4 }, `${side} window ${lo.toFixed(0)}–${hi.toFixed(0)} kHz`));
    }
    const step = h.hi - h.lo > 60 ? 10 : 5;
    for (let f = Math.ceil(h.lo / step) * step; f <= h.hi; f += step) {
      svg.append(sv('text', { x: x(f), y: H - 4, 'text-anchor': 'middle' }, f));
    }
    box.append(el('div', { cls: 'hband' }, el('div', { cls: 'name' },
      `${h.band} — L ${h.L.reduce((a, b) => a + b, 0)} · R ${h.R.reduce((a, b) => a + b, 0)} CQ spots`), svg));
  }
}

function renderSessions(S) {
  const t = $('#sessions');
  t.replaceChildren(el('thead', null, el('tr', null,
    ['Session', 'Start (UTC)', 'End', 'Recorded', 'Compared', 'RBN', ''].map((h) => el('th', null, h)))));
  t.append(el('tbody', null, [...S.sessions].reverse().map((s) => el('tr', null,
    el('td', null, s.T), el('td', null, utc(s.start)), el('td', null, s.live ? 'running' : utc(s.end)),
    el('td', null, dur(s.end - s.start)), el('td', null, dur(s.common_s)),
    el('td', null, s.has_rbn ? 'yes' : 'no'),
    el('td', null, el('button', { type: 'button', onclick: () => setRange('session:' + s.T) }, 'show'))))));
}

// ---- controls -------------------------------------------------------------------------------
function setRange(v) {
  url.set('range', v);
  if (v !== 'custom') { url.delete('from'); url.delete('to'); }
  pushUrl();
  $('#custom').hidden = v !== 'custom';
  if (v !== 'custom') refresh();
}
$('#range').addEventListener('change', (e) => setRange(e.target.value));
$('#engine').addEventListener('change', (e) => setEngine(e.target.value));
$('#apply-range').addEventListener('click', () => {
  const f = $('#from').value, t = $('#to').value;
  if (f) url.set('from', f); else url.delete('from');
  if (t) url.set('to', t); else url.delete('to');
  pushUrl(); refresh();
});
$('#params-btn').addEventListener('click', (e) => {
  const p = $('#params');
  p.hidden = !p.hidden;
  e.target.setAttribute('aria-expanded', String(!p.hidden));
});
$('#params-apply').addEventListener('click', () => {
  for (const [k, d] of Object.entries(st.S.param_defs)) {
    const v = $('#p-' + k).value;
    if (v === '' || Number(v) === d.default) url.delete(k); else url.set(k, v);
  }
  pushUrl(); refresh();
});
$('#params-reset').addEventListener('click', () => {
  for (const [k, d] of Object.entries(st.S.param_defs)) { url.delete(k); $('#p-' + k).value = d.default; }
  pushUrl(); refresh();
});
for (const [id, key] of [['#f-band', 'band'], ['#f-cat', 'cat'], ['#f-v', 'v']]) {
  $(id).value = st.f[key];
  $(id).addEventListener('change', (e) => { st.f[key] = e.target.value; st.shown = 200; pushUrl(); renderDiffs(); });
}
$('#f-call').value = st.f.call;
$('#f-call').addEventListener('input', (e) => { st.f.call = e.target.value.trim(); pushUrl(); renderDiffs(); });
$('#more').addEventListener('click', () => { st.shown += 300; renderDiffs(); });
for (const b of document.querySelectorAll('#hourly-unit button')) {
  b.addEventListener('click', () => { st.hourUnit = b.dataset.unit; if (st.S) renderHourly(st.S); });
}

const themes = ['auto', 'light', 'dark'];
function applyTheme(t) {
  if (t === 'auto') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', t);
  $('#theme').textContent = 'Theme: ' + t;
}
let theme = 'auto';
try { theme = localStorage.getItem('skimcmp-theme') || 'auto'; } catch (e) { /* private mode */ }
applyTheme(theme);
$('#theme').addEventListener('click', () => {
  theme = themes[(themes.indexOf(theme) + 1) % themes.length];
  applyTheme(theme);
  try { localStorage.setItem('skimcmp-theme', theme); } catch (e) { /* ignore */ }
});

// live: the server says when the logs changed; a slow poll covers a lost stream
// (?nolive=1: one snapshot, no stream — printing, headless screenshots)
if (!url.has('nolive')) {
  try {
    const es = new EventSource('/events');
    es.onmessage = () => { if (st.last) soon(); };
  } catch (e) { /* no SSE: the poll below still works */ }
  setInterval(soon, 60000);
}
let lastW = 0, rz = null;
window.addEventListener('resize', () => {
  clearTimeout(rz);
  rz = setTimeout(() => {
    const w = $('#ci').clientWidth;
    if (st.S && Math.abs(w - lastW) > 20) { lastW = w; render(); }
  }, 200);
});
refresh();
