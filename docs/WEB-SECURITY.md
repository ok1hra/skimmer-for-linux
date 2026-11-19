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

## What a viewer gets

| | viewer | manager |
|---|---|---|
| Status (map, DX cluster, bands), Statistics, Waterfall with audio | yes | yes |
| Settings and Log — in the menu and in the page's HTML | no (cut out on the server) | yes |
| `GET /api/config`, `/api/log`, `/api/skimmers` | 403 | yes |
| every `POST` (config, import, restart, rollback, DeepCW fetch) | 403 | yes |
| `status.json` | without addresses, file paths and the `SKIM_*` values | whole |
| `stats.json`, `/api/spots`, `/ws/waterfall` | yes | yes |

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
  `proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;` (and,
  for the waterfall, `proxy_http_version 1.1;` with the `Upgrade` and
  `Connection` headers). The skimmer then treats every request through it
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
