# web/js8 — JS8 frame semantics from WIFILT

A copy (not a submodule) of WIFILT's browser JS8 protocol module and its
JSC dictionary, used unchanged by the waterfall's JS8 palette
(`web/js8-palette.js`): the skimmer sends the raw frames it decodes
(`/api/js8`), these turn them into what JS8Call shows.

| file            | what                                                          |
|-----------------|---------------------------------------------------------------|
| js8-protocol.js | `Js8Protocol`: every frame type unpacked as JS8Call's Varicode does (heartbeat, CQ, compound, directed, Huffman and JSC data), `ActivityStore` — frames put back together into messages, a channel per submode and offset, gaps and the end frame |
| js8-jsc.bin     | the JSC dictionary (262 144 words, format JSC3) the compressed data frames index |

- Upstream: https://github.com/ok1hra/wifilt, `data/`
- Pinned commit: fd86b55057e44d2f650f163995517ee9e97e21cf (2026-11-17)
- sha256: js8-protocol.js `c3ef5850ea7a59720cd3f7b347ae9664499557ea3b6d657b51f99779b8531c9b`, js8-jsc.bin `e7b916ee187f0be2d347a2c4417e616c9898c5c4cf417ddb5671ef0d3b139cc1`
- License: GPL-3.0, the same as this project (the JSC dictionary comes from
  JS8Call, GPL-3.0).

Do not edit these files here: a fix goes to WIFILT first and is copied back
(as `web/rtty/`). The palette — polling, the list, the waterfall stamps —
is the skimmer's own, in `web/js8-palette.js`. The server's own reading of
a frame (the sender for a spot) is `src/headless/js8_unpack.c`, checked
against this module's output in the `js8` gate.
