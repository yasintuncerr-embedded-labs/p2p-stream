#include "event_bus.h"
#include "logger.h"

#include <pthread.h>
#include <stdlib.h>

#define MOD "EBUS"
#define BUS_QUEUE_SIZE 32
#define MAX_SUBSCRIBERS 8

typedef struct {
    SysEventCb cb;
    void *userdata;
} Subscriber;

static struct {
    SysEvent        queue[BUS_QUEUE_SIZE];
    int             q_head, q_tail, q_count;
    pthread_mutex_t q_lock;
    pthread_cond_t  q_cond;

    Subscriber      subs[MAX_SUBSCRIBERS];
    int             num_subs;
    pthread_mutex_t sub_lock;

    pthread_t       thread;
    int             running;
} g_bus;

static const char *evt_name(SysEvent e) {
    switch(e) {
        case SYS_EVT_NET_UP: return "NET_UP";
        case SYS_EVT_NET_DOWN: return "NET_DOWN";
        case SYS_EVT_CMD_START: return "CMD_START";
        case SYS_EVT_CMD_STOP: return "CMD_STOP";
        case SYS_EVT_STREAM_STARTED: return "STREAM_STARTED";
        case SYS_EVT_STREAM_STOPPED: return "STREAM_STOPPED";
        case SYS_EVT_STREAM_ERROR: return "STREAM_ERROR";
        case SYS_EVT_QUIT: return "QUIT";
        default: return "?";
    }
}

static int evt_is_critical(SysEvent e)
{
    return (e == SYS_EVT_QUIT || e == SYS_EVT_CMD_STOP || e == SYS_EVT_NET_DOWN);
}

static int evt_should_dedupe(SysEvent e)
{
    return (e == SYS_EVT_NET_UP || e == SYS_EVT_NET_DOWN ||
            e == SYS_EVT_STREAM_STARTED || e == SYS_EVT_STREAM_STOPPED);
}

static void *bus_thread(void *arg)
{
    (void)arg;
    LOG_INFO(MOD, "Event bus thread started");

    while (g_bus.running) {
        pthread_mutex_lock(&g_bus.q_lock);
        while (g_bus.q_count == 0 && g_bus.running)
            pthread_cond_wait(&g_bus.q_cond, &g_bus.q_lock);

        if (!g_bus.running) {
            pthread_mutex_unlock(&g_bus.q_lock);
            break;
        }

        SysEvent evt = g_bus.queue[g_bus.q_head];
        g_bus.q_head = (g_bus.q_head + 1) % BUS_QUEUE_SIZE;
        g_bus.q_count--;
        pthread_mutex_unlock(&g_bus.q_lock);

        LOG_DEBUG(MOD, "Dispatching: %s", evt_name(evt));
        pthread_mutex_lock(&g_bus.sub_lock);
        for (int i = 0; i < g_bus.num_subs; i++) {
            if (g_bus.subs[i].cb)
                g_bus.subs[i].cb(evt, g_bus.subs[i].userdata);
        }
        pthread_mutex_unlock(&g_bus.sub_lock);
    }
    LOG_INFO(MOD, "Event bus thread exiting");
    return NULL;
}

int event_bus_init(void)
{
    g_bus.running  = 1;
    g_bus.num_subs = 0;
    g_bus.q_head = g_bus.q_tail = g_bus.q_count = 0;

    pthread_mutex_init(&g_bus.q_lock, NULL);
    pthread_mutex_init(&g_bus.sub_lock, NULL);
    pthread_cond_init (&g_bus.q_cond, NULL);
    pthread_create(&g_bus.thread, NULL, bus_thread, NULL);
    return 0;
}

void event_bus_deinit(void)
{
    event_bus_publish(SYS_EVT_QUIT);
    pthread_mutex_lock(&g_bus.q_lock);
    g_bus.running = 0;
    pthread_cond_signal(&g_bus.q_cond);
    pthread_mutex_unlock(&g_bus.q_lock);

    pthread_join(g_bus.thread, NULL);
    pthread_mutex_destroy(&g_bus.q_lock);
    pthread_mutex_destroy(&g_bus.sub_lock);
    pthread_cond_destroy(&g_bus.q_cond);
}

int event_bus_publish(SysEvent evt)
{
    int prev_idx;
    int found = 0;

    if (!g_bus.running && evt != SYS_EVT_QUIT) return -1;
    pthread_mutex_lock(&g_bus.q_lock);

    /* Avoid queue churn for repeated state-like events. */
    if (evt_should_dedupe(evt) && g_bus.q_count > 0) {
        prev_idx = (g_bus.q_tail - 1 + BUS_QUEUE_SIZE) % BUS_QUEUE_SIZE;
        if (g_bus.queue[prev_idx] == evt) {
            pthread_mutex_unlock(&g_bus.q_lock);
            return 0;
        }
    }

    if (g_bus.q_count >= BUS_QUEUE_SIZE) {
        if (!evt_is_critical(evt)) {
            LOG_WARN(MOD, "Queue full, dropping %s", evt_name(evt));
            pthread_mutex_unlock(&g_bus.q_lock);
            return -1;
        }

        /* Preserve critical events by evicting oldest non-critical item. */
        for (int i = 0; i < g_bus.q_count; ++i) {
            int idx = (g_bus.q_head + i) % BUS_QUEUE_SIZE;
            if (!evt_is_critical(g_bus.queue[idx])) {
                found = 1;
                for (int j = i; j < g_bus.q_count - 1; ++j) {
                    int from = (g_bus.q_head + j + 1) % BUS_QUEUE_SIZE;
                    int to = (g_bus.q_head + j) % BUS_QUEUE_SIZE;
                    g_bus.queue[to] = g_bus.queue[from];
                }
                g_bus.q_tail = (g_bus.q_tail - 1 + BUS_QUEUE_SIZE) % BUS_QUEUE_SIZE;
                g_bus.q_count--;
                LOG_WARN(MOD, "Queue full, evicted non-critical event for %s", evt_name(evt));
                break;
            }
        }

        if (!found) {
            g_bus.q_head = (g_bus.q_head + 1) % BUS_QUEUE_SIZE;
            g_bus.q_count--;
            LOG_WARN(MOD, "Queue full, evicted oldest critical event to keep %s", evt_name(evt));
        }
    }

    g_bus.queue[g_bus.q_tail] = evt;
    g_bus.q_tail = (g_bus.q_tail + 1) % BUS_QUEUE_SIZE;
    g_bus.q_count++;
    pthread_cond_signal(&g_bus.q_cond);
    pthread_mutex_unlock(&g_bus.q_lock);
    return 0;
}

int event_bus_subscribe(SysEventCb cb, void *userdata)
{
    if (!cb) return -1;
    int ret = -1;
    pthread_mutex_lock(&g_bus.sub_lock);
    if (g_bus.num_subs < MAX_SUBSCRIBERS) {
        g_bus.subs[g_bus.num_subs].cb = cb;
        g_bus.subs[g_bus.num_subs].userdata = userdata;
        g_bus.num_subs++;
        ret = 0;
    } else {
        LOG_WARN(MOD, "Max subscribers reached");
    }
    pthread_mutex_unlock(&g_bus.sub_lock);
    return ret;
}
