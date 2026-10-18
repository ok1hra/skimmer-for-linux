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
