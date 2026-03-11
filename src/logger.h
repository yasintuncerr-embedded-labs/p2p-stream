#ifndef P2P_LOGGER_H
#define P2P_LOGGER_H

#include <stdarg.h>

/* -----------------------------------------------------------------------
 * Structured logger
 *   - Writes to syslog (journald picks it up)
 *   - Writes to rotating file (/var/log/p2p-stream.log)
 *   - Prefixes: [TIMESTAMP][LEVEL][MODULE]
 * --------------------------------------------------------------------- */

typedef enum {
    P2P_LOG_DEBUG = 0,
    P2P_LOG_INFO,
    P2P_LOG_WARN,
    P2P_LOG_ERROR,
    P2P_LOG_FATAL
} P2pLogLevel;

void logger_init   (const char *ident, const char *log_file, P2pLogLevel min_level);
void logger_deinit (void);
void logger_log    (P2pLogLevel level, const char *module, const char *fmt, ...);
void logger_set_level(P2pLogLevel level);

/* Convenience macros — always pass module tag */
#define LOG_DEBUG(mod, fmt, ...) logger_log(P2P_LOG_DEBUG, mod, fmt, ##__VA_ARGS__)
#define LOG_INFO(mod,  fmt, ...) logger_log(P2P_LOG_INFO,  mod, fmt, ##__VA_ARGS__)
#define LOG_WARN(mod,  fmt, ...) logger_log(P2P_LOG_WARN,  mod, fmt, ##__VA_ARGS__)
#define LOG_ERROR(mod, fmt, ...) logger_log(P2P_LOG_ERROR, mod, fmt, ##__VA_ARGS__)
#define LOG_FATAL(mod, fmt, ...) logger_log(P2P_LOG_FATAL, mod, fmt, ##__VA_ARGS__)

#endif /* P2P_LOGGER_H */
