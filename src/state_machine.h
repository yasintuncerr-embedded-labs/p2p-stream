#ifndef P2P_STATE_MACHINE_H
#define P2P_STATE_MACHINE_H

#include "pipeline/pipeline.h"


/* -------------------------------------------------------------------
 * Stream-level state machine
 *  
 * IDLE - (trigger) -> NetCheck -> (ok) -> READY -> (stable) -> STREAMING
 *                       |                                     |
 *                       |(fail)                         (error/drops)
 *                       v                                     v
 *                     IDLE <-------- RECOVER <----------------| 
 *
 * Transitions are posted as events; the SM runs in its own thread
 *
 * -------------------------------------------------------------------- */

typedef enum {
    SM_STATE_IDLE       = 0,
    SM_STATE_NET_CHECK,
    SM_STATE_READY,
    SM_STATE_STREAMING,
    SM_STATE_ERROR,
    SM_STATE_RECOVER,
    SM_STATE_COUNT,
} SmState;
 
typedef enum {
    SM_EVT_START        = 0,    /* trigger: start stream            */
    SM_EVT_STOP,                /* explicit stop                    */
    SM_EVT_NET_OK,              /* network check passed             */
    SM_EVT_NET_FAIL,            /* network check failed             */
    SM_EVT_STREAM_ERROR,        /* GST error from pipeline          */
    SM_EVT_STREAM_EOS,          /* EOS (file sink done, etc.)       */
    SM_EVT_RECOVER_OK,          /* recovery succeeded               */
    SM_EVT_RECOVER_FAIL,        /* recovery exhausted retries       */
    SM_EVT_COUNT                /* sentinel — keep last             */
} SmEvent;
 

typedef struct StreamSM StreamSM;
 
/* Callback: fired on every state change */
typedef void (*SmStateChangeCb)(SmState old_state, SmState new_state, void *userdata);
 
/* Create / destroy */
StreamSM *sm_create (const StreamConfig *cfg, SmStateChangeCb on_state, void *userdata);
void      sm_destroy(StreamSM *sm);
 
/* Post an event (thread-safe, non-blocking) */
int sm_post_event(StreamSM *sm, SmEvent evt);
 
/* Query current state (thread-safe) */
SmState sm_get_state(StreamSM *sm);
 
/* Human-readable names */
const char *sm_state_name(SmState s);
const char *sm_event_name(SmEvent e);
 

#endif /* P2P_STATE_MACHINE_H */
