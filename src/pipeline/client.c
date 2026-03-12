#include "client.h"
#include "../logger.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MOD       "CLIENT-PIPE"
#define CLIENT_PIPE_BUF  2048

static char s_pipe_buf[CLIENT_PIPE_BUF];

static int pcat(char *buf, size_t bufsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int pcat(char *buf, size_t bufsz, const char *fmt, ...)
{
    size_t used = strlen(buf);
    if (used >= bufsz - 1) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, bufsz - used, fmt, ap);
    va_end(ap);
    return (n < 0) ? -1 : 0;
}


/* --------------------------------------------------------------------------
 * build_client_pipeline_str
 * 
 * SINK_HDMI    : udpsrc - depay - parse - decode - [convert] -> kmssink
 * SINK_DISPLAY : udpsrc - depay - parse - decode - [convert] -> autovideosink
 * SINK_FILE    : udpsrc - depay - parse - muxer - filesink (no decode )
 * SINK_RTSP    : udpsrc - depay - parse - rtspclientsink (re-streams)
 *
 * FILE and RTSP sinks do not decode, - store/forward compressed stream
 * -------------------------------------------------------------------------- */

 const char *build_client_pipeline_str(const StreamConfig *cfg) 
 {
    const DeviceProfile *p = &cfg->profile;
    CodecType           cod = cfg->codec;
    
    memset(s_pipe_buf, 0, sizeof(s_pipe_buf)); 

    /* -- 1. UDP source + RTP caps --------------------------------*/
    const char *enc_name = (cod == CODEC_H265) ? "H265" : "H264";
    int        pt        = (cod == CODEC_H265) ? p->rtp_pt_h265 : p->rtp_pt_h264;
  
    pcat(s_pipe_buf, CLIENT_PIPE_BUF,
        "udpsrc port=%d buffer-size=2094512 "
        "caps=\"application/x-rtp,media=video,clock-rate=90000,"
        "encoding-name=%s,payload=%d\" ",
        p->stream_port, enc_name, pt
    );

    /* -- 2. RTP depayloader + parser --------------------------- */
    if (cod == CODEC_H265) {
        pcat(s_pipe_buf, CLIENT_PIPE_BUF, "! rtph265depay ! h265parse ");
    } else {
        pcat(s_pipe_buf, CLIENT_PIPE_BUF, "! rtph264depay ! h264parse ");
    }

    /* -- 3. Sink branch ------------------------------------------ */
    switch (cfg->sink) {
        /* HW*/
        case SINK_HDMI: {
            const char *dec_elem = p->dec_element[cod];
            const char *dec_extra = p->dec_extra[cod];
            pcat(s_pipe_buf, CLIENT_PIPE_BUF,
            "! %s %s ",
            dec_elem, dec_extra[0] ? dec_extra : "");
            pcat(s_pipe_buf, CLIENT_PIPE_BUF,
            "! %s sync=false",
            p->sink_hdmi);
            break;
        }

        /* SW Fallback decode -> autovideosink(PC/test) */
        case SINK_DISPLAY: {
            const char *dec_elem  = p->dec_element[cod];
            const char *dec_extra = p->dec_extra[cod];
            pcat(s_pipe_buf, CLIENT_PIPE_BUF,
            "! %s %s ! videoconvert ! %s sync=false",
            dec_elem,
            dec_extra[0] ? dec_extra : "",
            p->sink_deploy);
            break;
        }

        /* -- File: remux to MP4 without re-encoding ----------------------- */
        case SINK_FILE: {
            /* Use a tee to allow simultaneous display + record in future.
             * For now: pure file recording path.                            */
            const char *muxer = (cod == CODEC_H265) ? "mp4mux" : "mp4mux";
            /* h265 in MP4: needs mp4mux with allow-signed-integer-overflow=1
             * for some GStreamer versions — handled in mp4mux properties    */
            pcat(s_pipe_buf, CLIENT_PIPE_BUF,
             "! %s faststart=true ! filesink location=%s sync=false",
             muxer,
             cfg->file_path[0] ? cfg->file_path : "/tmp/p2p-record.mp4");
            LOG_WARN(MOD, "FILE sink: output → %s",
                     cfg->file_path[0] ? cfg->file_path : "/tmp/p2p-record.mp4");
            break;
        }
        /* -- RTSP: re-stream to RTSP server (gst-rtsp-server) ------------- */
        
        /* NOTE: rtspclientsink pushes to an existing RTSP server mount.
         *       For a self-hosted server, p2p-stream launches gst-rtsp-server
         *       separately (see rtsp_server.c).
         *       Here we use rtspclientsink for push-to-server mode.         */
        case SINK_RTSP: {
            pcat(s_pipe_buf, CLIENT_PIPE_BUF,
            "! rtspclientsink location=rtsp://127.0.0.1:%d/stream latency=0",
            cfg->rtsp_port);
            LOG_INFO(MOD, "RTSP sink: rtsp://127.0.0.1:%d/stream", cfg->rtsp_port);
            break;
        }

        default:
            LOG_ERROR(MOD, "Unknown sink type %d, defaulting to autovideosink", cfg->sink);
            pcat(s_pipe_buf, CLIENT_PIPE_BUF, "! videoconvert ! autovideosink sync=false");
            break;
    }

    LOG_INFO(MOD, "Pipeline: %s", s_pipe_buf);
    return s_pipe_buf;
}


