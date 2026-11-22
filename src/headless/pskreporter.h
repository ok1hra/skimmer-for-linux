/* pskreporter.h — the FT8/FT4 stations heard, to PSK Reporter
 * (pskreporter.info), as WSJT-X reports them: IPFIX over UDP to
 * report.pskreporter.info:4739, by the rules of https://pskreporter.info/
 * pskdev.html:
 *   - one UDP socket, the same source port for every datagram (the server
 *     keeps the templates by it), a random observation domain a session, the
 *     sequence number counting the reports;
 *   - a datagram at most every 5 min (the timer from the start, a few s of
 *     random added — never on the clock's minute) unless it is full; the two
 *     templates in the first three datagrams, then once an hour;
 *   - the receiver record (its call, locator, the software) and the sender
 *     records of the page's "sNR, iMD … senderLocator" template: call,
 *     frequency (Hz), SNR (dB), iMD (0: none), mode ("FT8"/"FT4"),
 *     informationSource 1 (automatic), the sender's locator ("" unknown),
 *     flowStartSeconds (the cycle's start);
 *   - each call once an hour a band and mode (the page: never more than once
 *     in 5 min, ideally once an hour while nothing changed). A locator heard
 *     from a call (its CQ, a grid in a reply) is kept for its later reports.
 * Reports wait in memory; an unreachable server keeps them for the next
 * datagram (an hour's worth at most).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_PSKR_HOST       "report.pskreporter.info"
#define SKIM_PSKR_PORT       4739
#define SKIM_PSKR_INTERVAL_S 300
#define SKIM_PSKR_REPEAT_S   3600       /* a call again a band and mode       */
#define SKIM_PSKR_MAX_BYTES  1400       /* a datagram: under the Ethernet MTU */

typedef struct {
  char   call[16];
  char   grid[8];                       /* "" = not known                     */
  double freq_hz;
  int    snr;
  char   mode[8];                       /* "FT8", "FT4"                       */
  gint64 t;                             /* when heard, unix s                 */
} SkimPskrReport;

/* One datagram (IPFIX, network byte order): the header (seq, odid, time),
 * the two templates when templates, the receiver record, then the reports
 * that fit in max_bytes from the first; *used = how many. The bytes (owned). */
GBytes *skim_pskr_encode(const char *rcall, const char *rgrid, const char *software,
                         guint32 seq, guint32 odid, guint32 now, gboolean templates,
                         const SkimPskrReport *r, guint n, gsize max_bytes, guint *used);

typedef struct {
  const char *call;                     /* the receiver                       */
  const char *locator;
  const char *software;                 /* "skimmer-for-linux 0.8.9 …"        */
  const char *host;                     /* NULL = SKIM_PSKR_HOST              */
  guint16     port;                     /* 0 = SKIM_PSKR_PORT                 */
  guint       interval_s;               /* 0 = SKIM_PSKR_INTERVAL_S (the gate: 1) */
} SkimPskrConfig;

typedef struct _SkimPskr SkimPskr;

SkimPskr *skim_pskr_new(const SkimPskrConfig *cfg);
void      skim_pskr_free(SkimPskr *p);   /* what waits is sent first, once  */

/* A station heard (any thread): dropped when the call went out on this band
 * and mode within the hour; grid "" or NULL = take the one kept for it. */
void skim_pskr_add(SkimPskr *p, const char *call, const char *grid, double freq_hz, int snr,
                   const char *mode, const char *band, gint64 t);

/* {"on":true,"call":…,"locator":<4>,"sent":N,"datagrams":N,"queued":N,
 *  "last":<unix s>|0,"error":…|null} — any thread. */
char *skim_pskr_status_json(SkimPskr *p);

G_END_DECLS
