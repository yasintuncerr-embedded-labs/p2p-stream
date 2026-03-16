#include "net_monitor.h"
#include "logger.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <ctype.h>

extern char **environ;

#define MOD "NET"

static struct {
    StreamConfig    cfg;
    pthread_t       thread;
    int             running;
    int             is_up;
} g_net;

static int is_safe_token(const char *s)
{
    if (!s || !s[0]) return 0;
    for (const char *p = s; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' ||
              *p == '.' || *p == ':' || *p == '%')) {
            return 0;
        }
    }
    return 1;
}

/* ping -q -c1 -W<timeout_s> -I<iface> <ip>
 * Returns true when command exits with status 0. */
static int ping_once(const char *iface, const char *ip, int timeout_ms)
{
    pid_t pid;
    int status;
    char timeout_buf[16];
    char *argv[10];
    int timeout_s = (timeout_ms + 999) / 1000;

    if (!is_safe_token(iface) || !is_safe_token(ip)) {
        LOG_ERROR(MOD, "Invalid iface/ip token for ping (iface='%s', ip='%s')", iface ? iface : "", ip ? ip : "");
        return 0;
    }

    if (timeout_s < 1) timeout_s = 1;

    snprintf(timeout_buf, sizeof(timeout_buf), "%d", timeout_s);
    argv[0] = "ping";
    argv[1] = "-q";
    argv[2] = "-c1";
    argv[3] = "-W";
    argv[4] = timeout_buf;
    argv[5] = "-I";
    argv[6] = (char *)iface;
    argv[7] = (char *)ip;
    argv[8] = NULL;

    if (posix_spawnp(&pid, "ping", NULL, NULL, argv, environ) != 0) {
        LOG_WARN(MOD, "Failed to spawn ping process");
        return 0;
    }
    if (waitpid(pid, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
