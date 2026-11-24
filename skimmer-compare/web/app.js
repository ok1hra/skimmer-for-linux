// app.js — the public page of the skimmer comparison. Static: it reads
// data.json (computed by compare-web.py) once a minute and draws three
// charts. No input, no other request. Plain DOM + hand-drawn SVG; every string
// goes in through textContent, never innerHTML.
'use strict';

const $ = (s) => document.querySelector(s);
const SVGNS = 'http://www.w3.org/2000/svg';
const MIN_N = 5;          // a point resting on fewer stations/episodes is left out
const SIDES = [['L', 'var(--L)', 'var(--L-wash)'], ['W', 'var(--W)', 'var(--W-wash)']];

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
    e.setAttribute(k === 'cls' ? 'class' : k, v === true ? '' : v);
  }
}
function svgRoot(w, h, label) {
  return sv('svg', { viewBox: `0 0 ${w} ${h}`, role: 'img', 'aria-label': label });
}
const widthOf = (box) => Math.max(300, Math.round(box.clientWidth || 600));

// ---- formatting ---------------------------------------------------------------------
const pct = (p) => (p == null ? '—' : (100 * p).toFixed(1) + ' %');
const pctShort = (p) => (p == null ? '—' : Math.round(100 * p) + ' %');
const iso = (t) => new Date(t * 1000).toISOString();
const utc = (t) => iso(t).slice(5, 16).replace('T', ' ');
const hhmm = (t) => iso(t).slice(11, 16);
const hms = (t) => iso(t).slice(11, 19);
const num = (v) => (v == null ? '—' : Number(v).toLocaleString('en'));
const signed = (v, unit) => (v == null ? '—' : (v > 0 ? '+' : v < 0 ? '−' : '±') + Math.abs(v) + unit);
function dur(s) {
  s = Math.round(s || 0);
  if (s < 3600) return Math.round(s / 60) + ' min';
  const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
  return h >= 48 ? (s / 86400).toFixed(1) + ' d' : `${h} h ${m} min`;
}
function niceMax(v) {
  if (v <= 0) return 1;
  const e = Math.pow(10, Math.floor(Math.log10(v)));
  for (const k of [1, 2, 2.5, 5, 10]) if (k * e >= v) return k * e;
  return 10 * e;
}
const sideName = (s) => (s === 'L' ? 'L Linux skimmer' : 'W Windows skimmer');

// ---- tooltip ------------------------------------------------------------------------
const tip = $('#tip');
function tipShow(ev, head, rows) {
  tip.replaceChildren(el('div', { cls: 'h' }, head));
  for (const [side, value, label] of rows) {
    tip.append(el('div', { cls: 'r' },
      side ? el('span', { cls: 'key key-' + side }) : null,
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
function hover(node, fn, on, off) {
  node.setAttribute('tabindex', '0');
  const show = (e) => { const [h, r] = fn(); tipShow(e, h, r); if (on) on(); };
  const hide = () => { tipHide(); if (off) off(); };
  node.addEventListener('pointermove', show);
  node.addEventListener('focus', show);
  node.addEventListener('pointerleave', hide);
  node.addEventListener('blur', hide);
}

// ---- data ---------------------------------------------------------------------------
let D = null, timer = null;

async function load() {
  clearTimeout(timer);
  try {
    const r = await fetch('data.json', { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const next = await r.json();
    if (!D || next.generated !== D.generated) { D = next; render(); }
    else renderNotice();
  } catch (e) {
    showNotice('Cannot load the numbers: ' + e.message + '. Trying again in a minute.');
  } finally {
    $('#main').classList.toggle('busy', !D || D.state !== 'ok');
    const every = 1000 * Math.max(30, (D && D.every_s) || 60);
    timer = setTimeout(() => { if (!document.hidden) load(); }, D && D.state === 'loading' ? 5000 : every);
  }
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) load(); });

function showNotice(text, warn) {
  const box = $('#notice');
  box.textContent = text;
  box.hidden = !text;
  box.classList.toggle('warn', !!warn);
}
function renderNotice() {
  if (!D) return;
  if (D.state === 'loading') {
    const p = D.total ? Math.min(99, Math.floor(100 * D.read / D.total)) : 0;
    showNotice(`A new session is being loaded (${p} %) — the numbers follow shortly.`);
    return;
  }
  const say = [];
  for (const w of D.warnings || []) {
    if (w.kind !== 'feed_port') continue;
    const what = w.served
      ? `The Linux skimmer (L) serves its telnet feed on port ${w.served}, but the recorder reads port ${w.read}`
      : 'The Linux skimmer (L) reports its telnet feed off';
    say.push(`${what} (since ${utc(w.since)} UTC): L's spots are not recorded and this time is not compared.`);
  }
  const age = Date.now() / 1000 - D.generated;
  if (age > 3 * (D.every_s || 60) + 60) {
    say.push(`These numbers are ${dur(age)} old: the comparison is not being updated at the moment.`);
  } else if (D.session && D.session.stopped) {
    say.push('The recording of this session has stopped; the numbers are final.');
  }
  showNotice(say.join(' '), say.length && (D.warnings || []).length);
}

// ---- render -------------------------------------------------------------------------
function render() {
  renderNotice();
  if (!D || D.state !== 'ok') return;
  renderStatus();
  renderFacts();
  renderAbout();
  renderVerdicts();
  renderSnr();
  renderCI();
  renderTiles();
  renderBands();
  renderTimeline();
  renderDownloads();
  const J = D.session.joined;
  $('#scope').textContent = J
    ? `· two CW skimmers on the same air, live, decoder ${D.names.decoder} since ${utc(J.since)} UTC`
    : '· two CW skimmers on the same air, live, the current session';
  $('#dl-scope').textContent = J ? `the time since ${utc(D.session.start)} UTC (L's other decoders' time is in them, not compared)`
    : 'the current session only';
  $('#updated').textContent = (J ? `Decoder ${D.names.decoder} since ${utc(D.session.start)} UTC, `
    + `${J.sessions.length} recording session${J.sessions.length === 1 ? '' : 's'} joined · `
    : `Current session since ${utc(D.session.start)} UTC · `)
    + `numbers from ${hms(D.generated)} UTC · they reload every ${Math.round((D.every_s || 60))} s`;
}

function renderStatus() {
  const box = $('#status');
  box.replaceChildren();
  for (const [k, name] of [['L', 'L feed'], ['W', 'W feed'], ['RBN', 'RBN']]) {
    const up = D.feeds[k];
    box.append(el('span', { cls: 'pill' }, el('span', { cls: 'dot ' + (up ? 'up' : 'down') }),
      el('b', null, name), up ? 'up' : 'down'));
  }
  box.append(el('span', { cls: 'pill' }, 'compared ', el('b', null, dur(D.time.common_s))));
}

function fact(term, ...kids) {
  return [el('dt', null, term), el('dd', null, ...kids)];
}
function renderFacts() {
  const s = D.session, t = D.time, c = D.counts, F = D.filter || {};
  const end = s.live ? 'now' : utc(s.end);
  const state = s.live ? 'recording' : s.stopped ? 'stopped' : 'not recording';
  const share = (a, b) => (b ? Math.round(100 * a / b) + ' %' : '—');
  $('#facts').replaceChildren(
    ...(s.joined
      ? fact('Data', el('b', null, `${utc(s.start)} – ${end} UTC`),
        el('span', { cls: 'muted' }, ` · only the time L ran decoder ${D.names.decoder}, `
          + `${s.joined.sessions.length} recording session${s.joined.sessions.length === 1 ? '' : 's'} joined`
          + ` · ${state} · ${dur(t.span_s)} recorded`))
      : fact('Session', el('b', null, `${utc(s.start)} – ${end} UTC`),
        el('span', { cls: 'muted' }, ` · ${state} · ${dur(t.span_s)} recorded`))),
    ...fact('Compared', t.common_from == null ? 'nothing yet'
      : [el('b', null, `${utc(t.common_from)} – ${utc(t.common_to)} UTC`),
        el('span', { cls: 'muted' }, ` · ${dur(t.common_s)} with both feeds up (${share(t.common_s, t.span_s)} of the session)`)]),
    ...fact('Reference', el('b', null, `${num(c.rbn_spots)} RBN spots`),
      el('span', { cls: 'muted' }, ` from other skimmers on the compared bands`
        + (c.rbn_own ? ` (${num(c.rbn_own)} of our own left out)` : '')
        + ` · RBN connected ${share(t.rbn_s, t.span_s)} of the time · MASTER.SCP (${num(c.scp_calls)} calls) where RBN has nothing`)),
    ...fact('Spots', el('b', null, `L ${num(c.spots_L)} · W ${num(c.spots_W)}`),
      el('span', { cls: 'muted' }, ` CQ spots compared (W sent ${num(c.spots_W_all)} in all, with DE or no comment)`
        + ` → ${num(D.recall.n)} real stations, ${num(c.events)} compared episodes`)),
    ...(F.on && (c.filtered_L || c.filtered_W) ? fact('Call filter',
      el('b', null, `W ${num(c.filtered_W)} · L ${num(c.filtered_L)}`),
      el('span', { cls: 'muted' }, ` CQ spots left out: L spots only the calls its callsign filter lets through`
        + (F.about ? ` (${F.about})` : '') + `, so both sides are compared on those calls only`
        + (F.since != null ? ` (since ${utc(F.since)} UTC)` : ''))) : []),
  );
}

function renderAbout() {
  const N = D.names;
  $('#decoder').textContent = N.decoder || '—';
  $('#decoder-what').textContent = N.decoder_what ? `(${N.decoder_what})` : '';
  $('#L-about').textContent = N.L_about || '';
  $('#W-about').textContent = N.W_about || '';
}

function renderVerdicts() {
  const V = $('#verdicts');
  V.replaceChildren();
  for (const v of D.verdicts) {
    const b = v.winner
      ? el('span', { cls: 'badge' }, el('span', { cls: 'key key-' + v.winner }), v.winner + ' better')
      : el('span', { cls: 'badge none' }, 'undecided');
    V.append(el('div', { cls: 'verdict' }, el('span', { cls: 'm' }, v.metric), b,
      el('span', { cls: 't' }, v.winner ? v.text : v.text.replace(/^undecided — /, ''))));
  }
}

function tile(label, value, sub) {
  return el('div', { cls: 'tile' }, el('div', { cls: 'l' }, label),
    el('div', { cls: 'v' }, value), el('div', { cls: 's' }, sub));
}
function renderTiles() {
  const r = D.recall, b = D.bust, l = D.latency;
  const diff = r.n ? (r.only_L - r.only_W) / r.n : null;
  const fmtP = (p) => (p == null ? '—' : p < 0.001 ? 'p < 0.001' : 'p = ' + p.toPrecision(2));
  $('#tiles').replaceChildren(
    tile('Real stations', num(r.n), `${num(r.only_L)} only L · ${num(r.only_W)} only W`),
    tile('Recall difference L − W', diff == null ? '—' : signed(+(100 * diff).toFixed(1), ' pp'),
      `McNemar ${fmtP(r.p)}`),
    tile('Busted episodes L / W', `${num(b.L.k)} / ${num(b.W.k)}`,
      `of ${num(b.L.n)} / ${num(b.W.n)} episodes · ${fmtP(b.p)}`),
    tile('First spot, median W − L', l.median_W_minus_L == null ? '—'
      : signed(Math.round(l.median_W_minus_L), ' s'),
    `L first ${num(l.L_first)} · W first ${num(l.W_first)} · tie ${num(l.ties)}`),
  );
}

function renderBands() {
  const t = $('#bands');
  // L off: the compared time the Linux skimmer had the band paused on its own
  // (the thermal guard) — neither side is compared on that band meanwhile
  const anyOff = D.bands.some((b) => b.off_s > 0);
  const head = ['Band', 'Stations', 'Recall L', 'Recall W', 'Bust L', 'Bust W', 'First W − L',
    ...(anyOff ? ['L off'] : [])];
  const tip = 'compared time the Linux skimmer had this band paused (its CPU ran hot) — ' +
    'neither skimmer is compared on the band meanwhile';
  t.replaceChildren(el('thead', null, el('tr', null,
    head.map((h, i) => el('th', { cls: i ? 'num' : '', title: h === 'L off' ? tip : null }, h)))));
  const dur = (s) => (!s ? '—' : s < 3600 ? `${Math.round(s / 60)} min`
    : `${Math.floor(s / 3600)} h ${String(Math.round((s % 3600) / 60)).padStart(2, '0')} min`);
  const share = (k, n) => (n ? pctShort(k / n) : '—');
  const row = (name, n, L, W, bL, eL, bW, eW, lat, cls, off) => el('tr', { cls },
    el('td', null, name),
    ...[num(n), L.p == null ? '—' : pctShort(L.p), W.p == null ? '—' : pctShort(W.p),
      share(bL, eL), share(bW, eW), lat == null ? '—' : signed(Math.round(lat), ' s'),
      ...(anyOff ? [cls === 'total' ? '' : dur(off)] : [])]
      .map((v) => el('td', { cls: 'num' }, v)));
  const tb = el('tbody');
  for (const b of D.bands) {
    if (!b.n && !b.eps_L && !b.eps_W) continue;
    tb.append(row(b.band, b.n, b.L, b.W, b.bust_L, b.eps_L, b.bust_W, b.eps_W, b.median_W_minus_L,
      '', b.off_s));
  }
  const r = D.recall, bu = D.bust;
  tb.append(row('all', r.n, r.L, r.W, bu.L.k, bu.L.n, bu.W.k, bu.W.n, D.latency.median_W_minus_L, 'total'));
  t.append(tb);
}

function size(b) {
  return b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.max(1, Math.round(b / 1e3)) + ' kB';
}
function renderDownloads() {
  const L = D.downloads || [];
  $('#dl-card').hidden = !L.length;
  $('#downloads').replaceChildren(...L.map((d) => {
    // only our own fixed paths: download/<name>, nothing from elsewhere
    if (!/^download\/[a-zA-Z0-9.-]+$/.test(d.path)) return null;
    return el('li', null, el('a', { href: d.path, download: '' }, d.path.slice(9)),
      el('span', { cls: 'muted' }, `${size(d.bytes)} · ${d.what}`));
  }).filter(Boolean));
}

// ---- chart 1: recall and bust rate by SNR, two panels over one SNR axis ------------------
function binLabel(b) {
  if (b.lo == null) return `< ${b.hi}`;
  if (b.hi == null) return `≥ ${b.lo}`;
  return `${b.lo}–${b.hi}`;
}
function renderSnr() {
  const box = $('#snr'), bins = D.snr.bins || [];
  $('#snr-pairs').textContent = num(D.snr.pairs);
  if (!bins.length || !bins.some((b) => b.n)) {
    box.replaceChildren(el('p', { cls: 'muted' }, 'No real station compared yet in this session.'));
    return;
  }
  const W = widthOf(box);
  const H = Math.round(Math.min(Math.max(W * 0.92, 360), 620));
  const m = { l: 46, r: 26, t: 24, b: 44 }, gap = 40;
  const plotH = H - m.t - m.b - gap, hTop = Math.round(plotH * 0.58), hBot = plotH - hTop;
  const top0 = m.t, bot0 = m.t + hTop + gap;
  const svg = svgRoot(W, H, 'Recall and bust rate of both skimmers by SNR');
  const n = bins.length, band = (W - m.l - m.r) / n;
  const cx = (i) => m.l + band * (i + 0.5);
  const ok = (d) => d && d.n >= MIN_N && d.p != null;
  // the bust panel's own scale: the largest shown bust rate, with headroom
  let bmax = 0;
  for (const b of bins) for (const k of ['bustL', 'bustW']) if (ok(b[k])) bmax = Math.max(bmax, b[k].p);
  const bm = [0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1].find((v) => v >= bmax * 1.2) || 1;
  const panels = [
    { title: 'Recall — real stations found', y0: top0, h: hTop, max: 1, ticks: [0, .25, .5, .75, 1], key: (s) => s },
    { title: 'Bust rate — episodes with a wrong call', y0: bot0, h: hBot, max: bm, ticks: [0, bm / 2, bm], key: (s) => 'bust' + s },
  ];
  for (const P of panels) {
    P.y = (p) => P.y0 + (1 - Math.min(p, P.max) / P.max) * P.h;
    svg.append(sv('text', { cls: 'ttl', x: m.l, y: P.y0 - 9 }, P.title));
    for (const v of P.ticks) {
      svg.append(sv('line', { cls: 'grid', x1: m.l, x2: W - m.r, y1: P.y(v), y2: P.y(v) }));
      svg.append(sv('text', { x: m.l - 6, y: P.y(v) + 4, 'text-anchor': 'end' }, pctShort(v)));
    }
    svg.append(sv('line', { cls: 'axis', x1: m.l, x2: W - m.r, y1: P.y(0), y2: P.y(0) }));
  }
  // confidence bands first, then lines, then points on top
  const runs = (key) => {     // consecutive bins with a point
    const out = [];
    let cur = [];
    bins.forEach((b, i) => {
      if (ok(b[key])) cur.push(i);
      else { if (cur.length) out.push(cur); cur = []; }
    });
    if (cur.length) out.push(cur);
    return out;
  };
  for (const P of panels) {
    for (const [s, , wash] of SIDES) {
      const key = P.key(s);
      for (const r of runs(key)) {
        const pts = r.map((i) => [cx(i), P.y(bins[i][key].hi), P.y(bins[i][key].lo)]);
        if (pts.length === 1) {
          const [x, a, z] = pts[0];
          svg.append(sv('rect', { x: x - 6, y: a, width: 12, height: Math.max(1, z - a), fill: wash, rx: 2 }));
          continue;
        }
        const d = 'M' + pts.map(([x, a]) => `${x.toFixed(1)},${a.toFixed(1)}`).join('L')
          + 'L' + pts.slice().reverse().map(([x, , z]) => `${x.toFixed(1)},${z.toFixed(1)}`).join('L') + 'Z';
        svg.append(sv('path', { d, fill: wash }));
      }
    }
  }
  const last = {};
  for (const P of panels) {
    for (const [s, color] of SIDES) {
      const key = P.key(s);
      for (const r of runs(key)) {
        if (r.length < 2) continue;
        svg.append(sv('path', { d: 'M' + r.map((i) => `${cx(i).toFixed(1)},${P.y(bins[i][key].p).toFixed(1)}`).join('L'),
          fill: 'none', stroke: color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
      }
      bins.forEach((b, i) => {
        if (!ok(b[key])) return;
        svg.append(sv('circle', { cx: cx(i), cy: P.y(b[key].p), r: 4, fill: color, stroke: 'var(--surface)', 'stroke-width': 2 }));
        if (P === panels[0]) last[s] = [cx(i), P.y(b[key].p)];
      });
    }
  }
  // direct labels at the right end of the recall lines, pushed apart when close
  if (last.L && last.W) {
    let yl = last.L[1], yw = last.W[1];
    if (Math.abs(yl - yw) < 13) {
      const mid = (yl + yw) / 2, up = yl <= yw ? -1 : 1;
      yl = mid + up * 6.5; yw = mid - up * 6.5;
    }
    svg.append(sv('text', { cls: 'lbl', x: last.L[0] + 9, y: yl + 4 }, 'L'));
    svg.append(sv('text', { cls: 'lbl', x: last.W[0] + 9, y: yw + 4 }, 'W'));
  }
  // the SNR axis, under the bust panel
  svg.append(sv('text', { cls: 'lbl', x: m.l - 6, y: H - 24, 'text-anchor': 'end' }, 'SNR dB'));
  svg.append(sv('text', { x: m.l - 6, y: H - 9, 'text-anchor': 'end' }, 'n'));
  bins.forEach((b, i) => {
    svg.append(sv('text', { cls: 'lbl', x: cx(i), y: H - 24, 'text-anchor': 'middle' }, binLabel(b)));
    svg.append(sv('text', { x: cx(i), y: H - 9, 'text-anchor': 'middle' }, num(b.n)));
  });
  // hover: one column per SNR bin across both panels
  bins.forEach((b, i) => {
    const hl = sv('rect', { x: cx(i) - band / 2 + 1, y: top0, width: band - 2, height: bot0 + hBot - top0,
      fill: 'var(--surface-2)', 'fill-opacity': 0.5, visibility: 'hidden' });
    svg.insertBefore(hl, svg.firstChild);
    const hit = sv('rect', { cls: 'hit', x: cx(i) - band / 2, y: top0, width: band, height: bot0 + hBot - top0 });
    const row = (s, d, what, of) => [s, ok(d) ? pct(d.p) : '—',
      `${s} ${what} · ${num(d.k)} of ${num(d.n)} ${of}` + (ok(d) ? ` (95 % ${pctShort(d.lo)}–${pctShort(d.hi)})` : '')];
    hover(hit, () => [`SNR ${binLabel(b)} dB · ${num(b.n)} real stations`, [
      row('L', b.L, 'recall', 'stations'), row('W', b.W, 'recall', 'stations'),
      row('L', b.bustL, 'bust rate', 'episodes'), row('W', b.bustW, 'bust rate', 'episodes')]],
    () => hl.setAttribute('visibility', 'visible'), () => hl.setAttribute('visibility', 'hidden'));
    svg.append(hit);
  });
  box.replaceChildren(svg);
}

// ---- chart 2: recall and bust rate with 95 % confidence intervals --------------------
function renderCI() {
  const box = $('#ci'), W = widthOf(box);
  const rows = [['Recall', D.recall, 'stations'], ['Bust rate', D.bust, 'episodes']];
  const m = { l: 76, r: 58, t: 8, b: 24 }, rowH = 48, H = m.t + rows.length * rowH + m.b;
  const svg = svgRoot(W, H, 'Recall and bust rate of both skimmers with confidence intervals');
  const x = (p) => m.l + p * (W - m.l - m.r);
  for (const v of [0, .25, .5, .75, 1]) {
    svg.append(sv('line', { cls: 'grid', x1: x(v), x2: x(v), y1: m.t, y2: H - m.b }));
    svg.append(sv('text', { x: x(v), y: H - 8, 'text-anchor': 'middle' }, pctShort(v)));
  }
  rows.forEach(([name, blk, of], i) => {
    const y0 = m.t + i * rowH;
    svg.append(sv('text', { cls: 'lbl', x: 0, y: y0 + rowH / 2 + 4 }, name));
    for (const [[side, color], dy] of [[SIDES[0], 15], [SIDES[1], 33]]) {
      const d = blk[side];
      if (!d || d.p == null) continue;
      const y = y0 + dy, g = sv('g');
      g.append(sv('line', { x1: x(d.lo), x2: x(d.hi), y1: y, y2: y, stroke: color, 'stroke-width': 2, 'stroke-linecap': 'round' }));
      g.append(sv('circle', { cx: x(d.p), cy: y, r: 5, fill: color, stroke: 'var(--surface)', 'stroke-width': 2 }));
      g.append(sv('text', { cls: 'val', x: x(d.hi) + 9, y: y + 4 }, pct(d.p)));
      g.append(sv('rect', { cls: 'hit', x: x(d.lo) - 8, y: y - 9, width: Math.max(24, x(d.hi) - x(d.lo) + 16), height: 18 }));
      hover(g, () => [`${name} · ${sideName(side)}`, [[side, pct(d.p), `${num(d.k)} of ${num(d.n)} ${of}`],
        [null, `${pct(d.lo)} – ${pct(d.hi)}`, '95 % confidence']]]);
      svg.append(g);
    }
  });
  box.replaceChildren(svg);
}

// ---- chart 3: recall over time + real stations per interval ----------------------------
function timeTicks(a, b, width) {
  const span = b - a, most = Math.max(3, Math.min(8, Math.floor(width / 80)));
  const steps = [600, 1800, 3600, 2 * 3600, 3 * 3600, 6 * 3600, 12 * 3600, 86400, 2 * 86400, 7 * 86400];
  const step = steps.find((s) => span / s <= most) || 7 * 86400;
  const out = [];
  for (let t = Math.ceil(a / step) * step; t <= b; t += step) out.push(t);
  return { out, fmt: step >= 86400 ? (t) => iso(t).slice(5, 10) : span > 86400 ? utc : hhmm };
}
function renderTimeline() {
  const T = D.timeline, ta = D.session.start, tb = Math.min(D.session.end, D.generated);
  const box = $('#timeline'), W = widthOf(box);
  const m = { l: 44, r: 12, t: 14, b: 26 }, H = 230;
  const svg = svgRoot(W, H, 'Recall of both skimmers over time');
  const x = (t) => m.l + ((t - ta) / Math.max(1, tb - ta)) * (W - m.l - m.r);
  const y = (p) => m.t + (1 - p) * (H - m.t - m.b);
  const shade = (a, b) => sv('rect', { x: x(Math.max(ta, a)), y: m.t, height: H - m.t - m.b,
    width: Math.max(1, x(Math.min(tb, b)) - x(Math.max(ta, a))), fill: 'var(--out)' });
  for (const [a, b] of [...T.outages, ...T.other, ...T.dead.map((d) => [d.t0, d.t1])]) svg.append(shade(a, b));
  for (const v of [0, .25, .5, .75, 1]) {
    svg.append(sv('line', { cls: 'grid', x1: m.l, x2: W - m.r, y1: y(v), y2: y(v) }));
    svg.append(sv('text', { x: m.l - 6, y: y(v) + 4, 'text-anchor': 'end' }, pctShort(v)));
  }
  const ticks = timeTicks(ta, tb, W - m.l - m.r);
  for (const t of ticks.out) svg.append(sv('text', { x: x(t), y: H - 8, 'text-anchor': 'middle' }, ticks.fmt(t)));
  for (const s of T.silences) {
    const r = sv('rect', { x: x(Math.max(ta, s.t0)), y: 2, height: 6, rx: 2,
      width: Math.max(3, x(Math.min(tb, s.t1)) - x(Math.max(ta, s.t0))), fill: s.side === 'L' ? 'var(--L)' : 'var(--W)' });
    const other = s.side === 'L' ? 'W' : 'L';
    hover(r, () => [`${sideName(s.side)} silent on ${s.band}`, [[null, `${hhmm(s.t0)}–${hhmm(s.t1)}`, 'UTC'],
      [other, num(s.other), `spots by ${other} meanwhile`]]]);
    svg.append(r);
  }
  const mid = (r) => r.t + T.step / 2;
  const shown = T.rows.filter((r) => r.n && mid(r) >= ta && mid(r) <= tb);
  for (const [side, color] of SIDES) {
    if (shown.length > 1) {
      svg.append(sv('path', { d: 'M' + shown.map((r) => `${x(mid(r)).toFixed(1)},${y(r[side] / r.n).toFixed(1)}`).join('L'),
        fill: 'none', stroke: color, 'stroke-width': 2, 'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    }
    for (const r of shown) {
      svg.append(sv('circle', { cx: x(mid(r)), cy: y(r[side] / r.n), r: 4, fill: color, stroke: 'var(--surface)', 'stroke-width': 2 }));
    }
  }
  svg.append(sv('line', { cls: 'axis', x1: m.l, x2: W - m.r, y1: y(0), y2: y(0) }));
  // crosshair, snapped to the nearest interval
  const cross = sv('line', { cls: 'cross', y1: m.t, y2: H - m.b, visibility: 'hidden' });
  svg.append(cross);
  const ov = sv('rect', { x: m.l, y: m.t, width: W - m.l - m.r, height: H - m.t - m.b, fill: 'transparent' });
  ov.addEventListener('pointermove', (ev) => {
    if (!shown.length) return;
    const b = svg.getBoundingClientRect();
    const t = ta + ((ev.clientX - b.left) * (W / b.width) - m.l) / (W - m.l - m.r) * (tb - ta);
    const r = shown.reduce((a, c) => (Math.abs(mid(c) - t) < Math.abs(mid(a) - t) ? c : a));
    cross.setAttribute('x1', x(mid(r))); cross.setAttribute('x2', x(mid(r)));
    cross.setAttribute('visibility', 'visible');
    tipShow(ev, `${utc(r.t)} – ${hhmm(r.t + T.step)} UTC`, [
      ['L', pct(r.L / r.n), `L ${num(r.L)} of ${num(r.n)} real stations`],
      ['W', pct(r.W / r.n), `W ${num(r.W)} of ${num(r.n)} real stations`],
      [null, `${num(r.bustL)} / ${num(r.bustW)}`, 'busted episodes L / W'],
      [null, dur(r.common_s), 'compared']]);
  });
  ov.addEventListener('pointerleave', () => { cross.setAttribute('visibility', 'hidden'); tipHide(); });
  svg.append(ov);
  box.replaceChildren(svg);

  // real stations per interval: one neutral series on the same time axis
  const H2 = 120, m2 = { l: 44, r: 12, t: 8, b: 22 };
  const v = svgRoot(W, H2, 'Real stations per interval');
  const max = niceMax(Math.max(1, ...T.rows.map((r) => r.n)));
  const y2 = (k) => m2.t + (1 - k / max) * (H2 - m2.t - m2.b);
  for (const k of [0, max / 2, max]) {
    v.append(sv('line', { cls: 'grid', x1: m2.l, x2: W - m2.r, y1: y2(k), y2: y2(k) }));
    v.append(sv('text', { x: m2.l - 6, y: y2(k) + 4, 'text-anchor': 'end' }, Math.round(k)));
  }
  const bw = Math.max(2, Math.min(24, x(ta + T.step) - x(ta) - 2));
  for (const r of T.rows) {
    if (mid(r) < ta || mid(r) > tb) continue;
    const g = sv('g'), h = y2(0) - y2(r.n), bx = x(mid(r)) - bw / 2;
    if (h > 0.5) {
      const rr = Math.min(4, bw / 2, h), b0 = y2(0);
      g.append(sv('path', { fill: 'var(--muted)', d: `M${bx},${b0}V${b0 - h + rr}Q${bx},${b0 - h} ${bx + rr},${b0 - h}`
        + `H${bx + bw - rr}Q${bx + bw},${b0 - h} ${bx + bw},${b0 - h + rr}V${b0}Z` }));
    }
    g.append(sv('rect', { cls: 'hit', x: x(mid(r)) - Math.max(12, bw / 2), y: m2.t, width: Math.max(24, bw), height: H2 - m2.t - m2.b }));
    hover(g, () => [`${utc(r.t)} UTC`, [[null, num(r.n), 'real stations'], [null, dur(r.common_s), 'compared']]]);
    v.append(g);
  }
  v.append(sv('line', { cls: 'axis', x1: m2.l, x2: W - m2.r, y1: y2(0), y2: y2(0) }));
  for (const t of ticks.out) v.append(sv('text', { x: x(t), y: H2 - 6, 'text-anchor': 'middle' }, ticks.fmt(t)));
  $('#volume').replaceChildren(v);

  const rows = [
    ...T.outages.map(([a, b]) => ['not compared', '', a, b, '']),
    ...T.other.map(([a, b]) => ['another L decoder', '', a, b, '']),
    ...T.dead.map((d) => [`${d.side} spotted nothing`, 'all', d.t0, d.t1, 'while the other side kept spotting']),
    ...T.silences.map((s) => [`${s.side} silent`, s.band, s.t0, s.t1, `${num(s.other)} spots by the other side`]),
  ].sort((p, q) => p[2] - q[2]);
  $('#silences').replaceChildren(rows.length
    ? el('div', { cls: 'table-wrap' }, el('table', null,
      el('thead', null, el('tr', null, ['What', 'Band', 'From (UTC)', 'To', 'Length', ''].map((h) => el('th', null, h)))),
      el('tbody', null, rows.map(([w, b, a, z, n]) => el('tr', null, el('td', null, w), el('td', null, b),
        el('td', null, utc(a)), el('td', null, utc(z)), el('td', null, dur(z - a)), el('td', null, n))))))
    : el('p', { cls: 'muted' }, 'No outage or silence in this session.'));
}

let rz = null, lastW = 0;
window.addEventListener('resize', () => {
  clearTimeout(rz);
  rz = setTimeout(() => {
    const w = $('#timeline').clientWidth;
    if (D && Math.abs(w - lastW) > 20) { lastW = w; render(); }
  }, 200);
});
load();
