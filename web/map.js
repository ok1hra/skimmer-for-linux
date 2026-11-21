// map.js — the status page's azimuthal map: the spots of the last half hour
// around the station, the way IP-rotator draws its map (web/map/ has the
// data): an azimuthal equidistant projection — distance and bearing true from
// the centre — land, borders, DXCC prefixes, the grey line, range rings.
//
// AUTO zooms to about 2000 km and widens as far as the farthest spot shown,
// so the end stations stay on the map (a ladder of steps, so it does not
// twitch at every refresh); the wheel takes over, AUTO gives it back. The
// centre is the station's locator (Settings; a manager gets it whole, the
// public page its first four characters only), or without one the DXCC of the station's call, or
// Paris (JN18) without either. A spot is placed by its DXCC
// (the prefix's label point), so the spots of one country share a point:
// they are fanned out around it, a dot each. The cursor reads azimuth,
// distance and locator.
// Per-viewer choices (layers, zoom) are kept in localStorage.
'use strict';

const SkMap = (() => {
  const R = 6371, rad = (d) => (d * Math.PI) / 180, deg = (r) => (r * 180) / Math.PI;
  const BAND_COLOR = { '160m': '#8e6cff', '80m': '#4f8dff', '60m': '#7fa3c8', '40m': '#18b0c9', '30m': '#27c26b',
    '20m': '#c7d23a', '17m': '#ffb21f', '15m': '#ff7a2f', '12m': '#ff4f6d', '10m': '#e24fd8', '6m': '#ff9ad5' };
  const bandColor = (b) => BAND_COLOR[b] || '#b8c4d2';

  // ---- geography --------------------------------------------------------------------
  function locToLL(l) {
    l = String(l || '').toUpperCase();
    if (!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(l)) return null;
    const six = l.length === 6;
    return {
      lon: (l.charCodeAt(0) - 65) * 20 - 180 + Number(l[2]) * 2 + (six ? (l.charCodeAt(4) - 65 + 0.5) / 12 : 1),
      lat: (l.charCodeAt(1) - 65) * 10 - 90 + Number(l[3]) + (six ? (l.charCodeAt(5) - 65 + 0.5) / 24 : 0.5),
    };
  }
  function llToLoc(lat, lon) {
    lon = ((lon + 180) % 360 + 360) % 360; lat = Math.min(89.9999, Math.max(-89.9999, lat)) + 90;
    const A = (n) => String.fromCharCode(65 + n);
    const fl = Math.floor(lon / 20), fa = Math.floor(lat / 10);
    lon -= fl * 20; lat -= fa * 10;
    const sl = Math.floor(lon / 2), sa = Math.floor(lat);
    lon -= sl * 2; lat -= sa;
    return A(fl) + A(fa) + sl + sa + A(Math.min(23, Math.floor(lon * 12))).toLowerCase() + A(Math.min(23, Math.floor(lat * 24))).toLowerCase();
  }
  function geo(from, lat, lon) {                    // distance (km) and bearing (°)
    const p1 = rad(from.lat), p2 = rad(lat), dl = rad(lon - from.lon);
    const c = Math.acos(Math.min(1, Math.max(-1, Math.sin(p1) * Math.sin(p2) + Math.cos(p1) * Math.cos(p2) * Math.cos(dl))));
    const az = (deg(Math.atan2(Math.sin(dl) * Math.cos(p2), Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl))) + 360) % 360;
    return { km: R * c, az };
  }
  // a call's DXCC: the longest prefix (after IP-rotator's aliases) among the dataset's labels
  let PFX = null;
  function locate(call) {
    if (typeof DXCC_PREFIX_LABELS === 'undefined') return null;
    if (!PFX) {
      PFX = {};
      for (const [la, lo, p, , name] of DXCC_PREFIX_LABELS) if (!PFX[p]) PFX[p] = { lat: la / 100, lon: lo / 100, name, prefix: p };
    }
    const parts = String(call).toUpperCase().split('/').filter((x) => x && !/^(P|M|MM|AM|QRP|\d)$/.test(x));
    const base = parts.sort((a, b) => b.length - a.length)[0] || '';
    const canon = window.canonicalizeDxccPrefixAlias ? window.canonicalizeDxccPrefixAlias(base) : base;
    for (const cand of [canon, base]) {
      if (PFX[cand]) return PFX[cand];
      for (let n = Math.min(4, cand.length); n >= 1; n--) if (PFX[cand.slice(0, n)]) return PFX[cand.slice(0, n)];
    }
    return null;
  }
  // the sun: subsolar point and altitude (IP-rotator's formulae)
  function subsolar(t) {
    const jd = t / 86400 + 2440587.5, n = jd - 2451545;
    const L = ((280.46 + 0.9856474 * n) % 360 + 360) % 360, g = rad(((357.528 + 0.9856003 * n) % 360 + 360) % 360);
    const lam = rad(L + 1.915 * Math.sin(g) + 0.02 * Math.sin(2 * g)), eps = rad(23.439 - 0.0000004 * n);
    const dec = Math.asin(Math.sin(eps) * Math.sin(lam)), ra = deg(Math.atan2(Math.cos(eps) * Math.sin(lam), Math.cos(lam)));
    const gmst = ((280.46061837 + 360.98564736629 * n) % 360 + 360) % 360;
    return { lat: deg(dec), lon: ((ra - gmst + 540) % 360) - 180 };
  }
  function sunAlt(lat, lon, s) {
    const v = Math.sin(rad(lat)) * Math.sin(rad(s.lat)) + Math.cos(rad(lat)) * Math.cos(rad(s.lat)) * Math.cos(rad(lon - s.lon));
    return deg(Math.asin(Math.min(1, Math.max(-1, v))));
  }

  // ---- state ----------------------------------------------------------------------------
  const store = (() => { try { return JSON.parse(localStorage.getItem('skmap') || '{}'); } catch (e) { return {}; } })();
  // every layer on by default; a viewer's choices are kept (v2: SNR joined them, on)
  if (store.v !== 2) { store.db = true; store.v = 2; }
  const opt = { grayline: true, borders: true, prefixes: true, lines: true, calls: true, db: true, zoom: 0, ...store };
  delete opt.center;                               // a typed-in centre (before v0.8.4) is gone
  const save = () => { try { localStorage.setItem('skmap', JSON.stringify(opt)); } catch (e) { /* private window */ } };
  let visitorCall = '';
  let el = null, home = '', homeCall = '', spots = [], view = null, baseKey = '', base = null, grayKey = '', gray = null;

  const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  // the centre: the locator, else the call's DXCC, else Paris — and what it is
  function centerFor(loc, call) {
    const l = locToLL(loc);
    if (l) return { ll: l, what: loc };
    const d = call && locate(call);
    if (d) return { ll: { lat: d.lat, lon: d.lon }, what: `${d.name} — ${call}'s DXCC (no locator in Settings)` };
    return { ll: locToLL('JN18'), what: 'JN18 (Paris — no locator and no call in Settings)' };
  }
  function centerOf() { return centerFor(home, homeCall); }
  function centerLL() { return centerOf().ll; }

  // ---- projection for the current view ----------------------------------------------------
  function makeView(size, zoom) {
    const c = centerLL(), cx = size / 2, cy = size / 2, rr = size / 2 - 2;
    const p1 = rad(c.lat), sp1 = Math.sin(p1), cp1 = Math.cos(p1);
    return {
      size, zoom, c, cx, cy, rr,
      P(lat, lon) {
        const p2 = rad(lat), dl = rad(lon - c.lon), sp2 = Math.sin(p2), cp2 = Math.cos(p2), cdl = Math.cos(dl);
        const k = Math.acos(Math.min(1, Math.max(-1, sp1 * sp2 + cp1 * cp2 * cdl)));
        const th = Math.atan2(Math.sin(dl) * cp2, cp1 * sp2 - sp1 * cp2 * cdl);
        const r = ((R * k) / zoom) * rr;
        return [cx + Math.sin(th) * r, cy - Math.cos(th) * r];
      },
      inv(x, y) {                                   // pixel → lat/lon (null outside)
        const dx = x - cx, dy = cy - y, rho = Math.hypot(dx, dy);
        if (rho > rr) return null;
        if (rho < 1e-6) return { lat: c.lat, lon: c.lon };
        const k = (rho / rr) * (zoom / R), sk = Math.sin(k), ck = Math.cos(k);
        const lat = deg(Math.asin(ck * sp1 + (dy * sk * cp1) / rho));
        let lon = c.lon + deg(Math.atan2(dx * sk, rho * cp1 * ck - dy * sp1 * sk));
        lon = ((lon + 540) % 360) - 180;
        return { lat, lon };
      },
    };
  }

  // ---- layers --------------------------------------------------------------------------
  function polyline(g, v, line, close) {
    let pen = false, prev = null;
    g.beginPath();
    for (const [la, lo] of line) {
      const p = v.P(la / 100, lo / 100);
      if (prev && Math.hypot(p[0] - prev[0], p[1] - prev[1]) > v.rr * 0.5) { if (close) return false; pen = false; }
      if (pen) g.lineTo(p[0], p[1]); else g.moveTo(p[0], p[1]);
      pen = true; prev = p;
    }
    if (close) g.closePath();
    return true;
  }
  function drawBase(v) {
    const key = [v.size, v.zoom.toFixed(0), v.c.lat.toFixed(3), v.c.lon.toFixed(3), opt.borders, opt.prefixes, devicePixelRatio].join('|');
    if (key === baseKey) return base;
    const dpr = devicePixelRatio || 1, cv = document.createElement('canvas');
    cv.width = cv.height = v.size * dpr;
    const g = cv.getContext('2d');
    g.scale(dpr, dpr);
    g.save();
    g.beginPath(); g.arc(v.cx, v.cy, v.rr, 0, Math.PI * 2); g.fillStyle = '#08111b'; g.fill(); g.clip();
    if (typeof LAND_OUTLINES !== 'undefined') {
      g.fillStyle = '#1b2a3c'; g.strokeStyle = '#2f4560'; g.lineWidth = 0.7;
      for (const line of LAND_OUTLINES) {
        if (line.length < 3 || line[0][0] < -6000) continue;     // Antarctica wraps the rim
        if (polyline(g, v, line, true)) { g.fill(); g.stroke(); }
      }
    }
    if (opt.borders && typeof COUNTRY_BORDERS !== 'undefined') {
      g.strokeStyle = 'rgba(125,143,166,.32)'; g.lineWidth = 0.6;
      for (const line of COUNTRY_BORDERS) { polyline(g, v, line, false); g.stroke(); }
    }
    // range rings with their distance, bearing spokes every 30°
    const ring = v.zoom <= 2500 ? 500 : v.zoom <= 5000 ? 1000 : v.zoom <= 10000 ? 2000 : 5000;
    g.strokeStyle = 'rgba(125,143,166,.16)'; g.fillStyle = '#5d708a'; g.font = '11px RC, sans-serif'; g.lineWidth = 1;
    for (let km = ring; km < v.zoom; km += ring) {
      const r = (km / v.zoom) * v.rr;
      g.beginPath(); g.arc(v.cx, v.cy, r, 0, Math.PI * 2); g.stroke();
      g.fillText(`${km >= 1000 ? `${km / 1000}k` : km} km`, v.cx + 3, v.cy - r - 3);
    }
    for (let a = 0; a < 360; a += 30) {
      g.beginPath(); g.moveTo(v.cx, v.cy); g.lineTo(v.cx + Math.sin(rad(a)) * v.rr, v.cy - Math.cos(rad(a)) * v.rr); g.stroke();
    }
    if (opt.prefixes && typeof DXCC_PREFIX_LABELS !== 'undefined') {
      const fs = v.zoom <= 3500 ? 12 : v.zoom <= 8000 ? 11 : 10;
      const max = v.zoom <= 3500 ? 82 : v.zoom <= 8000 ? 68 : v.zoom <= 14000 ? 34 : 22;
      const cand = DXCC_PREFIX_LABELS.map((r) => ({ p: v.P(r[0] / 100, r[1] / 100), t: r[2], a: r[3] }));
      if (v.zoom <= 8000 && typeof STATE_ABBR_LABELS !== 'undefined') {
        for (const r of STATE_ABBR_LABELS) cand.push({ p: v.P(r[0] / 100, r[1] / 100), t: r[2], a: r[3] * 40, st: true });
      }
      cand.sort((a, b) => b.a - a.a);
      g.font = `700 ${fs}px RC, sans-serif`; g.textAlign = 'center'; g.textBaseline = 'middle';
      const placed = [];
      for (const c of cand) {
        if (placed.length >= max) break;
        const w = g.measureText(c.t).width + 8, h = fs + 4, b = [c.p[0] - w / 2, c.p[1] - h / 2, c.p[0] + w / 2, c.p[1] + h / 2];
        if (Math.hypot(c.p[0] - v.cx, c.p[1] - v.cy) > v.rr - 8) continue;
        if (placed.some((o) => b[0] < o[2] && b[2] > o[0] && b[1] < o[3] && b[3] > o[1])) continue;
        placed.push(b);
        g.fillStyle = c.st ? 'rgba(160,178,200,.38)' : 'rgba(170,190,214,.55)';
        g.fillText(c.t, c.p[0], c.p[1]);
      }
      g.textAlign = 'start'; g.textBaseline = 'alphabetic';
    }
    g.restore();
    g.strokeStyle = '#2f4560'; g.lineWidth = 1.5; g.beginPath(); g.arc(v.cx, v.cy, v.rr, 0, Math.PI * 2); g.stroke();
    // compass letters at the rim
    g.fillStyle = '#7d8fa6'; g.font = '700 11px RC, sans-serif'; g.textAlign = 'center'; g.textBaseline = 'middle';
    [['N', 0], ['E', 90], ['S', 180], ['W', 270]].forEach(([t, a]) => g.fillText(t, v.cx + Math.sin(rad(a)) * (v.rr - 10), v.cy - Math.cos(rad(a)) * (v.rr - 10)));
    baseKey = key; base = cv;
    return cv;
  }
  function drawGray(v) {                            // night, shaded by the sun's altitude
    const minute = Math.floor(Date.now() / 60000);
    const key = [v.size, v.zoom.toFixed(0), v.c.lat.toFixed(3), v.c.lon.toFixed(3), minute].join('|');
    if (key === grayKey) return gray;
    const cv = document.createElement('canvas'); cv.width = cv.height = v.size;
    const g = cv.getContext('2d'), s = subsolar(minute * 60), step = 4;
    for (let y = 0; y < v.size; y += step) {
      for (let x = 0; x < v.size; x += step) {
        const q = v.inv(x + step / 2, y + step / 2);
        if (!q) continue;
        const alt = sunAlt(q.lat, q.lon, s);
        if (alt >= 1) continue;
        g.fillStyle = `rgba(0,0,0,${(alt <= -5 ? 0.5 : 0.5 * (1 - alt) / 6).toFixed(3)})`;
        g.fillRect(x, y, step, step);
      }
    }
    const sp = geo(v.c, s.lat, s.lon);
    if (sp.km < v.zoom) {                            // the subsolar point
      const p = v.P(s.lat, s.lon);
      g.fillStyle = '#ffd84d'; g.beginPath(); g.arc(p[0], p[1], 5, 0, 7); g.fill();
    }
    grayKey = key; gray = cv;
    return cv;
  }

  // ---- the visitor ------------------------------------------------------------------------
  // A visitor signed in from another country is marked on the map — a star at
  // their call's DXCC, its call beside it — and AUTO zooms out to keep it on.
  // Not for a call of the station's own DXCC (or the centre's, without a call).
  function visitorAt() {
    if (!visitorCall) return null;
    const w = locate(visitorCall);
    if (!w) return null;
    const mine = homeCall && locate(homeCall);
    if (mine ? mine.name === w.name : (() => { const c = centerLL(); return Math.abs(c.lat - w.lat) < 0.01 && Math.abs(c.lon - w.lon) < 0.01; })()) return null;
    return w;
  }
  function star(g, x, y, r) {
    g.beginPath();
    for (let k = 0; k < 10; k++) {
      const a = -Math.PI / 2 + (k * Math.PI) / 5, rr = k % 2 ? r * 0.45 : r;
      g.lineTo(x + rr * Math.cos(a), y + rr * Math.sin(a));
    }
    g.closePath();
  }
  // where the star and its call go (their box keeps the spots' labels off), or null
  function placeVisitor(g, v, boxes) {
    const w = visitorAt();
    if (!w) return null;
    const d = geo(v.c, w.lat, w.lon);
    if (d.km > v.zoom) return null;                  // off the map (a zoom by hand)
    const [x, y] = v.P(w.lat, w.lon);
    g.font = '700 14px RC, "Roboto Condensed", sans-serif';
    const tw = g.measureText(visitorCall).width;
    const lx = x + 15 + tw > v.size - 4 ? x - 15 - tw : x + 15;
    boxes.push([Math.min(x - 12, lx), y - 12, Math.max(x + 12, lx + tw), y + 12]);
    return { x, y, lx, w, km: d.km };
  }
  function paintVisitor(g, p) {                      // on top of everything
    g.save();
    g.shadowColor = 'rgba(0,0,0,.6)'; g.shadowBlur = 6;
    star(g, p.x, p.y, 11);
    g.fillStyle = '#ffffff'; g.fill();
    g.shadowBlur = 0;
    g.lineWidth = 2.5; g.strokeStyle = css('--accent') || '#39d98a'; g.stroke();
    g.font = '700 14px RC, "Roboto Condensed", sans-serif';
    g.textAlign = 'left'; g.textBaseline = 'middle';
    g.lineWidth = 4; g.strokeStyle = 'rgba(5,9,14,.85)'; g.strokeText(visitorCall, p.lx, p.y);
    g.fillStyle = '#ffffff'; g.fillText(visitorCall, p.lx, p.y);
    g.restore();
  }

  // ---- drawing ---------------------------------------------------------------------------
  const ZOOM_STEPS = [2000, 2500, 3000, 3500, 4000, 5000, 6000, 7000, 8000, 10000, 12000, 14000, 16000, 18000, 20000];
  function autoZoom() {
    const c = centerLL();
    const vis = visitorAt();
    const far = Math.max(0, ...spots.filter((s) => s.where).map((s) => geo(c, s.where.lat, s.where.lon).km),
                         vis ? geo(c, vis.lat, vis.lon).km : 0);
    const want = far * 1.1 + 200;                   // the farthest dot and its call inside the rim
    return ZOOM_STEPS.find((z) => z >= want) || 20000;
  }
  // ---- the labels -------------------------------------------------------------------------
  // Each call (with SNR its SNR in dB; SNR without calls: the number alone)
  // beside its dot when there is room, on the side away from the centre first.
  // Else it moves out along the line from the map's centre through its dot —
  // the leaders fan out from the centre like the spots' own lines — tried a
  // little to either side of it and further out step by step. A place is free
  // when the label is clear of the labels and leaders placed, of the dots and
  // of the rim, and its straight leader crosses no other leader and no label
  // (it may pass over a dot of a crowded country). The marked station goes first, then the strongest. A label
  // with no free place is left out (its dot and hover stay).
  const OUT = [16, 28, 42, 58, 76, 96, 120, 150, 185, 225];
  const FAN = [0, 10, -10, 20, -20, 32, -32, 45, -45, 60, -60, 75, -75, 90, -90].map((d) => (d * Math.PI) / 180);
  function cross(p, q, r, t) {                     // segments pq and rt properly intersect
    const o = (a, b, c) => Math.sign((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
    return o(p, q, r) * o(p, q, t) < 0 && o(r, t, p) * o(r, t, q) < 0;
  }
  function segBox(p, q, b) {                       // segment pq touches box b
    const inside = (x) => x[0] >= b[0] && x[0] <= b[2] && x[1] >= b[1] && x[1] <= b[3];
    if (inside(p) || inside(q)) return true;
    const c = [[b[0], b[1]], [b[2], b[1]], [b[2], b[3]], [b[0], b[3]]];
    return c.some((x, i) => cross(p, q, x, c[(i + 1) % 4]));
  }
  function placeLabels(g, marks, boxes) {
    const leaders = [];
    const hit = (b, own) => boxes.some((o) => b[0] < o[2] && b[2] > o[0] && b[1] < o[3] && b[3] > o[1]) ||
      leaders.some(([p, q]) => segBox(p, q, b)) ||
      marks.some((m) => m !== own && b[0] < m.x + m.r && b[2] > m.x - m.r && b[1] < m.y + m.r && b[3] > m.y - m.r) ||
      [[b[0], b[1]], [b[2], b[1]], [b[0], b[3]], [b[2], b[3]]].some(([px, py]) => Math.hypot(px - view.cx, py - view.cy) > view.rr - 2);
    const leaderFree = (p, q) => !leaders.some(([r, t]) => cross(p, q, r, t)) && !boxes.some((o) => segBox(p, q, o));
    const todo = [...marks].filter((m) => (opt.calls || opt.db || m.s.hi) && (!m.crowd || m.s.hi)).sort((a, b) => (b.s.hi - a.s.hi) || (b.s.snr - a.s.snr));
    for (const m of todo) {
      const s = m.s, col = s.hi ? '#fff' : bandColor(s.band);
      const call = opt.calls || s.hi ? s.call : '';           // SNR alone: the number only
      g.font = '700 11px RC, sans-serif';
      const wc = call ? g.measureText(call).width : 0;
      const db = opt.db && s.snr != null ? `${call ? ' ' : ''}${Math.round(s.snr)}` : '';
      if (!call && !db) continue;
      g.font = '400 11px RC, sans-serif';
      const w = wc + (db ? g.measureText(db).width : 0) + 2, h = 13;
      const phi = Math.hypot(m.x - view.cx, m.y - view.cy) < 1 ? 0 : Math.atan2(m.y - view.cy, m.x - view.cx);
      let spot = null;
      // beside the dot: the outer side first
      for (const left of Math.cos(phi) < 0 ? [true, false] : [false, true]) {
        const bx = left ? m.x - m.r - 4 - w : m.x + m.r + 3;
        const b = [bx, m.y - h / 2 - 1, bx + w, m.y + h / 2 - 1];
        if (!hit(b, m)) { spot = { b, ay: m.y }; break; }
      }
      // out along the centre's ray through the dot, fanned a little
      for (const rad of OUT) {
        if (spot) break;
        for (const d of FAN) {
          const a = phi + d, ux = Math.cos(a), uy = Math.sin(a);
          const ax = m.x + ux * rad, ay = m.y + uy * rad, left = ux < -0.2;
          const bx = left ? ax - w : ux > 0.2 ? ax : ax - w / 2;
          const by = Math.abs(ux) > 0.2 ? ay - h / 2 - 1 : uy < 0 ? ay - h : ay;
          const b = [bx, by, bx + w, by + h];
          const p = [m.x + ux * (m.r + 1), m.y + uy * (m.r + 1)], q = [ax, ay];
          if (!hit(b, m) && leaderFree(p, q)) { spot = { b, ay: by + h / 2 + 1, leader: [p, q] }; break; }
        }
      }
      if (!spot && !s.hi) continue;
      if (!spot) spot = { b: [m.x + m.r + 3, m.y - 8, m.x + m.r + 3 + w, m.y + 5], ay: m.y };
      boxes.push(spot.b);
      g.globalAlpha = 1 - Math.min(1, s.age / 1800) * 0.55;
      if (spot.leader) {                                   // a straight leader, out from the centre
        const [p, q] = spot.leader;
        leaders.push(spot.leader);
        g.strokeStyle = col; g.lineWidth = 0.8;
        g.beginPath(); g.moveTo(p[0], p[1]); g.lineTo(q[0], q[1]); g.stroke();
      }
      g.fillStyle = col; g.textBaseline = 'middle';
      if (call) { g.font = '700 11px RC, sans-serif'; g.fillText(call, spot.b[0] + 1, spot.ay); }
      if (db) { g.font = '400 11px RC, sans-serif'; g.fillText(db, spot.b[0] + 1 + wc, spot.ay); }
      g.textBaseline = 'alphabetic';
      g.globalAlpha = 1;
    }
  }

  function draw() {
    if (!el) return;
    const box = el.querySelector('.map-canvas');
    const size = Math.max(260, Math.min(1600, box.clientWidth));
    const cv = box.querySelector('canvas'), dpr = devicePixelRatio || 1;
    if (cv.width !== size * dpr) { cv.width = cv.height = size * dpr; cv.style.width = cv.style.height = `${size}px`; }
    const zoom = opt.zoom || autoZoom();
    view = makeView(size, zoom);
    const g = cv.getContext('2d');
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, cv.width, cv.height);
    g.drawImage(drawBase(view), 0, 0);
    g.scale(dpr, dpr);
    if (opt.grayline) {
      g.save(); g.beginPath(); g.arc(view.cx, view.cy, view.rr, 0, Math.PI * 2); g.clip();
      g.drawImage(drawGray(view), 0, 0); g.restore();
    }
    g.save(); g.beginPath(); g.arc(view.cx, view.cy, view.rr, 0, Math.PI * 2); g.clip();
    const boxes = [];
    view.marks = [];
    view.visitor = placeVisitor(g, view, boxes);    // its label keeps the spots' labels off
    // the spots of one DXCC share its point: fan them out on a sunflower
    // spiral around it (the strongest in the middle), a dot each
    const at = new Map(), groups = new Map();
    for (const s of spots) {
      if (!s.where) continue;
      const k = `${s.where.lat},${s.where.lon}`;
      if (!groups.has(k)) groups.set(k, []);
      groups.get(k).push(s);
    }
    const dotR = (s) => 2.5 + Math.max(0, s.snr) / 12;
    const disc = [];                                       // each DXCC's spiral: [x, y, radius, spots]
    for (const g of groups.values()) {
      const [px, py] = view.P(g[0].where.lat, g[0].where.lon);
      g.sort((a, b) => b.snr - a.snr).forEach((s, k) => {
        const r = 8 * Math.sqrt(k), th = k * 2.39996;      // the golden angle
        at.set(s, [px + r * Math.cos(th), py + r * Math.sin(th)]);
      });
      disc.push([px, py, 8 * Math.sqrt(g.length - 1) + Math.max(...g.map(dotR)), g]);
    }
    // a crowd: DXCCs whose spirals run into each other (a contest: Europe
    // round the centre, W) — their dots would be one blot, so they are left
    // out and only the lines stay (each to its own place in the spiral);
    // the marked station keeps its dot. Lines off: the dots stay.
    const crowd = new Set();
    if (opt.lines) {
      for (let i = 0; i < disc.length; i++) {
        for (let j = i + 1; j < disc.length; j++) {
          const [x1, y1, r1, g1] = disc[i], [x2, y2, r2, g2] = disc[j];
          if (Math.hypot(x1 - x2, y1 - y2) < r1 + r2) { g1.forEach((s) => crowd.add(s)); g2.forEach((s) => crowd.add(s)); }
        }
      }
    }
    // lines and dots first (the strongest on top), the labels after them
    const order = [...spots].filter((s) => s.where).sort((a, b) => a.snr - b.snr);
    for (const s of order) {
      const [x, y] = at.get(s), col = bandColor(s.band);
      g.globalAlpha = 1 - Math.min(1, s.age / 1800) * 0.65;
      if (opt.lines) {
        g.strokeStyle = col; g.lineWidth = 0.6;
        g.beginPath(); g.moveTo(view.cx, view.cy); g.lineTo(x, y); g.stroke();
      }
      if (crowd.has(s) && !s.hi) {                        // a line only (its hover stays)
        view.marks.push({ x, y, r: 0, s, crowd: true });
        g.globalAlpha = 1;
        continue;
      }
      const r = dotR(s);
      g.fillStyle = col; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill();
      if (s.hi) { g.strokeStyle = '#fff'; g.lineWidth = 1.5; g.beginPath(); g.arc(x, y, r + 4, 0, 7); g.stroke(); }
      view.marks.push({ x, y, r, s });
      g.globalAlpha = 1;
    }
    if (opt.calls || opt.db || spots.some((s) => s.hi)) placeLabels(g, view.marks, boxes);
    if (view.visitor) paintVisitor(g, view.visitor);
    g.restore();
    g.fillStyle = '#39d98a'; g.beginPath(); g.arc(view.cx, view.cy, 4, 0, 7); g.fill();
    el.querySelector('[data-k=zoom]').textContent = opt.zoom ? `${fmtKm(zoom)}` : `auto · ${fmtKm(zoom)}`;
    el.querySelector('[data-o=auto]').classList.toggle('on', !opt.zoom);
    const un = spots.filter((s) => !s.where).length;
    const lines = view.marks.filter((m) => m.crowd).length;
    el.querySelector('.map-count').textContent = `${spots.length} spot${spots.length === 1 ? '' : 's'} · ` +
      `${lines ? `${lines} in crowds as lines only (zoom in for their dots) · ` : ''}${un ? `${un} unplaced · ` : ''}centre ${centerOf().what}`;
  }
  const fmtKm = (km) => `${Math.round(km).toLocaleString('en-US')} km`;

  // ---- the cursor ------------------------------------------------------------------------
  function hover(e) {
    if (!view) return;
    const r = e.currentTarget.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
    const q = view.inv(x, y), out = el.querySelector('.map-read');
    if (!q) { out.hidden = true; return; }
    const d = geo(view.c, q.lat, q.lon);
    const near = (view.marks || []).find((m) => Math.hypot(m.x - x, m.y - y) < 8);
    out.hidden = false;
    const vv = view.visitor && Math.hypot(view.visitor.x - x, view.visitor.y - y) < 12 ? view.visitor : null;
    out.innerHTML = `<span>AZ <b>${Math.round(d.az)}°</b></span><span>QRB <b>${fmtKm(d.km)}</b></span><span>LOC <b>${llToLoc(q.lat, q.lon)}</b></span>` +
      (vv ? `<span class="spot">you: <b>${esc(visitorCall)}</b> · ${esc(vv.w.name)} · ${fmtKm(vv.km)}</span>` : '') +
      (near ? `<span class="spot" style="color:${bandColor(near.s.band)}"><b>${esc(near.s.call)}</b> ${fmtKhz(near.s.khz)} ${esc(near.s.mode)} ${esc(near.s.snr)} dB${near.s.where ? ` · ${esc(near.s.where.name)}` : ''}</span>` : '');
  }

  // ---- mounting --------------------------------------------------------------------------
  function mount(container) {
    el = container;
    el.innerHTML = `
      <div class="map-bar">
        <span class="seg">
          <button type="button" data-o="auto" title="about 2,000 km, wider as far as the farthest spot shown">auto</button>
          <button type="button" data-o="grayline">grayline</button>
          <button type="button" data-o="borders">borders</button>
          <button type="button" data-o="prefixes">DXCC</button>
          <button type="button" data-o="lines">lines</button>
          <button type="button" data-o="calls">calls</button>
          <button type="button" data-o="db" title="the spot's SNR in dB — behind its call, or alone without calls">SNR</button>
        </span>
        <span class="mut" data-k="zoom"></span>
      </div>
      <div class="map-canvas"><canvas></canvas><div class="map-read" hidden></div></div>
      <div class="map-foot"><span class="map-count mut"></span><span class="mut">wheel: zoom · auto: 2,000 km or out to the farthest spot</span></div>`;
    for (const k of ['grayline', 'borders', 'prefixes', 'lines', 'calls', 'db']) {
      el.querySelector(`[data-o=${k}]`).classList.toggle('on', !!opt[k]);
    }
    el.querySelector('.map-bar').addEventListener('click', (e) => {
      const o = e.target.dataset && e.target.dataset.o;
      if (!o) return;
      if (o === 'auto') opt.zoom = 0;
      else { opt[o] = !opt[o]; e.target.classList.toggle('on', opt[o]); }
      save(); draw();
    });
    const cv = el.querySelector('canvas');
    cv.addEventListener('wheel', (e) => {
      e.preventDefault();
      const z = opt.zoom || autoZoom();
      opt.zoom = Math.min(20000, Math.max(800, z * (e.deltaY > 0 ? 1.2 : 1 / 1.2)));
      save(); draw();
    }, { passive: false });
    cv.addEventListener('mousemove', hover);
    cv.addEventListener('mouseleave', () => { el.querySelector('.map-read').hidden = true; });
    cv.addEventListener('click', (e) => {
      const r = cv.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      const near = (view && view.marks || []).find((m) => Math.hypot(m.x - x, m.y - y) < 8);
      if (near) window.dispatchEvent(new CustomEvent('skmap-spot', { detail: near.s.call }));
    });
    addEventListener('resize', () => draw());
  }

  // spots: [{ call, khz, mode, snr, band, age, hi }] — placed here by their DXCC;
  // station: { locator, call, visitor } — the centre, and the visitor signed in (or none)
  function update(list, station) {
    if (station) { home = station.locator || ''; homeCall = station.call || ''; visitorCall = station.visitor || ''; }
    spots = list.map((s) => ({ ...s, where: s.where !== undefined ? s.where : locate(s.call) }));
    draw();
  }

  // site: where the station is for a locator and a call, as the map's centre
  // (Statistics shades the night there); subsolar, sunAlt: the sun, for it
  return { mount, update, redraw: () => draw(), locate, geo, locToLL, llToLoc, bandColor, center: centerLL,
           site: (loc, call) => centerFor(loc, call).ll, subsolar, sunAlt };
})();
