#include "pipeline.h"
#include "sender.h"
#include "receiver.h"
#include "../logger.h"

#include <gst/gst.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define MOD              "PIPELINE"
#define STATS_PERIOD_MS  2000

/* -----------------------------------------------------------------------
 * Internal context
 * --------------------------------------------------------------------- */
struct PipelineCtx {
    StreamConfig     cfg;
    char             description[2048];

    GstElement      *pipeline;
    GstBus          *bus;
    guint            bus_watch_id;

    /* Stats */
    guint64          bytes_processed;
    gdouble          bitrate_kbps;
    GTimer          *stats_timer;
    guint            stats_timeout_id;

    /* Callbacks */
    PipelineErrorCb  on_error;
    PipelineEosCb    on_eos;
    PipelineStatsCb  on_stats;
    void            *userdata;

    /* GLib main loop for bus watch */
    GMainLoop       *loop;
    GThread         *loop_thread;
};

/* -----------------------------------------------------------------------
 * GLib main loop thread
 * --------------------------------------------------------------------- */
static gpointer loop_thread_func(gpointer data)
{
    g_main_loop_run((GMainLoop *)data);
    return NULL;
}

/* -----------------------------------------------------------------------
 * Bus message handler
 * --------------------------------------------------------------------- */
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    PipelineCtx *ctx = (PipelineCtx *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            LOG_ERROR(MOD, "GstError: %s | debug: %s",
                      err ? err->message : "?",
                      dbg ? dbg : "none");
            if (ctx->on_error)
                ctx->on_error(ctx, err ? err->message : "GstError", ctx->userdata);
            g_clear_error(&err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar  *dbg = NULL;
            gst_message_parse_warning(msg, &err, &dbg);
            LOG_WARN(MOD, "GstWarning: %s | debug: %s",
                     err ? err->message : "?",
                     dbg ? dbg : "none");
            g_clear_error(&err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_EOS:
            LOG_INFO(MOD, "EOS received");
            if (ctx->on_eos)
                ctx->on_eos(ctx, ctx->userdata);
            break;

        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(ctx->pipeline)) {
                GstState old_s, new_s, pending;
                gst_message_parse_state_changed(msg, &old_s, &new_s, &pending);
                LOG_INFO(MOD, "State: %s -> %s (pending: %s)",
                         gst_element_state_get_name(old_s),
                         gst_element_state_get_name(new_s),
                         gst_element_state_get_name(pending));
            }
            break;
        }
        case GST_MESSAGE_QOS: {
            gboolean live;
            guint64  run_time, stream_time, timestamp, duration;
            gint64   jitter;
            gdouble  proportion;
            gint     quality;
            gst_message_parse_qos(msg, &live, &run_time, &stream_time,
                                  &timestamp, &duration);
            gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);
            LOG_WARN(MOD, "QoS drop: jitter=%.1fms proportion=%.2f quality=%d",
                     jitter / 1000000.0, proportion, quality);
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            const GstStructure *s = gst_message_get_structure(msg);
            if (s)
                LOG_DEBUG(MOD, "Element msg: %s", gst_structure_get_name(s));
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Stats timer callback
 * --------------------------------------------------------------------- */
static gboolean stats_timer_cb(gpointer data)
{
    PipelineCtx *ctx = (PipelineCtx *)data;
    if (!ctx->pipeline) return FALSE;

    GstQuery *q = gst_query_new_position(GST_FORMAT_BYTES);
    guint64   bytes_now = 0;

    if (gst_element_query(ctx->pipeline, q)) {
        gint64 pos = 0;
        gst_query_parse_position(q, NULL, &pos);
        bytes_now = (guint64)pos;
    }
    gst_query_unref(q);

    gdouble elapsed = g_timer_elapsed(ctx->stats_timer, NULL);
    if (elapsed > 0 && bytes_now > ctx->bytes_processed) {
        guint64 delta = bytes_now - ctx->bytes_processed;
        ctx->bitrate_kbps = (gdouble)(delta * 8) / (elapsed * 1000.0);
        ctx->bytes_processed = bytes_now;
        g_timer_reset(ctx->stats_timer);
    }

    if (ctx->on_stats)
        ctx->on_stats(ctx, ctx->bytes_processed, bytes_now,
                      ctx->bitrate_kbps, ctx->userdata);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
PipelineCtx *pipeline_create(const StreamConfig *cfg,
                              PipelineErrorCb on_error,
                              PipelineEosCb   on_eos,
                              PipelineStatsCb on_stats,
                              void            *userdata)
{
    if (!cfg) return NULL;

    const char *desc = (cfg->role == ROLE_SENDER)
                       ? build_sender_pipeline_str(cfg)
                       : build_receiver_pipeline_str(cfg);

    if (!desc) {
        LOG_ERROR(MOD, "Pipeline description is NULL");
        return NULL;
    }

    PipelineCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->cfg      = *cfg;
    ctx->on_error = on_error;
    ctx->on_eos   = on_eos;
    ctx->on_stats = on_stats;
    ctx->userdata = userdata;

    GError *err = NULL;
    ctx->pipeline = gst_parse_launch(desc, &err);
    if (!ctx->pipeline || err) {
        LOG_ERROR(MOD, "gst_parse_launch failed: %s",
                  err ? err->message : "unknown");
        g_clear_error(&err);
        free(ctx);
        return NULL;
    }

    ctx->loop       = g_main_loop_new(NULL, FALSE);
    ctx->bus        = gst_element_get_bus(ctx->pipeline);
    ctx->bus_watch_id = gst_bus_add_watch(ctx->bus, on_bus_message, ctx);
    gst_object_unref(ctx->bus);

    ctx->loop_thread = g_thread_new("gst-bus", loop_thread_func, ctx->loop);

    ctx->stats_timer      = g_timer_new();
    ctx->stats_timeout_id = g_timeout_add(STATS_PERIOD_MS, stats_timer_cb, ctx);

    LOG_INFO(MOD, "Pipeline created [%s] codec=%s sink=%d",
             cfg->role == ROLE_SENDER ? "SENDER" : "RECEIVER",
             cfg->codec == CODEC_H265 ? "H265"   : "H264",
             cfg->sink);
    return ctx;
}

int pipeline_start(PipelineCtx *ctx)
{
    if (!ctx || !ctx->pipeline) return -1;
    GstStateChangeReturn ret = gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(MOD, "Failed to set pipeline to PLAYING");
        return -1;
    }
    LOG_INFO(MOD, "Pipeline PLAYING (async=%s)",
             ret == GST_STATE_CHANGE_ASYNC ? "yes" : "no");
    g_timer_reset(ctx->stats_timer);
    return 0;
}

int pipeline_pause(PipelineCtx *ctx)
{
    if (!ctx || !ctx->pipeline) return -1;
    GstStateChangeReturn ret = gst_element_set_state(ctx->pipeline, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR(MOD, "Failed to PAUSE pipeline");
        return -1;
    }
    LOG_INFO(MOD, "Pipeline PAUSED");
    return 0;
}

int pipeline_stop(PipelineCtx *ctx)
{
    if (!ctx || !ctx->pipeline) return -1;
    gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
    LOG_INFO(MOD, "Pipeline stopped (NULL state)");
    return 0;
}

int pipeline_set_bitrate(PipelineCtx *ctx, int bitrate_bps)
{
    if (!ctx || !ctx->pipeline) return -1;

    const DeviceProfile *p = &ctx->cfg.profile;
    CodecType cod = ctx->cfg.codec;
    int enc_bitrate = p->enc_bitrate_unit_kbps ? (bitrate_bps / 1000) : bitrate_bps;

    GstElement *enc = gst_bin_get_by_name(GST_BIN(ctx->pipeline),
                                           p->enc_element[cod]);
    if (!enc)
        enc = gst_bin_get_by_interface(GST_BIN(ctx->pipeline), GST_TYPE_PRESET);

    if (enc) {
        g_object_set(enc, "bitrate", enc_bitrate, NULL);
        gst_object_unref(enc);
        LOG_INFO(MOD, "Bitrate updated to %d bps", bitrate_bps);
        return 0;
    }
    LOG_WARN(MOD, "Could not find encoder element for bitrate update");
    return -1;
}

void pipeline_destroy(PipelineCtx *ctx)
{
    if (!ctx) return;

    if (ctx->stats_timeout_id)
        g_source_remove(ctx->stats_timeout_id);
    if (ctx->stats_timer)
        g_timer_destroy(ctx->stats_timer);

    if (ctx->pipeline) {
        gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
        gst_object_unref(ctx->pipeline);
    }

    if (ctx->loop) {
        g_main_loop_quit(ctx->loop);
        if (ctx->loop_thread)
            g_thread_join(ctx->loop_thread);
        g_main_loop_unref(ctx->loop);
    }

    LOG_INFO(MOD, "Pipeline destroyed");
    free(ctx);
}

const char *pipeline_describe(PipelineCtx *ctx)
{
    if (!ctx) return "(null)";
    return ctx->description;
}
