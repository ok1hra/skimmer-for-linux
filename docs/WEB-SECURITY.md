# The web page on a public address

Can the skimmer's page (`[status] http_port`, default 8073) be reachable
from the internet? Yes — for watching. Managing it stays on the machine.
This file is the rule set and why; it comes from an audit of 2026-11-19.

## Who is a manager

A request may manage the skimmer only when all three hold:

1. its address is in `[web] settings_from` (default `127.0.0.1,::1`);
2. no proxy forwarded it — it carries none of `X-Forwarded-For`,
   `Forwarded`, `X-Real-IP`, `Via`;
3. its `Host` is this machine: an IP address, `localhost`, the host name,
   or the host name with `.local`, `.lan`, `.home`, `.localdomain`,
   `.home.arpa` (`src/headless/web.c`, `host_ok`).

Everyone else is a viewer.

## Who must sign in

With `[visitors] login=true` (the default), a viewer that is not **free**
signs in with a callsign before it gets any data. Free is a manager, or a
client whose address is in `[visitors] free_from` (default: the private
networks, loopback, link-local) and that no proxy forwarded — through a
proxy every visitor signs in, the same reason a proxy never makes a
manager. A visitor that has not signed in gets the page itself (HTML, JS,
CSS, fonts, the map's data, so the sign-in form can show) and nothing else:
`status.json`, `stats.json`, `/api/spots`, `/api/wspr` and `/api/ft8` answer 401 `{"error":"login"}`,
the waterfall's socket is closed with "login".

A client from outside — not free in the sense above, signed in or not, and
whether sign-in is on or off — gets no machine readings either: in
`status.json` the CPU (total and per mode), IQ lost and packets/s are
`null` (the workers' status the hub passes on too), and in `stats.json` the
`cpu`, `cpu_cw`, `cpu_rtty` and `lost` series are `null`; the page leaves
those out (Dan, 2026-11-20: how hard the box runs is nobody else's
business). The CPU temperature and the thermal guard's temperatures go to
everyone (Dan, 2026-11-21): the visitor sees why the guard took bands OFF. `[feed] address` — the telnet address the
header shows — goes to everyone on purpose, added after the address
rule, so an IP:port written there reaches the visitor as written. `rtty_palette` — the
RTTY palette's settings (`[rtty_palette]`: numbers and switches, nothing
about the machine) — goes to everyone the same way.

The callsign is checked on the server (`src/headless/visitors.c`): 3 to 15
characters of A–Z, 0–9 and `/`, at least one digit and one letter. It comes
back as an HttpOnly, SameSite=Strict cookie `skv=CALL.ISSUED.HMAC` valid 30
days, signed (HMAC-SHA256) with a key made once in the state directory
(`visitor.key`, 0600). A forged, altered or expired cookie is refused; the
skimmer keeps no session table, so a restart signs no one out.

This is an introduction, not a password: anyone may type any callsign. It
keeps the page from being scraped anonymously and gives the station a log
of who watched.

`POST /api/visitor` is the one POST open to viewers — it must be, a visitor
is not a manager. It still refuses a page from another origin (the Origin
check every POST has), reads at most 256 bytes, and changes nothing but the
caller's own cookie. A refused sign-in is logged at most 30 times a minute,
the rest only counted, so a script cannot fill the disk.

The logs (state directory, rolled to `.1` at 5 MB each): `visitors.tsv` — a
line a visit: start, end, seconds, call, the connection's address, what a
proxy said it forwarded for (its word, never used for a decision), the
waterfall windows and audio frequencies watched for 10 s or more;
`visitors-failed.tsv` — time, address, forwarded-for, what was typed
(control characters escaped, 32 characters at most), why. The sign-in form
tells the visitor what is logged. Managers read both in the Log tab
(`/api/visitors`). Since 2026-11-21 (Dan) Statistics lists the visitors
under its table from the same endpoint, by who asks: at home (a manager, an
address in free_from without a proxy) the whole log — addresses, what was
watched, the refusals; from outside, signed in, only the calls of the last
24 h with their start and how long they stayed (no address, nothing
watched); not signed in, 401.

## What a viewer gets

| | viewer | manager |
|---|---|---|
| Status (map, DX cluster, bands), Statistics, Waterfall with audio | yes | yes |
| Settings and Log — in the menu and in the page's HTML | no (cut out on the server) | yes |
| `GET /api/config`, `/api/log`, `/api/skimmers`, `/api/skimsrv` (CW Skimmer's band keys from its SkimSrv.ini on this PC — never its [User]), `/api/update` (the running AppImage's path, the update state) | 403 | yes |
| every `POST` (config, import, restart, rollback, DeepCW fetch, `/api/update`: check, install, back to the kept version) | 403 | yes |
| `POST /api/visitor` (sign in with a call, sign out) | yes (same origin) | yes |
| `/api/visitors` (the visitors' log) | from outside: the last 24 h's calls, start and length only (signed in); from the LAN (free_from): whole | yes |
| `status.json` | without addresses, file paths and the `SKIM_*` values; from outside also without the CPU load and lost | whole |
| `stats.json` | yes (from outside: once signed in, without the CPU load and lost series) | yes |
| `/api/spots`, `/api/wspr` (the WSPR stations of 24 h: calls and locators heard on the air), `/api/ft8` (the FT8/FT4 messages of 15 min as heard on the air — calls, locators, reports; the page escapes them), `/ws/waterfall` | yes (from outside free_from: once signed in) | yes |

The station's locator goes to a viewer as its square only — the first
four characters (JN18), in `status.json` and `/api/spots` (and
`status.json`'s `wspr` block, for a manager too: the Settings form has the
whole one): a six-character locator is a few kilometres, nearly an address. A manager
gets it whole there too, so the map sits on the station itself.

Each answer carries a Content-Security-Policy (the page's own scripts,
styles, fonts and sockets only; no framing), `X-Frame-Options: DENY`,
`X-Content-Type-Options: nosniff` and `Referrer-Policy: no-referrer`.
The waterfall's WebSocket refuses a page from another origin. At most
8 waterfall viewers and 4 listeners at once.

## Publishing it

- **Forward the port directly** (router → this machine's 8073). The
  skimmer then sees each visitor's own address, and they are viewers.
- **A reverse proxy is fine as long as it sends `X-Forwarded-For`** —
  Caddy and Apache's `mod_proxy` do by default; **nginx does not**: add
  `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;` and
  `proxy_set_header Host $host;` (the waterfall's socket compares the
  page's origin with the Host), and for the socket `proxy_http_version
  1.1;` with the `Upgrade` and `Connection` headers. The skimmer then treats every request through it
  as a viewer, the manager's own included. Manage the skimmer
  on the machine itself, or through an SSH tunnel:
  `ssh -L 8073:127.0.0.1:8073 <host>`, then `http://localhost:8073/`.
- **A proxy that strips those headers** would make every visitor
  127.0.0.1 — a manager. Do not run one.
- Never put a public network in `settings_from`.
- There is no TLS in the skimmer: for `https://`, a proxy does it (with the
  headers above), and the page's socket follows (`wss:`).

## What the audit found (all fixed)

- **DNS rebinding.** A site whose name resolves to 127.0.0.1, open in a
  browser on the skimmer's machine, was "this page" to the Origin check
  (its Origin matched its own Host) from a local address — it could change
  and restart the config. Fixed by the Host rule (3).
- **A reverse proxy on the same machine** made every visitor 127.0.0.1, a
  manager. Fixed by rule (2).
- **The config, the log and the LAN search were open to all.** The config
  shows the network's layout, the log the machine's paths; the LAN search
  holds the main loop 0.8 s per call — repeated from outside, it stalls the
  skimmer. Now 403 for viewers.
- **`status.json` told about the LAN and the machine**: the radio's
  address (also inside the status text), the peers' and workers'
  addresses, file paths with the user's name, the environment switches.
  Viewers get it without them (`status_public` in `headless_main.c`).
- **A peer's feed could put markup on the page**: a spot's mode was taken
  as written and shown as HTML. The skimmer keeps letters and digits only;
  the page escapes calls, modes and countries everywhere they are shown.
- **`stats.json` was computed afresh for every request** — up to two years
  of rings each time. The same question within 20 s now gets the same
  answer.

Not changed, on purpose: the spots, the statistics and the waterfall are
public by nature; the waterfall and its audio cost CPU (about 0.3 % of a
core a band, ~1–2 % a listener) and are capped by the viewer and listener
limits above.
