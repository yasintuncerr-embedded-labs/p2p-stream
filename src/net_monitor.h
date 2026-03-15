#ifndef P2P_NET_MONITOR_H
#define P2P_NET_MONITOR_H

#include "pipeline/pipeline.h"

int  net_monitor_init(const StreamConfig *cfg);
void net_monitor_deinit(void);

#endif /* P2P_NET_MONITOR_H */
