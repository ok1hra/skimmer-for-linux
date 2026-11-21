/* skimsrv.h — CW Skimmer Server's band choice, read from its SkimSrv.ini.
 *
 * Settings → Bands can take the bands SkimSrv uses ("from CW Skimmer"): the
 * rate (Rate=0/1/2 = 48/96/192 kHz), each rate's segment centres
 * (CenterFreqs48/96/192, Hz) and which are on (SegmentSel48/96/192, a 0/1
 * a segment). SkimSrv under wine on this PC keeps them in
 *   <prefix>/drive_c/users/<user>/{Application Data,AppData/Roaming}/
 *     Afreet/Products/SkimSrv/SkimSrv.ini
 * with <prefix> $WINEPREFIX, else ~/.wine; the newest such file wins.
 *
 * Only those keys of [Skimmer] leave (and FreqCalibration, a hint of the
 * radio's clock error), each checked to hold digits, commas, a dot — never
 * [User] (the operator's name, call, QTH) or anything else of the file.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* {"found":true,"path":"…","ini":"[Skimmer]\nRate=1\n…"} or
 * {"found":false,"looked":["…",…]}; home: the home directory, wineprefix:
 * $WINEPREFIX (NULL: none). */
char *skim_hd_skimsrv_json(const char *home, const char *wineprefix);

G_END_DECLS
