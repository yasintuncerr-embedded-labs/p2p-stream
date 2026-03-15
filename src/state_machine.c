#include "state_machine.h"
#include "logger.h"
#include "event_bus.h"
#include "pipeline/pipeline.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define MOD              "SM"
#define EVENT_QUEUE_SIZE  16

struct StreamSM {
    SmState          state;
    StreamConfig     cfg;
    SmStateChangeCb  on_state;
    void            *userdata;

    PipelineCtx     *pipeline;

    SmEvent          queue[EVENT_QUEUE_SIZE];
    int              q_head, q_tail, q_count;
    pthread_mutex_t  q_lock;
    pthread_cond_t   q_cond;

    pthread_t        thread;
    int              running;
};

static const char *s_state_names[] = {
    "IDLE", "STREAMING", "ERROR"
};
static const char *s_event_names[] = {
    "START", "STOP", "PIPELINE_FAIL", "STREAM_ERROR", "STREAM_EOS"
};

const char *sm_state_name(SmState s) { return (s < SM_STATE_COUNT) ? s_state_names[s] : "?"; }
const char *sm_event_name(SmEvent e) { return (e < SM_EVT_COUNT)   ? s_event_names[e] : "?"; }

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

static void stop_pipeline(StreamSM *sm)
{
    if (sm->pipeline) {
        pipeline_stop(sm->pipeline);
        pipeline_destroy(sm->pipeline);
        sm->pipeline = NULL;
    }
}

static void enter_idle(StreamSM *sm)
{
    stop_pipeline(sm);
    event_bus_publish(SYS_EVT_STREAM_STOPPED);
}

static void enter_streaming(StreamSM *sm)
{
    stop_pipeline(sm);

    sm->pipeline = pipeline_create(&sm->cfg, on_pipe_error, on_pipe_eos, on_pipe_stats, sm);
    if (!sm->pipeline) {
        LOG_ERROR(MOD, "Pipeline creation failed");
        sm_post_event(sm, SM_EVT_PIPELINE_FAIL);
        return;
    }

    if (pipeline_start(sm->pipeline) != 0) {
        sm_post_event(sm, SM_EVT_PIPELINE_FAIL);
        return;
    }
    
    LOG_INFO(MOD, "▶ Streaming started (%s → %s)",
             sm->cfg.role == ROLE_SENDER ? "camera" : "network",
             sm->cfg.role == ROLE_SENDER ? "network" : "display");
    
    event_bus_publish(SYS_EVT_STREAM_STARTED);
}

static void enter_error(StreamSM *sm)
{
    stop_pipeline(sm);
    event_bus_publish(SYS_EVT_STREAM_ERROR);
}

typedef struct { SmState next; void (*action)(StreamSM *sm); } Transition;

static void do_idle      (StreamSM *sm) { enter_idle(sm); }
static void do_streaming (StreamSM *sm) { enter_streaming(sm); }
static void do_error     (StreamSM *sm) { enter_error(sm); }

/* [current_state][event] → {next_state, action_fn} */
static const Transition s_trans[SM_STATE_COUNT][SM_EVT_COUNT] = {
/*               START                              STOP                          PIPELINE_FAIL                  STREAM_ERR                     STREAM_EOS          */
/* IDLE      */ {{SM_STATE_STREAMING,do_streaming}, {-1,NULL},                    {-1,NULL},                     {-1,NULL},                     {-1,NULL}             },
/* STREAMING */ {{-1,NULL},                         {SM_STATE_IDLE,do_idle},      {SM_STATE_ERROR,do_error},     {SM_STATE_ERROR,do_error},     {SM_STATE_IDLE,do_idle}},
/* ERROR     */ {{SM_STATE_STREAMING,do_streaming}, {SM_STATE_IDLE,do_idle},      {-1,NULL},                     {-1,NULL},                     {-1,NULL}             },
};

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

    LOG_INFO(MOD, "SM thread started — role=%s",
             sm->cfg.role == ROLE_SENDER ? "sender" : "receiver");

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
