#ifndef P2P_STATE_MACHINE_H
#define P2P_STATE_MACHINE_H

#include "pipeline/pipeline.h"

typedef enum {
    SM_STATE_IDLE = 0,
    SM_STATE_STREAMING,
    SM_STATE_ERROR,
    SM_STATE_COUNT
} SmState;

typedef enum {
    SM_EVT_START = 0,
    SM_EVT_STOP,
    SM_EVT_PIPELINE_FAIL,
    SM_EVT_STREAM_ERROR,
    SM_EVT_STREAM_EOS,
    SM_EVT_COUNT
} SmEvent;

typedef void (*SmStateChangeCb)(SmState old_state, SmState new_state, void *userdata);

typedef struct StreamSM StreamSM;

StreamSM *sm_create(const StreamConfig *cfg, SmStateChangeCb on_state, void *userdata);
void      sm_destroy(StreamSM *sm);
int       sm_post_event(StreamSM *sm, SmEvent evt);
SmState   sm_get_state(StreamSM *sm);
const char *sm_state_name(SmState s);

#endif /* P2P_STATE_MACHINE_H */
