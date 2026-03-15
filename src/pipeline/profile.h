#ifndef P2P_PROFILE_H
#define P2P_PROFILE_H

/* -----------------------------------------------------------------------
 * Device profile — loaded from device-profiles/<device>.ini
 * Drives GStreamer element selection per platform.
 * --------------------------------------------------------------------- */

#define PROFILE_STR_MAX 256

typedef enum {
    CODEC_H265 = 0,
    CODEC_H264,
    CODEC_COUNT
} CodecType;

typedef enum {
    SINK_HDMI = 0,   /* waylandsink / kmssink     */
    SINK_DISPLAY,    /* autovideosink (PC/test)   */
    SINK_FILE,       /* mp4mux + filesink         */
    SINK_RTSP,       /* RTSP server re-stream     */
    SINK_COUNT
} SinkType;


typedef struct {
    /* Identity */
    char device_name [PROFILE_STR_MAX];     /* e.g. "nxp"               */
    char iface       [PROFILE_STR_MAX];     /* Wi-Fi iface: mlan0/wlan0 */

    /* Camera / Source */
    char camera_device[PROFILE_STR_MAX];    /* /dev/videoN                      */
    char src_element  [PROFILE_STR_MAX];    /* v4l2src / libcamerasrc           */
    char src_caps_fmt [PROFILE_STR_MAX];    /* NV12 / I420 / BGRx               */
    int  need_convert;                      /* 1 = insert videoconvert          */
    /* V4L2 I/O mode for v4l2src.
     * 2 = mmap  — safe default, works with all HW encoders incl. Hantro M2M.
     * 4 = dmabuf-export — DO NOT use with v4l2h265enc/v4l2h264enc: the Hantro
     *     VPU M2M encoder cannot import DMA-BUF from v4l2src, causing
     *     not-negotiated (-4) immediately on pipeline start.
     * 5 = dmabuf-import */
    int  src_io_mode;

    /* Encoder - indexed by CodecType  */
    char enc_element    [CODEC_COUNT][PROFILE_STR_MAX]; /* v4l2h265enc ...      */
    char enc_extra      [CODEC_COUNT][PROFILE_STR_MAX]; /* extra params         */
    /* output-io-mode on HW encoder output pad.
     * 0 = disabled (default), 4 = dmabuf-export.
     * Setting 4 lets the encoder push encoded buffers via DMA-BUF, bypassing
     * the internal output queue and reducing encode latency by ~1 second on
     * Hantro VPU. Safe to enable on iMX8MP. */
    int  enc_output_io_mode;
    int  enc_bitrate_unit_kbps;             /* 1 if element uses kbps            */

    /* Decoder - indexed by CodecType */
    char dec_element    [CODEC_COUNT][PROFILE_STR_MAX];
    char dec_extra      [CODEC_COUNT][PROFILE_STR_MAX];

    /* Sinks */
    char sink_hdmi      [PROFILE_STR_MAX];  /* kmssink / waylandsink            */
    char sink_deploy    [PROFILE_STR_MAX];  /* autovideosink / glimagesink      */

    /* RTP payload types */
    int  rtp_pt_h265;   /* default 96 */
    int  rtp_pt_h264;   /* default 97 */

    /* Network — UDP / RTP tuning */
    char peer_ip_host  [64];    /* peer address when role=host          */
    char peer_ip_client[64];    /* peer address when role=client        */
    int  stream_port;           /* default 5600                         */
    int  rtsp_port;             /* default 8554                         */

    int  net_retries;           /* default 3 */
    int  net_timeout_ms;        /* default 1000 */
    int  net_mtu;               /* default 1316 */

    int  sm_max_retries;        /* default 5 */
    int  sm_backoff_base_s;     /* default 2 */

    int  pipe_stats_period_ms;  /* default 2000 */
    int  pipe_h265_config_interval; /* default -1 */
    int  pipe_h264_config_interval; /* default -1 */
    int  pipe_pay_config_interval;  /* default 1 */

    /* UDP receive socket buffer size in bytes.
     * Increase on lossy links to absorb burst drops.
     * Requires net.core.rmem_max >= this value (set via sysctl).
     * Default: 2097152 (2 MB). Mac/PC test: 8388608 (8 MB). */
    int  udp_buffer_size;

    /* 1 = override incoming RTP timestamps with local wall-clock on udpsrc.
     * Required when sender and receiver clocks are not synchronised (e.g.
     * NXP → Mac test setup). For NXP↔NXP with a shared NTP source, set 0. */
    int  rtp_do_timestamp;

    /* rtpjitterbuffer latency in milliseconds.
     * 0 = jitterbuffer disabled (lowest latency, no reorder protection).
     * 50 = good starting point for Wi-Fi Direct.
     * Increase if mosaic appears on burst packet loss. */
    int  jitterbuffer_ms;

} DeviceProfile;

/* Load profile from INI file.
 * Searches: <profiles_dir>/<device>.ini
 * Returns 0 on success, -1 on error.
 */
int  profile_load (DeviceProfile *p, const char *profiles_dir, const char *device);
void profile_dump (const DeviceProfile *p);    /* Log all fields */

#endif /* P2P_PROFILE_H */
