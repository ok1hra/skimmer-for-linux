# web/rtty — RTTY modules from WIFILT

A copy (not a submodule) of five browser modules of WIFILT's RTTY-ICOM,
used unchanged by the waterfall's RTTY palette (`web/rtty-palette.js`):

| file           | what                                                      |
|----------------|-----------------------------------------------------------|
| rtty-codec.js  | Baudot/ITA2 decoder (Goertzel, ATC, USOS; DEC 1 and DEC 2) |
| rtty-afc.js    | AFC tracker and AUTOTUNE (mark/space pair finder)          |
| spectrum.js    | the audio FFT and its small waterfall                      |
| rtty-scope.js  | live spectrum + waterfall + the mark/space markers         |
| rtty-rxlog.js  | the two-column RX tape                                     |

- Upstream: https://github.com/ok1hra/wifilt, `data/`
- Pinned commit: fd86b55057e44d2f650f163995517ee9e97e21cf (2026-11-17)
- License: GPL-3.0, the same as this project.

Do not edit these files here: a fix goes to WIFILT first and is copied back
(same policy as `vendor/wdsp`). What is the skimmer's own — the audio
source, tuning, settings — lives in `web/rtty-palette.js`.
