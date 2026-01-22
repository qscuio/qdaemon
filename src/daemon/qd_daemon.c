/*
 * QDaemon - Daemon Lifecycle Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <pwd.h>
#include <grp.h>

#include "qdaemon/qd_daemon.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_ipc.h"
#include "qdaemon/qd_netlink.h"
#include "qdaemon/qd_log.h"
#include "../util/qd_atomic.h"

/* Forward declarations */
extern int qd_signal_init(qd_daemon_t *daemon);
extern void qd_signal_shutdown(void);
extern int qd_signal_get_fd(void);
extern int qd_signal_process(void);
extern void qd_health_init(void);
extern int qd_health_start_timer(qd_daemon_t *daemon, void *wheel);

static qd_daemon_t *g_daemon_instance = NULL;

qd_daemon_t *qd_daemon_create(const qd_daemon_config_t *config)
{
    qd_daemon_t *daemon = calloc(1, sizeof(qd_daemon_t));
    if (!daemon) return NULL;

    if (config) memcpy(&daemon->config, config, sizeof(*config));
    else {
        qd_daemon_config_t def = QD_DAEMON_CONFIG_DEFAULT;
        memcpy(&daemon->config, &def, sizeof(def));
    }

    pthread_mutex_init(&daemon->lock, NULL);
    atomic_store(&daemon->state, QD_DAEMON_STOPPED);
    daemon->healthy = 1;
    daemon->pid = getpid();
    daemon->parent_pid = getppid();

    g_daemon_instance = daemon;
    return daemon;
}

void qd_daemon_destroy(qd_daemon_t *daemon)
{
    if (!daemon) return;

    if (daemon->netlink) qd_netlink_destroy(daemon->netlink);
    if (daemon->ipc) qd_ipc_server_destroy(daemon->ipc);
    if (daemon->pool) qd_threadpool_destroy(daemon->pool);
    if (daemon->loop) qd_event_loop_destroy(daemon->loop);

    pthread_mutex_destroy(&daemon->lock);

    if (g_daemon_instance == daemon) g_daemon_instance = NULL;
    free(daemon);
}

int qd_daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0) return QD_ERR_SYSTEM;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return QD_ERR_SYSTEM;

    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) return QD_ERR_SYSTEM;
    if (pid > 0) _exit(0);

    umask(0);
    if (chdir("/") < 0) { /* ignore */ }

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);

    return QD_OK;
}

int qd_pidfile_write(const char *path, pid_t pid)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return QD_ERR_IO;
    fprintf(fp, "%d\n", pid);
    fclose(fp);
    return QD_OK;
}

pid_t qd_pidfile_read(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    pid_t pid = -1;
    if (fscanf(fp, "%d", &pid) != 1) pid = -1;
    fclose(fp);
    return pid;
}

int qd_pidfile_remove(const char *path)
{
    return unlink(path) == 0 ? QD_OK : QD_ERR_IO;
}

int qd_pidfile_check(const char *path)
{
    pid_t pid = qd_pidfile_read(path);
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0 ? 1 : 0;
}

int qd_pidfile_lock(const char *path)
{
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return QD_ERR_IO;

    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return QD_ERR_BUSY;
    }

    if (ftruncate(fd, 0) < 0) { /* ignore */ }
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (write(fd, buf, len) != len) { /* ignore */ }

    return QD_OK;
}

int qd_lookup_user(const char *name, uid_t *uid)
{
    struct passwd *pw = getpwnam(name);
    if (!pw) return QD_ERR_NOENT;
    *uid = pw->pw_uid;
    return QD_OK;
}

int qd_lookup_group(const char *name, gid_t *gid)
{
    struct group *gr = getgrnam(name);
    if (!gr) return QD_ERR_NOENT;
    *gid = gr->gr_gid;
    return QD_OK;
}

int qd_drop_privileges(uid_t uid, gid_t gid)
{
    if (gid != 0 && setgid(gid) < 0) return QD_ERR_SYSTEM;
    if (uid != 0 && setuid(uid) < 0) return QD_ERR_SYSTEM;
    return QD_OK;
}

int qd_set_resource_limits(int max_fd, size_t max_memory, int core_dump)
{
    if (max_fd > 0) {
        struct rlimit rl = { .rlim_cur = max_fd, .rlim_max = max_fd };
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    if (max_memory > 0) {
        struct rlimit rl = { .rlim_cur = max_memory, .rlim_max = max_memory };
        setrlimit(RLIMIT_AS, &rl);
    }
    if (core_dump) {
        struct rlimit rl = { .rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY };
        setrlimit(RLIMIT_CORE, &rl);
    } else {
        struct rlimit rl = { 0 };
        setrlimit(RLIMIT_CORE, &rl);
    }
    return QD_OK;
}

int qd_chroot(const char *path)
{
    if (chroot(path) < 0) return QD_ERR_SYSTEM;
    if (chdir("/") < 0) return QD_ERR_SYSTEM;
    return QD_OK;
}

static void signal_event_callback(int fd, uint32_t events, void *arg)
{
    (void)fd; (void)events; (void)arg;
    qd_signal_process();
}

int qd_daemon_init(qd_daemon_t *daemon)
{
    if (!daemon) return QD_ERR_INVAL;

    atomic_store(&daemon->state, QD_DAEMON_STARTING);

    /* Check for existing instance */
    if (daemon->config.pid_file && qd_pidfile_check(daemon->config.pid_file)) {
        qd_log_error("Daemon already running");
        return QD_ERR_BUSY;
    }

    /* Daemonize if requested */
    if (daemon->config.daemonize && !daemon->config.foreground) {
        int ret = qd_daemonize();
        if (ret != QD_OK) return ret;
        daemon->pid = getpid();
    }

    /* Write PID file */
    if (daemon->config.pid_file) {
        if (qd_pidfile_lock(daemon->config.pid_file) != QD_OK) {
            qd_log_error("Failed to create PID file");
            return QD_ERR_IO;
        }
    }

    /* Set resource limits */
    qd_set_resource_limits(daemon->config.max_fd, daemon->config.max_memory,
                           daemon->config.core_dump);

    /* Initialize logging */
    if (daemon->config.log_file || daemon->config.log_to_syslog) {
        qd_log_config_t log_cfg = QD_LOG_CONFIG_DEFAULT;
        log_cfg.level = daemon->config.log_level;
        log_cfg.file_path = daemon->config.log_file;
        log_cfg.targets = 0;
        if (daemon->config.log_to_file) log_cfg.targets |= QD_LOG_TARGET_FILE;
        if (daemon->config.log_to_syslog) log_cfg.targets |= QD_LOG_TARGET_SYSLOG;
        if (daemon->config.log_to_stderr) log_cfg.targets |= QD_LOG_TARGET_STDERR;
        qd_log_init(&log_cfg);
    }

    qd_log_info("Starting %s version %s",
                daemon->config.name, daemon->config.version);

    /* Initialize signal handling */
    if (daemon->config.handle_signals) {
        qd_signal_init(daemon);
    }

    /* Create event loop */
    daemon->loop = qd_event_loop_create();
    if (!daemon->loop) {
        qd_log_error("Failed to create event loop");
        return QD_ERR_NOMEM;
    }

    /* Add signal fd to event loop */
    if (daemon->config.handle_signals) {
        int sig_fd = qd_signal_get_fd();
        if (sig_fd >= 0) {
            qd_event_add(daemon->loop, sig_fd, QD_EVENT_READ,
                        signal_event_callback, daemon);
        }
    }

    /* Create thread pool */
    if (daemon->config.enable_threadpool && daemon->config.num_workers > 0) {
        daemon->pool = qd_threadpool_create(daemon->config.num_workers);
        if (!daemon->pool) {
            qd_log_error("Failed to create thread pool");
            return QD_ERR_NOMEM;
        }
    }

    /* Create IPC server */
    if (daemon->config.enable_ipc && daemon->config.socket_path) {
        daemon->ipc = qd_ipc_server_create(daemon->config.socket_path);
        if (!daemon->ipc) {
            qd_log_error("Failed to create IPC server");
            return QD_ERR_IO;
        }
        qd_ipc_server_start(daemon->ipc, daemon->loop);
    }

    /* Drop privileges */
    uid_t uid = daemon->config.uid;
    gid_t gid = daemon->config.gid;
    if (daemon->config.user) qd_lookup_user(daemon->config.user, &uid);
    if (daemon->config.group) qd_lookup_group(daemon->config.group, &gid);
    if (uid != 0 || gid != 0) qd_drop_privileges(uid, gid);

    /* Initialize health monitoring */
    qd_health_init();

    /* Call user init callback */
    if (daemon->callbacks.on_init) {
        int ret = daemon->callbacks.on_init(daemon, daemon->callbacks.on_init_arg);
        if (ret != 0) {
            qd_log_error("User init callback failed");
            return ret;
        }
    }

    atomic_store(&daemon->state, QD_DAEMON_RUNNING);
    qd_log_info("Daemon initialized successfully");

    return QD_OK;
}

int qd_daemon_run(qd_daemon_t *daemon)
{
    if (!daemon || !daemon->loop) return QD_ERR_INVAL;

    qd_log_info("Daemon entering main loop");
    int ret = qd_event_loop_run(daemon->loop);
    qd_log_info("Daemon exiting main loop");

    return ret;
}

void qd_daemon_shutdown(qd_daemon_t *daemon)
{
    if (!daemon) return;

    qd_daemon_state_t expected = QD_DAEMON_RUNNING;
    if (!atomic_compare_exchange_strong(&daemon->state, &expected, QD_DAEMON_STOPPING))
        return;

    qd_log_info("Initiating daemon shutdown");

    /* Call shutdown callback */
    if (daemon->callbacks.on_shutdown) {
        daemon->callbacks.on_shutdown(daemon, daemon->callbacks.on_shutdown_arg);
    }

    /* Stop event loop */
    if (daemon->loop) qd_event_loop_stop(daemon->loop);

    /* Shutdown thread pool */
    if (daemon->pool) {
        qd_threadpool_shutdown(daemon->pool, QD_SHUTDOWN_GRACEFUL);
        qd_threadpool_wait(daemon->pool, 5000);
    }

    /* Remove PID file */
    if (daemon->config.pid_file) qd_pidfile_remove(daemon->config.pid_file);

    /* Shutdown signal handling */
    qd_signal_shutdown();

    /* Shutdown logging */
    qd_log_info("Daemon shutdown complete");
    qd_log_shutdown();

    atomic_store(&daemon->state, QD_DAEMON_STOPPED);
}

void qd_daemon_reload(qd_daemon_t *daemon)
{
    if (!daemon) return;

    atomic_store(&daemon->state, QD_DAEMON_RELOADING);
    qd_log_info("Reloading configuration");

    if (daemon->callbacks.on_reload) {
        daemon->callbacks.on_reload(daemon, daemon->callbacks.on_reload_arg);
    }

    atomic_store(&daemon->state, QD_DAEMON_RUNNING);
}

qd_daemon_state_t qd_daemon_get_state(qd_daemon_t *daemon)
{
    return daemon ? atomic_load(&daemon->state) : QD_DAEMON_STOPPED;
}

int qd_daemon_is_running(qd_daemon_t *daemon)
{
    return daemon && atomic_load(&daemon->state) == QD_DAEMON_RUNNING;
}

void qd_daemon_set_init_callback(qd_daemon_t *d, qd_daemon_init_cb_t cb, void *arg)
{
    if (d) { d->callbacks.on_init = cb; d->callbacks.on_init_arg = arg; }
}

void qd_daemon_set_shutdown_callback(qd_daemon_t *d, qd_daemon_shutdown_cb_t cb, void *arg)
{
    if (d) { d->callbacks.on_shutdown = cb; d->callbacks.on_shutdown_arg = arg; }
}

void qd_daemon_set_reload_callback(qd_daemon_t *d, qd_daemon_reload_cb_t cb, void *arg)
{
    if (d) { d->callbacks.on_reload = cb; d->callbacks.on_reload_arg = arg; }
}

void qd_daemon_set_health_callback(qd_daemon_t *d, qd_daemon_health_cb_t cb, void *arg)
{
    if (d) { d->callbacks.on_health_check = cb; d->callbacks.on_health_check_arg = arg; }
}

int qd_daemon_register_service(qd_daemon_t *d, const char *name, qd_service_handler_t h)
{
    (void)d; (void)name; (void)h;
    return QD_OK; /* TODO: Implement service registration */
}

int qd_daemon_unregister_service(qd_daemon_t *d, const char *name)
{
    (void)d; (void)name;
    return QD_OK;
}

qd_event_loop_t *qd_daemon_get_loop(qd_daemon_t *d) { return d ? d->loop : NULL; }
qd_threadpool_t *qd_daemon_get_threadpool(qd_daemon_t *d) { return d ? d->pool : NULL; }
qd_ipc_server_t *qd_daemon_get_ipc(qd_daemon_t *d) { return d ? d->ipc : NULL; }
qd_netlink_t *qd_daemon_get_netlink(qd_daemon_t *d) { return d ? d->netlink : NULL; }

void qd_daemon_set_user_data(qd_daemon_t *d, void *data) { if (d) d->user_data = data; }
void *qd_daemon_get_user_data(qd_daemon_t *d) { return d ? d->user_data : NULL; }

qd_daemon_t *qd_daemon_instance(void) { return g_daemon_instance; }
qd_daemon_t *qd_daemon_find(const char *name) { (void)name; return g_daemon_instance; }

int qd_daemon_parse_args(qd_daemon_config_t *cfg, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0)
            cfg->foreground = 1;
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0)
            cfg->log_level = 4;
        else if (strcmp(argv[i], "-c") == 0 && i+1 < argc)
            cfg->config_file = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc)
            cfg->pid_file = argv[++i];
    }
    return QD_OK;
}

void qd_daemon_print_help(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -f, --foreground  Run in foreground\n");
    printf("  -d, --debug       Enable debug logging\n");
    printf("  -c <file>         Configuration file\n");
    printf("  -p <file>         PID file\n");
    printf("  -h, --help        Show this help\n");
}

void qd_daemon_print_version(const qd_daemon_config_t *cfg)
{
    printf("%s version %s\n", cfg->name, cfg->version);
}
