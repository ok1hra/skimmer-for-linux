// tour.js — the "?" in the menu: a guided tour of the page open (Status,
// Statistics, Waterfall). The page goes dim and blurred but for one part,
// ringed, with a card that says what it is and how it is used; "Got it"
// goes to the next part, "Back" to the one before, "End the tour" (or Esc)
// ends it at once. A part the page does not show now (a mode not decoded, a
// palette not offered) is left out. Nothing on the page is changed or
// clicked by the tour — while it runs the page takes no clicks.
'use strict';

const Tour = (() => {
  // the parts of each page: what to ring (one selector or several — the box
  // around all of them), a title, what to say
  const STEPS = {
    status: [
      { sel: 'nav', t: 'The pages',
        x: 'Status shows what the skimmer hears now, Statistics its history, Waterfall one band live. The <b>?</b> starts this guide on each of these pages.' },
      { sel: '.strip', t: 'The skimmer at a glance',
        x: 'The radio (green dot: it streams IQ), the CPU load and its temperature (green, amber from 70 °C, red from 85 °C), IQ lost on the wire, the telnet clients on the spot feed, the CW decoder and the version.' },
      { sel: '#feed-address', t: 'The spot feed',
        x: 'The address of this skimmer\'s telnet DX cluster feed — connect a logger or a cluster client there to get its spots.' },
      { sel: '#map-age', t: 'How far back',
        x: 'The age of the spots shown: 5, 10, 15 or 30 minutes. It sets the map, the bands table and the DX cluster together.' },
      { sel: '#skmap .map-bar', t: 'The map\'s layers',
        x: 'AUTO zooms to the spots; grayline, borders, DXCC, the lines from the station, the dots, the calls, the SNR and the heat map can each be switched on or off.' },
      { sel: '#skmap .map-canvas', t: 'The map',
        x: 'Every station spotted, at its true distance and bearing from this station (the centre). The mouse wheel zooms, a spot under the mouse tells its call, band and signal.' },
      { sel: '#ladder .legend-row', t: 'The modes',
        x: 'Each mode in its colour — CW, RTTY, FT8, FT4, JS8, WSPR. A click on the eye leaves the mode out of the bands, the heat map, the map and the DX cluster (and counts it in again).' },
      { sel: '#ladder', t: 'The bands',
        x: 'A row a band, the highest on top: the spots of the age chosen and the decoders active now, each mode its part of the bar; the continents the band is open to (brighter = more and louder stations); what a worker or the thermal guard does with the band. The eye on the left hides the band everywhere.' },
      { sel: '#readouts', t: 'The numbers',
        x: 'The decoders reading a signal now, the spots of the age chosen, and everything since the start.' },
      { sel: '#dxc .dxc-bar', t: 'The DX cluster',
        x: 'The newest spots of this skimmer, as a DX cluster shows them. <b>columns</b> picks the columns shown.' },
      { sel: '#dxc table', t: 'Filters and listening',
        x: 'The ▾ in a column head filters by it — band, mode, distance, bearing, signal, speed, country, the age. The speaker in a row tunes the waterfall there and plays it (an RTTY spot opens the RTTY palette, an FT8 or JS8 spot its palette).' },
    ],
    stats: [
      { sel: '#range', t: 'The range',
        x: 'How much history the charts show: 6 hours up to 2 years. <b>grayline</b> shades the night at this station.' },
      { sel: ['#stats-dec', '#stats-tiles'], t: 'The totals',
        x: 'The decoders active now, and over the range: the spots sent, the calls decoded, the telnet clients and how long the radio streamed — each mode on its own line.' },
      { sel: ['#spots-y', '#spots-smooth'], t: 'How the charts are drawn',
        x: '<b>linear</b> or <b>log</b>: a logarithmic scale keeps the quiet hours readable beside a busy one. <b>smooth</b> averages each bucket with its neighbours — the trend instead of every bucket.' },
      { sel: '#chart-spots', t: 'Spots per hour',
        x: 'CW up and RTTY down from the same zero, a colour a band (the band\'s own, never its rank). The mouse shows the values of a moment.' },
      { sel: '#ft8-spots', t: 'The digital modes',
        x: 'FT8/FT4 and JS8 send many more spots than CW: a chart of their own, FT8/FT4 up and JS8 down.' },
      { sel: '#timebar', t: 'Back in time',
        x: 'The whole history kept, the green frame is the window the charts show: drag it, or click where it should be.' },
      { sel: '#chart-active', t: 'Active decoders',
        x: 'How busy each band was: a strip a mode (CW, then RTTY, FT8, JS8), darker = busier. Red: the band was paused by the thermal guard.' },
      { sel: '#card-cpu', t: 'CPU and temperature',
        x: 'The CPU the skimmer takes (and what its decoders take of it), IQ lost on the wire, and the hottest CPU sensor — with the thermal guard\'s limit when it is on.' },
      { sel: '#card-phase', t: 'Phasing',
        x: 'A band received from both antennas (IN1 + IN2): what the combination gave, a pixel a channel and a row a minute — the interference it took off the floor, or a station\'s gain over the better antenna alone. Greyed rows: phasing was off (what it would have given). On the right the band\'s medians.' },
      { sel: '#stats-tbl', t: 'The table',
        x: 'The range band by band: spots, spots per hour, calls decoded, busy decoders, the typical SNR — each mode on its own line, the bars beside the numbers comparing the bands.' },
      { sel: '#stats-wspr', t: 'WSPR stations',
        x: 'The stations WSPR heard in the last 24 h. A click on a column head sorts by it.' },
    ],
    waterfall: [
      { sel: ['#wf-band', '#wf-span'], t: 'Which band, how wide',
        x: 'The band shown and the span: the whole band, or zoomed in on a part of it.' },
      { sel: '#wf-rate', t: 'The speed',
        x: 'Rows a second the skimmer sends. A slow line gets fewer rows by itself.' },
      { sel: ['#wf-range', '#wf-floor', '#wf-auto'], t: 'Contrast',
        x: 'RANGE is how many dB the colours span, FLOOR where black starts. <b>auto</b> puts both back on the noise.' },
      { sel: ['#wf-spec', '#wf-calls'], t: 'Spectrum and calls',
        x: '<b>spectrum</b> adds the signal level above the picture; <b>calls</b> writes the calls spotted on the band where they were heard.' },
      { sel: ['#wf-audio', '#wf-freq', '#wf-bw', '#wf-smeter'], t: 'Listening',
        x: 'The speaker plays the frequency heard; type a frequency in kHz and press Enter to tune there; the passband beside it; the S-meter shows the signal.' },
      { sel: ['#wf-rtty', '#wf-ft8', '#wf-js8'], t: 'The palettes',
        x: '<b>RTTY</b> decodes the frequency heard in the browser. <b>FT8</b> and <b>JS8</b> show what the skimmer decodes on the band\'s FT8 or JS8 frequency — JS8\'s messages put back together — and narrow the picture to it.' },
      { sel: '#wf-phase-ctl', t: 'Phasing',
        x: 'This band comes from both antennas. <b>Φ</b> switches the combination on and off — for decoding too — and the picture shows the difference within a second or two. The list beside it puts a line over the spectrum: per channel the interference taken off the floor, a station\'s gain over the better antenna, or the deepest null the coherence allows. <b>⚙</b> opens the panel: the mode, a fixed weight set by hand (or taken from MVDR), three memories and Save.' },
      { sel: '#wf-scale', t: 'The scale',
        x: 'The frequency in kHz. Coloured feet mark the windows the digital decoders listen to (FT8 violet, JS8 gold, WSPR red).' },
      { sel: '#wf-box', t: 'The waterfall',
        x: 'Time runs down. A click listens there; the mouse wheel zooms around the cursor (over the spectrum it tunes, Shift: 10 Hz steps); drag moves the view; a double click centres it. The calls near the mouse show in a tip.' },
    ],
  };

  let el = null, steps = [], i = 0, prevFocus = null;
  const visible = (n) => n && !n.hidden && n.offsetParent !== null && n.getBoundingClientRect().width > 0;
  const targets = (s) => (Array.isArray(s.sel) ? s.sel : [s.sel]).map((q) => document.querySelector(q)).filter(visible);

  function build() {
    el = document.createElement('div');
    el.className = 'tour';
    el.hidden = true;
    el.innerHTML = '<div class="tour-shade"></div><div class="tour-shade"></div><div class="tour-shade"></div>' +
      '<div class="tour-shade"></div><div class="tour-ring"></div>' +
      '<div class="tour-card" role="dialog" aria-modal="true" aria-labelledby="tour-t">' +
      '<div class="tour-n"></div><h3 id="tour-t"></h3><p class="tour-x"></p>' +
      '<div class="tour-btns"><button type="button" class="tour-end">End the tour</button>' +
      '<button type="button" class="tour-back">Back</button>' +
      '<button type="button" class="tour-next primary">Got it</button></div></div>';
    document.body.appendChild(el);
    el.querySelector('.tour-next').addEventListener('click', next);
    el.querySelector('.tour-back').addEventListener('click', () => go(i - 1));
    el.querySelector('.tour-end').addEventListener('click', end);
    addEventListener('resize', () => { if (!el.hidden) place(); });
    addEventListener('scroll', () => { if (!el.hidden) place(); }, true);
    document.addEventListener('keydown', (e) => {
      if (!el || el.hidden) return;
      if (e.key === 'Escape') { e.preventDefault(); end(); }
      else if (e.key === 'ArrowRight' || e.key === 'Enter') { e.preventDefault(); next(); }
      else if (e.key === 'ArrowLeft') { e.preventDefault(); go(i - 1); }
    });
  }

  // the box around the step's parts, a little apart from them
  function box() {
    const ts = targets(steps[i]);
    if (!ts.length) return null;
    const rs = ts.map((t) => t.getBoundingClientRect()), pad = 6;
    return {
      l: Math.max(0, Math.min(...rs.map((r) => r.left)) - pad), t: Math.max(0, Math.min(...rs.map((r) => r.top)) - pad),
      r: Math.min(innerWidth, Math.max(...rs.map((r) => r.right)) + pad),
      b: Math.min(innerHeight, Math.max(...rs.map((r) => r.bottom)) + pad),
    };
  }
  function place() {
    const b = box();
    if (!b) return;
    const [top, bottom, left, right] = el.querySelectorAll('.tour-shade');
    const set = (n, l, t, w, h) => Object.assign(n.style, { left: `${l}px`, top: `${t}px`, width: `${Math.max(0, w)}px`, height: `${Math.max(0, h)}px` });
    set(top, 0, 0, innerWidth, b.t);
    set(bottom, 0, b.b, innerWidth, innerHeight - b.b);
    set(left, 0, b.t, b.l, b.b - b.t);
    set(right, b.r, b.t, innerWidth - b.r, b.b - b.t);
    set(el.querySelector('.tour-ring'), b.l, b.t, b.r - b.l, b.b - b.t);
    // the card: below the part, else above it, else over its bottom
    const c = el.querySelector('.tour-card'), cw = c.offsetWidth, ch = c.offsetHeight, gap = 12;
    let y = b.b + gap;
    if (y + ch > innerHeight - 8) y = b.t - gap - ch >= 8 ? b.t - gap - ch : Math.max(8, innerHeight - ch - 8);
    const x = Math.max(8, Math.min(innerWidth - cw - 8, (b.l + b.r) / 2 - cw / 2));
    c.style.left = `${x}px`; c.style.top = `${y}px`;
  }
  function go(k) {
    if (k < 0 || k >= steps.length) return;
    i = k;
    const s = steps[i], ts = targets(s);
    // the part into view (the tallest one in the middle), then the ring around it
    const tall = ts.reduce((a, t) => (t.getBoundingClientRect().height > a.getBoundingClientRect().height ? t : a), ts[0]);
    const r = tall.getBoundingClientRect();
    if (r.top < 60 || r.bottom > innerHeight - 8) tall.scrollIntoView({ block: r.height > innerHeight * 0.7 ? 'start' : 'center' });
    el.querySelector('.tour-n').textContent = `${i + 1} / ${steps.length}`;
    el.querySelector('#tour-t').textContent = s.t;
    el.querySelector('.tour-x').innerHTML = s.x;
    el.querySelector('.tour-back').hidden = i === 0;
    el.querySelector('.tour-next').textContent = i === steps.length - 1 ? 'Got it — done' : 'Got it — next';
    requestAnimationFrame(place);
    el.querySelector('.tour-next').focus();
  }
  function next() { if (i >= steps.length - 1) end(); else go(i + 1); }
  function start(page) {
    // the parts this page shows now
    steps = (STEPS[page] || []).filter((s) => targets(s).length);
    if (!steps.length) return;
    if (!el) build();
    prevFocus = document.activeElement;
    el.hidden = false;
    document.body.classList.add('touring');
    go(0);
  }
  function end() {
    if (!el) return;
    el.hidden = true;
    document.body.classList.remove('touring');
    if (prevFocus && prevFocus.focus) prevFocus.focus();
  }

  // the "?" in the menu: there on the pages that have a tour
  const btn = document.querySelector('#tour-open');
  const page = () => { const b = document.querySelector('nav button[data-tab].on'); return b ? b.dataset.tab : ''; };
  const sync = () => { if (btn) btn.hidden = !STEPS[page()]; };
  if (btn) btn.addEventListener('click', () => start(page()));
  for (const b of document.querySelectorAll('nav button[data-tab]')) b.addEventListener('click', () => { end(); sync(); });
  sync();
  return { start, end, sync };
})();
