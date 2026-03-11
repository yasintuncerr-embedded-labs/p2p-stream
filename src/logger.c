#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <pthread.h>
#include <errno.h>

#define LOG_FILE_MAX_BYTES  (5 * 1024 * 1024)   /* 5 MB per file  */
#define LOG_FILE_ROTATE_MAX  3                   /* keep 3 backups */

static struct {
    FILE       *fp;
    char        path[256];
    P2pLogLevel min_level;
    pthread_mutex_t lock;
    int         initialized;
} g_log;

static const char *level_str[] = {
    "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static int syslog_prio[] = {
    LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT
};

/* -----------------------------------------------------------------------
 * Rotate: rename .log → .log.1 → .log.2 → .log.3 (drop oldest)
 * --------------------------------------------------------------------- */
static void rotate_if_needed(void)
{
    if (!g_log.fp) return;

    long pos = ftell(g_log.fp);
    if (pos < LOG_FILE_MAX_BYTES) return;

    fclose(g_log.fp);
    g_log.fp = NULL;

    char old_name[280], new_name[280];
    for (int i = LOG_FILE_ROTATE_MAX - 1; i >= 1; i--) {
        snprintf(old_name, sizeof(old_name), "%s.%d", g_log.path, i);
        snprintf(new_name, sizeof(new_name), "%s.%d", g_log.path, i + 1);
        rename(old_name, new_name);
    }
    snprintf(old_name, sizeof(old_name), "%s", g_log.path);
    snprintf(new_name, sizeof(new_name), "%s.1", g_log.path);
    rename(old_name, new_name);

    g_log.fp = fopen(g_log.path, "a");
}

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */
void logger_init(const char *ident, const char *log_file, P2pLogLevel min_level)
{
    pthread_mutex_init(&g_log.lock, NULL);
    g_log.min_level   = min_level;
    g_log.initialized = 1;

    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (log_file) {
        strncpy(g_log.path, log_file, sizeof(g_log.path) - 1);
        g_log.fp = fopen(log_file, "a");
        if (!g_log.fp) {
            const char *fallback = "/tmp/p2p-stream.log";
            syslog(LOG_WARNING,
                   "logger: cannot open %s (%s), trying fallback %s",
                   log_file, strerror(errno), fallback);

            strncpy(g_log.path, fallback, sizeof(g_log.path) - 1);
            g_log.path[sizeof(g_log.path) - 1] = '\0';
            g_log.fp = fopen(fallback, "a");
            if (!g_log.fp) {
                syslog(LOG_WARNING,
                       "logger: cannot open fallback %s (%s), file logging disabled",
                       fallback, strerror(errno));
            }
        }
    }
}

void logger_deinit(void)
{
    if (!g_log.initialized) return;
    if (g_log.fp) { fclose(g_log.fp); g_log.fp = NULL; }
    closelog();
    pthread_mutex_destroy(&g_log.lock);
    g_log.initialized = 0;
}

void logger_set_level(P2pLogLevel level)
{
    g_log.min_level = level;
}

void logger_log(P2pLogLevel level, const char *module, const char *fmt, ...)
{
    if (!g_log.initialized || level < g_log.min_level) return;

    va_list ap;
    char    msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* syslog — journald picks this up with SYSLOG_IDENTIFIER field */
    syslog(syslog_prio[level], "[%s] %s", module, msg);

    /* file log */
    pthread_mutex_lock(&g_log.lock);
    if (g_log.fp) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_info;
        gmtime_r(&ts.tv_sec, &tm_info);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);

        fprintf(g_log.fp, "[%s.%03ldZ][%s][%-6s] %s\n",
                tbuf, ts.tv_nsec / 1000000,
                level_str[level], module, msg);
        fflush(g_log.fp);
        rotate_if_needed();
    }
    pthread_mutex_unlock(&g_log.lock);
}
