#include "sender.h"
#include "../logger.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define MOD          "SENDER-PIPE"
#define SENDER_PIPE_BUF  2048

static char s_pipe_buf[SENDER_PIPE_BUF];

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
 * build_sender_pipeline_str
 *
 * Builds a gst-launch-style pipeline string for the SENDER role:
 *
 *   [test]  videotestsrc  ──┐
 *   [csi]   v4l2src       ──┴── caps ── [convert?] ── encoder ── rtppay ── udpsink
 *
 * enc_output_io_mode = 4 (dmabuf-export on encoder OUTPUT pad):
 *   Bypasses the Hantro VPU internal output queue, removing ~1 s of encode
 *   latency. Set output_io_mode=4 in [encoder] for NXP profiles.
 *   NOTE: this is the encoder's OUTPUT io-mode, completely independent of
 *   v4l2src's INPUT io-mode (which must stay at 2/mmap for Hantro M2M).
 *
 * The caller owns the returned pointer (static buffer — not re-entrant).
 * -------------------------------------------------------------------------- */
const char *build_sender_pipeline_str(const StreamConfig *cfg)
{
    const DeviceProfile *p   = &cfg->profile;
    CodecType            cod = cfg->codec;
    int                  bps = cfg->bitrate_bps;

    /* Bitrate in the unit the encoder expects */
    int enc_bitrate = p->enc_bitrate_unit_kbps ? (bps / 1000) : bps;

    memset(s_pipe_buf, 0, sizeof(s_pipe_buf));

    /* -- 1. Source -------------------------------------------------- */
    if (cfg->use_test_pattern) {
        pcat(s_pipe_buf, SENDER_PIPE_BUF,
             "videotestsrc pattern=ball is-live=true ");
    } else {
        /* Sources that do NOT accept device= or io-mode= properties.
         * libcamerasrc : Raspberry Pi libcamera source.
         * avfvideosrc  : macOS AVFoundation camera source. */
        int is_no_device_prop = (strncmp(p->src_element, "libcamera",   9)  == 0 ||
                                 strncmp(p->src_element, "avfvideosrc", 11) == 0);
        if (is_no_device_prop) {
            pcat(s_pipe_buf, SENDER_PIPE_BUF, "%s ", p->src_element);
        } else {
            /* io-mode=2 (mmap): safe with all V4L2 M2M encoders incl. Hantro.
             * Do NOT use 4 (dmabuf-export) here — the Hantro VPU M2M encoder
             * cannot import DMA-BUF from v4l2src → not-negotiated (-4). */
            pcat(s_pipe_buf, SENDER_PIPE_BUF,
                 "%s device=%s io-mode=%d ",
                 p->src_element, p->camera_device, p->src_io_mode);
        }
    }

    /* -- 2. Source caps -------------------------------------------- */
    pcat(s_pipe_buf, SENDER_PIPE_BUF,
         "! video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1 ",
         p->src_caps_fmt, cfg->width, cfg->height, cfg->fps);

    /* -- 3. Optional colorspace convert (SW encoders / test pattern) */
    if (p->need_convert || cfg->use_test_pattern) {
        pcat(s_pipe_buf, SENDER_PIPE_BUF, "! videoconvert ");
        if (!cfg->use_test_pattern) {
            pcat(s_pipe_buf, SENDER_PIPE_BUF,
                 "! video/x-raw,format=NV12,width=%d,height=%d ",
                 cfg->width, cfg->height);
        }
    }

    /* -- 4. Encoder ------------------------------------------------- */
    const char *enc_elem  = p->enc_element[cod];
    const char *enc_extra = p->enc_extra[cod];

    /* V4L2 HW encoders (v4l2h265enc, v4l2h264enc) do not have a GStreamer
     * "bitrate" property — bitrate is a V4L2 control injected via
     * extra-controls as "video_bitrate=N".
     *
     * We initialize them here without injecting the bitrate manually into
     * the pipeline string. The bitrate will be properly written via 
     * g_object_set / GstStructure inside pipeline_create(). */
    int is_v4l2_enc = (strncmp(enc_elem, "v4l2", 4) == 0);

    /* Build output-io-mode fragment (empty string if not set) */
    char out_io[32] = "";
    if (is_v4l2_enc && p->enc_output_io_mode != 0)
        snprintf(out_io, sizeof(out_io), "output-io-mode=%d ", p->enc_output_io_mode);

    if (is_v4l2_enc) {
        /* Do not specify video_bitrate here. Pass down enc_extra unaltered. */
        pcat(s_pipe_buf, SENDER_PIPE_BUF,
             "! %s name=encoder %s %s ",
             enc_elem, out_io,
             enc_extra[0] ? enc_extra : "");
    } else {
        /* SW encoders: bitrate is a plain GStreamer property */
        pcat(s_pipe_buf, SENDER_PIPE_BUF,
             "! %s name=encoder bitrate=%d %s ",
             enc_elem, enc_bitrate,
             enc_extra[0] ? enc_extra : "");
    }

    /* -- 5. Parse + RTP packetiser ---------------------------------- */
    if (cod == CODEC_H265) {
        pcat(s_pipe_buf, SENDER_PIPE_BUF, "! h265parse config-interval=%d ", p->pipe_h265_config_interval);
        pcat(s_pipe_buf, SENDER_PIPE_BUF,
             "! rtph265pay config-interval=%d pt=%d mtu=%d ",
             p->pipe_pay_config_interval, p->rtp_pt_h265, p->net_mtu);
    } else {
        pcat(s_pipe_buf, SENDER_PIPE_BUF, "! h264parse config-interval=%d ", p->pipe_h264_config_interval);
        pcat(s_pipe_buf, SENDER_PIPE_BUF,
             "! rtph264pay config-interval=%d pt=%d mtu=%d ",
             p->pipe_pay_config_interval, p->rtp_pt_h264, p->net_mtu);
    }

    /* -- 6. UDP sink ------------------------------------------------ */
    pcat(s_pipe_buf, SENDER_PIPE_BUF,
         "! udpsink host=%s port=%d sync=false async=false",
         cfg->peer_ip, p->stream_port);

    LOG_INFO(MOD, "Pipeline: %s", s_pipe_buf);
    return s_pipe_buf;
}