#include "profile.h"
#include "../logger.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define MOD "PROFILE"

static int valid_device_selector(const char *device)
{
    if (!device || !device[0]) return 0;
    for (const char *p = device; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
            return 0;
        }
    }
    return 1;
}

/* -----------------------------------------------------------------------
 * Helper: read string from key file with fallback
 * --------------------------------------------------------------------- */
static void kf_str(GKeyFile *kf, const char *grp, const char *key,
                   char *dst, size_t dstsz, const char *fallback)
{
    GError *err = NULL;
    char   *val = g_key_file_get_string(kf, grp, key, &err);
    if (err) {
        g_error_free(err);
        strncpy(dst, fallback, dstsz - 1);
    } else {
        strncpy(dst, val, dstsz - 1);
        g_free(val);
    }
    dst[dstsz - 1] = '\0';
}

static int kf_int(GKeyFile *kf, const char *grp, const char *key, int fallback)
{
    GError *err = NULL;
    int val = g_key_file_get_integer(kf, grp, key, &err);
    if (err) { g_error_free(err); return fallback; }
    return val;
}

static int kf_bool(GKeyFile *kf, const char *grp, const char *key, int fallback)
{
    GError *err = NULL;
    gboolean val = g_key_file_get_boolean(kf, grp, key, &err);
    if (err) { g_error_free(err); return fallback; }
    return (int)val;
}

/* -----------------------------------------------------------------------
 * profile_load
 * --------------------------------------------------------------------- */
int profile_load(DeviceProfile *p, const char *profiles_dir, const char *device)
{
    if (!p || !profiles_dir || !device) return -1;
    if (!valid_device_selector(device)) {
        LOG_ERROR(MOD, "Invalid device selector: '%s'", device ? device : "");
        return -1;
    }
    if (strstr(profiles_dir, "..") != NULL) {
        LOG_ERROR(MOD, "Invalid profiles_dir contains '..': %s", profiles_dir);
        return -1;
    }
    memset(p, 0, sizeof(*p));

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ini", profiles_dir, device);

    GKeyFile *kf = g_key_file_new();
    GError   *err = NULL;
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        LOG_ERROR(MOD, "Cannot load profile '%s': %s", path,
                  err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_key_file_free(kf);
        return -1;
    }

    /* [device] */
    kf_str(kf, "device", "name",  p->device_name, sizeof(p->device_name), device);
    kf_str(kf, "device", "iface", p->iface,        sizeof(p->iface),        "wlan0");

    /* [source] */
    kf_str(kf, "source", "camera_device", p->camera_device, sizeof(p->camera_device), "/dev/video0");
    kf_str(kf, "source", "element",       p->src_element,   sizeof(p->src_element),   "v4l2src");
    kf_str(kf, "source", "caps_format",   p->src_caps_fmt,  sizeof(p->src_caps_fmt),  "NV12");
    p->need_convert = kf_bool(kf, "source", "need_convert", 0);
    /* io_mode: 2=mmap (safe default). 4=dmabuf-export must NOT be used with
     * v4l2h265enc/v4l2h264enc — the Hantro M2M encoder cannot import DMA-BUF
     * from v4l2src, causing not-negotiated (-4) on pipeline start. */
    p->src_io_mode  = kf_int(kf,  "source", "io_mode",      2);

    /* [encoder] */
    kf_str(kf, "encoder", "h265_element", p->enc_element[CODEC_H265], PROFILE_STR_MAX, "x265enc");
    kf_str(kf, "encoder", "h265_extra",   p->enc_extra  [CODEC_H265], PROFILE_STR_MAX, "");
    kf_str(kf, "encoder", "h264_element", p->enc_element[CODEC_H264], PROFILE_STR_MAX, "x264enc");
    kf_str(kf, "encoder", "h264_extra",   p->enc_extra  [CODEC_H264], PROFILE_STR_MAX, "");
    p->enc_bitrate_unit_kbps = kf_bool(kf, "encoder", "bitrate_unit_kbps", 0);
    /* output-io-mode on encoder output pad.
     * 0 = off (default). 4 = dmabuf-export — reduces Hantro VPU encode
     * latency by ~1 s by bypassing the internal output queue. Safe on iMX8MP.
     * Set output_io_mode=4 in [encoder] for NXP profiles. */
    p->enc_output_io_mode = kf_int(kf, "encoder", "output_io_mode", 0);

    /* [decoder] */
    kf_str(kf, "decoder", "h265_element", p->dec_element[CODEC_H265], PROFILE_STR_MAX, "avdec_h265");
    kf_str(kf, "decoder", "h265_extra",   p->dec_extra  [CODEC_H265], PROFILE_STR_MAX, "");
    kf_str(kf, "decoder", "h264_element", p->dec_element[CODEC_H264], PROFILE_STR_MAX, "avdec_h264");
    kf_str(kf, "decoder", "h264_extra",   p->dec_extra  [CODEC_H264], PROFILE_STR_MAX, "");

    /* [sink] */
    kf_str(kf, "sink", "hdmi",    p->sink_hdmi,   sizeof(p->sink_hdmi),   "kmssink");
    kf_str(kf, "sink", "display", p->sink_deploy, sizeof(p->sink_deploy), "autovideosink");

    /* [network] */
    p->rtp_pt_h265  = kf_int(kf, "network", "rtp_pt_h265",  96);
    p->rtp_pt_h264  = kf_int(kf, "network", "rtp_pt_h264",  97);
    
    p->fec_percentage = kf_int(kf, "network", "fec_percentage", 0);
    p->fec_pt         = kf_int(kf, "network", "fec_pt", 122);
    p->stream_port  = kf_int(kf, "network", "stream_port",  5600);
    p->rtsp_port    = kf_int(kf, "network", "rtsp_port",    8554);

    p->net_retries    = kf_int(kf, "network", "retries", 3);
    p->net_timeout_ms = kf_int(kf, "network", "timeout_ms", 1000);
    p->net_mtu        = kf_int(kf, "network", "mtu", 1316);

    p->sm_max_retries = kf_int(kf, "network", "sm_max_retries", 5);
    p->sm_backoff_base_s = kf_int(kf, "network", "sm_backoff_base_s", 2);

    p->pipe_stats_period_ms = kf_int(kf, "network", "pipe_stats_period_ms", 2000);
    p->pipe_h265_config_interval = kf_int(kf, "network", "pipe_h265_config_interval", -1);
    p->pipe_h264_config_interval = kf_int(kf, "network", "pipe_h264_config_interval", -1);
    p->pipe_pay_config_interval  = kf_int(kf, "network", "pipe_pay_config_interval", 1);

    kf_str(kf, "network", "peer_ip_host",   p->peer_ip_host,   sizeof(p->peer_ip_host),   "192.168.77.1");
    kf_str(kf, "network", "peer_ip_client", p->peer_ip_client, sizeof(p->peer_ip_client), "192.168.77.2");

    /* UDP receive buffer size. Requires net.core.rmem_max >= this value.
     * Default 2 MB is safe; increase to 8 MB on lossy Wi-Fi links. */
    p->udp_buffer_size = kf_int(kf, "network", "udp_buffer_size", 2097152);

    /* Override RTP timestamps with local wall-clock on receive side.
     * Set true when sender/receiver clocks are not synced (e.g. NXP→Mac).
     * For NXP↔NXP with shared NTP, leave false. */
    p->rtp_do_timestamp = kf_bool(kf, "network", "rtp_do_timestamp", 0);

    /* rtpjitterbuffer latency in ms. 0 = jitterbuffer disabled.
     * 50 ms is a good starting point for Wi-Fi Direct.
     * Increase if mosaic appears on burst packet loss. */
    p->jitterbuffer_ms = kf_int(kf, "network", "jitterbuffer_ms", 0);

    g_key_file_free(kf);
    LOG_INFO(MOD, "Loaded profile '%s' from %s", p->device_name, path);
    return 0;
}


/* -----------------------------------------------------------------------
 * profile_dump — logs all fields at DEBUG level
 * --------------------------------------------------------------------- */
void profile_dump(const DeviceProfile *p)
{
    LOG_DEBUG(MOD, "=== Device Profile ===");
    LOG_DEBUG(MOD, "  device         : %s", p->device_name);
    LOG_DEBUG(MOD, "  iface          : %s", p->iface);
    LOG_DEBUG(MOD, "  camera         : %s (elem=%s fmt=%s conv=%d io=%d)",
              p->camera_device, p->src_element, p->src_caps_fmt,
              p->need_convert, p->src_io_mode);
    LOG_DEBUG(MOD, "  enc H265       : %s %s (out-io=%d)",
              p->enc_element[CODEC_H265], p->enc_extra[CODEC_H265], p->enc_output_io_mode);
    LOG_DEBUG(MOD, "  enc H264       : %s %s (out-io=%d)",
              p->enc_element[CODEC_H264], p->enc_extra[CODEC_H264], p->enc_output_io_mode);
    LOG_DEBUG(MOD, "  dec H265       : %s %s", p->dec_element[CODEC_H265], p->dec_extra[CODEC_H265]);
    LOG_DEBUG(MOD, "  dec H264       : %s %s", p->dec_element[CODEC_H264], p->dec_extra[CODEC_H264]);
    LOG_DEBUG(MOD, "  sink hdmi      : %s", p->sink_hdmi);
    LOG_DEBUG(MOD, "  sink display   : %s", p->sink_deploy);
    LOG_DEBUG(MOD, "  port stream    : %d  rtsp: %d", p->stream_port, p->rtsp_port);
    LOG_DEBUG(MOD, "  peer host      : %s  client: %s", p->peer_ip_host, p->peer_ip_client);
    LOG_DEBUG(MOD, "  udp_buf        : %d bytes", p->udp_buffer_size);
    LOG_DEBUG(MOD, "  do-timestamp   : %s", p->rtp_do_timestamp ? "true" : "false");
    LOG_DEBUG(MOD, "  jitterbuffer   : %d ms%s", p->jitterbuffer_ms,
              p->jitterbuffer_ms == 0 ? " (disabled)" : "");
}
