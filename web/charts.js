// charts.js — the few charts the statistics tab draws, in plain SVG.
//
// Specs (the dataviz method): the categorical hues in their validated fixed
// order (CSS --s1…--s8, colour follows the band, never its rank); 2 px surface
// gap between stacked segments, ≤ 24 px columns with a 4 px rounded data end;
// 2 px lines; hairline recessive grid; one y-axis per chart; text in text
// colours only; every mark has a hover/focus tooltip, values lead; one
// crosshair runs through all the charts at the bucket under the cursor.
'use strict';

const Charts = (() => {
  const NS = 'http://www.w3.org/2000/svg';
  const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const el = (tag, attrs = {}, parent) => {
    const e = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (parent) parent.appendChild(e);
    return e;
  };
  const seriesColor = (i) => css(`--s${(i % 8) + 1}`);
  // one plot frame for every chart: their time axes line up under each other
  // (the right margin holds the CPU chart's second axis)
  const ML = 48, MR = 46;

  // ---- one crosshair for every chart -------------------------------------------------
  // The charts share their buckets (one time axis). The cursor over any of them
  // puts a vertical line at its bucket in all of them, moving together; the
  // tooltip stays with the chart under the cursor.
  const sync = new Map();                        // box → { at(i), off() }
  function syncAt(i) { for (const [box, h] of sync) { if (box.isConnected) h.at(i); else sync.delete(box); } }
  function syncOff() { for (const h of sync.values()) h.off(); }
  // a hairline at bucket centre x(i), above the marks, never in the way of the pointer
  function crossLine(box, svg, y1, y2, xOf) {
    const ln = el('line', { y1, y2, stroke: css('--fg'), 'stroke-opacity': 0.45, 'stroke-width': 1,
                            visibility: 'hidden', 'pointer-events': 'none', 'shape-rendering': 'crispEdges' }, svg);
    sync.set(box, {
      at(i) { const x = xOf(i); ln.setAttribute('x1', x); ln.setAttribute('x2', x); ln.setAttribute('visibility', 'visible'); },
      off() { ln.setAttribute('visibility', 'hidden'); },
    });
  }

  // ---- scales and axes -------------------------------------------------------------
  function niceMax(v) {
    if (!(v > 0)) return 1;
    const p = 10 ** Math.floor(Math.log10(v));
    for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p;
    return 10 * p;
  }
  const compact = (v) => v >= 1e6 ? `${+(v / 1e6).toFixed(1)}M` : v >= 1e3 ? `${+(v / 1e3).toFixed(1)}K`
    : `${+v.toFixed(v < 10 && v % 1 ? 1 : 0)}`;
  const two = (n) => String(n).padStart(2, '0');
  function timeLabel(t, spanH) {
    const d = new Date(t * 1000);
    const hm = `${two(d.getUTCHours())}:${two(d.getUTCMinutes())}`;
    const dm = `${two(d.getUTCDate())}.${two(d.getUTCMonth() + 1)}.`;
    if (spanH > 2160) return `${dm}${String(d.getUTCFullYear()).slice(2)}`;   // months: the date
    if (spanH > 168) return dm;
    return spanH > 24 ? `${dm} ${hm}` : hm;
  }
  function frame(box, height) {
    box.innerHTML = '';
    const W = Math.max(box.clientWidth, 280);
    const svg = el('svg', { viewBox: `0 0 ${W} ${height}`, height, role: 'img' }, box);
    return { svg, W, H: height };
  }
  // ---- the night at the station, the plot darkened while the sun is down -------------
  // x.site { lat, lon } and x.sun { at(t) → the subsolar point, alt(lat, lon, s) → ° }
  // (map.js's formulae: the date and the place both count). Each ~3 px column of
  // the plot is shaded by its MEAN darkness over its own stretch of time: within
  // a day the dusk is a soft edge, over days soft stripes, and once a column is
  // a day or longer its shade is that day's share of the night — no thicket of
  // edges over months and years, and winter reads darker than summer. Dark from
  // the civil dusk (−6°), light from +1° (the map's grey line uses the same).
  const NIGHT_MAX = 0.38;
  let nightOn = true, nightN = 0;
  const darkness = (alt) => (alt >= 1 ? 0 : alt <= -6 ? 1 : (1 - alt) / 7);
  function night(svg, l, t, pw, ph, x, scale = 1) {
    if (!nightOn || !x.site || !x.sun || !x.step || !x.t.length || pw <= 0 || ph <= 0) return;
    const n = x.t.length, t0 = x.t[0], span = n * x.step;
    const cols = Math.max(2, Math.min(400, Math.round(pw / 3))), dt = span / cols;
    const whole = dt >= 86400, len = whole ? 86400 : dt;
    const k = whole ? 48 : Math.max(4, Math.min(24, Math.ceil(len / 900)));
    const g = el('linearGradient', { id: `night${++nightN}`, x1: 0, x2: 1, y1: 0, y2: 0 }, el('defs', {}, svg));
    for (let c = 0; c < cols; c++) {
      const from = whole ? t0 + (c + 0.5) * dt - 43200 : t0 + c * dt;
      let d = 0;
      for (let j = 0; j < k; j++) {
        d += darkness(x.sun.alt(x.site.lat, x.site.lon, x.sun.at(from + ((j + 0.5) * len) / k)));
      }
      el('stop', { offset: ((c + 0.5) / cols).toFixed(4), 'stop-color': '#000',
                   'stop-opacity': ((NIGHT_MAX * scale * d) / k).toFixed(3) }, g);
    }
    el('rect', { x: l, y: t, width: pw, height: ph, fill: `url(#${g.id})`, 'pointer-events': 'none' }, svg);
  }

  // y grid + ticks (4 lines), x ticks at ~6 positions; returns the plot area
  function axes(svg, W, H, x, yMax, left = 40, right = 8) {
    const m = { l: left, r: right, t: 6, b: 22 };
    const pw = W - m.l - m.r, ph = H - m.t - m.b;
    for (let i = 0; i <= 4; i++) {
      const v = (yMax * i) / 4, y = m.t + ph - (ph * i) / 4;
      el('line', { x1: m.l, x2: W - m.r, y1: y, y2: y, stroke: i ? css('--grid') : css('--axis'),
                   'stroke-width': 1, 'shape-rendering': 'crispEdges' }, svg);
      const t = el('text', { x: m.l - 6, y: y + 4, 'text-anchor': 'end', 'font-size': 11,
                             fill: css('--tick') }, svg);
      t.textContent = compact(v);
    }
    const n = x.t.length, every = Math.max(1, Math.ceil(n / Math.max(2, Math.floor(pw / 90))));
    for (let i = 0; i < n; i += every) {
      const t = el('text', { x: m.l + ((i + 0.5) * pw) / n, y: H - 6, 'text-anchor': 'middle',
                             'font-size': 11, fill: css('--tick') }, svg);
      t.textContent = timeLabel(x.t[i], x.spanH);
    }
    return { ...m, pw, ph };
  }

  // ---- tooltip -------------------------------------------------------------------
  function tip(box) {
    let t = box.querySelector('.tip');
    if (!t) { t = document.createElement('div'); t.className = 'tip'; t.hidden = true; box.appendChild(t); }
    return {
      show(px, py, head, rows) {
        t.replaceChildren();
        const h = document.createElement('div'); h.className = 'h'; h.textContent = head; t.appendChild(h);
        for (const r of rows) {
          const d = document.createElement('div'); d.className = 'r';
          const b = document.createElement('b'); b.textContent = r.value;
          const s = document.createElement('span'); s.textContent = r.label;
          if (r.color) { const i = document.createElement('i'); i.style.background = r.color; d.appendChild(i); }
          d.append(b, s); t.appendChild(d);
        }
        t.hidden = false;
        const bw = box.clientWidth, tw = t.offsetWidth;
        t.style.left = `${Math.min(Math.max(0, px + 12), bw - tw)}px`;
        t.style.top = `${Math.max(0, py - t.offsetHeight - 8)}px`;
      },
      hide() { t.hidden = true; },
    };
  }

  // ---- mirrored stacked columns: CW up, RTTY down from one zero line -----------------
  // halves: [{ name, series }, …] — the first grows up, the second (if any) down.
  // Each half has its own scale, its tallest column reaching 100 % of the half.
  // log: the column's height is log10(1 + total) — the quiet hours readable
  // beside a contest peak; its bands share the column in proportion as before,
  // the grid at the powers of ten.
  function mirror(box, x, halves, fmtV, halfH = 160, log = false) {
    const m = { l: ML, r: MR, t: 8, b: 22 };
    const two = halves.length > 1;
    const { svg, W, H } = frame(box, m.t + halfH * (two ? 2 : 1) + m.b);
    const n = x.t.length, pw = W - m.l - m.r;
    const zero = m.t + halfH;
    const slot = pw / n, bw = Math.min(24, Math.max(1, slot - (slot > 4 ? 2 : 0)));
    const tot = halves.map((h) => x.t.map((_, i) => h.series.reduce((a, s) => a + (s.values[i] || 0), 0)));
    const max = tot.map((t) => Math.max(0, ...t) || 1);
    night(svg, m.l, m.t, pw, halfH * (two ? 2 : 1), x);
    // a column's length in px for a total v of half k
    const len = (k, v) => (log ? (halfH * Math.log10(1 + Math.max(0, v))) / Math.log10(1 + max[k])
      : (halfH * v) / max[k]);
    halves.forEach((h, k) => {
      const dir = k ? 1 : -1;                                   // up for the first
      const yv = (v) => zero + dir * len(k, v);
      // grid: 0, 50 %, 100 % of this half's maximum — or the powers of ten
      const marks = [];
      if (log) { for (let p = 1; p <= max[k]; p *= 10) marks.push(p); if (max[k] / marks[marks.length - 1] > 3) marks.push(max[k]); }
      else marks.push(max[k] * 0.5, max[k]);
      for (const v of marks) {
        const y = yv(v);
        el('line', { x1: m.l, x2: W - m.r, y1: y, y2: y, stroke: css('--grid'), 'stroke-width': 1,
                     'shape-rendering': 'crispEdges' }, svg);
        const t = el('text', { x: m.l - 6, y: y + 4, 'text-anchor': 'end', 'font-size': 11, fill: css('--tick') }, svg);
        t.textContent = compact(v);
      }
      const lab = el('text', { x: m.l + 6, y: k ? zero + halfH - 6 : m.t + 12, 'font-size': 11,
                               'font-weight': 700, fill: h.color || css('--mut'), stroke: css('--panel'),
                               'stroke-width': 3, 'paint-order': 'stroke' }, svg);
      lab.textContent = h.name;
      h.lab = lab;
      for (let i = 0; i < n; i++) {
        let acc = 0;
        const x0 = m.l + i * slot + (slot - bw) / 2;
        const col = len(k, tot[k][i]);                         // the whole column; bands share it
        const top = h.series.reduce((q, s, j) => (s.values[i] > 0 ? j : q), -1);
        h.series.forEach((s, j) => {
          const v = s.values[i] || 0;
          if (v <= 0) return;
          const sl = (col * v) / tot[k][i];
          const gap = j === top ? 0 : Math.min(2, sl / 2);       // surface gap before the next one
          const ll = Math.max(0.5, sl - gap);
          const inner = zero + dir * (col * acc) / tot[k][i];     // the end nearer zero
          acc += v;
          const y0 = k ? inner : inner - ll;                      // top edge of the segment
          if (j === top && ll > 4 && bw >= 6) {                   // 4 px rounded data end
            const r = 4;
            el('path', { d: k
              ? `M${x0},${y0}V${y0 + ll - r}q0,${r} ${r},${r}h${bw - 2 * r}q${r},0 ${r},-${r}V${y0}Z`
              : `M${x0},${y0 + ll}V${y0 + r}q0,-${r} ${r},-${r}h${bw - 2 * r}q${r},0 ${r},${r}V${y0 + ll}Z`,
            fill: s.color }, svg);
          } else {
            el('rect', { x: x0, y: y0, width: bw, height: ll, fill: s.color }, svg);
          }
        });
      }
    });
    el('line', { x1: m.l, x2: W - m.r, y1: zero, y2: zero, stroke: css('--axis'), 'stroke-width': 1,
                 'shape-rendering': 'crispEdges' }, svg);
    for (const h of halves) { svg.appendChild(h.lab); delete h.lab; }       // above the columns
    const t0 = el('text', { x: m.l - 6, y: zero + 4, 'text-anchor': 'end', 'font-size': 11, fill: css('--tick') }, svg);
    t0.textContent = '0';
    const every = Math.max(1, Math.ceil(n / Math.max(2, Math.floor(pw / 90))));
    for (let i = 0; i < n; i += every) {
      const t = el('text', { x: m.l + ((i + 0.5) * pw) / n, y: H - 6, 'text-anchor': 'middle',
                             'font-size': 11, fill: css('--tick') }, svg);
      t.textContent = timeLabel(x.t[i], x.spanH);
    }
    // hit targets: the whole slot, both halves
    const tt = tip(box);
    const plotH = halfH * (two ? 2 : 1);
    crossLine(box, svg, m.t, m.t + plotH, (i) => Math.round(m.l + (i + 0.5) * slot) + 0.5);
    for (let i = 0; i < n; i++) {
      const r = el('rect', { x: m.l + i * slot, y: m.t, width: slot, height: plotH, fill: 'transparent',
                             tabindex: -1 }, svg);
      const show = () => {
        const rows = [];
        halves.forEach((h, k) => {
          rows.push({ value: fmtV(tot[k][i]), label: `${h.name}, all bands` });
          for (const s of [...h.series].reverse()) {
            if (s.values[i] != null && s.values[i] > 0) rows.push({ value: fmtV(s.values[i]), label: `${h.name} ${s.name}`, color: s.color });
          }
        });
        tt.show(m.l + (i + 1) * slot, zero - len(0, tot[0][i]), x.label(i), rows);
        r.setAttribute('fill', 'rgba(127,127,127,.08)');
        syncAt(i);
      };
      r.addEventListener('pointerenter', show);
      r.addEventListener('focus', show);
      r.addEventListener('pointerleave', () => { tt.hide(); r.setAttribute('fill', 'transparent'); syncOff(); });
    }
  }

  // ---- heatmap: a row per band, a cell per bucket, one-hue ramp --------------------
  function mix(a, b, t) {
    const p = (h) => [1, 3, 5].map((k) => parseInt(h.slice(k, k + 2), 16));
    const A = p(a), B = p(b);
    return `rgb(${A.map((c, i) => Math.round(c + (B[i] - c) * t)).join(',')})`;
  }
  // rows: [{ name, parts: [{ label, values | null, lo, hi }, …] }] — a band is one
  // block, its parts stacked inside it with no gap (CW over RTTY), a gap between
  // bands. Each part label has its own ramp and its own maximum.
  function heat(box, x, rows, fmtV, what) {
    const partH = 13, bandGap = 5;
    const np = Math.max(1, ...rows.map((r) => r.parts.length));
    const blockH = partH * np;
    const { svg, W, H } = frame(box, rows.length * (blockH + bandGap) + 26);
    const m = { l: ML, r: MR, t: 2 };
    const n = x.t.length, pw = W - m.l - m.r, cw = pw / n;
    const max = {};
    for (const r of rows) for (const p of r.parts) {
      if (!p.values) continue;
      max[p.label] = Math.max(max[p.label] || 1, ...p.values.filter((v) => v != null));
    }
    const gap = cw > 4 ? 1 : 0;
    const tt = tip(box);
    rows.forEach((r, j) => {
      const yy = m.t + j * (blockH + bandGap);
      const t = el('text', { x: m.l - 6, y: yy + blockH / 2 + 4, 'text-anchor': 'end', 'font-size': 12,
                             fill: css('--mut') }, svg);
      t.textContent = r.name;
      r.parts.forEach((p, k) => {
        const py = yy + k * partH;
        if (!p.values) {                                         // the band does not decode it
          el('rect', { x: m.l, y: py, width: pw - gap, height: partH, fill: css('--grid'), opacity: 0.5 }, svg);
          return;
        }
        const lo = css(p.lo), hi = css(p.hi);
        p.values.forEach((v, i) => {
          if (v == null) return;
          const c = el('rect', { x: m.l + i * cw, y: py, width: Math.max(0.5, cw - gap), height: partH,
                                 fill: mix(lo, hi, Math.sqrt(v / max[p.label])) }, svg);
          c.addEventListener('pointerenter', () => {
            tt.show(m.l + (i + 1) * cw, py, `${r.name} ${p.label} · ${x.label(i)}`, [{ value: fmtV(v), label: what }]);
            c.setAttribute('stroke', css('--fg'));
          });
          c.addEventListener('pointerleave', () => { tt.hide(); c.removeAttribute('stroke'); });
        });
      });
    });
    const every = Math.max(1, Math.ceil(n / Math.max(2, Math.floor(pw / 90))));
    for (let i = 0; i < n; i += every) {
      const t = el('text', { x: m.l + (i + 0.5) * cw, y: H - 6, 'text-anchor': 'middle', 'font-size': 11,
                             fill: css('--tick') }, svg);
      t.textContent = timeLabel(x.t[i], x.spanH);
    }
    // the shared crosshair: the bucket under the cursor anywhere in the rows
    const plotB = m.t + rows.length * (blockH + bandGap) - bandGap;
    night(svg, m.l, m.t, pw, plotB - m.t, x, 0.6);   // over the cells (they fill the plot), lighter
    crossLine(box, svg, m.t, plotB, (i) => Math.round(m.l + (i + 0.5) * cw) + 0.5);
    svg.addEventListener('pointermove', (e) => {
      const r = svg.getBoundingClientRect(), mx = ((e.clientX - r.left) * W) / r.width;
      if (mx < m.l || mx > m.l + pw) { syncOff(); return; }
      syncAt(Math.min(n - 1, Math.max(0, Math.floor((mx - m.l) / cw))));
    });
    svg.addEventListener('pointerleave', syncOff);
    return max;
  }

  // ---- stacked areas under a total line, crosshair tooltip --------------------------
  // layers: [{ name, values, color }] stacked from zero (a bucket where any layer is
  // null draws no area); total: the line on top. right: { name, values, color, fmtV }
  // — a second quantity as a line on its own scale, the right axis in its colour.
  function area(box, x, layers, total, fmtV, totalName, right) {
    const { svg, W, H } = frame(box, 180);
    const yMax = niceMax(Math.max(0, ...total.filter((v) => v != null)));
    const a = axes(svg, W, H, x, yMax, ML, MR);
    night(svg, a.l, a.t, a.pw, a.ph, x);
    const n = x.t.length, px = (i) => a.l + ((i + 0.5) * a.pw) / n,
      py = (v) => a.t + a.ph - (a.ph * Math.min(v, yMax)) / yMax;
    const ok = (i) => layers.every((l) => l.values[i] != null);
    const base = new Array(n).fill(0);
    for (const l of layers) {
      let d = '';
      for (let i = 0; i < n;) {
        if (!ok(i)) { i++; continue; }
        let j = i;
        while (j + 1 < n && ok(j + 1)) j++;
        const up = [], lo = [];
        for (let k = i; k <= j; k++) {
          up.push(`${px(k).toFixed(1)},${py(base[k] + l.values[k]).toFixed(1)}`);
          lo.push(`${px(k).toFixed(1)},${py(base[k]).toFixed(1)}`);
        }
        if (i === j) {                                         // a lone bucket: a thin bar
          const w = Math.max(1, a.pw / n / 2);
          d += `M${(px(i) - w).toFixed(1)},${py(base[i]).toFixed(1)}V${py(base[i] + l.values[i]).toFixed(1)}` +
               `H${(px(i) + w).toFixed(1)}V${py(base[i]).toFixed(1)}Z`;
        } else {
          d += `M${up.join('L')}L${lo.reverse().join('L')}Z`;
        }
        i = j + 1;
      }
      if (d) el('path', { d, fill: l.color, 'fill-opacity': 0.8, stroke: 'none' }, svg);
      for (let i = 0; i < n; i++) if (ok(i)) base[i] += l.values[i];
    }
    let d = '', pen = false;
    total.forEach((v, i) => {
      if (v == null) { pen = false; return; }
      d += `${pen ? 'L' : 'M'}${px(i).toFixed(1)},${py(v).toFixed(1)}`;
      pen = true;
    });
    const ink = css('--fg');
    el('path', { d, fill: 'none', stroke: ink, 'stroke-width': 1.5, 'stroke-linejoin': 'round',
                 'stroke-opacity': 0.75 }, svg);
    let ry = null;
    if (right) {                                   // the second scale, at the right
      const rMax = niceMax(Math.max(0, ...right.values.filter((v) => v != null)));
      ry = (v) => a.t + a.ph - (a.ph * Math.min(v, rMax)) / rMax;
      for (let i = 0; i <= 4; i++) {
        const t = el('text', { x: W - a.r + 6, y: a.t + a.ph - (a.ph * i) / 4 + 4, 'font-size': 11,
                               fill: right.color }, svg);
        t.textContent = right.fmtTick ? right.fmtTick((rMax * i) / 4) : compact((rMax * i) / 4);
      }
      let rd = '', rp = false;
      right.values.forEach((v, i) => {
        if (v == null) { rp = false; return; }
        rd += `${rp ? 'L' : 'M'}${px(i).toFixed(1)},${ry(v).toFixed(1)}`;
        rp = true;
      });
      el('path', { d: rd, fill: 'none', stroke: right.color, 'stroke-width': 2, 'stroke-linejoin': 'round',
                   'stroke-linecap': 'round' }, svg);
    }
    crossLine(box, svg, a.t, a.t + a.ph, (i) => Math.round(px(i)) + 0.5);
    const tt = tip(box);
    const hit = el('rect', { x: a.l, y: a.t, width: a.pw, height: a.ph, fill: 'transparent' }, svg);
    hit.addEventListener('pointermove', (e) => {
      const r = svg.getBoundingClientRect();
      const mx = ((e.clientX - r.left) * W) / r.width;
      const i = Math.min(n - 1, Math.max(0, Math.floor(((mx - a.l) * n) / a.pw)));
      syncAt(i);
      const rows = [{ value: total[i] == null ? 'no data' : fmtV(total[i]), label: totalName }];
      if (ok(i)) for (const l of [...layers].reverse()) rows.push({ value: fmtV(l.values[i]), label: l.name, color: l.color });
      else if (total[i] != null) rows.push({ value: '–', label: 'no split recorded' });
      if (right) rows.push({ value: right.values[i] == null ? '–' : right.fmtV(right.values[i]), label: right.name, color: right.color });
      tt.show((px(i) * r.width) / W, (py(total[i] ?? 0) * r.height) / H, x.label(i), rows);
    });
    hit.addEventListener('pointerleave', () => { tt.hide(); syncOff(); });
  }

  // ---- one quantity as a line on its own scale (not from zero) ---------------------
  let rampN = 0;
  function rampAt(ramp, v) {                       // the colour at v, mixed between the stops
    if (v <= ramp[0][0]) return ramp[0][1];
    for (let k = 1; k < ramp.length; k++) {
      if (v <= ramp[k][0]) return mix(ramp[k - 1][1], ramp[k][1], (v - ramp[k - 1][0]) / (ramp[k][0] - ramp[k - 1][0]));
    }
    return ramp[ramp.length - 1][1];
  }
  // opts: { name, color, fmtV, fmtTick, height, minSpan, ramp } — the scale spans
  // the values' range rounded out to tens, at least minSpan wide. ramp:
  // [[value, '#rrggbb'], …] ascending — the line takes the colour of its height
  // (a gradient up the plot), the dots too.
  function line(box, x, values, opts) {
    const { svg, W, H } = frame(box, opts.height || 110);
    const v = values.filter((y) => y != null);
    const span = opts.minSpan || 20;
    let lo = v.length ? Math.floor(Math.min(...v) / 10) * 10 : 0;
    let hi = v.length ? Math.ceil(Math.max(...v) / 10) * 10 : span;
    if (opts.mark) { lo = Math.min(lo, Math.floor(opts.mark.value / 10) * 10); hi = Math.max(hi, Math.ceil(opts.mark.value / 10) * 10); }
    if (hi - lo < span) { const c = (hi + lo) / 2; lo = Math.floor((c - span / 2) / 10) * 10; hi = lo + span; }
    const m = { l: ML, r: MR, t: 6, b: 22 };
    const pw = W - m.l - m.r, ph = H - m.t - m.b, n = x.t.length;
    const px = (i) => m.l + ((i + 0.5) * pw) / n, py = (y) => m.t + ph - (ph * (y - lo)) / (hi - lo);
    night(svg, m.l, m.t, pw, ph, x);
    for (let i = 0; i <= 2; i++) {
      const y = lo + ((hi - lo) * i) / 2, yy = py(y);
      el('line', { x1: m.l, x2: W - m.r, y1: yy, y2: yy, stroke: i ? css('--grid') : css('--axis'),
                   'stroke-width': 1, 'shape-rendering': 'crispEdges' }, svg);
      const t = el('text', { x: m.l - 6, y: yy + 4, 'text-anchor': 'end', 'font-size': 11,
                             fill: opts.ramp ? css('--tick') : opts.color }, svg);
      t.textContent = opts.fmtTick(y);
    }
    const every = Math.max(1, Math.ceil(n / Math.max(2, Math.floor(pw / 90))));
    for (let i = 0; i < n; i += every) {
      const t = el('text', { x: px(i), y: H - 6, 'text-anchor': 'middle', 'font-size': 11, fill: css('--tick') }, svg);
      t.textContent = timeLabel(x.t[i], x.spanH);
    }
    if (opts.mark) {                               // a level to compare with: dashed
      const my = Math.round(py(opts.mark.value)) + 0.5;
      el('line', { x1: m.l, x2: W - m.r, y1: my, y2: my, stroke: css('--bad'), 'stroke-width': 1,
                   'stroke-dasharray': '5 4', 'shape-rendering': 'crispEdges' }, svg);
      el('title', {}, svg).textContent = opts.mark.label;
    }
    let d = '', pen = false;
    values.forEach((y, i) => {
      if (y == null) { pen = false; return; }
      d += `${pen ? 'L' : 'M'}${px(i).toFixed(1)},${py(y).toFixed(1)}`;
      pen = true;
    });
    const at = (y) => (opts.ramp ? rampAt(opts.ramp, y) : opts.color);
    let stroke = opts.color;
    if (opts.ramp) {                               // bottom → top: the ramp's colours
      const id = `ramp${++rampN}`, g = el('linearGradient', { id, gradientUnits: 'userSpaceOnUse',
        x1: 0, x2: 0, y1: py(lo), y2: py(hi) }, el('defs', {}, svg));
      for (let k = 0; k <= 20; k++) el('stop', { offset: k / 20, 'stop-color': at(lo + ((hi - lo) * k) / 20) }, g);
      stroke = `url(#${id})`;
    }
    el('path', { d, fill: 'none', stroke, 'stroke-width': 2, 'stroke-linejoin': 'round',
                 'stroke-linecap': 'round' }, svg);
    values.forEach((y, i) => {                     // a bucket alone between gaps: a dot
      if (y != null && values[i - 1] == null && values[i + 1] == null) {
        el('circle', { cx: px(i), cy: py(y), r: 2, fill: at(y) }, svg);
      }
    });
    if (!v.length) {
      const t = el('text', { x: m.l + pw / 2, y: m.t + ph / 2 + 4, 'text-anchor': 'middle', 'font-size': 12,
                             fill: css('--tick') }, svg);
      t.textContent = 'not recorded in this range';
    }
    crossLine(box, svg, m.t, m.t + ph, (i) => Math.round(px(i)) + 0.5);
    const tt = tip(box);
    const hit = el('rect', { x: m.l, y: m.t, width: pw, height: ph, fill: 'transparent' }, svg);
    hit.addEventListener('pointermove', (e) => {
      const r = svg.getBoundingClientRect(), mx = ((e.clientX - r.left) * W) / r.width;
      const i = Math.min(n - 1, Math.max(0, Math.floor(((mx - m.l) * n) / pw)));
      syncAt(i);
      tt.show((px(i) * r.width) / W, (py(values[i] ?? lo) * r.height) / H, x.label(i),
              [{ value: values[i] == null ? '–' : opts.fmtV(values[i]), label: opts.name,
                 color: values[i] == null ? opts.color : at(values[i]) }]);
    });
    hit.addEventListener('pointerleave', () => { tt.hide(); syncOff(); });
  }

  return { mirror, heat, area, line, seriesColor,
           setNight(on) { nightOn = !!on; } };
})();
