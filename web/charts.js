// charts.js — the few charts the statistics tab draws, in plain SVG.
//
// Specs (the dataviz method): the categorical hues in their validated fixed
// order (CSS --s1…--s8, colour follows the band, never its rank); 2 px surface
// gap between stacked segments, ≤ 24 px columns with a 4 px rounded data end;
// 2 px lines; hairline recessive grid; one y-axis per chart; text in text
// colours only; every mark has a hover/focus tooltip, values lead.
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
    return spanH > 24 ? `${two(d.getUTCDate())}.${two(d.getUTCMonth() + 1)}. ${hm}` : hm;
  }
  function frame(box, height) {
    box.innerHTML = '';
    const W = Math.max(box.clientWidth, 280);
    const svg = el('svg', { viewBox: `0 0 ${W} ${height}`, height, role: 'img' }, box);
    return { svg, W, H: height };
  }
  // y grid + ticks (4 lines), x ticks at ~6 positions; returns the plot area
  function axes(svg, W, H, x, yMax, left = 40) {
    const m = { l: left, r: 8, t: 6, b: 22 };
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
  function mirror(box, x, halves, fmtV, halfH = 160) {
    const m = { l: 44, r: 8, t: 8, b: 22 };
    const two = halves.length > 1;
    const { svg, W, H } = frame(box, m.t + halfH * (two ? 2 : 1) + m.b);
    const n = x.t.length, pw = W - m.l - m.r;
    const zero = m.t + halfH;
    const slot = pw / n, bw = Math.min(24, Math.max(1, slot - (slot > 4 ? 2 : 0)));
    const tot = halves.map((h) => x.t.map((_, i) => h.series.reduce((a, s) => a + (s.values[i] || 0), 0)));
    const max = tot.map((t) => Math.max(0, ...t) || 1);
    halves.forEach((h, k) => {
      const dir = k ? 1 : -1;                                   // up for the first
      const yv = (v) => zero + dir * (halfH * v) / max[k];
      // grid: 0, 50 %, 100 % of this half's maximum
      for (const f of [0.5, 1]) {
        const y = yv(max[k] * f);
        el('line', { x1: m.l, x2: W - m.r, y1: y, y2: y, stroke: css('--grid'), 'stroke-width': 1,
                     'shape-rendering': 'crispEdges' }, svg);
        const t = el('text', { x: m.l - 6, y: y + 4, 'text-anchor': 'end', 'font-size': 11, fill: css('--tick') }, svg);
        t.textContent = compact(max[k] * f);
      }
      const lab = el('text', { x: m.l + 6, y: k ? zero + halfH - 6 : m.t + 12, 'font-size': 11,
                               'font-weight': 600, fill: css('--mut'), stroke: css('--panel'),
                               'stroke-width': 3, 'paint-order': 'stroke' }, svg);
      lab.textContent = h.name;
      h.lab = lab;
      for (let i = 0; i < n; i++) {
        let acc = 0;
        const x0 = m.l + i * slot + (slot - bw) / 2;
        const top = h.series.reduce((q, s, j) => (s.values[i] > 0 ? j : q), -1);
        h.series.forEach((s, j) => {
          const v = s.values[i] || 0;
          if (v <= 0) return;
          const len = (halfH * v) / max[k];
          const gap = j === top ? 0 : Math.min(2, len / 2);      // surface gap before the next one
          const ll = Math.max(0.5, len - gap);
          const inner = zero + dir * (halfH * acc) / max[k];      // the end nearer zero
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
        tt.show(m.l + (i + 1) * slot, zero - (halfH * tot[0][i]) / max[0], x.label(i), rows);
        r.setAttribute('fill', 'rgba(127,127,127,.08)');
      };
      r.addEventListener('pointerenter', show);
      r.addEventListener('focus', show);
      r.addEventListener('pointerleave', () => { tt.hide(); r.setAttribute('fill', 'transparent'); });
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
    const m = { l: 48, r: 8, t: 2 };
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
    return max;
  }

  // ---- one line, crosshair tooltip -------------------------------------------------
  function line(box, x, values, color, fmtV, unit) {
    const { svg, W, H } = frame(box, 160);
    const yMax = niceMax(Math.max(0, ...values.filter((v) => v != null)));
    const a = axes(svg, W, H, x, yMax, 36);
    const n = x.t.length, px = (i) => a.l + ((i + 0.5) * a.pw) / n, py = (v) => a.t + a.ph - (a.ph * v) / yMax;
    let d = '', pen = false;
    values.forEach((v, i) => {
      if (v == null) { pen = false; return; }
      d += `${pen ? 'L' : 'M'}${px(i).toFixed(1)},${py(v).toFixed(1)}`;
      pen = true;
    });
    el('path', { d, fill: 'none', stroke: color, 'stroke-width': 2, 'stroke-linejoin': 'round',
                 'stroke-linecap': 'round' }, svg);
    const cross = el('line', { y1: a.t, y2: a.t + a.ph, stroke: css('--axis'), 'stroke-width': 1,
                               visibility: 'hidden' }, svg);
    const dot = el('circle', { r: 4, fill: color, stroke: css('--panel'), 'stroke-width': 2,
                               visibility: 'hidden' }, svg);
    const tt = tip(box);
    const hit = el('rect', { x: a.l, y: a.t, width: a.pw, height: a.ph, fill: 'transparent' }, svg);
    hit.addEventListener('pointermove', (e) => {
      const r = svg.getBoundingClientRect();
      const mx = ((e.clientX - r.left) * W) / r.width;
      const i = Math.min(n - 1, Math.max(0, Math.floor(((mx - a.l) * n) / a.pw)));
      cross.setAttribute('x1', px(i)); cross.setAttribute('x2', px(i));
      cross.setAttribute('visibility', 'visible');
      const v = values[i];
      if (v != null) {
        dot.setAttribute('cx', px(i)); dot.setAttribute('cy', py(v)); dot.setAttribute('visibility', 'visible');
      } else dot.setAttribute('visibility', 'hidden');
      tt.show((px(i) * r.width) / W, (py(v ?? 0) * r.height) / H, x.label(i),
              [{ value: v == null ? 'no data' : fmtV(v), label: unit }]);
    });
    hit.addEventListener('pointerleave', () => {
      tt.hide(); cross.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden');
    });
  }

  return { mirror, heat, line, seriesColor };
})();
