#include "receiver.h"
#include "../logger.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MOD                 "RECEIVER-PIPE"
#define RECEIVER_PIPE_BUF   2048

static char s_pipe_buf[RECEIVER_PIPE_BUF];


static int pcat(char *buf, size_t bufsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
 

static int pcat(char *buf, size_t bufsz, const char *fmt, ...)
{
    size_t used = strlen(buf);
    if (used >= bufsz -1) return -1;
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf + used, bufsz - used, fmt, ap);
    va_end(ap);
    return (ret < 0) ? -1:0;
}


/* ------------------------------------------------------------------------------- 
 * build_receiver_pipeline_str
 * 
 * Builds a gst-launch style pipeline string for the RECEIVER role:
 * 
 *      udpsrc -- [jitterbuffer] -- depay -- parse -- decode -- [convert] -- sink
 * 
 * Sink Variants:
 *  SINK_HDMI   : HW decode -> kmssink (zero-copy HDMI, no video conversion)
 *  SINK_DISPLAY: HW decode -> videoconvert -> autovideosink / glimagesink
 *  SINK_FILE   : parse -> mp4mux -> filesink (no decode, store compressed)
 *  SINK_RTSP   : parse -> rstpclientsink (no decode, re-stream)
 *
 * Profile-driven tuning:
 *  udp_buffer_size : UDP socket recv buffer (requires rmem_max >= value)
 *  rtp_do_timestamp: Override RTP timestamps with local clock.
 *                    Set true when sender /receiver clocks are not synced
 *                    (e.g. NXP -> Mac test). false for NXP->NXP with NTP.
 *  jitterbuffer_ms : rtpjitterbuffer latency in ms. 0 - disabled.
 *                     50 ms good starting point for Wi-Fi direct.
 * 
 * -------------------------------------------------------------------------------- */
const char *build_receiver_pipeline_str(const StreamConfig *cfg)
{
    const DeviceProfile *p  = &cfg->profile;
    CodecType           cod = cfg->codec;

    memset(s_pipe_buf, 0, sizeof(s_pipe_buf));

    /* -- 1. UDP source -------------------------------------------------- */
    const char *enc_name    = (cod == CODEC_H265) ? "H265" : "H264";
    int         pt          = (cod == CODEC_H265) ? p->rtp_pt_h265 : p->rtp_pt_h264;

     pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
         "udpsrc port=%d buffer-size=%d do-timestamp=%s "
         "caps=\"application/x-rtp,media=video,clock-rate=90000,"
         "encoding-name=%s,payload=%d\" ",
         p->stream_port,
         p->udp_buffer_size,
         p->rtp_do_timestamp ? "true" : "false",
         enc_name, pt);


    /* -- 2. Optional jitterbuffer --------------------------------------- */
    if (p->jitterbuffer_ms > 0) {
        pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
             "! rtpjitterbuffer latency=%d drop-on-latency=true ",
             p->jitterbuffer_ms);
    }

    /* -- 3. RTP depayloader + parser ------------------------------- */
    if (cod == CODEC_H265) {
        pcat(s_pipe_buf, RECEIVER_PIPE_BUF, "! rtph265depay ! h265parse ");
    } else {
        pcat(s_pipe_buf, RECEIVER_PIPE_BUF, "! rtph264depay ! h264parse ");
    }

    /* -- 4. Sink branch -------------------------------------------- */
    switch (cfg->sink) {
 
        /* HW decode → direct HDMI (zero-copy, no videoconvert) */
        case SINK_HDMI: {
            const char *dec_elem  = p->dec_element[cod];
            const char *dec_extra = p->dec_extra[cod];
            pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
                 "! %s %s ! %s sync=false",
                 dec_elem,
                 dec_extra[0] ? dec_extra : "",
                 p->sink_hdmi);
            break;
        }
 
        /* HW decode → videoconvert → display sink (PC/test) */
        case SINK_DISPLAY: {
            const char *dec_elem  = p->dec_element[cod];
            const char *dec_extra = p->dec_extra[cod];
            pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
                 "! %s %s ! videoconvert ! %s sync=false",
                 dec_elem,
                 dec_extra[0] ? dec_extra : "",
                 p->sink_deploy);
            break;
        }
 
        /* Remux to MP4 without decoding (store compressed stream) */
        case SINK_FILE: {
            const char *output = cfg->file_path[0]
                                 ? cfg->file_path
                                 : "/tmp/p2p-record.mp4";
            pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
                 "! mp4mux faststart=true ! filesink location=%s sync=false",
                 output);
            LOG_INFO(MOD, "FILE sink: output → %s", output);
            break;
        }
 
        /* Re-stream to RTSP server (push mode via rtspclientsink) */
        case SINK_RTSP: {
            pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
                 "! rtspclientsink location=rtsp://127.0.0.1:%d/stream latency=0",
                 cfg->rtsp_port);
            LOG_INFO(MOD, "RTSP sink: rtsp://127.0.0.1:%d/stream", cfg->rtsp_port);
            break;
        }
 
        default:
            LOG_ERROR(MOD, "Unknown sink type %d, falling back to autovideosink", cfg->sink);
            pcat(s_pipe_buf, RECEIVER_PIPE_BUF,
                 "! videoconvert ! autovideosink sync=false");
            break;
    }
 
    LOG_INFO(MOD, "Pipeline: %s", s_pipe_buf);
    return s_pipe_buf;
}