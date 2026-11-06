/* oracle.h — measurement only: tell the decoder where R heard a station.
 *
 * Step 0.1 of docs/WEAK-SIGNAL-PLAN.md. SKIM_ORACLE=<file> (the output of
 * skimmer-compare/oracle-truth.py: "t0_s t1_s carrier_hz wpm call …" in
 * stream seconds) and SKIM_ORACLE_MODE=gate,clock,freq,ghost,gatesoft hand the CW v2
 * decoder and the ghost arbitration what CW Skimmer Server (R) heard:
 *
 *   gate   the squelch and the keying duty are held open on the channel
 *          that owns R's station while R heard it,
 *   clock  the dit comes from R's WPM instead of the bootstrap, and a clock
 *          more than 25 % off it is reset,
 *   freq   the tone offset is the carrier measured in the IQ, not tracked,
 *   ghost  a stronger neighbour does not suppress that channel's decodes,
 *   gatesoft  the squelch and keying-duty bars are halved there (2.0×,
 *          0.07) instead of held open — the gate still shuts on the gaps.
 *
 * How much more of R's stations L reads with each is how much each stage
 * costs today. Unset, nothing changes: skim_oracle_modes() is 0.
 * SKIM_ORACLE_MINPROM (dB, default 6) skips carriers the truth file could
 * not find clearly. Stream time is the replay's plus SKIM_REPLAY_FROM.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#ifndef SKIMMER_ORACLE_H
#define SKIMMER_ORACLE_H

#include <glib.h>

G_BEGIN_DECLS

enum {
  SKIM_ORACLE_GATE  = 1 << 0,
  SKIM_ORACLE_CLOCK = 1 << 1,
  SKIM_ORACLE_FREQ  = 1 << 2,
  SKIM_ORACLE_GHOST = 1 << 3,
  SKIM_ORACLE_GSOFT = 1 << 4,
};

typedef struct {
  double carrier_hz;   /* absolute                                          */
  double wpm;          /* R's                                               */
} SkimOracleHit;

/* The modes switched on (0 = oracle off). Loads the file on first call. */
guint    skim_oracle_modes(void);

/* Stream seconds of the replay at stream time t_s of the pipeline. */
double   skim_oracle_time(double t_s);

/* The station R heard at time t_s whose carrier lies within half_hz of
 * chan_hz (the nearest one if several); FALSE when none. */
gboolean skim_oracle_at(double chan_hz, double half_hz, double t_s,
                        SkimOracleHit *hit);

G_END_DECLS

#endif
