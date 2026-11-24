/* js8_unpack — what a JS8 frame says, as far as a spot needs it.
 *
 * A frame is skimmer-js8d's RAW (12 characters of JS8's alphabet72 = 72
 * bits) plus its 3 transmission bits (1 first, 2 last, 4 fast data). This
 * reads the frames that carry callsigns — heartbeat (CQ), compound,
 * compound-directed, directed — the way JS8Call's Varicode and WIFILT's
 * js8-protocol.js do; data frames (Huffman / JSC text) are only marked as
 * such: their text is unpacked in the browser (web/js8/js8-protocol.js).
 * The packers exist for the gates (synthetic frames). GLib only. */
#ifndef SKIM_JS8_UNPACK_H
#define SKIM_JS8_UNPACK_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  SKIM_JS8_HEARTBEAT = 0,            /* HB or CQ: callsign + grid            */
  SKIM_JS8_COMPOUND = 1,             /* a compound sender (+ grid)           */
  SKIM_JS8_COMPOUND_DIRECTED = 2,    /* a compound ADDRESSEE (+ command)     */
  SKIM_JS8_DIRECTED = 3,             /* from: to command                     */
  SKIM_JS8_DATA = 4,                 /* text, no callsign of its own         */
  SKIM_JS8_UNKNOWN = 255,
} SkimJs8Kind;

#define SKIM_JS8_TYPE_FIRST 1
#define SKIM_JS8_TYPE_LAST  2
#define SKIM_JS8_TYPE_FAST  4

typedef struct {
  SkimJs8Kind kind;
  gboolean cq;          /* heartbeat with the CQ bit (@ALLCALL CQ …)         */
  char from[20];        /* the station that sent it; "" when the frame names
                         * none (compound-directed, data) or "<....>"      */
  char to[20];          /* the addressee (@HB / @ALLCALL for a heartbeat)    */
  char grid[5];         /* 4-character locator or ""                         */
  char cmd[20];         /* directed command ("SNR", "73", "CQ CQ CQ" …) or "" */
  char text[64];        /* JS8Call's rendering of the frame ("" for data)    */
} SkimJs8Frame;

/* FALSE if raw is not 12 alphabet72 characters. */
gboolean skim_js8_unpack(const char *raw, int type, SkimJs8Frame *out);

/* The 50-bit alphanumeric field (heartbeat / compound) and the 28-bit
 * callsign (directed), both ways; packers return FALSE when the call does
 * not fit the field. Exposed for the gates. */
gboolean skim_js8_pack_heartbeat(const char *call, const char *grid, gboolean cq,
                                 int cq_index, char raw[13]);
gboolean skim_js8_pack_compound(SkimJs8Kind flag, const char *call, int number,
                                int bits3, char raw[13]);
gboolean skim_js8_pack_directed(const char *from, const char *to, int command,
                                int number, char raw[13]);
int      skim_js8_pack_grid(const char *grid);   /* 15 bits; 0x7fff = none */

G_END_DECLS

#endif
