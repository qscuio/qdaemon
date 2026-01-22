/*
 * QDaemon - Daemon Lifecycle API
 * Complete daemon infrastructure management
 */

#ifndef QD_DAEMON_H
#define QD_DAEMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_daemon qd_daemon_t;
typedef struct qd_event_loop qd_event_loop_t;
typedef struct qd_threadpool qd_threadpool_t;
typedef struct qd_ipc_server qd_ipc_server_t;
typedef struct qd_netlink qd_netlink_t;

/* Daemon state */
typedef enum {
    QD_DAEMON_STOPPED = 0,
    QD_DAEMON_STARTING,
    QD_DAEMON_RUNNING,
    QD_DAEMON_RELOADING,
    QD_DAEMON_STOPPING,
} qd_daemon_state_t;

/* Daemon configuration */
typedef struct qd_daemon_config {
    /* Basic settings */
    const char *name;            /* Daemon name */
    const char *version;         /* Version string */
    const char *description;     /* Description */

    /* File paths */
    const char *pid_file;        /* PID file path */
    const char *config_file;     /* Configuration file path */
    const char *log_file;        /* Log file path */
    const char *socket_path;     /* Unix socket path */
    const char *run_dir;         /* Runtime directory */

    /* Process settings */
    int daemonize;               /* Run as daemon */
    int foreground;              /* Force foreground */
    uid_t uid;                   /* User ID to run as */
    gid_t gid;                   /* Group ID to run as */
    const char *user;            /* Username (alternative to uid) */
    const char *group;           /* Group name (alternative to gid) */
    const char *chroot;          /* Chroot directory */
    const char *working_dir;     /* Working directory */
    mode_t umask;                /* File creation mask */

    /* Resource limits */
    int max_fd;                  /* Maximum file descriptors */
    size_t max_memory;           /* Maximum memory (bytes) */
    int core_dump;               /* Enable core dumps */

    /* Thread pool */
    int num_workers;             /* Number of worker threads */
    int enable_threadpool;       /* Enable thread pool */

    /* IPC settings */
    int enable_ipc;              /* Enable IPC server */
    int enable_netlink;          /* Enable netlink interface */

    /* Logging */
    int log_level;               /* Log level */
    int log_to_syslog;           /* Log to syslog */
    int log_to_file;             /* Log to file */
    int log_to_stderr;           /* Log to stderr */

    /* Signals */
    int handle_signals;          /* Handle standard signals */

    /* Health monitoring */
    int enable_health_check;     /* Enable health monitoring */
    int health_check_interval;   /* Health check interval (seconds) */
} qd_daemon_config_t;

/* Default configuration */
#define QD_DAEMON_CONFIG_DEFAULT { \
    .name = "qdaemon", \
    .version = QD_VERSION_STRING, \
    .description = NULL, \
    .pid_file = "/var/run/qdaemon.pid", \
    .config_file = "/etc/qdaemon.conf", \
    .log_file = "/var/log/qdaemon.log", \
    .socket_path = "/var/run/qdaemon.sock", \
    .run_dir = "/var/run", \
    .daemonize = 1, \
    .foreground = 0, \
    .uid = 0, \
    .gid = 0, \
    .user = NULL, \
    .group = NULL, \
    .chroot = NULL, \
    .working_dir = "/", \
    .umask = 022, \
    .max_fd = 1024, \
    .max_memory = 0, \
    .core_dump = 0, \
    .num_workers = 4, \
    .enable_threadpool = 1, \
    .enable_ipc = 1, \
    .enable_netlink = 0, \
    .log_level = 3, \
    .log_to_syslog = 1, \
    .log_to_file = 0, \
    .log_to_stderr = 0, \
    .handle_signals = 1, \
    .enable_health_check = 1, \
    .health_check_interval = 30 \
}

/* Service handler */
typedef int (*qd_service_handler_t)(qd_daemon_t *daemon, const char *service,
                                     const void *request, size_t req_len,
                                     void **response, size_t *resp_len);

/* Daemon callbacks */
typedef int (*qd_daemon_init_cb_t)(qd_daemon_t *daemon, void *arg);
typedef void (*qd_daemon_shutdown_cb_t)(qd_daemon_t *daemon, void *arg);
typedef int (*qd_daemon_reload_cb_t)(qd_daemon_t *daemon, void *arg);
typedef void (*qd_daemon_health_cb_t)(qd_daemon_t *daemon, int *healthy, void *arg);

/* Daemon callbacks structure */
typedef struct qd_daemon_callbacks {
    qd_daemon_init_cb_t on_init;
    void *on_init_arg;
    qd_daemon_shutdown_cb_t on_shutdown;
    void *on_shutdown_arg;
    qd_daemon_reload_cb_t on_reload;
    void *on_reload_arg;
    qd_daemon_health_cb_t on_health_check;
    void *on_health_check_arg;
} qd_daemon_callbacks_t;

/* Daemon structure */
struct qd_daemon {
    qd_daemon_config_t config;
    qd_daemon_callbacks_t callbacks;
    _Atomic int state;

    /* Core components */
    qd_event_loop_t *loop;
    qd_threadpool_t *pool;
    qd_ipc_server_t *ipc;
    qd_netlink_t *netlink;

    /* Process info */
    pid_t pid;
    pid_t parent_pid;

    /* Health */
    int healthy;
    qd_time_t last_health_check;

    /* User data */
    void *user_data;

    /* Internal */
    int exit_code;
    pthread_mutex_t lock;
};

/*
 * Daemon Lifecycle API
 */

/* Create daemon instance */
qd_daemon_t *qd_daemon_create(const qd_daemon_config_t *config);

/* Destroy daemon instance */
void qd_daemon_destroy(qd_daemon_t *daemon);

/* Initialize daemon (daemonize, setup signals, etc.) */
int qd_daemon_init(qd_daemon_t *daemon);

/* Run daemon (main loop) */
int qd_daemon_run(qd_daemon_t *daemon);

/* Request daemon shutdown */
void qd_daemon_shutdown(qd_daemon_t *daemon);

/* Request daemon reload */
void qd_daemon_reload(qd_daemon_t *daemon);

/* Get daemon state */
qd_daemon_state_t qd_daemon_get_state(qd_daemon_t *daemon);

/* Check if daemon is running */
int qd_daemon_is_running(qd_daemon_t *daemon);

/*
 * Callback Registration
 */

void qd_daemon_set_init_callback(qd_daemon_t *daemon, qd_daemon_init_cb_t cb, void *arg);
void qd_daemon_set_shutdown_callback(qd_daemon_t *daemon, qd_daemon_shutdown_cb_t cb, void *arg);
void qd_daemon_set_reload_callback(qd_daemon_t *daemon, qd_daemon_reload_cb_t cb, void *arg);
void qd_daemon_set_health_callback(qd_daemon_t *daemon, qd_daemon_health_cb_t cb, void *arg);

/*
 * Service Registration
 */

/* Register a service handler */
int qd_daemon_register_service(qd_daemon_t *daemon, const char *name,
                                qd_service_handler_t handler);

/* Unregister a service */
int qd_daemon_unregister_service(qd_daemon_t *daemon, const char *name);

/*
 * Component Access
 */

qd_event_loop_t *qd_daemon_get_loop(qd_daemon_t *daemon);
qd_threadpool_t *qd_daemon_get_threadpool(qd_daemon_t *daemon);
qd_ipc_server_t *qd_daemon_get_ipc(qd_daemon_t *daemon);
qd_netlink_t *qd_daemon_get_netlink(qd_daemon_t *daemon);

/*
 * User Data
 */

void qd_daemon_set_user_data(qd_daemon_t *daemon, void *data);
void *qd_daemon_get_user_data(qd_daemon_t *daemon);

/*
 * PID File Management
 */

/* Write PID file */
int qd_pidfile_write(const char *path, pid_t pid);

/* Read PID from file */
pid_t qd_pidfile_read(const char *path);

/* Remove PID file */
int qd_pidfile_remove(const char *path);

/* Check if PID file exists and process is running */
int qd_pidfile_check(const char *path);

/* Acquire PID file lock */
int qd_pidfile_lock(const char *path);

/*
 * Daemonization
 */

/* Daemonize process */
int qd_daemonize(void);

/* Drop privileges */
int qd_drop_privileges(uid_t uid, gid_t gid);

/* Lookup user/group IDs */
int qd_lookup_user(const char *name, uid_t *uid);
int qd_lookup_group(const char *name, gid_t *gid);

/* Set resource limits */
int qd_set_resource_limits(int max_fd, size_t max_memory, int core_dump);

/* Change root directory */
int qd_chroot(const char *path);

/*
 * Health Monitoring
 */

typedef struct qd_health_status {
    int healthy;
    qd_time_t uptime;
    qd_time_t last_check;
    size_t memory_used;
    int active_connections;
    int pending_tasks;
    char message[256];
} qd_health_status_t;

/* Get health status */
int qd_daemon_health_status(qd_daemon_t *daemon, qd_health_status_t *status);

/* Force health check */
int qd_daemon_health_check(qd_daemon_t *daemon);

/*
 * Utility Functions
 */

/* Get daemon by name (for IPC discovery) */
qd_daemon_t *qd_daemon_find(const char *name);

/* Get running daemon instance (singleton pattern) */
qd_daemon_t *qd_daemon_instance(void);

/* Parse command line arguments */
int qd_daemon_parse_args(qd_daemon_config_t *config, int argc, char **argv);

/* Print help message */
void qd_daemon_print_help(const char *program_name);

/* Print version */
void qd_daemon_print_version(const qd_daemon_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* QD_DAEMON_H */
