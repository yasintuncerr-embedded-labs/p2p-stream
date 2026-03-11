#ifndef P2P_PROFILE_H
#define P2P_PROFILE_H

/* -----------------------------------------------------------------------
 * Device profile — loaded from device-profiles/<device>.ini
 * Drives GStreamer element selection per platform.
 * --------------------------------------------------------------------- */

#define PROFILE_STR_MAX 128

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
    char camera_device[PROFILE_STR_MAX];    /* dev/vide0                */
    char src_element[PROFILE_STR_MAX];      /* v4l2src / videotestsrc   */
    char src_caps_fmt[PROFILE_STR_MAX];     /* NV12 / I420 / BGRx       */
    int need_convert;                       /* 1 = insert videoconvert  */

    /* Encoder - indexed by CodecType  */
    char enc_element    [CODEC_COUNT][PROFILE_STR_MAX]; /* v4l2h265enc ...*/
    char enc_extra      [CODEC_COUNT][PROFILE_STR_MAX]; /* extra params   */
    int enc_bitrate_unit_kbps;                          /* 1 if elemenr uses kbps */

    /* Decoder - indexed by CodecType */
    char dec_element    [CODEC_COUNT][PROFILE_STR_MAX];
    char dec_extra      [CODEC_COUNT][PROFILE_STR_MAX];

    /* Sinks */
    char sink_hdmi      [PROFILE_STR_MAX];  /* waylandsink / kmssink ...*/
    char sink_deploy    [PROFILE_STR_MAX];  /* autovideosink            */

    /* RTP payload types */
    int  rtp_pt_h265;    /* default 96 */
    int  rtp_pt_h264;    /* default 97 */

    /* Network */
    char peer_ip_host  [64];   /* 192.168.77.2 (filled at runtime)   */
    char peer_ip_client[64];   /* 192.168.77.1                       */
    int  stream_port;          /* default 5600                       */
    int  rtsp_port;            /* default 8554                       */

} DeviceProfile;

/* Load profile from INI file.
 * Searches: <profiles_dir>/<device>.ini
 * Returns 0 on success, -1 on error. 
 */
 int profile_load (DeviceProfile *p, const char *profiles_dir, const char *device);
 void profile_dump (const DeviceProfile *p);    /* Log all fields */

#endif /* P2P_PROFILE_H */