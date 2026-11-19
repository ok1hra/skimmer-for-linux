# Live air against CW Skimmer Server — the v0.6.0 defaults

From 2026-10-15 to 10-18 the decoder and the RBN feed were tuned on
recordings of live air with CW Skimmer Server (R, VE3NEA) spotting the
same antenna, then checked live in skimmer-compare. This file keeps the
method, the numbers that decided the v0.6.0 defaults, and what was tried
and rejected. The CQ WW 2012 work (docs/CQWW2012.md) came first; its
edge model, Rayleigh fade and 6-dit dah window lost ~10 points on strong
live signals and are off since 2026-10-13.

## The bench

- `skimmer-headless [record]` writes each band's IQ beside the decoding
  (cf32 + .meta). The directory must be under `$HOME`: `/var/tmp` inside
  the flatpak sandbox is RAM.
- `skimmer-replay` (`SKIM_REPLAY_TAP`) → `skimmer-tap-replay` (live
  policy) → `skimmer-compare/tap-score.py`: the same analysis compare-web
  runs, on the replay's spot rows. A replay lands on the live numbers —
  session 2026-10-13, 50 h: recall 69.9 / bust 7.2 % against live 69.7 /
  7.5 %; recording 2: 68.5 against live 68.6 — so the recordings decide,
  live only confirms.
- Changes after the decoder (extractor, station table, feed) replay from
  the decode taps alone (`skimmer-tap-replay`), minutes for days of air.
- `skimmer-compare/live-breakdown.py` and `reads.py`-style sorting: for
  each station R spotted and L did not, what L's decode text held there.
- Recall in compare is over the union of real stations; "L of R" (the
  share of R's stations L caught, per bin of R's SNR) is the fair figure
  when L stops spotting stations R never does.

## Why L missed R's stations (recordings, ~270 misses each)

| L's text around the station | share |
|---|---|
| the call read exactly, not spotted | ~30 % |
| torn — spaces inside the call ("G 0WD T", a stretched fist) | ~15 % |
| fused into a longer token (no word gaps at all: "CQCQCQDEIT9VJOIT9VJO") | ~10 % |
| a misread 1–2 edits away | ~14 % |
| nothing like the call (mostly under 10 dB) | ~25 % |
| no text | ~6 % |

Busts (25 h live, 173): half were real stations spotted as calling while
they were the other side of a QSO (`TU 73 UA3WBC DE DK2DR`), most of the
rest calls cut short or with a QSO word glued on, and singletons.

## What the defaults are, and what each bought

Recall / bust %, recordings 1 and 2 (30/20/40 m, 14 h each) and 50 h of
live taps (7 bands). Each row on top of the ones above it.

| change | switch | effect |
|---|---|---|
| carrier-centred 45 Hz envelope | `SKIM_V2_NARROW=45` | rec 1 +1.3, rec 2 +2.1 recall, busts never up; under 10 dB +5 points |
| word re-read against MASTER.SCP | `SKIM_V2_RESCORE=5,3,2.0` | CQ WW 2012: recall up on every band-hour, busts never up; everyday air ~neutral |
| fused run split where a call repeats | `SKIM_CALL_FUSED` | +0.2 … +0.4, busts unchanged |
| min_score 0.85 → 0.90 | `[feed] min_score` | taps 7.2 → 5.5 % busts for −0.5 recall |
| TU blesses the next call only, DE after it takes it back | `SKIM_CALL_TU=2` | taps 5.5 → 4.9 % busts, L of R unchanged |
| unknown call held 30 → 60 s | `[feed] settle_nodict_s` | taps 4.9 → 4.3 % busts for −0.4 recall |
| torn over re-read with spaces dropped | `SKIM_CALL_DESPACE` | recall +0.9 … +1.5 and busts −0.3 on all three sets |

The hold is a straight trade (taps, recall / bust): 30 s 67.7 / 4.9,
60 s 67.3 / 4.3, 90 s 67.0 / 3.8, 120 s 63.4 / 2.3, 240 s 55.3 / 1.0 —
past ~90 s the release finds the station no longer fresh.

Live, 24 h after the last change (a contest weekend, so not comparable
to the weekdays before it): recall L 82.0 / R 71.7, bust L 1.3 / R 1.5 %;
on the WARC bands, where nobody contests, 30 m 60 / 88 and 17 m 61 / 93
— the gap that is left.

## Spot frequency: snap, not crawl (2026-11-18)

Against RBN's most frequent frequency for the call within ±5 min (same
band, ≥ 3 RBN reports, OK1HRA's own spots left out — R feeds RBN under
that call), 12.7–23.7 % of L's spots sat over 150 Hz off it, R's
4.3–4.4 % — on v2 and v3 alike. The station table pulled its position
30 % of the way to every stronger report within SAMECALL_HZ (2.5 kHz), an
EMA meant for ±10-20 Hz of scatter: a record that started on a far ghost,
or a station that moved 2 kHz, crawled across in 30 % steps and the feed
re-spotted every step of 100 Hz or more (LZ1ZF 7016.7, 7017.1, 7017.4,
7017.6, 7017.7, 7017.9 within a second for a carrier on 7018.0). Reports
of the call in a runner's exchange (`RV6HV 5NN TT3` on the runner's
frequency) dragged it the same way.

`SKIM_ST_SNAP` (Hz, station.h): beyond it a stronger report of the
station calling (cq, not addressed) moves the record at once; any other
report changes neither position nor SNR. Live v3.1 taps through
skimmer-tap-replay, off-frequency share / spots / calls spotted on RBN's
frequency at least once:

| tap | off | 100 Hz |
|---|---|---|
| 11-17 22:39 → 11-18 16:05 (17.5 h, 7 bands) | 12.7 % / 3928 / 626 | 5.6 % / 3631 / 626 |
| 11-18 16:42 → 22:39 (6 h, 40 m contest evening) | 23.7 % / 4195 / 454 | 11.0 % / 3597 / 458 |

Not a single call lost; 200 Hz gives the same. Variants measured on the
6 h tap: snap on any stronger report 12.6 % (but it puts RV6HV on the
runner's frequency); snap only the calling station, others still pulling
the EMA 17.3 %; the far-QSY branch (> 2.5 kHz) also only for the calling
station 16.9 % — a far non-CQ report is usually a real move, so that
branch stays as it was.

## One place at a time, one spot per run (2026-11-18)

With the snap L still spotted unlike R in two ways:

- **Ping-pong between two places.** One record per call per band follows
  the last confident report. In a contest a station runs on one frequency
  and answers others elsewhere; its call on a runner's frequency can even
  read as a CQ (CWT: "CQ CWT SP3FSM … PA3AAV GE GERT", no 5NN to mark it
  addressed). PA3AAV flipped 3540.9 ↔ 3536.0 every second, each flip a
  spot. `SKIM_ST_HOLD` (s): beyond SAMECALL_HZ the record moves only once
  its place has gone that long without a fresh read.
- **A pause re-spots.** The station table's 120 s TTL (panadapter labels)
  took the RBN dedup memo with it: a station silent for two minutes was
  spotted again on the same frequency — median gap between spots of a call
  5 min, R's 10. `SKIM_RBN_MEMO`: the memo outlives the table (pruned past
  RBN_RESPOT_S).

Live taps, snap on; spots per call / over 150 Hz off / re-spots within 60 s
/ median gap between spots of a call:

| window | R | L snap | L snap + memo + hold 30 s |
|---|---|---|---|
| 11-17 14:20–17:20, weekday afternoon | 1.77 / 4.4 % / 3.8 % / 9.7 min | 2.27 / 2.2 % / 4.0 % / 5.4 min | 1.67 / 2.8 % / 0.5 % / 10.8 min |
| 11-17 22:40 → 11-18 16:05 (R down) | — | 3.24 / 5.7 % / 15.0 % / 4.6 min | 2.21 / 5.0 % / 2.3 % / 10.7 min |
| 11-18 19:21–22:39, CWT evening | 2.09 / 4.3 % / 5.8 % / 10.1 min | 3.04 / 8.8 % / 15.2 % / 3.6 min | 2.01 / 9.1 % / 4.1 % / 10.0 min |

The off share rises a little with the memo only because the re-spots it
drops were on the right frequency: off spots 99 → 92 (long) and 74 → 68
(CWT). Calls ever spotted on RBN's frequency unchanged — the 1–3 "lost"
were spotted minutes before the window and, rightly, not again inside it.
Hold 15 s: re-spots within 60 s 3.4 % on the long tap; 60 s: 1.7 %, but two
calls lost there. What is left in the CWT evening is the misread CQ itself
(a call on a runner's frequency that the hold does not catch when the
station never called on its own).

## A caller is not calling (2026-11-19)

What the hold leaves in a CWT evening is a call read as CQ on another
station's frequency. The extractor gives a call the calling context when
it falls within CQ_WINDOW (3) tokens after a CQ; a caller answering
right after the runner's CQ is decoded in the same channel and lands in
it ("CQ CWT SP3FSM PA3AAV"). The report rule that takes it back needs an
RST, and a name-and-number exchange has none ("PA3AAV GE GERT 2624").

- `SKIM_CALL_CQNAMED`: the window belongs to the call it names first; a
  different call later in it does not inherit it (its torn head or longer
  reading does).
- `SKIM_CALL_NAMEEXCH`: a call followed by a name and a number or a short
  state is being worked — addressed, so a window reaching it across an
  over break ("CQ CWT · PA3AAV GE GERT 2624", the runner's call not read)
  is withdrawn.

Live taps, both against the feed with the snap, the hold and the memo:

| window | spots | calls | over 150 Hz off RBN | vs R: recall L / bust L |
|---|---|---|---|---|
| weekday afternoon 3 h | 605 → 597 | 318 → 311 | 14 → 14 | 69.4 / 4.5 % → 69.2 / 4.4 % |
| 17.5 h, 7 bands | 2478 → 2356 | 907 → 876 | 92 → 86 | (R down) |
| CWT evening 3.3 h | 1011 → 923 | 431 → 399 | 68 → 56 | 81.5 / 2.9 % → 79.7 / 3.1 % |

The calls the rules drop (CWT 32, 17.5 h 34, afternoon 7): RBN had them
only elsewhere 19 / 13 / 4, never spotted them 6 / 13 / 2, and also on
that frequency 7 / 8 / 1 — read in the decodes, nearly all of those
answered a runner too (other skimmers fell for the same; MX4U was a bust
of 3X4U in "CQ 3X4U MX4U UP"); a real loss is OK7PZ, whose window named a
garbled OK7W first. Against R the CWT recall falls only because L stops
finding stations R does not spot either (L-only 196 → 157); of R's
stations L finds 263 → 260. Alone: CQNAMED drops 24 calls in the CWT
evening (1 on RBN's frequency lost), NAMEEXCH 9 (3).

What is left off frequency is no longer one thing: QSYs inside RBN's
±5 min, a station read 2 kHz from its carrier (SM5ACQ "CQ CWT SM5ACQ" on
7029.4, RBN 7031.4), garbled exchanges ("N7US GEORGE 3T33"). A first
count put 87 % of the CWT evening's off spots on a runner's frequency;
on a band that full almost every frequency has somebody within 150 Hz, so
that count was too generous.

## Tried and not kept

- Extractor: noise letters not ageing the CQ window, a 5-token window,
  a glued TEST as a marker, splitting the glued over-break mark off
  (gh#20) — each about +1 recall for +0.6 … 1.3 busts.
- Feed gate: a cut call yields to its longer twin (`SKIM_GATE_CUT`,
  not committed) — 5 busts fewer in 50 h; cut heads go out before the
  full call is read. Release a held call once read again
  (`SKIM_FEED_CONFIRM`) — 120 s + 1 read: 64.2 / 2.8, little over the
  plain hold: many of R's stations are read only once or twice.
- QSO words glued on (`SKIM_CALL_GLUE`: RO3ZPSE, G4ENBK) — real but rare,
  ±2 busts per set; off.
- A TU window that never makes a call clean-calling
  (`SKIM_CALL_TU_STRICT`) — same busts as the default, and it read a
  contest runner's "5NN TU M0NGN 5NN" as addressed.
- 30 m weak-signal variants (envelope 30 / 35 Hz, re-read at ev 1.5):
  1–3 stations either way on ~160 of R's — noise. The re-read never
  fires on signals that weak.

## What is left

Weak signals, mostly under 15 dB: L reads R's weak DX as garbage or
misreads, which no rule after the decoder can recover. That needs the
decoder to read more of them — not a policy.
