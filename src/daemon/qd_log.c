/*
 * QDaemon - Logging Implementation
 * Thread-safe logging with file rotation and syslog support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <limits.h>

#include "qdaemon/qd_log.h"

/* Global log state */
static struct {
    qd_log_config_t config;
    FILE *file;
    int initialized;
    pthread_mutex_t lock;
    qd_log_handler_t custom_handler;
    void *custom_handler_arg;
    qd_log_stats_t stats;
    qd_log_context_t *contexts;
    int syslog_open;
    int rate_limit;
    char *buffer;
    size_t buffer_pos;
} g_log = {
    .config = QD_LOG_CONFIG_DEFAULT,
    .file = NULL,
    .initialized = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .custom_handler = NULL,
    .custom_handler_arg = NULL,
    .stats = {{0}},
    .contexts = NULL,
    .syslog_open = 0,
    .rate_limit = 0,
    .buffer = NULL,
    .buffer_pos = 0,
};

/* Level names */
static const char *level_names[] = {
    "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

/* Level colors (ANSI) */
static const char *level_colors[] = {
    "\033[1;31m",  /* FATAL: bold red */
    "\033[0;31m",  /* ERROR: red */
    "\033[0;33m",  /* WARN: yellow */
    "\033[0;32m",  /* INFO: green */
    "\033[0;36m",  /* DEBUG: cyan */
    "\033[0;37m",  /* TRACE: white */
};
static const char *color_reset = "\033[0m";

/* Syslog priority mapping */
static int syslog_priorities[] = {
    LOG_CRIT,    /* FATAL */
    LOG_ERR,     /* ERROR */
    LOG_WARNING, /* WARN */
    LOG_INFO,    /* INFO */
    LOG_DEBUG,   /* DEBUG */
    LOG_DEBUG,   /* TRACE */
};

/* Forward declarations */
static void log_rotate_check(void);
static int log_open_file(void);

int qd_log_init(const qd_log_config_t *config)
{
    pthread_mutex_lock(&g_log.lock);

    if (g_log.initialized) {
        pthread_mutex_unlock(&g_log.lock);
        return QD_ERR_EXIST;
    }

    if (config) {
        memcpy(&g_log.config, config, sizeof(qd_log_config_t));
    }

    /* Allocate buffer */
    if (g_log.config.buffer_size > 0) {
        g_log.buffer = malloc(g_log.config.buffer_size);
        if (!g_log.buffer) {
            pthread_mutex_unlock(&g_log.lock);
            return QD_ERR_NOMEM;
        }
        g_log.buffer_pos = 0;
    }

    /* Open file if target enabled */
    if (g_log.config.targets & QD_LOG_TARGET_FILE) {
        if (log_open_file() != QD_OK) {
            free(g_log.buffer);
            g_log.buffer = NULL;
            pthread_mutex_unlock(&g_log.lock);
            return QD_ERR_IO;
        }
    }

    /* Open syslog if target enabled */
    if (g_log.config.targets & QD_LOG_TARGET_SYSLOG) {
        openlog(g_log.config.syslog_ident,
                LOG_PID | LOG_NDELAY,
                g_log.config.syslog_facility);
        g_log.syslog_open = 1;
    }

    g_log.initialized = 1;
    pthread_mutex_unlock(&g_log.lock);

    return QD_OK;
}

void qd_log_shutdown(void)
{
    pthread_mutex_lock(&g_log.lock);

    if (!g_log.initialized) {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    /* Flush buffer */
    qd_log_flush();

    /* Close file */
    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }

    /* Close syslog */
    if (g_log.syslog_open) {
        closelog();
        g_log.syslog_open = 0;
    }

    /* Free buffer */
    if (g_log.buffer) {
        free(g_log.buffer);
        g_log.buffer = NULL;
    }

    /* Free contexts */
    qd_log_context_t *ctx = g_log.contexts;
    while (ctx) {
        qd_log_context_t *next = ctx->next;
        free(ctx);
        ctx = next;
    }
    g_log.contexts = NULL;

    g_log.initialized = 0;
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_set_level(qd_log_level_t level)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.config.level = level;
    pthread_mutex_unlock(&g_log.lock);
}

qd_log_level_t qd_log_get_level(void)
{
    return g_log.config.level;
}

void qd_log_set_targets(uint32_t targets)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.config.targets = targets;
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_set_flags(uint32_t flags)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.config.flags = flags;
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_set_handler(qd_log_handler_t handler, void *arg)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.custom_handler = handler;
    g_log.custom_handler_arg = arg;
    pthread_mutex_unlock(&g_log.lock);
}

int qd_log_enabled(qd_log_level_t level)
{
    return level <= g_log.config.level;
}

static int log_open_file(void)
{
    if (!g_log.config.file_path)
        return QD_ERR_INVAL;

    g_log.file = fopen(g_log.config.file_path, "a");
    if (!g_log.file)
        return QD_ERR_IO;

    /* Set close-on-exec */
    int fd = fileno(g_log.file);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    return QD_OK;
}

static void log_rotate_check(void)
{
    if (!g_log.file || g_log.config.rotation.max_size == 0)
        return;

    struct stat st;
    if (fstat(fileno(g_log.file), &st) != 0)
        return;

    if ((size_t)st.st_size < g_log.config.rotation.max_size)
        return;

    /* Rotate files */
    qd_log_rotate();
}

int qd_log_rotate(void)
{
    pthread_mutex_lock(&g_log.lock);

    if (!g_log.file || !g_log.config.file_path) {
        pthread_mutex_unlock(&g_log.lock);
        return QD_ERR_INVAL;
    }

    /* Close current file */
    fclose(g_log.file);
    g_log.file = NULL;

    /* Rotate existing files */
    int max_files = g_log.config.rotation.max_files;
    if (max_files <= 0)
        max_files = 5;

    char old_path[PATH_MAX];
    char new_path[PATH_MAX];

    /* Remove oldest file */
    snprintf(old_path, sizeof(old_path), "%s.%d",
             g_log.config.file_path, max_files);
    unlink(old_path);

    /* Shift existing files */
    for (int i = max_files - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d",
                 g_log.config.file_path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d",
                 g_log.config.file_path, i + 1);
        rename(old_path, new_path);
    }

    /* Move current to .1 */
    snprintf(new_path, sizeof(new_path), "%s.1", g_log.config.file_path);
    rename(g_log.config.file_path, new_path);

    /* Reopen file */
    int ret = log_open_file();

    g_log.stats.rotations++;

    pthread_mutex_unlock(&g_log.lock);
    return ret;
}

int qd_log_reopen(void)
{
    pthread_mutex_lock(&g_log.lock);

    if (g_log.file) {
        fclose(g_log.file);
        g_log.file = NULL;
    }

    int ret = log_open_file();

    pthread_mutex_unlock(&g_log.lock);
    return ret;
}

void qd_log_flush(void)
{
    pthread_mutex_lock(&g_log.lock);

    /* Flush buffer to file */
    if (g_log.buffer && g_log.buffer_pos > 0 && g_log.file) {
        fwrite(g_log.buffer, 1, g_log.buffer_pos, g_log.file);
        g_log.buffer_pos = 0;
    }

    if (g_log.file)
        fflush(g_log.file);

    pthread_mutex_unlock(&g_log.lock);
}

static void format_timestamp(char *buf, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             tv.tv_usec / 1000);
}

static void log_output(qd_log_level_t level, const char *file, int line,
                       const char *func, const char *msg)
{
    uint32_t flags = g_log.config.flags;
    char prefix[512] = "";
    char *p = prefix;
    int rem = sizeof(prefix);
    int n;

    /* Build prefix */
    if (flags & QD_LOG_FLAG_TIMESTAMP) {
        char ts[64];
        format_timestamp(ts, sizeof(ts));
        n = snprintf(p, rem, "[%s] ", ts);
        p += n; rem -= n;
    }

    if (flags & QD_LOG_FLAG_LEVEL) {
        n = snprintf(p, rem, "[%-5s] ", level_names[level]);
        p += n; rem -= n;
    }

    if (flags & QD_LOG_FLAG_PID) {
        n = snprintf(p, rem, "[%d] ", getpid());
        p += n; rem -= n;
    }

    if (flags & QD_LOG_FLAG_TID) {
        n = snprintf(p, rem, "[%ld] ", (long)pthread_self());
        p += n; rem -= n;
    }

    if ((flags & QD_LOG_FLAG_FILE) && file) {
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;
        if (flags & QD_LOG_FLAG_LINE) {
            n = snprintf(p, rem, "[%s:%d] ", basename, line);
        } else {
            n = snprintf(p, rem, "[%s] ", basename);
        }
        p += n; rem -= n;
    }

    if ((flags & QD_LOG_FLAG_FUNC) && func) {
        n = snprintf(p, rem, "[%s] ", func);
        p += n; rem -= n;
    }

    /* Output to stderr */
    if (g_log.config.targets & QD_LOG_TARGET_STDERR) {
        int use_color = (flags & QD_LOG_FLAG_COLOR) && isatty(STDERR_FILENO);
        if (use_color) {
            fprintf(stderr, "%s%s%s%s\n", level_colors[level], prefix, msg, color_reset);
        } else {
            fprintf(stderr, "%s%s\n", prefix, msg);
        }
    }

    /* Output to file */
    if ((g_log.config.targets & QD_LOG_TARGET_FILE) && g_log.file) {
        if (g_log.buffer && g_log.config.buffer_size > 0) {
            size_t prefix_len = strlen(prefix);
            size_t msg_len = strlen(msg);
            size_t total = prefix_len + msg_len + 1;

            if (g_log.buffer_pos + total > g_log.config.buffer_size) {
                fwrite(g_log.buffer, 1, g_log.buffer_pos, g_log.file);
                g_log.buffer_pos = 0;
                g_log.stats.bytes_written += g_log.buffer_pos;
            }

            if (total < g_log.config.buffer_size) {
                memcpy(g_log.buffer + g_log.buffer_pos, prefix, prefix_len);
                g_log.buffer_pos += prefix_len;
                memcpy(g_log.buffer + g_log.buffer_pos, msg, msg_len);
                g_log.buffer_pos += msg_len;
                g_log.buffer[g_log.buffer_pos++] = '\n';
            }
        } else {
            fprintf(g_log.file, "%s%s\n", prefix, msg);
            g_log.stats.bytes_written += strlen(prefix) + strlen(msg) + 1;
        }

        log_rotate_check();
    }

    /* Output to syslog */
    if ((g_log.config.targets & QD_LOG_TARGET_SYSLOG) && g_log.syslog_open) {
        syslog(syslog_priorities[level], "%s", msg);
    }

    /* Custom handler */
    if ((g_log.config.targets & QD_LOG_TARGET_CUSTOM) && g_log.custom_handler) {
        g_log.custom_handler(level, file, line, func, msg, g_log.custom_handler_arg);
    }

    /* Update stats */
    if (level < 6)
        g_log.stats.messages[level]++;
}

void qd_log(qd_log_level_t level, const char *file, int line, const char *func,
            const char *fmt, ...)
{
    if (!qd_log_enabled(level))
        return;

    va_list args;
    va_start(args, fmt);
    qd_logv(level, file, line, func, fmt, args);
    va_end(args);
}

void qd_logv(qd_log_level_t level, const char *file, int line, const char *func,
             const char *fmt, va_list args)
{
    if (!qd_log_enabled(level))
        return;

    char msg[4096];
    vsnprintf(msg, sizeof(msg), fmt, args);

    pthread_mutex_lock(&g_log.lock);
    log_output(level, file, line, func, msg);
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_raw(qd_log_level_t level, const char *msg)
{
    if (!qd_log_enabled(level))
        return;

    pthread_mutex_lock(&g_log.lock);
    log_output(level, NULL, 0, NULL, msg);
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_hexdump(qd_log_level_t level, const char *prefix,
                    const void *data, size_t len)
{
    if (!qd_log_enabled(level))
        return;

    const uint8_t *bytes = data;
    char line[128];

    for (size_t i = 0; i < len; i += 16) {
        char *p = line;
        int rem = sizeof(line);
        int n;

        n = snprintf(p, rem, "%s%04zx: ", prefix ? prefix : "", i);
        p += n; rem -= n;

        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                n = snprintf(p, rem, "%02x ", bytes[i + j]);
            } else {
                n = snprintf(p, rem, "   ");
            }
            p += n; rem -= n;
        }

        n = snprintf(p, rem, " ");
        p += n; rem -= n;

        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = bytes[i + j];
            *p++ = (c >= 32 && c < 127) ? c : '.';
            rem--;
        }
        *p = '\0';

        qd_log_raw(level, line);
    }
}

const char *qd_log_level_name(qd_log_level_t level)
{
    if (level < sizeof(level_names) / sizeof(level_names[0]))
        return level_names[level];
    return "UNKNOWN";
}

qd_log_level_t qd_log_level_from_name(const char *name)
{
    if (!name)
        return QD_LOG_INFO;

    for (int i = 0; i < (int)(sizeof(level_names) / sizeof(level_names[0])); i++) {
        if (strcasecmp(name, level_names[i]) == 0)
            return (qd_log_level_t)i;
    }

    return QD_LOG_INFO;
}

const char *qd_log_get_file(void)
{
    return g_log.config.file_path;
}

void qd_log_stats(qd_log_stats_t *stats)
{
    if (!stats)
        return;

    pthread_mutex_lock(&g_log.lock);
    memcpy(stats, &g_log.stats, sizeof(qd_log_stats_t));
    pthread_mutex_unlock(&g_log.lock);
}

void qd_log_set_rate_limit(int max_per_second)
{
    pthread_mutex_lock(&g_log.lock);
    g_log.rate_limit = max_per_second;
    pthread_mutex_unlock(&g_log.lock);
}

/*
 * Per-Module Logging
 */

qd_log_context_t *qd_log_context_create(const char *name)
{
    qd_log_context_t *ctx = calloc(1, sizeof(qd_log_context_t));
    if (!ctx)
        return NULL;

    strncpy(ctx->name, name, sizeof(ctx->name) - 1);
    ctx->level = g_log.config.level;
    ctx->flags = g_log.config.flags;

    pthread_mutex_lock(&g_log.lock);
    ctx->next = g_log.contexts;
    g_log.contexts = ctx;
    pthread_mutex_unlock(&g_log.lock);

    return ctx;
}

void qd_log_context_destroy(qd_log_context_t *ctx)
{
    if (!ctx)
        return;

    pthread_mutex_lock(&g_log.lock);

    qd_log_context_t **pp = &g_log.contexts;
    while (*pp) {
        if (*pp == ctx) {
            *pp = ctx->next;
            break;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&g_log.lock);

    free(ctx);
}

void qd_log_context_set_level(qd_log_context_t *ctx, qd_log_level_t level)
{
    if (ctx)
        ctx->level = level;
}

void qd_log_ctx(qd_log_context_t *ctx, qd_log_level_t level,
                const char *file, int line, const char *func,
                const char *fmt, ...)
{
    if (!ctx || level > ctx->level)
        return;

    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char prefixed[4200];
    snprintf(prefixed, sizeof(prefixed), "[%s] %s", ctx->name, msg);

    pthread_mutex_lock(&g_log.lock);
    log_output(level, file, line, func, prefixed);
    pthread_mutex_unlock(&g_log.lock);
}
