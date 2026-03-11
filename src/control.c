#include "control.h"
#include "logger.h"
#include "state_machine.h"

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
    StreamSM        *sm;
    pthread_t        thread;
    int              running;
    int              fifo_fd;
    char             cmd_fifo[128];
    char             status_file[128];
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
    SmState st = sm_get_state(g_ctrl.sm);

    /* Get current time */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    gmtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    FILE *fp = fopen(g_ctrl.status_file, "w");
    if (!fp) return;

    fprintf(fp,
            "{\n"
            "  \"timestamp\": \"%s\",\n"
            "  \"stream_state\": \"%s\",\n"
            "  \"stream_state_id\": %d\n"
            "}\n",
            tbuf,
            sm_state_name(st),
            (int)st);

    fclose(fp);
}


/* -----------------------------------------------------------------------
 * Command dispatcher
 * --------------------------------------------------------------------- */
static void dispatch_cmd(const char *cmd)
{
    /* strip trailing whitespace/newline */
    char buf[BUF_SZ];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';

    LOG_INFO(MOD, "CMD: '%s'", buf);

    if (strcmp(buf, "start") == 0) {
        sm_post_event(g_ctrl.sm, SM_EVT_START);
    } else if (strcmp(buf, "stop") == 0) {
        sm_post_event(g_ctrl.sm, SM_EVT_STOP);
    } else if (strcmp(buf, "status") == 0) {
        write_status();
    } else {
        LOG_WARN(MOD, "Unknown command: '%s'", buf);
        LOG_INFO(MOD, "Valid commands: start | stop | status");
    }
}


/* -----------------------------------------------------------------------
 * FIFO reader thread
 * --------------------------------------------------------------------- */
static void *ctrl_thread(void *arg)
{
    (void)arg;
    char line[BUF_SZ];

    while (g_ctrl.running) {
        /* Re-open FIFO each iteration (readers block until writer opens) */
        g_ctrl.fifo_fd = open(g_ctrl.cmd_fifo, O_RDONLY);
        if (g_ctrl.fifo_fd < 0) {
            LOG_ERROR(MOD, "Cannot open FIFO %s: %s", g_ctrl.cmd_fifo, strerror(errno));
            sleep(1);
            continue;
        }

        while (g_ctrl.running) {
            ssize_t n = read(g_ctrl.fifo_fd, line, sizeof(line) - 1);
            if (n <= 0) break;  /* EOF = writer closed, reopen */
            line[n] = '\0';

            /* Handle multiple commands in one write */
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



/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
int control_init(StreamSM *sm)
{
    g_ctrl.sm      = sm;
    g_ctrl.running = 1;
    g_ctrl.fifo_fd = -1;
    resolve_runtime_paths();

    /* Create FIFO if not exists */
    if (mkfifo(g_ctrl.cmd_fifo, 0666) < 0 && errno != EEXIST) {
        LOG_ERROR(MOD, "mkfifo(%s) failed: %s", g_ctrl.cmd_fifo, strerror(errno));
        return -1;
    }
    chmod(g_ctrl.cmd_fifo, 0666);

    /* Initial status */
    write_status();

    pthread_create(&g_ctrl.thread, NULL, ctrl_thread, NULL);
    LOG_INFO(MOD, "Control FIFO ready: %s", g_ctrl.cmd_fifo);
    LOG_INFO(MOD, "Status file: %s", g_ctrl.status_file);
    return 0;
}

void control_deinit(void)
{
    g_ctrl.running = 0;
    if (g_ctrl.fifo_fd >= 0) close(g_ctrl.fifo_fd);
    /* Unblock thread by opening the FIFO briefly */
    int fd = open(g_ctrl.cmd_fifo, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) close(fd);
    pthread_join(g_ctrl.thread, NULL);
    unlink(g_ctrl.cmd_fifo);
    unlink(g_ctrl.status_file);
    LOG_INFO(MOD, "Control FIFO closed");
}
