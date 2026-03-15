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
        for (int i = 0; i < g_bus.num_subs; i++) {
            if (g_bus.subs[i].cb)
                g_bus.subs[i].cb(evt, g_bus.subs[i].userdata);
        }
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
    pthread_cond_destroy(&g_bus.q_cond);
}

int event_bus_publish(SysEvent evt)
{
    if (!g_bus.running && evt != SYS_EVT_QUIT) return -1;
    pthread_mutex_lock(&g_bus.q_lock);
    if (g_bus.q_count >= BUS_QUEUE_SIZE) {
        LOG_WARN(MOD, "Queue full, dropping %s", evt_name(evt));
        pthread_mutex_unlock(&g_bus.q_lock);
        return -1;
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
    pthread_mutex_lock(&g_bus.q_lock);
    if (g_bus.num_subs < MAX_SUBSCRIBERS) {
        g_bus.subs[g_bus.num_subs].cb = cb;
        g_bus.subs[g_bus.num_subs].userdata = userdata;
        g_bus.num_subs++;
        ret = 0;
    } else {
        LOG_WARN(MOD, "Max subscribers reached");
    }
    pthread_mutex_unlock(&g_bus.q_lock);
    return ret;
}
