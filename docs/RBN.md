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

## Uploading without the Aggregator (not built — needs the RBN's consent)

Asked for 2026-11-19: the Aggregator built into the skimmer. The upload
protocol is not public. The one known replacement is SparkGap's
`rbn2_feeder.py` (github.com/hotairfred/SparkGap issues #49, cdub89/SparkGap
#42, Oct–Nov 2026):

- It posts to two RBN scripts, `id.php` (sign-in, at start and every
  300 s) and `s.php` (the spots, a batch per tick).
- Both carry fields computed by **`librbn2_auth.so`**, a small private
  library (Linux x86_64, `rbn2auth/1.2`): `rbn2_auth_short_hash` for the
  sign-in, `rbn2_auth_spot_hash` over the batch's calls. It is not
  published; the RBN team (Chris WX7V) gives it to approved testers on
  request, not for redistribution.
- The RBN authorises a feeder by name and version after it passes their
  test criteria. What SparkGap had to do in the Aggregator's place: spot
  only a station calling CQ or TEST; repeat counts by RBN's `patt3ch.lst`;
  upload CQ-tagged lines only; mode codes CW = 1, RTTY = 11; no beacons;
  the frequency right (ppm-corrected).

So the built-in upload waits for the RBN: ask them for the criteria and
the library. Once granted, it goes in as DeepCW's runtime did — the
library `dlopen`ed from outside git, never shipped, `[rbn] upload=direct`
off by default, the Aggregator port kept as it is.
