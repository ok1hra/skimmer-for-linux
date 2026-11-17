# CW decoder v3: v2 plus a weak-signal path

v3 (`src/engine/decode_cw_v3.c`, `[decode] engine=v3`, the default on the
`v3` branch) is v2 with everything new confined to stations v2 cannot read.
A station whose SNR in 500 Hz (measured by v3, the scale CW Skimmer Server
reports on) is above `SKIM_V3_WEAK_DB` runs v2's view verbatim: with the weak
view switched off (`SKIM_V3_WEAK_DB=-99`) v3's decoded text is v2's byte for
byte on the 20 m golden slice. `cw:v2` / `SKIM_CW_ENGINE=v2` keep v2.

## Why (measured before v3, docs/WEAK-SIGNAL-PLAN.md §7–10)

- Under 10 dB (R's SNR) ~43 % of the CQ stations R spotted had nothing like
  their call in v2's text. Holding v2's gate open turned 9–11 points of them
  into reads but 0–2 into spots: the reading itself is the loss.
- Above 10 dB 11–25 % were read exactly and not spotted; 69 % of those are
  torn or glued calls and CQs the extractor's window does not reach.
- Forced alignment of R's call on the decoder's LLR (the kws oracle) found
  291 of the 711 CQ stations v2 missed, 218 of them with a candidate in the
  text, at ~30 ppm false alarms per call and window.
- R's clock, the carrier measured in the IQ and the ghost rule changed
  nothing: speed tracking, AFC and neighbours are not where v2 loses.

## The literature it rests on

| Source | What v3 takes from it |
|---|---|
| E. L. Bell, NPS MS thesis 1975 (DTIC ADA019493) and DEng 1977 (ADA046503) | Bayesian decoding with the amplitude tracked per hypothesis; noise from the envelope minimum; "a 100 Hz filter costs ~10 dB against the optimum 0.613/T". 4 % letter errors at Es/N0 ≈ 13.8 dB (20 WPM) |
| D. L. Mills, Univ. of Maryland TR-554, 1977 (DTIC ADA051001) | Matched filter = a moving sum one element long; Viterbi ~5 dB over a fixed classifier; amplitude in the likelihood is "worthless" unless tracked — why v2's Rician try lost |
| PA3FWM, RSCW | Coherent sum over a bit, magnitude, correlation with the candidate's pattern: machine CW nearly error-free at Es/N0 12 dB |
| VE3NEA via AG1LE (2013) | No hard decision per sample; carry probabilities up to the word |
| N4ZR/VE3NEA, RBN blog 2014 | How CW Skimmer measures SNR: carrier in 50 Hz, noise density over the band × 500 Hz, key-on samples, Rayleigh model |
| AG1LE, FLDIGI CER tests 2013 | 35 Hz vs 68 Hz filter at −13 dB (3 kHz): CER 0.15 vs 0.66 |
| J. Taylor K1JT, QEX 2005 (JT65 §11 Deep Search) | A callsign database as a bank of matched filters: ~4 dB, safe only with a margin over the runner-up |
| Proakis (noncoherent OOK, Rayleigh fading), Raheli et al. 1995 (per-survivor processing) | The exponential-power LLR; time diversity of repeated calls against fading |

## What the weak path is

1. **Carrier detector** on every channel: a 64-point spectrum of the 250 Hz
   stream every 256 ms, 2 s EMA; a bin 6 dB (`SKIM_V3_DET_DB`) over the floor
   for 1 s (`SKIM_V3_DET_S`) makes a candidate. The floor is the larger of
   the band's (snr_meter via the `set_floor` hook of decode.h) and the
   channel's own median over its passband. A carrier belongs to the channel
   whose centre is nearest. The peak, interpolated, is the AFC.
2. **Matched statistic**: derotation onto the carrier, a 60 Hz low-pass, a
   coherent sum over half a dit, its power r². A 6 s raw ring runs through
   it when the candidate is born, so the pre-roll holds the head of the over.
3. **Calibrated LLR**: `ln(σ0²/σ1²) + r²(1/σ0² − 1/σ1²)`, weighted 1/L for the
   sum's correlation. σ0² is the noise power of r² (a truncated-exponential
   estimate from its lower tail, bounded by the band floor), σ1² the held
   mark power (the mean of samples within 6 dB of a 2 s peak). The ratio is
   capped at 9, where the zero crossing sits at half amplitude — the half-dit
   sum's edges keep their true length.
4. **Gate**: a candidate, and a soft presence log-odds (keyed carrier : noise,
   1 s EMA); a locked channel keeps its over through v2's 16-dit break.
   Under `SKIM_V3_MIN_DB` (−4 dB) the weak view is dead air.
5. **Views**: the weak view is chosen at a candidate's birth and between
   overs only (hysteresis 3 dB). Strong channels never pay for the matched
   path while their lattice runs.
6. The lattice, fist model, clock and word re-read are v2's; the re-read
   has its own evidence bar on the calibrated scale (`SKIM_V3_RS_EV_WEAK`).

## The call reader (M8, all views)

Every 3 s on a locked channel the last 6 s of the lattice's LLR, a slot every
third of a dit, are searched for the MASTER.SCP calls the text in that window
points at: tokens two edits away, two or three torn tokens joined, calls
inside a glued token, calls this channel confirmed in the last 5 min. A call
within `SKIM_V3_KWS_DC` (−5) of the free reading that beats every candidate
and its own dictionary twins by `SKIM_V3_KWS_SEP` (3), read in two windows
that share no audio (or once at −2 with a margin of 5), goes to the extractor
as a correction token (`callsign.h` FIX), at a word gap, at most every 20 s,
with a CQ token before it when the window's text or the audio (a CQ/TEST
alignment within 3) says the over calls. Alignments stay 4 dits inside the
window (a call cut by the edge fits as its own shorter twin). Unknown calls
are never written. `SKIM_V3_KWS=0` turns it off, `SKIM_V3_KWS_DEBUG=1` prints
every read.

## Tried and changed on the way (20 m golden slice, cw-decode)

- The calibrated LLR alone on every channel: strong signals fused their dits
  (the crossing sank onto the half-dit sum's ramps) — the ratio cap.
- The 20th percentile as σ0²: an exponential's is 0.22 of the mean, noise
  read as marks — the truncated-exponential estimate.
- A detector floor from the median of all 64 bins: the stopband pulls it
  down, every channel had a "carrier" — the passband median.
- The weak path on strong stations too: lost what v2 reads on live air —
  strong stations keep v2's view.
- The presence gate alone mid-over: a weak over broke at every word gap —
  the 16-dit hold once locked.
- `SKIM_V3_MIN_DB` −7: noise read into dictionary words (EE3E, EE5M); −4.

## Tried and changed on recordings (5 h replays against CW Skimmer Server)

- **Dead channels.** Recording 2 had 24 + 8 + 4 channels (40/20/30 m) that
  v3 silenced for hours, strong stations included (HB9DGZ, R8MC). A weak
  view near −3 dB glued marks; the clock followed the marks to its 4 WPM
  floor; the carrier rule fired; the fist forget zeroed the clock but kept
  the carrier, and the over break restarted the lattice with no clock on
  every block. Fixed in layers: no lattice without a clock, the forget
  clears the carrier, a weak-view watchdog drops a clock whose
  intra-character spaces collapse under 0.35 dit, a stall watchdog drops
  one that commits nothing for 10 s on a live channel, and the matched sum
  takes its length from (dit + esp) / 2, at most 10 samples. The stall
  watchdog and the length rule alone did not help — measured.
- **The call reader without a calling context.** It confirmed 21 of the
  162 stations R spotted and v2 missed (recording 2), and none of them
  spotted: the window that confirms a call often starts after the over's
  CQ, and the free text glues it ("CQCE"). Glued "CQxx" tokens count now,
  and CQ evidence (text or audio) covers the channel for 12 s.
- **`SKIM_V3_DEAD_S` 1.0** (an unlocked weak over breaks after 1 s of no
  presence instead of 0.3): recording 2 bust 6.9 → 6.4 but every bin under
  20 dB 1–2.6 points down; 0.3 stays.
- **The reader alone** (`SKIM_V3_WEAK_DB=-99`, v2's view + M8) adds nothing
  over v2 (69.3 / 7.4 vs 69.8 / 7.5): the gain needs the weak view.

## Results

First 5 h of recordings 1 and 2, 30/20/40 m, `tap-score.py` against CW
Skimmer Server (session 20261013-183853). Recall and bust of L in %,
then L of R by R's SNR (dB).

| | recall | bust | < 10 | 10–15 | 15–20 | 20–25 | 25–30 | ≥ 30 |
|---|---|---|---|---|---|---|---|---|
| rec 2, v2 | 69.8 | 7.5 | 36.0 | 58.0 | 64.7 | 72.9 | 77.4 | 80.4 |
| rec 2, v3 | 72.9 | 6.9 | 53.3 | 54.4 | 63.4 | 77.1 | 76.9 | 76.1 |
| rec 1, v2 | 56.9 | 2.3 | 14.8 | 35.5 | 57.1 | 50.0 | 90.0 | 77.8 |
| rec 1, v3 | 65.3 | 3.4 | 30.4 | 32.3 | 54.5 | 46.2 | 90.0 | 88.9 |
| rec 3, v2 | 83.1 | 0.8 | 51.0 | 77.1 | 82.8 | 80.8 | 76.9 | 80.6 |
| rec 3, v3 | 86.7 | 0.8 | 66.5 | 77.6 | 83.5 | 83.5 | 77.9 | 81.1 |

Recording 3 is held out (nothing was tuned on it): its last 5 h, 05:17–10:17Z,
2117 stations; its ≥ 30 column is 30–35 dB (35+: v2 82.6, v3 81.0).
Recording 1 has 144 stations (one station ≈ 0.7 point), recording 2 566.
v3's remaining loss at 10–20 dB is mostly calling context again: overs
torn by over breaks in the weak view, the CQ on one side, the call on the
other.

CPU: a 5 h replay of three bands takes v3 about 2.7× v2's time.

## Precision: the reader's own busts (opt-in, 2026-11-17)

Live 2026-11-15..17 (36.7 h against CW Skimmer Server): v3 recall 75.9 vs
R 72.9, bust 4.9 % vs 2.4 %, and 550 of 2882 stations L "found" were vouched
for by MASTER.SCP alone (no RBN skimmer, not R; R has 16 such). Each L
episode was traced back to what started it: a reader correction (FIX
token) or the free text (`skimmer-compare/skimcmp/fixsrc.py`,
`tap-score.py --fix`). Corrections whose call the text read exactly (class
C) were sound: 773 good, 94 SCP-only, 10 busts. The bad ones were:
- A: the text read a valid call and the reader "corrected" it to a
  different SCP call (W1KE → W1KT, EA3CE → TA3CE). 56 / 66 / 10.
- B: the reader took the head of a torn call ("SM5D AJ" → SM5D, since
  SM5DAJ is not in SCP). 48 / 40 / 16.
- D: nothing like the call was in the text, mostly short SCP calls fitted
  into noise (G0T, E2E). 70 / 162 / 4.

Three switches, all default off:

- `SKIM_V3_KR_RIVAL=1`: the text's own readings compete with the winner,
  known to the dictionary or not. These are valid calls the text read
  whole or as 2–3 torn tokens, within `skim_feed_gate_similar` ≤ 3 of the
  winner. A rival only blocks; unknown calls are still never written.
- `SKIM_V3_KR_TWO=1`: a correction the text does not support (only a near
  form of a fragment, or the hit memory), or a call of 4 characters or
  fewer that the text did not read, needs two reads with no shared audio.
  The hit memory alone no longer counts as a read.
- `SKIM_GATE_HEAD=1` (`pipeline.c`, `near_conflict`): a call and its own
  longer reading on the same frequency are one station that lost its tail.
  Before this, the near-call rule kept the full call off the wire because
  SCP knows the head. SM5DAJ was read 298 times and held, while SM5D, read
  once, went out. Now the head yields to a longer reading read at least
  twice and at least as often. A glued word (K, E, R, DX, KN, TU, 5NN, the
  call's own head again) is not a tail.

Results: recall, bust, stations confirmed by RBN or R, stations vouched for
by SCP alone, and busts.

| | recall | bust | confirmed | SCP alone | busts |
|---|---|---|---|---|---|
| rec 2, v3 | 72.9 | 6.9 | 374 | 64 | 34 |
| rec 2, RIVAL + TWO | 72.2 | 6.3 | 374 | 55 | 30 |
| rec 2, + HEAD | 73.3 | 5.8 | 382 | 57 | 28 |
| rec 3, v3 | 86.7 | 0.8 | 1346 | 673 | 17 |
| rec 3, RIVAL + TWO | 86.2 | 0.8 | 1346 | 613 | 16 |
| rec 3, + HEAD | 86.2 | 0.8 | 1347 | 613 | 16 |

Recording 3 (a contest, WAG) is the guard for the strong contest stations
that L hears and R does not.
- With RIVAL + TWO, its 1346 confirmed stations stayed. Of the 45 SCP-only
  stations that went, most were busts of a call RBN spotted on the same
  frequency (K6JS for DK6JU, W6AG for DL6AG, DA3T for DA3M, DD5Y for
  DL5YM), or a caller spotted with the runner's CQ (DF7TS answering LZ1ZF).
- HEAD alone, on the live v3 taps 11-15..17: confirmed 2490 → 2508, busts
  174 → 173.
- HEAD alone, on the v2 contest taps 11-14: confirmed 4314 → 4320, busts
  84 → 82, L-only ≥ 15 dB 2059 → 2063.
- The tap replay of the live v3 taps matches live within one station in
  250 (confirmed 2490 vs 2489, busts 174 vs 170).

Tried and not kept (tap replay of the live v3 taps):
- A glue tail K/E/DX at one read ratio instead of two: no bust gone,
  +3 confirmed.
- A non-SCP call whose 1–2 character longer form SCP knows (DF6Y, DF6YT)
  needs 3 reads: −6 busts but −5 confirmed.
- Rules over the remaining non-SCP busts (≤ 3 characters, a locator shape,
  ≥ 40 WPM, 4 characters read once): each cost about as many real
  stations as busts.

What remains is mostly text busts, the same kind v2 had: heads of calls
read only later (R4NC before R4NCU), and busts that are single-read and
non-SCP (TM140FHBK read as T4EHBK). `SKIM_V3_KR_CQX=1` (an audio-only CQ
vouches only for a call the text read) is built but not measured.
