/* js8_unpack — JS8 frames that carry callsigns (see js8_unpack.h). A port of
 * the frame layouts in JS8Call's Varicode.cpp as WIFILT's js8-protocol.js
 * has them (the reference the gate's vectors come from). */
#include "js8_unpack.h"

#include <string.h>

__extension__ typedef unsigned __int128 U72;   /* the 72-bit frame */

static const char A72[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?.";
static const char AN[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
#define NBASECALL (37L * 36 * 10 * 27 * 27 * 27)
#define NBASEGRID (180 * 180)
#define NUSERGRID (NBASEGRID + 10)
#define NMAXGRID ((1 << 15) - 1)

static const char *const COMMANDS[32] = {
  " SNR?", " DIT DIT", " NACK", " HEARING?", " GRID?", ">",
  " STATUS?", " STATUS", " HEARING", " MSG", " MSG TO:", " QUERY",
  " QUERY MSGS", " QUERY CALL", " ACK", " GRID", " INFO?", " INFO",
  " FB", " HW CPY?", " SK", " RR", " QSL?", " QSL", " CMD", " SNR",
  " NO", " YES", " 73", " HEARTBEAT SNR", " AGN?", " ",
};
static const char *const SPECIAL[] = {
  "<....>", "@ALLCALL", "@JS8NET", "@DX/NA", "@DX/SA", "@DX/EU",
  "@DX/AS", "@DX/AF", "@DX/OC", "@DX/AN", "@REGION/1", "@REGION/2",
  "@REGION/3", "@GROUP/0", "@GROUP/1", "@GROUP/2", "@GROUP/3",
  "@GROUP/4", "@GROUP/5", "@GROUP/6", "@GROUP/7", "@GROUP/8",
  "@GROUP/9", "@COMMAND", "@CONTROL", "@NET", "@NTS", "@RESERVE/0",
  "@RESERVE/1", "@RESERVE/2", "@RESERVE/3", "@RESERVE/4", "@APRSIS",
  "@RAGCHEW", "@JS8", "@EMCOMM", "@ARES", "@MARS", "@AMRRON",
  "@RACES", "@RAYNET", "@RADAR", "@SKYWARN", "@CQ", "@HB", "@QSO",
  "@QSOPARTY", "@CONTEST", "@FIELDDAY", "@SOTA", "@IOTA", "@POTA",
  "@QRP", "@QRO",
};
#define NSPECIAL ((long)G_N_ELEMENTS(SPECIAL))
static const char *const CQ[8] = {
  "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ",
};

static gboolean unpack72(const char *raw, U72 *v) {
  if (!raw || strlen(raw) != 12) { return FALSE; }
  U72 x = 0;
  for (int i = 0; i < 12; i++) {
    const char *p = strchr(A72, raw[i]);
    if (!p || !raw[i]) { return FALSE; }
    x = (x << 6) | (U72)(p - A72);
  }
  *v = x;
  return TRUE;
}

static void pack72(U72 v, char raw[13]) {
  for (int i = 0; i < 12; i++) { raw[i] = A72[(int)((v >> (66 - 6 * i)) & 63)]; }
  raw[12] = 0;
}

static guint64 field(U72 v, int start, int count) {
  return (guint64)((v >> (72 - start - count)) & (((U72)1 << count) - 1));
}

/* ---- callsigns --------------------------------------------------------- */

static void unpack_call28(guint64 value, gboolean portable, char *out, gsize size) {
  const long special = (long)value - NBASECALL - 1;
  if (special >= 0 && special < NSPECIAL) { g_strlcpy(out, SPECIAL[special], size); return; }
  char w[7];
  guint64 p = value;
  w[5] = AN[p % 27 + 10]; p /= 27;
  w[4] = AN[p % 27 + 10]; p /= 27;
  w[3] = AN[p % 27 + 10]; p /= 27;
  w[2] = AN[p % 10]; p /= 10;
  w[1] = AN[p % 36]; p /= 36;
  w[0] = p < sizeof AN - 1 ? AN[p] : '?';
  w[6] = 0;
  char *c = g_strstrip(w);
  char call[16];
  if (g_str_has_prefix(c, "3D0")) {
    g_snprintf(call, sizeof call, "3DA0%s", c + 3);
  } else if (c[0] == 'Q' && c[1] >= 'A' && c[1] <= 'Z') {
    g_snprintf(call, sizeof call, "3X%s", c + 1);
  } else {
    g_strlcpy(call, c, sizeof call);
  }
  g_snprintf(out, size, "%s%s", call, portable ? "/P" : "");
}

static long pack_call28(const char *in, gboolean *portable) {
  char call[16];
  g_strlcpy(call, in, sizeof call);
  g_strstrip(call);
  for (char *q = call; *q; q++) { *q = g_ascii_toupper(*q); }
  for (long i = 0; i < NSPECIAL; i++) {
    if (strcmp(call, SPECIAL[i]) == 0) { *portable = FALSE; return NBASECALL + i + 1; }
  }
  *portable = FALSE;
  gsize n = strlen(call);
  if (n > 2 && strcmp(call + n - 2, "/P") == 0) { call[n - 2] = 0; *portable = TRUE; n -= 2; }
  char t[16];
  if (g_str_has_prefix(call, "3DA0")) { g_snprintf(t, sizeof t, "3D0%s", call + 4); g_strlcpy(call, t, sizeof call); }
  if (call[0] == '3' && call[1] == 'X' && call[2] >= 'A' && call[2] <= 'Z') {
    g_snprintf(t, sizeof t, "Q%s", call + 2);
    g_strlcpy(call, t, sizeof call);
  }
  n = strlen(call);
  if (n < 2 || n > 6) { return -1; }
  /* the permutations JS8Call tries: the digit must land in position 2 */
  char cand[3][8];
  int nc = 0;
  g_strlcpy(cand[nc++], call, 8);
  if (n == 2) { g_snprintf(cand[nc++], 8, " %s   ", call); }
  if (n == 3) { g_snprintf(cand[nc++], 8, " %s  ", call); g_snprintf(cand[nc++], 8, "%s   ", call); }
  if (n == 4) { g_snprintf(cand[nc++], 8, " %s ", call); g_snprintf(cand[nc++], 8, "%s  ", call); }
  if (n == 5) { g_snprintf(cand[nc++], 8, " %s", call); g_snprintf(cand[nc++], 8, "%s ", call); }
  for (int k = 0; k < nc; k++) {
    const char *c = cand[k];
    if (strlen(c) != 6) { continue; }
    const gboolean ok = (g_ascii_isdigit(c[0]) || g_ascii_isupper(c[0]) || c[0] == ' ') &&
                        (g_ascii_isdigit(c[1]) || g_ascii_isupper(c[1])) && g_ascii_isdigit(c[2]) &&
                        (g_ascii_isupper(c[3]) || c[3] == ' ') && (g_ascii_isupper(c[4]) || c[4] == ' ') &&
                        (g_ascii_isupper(c[5]) || c[5] == ' ');
    if (!ok) { continue; }
    long v = strchr(AN, c[0]) - AN;
    v = 36 * v + (strchr(AN, c[1]) - AN);
    v = 10 * v + (strchr(AN, c[2]) - AN);
    v = 27 * v + (strchr(AN, c[3]) - AN) - 10;
    v = 27 * v + (strchr(AN, c[4]) - AN) - 10;
    v = 27 * v + (strchr(AN, c[5]) - AN) - 10;
    return v > 0 ? v : -1;
  }
  return -1;
}

static void unpack_an50(guint64 p, char *out, gsize size) {
  char w[12];
  for (int pos = 10; pos >= 8; pos--) { w[pos] = AN[p % 38]; p /= 38; }
  w[7] = p % 2 ? '/' : ' '; p /= 2;
  for (int pos = 6; pos >= 4; pos--) { w[pos] = AN[p % 38]; p /= 38; }
  w[3] = p % 2 ? '/' : ' '; p /= 2;
  for (int pos = 2; pos >= 1; pos--) { w[pos] = AN[p % 38]; p /= 38; }
  w[0] = p % 39 < sizeof AN - 1 ? AN[p % 39] : ' ';
  gsize o = 0;
  for (int i = 0; i < 11 && o + 1 < size; i++) {
    if (w[i] != ' ') { out[o++] = w[i]; }
  }
  out[o] = 0;
}

static gboolean pack_an50(const char *in, guint64 *out) {
  char w[16] = "";
  gsize n = 0;
  for (const char *q = in; *q && n < 14; q++) {
    const char c = g_ascii_toupper(*q);
    if (g_ascii_isupper(c) || g_ascii_isdigit(c) || c == ' ' || c == '/' || c == '@') { w[n++] = c; }
  }
  w[n] = 0;
  char t[24];
  if (strlen(w) > 3 && w[3] != '/') { g_snprintf(t, sizeof t, "%.3s %s", w, w + 3); g_strlcpy(w, t, sizeof w); }
  if (strlen(w) > 7 && w[7] != '/') { g_snprintf(t, sizeof t, "%.7s %s", w, w + 7); g_strlcpy(w, t, sizeof w); }
  char p[12];
  g_snprintf(p, sizeof p, "%-11.11s", w);
  int ix[11];
  for (int i = 0; i < 11; i++) {
    const char *f = strchr(AN, p[i]);
    ix[i] = f && p[i] ? (int)(f - AN) : -1;
    if (i != 3 && i != 7 && ix[i] < 0) { return FALSE; }
  }
  guint64 v = (guint64)ix[0];
  v = v * 38 + ix[1];
  v = v * 38 + ix[2];
  v = v * 2 + (p[3] == '/');
  for (int i = 4; i <= 6; i++) { v = v * 38 + ix[i]; }
  v = v * 2 + (p[7] == '/');
  for (int i = 8; i <= 10; i++) { v = v * 38 + ix[i]; }
  *out = v;
  return v != 0;
}

/* ---- grid -------------------------------------------------------------- */

int skim_js8_pack_grid(const char *g) {
  if (!g || strlen(g) < 4) { return NMAXGRID; }
  const char a = g_ascii_toupper(g[0]), b = g_ascii_toupper(g[1]);
  if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !g_ascii_isdigit(g[2]) || !g_ascii_isdigit(g[3])) {
    return NMAXGRID;
  }
  const int lon = (int)(180 - 20 * (a - 'A') - 2 * (g[2] - '0') - 62.5 / 60);
  const int lat = (int)(-90 + 10 * (b - 'A') + (g[3] - '0') + 31.25 / 60);
  return ((lon + 180) / 2) * 180 + lat + 90;
}

static void unpack_grid(int v, char out[5]) {
  out[0] = 0;
  if (v > NBASEGRID) { return; }
  const int lat = v % 180 - 90;
  const int lon = (v / 180) * 2 - 178;
  const int nlong = 60 * (180 - lon) / 5;
  const int nlat = 60 * (lat + 90) / 2.5;
  out[0] = (char)('A' + nlong / 240);
  out[1] = (char)('A' + nlat / 240);
  out[2] = (char)('0' + (nlong % 240) / 24);
  out[3] = (char)('0' + (nlat % 240) / 24);
  out[4] = 0;
  if (out[0] > 'R' || out[1] > 'R') { out[0] = 0; }
}

static void snr_text(int v, char *out, gsize size) {
  v = CLAMP(v, -60, 60);
  g_snprintf(out, size, "%c%02d", v >= 0 ? '+' : '-', ABS(v));
}

/* ---- frames ------------------------------------------------------------ */

static void cmd_trim(const char *c, char *out, gsize size) {
  while (*c == ' ') { c++; }
  g_strlcpy(out, c, size);
}

gboolean skim_js8_unpack(const char *raw, int type, SkimJs8Frame *out) {
  memset(out, 0, sizeof *out);
  out->kind = SKIM_JS8_UNKNOWN;
  U72 v;
  if (!unpack72(raw, &v)) { return FALSE; }
  if ((type & SKIM_JS8_TYPE_FAST) || field(v, 0, 1)) {   /* data: 1xx, or fast */
    out->kind = SKIM_JS8_DATA;
    return TRUE;
  }
  const int flag = (int)field(v, 0, 3);
  if (flag == SKIM_JS8_DIRECTED) {
    const int extra = (int)(v & 0xff);
    unpack_call28(field(v, 3, 28), (extra & 0x80) != 0, out->from, sizeof out->from);
    unpack_call28(field(v, 31, 28), (extra & 0x40) != 0, out->to, sizeof out->to);
    const char *cmd = COMMANDS[field(v, 59, 5)];
    const int number = extra & 0x3f;
    char suffix[8] = "";
    if (number != 0) {
      if (strcmp(cmd, " SNR") == 0 || strcmp(cmd, " HEARTBEAT SNR") == 0) {
        snr_text(number - 31, suffix, sizeof suffix);
      } else {
        g_snprintf(suffix, sizeof suffix, "%d", number - 31);
      }
    }
    cmd_trim(cmd, out->cmd, sizeof out->cmd);
    g_snprintf(out->text, sizeof out->text, "%s: %s%s%s%s", out->from, out->to, cmd,
               suffix[0] ? " " : "", suffix);
    out->kind = SKIM_JS8_DIRECTED;
    return TRUE;
  }
  char call[20];
  unpack_an50(field(v, 3, 50), call, sizeof call);
  const int extra = (int)(v & 0xff);
  const int number = (int)(field(v, 53, 11) << 5) | (extra >> 3);
  const int bits3 = extra & 7;
  if (flag == SKIM_JS8_HEARTBEAT) {
    out->kind = SKIM_JS8_HEARTBEAT;
    out->cq = (number & 0x8000) != 0;
    unpack_grid(number & 0x7fff, out->grid);
    g_strlcpy(out->from, call, sizeof out->from);
    g_strlcpy(out->to, out->cq ? "@ALLCALL" : "@HB", sizeof out->to);
    g_strlcpy(out->cmd, out->cq ? CQ[bits3] : "HEARTBEAT", sizeof out->cmd);
    if (out->cq) {
      g_snprintf(out->text, sizeof out->text, "%s: @ALLCALL %s%s%s", call, CQ[bits3],
                 out->grid[0] ? " " : "", out->grid);
    } else {
      g_snprintf(out->text, sizeof out->text, "%s: @HB%s%s", call, out->grid[0] ? " " : "", out->grid);
    }
    return TRUE;
  }
  /* compound / compound-directed: the number is a grid or a command */
  char cmd[20] = "", suffix[8] = "";
  if (number <= NBASEGRID) {
    unpack_grid(number, out->grid);
  } else if (number >= NUSERGRID && number < NMAXGRID) {
    const int packed = number - NUSERGRID;
    const int ci = (packed & 0x80) ? ((packed & 0x40) ? 29 : 25) : (packed & 31);
    g_strlcpy(cmd, COMMANDS[ci], sizeof cmd);
    if (packed & 0x80) { snr_text((packed & 0x3f) - 31, suffix, sizeof suffix); }
  }
  cmd_trim(cmd, out->cmd, sizeof out->cmd);
  if (flag == SKIM_JS8_COMPOUND_DIRECTED) {
    out->kind = SKIM_JS8_COMPOUND_DIRECTED;
    g_strlcpy(out->to, call, sizeof out->to);
    g_snprintf(out->text, sizeof out->text, "%s%s%s%s", call, cmd, suffix[0] ? " " : "", suffix);
  } else {
    out->kind = SKIM_JS8_COMPOUND;
    g_strlcpy(out->from, call, sizeof out->from);
    g_snprintf(out->text, sizeof out->text, "%s:", call);
  }
  return TRUE;
}

/* ---- packers (the gates) ----------------------------------------------- */

gboolean skim_js8_pack_compound(SkimJs8Kind flag, const char *call, int number, int bits3,
                                char raw[13]) {
  guint64 c;
  if (!pack_an50(call, &c)) { return FALSE; }
  U72 v = ((((U72)flag << 50) | c) << 11) | (U72)((number >> 5) & 0x7ff);
  v = (v << 8) | (U72)(((number & 0x1f) << 3) | (bits3 & 7));
  pack72(v, raw);
  return TRUE;
}

gboolean skim_js8_pack_heartbeat(const char *call, const char *grid, gboolean cq, int cq_index,
                                 char raw[13]) {
  const int number = skim_js8_pack_grid(grid) | (cq ? 0x8000 : 0);
  return skim_js8_pack_compound(SKIM_JS8_HEARTBEAT, call, number, cq ? cq_index : 0, raw);
}

gboolean skim_js8_pack_directed(const char *from, const char *to, int command, int number,
                                char raw[13]) {
  gboolean pf, pt;
  const long f = pack_call28(from, &pf), t = pack_call28(to, &pt);
  if (f < 0 || t < 0) { return FALSE; }
  U72 v = (U72)SKIM_JS8_DIRECTED;
  v = (v << 28) | (U72)f;
  v = (v << 28) | (U72)t;
  v = (v << 5) | (U72)(command & 31);
  v = (v << 8) | (U72)((pf ? 0x80 : 0) | (pt ? 0x40 : 0) | (number & 0x3f));
  pack72(v, raw);
  return TRUE;
}
