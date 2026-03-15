#include "net_monitor.h"
#include "logger.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MOD "NET"

static struct {
    StreamConfig    cfg;
    pthread_t       thread;
    int             running;
    int             is_up;
} g_net;

/* ping -c1 -W<timeout_s> -I<iface> <ip>
 * Returns exit code 0 = reachable */
static int ping_once(const char *iface, const char *ip, int timeout_ms)
{
    char cmd[256];
    int timeout_s = (timeout_ms + 999) / 1000;
    if (timeout_s < 1) timeout_s = 1;
    snprintf(cmd, sizeof(cmd),
             "ping -q -c1 -W%d -I%s %s > /dev/null 2>&1",
             timeout_s, iface, ip);
    return system(cmd) == 0;
}

static const char *resolve_peer_ip(const StreamConfig *cfg)
{
    if (cfg->peer_ip[0])
        return cfg->peer_ip;
    return (cfg->role == ROLE_SENDER)
           ? cfg->profile.peer_ip_host
           : cfg->profile.peer_ip_client;
}

static void *net_thread(void *arg)
{
    (void)arg;
    const char *peer = resolve_peer_ip(&g_net.cfg);
    const DeviceProfile *p = &g_net.cfg.profile;

    LOG_INFO(MOD, "Network monitor started — checking %s via %s",
             peer, p->iface);

    while (g_net.running) {
        int reachable = 0;
        
        for (int i = 0; i < p->net_retries && g_net.running; i++) {
            if (ping_once(p->iface, peer, p->net_timeout_ms)) {
                reachable = 1;
                break;
            }
            if (!g_net.running) break;
            /* Sleep a bit between retries using usleep in chunks so we can terminate quickly */
            for (int k=0; k<5 && g_net.running; k++) usleep(100000);
        }

        if (!g_net.running) break;

        if (reachable && !g_net.is_up) {
            LOG_INFO(MOD, "Peer %s is UP", peer);
            g_net.is_up = 1;
            event_bus_publish(SYS_EVT_NET_UP);
        } else if (!reachable && g_net.is_up) {
            LOG_WARN(MOD, "Peer %s went DOWN", peer);
            g_net.is_up = 0;
            event_bus_publish(SYS_EVT_NET_DOWN);
        }

        /* Check periodically, sleep for 1 second in chunks */
        for (int k=0; k<10 && g_net.running; k++) usleep(100000);
    }

    LOG_INFO(MOD, "Network monitor thread exiting");
    return NULL;
}

int net_monitor_init(const StreamConfig *cfg)
{
    if (!cfg) return -1;
    g_net.cfg = *cfg;
    g_net.running = 1;
    g_net.is_up = 0;
    
    pthread_create(&g_net.thread, NULL, net_thread, NULL);
    return 0;
}

void net_monitor_deinit(void)
{
    g_net.running = 0;
    pthread_join(g_net.thread, NULL);
}
