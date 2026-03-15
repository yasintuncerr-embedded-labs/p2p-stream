#include "control.h"
#include "logger.h"
#include "event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define MOD          "CTRL"
#define DEFAULT_RUN_DIR "/run"
#define FALLBACK_RUN_DIR "/tmp"
#define BUF_SZ       256

static struct {
    pthread_t        thread;
    volatile int     running;
    int              fifo_fd;
    char             cmd_fifo[128];
    char             status_file[128];
    int              current_state; /* Tracks SmState approximation */
} g_ctrl;

static void resolve_runtime_paths(void)
{
    const char *run_dir = getenv("P2P_STREAM_RUN_DIR");
    if (!run_dir || !run_dir[0]) run_dir = DEFAULT_RUN_DIR;

    if (access(run_dir, W_OK) != 0) {
        LOG_WARN(MOD, "Runtime dir '%s' not writable, using %s", run_dir, FALLBACK_RUN_DIR);
        run_dir = FALLBACK_RUN_DIR;
    }

    snprintf(g_ctrl.cmd_fifo, sizeof(g_ctrl.cmd_fifo), "%s/p2p-stream.cmd", run_dir);
    snprintf(g_ctrl.status_file, sizeof(g_ctrl.status_file), "%s/p2p-stream.status", run_dir);
}

/* -----------------------------------------------------------------------
 * Status JSON writer
 * --------------------------------------------------------------------- */

static void write_status(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    gmtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    FILE *fp = fopen(g_ctrl.status_file, "w");
    if (!fp) return;

    /* state 0 = IDLE, state 1 = STREAMING, state 2 = ERROR */
    const char *state_str = (g_ctrl.current_state == 1) ? "STREAMING" :
                            (g_ctrl.current_state == 2) ? "ERROR" : "IDLE";

    fprintf(fp,
            "{\n"
            "  \"timestamp\": \"%s\",\n"
            "  \"stream_state\": \"%s\",\n"
            "  \"stream_state_id\": %d\n"
            "}\n",
            tbuf, state_str, g_ctrl.current_state);

    fclose(fp);
}

static void on_sys_event(SysEvent evt, void *userdata)
{
    (void)userdata;
    int prev = g_ctrl.current_state;
    if (evt == SYS_EVT_STREAM_STARTED) g_ctrl.current_state = 1;
    else if (evt == SYS_EVT_STREAM_STOPPED) g_ctrl.current_state = 0;
    else if (evt == SYS_EVT_STREAM_ERROR) g_ctrl.current_state = 2;
    
    if (prev != g_ctrl.current_state) write_status();
}

static void dispatch_cmd(const char *cmd)
{
    char buf[BUF_SZ];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';

    LOG_INFO(MOD, "CMD: '%s'", buf);

    if (strcmp(buf, "start") == 0) {
        event_bus_publish(SYS_EVT_CMD_START);
    } else if (strcmp(buf, "stop") == 0) {
        event_bus_publish(SYS_EVT_CMD_STOP);
    } else if (strcmp(buf, "status") == 0) {
        write_status();
    } else {
        LOG_WARN(MOD, "Unknown command: '%s'", buf);
        LOG_INFO(MOD, "Valid commands: start | stop | status");
    }
}

static void *ctrl_thread(void *arg)
{
    (void)arg;
    char line[BUF_SZ];

    while (g_ctrl.running) {
        g_ctrl.fifo_fd = open(g_ctrl.cmd_fifo, O_RDONLY);
        if (g_ctrl.fifo_fd < 0) {
            LOG_ERROR(MOD, "Cannot open FIFO %s: %s", g_ctrl.cmd_fifo, strerror(errno));
            sleep(1);
            continue;
        }

        while (g_ctrl.running) {
            ssize_t n = read(g_ctrl.fifo_fd, line, sizeof(line) - 1);
            if (n <= 0) break;
            line[n] = '\0';

            char *tok = strtok(line, "\n");
            while (tok) {
                if (strlen(tok) > 0) dispatch_cmd(tok);
                tok = strtok(NULL, "\n");
            }
        }
        close(g_ctrl.fifo_fd);
        g_ctrl.fifo_fd = -1;
    }
    return NULL;
}

int control_init(void)
{
    g_ctrl.running = 1;
    g_ctrl.fifo_fd = -1;
    g_ctrl.current_state = 0;
    resolve_runtime_paths();

    if (mkfifo(g_ctrl.cmd_fifo, 0666) < 0 && errno != EEXIST) {
        LOG_ERROR(MOD, "mkfifo(%s) failed: %s", g_ctrl.cmd_fifo, strerror(errno));
        return -1;
    }
    chmod(g_ctrl.cmd_fifo, 0666);

    write_status(); /* initial write */
    event_bus_subscribe(on_sys_event, NULL);

    pthread_create(&g_ctrl.thread, NULL, ctrl_thread, NULL);
    LOG_INFO(MOD, "Control FIFO ready: %s", g_ctrl.cmd_fifo);
    LOG_INFO(MOD, "Status file ready: %s", g_ctrl.status_file);
    return 0;
}

void control_deinit(void)
{
    g_ctrl.running = 0;
    if (g_ctrl.fifo_fd >= 0) close(g_ctrl.fifo_fd);
    int fd = open(g_ctrl.cmd_fifo, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) close(fd);
    pthread_join(g_ctrl.thread, NULL);
    unlink(g_ctrl.cmd_fifo);
    unlink(g_ctrl.status_file);
    LOG_INFO(MOD, "Control FIFO closed");
}
