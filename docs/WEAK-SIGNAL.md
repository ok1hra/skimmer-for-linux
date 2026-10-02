# Weak-signal CW: why the narrow path, and what was measured

Skimmer-compare (2026-10-02) compared the headless skimmer (L, cw-v2) with CW Skimmer Server (R). Both listen to the same antenna. Taken on R's SNR scale, L caught 20 / 35 / 54 / 62 / 72 / 79 % of R's stations at < 5 / 5–10 / 10–15 / 15–20 / 20–25 / ≥ 25 dB. L trailed at every SNR below ~25 dB, not only below 10. Note that binning by each side's own SNR hides this. L's old SNR was floored near 9 dB, so every weak station L caught landed in a higher bin than the ones it missed (skimmer-compare now bins on R's scale).

## The bench

`skimmer-sweep` runs the whole offline pipeline: channelizer → decoder → extractor → station table → RBN policy. Each run puts 12 CQing stations into a 48 kHz band for 60 s. It reports, as a function of SNR in 500 Hz, how many calls reach the feed and how many wrong lines the feed sends. The scenarios are:

- tone at the channel centre, at the channel edge, or anywhere in the channel
- Rayleigh QSB
- hand-key jitter
- drift
- chirp
- a +10 / +20 dB neighbour 50 / 100 / 200 Hz away
- pure noise

`meson test weak-sweep` is its quick gate. Result CSVs from this work are in `/var/tmp/skimmer-iq/sweep/` on OK1HRA's machine.

## What v2 lost, and where

Each item was measured on the bench and confirmed in the code.

- **106 Hz of noise.** v2 decodes `|IQ|` of the whole 125 Hz channel; there is no matched filter. Its squelch opens at a 12 dB peak/mean envelope ratio.
- **Up to 6 dB on a channel boundary.** The CW prototype is −6 dB at ±62.5 Hz. A tone there loses that much SNR in both channels. Midpoint at 50 % recall: centre 2.4 dB, edge 6.3 dB.
- **Neighbours.** A decode was dropped when any channel within ±2 held a +6 dB stronger envelope peak. As a result, a +10 dB station 200 Hz away cost the call at **any** SNR (19 % of calls at ≥ 12 dB).

## The narrow path (`[decode] path=both`, SkimDecodePath)

How it works:

- The tone splitter's focus slot (a mix onto the carrier and a 1.3 × WPM low-pass) runs on every clean detected carrier. The wide channel runs where no carrier is detected.
- Carriers are searched to ±85 Hz but owned only within half the channel spacing. A neighbour's carrier also owns its skirt (to 45 Hz).
- The mirror and skirt tests compare carrier and sideband powers with the channel response taken back out. Next to a channel edge, the filter leaves a keying sideband pair lopsided, and the inner sideband used to read as a second station. That flagged a strong station's channel "contested" and dropped its text.
- Narrow slots are arbitrated by carrier frequency, not by envelope level.
- A strong carrier (over the old 21 dB focus bar) gets the 55 Hz cutoff. The speed-riding cutoff fused its dits, the F5IN lesson again (N8RYH → T8RYH).

SNR500 at 50 % of calls in the feed, wide → both (2–3 × 12 stations a point, real MASTER.SCP):

| scenario | wide | both |
|---|---|---|
| channel centre | 2.4 dB | 0.2 dB |
| channel edge | 6.3 | 1.2 |
| QSB | 12.0 | 9.0 |
| hand key | 2.8 | 1.1 |
| drift | 2.6 | 0.6 |
| chirp | 2.8 | 1.2 |
| +10 dB neighbour 200 Hz | never | 1.0 (≥ 12 dB: 19 → 95 % of calls) |
| +10 dB neighbour 100 Hz | never | 0.0 |
| +20 dB neighbour ≤ 100 Hz | never | never |

Further results:

- **Noise.** There was no feed line in 30 min of pure noise on either path.
- **Wrong lines.** 0.5 per 100 stations below 14 dB and 1.2 above, against 0.4 on the wide path.
- **Channel-edge tone above 22 dB.** 95 % of calls, against 100 % on the wide path. Before the channel-response compensation it was 87–90 %.
- **CPU.** About 2× per band (a 96 kHz band replays 14× real time instead of 28×).

## Tried and rejected (the bench said no)

- **The carrier detector as v2's gate** instead of its envelope squelch: 2–5 dB worse in every scenario.
- **Replaying 4 s of channel history into a newborn slot** (the head of the over): ±0.5 dB, which is noise.
- **Focus bar kept at 21 dB** (strong carriers stay wide): with a +10 dB neighbour 200 Hz away, 59 % of calls at ≥ 12 dB against 94 % without the bar.
- **Focus cutoff floor 15 / 20 / 25 Hz:** identical within 0.3 dB, because 1.3 × WPM sits above the floor at 16–33 WPM.
- **Candidate age clock skipping the lone E / T / I of noise:** no measurable effect.
- **Spotting a MASTER.SCP call after one "CQ CALL" read** (score 0.80): +1.2 points of recall, but a large share of the lines it added were torn calls the dictionary also knows (UT7E for UT7TM, SP4Z for SP4ZH). Neither the decoder's confidence nor a 20 s hold for the full call to evict the torn one filtered them.
- **Wide lane decoding beside a strong carrier's slot** (both texts to the station table): +1 point on channel-edge tones, the rest unchanged, within noise. The loss it was meant to cover was the contested flag, fixed at its cause.
- **A strong carrier's slot as the mixer alone, with no filter:** identical to the 55 Hz cutoff, with more wrong lines next to a neighbour.
- **Rician/Rayleigh LLR in the Viterbi** (instead of the span + Rayleigh heuristic): QSB 73 → 44 % of calls at ≥ 12 dB, everything else slightly worse, at sample weights 0.15–0.6.

## SNR in spots

v2 now estimates carrier over noise power in the band it was fed, from its discriminator means, and the pipeline converts that to SNR in 500 Hz (decode.h `snr_in_band`).

| | true 2 dB | 6 dB | 10 dB | 14 dB |
|---|---|---|---|---|
| old estimate (wide path) | 11.5 | 15.0 | 17.8 | 20.7 |
| new estimate (narrow path) | 3.6 | 6.0 | 10.0 | 13.0 |

Above ~15 dB the new estimate compresses (30 → 22). Whether CW Skimmer's scale is SNR in 500 Hz is still open; the first live comparison will show it.

## Still ahead

- A +20 dB neighbour within 100 Hz. The splitter drops a second carrier more than 12 dB under the first (`TS_REL_DB`).
- Live verdict: real-air A/B on long `[record]` recordings with `skimmer-compare/replay-vs-r.py`, then a night in skimmer-compare.
