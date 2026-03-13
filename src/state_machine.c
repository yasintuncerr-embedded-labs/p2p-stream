#include "state_machine.h"
#include "logger.h"
#include "net_monitor.h"
#include "pipeline/pipeline.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define MOD              "SM"
#define EVENT_QUEUE_SIZE  16
#define MAX_RETRIES        5
#define BACKOFF_BASE_S     2    /* 2, 4, 8, 16, 32 seconds */

/* -----------------------------------------------------------------------
 * Internal
 * --------------------------------------------------------------------- */

struct StreamSM {
    SmState          state;
    StreamConfig     cfg;
    SmStateChangeCb  on_state;
    void            *userdata;

    /* Pipeline */
    PipelineCtx     *pipeline;

    /* Event queue */
    SmEvent          queue[EVENT_QUEUE_SIZE];
    int              q_head, q_tail, q_count;
    pthread_mutex_t  q_lock;
    pthread_cond_t   q_cond;

    /* SM thread */
    pthread_t        thread;
    int              running;

    /* Recovery */
    int              retry_count;
};

static const char *s_state_names[] = {
    "IDLE", "NET_CHECK", "READY", "STREAMING", "ERROR", "RECOVER"
};
static const char *s_event_names[] = {
    "START", "STOP", "NET_OK", "NET_FAIL",
    "STREAM_ERROR", "STREAM_EOS", "RECOVER_OK", "RECOVER_FAIL"
};

const char *sm_state_name(SmState s) { return (s < SM_STATE_COUNT) ? s_state_names[s] : "?"; }
const char *sm_event_name(SmEvent e) { return (e < SM_EVT_COUNT)   ? s_event_names[e] : "?"; }


/* -----------------------------------------------------------------------
 * Pipeline callbacks — post events back into SM
 * --------------------------------------------------------------------- */
static void on_pipe_error(PipelineCtx *ctx, const char *msg, void *ud)
{
    (void)ctx;
    LOG_ERROR(MOD, "Pipeline error: %s", msg);
    sm_post_event((StreamSM *)ud, SM_EVT_STREAM_ERROR);
}
 
static void on_pipe_eos(PipelineCtx *ctx, void *ud)
{
    (void)ctx;
    LOG_INFO(MOD, "Pipeline EOS");
    sm_post_event((StreamSM *)ud, SM_EVT_STREAM_EOS);
}
 
static void on_pipe_stats(PipelineCtx *ctx, guint64 bytes_in,
                           guint64 bytes_out, gdouble kbps, void *ud)
{
    (void)ctx; (void)bytes_in; (void)bytes_out; (void)ud;
    if (kbps > 0)
        LOG_DEBUG(MOD, "Stream: %.1f kbps", kbps);
}
 

/* -----------------------------------------------------------------------
 * Peer IP helper
 *   SENDER   → peer is the receiver (AP side) → use peer_ip_host
 *   RECEIVER → peer is the sender  (STA side) → use peer_ip_client
 * --------------------------------------------------------------------- */
static const char *resolve_peer_ip(const StreamSM *sm)
{
    if (sm->cfg.peer_ip[0])
        return sm->cfg.peer_ip;     /* explicit override */
 
    return (sm->cfg.role == ROLE_SENDER)
           ? sm->cfg.profile.peer_ip_host
           : sm->cfg.profile.peer_ip_client;
}

/* -----------------------------------------------------------------------
 * State actions
 * --------------------------------------------------------------------- */
static void enter_net_check(StreamSM *sm)
{
    const char *peer = resolve_peer_ip(sm);
    LOG_INFO(MOD, "Checking network — peer=%s iface=%s",
             peer, sm->cfg.profile.iface);
 
    int ok = net_check_peer(&sm->cfg.profile, peer, 3, 1000);
    sm_post_event(sm, ok ? SM_EVT_NET_OK : SM_EVT_NET_FAIL);
}
 

static void enter_ready(StreamSM *sm)
{
    LOG_INFO(MOD, "Network OK — creating pipeline (role=%s)",
             sm->cfg.role == ROLE_SENDER ? "sender" : "receiver");
 
    if (sm->pipeline) {
        pipeline_destroy(sm->pipeline);
        sm->pipeline = NULL;
    }
 
    sm->pipeline = pipeline_create(&sm->cfg,
                                    on_pipe_error, on_pipe_eos,
                                    on_pipe_stats, sm);
    if (!sm->pipeline) {
        LOG_ERROR(MOD, "Pipeline creation failed");
        sm_post_event(sm, SM_EVT_STREAM_ERROR);
        return;
    }
 
    sm->retry_count = 0;
    sm_post_event(sm, SM_EVT_START);
}

static void enter_streaming(StreamSM *sm)
{
    if (!sm->pipeline) { sm_post_event(sm, SM_EVT_STREAM_ERROR); return; }
    if (pipeline_start(sm->pipeline) != 0) {
        sm_post_event(sm, SM_EVT_STREAM_ERROR);
        return;
    }
    LOG_INFO(MOD, "▶ Streaming started (%s → %s)",
             sm->cfg.role == ROLE_SENDER ? "camera" : "network",
             sm->cfg.role == ROLE_SENDER ? "network" : "display");
}
 

static void enter_recover(StreamSM *sm)
{
    sm->retry_count++;
    if (sm->retry_count > MAX_RETRIES) {
        LOG_ERROR(MOD, "Max retries (%d) exhausted — giving up", MAX_RETRIES);
        sm_post_event(sm, SM_EVT_RECOVER_FAIL);
        return;
    }
 
    int delay = BACKOFF_BASE_S * (1 << (sm->retry_count - 1));
    LOG_WARN(MOD, "Recovery attempt %d/%d in %ds...",
             sm->retry_count, MAX_RETRIES, delay);
 
    if (sm->pipeline) {
        pipeline_stop(sm->pipeline);
        pipeline_destroy(sm->pipeline);
        sm->pipeline = NULL;
    }
 
    sleep(delay);
 
    const char *peer = resolve_peer_ip(sm);
    int ok = net_check_peer(&sm->cfg.profile, peer, 3, 1000);
    sm_post_event(sm, ok ? SM_EVT_RECOVER_OK : SM_EVT_RECOVER_FAIL);
}

static void enter_idle(StreamSM *sm, int log_reason)
{
    if (log_reason)
        LOG_INFO(MOD, "Entering IDLE — stopping pipeline");
    if (sm->pipeline) {
        pipeline_stop(sm->pipeline);
        pipeline_destroy(sm->pipeline);
        sm->pipeline = NULL;
    }
    sm->retry_count = 0;
}


/* -----------------------------------------------------------------------
 * Transition table
 * --------------------------------------------------------------------- */
typedef struct { SmState next; void (*action)(StreamSM *sm); } Transition;
 
static void do_net_check  (StreamSM *sm) { enter_net_check(sm); }
static void do_ready      (StreamSM *sm) { enter_ready(sm); }
static void do_streaming  (StreamSM *sm) { enter_streaming(sm); }
static void do_recover    (StreamSM *sm) { enter_recover(sm); }
static void do_idle_stop  (StreamSM *sm) { enter_idle(sm, 1); }
static void do_idle_fail  (StreamSM *sm) { enter_idle(sm, 0); }

/* [current_state][event] → {next_state, action_fn}
 * -1 in next = ignored transition */
static const Transition s_trans[SM_STATE_COUNT][SM_EVT_COUNT] = {
/*               START                              STOP                          NET_OK                        NET_FAIL                       STREAM_ERR                     STREAM_EOS                     RECOVER_OK                    RECOVER_FAIL           */
/* IDLE      */ {{SM_STATE_NET_CHECK, do_net_check},{-1,NULL},                    {-1,NULL},                    {-1,NULL},                     {-1,NULL},                     {-1,NULL},                     {-1,NULL},                    {-1,NULL}             },
/* NET_CHECK */ {{-1,NULL},                         {SM_STATE_IDLE,do_idle_stop}, {SM_STATE_READY,do_ready},    {SM_STATE_IDLE,do_idle_fail},   {-1,NULL},                     {-1,NULL},                     {-1,NULL},                    {-1,NULL}             },
/* READY     */ {{SM_STATE_STREAMING,do_streaming}, {SM_STATE_IDLE,do_idle_stop}, {-1,NULL},                    {SM_STATE_RECOVER,do_recover},  {SM_STATE_RECOVER,do_recover}, {-1,NULL},                     {-1,NULL},                    {-1,NULL}             },
/* STREAMING */ {{-1,NULL},                         {SM_STATE_IDLE,do_idle_stop}, {-1,NULL},                    {SM_STATE_RECOVER,do_recover},  {SM_STATE_RECOVER,do_recover}, {SM_STATE_IDLE,do_idle_stop},  {-1,NULL},                    {-1,NULL}             },
/* ERROR     */ {{-1,NULL},                         {SM_STATE_IDLE,do_idle_stop}, {-1,NULL},                    {-1,NULL},                     {-1,NULL},                     {-1,NULL},                     {-1,NULL},                    {-1,NULL}             },
/* RECOVER   */ {{-1,NULL},                         {SM_STATE_IDLE,do_idle_stop}, {-1,NULL},                    {-1,NULL},                     {-1,NULL},                     {-1,NULL},                     {SM_STATE_READY,do_ready},    {SM_STATE_IDLE,do_idle_fail}},
};


/* -----------------------------------------------------------------------
 * SM thread
 * --------------------------------------------------------------------- */
static void transition(StreamSM *sm, SmState next, void (*action)(StreamSM *))
{
    SmState old = sm->state;
    sm->state   = next;
    LOG_INFO(MOD, "[ %s ] → [ %s ]", sm_state_name(old), sm_state_name(next));
    if (action) action(sm);
    if (sm->on_state) sm->on_state(old, next, sm->userdata);
}
 
static void *sm_thread(void *arg)
{
    StreamSM *sm = (StreamSM *)arg;
 
    LOG_INFO(MOD, "SM thread started — net=%s stream=%s trigger=%d",
             sm->cfg.net_role == NET_ROLE_AP  ? "ap"  : "sta",
             sm->cfg.role     == ROLE_SENDER  ? "sender" : "receiver",
             sm->cfg.trigger);
 
    if (sm->cfg.trigger == TRIGGER_AUTO)
        sm_post_event(sm, SM_EVT_START);
 
    while (sm->running) {
        pthread_mutex_lock(&sm->q_lock);
        while (sm->q_count == 0 && sm->running)
            pthread_cond_wait(&sm->q_cond, &sm->q_lock);
 
        if (!sm->running) { pthread_mutex_unlock(&sm->q_lock); break; }
 
        SmEvent evt = sm->queue[sm->q_head];
        sm->q_head  = (sm->q_head + 1) % EVENT_QUEUE_SIZE;
        sm->q_count--;
        pthread_mutex_unlock(&sm->q_lock);
 
        LOG_DEBUG(MOD, "Event: %s (state=%s)",
                  sm_event_name(evt), sm_state_name(sm->state));
 
        const Transition *t = &s_trans[sm->state][evt];
        if (t->next == (SmState)-1) {
            LOG_WARN(MOD, "Ignored event %s in state %s",
                     sm_event_name(evt), sm_state_name(sm->state));
            continue;
        }
        transition(sm, t->next, t->action);
    }
 
    LOG_INFO(MOD, "SM thread exiting");
    return NULL;
}

 
/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
StreamSM *sm_create(const StreamConfig *cfg, SmStateChangeCb on_state, void *userdata)
{
    StreamSM *sm = calloc(1, sizeof(*sm));
    if (!sm) return NULL;
 
    sm->cfg      = *cfg;
    sm->state    = SM_STATE_IDLE;
    sm->on_state = on_state;
    sm->userdata = userdata;
    sm->running  = 1;
 
    pthread_mutex_init(&sm->q_lock, NULL);
    pthread_cond_init (&sm->q_cond, NULL);
    pthread_create(&sm->thread, NULL, sm_thread, sm);
 
    LOG_INFO(MOD, "State machine created (net=%s stream=%s trigger=%d)",
             cfg->net_role == NET_ROLE_AP ? "ap" : "sta",
             cfg->role     == ROLE_SENDER ? "sender" : "receiver",
             cfg->trigger);
    return sm;
}
 
void sm_destroy(StreamSM *sm)
{
    if (!sm) return;
    sm_post_event(sm, SM_EVT_STOP);
 
    pthread_mutex_lock(&sm->q_lock);
    sm->running = 0;
    pthread_cond_signal(&sm->q_cond);
    pthread_mutex_unlock(&sm->q_lock);
    pthread_join(sm->thread, NULL);
 
    if (sm->pipeline) {
        pipeline_destroy(sm->pipeline);
        sm->pipeline = NULL;
    }
    pthread_mutex_destroy(&sm->q_lock);
    pthread_cond_destroy (&sm->q_cond);
    free(sm);
}
 
int sm_post_event(StreamSM *sm, SmEvent evt)
{
    if (!sm) return -1;
    pthread_mutex_lock(&sm->q_lock);
    if (sm->q_count >= EVENT_QUEUE_SIZE) {
        LOG_WARN(MOD, "Event queue full, dropping %s", sm_event_name(evt));
        pthread_mutex_unlock(&sm->q_lock);
        return -1;
    }
    sm->queue[sm->q_tail] = evt;
    sm->q_tail  = (sm->q_tail + 1) % EVENT_QUEUE_SIZE;
    sm->q_count++;
    pthread_cond_signal(&sm->q_cond);
    pthread_mutex_unlock(&sm->q_lock);
    return 0;
}
 
SmState sm_get_state(StreamSM *sm)
{
    if (!sm) return SM_STATE_IDLE;
    return sm->state;
}
