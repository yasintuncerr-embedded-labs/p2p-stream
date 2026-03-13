#ifndef P2P_PIPELINE_H
#define P2P_PIPELINE_H

#include <gst/gst.h>
#include "profile.h"


/* -----------------------------------------------------------------------
 * Network role * who owns the Wi-Fi link
 * 
 *      NET_ROLE_AP :   Creates the Wi-Fi Direct / hotspot link
 *                      This is the display-side device (receiver)
 *                      Starts p2p-init, waits for peer to associate
 *
 *      NET_ROLE_STA:   Joins the Wi-Fi link created by the AP.
 *                      This is the camera-side device (sender).
 *                      Waits for link. then connects and starts streaming.
 *
 * Network role is independent of stream direction.
 * ----------------------------------------------------------------------- */
 typedef enum {
    NET_ROLE_AP     = 0,
    NET_ROLE_STA    = 1
 } NetworkRole;



 /* ------------------------------------------------------------------------------
  * Stream role - who sends / receives the video
  *
  *     ROLE_SENDER :   Captures from camera. encodes, sends UDP.
  *                     Runs on the camera device (NET_ROLE_STA in typical setup)
  * 
  *     ROLE_RECEIVER:  Receives UDP, decodes outputs to display.
  *                     Runs on the display device (NET_ROLE_AP in typical setup)
  *
  * Keeping these separate allows future bidirectional or role-swap test
  * ----------------------------------------------------------------------------- */
  typedef enum {
    ROLE_SENDER     = 0,
    ROLE_RECEIVER   = 1,
  }StreamRole;


  /* -----------------------------------------------------------------------
   * Trigger mode - when to start the stream
   * ----------------------------------------------------------------------- */
  typedef enum {
    TRIGGER_AUTO    = 0,  /* start as soon as network is up         */
    TRIGGER_MANUAL  = 1,  /* wait for explicit cmd via FIFO         */
    TRIGGER_GPIO    = 2,  /* wait for GPIO rising edge (sender only)  */
  }TriggerMode;


/* -----------------------------------------------------------------------
 * Stream configuration - runtime parameters merged with profile
 * ----------------------------------------------------------------------- */
typedef struct {
    NetworkRole net_role;
    StreamRole role;
    CodecType codec;
    SinkType sink;
    TriggerMode trigger;

    /* Video */
    int width;
    int height;
    int fps;
    int bitrate_bps;        /* Always stored as bps. converted on use */

    /* Source override */
    int use_test_pattern;   /* 1 - videotestsrc instead of camera */

    /* Sink-specific */
    char file_path[256];    /* for SINK_FILE                */
    int rtsp_port;          /* for SINK_RTSP                */

    /* Resolved peer IP (set by net_monitor) */
    char peer_ip[64];

    /* Profile (loaded by profile_load) */
    DeviceProfile profile;

}StreamConfig;


/* ----------------------------------------------------------------------- 
 * Pipeline handle - opaque to callers
 * ----------------------------------------------------------------------- */
 typedef struct PipelineCtx PipelineCtx;

 /* Callbacks fired from the Gstreamer bus watch thread */
 typedef void (*PipelineErrorCb)(PipelineCtx *ctx, const char *msg, void *userdata);
 typedef void (*PipelineEosCb)  (PipelineCtx *ctx, void *userdata);
 typedef void (*PipelineStatsCb)(PipelineCtx *ctx, 
                                    guint64 bytes_in, guint64 bytes_out,
                                    gdouble bitrate_kbps, void *userdata);

/* ----------------------------------------------------------------------- 
 * API
 * ----------------------------------------------------------------------- */
PipelineCtx *pipeline_create(const StreamConfig *cfg,
                                PipelineErrorCb on_error,
                                PipelineEosCb   on_eos,
                                PipelineStatsCb on_stats,
                                void            *userdata);

int pipeline_start(PipelineCtx *ctx);
int pipeline_status(PipelineCtx *ctx);
int pipeline_stop(PipelineCtx *ctx);
int pipeline_set_bitrate(PipelineCtx *ctx, int bitrate_bps);
void pipeline_destroy(PipelineCtx *ctx);
const char *pipeline_describe (PipelineCtx *ctx);

#endif /* P2P_PIPELINE_H */
