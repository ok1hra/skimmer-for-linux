/* visitors.h — who watches the web page from outside the LAN.
 *
 * A visitor from an address outside [visitors] free_from (the private
 * ranges by default), or through a proxy, signs in with a callsign before
 * the page gets any data (web.c). The call is checked here — 3 to 15
 * characters of A–Z, 0–9 and /, at least one digit and one letter — and
 * comes back as a cookie: CALL.ISSUED.HMAC, signed with a key kept in the
 * state directory (visitor.key, made once), valid 30 days. Nothing about a
 * visitor is kept in memory but its open visit, so a restart or a new build
 * does not sign anyone out.
 *
 * The log, two tab-separated files in the state directory:
 *   visitors.tsv         a line a visit: start, end (UTC), seconds, call,
 *                        address, forwarded-for (a proxy's word, unchecked),
 *                        what it watched on the waterfall for 10 s or more —
 *                        "wf:20m@14.026=35" (the window's centre, MHz, to
 *                        1 kHz; seconds) and "audio:20m@14.0253=12" (the
 *                        tuned frequency, to 100 Hz), the longest first
 *   visitors-failed.tsv  a line a refused sign-in: time, address,
 *                        forwarded-for, what was typed (escaped, 32 chars at
 *                        most), why — at most 30 lines a minute
 * A visit ends 10 minutes after its last request (the page asks every 2 s
 * while open), or when the skimmer stops; each file rolls over to .1 at 5 MB.
 *
 * Main loop only. Time comes in from outside (unix s) — the gate drives it.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_VIS_COOKIE      "skv"
#define SKIM_VIS_MAX_AGE_S   (30 * 86400)
#define SKIM_VIS_IDLE_S      600        /* a visit ends this long after its last request */
#define SKIM_VIS_DWELL_S     10         /* a frequency watched this long is logged       */
#define SKIM_VIS_FAILS_MIN   30         /* refused sign-ins logged a minute at most      */
#define SKIM_VIS_ROLL_BYTES  (5 * 1024 * 1024)
#define SKIM_VIS_CALL_MAX    15

typedef struct _SkimHdVisitors SkimHdVisitors;

/* dir: where the key and the logs live (created). NULL + error when the key
 * can be neither read nor made. */
SkimHdVisitors *skim_hd_visitors_new(const char *dir, GError **error);
/* Ends every open visit (logged) and frees. */
void            skim_hd_visitors_free(SkimHdVisitors *v);

/* Is `in` a callsign? TRUE and its canonical form (capitals, no spaces) in
 * out[SKIM_VIS_CALL_MAX + 1]; FALSE and *why (a static English sentence). */
gboolean skim_hd_visitor_call_ok(const char *in, char *out, const char **why);

/* The cookie value for call, signed at now. */
char    *skim_hd_visitors_token(SkimHdVisitors *v, const char *call, gint64 now);
/* A cookie value of ours, not expired: TRUE and its call in
 * call[SKIM_VIS_CALL_MAX + 1]. */
gboolean skim_hd_visitors_check(SkimHdVisitors *v, const char *token, gint64 now, char *call);

/* A request from a signed-in visitor (token checked): opens its visit or
 * keeps it open. addr: the connection's address; fwd: what a proxy said
 * (X-Forwarded-For's last entry or X-Real-IP; NULL: none). */
void skim_hd_visitors_seen(SkimHdVisitors *v, const char *token, const char *call,
                           const char *addr, const char *fwd, gint64 now);
/* Ends the visit of token now (signed out). */
void skim_hd_visitors_leave(SkimHdVisitors *v, const char *token, gint64 now);

/* The waterfall: viewer `conn` of the visit `token` watches band from lo to
 * hi (Hz), its audio tuned to af (Hz) while audio; band NULL = it left. */
void skim_hd_visitors_view(SkimHdVisitors *v, const char *token, gconstpointer conn,
                           const char *band, double lo, double hi, gboolean audio, double af,
                           gint64 now);

/* A refused sign-in. */
void skim_hd_visitors_failed(SkimHdVisitors *v, gint64 now, const char *addr, const char *fwd,
                             const char *typed, const char *why);

/* Once in a while (the main loop's second): visits idle SKIM_VIS_IDLE_S end. */
void skim_hd_visitors_tick(SkimHdVisitors *v, gint64 now);

/* For the managers' page: {"open":[…],"visits":[…],"failed":[…]} — the open
 * visits and the last n lines of each file, newest first. */
char *skim_hd_visitors_json(SkimHdVisitors *v, guint n, gint64 now);

/* For the visitors from outside (Statistics): {"public":true,"hours":H,
 * "open":[…],"visits":[…]} — the open visits and those that ended in the
 * last `hours`, newest first, each its start, seconds and call only. */
char *skim_hd_visitors_public_json(SkimHdVisitors *v, guint hours, gint64 now);

/* The files (for messages). */
const char *skim_hd_visitors_path(const SkimHdVisitors *v);

G_END_DECLS
