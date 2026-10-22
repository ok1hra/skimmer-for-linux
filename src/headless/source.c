/* source.c — see source.h. Part of skimmer-for-linux. GPL-3.0-or-later. */
#include "source.h"

SkimIqSource *skim_iq_source_new(const SkimHdConfig *cfg, gboolean take_over,
                                 SkimIqSourceIqCb iq_cb, gpointer user) {
  SkimIqSource *s = cfg->source == SKIM_HD_SOURCE_MOCK
                        ? skim_iq_source_mock_new(cfg)
                        : skim_iq_source_hpsdr_p1_new(cfg, take_over);
  s->cfg = cfg;
  s->iq_cb = iq_cb;
  s->iq_user = user;
  return s;
}
