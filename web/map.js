// map.js — the status page's azimuthal map: the spots of the last half hour
// around the station, the way IP-rotator draws its map (web/map/ has the
// data): an azimuthal equidistant projection — distance and bearing true from
// the centre — land, borders, DXCC prefixes, the grey line, range rings.
//
// The zoom fits the spots shown (the farthest but the last tenth); the wheel
// takes over, AUTO gives it back. The centre is the station's locator unless
// another is typed in. The cursor reads azimuth, distance and locator.
// Per-viewer choices (layers, centre, zoom) are kept in localStorage.
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
  const opt = { grayline: true, borders: true, prefixes: true, lines: true, calls: true, center: '', zoom: 0, ...store };
  const save = () => { try { localStorage.setItem('skmap', JSON.stringify(opt)); } catch (e) { /* private window */ } };
  let el = null, home = '', spots = [], view = null, baseKey = '', base = null, grayKey = '', gray = null;

  const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  function centerLL() { return locToLL(opt.center) || locToLL(home) || { lat: 50, lon: 15 }; }

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

  // ---- drawing ---------------------------------------------------------------------------
  function autoZoom() {
    const c = centerLL();
    const ds = spots.filter((s) => s.where).map((s) => geo(c, s.where.lat, s.where.lon).km).sort((a, b) => a - b);
    return ds.length ? Math.min(20000, Math.max(2000, ds[Math.floor(ds.length * 0.9)] * 1.25)) : 12000;
  }
  function draw() {
    if (!el) return;
    const box = el.querySelector('.map-canvas');
    const size = Math.max(260, Math.min(680, box.clientWidth));
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
    for (const s of [...spots].sort((a, b) => b.snr - a.snr)) {
      if (!s.where) continue;
      const [x, y] = view.P(s.where.lat, s.where.lon), col = bandColor(s.band);
      g.globalAlpha = 1 - Math.min(1, s.age / 1800) * 0.65;
      if (opt.lines) {
        g.strokeStyle = col; g.lineWidth = 0.6;
        g.beginPath(); g.moveTo(view.cx, view.cy); g.lineTo(x, y); g.stroke();
      }
      const r = 2.5 + Math.max(0, s.snr) / 12;
      g.fillStyle = col; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill();
      if (s.hi) { g.strokeStyle = '#fff'; g.lineWidth = 1.5; g.beginPath(); g.arc(x, y, r + 4, 0, 7); g.stroke(); }
      view.marks.push({ x, y, s });
      if (opt.calls || s.hi) {
        g.font = '700 11px RC, sans-serif';
        const w = g.measureText(s.call).width, b = [x + 5, y - 15, x + 7 + w, y - 2];
        if (s.hi || !boxes.some((o) => b[0] < o[2] && b[2] > o[0] && b[1] < o[3] && b[3] > o[1])) {
          boxes.push(b);
          g.fillStyle = s.hi ? '#fff' : col; g.fillText(s.call, x + 6, y - 5);
        }
      }
      g.globalAlpha = 1;
    }
    g.restore();
    g.fillStyle = '#39d98a'; g.beginPath(); g.arc(view.cx, view.cy, 4, 0, 7); g.fill();
    el.querySelector('[data-k=zoom]').textContent = opt.zoom ? `${fmtKm(zoom)}` : `auto · ${fmtKm(zoom)}`;
    el.querySelector('[data-o=auto]').classList.toggle('on', !opt.zoom);
    el.querySelector('.map-count').textContent = `${spots.length} spot${spots.length === 1 ? '' : 's'} · ${spots.filter((s) => !s.where).length ? `${spots.filter((s) => !s.where).length} unplaced · ` : ''}centre ${opt.center || home || '—'}`;
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
    out.innerHTML = `<span>AZ <b>${Math.round(d.az)}°</b></span><span>QRB <b>${fmtKm(d.km)}</b></span><span>LOC <b>${llToLoc(q.lat, q.lon)}</b></span>` +
      (near ? `<span class="spot" style="color:${bandColor(near.s.band)}"><b>${near.s.call}</b> ${near.s.khz.toFixed(1)} ${near.s.mode} ${near.s.snr} dB${near.s.where ? ` · ${near.s.where.name}` : ''}</span>` : '');
  }

  // ---- mounting --------------------------------------------------------------------------
  function mount(container) {
    el = container;
    el.innerHTML = `
      <div class="map-bar">
        <span class="seg">
          <button type="button" data-o="auto" title="fit the spots shown">auto</button>
          <button type="button" data-o="grayline">grayline</button>
          <button type="button" data-o="borders">borders</button>
          <button type="button" data-o="prefixes">DXCC</button>
          <button type="button" data-o="lines">lines</button>
          <button type="button" data-o="calls">calls</button>
        </span>
        <label class="map-centre">centre <input data-k="centre" maxlength="6" size="7" placeholder="locator"></label>
        <button type="button" class="small" data-o="home" title="back to the station's locator">home</button>
        <span class="mut" data-k="zoom"></span>
      </div>
      <div class="map-canvas"><canvas></canvas><div class="map-read" hidden></div></div>
      <div class="map-foot"><span class="map-count mut"></span><span class="mut">wheel: zoom · auto: fit the spots</span></div>`;
    for (const k of ['grayline', 'borders', 'prefixes', 'lines', 'calls']) {
      el.querySelector(`[data-o=${k}]`).classList.toggle('on', !!opt[k]);
    }
    el.querySelector('[data-k=centre]').value = opt.center;
    el.querySelector('.map-bar').addEventListener('click', (e) => {
      const o = e.target.dataset && e.target.dataset.o;
      if (!o) return;
      if (o === 'auto') opt.zoom = 0;
      else if (o === 'home') { opt.center = ''; el.querySelector('[data-k=centre]').value = ''; }
      else { opt[o] = !opt[o]; e.target.classList.toggle('on', opt[o]); }
      save(); draw();
    });
    el.querySelector('[data-k=centre]').addEventListener('change', (e) => {
      const v = e.target.value.trim().toUpperCase();
      e.target.classList.toggle('bad', !!v && !locToLL(v));
      if (v && !locToLL(v)) return;
      opt.center = v; save(); draw();
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

  // spots: [{ call, khz, mode, snr, band, age, hi }] — placed here by their DXCC
  function update(list, station) {
    home = station || home;
    spots = list.map((s) => ({ ...s, where: s.where !== undefined ? s.where : locate(s.call) }));
    draw();
  }

  return { mount, update, locate, geo, locToLL, llToLoc, bandColor, center: centerLL };
})();
