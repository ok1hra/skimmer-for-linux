/* discovery.h — skimmer-headless instances finding each other on the LAN.
 *
 * Every instance answers a UDP broadcast on SKIM_HD_DISCOVERY_PORT: the
 * request is the datagram "SKIMMER? 1", the answer "SKIMMER 1 {json}" with
 * what the owner says about itself (name, version, call, role, ports,
 * bands — headless_main.c). The sockets set SO_REUSEADDR, so several
 * instances on one machine all hear a broadcast.
 *
 * $SKIM_DISCOVERY_PORT moves both sides elsewhere (the gates, so they never
 * meet the real instances on the LAN).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define SKIM_HD_DISCOVERY_PORT 8075

typedef struct _SkimHdDiscovery SkimHdDiscovery;

/* The owner's JSON object (newly allocated), asked for at each request. */
typedef char *(*SkimHdDiscoveryInfo)(gpointer user);

/* Answer requests on the default main context. NULL + error when the port
 * cannot be bound. */
SkimHdDiscovery *skim_hd_discovery_new(SkimHdDiscoveryInfo info, gpointer user,
                                       GError **error);
void             skim_hd_discovery_free(SkimHdDiscovery *d);

/* Broadcast a request and collect the answers for timeout_ms (blocks): a
 * JSON array of the answers, each object with "addr" (the sender) put in
 * front. "[]" when nobody answered. */
char *skim_hd_discovery_find(guint timeout_ms);

/* The port in use ($SKIM_DISCOVERY_PORT or SKIM_HD_DISCOVERY_PORT). */
guint16 skim_hd_discovery_port(void);

G_END_DECLS
