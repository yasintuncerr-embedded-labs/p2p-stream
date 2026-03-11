#include "net_monitor.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD "NET"

/* ping -c1 -W<timeout_s> -I<iface> <ip>
 * Returns exit code 0 = reachable */
static int ping_once(const char *iface, const char *ip, int timeout_ms)
{
    char cmd[256];
    /* -q: quiet, -c1: one packet, -W: wait seconds (round up) */
    int timeout_s = (timeout_ms + 999) / 1000;
    if (timeout_s < 1) timeout_s = 1;
    snprintf(cmd, sizeof(cmd),
             "ping -q -c1 -W%d -I%s %s > /dev/null 2>&1",
             timeout_s, iface, ip);
    return system(cmd) == 0;
}

int net_check_peer(const DeviceProfile *p, const char *peer_ip,
                   int retries, int timeout_ms)
{
    LOG_DEBUG(MOD, "Checking peer %s via %s (%d retries, %dms)",
              peer_ip, p->iface, retries, timeout_ms);

    for (int i = 0; i < retries; i++) {
        if (ping_once(p->iface, peer_ip, timeout_ms)) {
            LOG_INFO(MOD, "Peer %s reachable (attempt %d)", peer_ip, i + 1);
            return 1;
        }
        LOG_WARN(MOD, "Ping attempt %d/%d failed for %s", i + 1, retries, peer_ip);
        if (i < retries - 1) usleep(500000); /* 500ms between retries */
    }
    LOG_ERROR(MOD, "Peer %s unreachable after %d attempts", peer_ip, retries);
    return 0;
}
