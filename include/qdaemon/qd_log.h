/*
 * QDaemon - Logging API
 * Flexible logging with file rotation and syslog support
 */

#ifndef QD_LOG_H
#define QD_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Log levels */
typedef enum {
    QD_LOG_FATAL = 0,    /* System is unusable */
    QD_LOG_ERROR = 1,    /* Error conditions */
    QD_LOG_WARN  = 2,    /* Warning conditions */
    QD_LOG_INFO  = 3,    /* Informational */
    QD_LOG_DEBUG = 4,    /* Debug messages */
    QD_LOG_TRACE = 5,    /* Trace messages */
} qd_log_level_t;

/* Log output targets */
typedef enum {
    QD_LOG_TARGET_NONE   = 0x00,
    QD_LOG_TARGET_STDERR = 0x01,
    QD_LOG_TARGET_FILE   = 0x02,
    QD_LOG_TARGET_SYSLOG = 0x04,
    QD_LOG_TARGET_CUSTOM = 0x08,
} qd_log_target_t;

/* Log flags */
typedef enum {
    QD_LOG_FLAG_NONE      = 0x00,
    QD_LOG_FLAG_TIMESTAMP = 0x01,  /* Include timestamp */
    QD_LOG_FLAG_LEVEL     = 0x02,  /* Include log level */
    QD_LOG_FLAG_FILE      = 0x04,  /* Include file name */
    QD_LOG_FLAG_LINE      = 0x08,  /* Include line number */
    QD_LOG_FLAG_FUNC      = 0x10,  /* Include function name */
    QD_LOG_FLAG_PID       = 0x20,  /* Include process ID */
    QD_LOG_FLAG_TID       = 0x40,  /* Include thread ID */
    QD_LOG_FLAG_COLOR     = 0x80,  /* Use colors (stderr only) */
    QD_LOG_FLAG_DEFAULT   = (QD_LOG_FLAG_TIMESTAMP | QD_LOG_FLAG_LEVEL),
} qd_log_flags_t;

/* Log rotation settings */
typedef struct qd_log_rotation {
    size_t max_size;         /* Max file size before rotation (0 = no limit) */
    int max_files;           /* Max number of rotated files (0 = no limit) */
    int daily;               /* Rotate daily */
    int compress;            /* Compress rotated files */
} qd_log_rotation_t;

/* Log configuration */
typedef struct qd_log_config {
    qd_log_level_t level;    /* Minimum log level */
    uint32_t targets;        /* Output targets (bitmask) */
    uint32_t flags;          /* Log flags (bitmask) */
    const char *file_path;   /* Log file path */
    const char *syslog_ident; /* Syslog identifier */
    int syslog_facility;     /* Syslog facility */
    qd_log_rotation_t rotation; /* Rotation settings */
    size_t buffer_size;      /* Output buffer size */
} qd_log_config_t;

/* Default configuration */
#define QD_LOG_CONFIG_DEFAULT { \
    .level = QD_LOG_INFO, \
    .targets = QD_LOG_TARGET_STDERR, \
    .flags = QD_LOG_FLAG_DEFAULT, \
    .file_path = NULL, \
    .syslog_ident = "qdaemon", \
    .syslog_facility = 1, /* LOG_USER */ \
    .rotation = { .max_size = 10 * 1024 * 1024, .max_files = 5, .daily = 0, .compress = 0 }, \
    .buffer_size = 4096 \
}

/* Custom log handler */
typedef void (*qd_log_handler_t)(qd_log_level_t level, const char *file, int line,
                                  const char *func, const char *msg, void *arg);

/* Log context (for per-module logging) */
typedef struct qd_log_context {
    char name[32];
    qd_log_level_t level;
    uint32_t flags;
    struct qd_log_context *next;
} qd_log_context_t;

/*
 * Global Log API
 */

/* Initialize logging system */
int qd_log_init(const qd_log_config_t *config);

/* Shutdown logging system */
void qd_log_shutdown(void);

/* Set global log level */
void qd_log_set_level(qd_log_level_t level);

/* Get global log level */
qd_log_level_t qd_log_get_level(void);

/* Set log targets */
void qd_log_set_targets(uint32_t targets);

/* Set log flags */
void qd_log_set_flags(uint32_t flags);

/* Set custom handler */
void qd_log_set_handler(qd_log_handler_t handler, void *arg);

/* Check if level is enabled */
int qd_log_enabled(qd_log_level_t level);

/*
 * Logging Functions
 */

/* Main log function */
void qd_log(qd_log_level_t level, const char *file, int line, const char *func,
            const char *fmt, ...) QD_PRINTF(5, 6);

/* Variable argument list version */
void qd_logv(qd_log_level_t level, const char *file, int line, const char *func,
             const char *fmt, va_list args);

/* Raw log (no formatting) */
void qd_log_raw(qd_log_level_t level, const char *msg);

/* Hex dump */
void qd_log_hexdump(qd_log_level_t level, const char *prefix,
                    const void *data, size_t len);

/*
 * Logging Macros
 */

#define qd_log_fatal(...) \
    qd_log(QD_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define qd_log_error(...) \
    qd_log(QD_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define qd_log_warn(...) \
    qd_log(QD_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define qd_log_info(...) \
    qd_log(QD_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define qd_log_debug(...) \
    qd_log(QD_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define qd_log_trace(...) \
    qd_log(QD_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Conditional logging */
#define qd_log_if(cond, level, ...) \
    do { if (cond) qd_log(level, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)

/* Log with errno */
#define qd_log_errno(level, msg) \
    qd_log(level, __FILE__, __LINE__, __func__, "%s: %s", msg, strerror(errno))

#define qd_log_perror(msg) qd_log_errno(QD_LOG_ERROR, msg)

/*
 * Per-Module Logging
 */

/* Create log context for a module */
qd_log_context_t *qd_log_context_create(const char *name);

/* Destroy log context */
void qd_log_context_destroy(qd_log_context_t *ctx);

/* Set context level */
void qd_log_context_set_level(qd_log_context_t *ctx, qd_log_level_t level);

/* Log with context */
void qd_log_ctx(qd_log_context_t *ctx, qd_log_level_t level,
                const char *file, int line, const char *func,
                const char *fmt, ...) QD_PRINTF(6, 7);

/* Context logging macros */
#define qd_ctx_log_error(ctx, ...) \
    qd_log_ctx(ctx, QD_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define qd_ctx_log_warn(ctx, ...) \
    qd_log_ctx(ctx, QD_LOG_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define qd_ctx_log_info(ctx, ...) \
    qd_log_ctx(ctx, QD_LOG_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define qd_ctx_log_debug(ctx, ...) \
    qd_log_ctx(ctx, QD_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

/*
 * File Management
 */

/* Force log rotation */
int qd_log_rotate(void);

/* Reopen log file (for log rotation by external tools) */
int qd_log_reopen(void);

/* Flush log buffers */
void qd_log_flush(void);

/*
 * Utility Functions
 */

/* Get level name */
const char *qd_log_level_name(qd_log_level_t level);

/* Parse level from string */
qd_log_level_t qd_log_level_from_name(const char *name);

/* Get current log file path */
const char *qd_log_get_file(void);

/*
 * Statistics
 */

typedef struct qd_log_stats {
    uint64_t messages[6];    /* Messages per level */
    uint64_t bytes_written;
    uint64_t rotations;
    uint64_t drops;          /* Dropped due to rate limiting */
} qd_log_stats_t;

void qd_log_stats(qd_log_stats_t *stats);

/*
 * Rate Limiting
 */

/* Enable rate limiting */
void qd_log_set_rate_limit(int max_per_second);

/* Rate-limited log macro */
#define qd_log_rate_limited(level, ...) \
    do { \
        static qd_time_t __last_log = 0; \
        static int __count = 0; \
        qd_time_t __now = qd_time_now(); \
        if (__now - __last_log >= 1000) { \
            if (__count > 1) { \
                qd_log(level, __FILE__, __LINE__, __func__, \
                       "(repeated %d times)", __count); \
            } \
            qd_log(level, __FILE__, __LINE__, __func__, __VA_ARGS__); \
            __last_log = __now; \
            __count = 0; \
        } else { \
            __count++; \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* QD_LOG_H */
