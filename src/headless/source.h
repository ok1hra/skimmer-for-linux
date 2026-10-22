/* source.h — where skimmer-headless's IQ comes from.
 *
 * A source delivers one IQ stream per band, in TRUE spectrum orientation
 * (whatever the wire carries — an RF-inverted HPSDR feed is conjugated
 * inside the source). How it gets there is the source's business: hardware
 * DDCs, a software DDC off one wideband stream, a TCI receiver. The owner
 * sees only bands, blocks and a state.
 *
 * Threading: the iq callback fires on the source's own thread(s); tick(),
 * status() and free() are called from the owner's main loop only.
 *
 * A source never stops the owner. When it gives up for good (no radio free,
 * the search timed out) it says so in status() (gave_up) and the owner
 * decides — skimmer-headless exits with code 3 and its supervisor retries.
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

#include "config.h"

G_BEGIN_DECLS

typedef struct _SkimIqSource SkimIqSource;

/* One IQ block of band (index into the config's bands). */
typedef void (*SkimIqSourceIqCb)(guint band, const float *iq, guint nframes,
                                 double rate, double centre_hz, gpointer user);

typedef struct {
  gboolean streaming;
  gboolean gave_up;              /* no radio: the owner should stop          */
  char     state[160];           /* one line for the console and the web    */
  char     host[64];             /* the radio streaming now (or the first)  */
  double   ppm;                  /* … its clock correction                  */
  guint64  packets;              /* since start (rate: the owner's delta)   */
  guint64  lost_frames;          /* frames the wire lost, zero-filled       */
} SkimIqSourceStatus;

typedef struct {
  void (*start)(SkimIqSource *s);          /* begin (first search now)       */
  void (*tick)(SkimIqSource *s);           /* 1 Hz supervision               */
  void (*status)(SkimIqSource *s, SkimIqSourceStatus *out);
  void (*free)(SkimIqSource *s);           /* stops a stream that is ours    */
} SkimIqSourceVtable;

struct _SkimIqSource {
  const SkimIqSourceVtable *vt;
  const SkimHdConfig       *cfg;           /* borrowed, outlives the source  */
  SkimIqSourceIqCb          iq_cb;
  gpointer                  iq_user;
};

/* The source [radio] type names; take_over: take the first busy radio when
 * none is idle, on the first start only. */
SkimIqSource *skim_iq_source_new(const SkimHdConfig *cfg, gboolean take_over,
                                 SkimIqSourceIqCb iq_cb, gpointer user);

static inline void skim_iq_source_start(SkimIqSource *s) { s->vt->start(s); }
static inline void skim_iq_source_tick(SkimIqSource *s) { s->vt->tick(s); }
static inline void skim_iq_source_status(SkimIqSource *s, SkimIqSourceStatus *o) {
  s->vt->status(s, o);
}
static inline void skim_iq_source_free(SkimIqSource *s) { if (s) { s->vt->free(s); } }

/* The implementations (source_hpsdr_p1.c, source_mock.c). */
SkimIqSource *skim_iq_source_hpsdr_p1_new(const SkimHdConfig *cfg, gboolean take_over);

/* The Red Pitayas on the LAN (a 1 s broadcast discovery — what host=auto
 * tries): SkimHpsdrFound, sorted by address; empty when none answered.
 * SKIM_HPSDR_DISCOVER=addr[:port] aims it elsewhere (a gate's fake radio). */
GArray *skim_iq_source_hpsdr_discover(void);
SkimIqSource *skim_iq_source_mock_new(const SkimHdConfig *cfg);

G_END_DECLS
