#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

#include "logger.h"
#include "state_machine.h"
#include "control.h"
#include "pipeline/profile.h"
#include "pipeline/pipeline.h"

#define DEFAULT_PROFILES_DIR  "/etc/p2p-stream/device-profiles"
#define DEFAULT_LOG_FILE      "/var/log/p2p-stream.log"
#define DEFAULT_WIDTH         1280
#define DEFAULT_HEIGHT        720
#define DEFAULT_FPS           120
#define DEFAULT_BITRATE_BPS   (6 * 1000 * 1000)  /* 6 Mbps */


/* -----------------------------------------------------------------------
 * Global SM for signal handler
 * --------------------------------------------------------------------- */
static StreamSM *g_sm = NULL;

static void sig_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\np2p-stream: caught signal, stopping...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    if (g_sm) sm_post_event(g_sm, SM_EVT_STOP);
}


/* -----------------------------------------------------------------------
 * Usage
 * --------------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Network role (who owns the Wi-Fi link):\n"
        "  -n, --net-role    <ap|sta>               (required)\n"
        "        ap  = this device creates the Wi-Fi link (display side)\n"
        "        sta = this device joins the Wi-Fi link (camera side)\n\n"
        "Stream role (who sends the video):\n"
        "  -r, --role        <sender|receiver>       (required)\n"
        "        sender   = capture, encode, send UDP\n"
        "        receiver = receive UDP, decode, display\n\n"
        "  Typical pairing:\n"
        "        Camera device  : --net-role sta --role sender\n"
        "        Display device : --net-role ap  --role receiver\n\n"
        "Other options:\n"
        "  -d, --device      <nxp|jetson|rpi4|pc>   (required)\n"
        "  -c, --codec       <h265|h264>              (default: h264)\n"
        "  -s, --sink        <hdmi|display|file|rtsp> (default: hdmi)\n"
        "  -t, --trigger     <auto|manual|gpio>       (default: auto)\n"
        "  -W, --width       <pixels>                 (default: %d)\n"
        "  -H, --height      <pixels>                 (default: %d)\n"
        "  -f, --fps         <fps>                    (default: %d)\n"
        "  -b, --bitrate     <bps>                    (default: %d)\n"
        "  -o, --output      <file path>              (for --sink file)\n"
        "  -p, --peer-ip     <ip>                     (override peer IP)\n"
        "  -P, --profiles    <dir>                    (default: %s)\n"
        "  -l, --log-file    <path>                   (default: %s)\n"
        "  -T, --test-pattern                         (use videotestsrc)\n"
        "  -v, --verbose                              (debug logging)\n"
        "  -h, --help\n\n"
        "Examples:\n"
        "  Camera  : %s --net-role sta --role sender   --device nxp\n"
        "  Display : %s --net-role ap  --role receiver --device nxp --sink hdmi\n"
        "  PC test : %s --net-role sta --role sender   --device pc  --test-pattern\n\n",
        prog,
        DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FPS, DEFAULT_BITRATE_BPS,
        DEFAULT_PROFILES_DIR, DEFAULT_LOG_FILE,
        prog, prog, prog);
}


/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    /* Defaults */
    StreamConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.net_role    = NET_ROLE_AP;
    cfg.role        = ROLE_RECEIVER;
    cfg.codec       = CODEC_H264;
    cfg.sink        = SINK_HDMI;
    cfg.trigger     = TRIGGER_AUTO;
    cfg.width       = DEFAULT_WIDTH;
    cfg.height      = DEFAULT_HEIGHT;
    cfg.fps         = DEFAULT_FPS;
    cfg.bitrate_bps = DEFAULT_BITRATE_BPS;
    cfg.rtsp_port   = 8554;

    char device[64]        = "";
    char profiles_dir[256] = DEFAULT_PROFILES_DIR;
    char log_file[256]     = DEFAULT_LOG_FILE;
    int  verbose           = 0;
    int  net_role_set      = 0;
    int  stream_role_set   = 0;

    static struct option long_opts[] = {
        {"net-role",      required_argument, 0, 'n'},
        {"role",          required_argument, 0, 'r'},
        {"device",        required_argument, 0, 'd'},
        {"codec",         required_argument, 0, 'c'},
        {"sink",          required_argument, 0, 's'},
        {"trigger",       required_argument, 0, 't'},
        {"width",         required_argument, 0, 'W'},
        {"height",        required_argument, 0, 'H'},
        {"fps",           required_argument, 0, 'f'},
        {"bitrate",       required_argument, 0, 'b'},
        {"output",        required_argument, 0, 'o'},
        {"peer-ip",       required_argument, 0, 'p'},
        {"profiles",      required_argument, 0, 'P'},
        {"log-file",      required_argument, 0, 'l'},
        {"test-pattern",  no_argument,       0, 'T'},
        {"verbose",       no_argument,       0, 'v'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "n:r:d:c:s:t:W:H:f:b:o:p:P:l:Tvh",
                               long_opts, &idx)) != -1) {
        switch (opt) {
        case 'n':
            if      (strcmp(optarg, "ap")  == 0) cfg.net_role = NET_ROLE_AP;
            else if (strcmp(optarg, "sta") == 0) cfg.net_role = NET_ROLE_STA;
            else { fprintf(stderr, "Unknown net-role: %s (use ap|sta)\n", optarg); return 1; }
            net_role_set = 1;
            break;
        case 'r':
            if      (strcmp(optarg, "sender")   == 0) cfg.role = ROLE_SENDER;
            else if (strcmp(optarg, "receiver") == 0) cfg.role = ROLE_RECEIVER;
            else { fprintf(stderr, "Unknown role: %s (use sender|receiver)\n", optarg); return 1; }
            stream_role_set = 1;
            break;
        case 'd': strncpy(device, optarg, sizeof(device) - 1);  break;
        case 'c':
            if      (strcmp(optarg, "h265") == 0) cfg.codec = CODEC_H265;
            else if (strcmp(optarg, "h264") == 0) cfg.codec = CODEC_H264;
            else { fprintf(stderr, "Unknown codec: %s\n", optarg); return 1; }
            break;
        case 's':
            if      (strcmp(optarg, "hdmi")    == 0) cfg.sink = SINK_HDMI;
            else if (strcmp(optarg, "display") == 0) cfg.sink = SINK_DISPLAY;
            else if (strcmp(optarg, "file")    == 0) cfg.sink = SINK_FILE;
            else if (strcmp(optarg, "rtsp")    == 0) cfg.sink = SINK_RTSP;
            else { fprintf(stderr, "Unknown sink: %s\n", optarg); return 1; }
            break;
        case 't':
            if      (strcmp(optarg, "auto")   == 0) cfg.trigger = TRIGGER_AUTO;
            else if (strcmp(optarg, "manual") == 0) cfg.trigger = TRIGGER_MANUAL;
            else if (strcmp(optarg, "gpio")   == 0) cfg.trigger = TRIGGER_GPIO;
            else { fprintf(stderr, "Unknown trigger: %s\n", optarg); return 1; }
            break;
        case 'W': cfg.width       = atoi(optarg); break;
        case 'H': cfg.height      = atoi(optarg); break;
        case 'f': cfg.fps         = atoi(optarg); break;
        case 'b': cfg.bitrate_bps = atoi(optarg); break;
        case 'o': strncpy(cfg.file_path, optarg, sizeof(cfg.file_path) - 1); break;
        case 'p': strncpy(cfg.peer_ip,   optarg, sizeof(cfg.peer_ip)   - 1); break;
        case 'P': strncpy(profiles_dir,  optarg, sizeof(profiles_dir)  - 1); break;
        case 'l': strncpy(log_file,      optarg, sizeof(log_file)      - 1); break;
        case 'T': cfg.use_test_pattern = 1; break;
        case 'v': verbose = 1;  break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Validate required args */
    if (!device[0]) {
        fprintf(stderr, "Error: --device is required\n\n");
        usage(argv[0]); return 1;
    }
    if (!net_role_set) {
        fprintf(stderr, "Error: --net-role is required (ap|sta)\n\n");
        usage(argv[0]); return 1;
    }
    if (!stream_role_set) {
        fprintf(stderr, "Error: --role is required (sender|receiver)\n\n");
        usage(argv[0]); return 1;
    }

    /* ── Init logger ─────────────────────────────────────────────── */
    logger_init("p2p-stream", log_file,
                verbose ? P2P_LOG_DEBUG : P2P_LOG_INFO);

    LOG_INFO("MAIN", "p2p-stream starting — net=%s stream=%s device=%s codec=%s",
             cfg.net_role == NET_ROLE_AP  ? "ap"  : "sta",
             cfg.role     == ROLE_SENDER  ? "sender" : "receiver",
             device,
             cfg.codec    == CODEC_H265   ? "h265" : "h264");

    /* ── Load device profile ─────────────────────────────────────── */
    if (profile_load(&cfg.profile, profiles_dir, device) != 0) {
        LOG_FATAL("MAIN", "Failed to load profile '%s' from '%s'", device, profiles_dir);
        logger_deinit();
        return 1;
    }
    profile_dump(&cfg.profile);

    /* ── Resolve peer IP ─────────────────────────────────────────── */
    if (cfg.peer_ip[0]) {
        LOG_INFO("MAIN", "Peer IP override: %s", cfg.peer_ip);
    } else {
        /* Sender → peer is the receiver (AP side).
         * Receiver → peer is the sender (STA side). */
        const char *default_peer = (cfg.role == ROLE_SENDER)
                                   ? cfg.profile.peer_ip_host
                                   : cfg.profile.peer_ip_client;
        snprintf(cfg.peer_ip, sizeof(cfg.peer_ip), "%s", default_peer);
        LOG_INFO("MAIN", "Peer IP from profile: %s", cfg.peer_ip);
    }

    /* ── Signal handlers ─────────────────────────────────────────── */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* ── Create state machine ────────────────────────────────────── */
    g_sm = sm_create(&cfg, NULL, NULL);
    if (!g_sm) {
        LOG_FATAL("MAIN", "Failed to create state machine");
        logger_deinit();
        return 1;
    }

    /* ── Start control FIFO ──────────────────────────────────────── */
    if (control_init(g_sm) != 0)
        LOG_WARN("MAIN", "Control FIFO init failed — running without remote control");

    LOG_INFO("MAIN", "Running. Send commands via /run/p2p-stream.cmd");

    /* Park main thread — everything runs in SM/pipeline/control threads */
    pause();

    /* ── Cleanup ──────────────────────────────────────────────────── */
    LOG_INFO("MAIN", "Shutting down...");
    control_deinit();
    sm_destroy(g_sm);
    gst_deinit();
    logger_deinit();
    return 0;
}
