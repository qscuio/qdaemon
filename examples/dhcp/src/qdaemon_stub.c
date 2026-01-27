/*
 * QDaemon Stub Library
 * Minimal implementations for standalone building/testing
 * Replace with real libqdaemon for production use
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <time.h>

/* Forward declarations */
typedef struct qd_daemon qd_daemon_t;
typedef struct qd_event_loop qd_event_loop_t;
typedef struct qd_netlink qd_netlink_t;
typedef struct qd_netlink_msg qd_netlink_msg_t;
typedef struct qd_handler_table qd_handler_table_t;
typedef struct qd_handler_ctx qd_handler_ctx_t;
typedef struct qd_handler_entry qd_handler_entry_t;

typedef int qd_handler_result_t;

#define QD_OK 0
#define QD_ERROR -1
#define QD_HANDLER_OK 0
#define QD_HANDLER_ERROR -1
#define QD_EVENT_READ 1
#define QD_LOG_DEBUG 0
#define QD_LOG_INFO 1
#define QD_LOG_WARN 2
#define QD_LOG_ERROR 3

/* Daemon context */
struct qd_daemon {
    const char *name;
    int (*init_cb)(qd_daemon_t *, void *);
    void (*shutdown_cb)(qd_daemon_t *, void *);
    void *init_arg;
    void *shutdown_arg;
    qd_event_loop_t *loop;
    volatile int running;
};

/* Event loop */
typedef void (*qd_event_cb_t)(int fd, uint32_t events, void *arg);

struct qd_event_entry {
    int fd;
    qd_event_cb_t cb;
    void *arg;
};

struct qd_event_loop {
    int epoll_fd;
    struct qd_event_entry entries[256];
    int num_entries;
    volatile int running;
};

/* Handler context */
struct qd_handler_ctx {
    int cmd;
    void *data;
    size_t len;
    int seq;
    int status;
    void *response;
    size_t response_len;
};

struct qd_handler_entry {
    int cmd;
    qd_handler_result_t (*handler)(qd_handler_ctx_t *, void *);
    void *arg;
};

struct qd_handler_table {
    const char *name;
    const qd_handler_entry_t *entries;
};

/* Netlink stub */
struct qd_netlink {
    const char *family;
    int fd;
};

struct qd_netlink_msg {
    int cmd;
    uint8_t data[4096];
    size_t data_len;
};

/* Daemon config */
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

static volatile int g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* Logging */
void qd_log(int level, const char *fmt, ...)
{
    const char *level_str[] = { "DEBUG", "INFO", "WARN", "ERROR" };
    va_list ap;
    fprintf(stderr, "[%s] ", level_str[level]);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

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
qd_event_loop_t *qd_event_loop_create(void)
{
    qd_event_loop_t *loop = calloc(1, sizeof(*loop));
    if (!loop) return NULL;
    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd < 0) {
        free(loop);
        return NULL;
    }
    loop->running = 1;
    return loop;
}

void qd_event_loop_destroy(qd_event_loop_t *loop)
{
    if (loop) {
        close(loop->epoll_fd);
        free(loop);
    }
}

int qd_event_add(qd_event_loop_t *loop, int fd, uint32_t events,
                 qd_event_cb_t cb, void *arg)
{
    if (!loop || loop->num_entries >= 256) return -1;

    struct epoll_event ev = {
        .events = (events & QD_EVENT_READ) ? EPOLLIN : 0,
        .data.fd = fd
    };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return -1;

    loop->entries[loop->num_entries].fd = fd;
    loop->entries[loop->num_entries].cb = cb;
    loop->entries[loop->num_entries].arg = arg;
    loop->num_entries++;
    return 0;
}

int qd_event_del_fd(qd_event_loop_t *loop, int fd)
{
    if (!loop) return -1;
    epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    for (int i = 0; i < loop->num_entries; i++) {
        if (loop->entries[i].fd == fd) {
            memmove(&loop->entries[i], &loop->entries[i + 1],
                    (loop->num_entries - i - 1) * sizeof(loop->entries[0]));
            loop->num_entries--;
            break;
        }
    }
    return 0;
}

void qd_event_loop_run(qd_event_loop_t *loop)
{
    struct epoll_event events[32];

    while (loop->running && g_running) {
        int n = epoll_wait(loop->epoll_fd, events, 32, 100);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            for (int j = 0; j < loop->num_entries; j++) {
                if (loop->entries[j].fd == fd && loop->entries[j].cb) {
                    loop->entries[j].cb(fd, events[i].events,
                                        loop->entries[j].arg);
                    break;
                }
            }
        }
    }
}

void qd_event_loop_stop(qd_event_loop_t *loop)
{
    if (loop) loop->running = 0;
}

/* Daemon */
qd_daemon_t *qd_daemon_create(const qd_daemon_config_t *config)
{
    qd_daemon_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->name = config->name;
    d->loop = qd_event_loop_create();
    if (!d->loop) {
        free(d);
        return NULL;
    }
    d->running = 1;
    return d;
}

void qd_daemon_destroy(qd_daemon_t *d)
{
    if (d) {
        qd_event_loop_destroy(d->loop);
        free(d);
    }
}

void qd_daemon_set_init_callback(qd_daemon_t *d,
    int (*cb)(qd_daemon_t *, void *), void *arg)
{
    d->init_cb = cb;
    d->init_arg = arg;
}

void qd_daemon_set_shutdown_callback(qd_daemon_t *d,
    void (*cb)(qd_daemon_t *, void *), void *arg)
{
    d->shutdown_cb = cb;
    d->shutdown_arg = arg;
}

qd_event_loop_t *qd_daemon_get_loop(qd_daemon_t *d)
{
    return d ? d->loop : NULL;
}

int qd_daemon_init(qd_daemon_t *d)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (d->init_cb) {
        return d->init_cb(d, d->init_arg);
    }
    return 0;
}

void qd_daemon_run(qd_daemon_t *d)
{
    qd_event_loop_run(d->loop);

    if (d->shutdown_cb) {
        d->shutdown_cb(d, d->shutdown_arg);
    }
}

/* Handler */
qd_handler_table_t *qd_handler_table_create(const char *name)
{
    qd_handler_table_t *t = calloc(1, sizeof(*t));
    if (t) t->name = name;
    return t;
}

void qd_handler_table_destroy(qd_handler_table_t *t)
{
    free(t);
}

void qd_handler_register_table(qd_handler_table_t *t,
                               const qd_handler_entry_t *entries)
{
    if (t) t->entries = entries;
}

int qd_handler_register_fn(qd_handler_table_t *t, int cmd,
                           qd_handler_result_t (*handler)(qd_handler_ctx_t *, void *),
                           void *arg, int flags)
{
    (void)t;
    (void)cmd;
    (void)handler;
    (void)arg;
    (void)flags;
    /* In stub mode, we don't actually register individual handlers */
    return 0;
}

void qd_handler_ctx_init(qd_handler_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void *qd_handler_ctx_alloc_response(qd_handler_ctx_t *ctx, size_t size)
{
    ctx->response = malloc(size);
    ctx->response_len = size;
    return ctx->response;
}

int qd_handler_dispatch(qd_handler_table_t *t, qd_handler_ctx_t *ctx)
{
    if (!t || !t->entries) return -1;

    for (const qd_handler_entry_t *e = t->entries; e->handler; e++) {
        if (e->cmd == ctx->cmd) {
            return e->handler(ctx, e->arg);
        }
    }
    return -1;
}

/* Netlink stub - returns NULL since we use raw sockets */
qd_netlink_t *qd_netlink_create(const char *family)
{
    (void)family;
    /* Return NULL - we don't need netlink for basic operation */
    return NULL;
}

void qd_netlink_destroy(qd_netlink_t *nl)
{
    (void)nl;
}

void qd_netlink_set_callback(qd_netlink_t *nl, void *cb, void *arg)
{
    (void)nl;
    (void)cb;
    (void)arg;
}

void qd_netlink_msg_init(qd_netlink_msg_t *msg, int cmd)
{
    if (msg) {
        memset(msg, 0, sizeof(*msg));
        msg->cmd = cmd;
    }
}

int qd_netlink_msg_add_attr(qd_netlink_msg_t *msg, int type,
                             const void *data, size_t len)
{
    (void)msg;
    (void)type;
    (void)data;
    (void)len;
    return 0;
}

int qd_netlink_send_msg(qd_netlink_t *nl, qd_netlink_msg_t *msg)
{
    (void)nl;
    (void)msg;
    return QD_OK;
}

void *qd_netlink_msg_get_data(qd_netlink_msg_t *msg, int type, size_t *len)
{
    (void)msg;
    (void)type;
    (void)len;
    return NULL;
}

/* Handler macro compatible with the main code */
#define QD_HANDLER(cmd, fn) { cmd, fn, NULL }
#define QD_HANDLER_END() { 0, NULL, NULL }

/* CLI types - must match qdaemon_stub.h */
typedef struct qd_cli_ctx {
    struct qd_cli_shell *shell;
    struct qd_cli_mode *mode;
    int argc;
    char **argv;
    void *user_data;
    void (*print)(const char *fmt, ...);
    void (*error)(const char *fmt, ...);
} qd_cli_ctx_t;

typedef int qd_cli_result_t;
typedef qd_cli_result_t (*qd_cli_cmd_handler_t)(qd_cli_ctx_t *);

#define QD_CLI_OK 0
#define QD_CLI_ERROR -1
#define QD_CLI_MODE_CHANGE 1

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

struct qd_cli_cmd {
    char name[64];
    char help[256];
    qd_cli_cmd_handler_t handler;
    struct qd_cli_cmd *next;
};

struct qd_cli_mode {
    char name[32];
    char prompt[32];
    struct qd_cli_mode *parent;
    struct qd_cli_cmd *commands;
};

struct qd_cli_shell {
    char name[32];
    struct qd_cli_mode *root_mode;
    struct qd_cli_mode *current_mode;
    volatile int running;
};

typedef struct qd_cli_shell qd_cli_shell_t;
typedef struct qd_cli_mode qd_cli_mode_t;

/* Forward declarations */
qd_cli_mode_t *qd_cli_mode_create(qd_cli_shell_t *shell, const char *name,
                                  const char *prompt, const char *help);

/* CLI implementation using readline */
#include <readline/readline.h>
#include <readline/history.h>

qd_cli_shell_t *qd_cli_create(const qd_cli_config_t *config)
{
    qd_cli_shell_t *shell = calloc(1, sizeof(*shell));
    if (!shell) return NULL;

    strncpy(shell->name, config->name, sizeof(shell->name) - 1);
    shell->root_mode = qd_cli_mode_create(shell, "root", config->prompt, "Root Mode");
    shell->current_mode = shell->root_mode;
    shell->running = 1;

    using_history();

    return shell;
}

void qd_cli_destroy(qd_cli_shell_t *shell)
{
    if (!shell) return;

    /* Free modes and commands would go here */
    free(shell);
}

qd_cli_mode_t *qd_cli_mode_create(qd_cli_shell_t *shell, const char *name,
                                  const char *prompt, const char *help)
{
    (void)shell;
    (void)help;

    qd_cli_mode_t *mode = calloc(1, sizeof(*mode));
    if (!mode) return NULL;

    strncpy(mode->name, name, sizeof(mode->name) - 1);
    strncpy(mode->prompt, prompt, sizeof(mode->prompt) - 1);
    return mode;
}

void qd_cli_mode_set_parent(qd_cli_mode_t *mode, qd_cli_mode_t *parent)
{
    if (mode) mode->parent = parent;
}

qd_cli_mode_t *qd_cli_get_root_mode(qd_cli_shell_t *shell)
{
    return shell ? shell->root_mode : NULL;
}

int qd_cli_register_cmd(qd_cli_mode_t *mode, const char *name,
                        qd_cli_cmd_handler_t handler, const char *help)
{
    if (!mode || !name) return -1;

    struct qd_cli_cmd *cmd = calloc(1, sizeof(*cmd));
    if (!cmd) return -1;

    strncpy(cmd->name, name, sizeof(cmd->name) - 1);
    if (help) strncpy(cmd->help, help, sizeof(cmd->help) - 1);
    cmd->handler = handler;
    cmd->next = mode->commands;
    mode->commands = cmd;

    return 0;
}

void qd_cli_enter_mode(qd_cli_shell_t *shell, qd_cli_mode_t *mode)
{
    if (shell && mode) {
        shell->current_mode = mode;
    }
}

void qd_cli_exit_mode(qd_cli_shell_t *shell)
{
    if (shell && shell->current_mode && shell->current_mode->parent) {
        shell->current_mode = shell->current_mode->parent;
    }
}

static void cli_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void cli_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "Error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void cli_show_help(qd_cli_mode_t *mode)
{
    printf("\nAvailable commands:\n");
    for (struct qd_cli_cmd *cmd = mode->commands; cmd; cmd = cmd->next) {
        printf("  %-20s %s\n", cmd->name, cmd->help);
    }
    printf("  %-20s %s\n", "exit", "Exit current mode or quit");
    printf("  %-20s %s\n", "help", "Show this help");
    printf("\n");
}

static int cli_parse_args(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    return argc;
}

int qd_cli_run(qd_cli_shell_t *shell)
{
    if (!shell) return -1;

    printf("QDaemon DHCP CLI\n");
    printf("Type 'help' for available commands\n\n");

    char *argv[32];
    qd_cli_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.print = cli_print;
    ctx.error = cli_error;

    while (shell->running) {
        char *line = readline(shell->current_mode->prompt);
        if (!line) {
            printf("\n");
            break;
        }

        /* Skip empty lines */
        if (!*line) {
            free(line);
            continue;
        }

        add_history(line);

        /* Parse arguments */
        int argc = cli_parse_args(line, argv, 32);
        if (argc == 0) {
            free(line);
            continue;
        }

        /* Built-in commands */
        if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
            if (shell->current_mode->parent) {
                shell->current_mode = shell->current_mode->parent;
            } else {
                shell->running = 0;
            }
            free(line);
            continue;
        }

        if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
            cli_show_help(shell->current_mode);
            free(line);
            continue;
        }

        /* Find and execute command */
        struct qd_cli_cmd *cmd;
        int found = 0;
        for (cmd = shell->current_mode->commands; cmd; cmd = cmd->next) {
            if (strcmp(cmd->name, argv[0]) == 0) {
                found = 1;
                ctx.shell = shell;
                ctx.mode = shell->current_mode;
                ctx.argc = argc;
                ctx.argv = argv;
                ctx.user_data = NULL;

                if (cmd->handler) {
                    cmd->handler(&ctx);
                }
                break;
            }
        }

        if (!found) {
            printf("Unknown command: %s\n", argv[0]);
            printf("Type 'help' for available commands\n");
        }

        free(line);
    }

    return 0;
}
