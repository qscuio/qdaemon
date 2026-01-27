/*
 * QDaemon Stub Header
 * Minimal declarations for standalone building/testing
 */

#ifndef QDAEMON_STUB_H
#define QDAEMON_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define QD_OK 0
#define QD_ERROR -1
#define QD_HANDLER_OK 0
#define QD_HANDLER_ERROR -1

/* Event flags */
#define QD_EVENT_READ 1
#define QD_EVENT_WRITE 2

/* Log levels */
#define QD_LOG_DEBUG 0
#define QD_LOG_INFO 1
#define QD_LOG_WARN 2
#define QD_LOG_ERROR 3

/* Opaque types */
typedef struct qd_daemon qd_daemon_t;
typedef struct qd_event_loop qd_event_loop_t;
typedef struct qd_netlink qd_netlink_t;
typedef struct qd_handler_table qd_handler_table_t;

/* Netlink message - needs to be defined for local variable usage */
typedef struct qd_netlink_msg {
    int cmd;
    uint8_t data[4096];
    size_t data_len;
} qd_netlink_msg_t;

typedef int qd_handler_result_t;
typedef void (*qd_event_cb_t)(int fd, uint32_t events, void *arg);

/* Handler context - must be defined before handler_entry */
typedef struct qd_handler_ctx {
    int cmd;
    void *data;
    size_t len;
    int seq;
    int status;
    void *response;
    size_t response_len;
    void *user_data;
} qd_handler_ctx_t;

/* Handler entry */
typedef struct qd_handler_entry {
    int cmd;
    qd_handler_result_t (*handler)(qd_handler_ctx_t *, void *);
    void *arg;
} qd_handler_entry_t;

/* Daemon configuration */
typedef struct {
    const char *name;
    const char *version;
    const char *description;
    const char *pid_file;
    int foreground;
    int log_to_stderr;
    int log_level;
} qd_daemon_config_t;

#define QD_DAEMON_CONFIG_DEFAULT { \
    .name = "daemon", \
    .version = "1.0.0", \
    .description = "QDaemon", \
    .pid_file = "/tmp/daemon.pid", \
    .foreground = 0, \
    .log_to_stderr = 1, \
    .log_level = QD_LOG_INFO \
}

/* Handler macros */
#define QD_HANDLER(cmd, fn) { cmd, fn, NULL }
#define QD_HANDLER_END() { 0, NULL, NULL }

/* Logging */
void qd_log(int level, const char *fmt, ...);
#define qd_log_debug(...) qd_log(QD_LOG_DEBUG, __VA_ARGS__)
#define qd_log_info(...)  qd_log(QD_LOG_INFO, __VA_ARGS__)
#define qd_log_warn(...)  qd_log(QD_LOG_WARN, __VA_ARGS__)
#define qd_log_error(...) qd_log(QD_LOG_ERROR, __VA_ARGS__)

/* Time */
static inline uint64_t qd_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Event loop */
qd_event_loop_t *qd_event_loop_create(void);
void qd_event_loop_destroy(qd_event_loop_t *loop);
int qd_event_add(qd_event_loop_t *loop, int fd, uint32_t events,
                 qd_event_cb_t cb, void *arg);
int qd_event_del_fd(qd_event_loop_t *loop, int fd);
void qd_event_loop_run(qd_event_loop_t *loop);
void qd_event_loop_stop(qd_event_loop_t *loop);

/* Daemon */
qd_daemon_t *qd_daemon_create(const qd_daemon_config_t *config);
void qd_daemon_destroy(qd_daemon_t *d);
void qd_daemon_set_init_callback(qd_daemon_t *d,
    int (*cb)(qd_daemon_t *, void *), void *arg);
void qd_daemon_set_shutdown_callback(qd_daemon_t *d,
    void (*cb)(qd_daemon_t *, void *), void *arg);
qd_event_loop_t *qd_daemon_get_loop(qd_daemon_t *d);
int qd_daemon_init(qd_daemon_t *d);
void qd_daemon_run(qd_daemon_t *d);

/* Handler */
qd_handler_table_t *qd_handler_table_create(const char *name);
void qd_handler_table_destroy(qd_handler_table_t *t);
void qd_handler_register_table(qd_handler_table_t *t,
                               const qd_handler_entry_t *entries);
int qd_handler_register_fn(qd_handler_table_t *t, int cmd,
                           qd_handler_result_t (*handler)(qd_handler_ctx_t *, void *),
                           void *arg, int flags);
void qd_handler_ctx_init(qd_handler_ctx_t *ctx);
void *qd_handler_ctx_alloc_response(qd_handler_ctx_t *ctx, size_t size);
int qd_handler_dispatch(qd_handler_table_t *t, qd_handler_ctx_t *ctx);

/* Netlink (stub - returns NULL) */
qd_netlink_t *qd_netlink_create(const char *family);
void qd_netlink_destroy(qd_netlink_t *nl);
void qd_netlink_set_callback(qd_netlink_t *nl, void *cb, void *arg);
void qd_netlink_msg_init(qd_netlink_msg_t *msg, int cmd);
int qd_netlink_msg_add_attr(qd_netlink_msg_t *msg, int type,
                             const void *data, size_t len);
int qd_netlink_send_msg(qd_netlink_t *nl, qd_netlink_msg_t *msg);
void *qd_netlink_msg_get_data(qd_netlink_msg_t *msg, int type, size_t *len);

/* CLI types and functions */
typedef struct qd_cli_shell qd_cli_shell_t;
typedef struct qd_cli_mode qd_cli_mode_t;

typedef struct qd_cli_ctx {
    qd_cli_shell_t *shell;
    qd_cli_mode_t *mode;
    int argc;
    char **argv;
    void *user_data;
    void (*print)(const char *fmt, ...);
    void (*error)(const char *fmt, ...);
} qd_cli_ctx_t;

typedef int qd_cli_result_t;

typedef struct {
    const char *name;
    const char *version;
    const char *prompt;
    const char *base_prompt;
    const char *history_file;
    int history_size;
} qd_cli_config_t;

#define QD_CLI_CONFIG_DEFAULT { \
    .name = "cli", \
    .version = "1.0", \
    .prompt = "> ", \
    .base_prompt = "", \
    .history_file = NULL, \
    .history_size = 100 \
}

#define QD_CLI_OK 0
#define QD_CLI_ERROR -1
#define QD_CLI_MODE_CHANGE 1

qd_cli_shell_t *qd_cli_create(const qd_cli_config_t *config);
void qd_cli_destroy(qd_cli_shell_t *shell);
qd_cli_mode_t *qd_cli_mode_create(qd_cli_shell_t *shell, const char *name,
                                  const char *prompt, const char *help);
void qd_cli_mode_set_parent(qd_cli_mode_t *mode, qd_cli_mode_t *parent);
qd_cli_mode_t *qd_cli_get_root_mode(qd_cli_shell_t *shell);
int qd_cli_register_cmd(qd_cli_mode_t *mode, const char *name,
                        qd_cli_result_t (*handler)(qd_cli_ctx_t *),
                        const char *help);
void qd_cli_enter_mode(qd_cli_shell_t *shell, qd_cli_mode_t *mode);
void qd_cli_exit_mode(qd_cli_shell_t *shell);
int qd_cli_run(qd_cli_shell_t *shell);

#ifdef __cplusplus
}
#endif

#endif /* QDAEMON_STUB_H */
