#include "host.h"
#include "../logger.h"

#include <stdio.h>
#include <string.h>

#define MOD "HOST-PIPE"
/* ----------------------------------------------------------------------------
 * build host_pipeline_str
 * 
 * Builds a gst-launch-style pipeline description string for:
 * 
 *  [test] videotestsrc  --
 *  [csi] v4l2src        -- --- caps - [convert?] - encoder - rtppay - udpsink
 * 
 *  The caller owns the buffer (static, not re-entrant - single pipeline). 
 * ----------------------------------------------------------------------------
*/

#define HOST_PIPE_BUF 2048

static char s_pipe_buf[HOST_PIPE_BUF];

/* Helper: append to buffer with bounds check */
static int pcat(char *buf, size_t bufsz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int pcat(char *buf, size_t bufsz, const char *fmt, ...) 
{
    size_t used = strlen(buf);
    if (used>= bufsz -1) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, bufsz -used, fmt, ap);
    va_end(ap);
    return (n < 0) ? -1:0;
}


const char *build_host_pipeline_str(const StreamConfig *cfg) 
{
    const DeviceProfile *p  = &cfg->profile;
    CodecType           cod = cfg->codec;
    int                 bps = cfg->bitrate_bps;

    /* bitrate in the unit the encoder expects */
    int enc_bitrate = p->enc_bitrate_unit_kbps ? (bps / 1000 ) : bps;

    memset(s_pipe_buf, 0, sizeof(s_pipe_buf));

    /* -- 1. Source -------------------------------------------------- */
    if (cfg->use_test_pattern) {
        pcat(s_pipe_buf, HOST_PIPE_BUF,
                "videotestsrc pattern=ball is-live=true "
        );
    } else {
        pcat(s_pipe_buf, HOST_PIPE_BUF,
            "%s device=%s io-mode=4 ", /* trailing space keeps '!' separate */
            p->src_element, p->camera_device);
    }

    /* -- 2. Source caps -------------------------------------------- */
    pcat(s_pipe_buf, HOST_PIPE_BUF,
        "! video/x-raw, format=%s,width=%d,height=%d,framerate=%d/1 ",
        p->src_caps_fmt, cfg->width, cfg->height, cfg->fps);

    /* -- 3. Optional Colorspace convert(needed for SW encoders)----- */
    if (p->need_convert || cfg->use_test_pattern) {
        pcat(s_pipe_buf, HOST_PIPE_BUF, "! videoconvert");
        /* Re-force format after convert - some encoders need explicit NV12 */
        if(!cfg->use_test_pattern) {
            pcat(s_pipe_buf, HOST_PIPE_BUF,
                "! video/x-raw,format=NV12,width=%d,height=%d",
                cfg->width, cfg->height
            );
        }
    }

    /* -- 4. Encoder ----------------------------------------------- */
    const char *enc_elem  = p->enc_element[cod];
    const char *enc_extra = p->enc_extra[cod];

    /* V4L2 hardware encoders (v4l2h265enc, v4l2h264enc ...) do not have a
     * GStreamer "bitrate" property.  Bitrate is a V4L2 control, so it must
     * be injected into the extra-controls string as "video_bitrate=N".     */
    int is_v4l2_enc = (strncmp(enc_elem, "v4l2", 4) == 0);

    if (is_v4l2_enc) {
        /* Build the encoder fragment with video_bitrate inside extra-controls.
         * Profile enc_extra is expected to look like:
         *   extra-controls="controls,key=val,..."
         * We insert video_bitrate right after "controls,".                  */
        const char *tag = "extra-controls=\"controls,";
        const char *pos = enc_extra[0] ? strstr(enc_extra, tag) : NULL;

        if (pos) {
            /* Inject video_bitrate right after "controls," */
            int prefix_len = (int)(pos - enc_extra) + (int)strlen(tag);
            pcat(s_pipe_buf, HOST_PIPE_BUF,
                 " ! %s %.*svideo_bitrate=%d,%s ",
                 enc_elem, prefix_len, enc_extra,
                 enc_bitrate, pos + strlen(tag));
        } else {
            /* No extra-controls in profile — build one from scratch */
            pcat(s_pipe_buf, HOST_PIPE_BUF,
                 " ! %s extra-controls=\"controls,video_bitrate=%d\" %s ",
                 enc_elem, enc_bitrate,
                 enc_extra[0] ? enc_extra : "");
        }
    } else {
        /* Software encoders: bitrate is a plain GStreamer property */
        pcat(s_pipe_buf, HOST_PIPE_BUF,
             " ! %s bitrate=%d %s ",
             enc_elem, enc_bitrate,
             enc_extra[0] ? enc_extra : "");
    }

    /* RTP packetiser */
    if (cod == CODEC_H265) {
        pcat(s_pipe_buf, HOST_PIPE_BUF, "! h265parse config-interval=-1 ");
        pcat(s_pipe_buf, HOST_PIPE_BUF,
             "! rtph265pay config-interval=1 pt=%d mtu=1316 ",
             p->rtp_pt_h265);
    } else {
        pcat(s_pipe_buf, HOST_PIPE_BUF, "! h264parse config-interval=-1 ");
        pcat(s_pipe_buf, HOST_PIPE_BUF,
             "! rtph264pay config-interval=1 pt=%d mtu=1316 ",
             p->rtp_pt_h264);
    }

    /* -- 5. UDP Sink -------------------------------------------- */
    pcat(s_pipe_buf, HOST_PIPE_BUF,
        "! udpsink host=%s port=%d sync=false async=false",
        cfg->peer_ip, p->stream_port);

    LOG_INFO(MOD, "Pipeline: %s", s_pipe_buf);
    return s_pipe_buf;
}
