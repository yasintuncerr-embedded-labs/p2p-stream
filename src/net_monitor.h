#ifndef P2P_NET_MONITOR_H
#define P2P_NET_MONITOR_H

#include "pipeline/profile.h"

/* Ping peer IP, returns 1 (reachable) or 0 (unreachable).
 * retries: number of attempts before giving up.
 * timeout_ms: per-attempt timeout. */
int net_check_peer(const DeviceProfile *p, const char *peer_ip,
                   int retries, int timeout_ms);

#endif
