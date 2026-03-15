#ifndef P2P_EVENT_BUS_H
#define P2P_EVENT_BUS_H

typedef enum {
    SYS_EVT_NONE = 0,
    SYS_EVT_NET_UP,      /* Network is up/reachable */
    SYS_EVT_NET_DOWN,    /* Network is down */
    SYS_EVT_CMD_START,   /* Command: start streaming */
    SYS_EVT_CMD_STOP,    /* Command: stop streaming */
    SYS_EVT_STREAM_STARTED,
    SYS_EVT_STREAM_STOPPED,
    SYS_EVT_STREAM_ERROR,
    SYS_EVT_QUIT         /* Application exit */
} SysEvent;

typedef void (*SysEventCb)(SysEvent evt, void *userdata);

int  event_bus_init(void);
void event_bus_deinit(void);

int  event_bus_publish(SysEvent evt);
int  event_bus_subscribe(SysEventCb cb, void *userdata);

#endif /* P2P_EVENT_BUS_H */
