/*
 * skimmer-cw-bench — run the CW backend alone on recorded channels.
 *
 *   skimmer-cw-bench FILE...
 *
 * Each FILE is one channelizer channel as cf32 (interleaved float I/Q at
 * the CW channel rate, 250 Hz) — what skimmer-replay writes with
 * SKIM_DUMP_CHAN + SKIM_DUMP_DIR. Every file gets a fresh decoder; the
 * output is "== FILE" and then one "<stream s> |text|" line per decode.
 * The decoder's own SKIM_V2_* / SKIM_CW_V1 switches apply, so a change to
 * the decoder is screened in seconds against real channels
 * (skimmer-compare/cqww/corpus.py bench) before a full replay confirms it.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <stdio.h>

#include "engine/decode_cw.h"

#define RATE 250.0
#define BLK  64                           /* frames per process() call      */

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: skimmer-cw-bench FILE.cf32...\n");
    return 2;
  }
  const SkimDecodeBackend *b =
      g_getenv("SKIM_CW_V1") ? skim_decode_cw() : skim_decode_cw_v2();
  static float buf[2 * BLK];
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) {
      fprintf(stderr, "cannot open %s\n", argv[i]);
      continue;
    }
    gpointer st = b->channel_new(RATE);
    guint64 n = 0;
    size_t k;
    printf("== %s\n", argv[i]);
    while ((k = fread(buf, 2 * sizeof(float), BLK, f)) > 0) {
      SkimDecode d;
      if (b->process(st, buf, (guint)k, &d)) {
        printf("%.2f |%s|\n", (double)n / RATE, d.text);
      }
      n += k;
    }
    b->channel_free(st);
    fclose(f);
  }
  return 0;
}
