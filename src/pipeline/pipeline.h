#ifndef P2P_PIPELINE_H
#define P2P_PIPELINE_H

#include <gst/gst.h>
#include "profile.h"


/* -----------------------------------------------------------------------
 * Stream configuration — runtime parameters merged with profile
 * --------------------------------------------------------------------- */
typedef enum {
    ROLE_HOST   = 0,
    ROLE_CLIENT = 1
} StreamRole;

typedef enum {
    TRIGGER_AUTO   = 0,   /* start as soon as network is up         */
    TRIGGER_MANUAL = 1,   /* wait for explicit cmd                  */
    TRIGGER_GPIO   = 2    /* wait for GPIO rising edge (host only)  */
} TriggerMode;


typedef struct {
    StreamRole  role;
    CodecType   codec;
    SinkType    sink;
    TriggerMode trigger;

    /* Video */
    int width;
    int height;
    int fps;
    int bitrate_bps;        /* Always stored as bps, converted on use   */

    /* Source override */
    int use_test_pattern;   /* 1 = videotestsrc instead of camera       */

    /* Sink-specific */
    char file_path[256];    /* for SINK_FILE                            */
    int rtsp_port;          /* for SINK_RTSP                            */

    /* Resolved peer IP (set by net_mınitor)*/
    char peer_ip[64];

    /* Profile (loaded by profile_load)*/
    DeviceProfile profile;
}StreamConfig;

/* -----------------------------------------------------------------------
 * Pipeline handle — opaque to callers
 * --------------------------------------------------------------------- */
typedef struct PipelineCtx PipelineCtx;

/* Callbacks fired from the GStreamer bus watch thread */
typedef void (*PipelineErrorCb)(PipelineCtx *ctx, const char *msg, void *userdata);
typedef void (*PipelineEosCb)  (PipelineCtx *ctx, void *userdata);
typedef void (*PipelineStatsCb)(PipelineCtx *ctx,
                                guint64 bytes_in, guint64 bytes_out,
                                gdouble bitrate_kbps, void *userdata);


/* -----------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------- */
/* Create pipeline (does not start it yet).  Returns NULL on error. */
PipelineCtx *pipeline_create (const StreamConfig *cfg,
                               PipelineErrorCb on_error,
                               PipelineEosCb   on_eos,
                               PipelineStatsCb on_stats,
                               void           *userdata);

/* Start / pause / stop */
int  pipeline_start  (PipelineCtx *ctx);
int  pipeline_pause  (PipelineCtx *ctx);
int  pipeline_stop   (PipelineCtx *ctx);

/* Dynamic bitrate update (best-effort, VPU may ignore) */
int  pipeline_set_bitrate (PipelineCtx *ctx, int bitrate_bps);

/* Destroy and free */
void pipeline_destroy (PipelineCtx *ctx);

/* Describe the pipeline as a string (for logging) */
const char *pipeline_describe (PipelineCtx *ctx);

#endif /* P2P_PIPELINE_H */
