# Spots to the Reverse Beacon Network

## How it works today

The RBN takes spots only from its own program, the **RBN Aggregator**
(Windows, .NET; it runs under wine). The Aggregator is a telnet *client*:
it connects to a skimmer's telnet port, logs in with the skimmer's call,
reads the `DX de CALL-#:` lines and uploads them to reversebeacon.net.
`[rbn] enabled=true` makes skimmer-headless offer its feed's spots on a
second telnet port for it (`src/engine/rbn_feed.c`, `[rbn] port`,
optionally held `delay_s` seconds). Nothing else leaves for the RBN — with
no Aggregator connected (`status.json` `rbn.clients` = 0, "RBN now" on the
status page) no spot reaches it.

Facts from hra2 (2026-11-19, Aggregator v6.4):

- One Aggregator has **one** skimmer connection (`Aggregator.ini`,
  `[Connections - Skimmer]`: `Skimmer IP`, `Skimmer Port`, `Skimmer Call`).
  CW Skimmer Server and skimmer-headless side by side need two Aggregators,
  each with its own working directory (it keeps `Aggregator.ini` there).
- The Aggregator listens itself on `[Connections - Local User] Port
  Number` (default **7550**) for loggers. On the same machine the
  skimmer's `[rbn] port` must be another one — hra2 runs 7551.
- After its login the Aggregator sends `SKIMMER/SETT`; CW Skimmer Server
  answers `SETT: vlNormal 1800.0-1870.0,3500.0-3570.0,…` (the validation
  level, then the segments it skims in kHz) and the Aggregator hands the
  ranges to the RBN as the node's bands. The feed answers it the same way
  (`skim_rbn_feed_set_bands`): every received CW/RTTY band as CW Skimmer
  reports a segment, 91/96 of its rate around its centre, ascending,
  overlaps merged, level `Normal`. Before 2026-11-27 it ignored the
  command, so a node behind our port showed the RBN no bands.

## Uploading without the Aggregator — rejected (2026-11-27)

Seen through a logging proxy on hra2 (Aggregator v6.4, 2026-11-27): the
Aggregator posts JSON over plain HTTP to
`http://x.reversebeacon.net:88/rx/6/` — `id.php` at start and about every
5 min (the node's call, grid, name, QTH, bands, versions; the reply carries
the server's rules: upload interval 10 s, 1500 spots in 3 min, which
windows take CQ/DE/beacon spots, excluded frequencies, the URLs and hashes
of master.scp, patt3ch.lst and bcn.csv), and `s.php` every 10 s when it
has spots (CQ spots only, as read from the telnet line, held only until
the next tick). Three fields of those posts (`fingerPrint`/`fp`,
`shortHash`, `h`) come from a private algorithm. The RBN gives approved
feeders a closed library for them (`librbn2_auth.so`, `rbn2auth/1.2`,
from Chris WX7V; SparkGap's `rbn2_feeder.py`, cdub89/SparkGap#42,
hotairfred/SparkGap#49) after its own test criteria.

Dan, 2026-11-27: we do not ask for a binary blob for authentication — the
upload stays with the RBN Aggregator on the `[rbn]` port. Extracting or
imitating the Aggregator's hash is out as well: the RBN authorises a
feeder by name and version. Revisit only if the RBN publishes an open
upload protocol.
