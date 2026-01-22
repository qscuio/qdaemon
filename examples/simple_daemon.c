/*
 * QDaemon - Simple Daemon Example
 * Demonstrates basic daemon lifecycle and IPC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <qdaemon/qdaemon.h>

/* User data */
typedef struct {
    int request_count;
} app_data_t;

/* Initialization callback */
static int on_init(qd_daemon_t *daemon, void *arg)
{
    (void)arg;
    qd_log_info("Application initialized");
    
    app_data_t *data = calloc(1, sizeof(app_data_t));
    qd_daemon_set_user_data(daemon, data);
    
    return 0;
}

/* Shutdown callback */
static void on_shutdown(qd_daemon_t *daemon, void *arg)
{
    (void)arg;
    qd_log_info("Application shutting down");
    
    app_data_t *data = qd_daemon_get_user_data(daemon);
    if (data) {
        qd_log_info("Processed %d requests", data->request_count);
        free(data);
    }
}

/* Reload callback */
static int on_reload(qd_daemon_t *daemon, void *arg)
{
    (void)daemon;
    (void)arg;
    qd_log_info("Configuration reloaded");
    return 0;
}

/* Health check callback */
static void on_health_check(qd_daemon_t *daemon, int *healthy, void *arg)
{
    (void)daemon;
    (void)arg;
    *healthy = 1;
    qd_log_debug("Health check passed");
}

int main(int argc, char **argv)
{
    /* Parse command line */
    qd_daemon_config_t config = QD_DAEMON_CONFIG_DEFAULT;
    config.name = "simple_daemon";
    config.version = "1.0.0";
    config.description = "Simple daemon example";
    config.pid_file = "/tmp/simple_daemon.pid";
    config.socket_path = "/tmp/simple_daemon.sock";
    config.log_file = "/tmp/simple_daemon.log";
    config.log_to_file = 1;
    config.log_to_stderr = 1;
    config.log_level = QD_LOG_DEBUG;
    
    qd_daemon_parse_args(&config, argc, argv);
    
    /* Check for help */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            qd_daemon_print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            qd_daemon_print_version(&config);
            return 0;
        }
    }
    
    /* Create daemon */
    qd_daemon_t *daemon = qd_daemon_create(&config);
    if (!daemon) {
        fprintf(stderr, "Failed to create daemon\n");
        return 1;
    }
    
    /* Set callbacks */
    qd_daemon_set_init_callback(daemon, on_init, NULL);
    qd_daemon_set_shutdown_callback(daemon, on_shutdown, NULL);
    qd_daemon_set_reload_callback(daemon, on_reload, NULL);
    qd_daemon_set_health_callback(daemon, on_health_check, NULL);
    
    /* Initialize daemon */
    int ret = qd_daemon_init(daemon);
    if (ret != QD_OK) {
        fprintf(stderr, "Failed to initialize daemon: %d\n", ret);
        qd_daemon_destroy(daemon);
        return 1;
    }
    
    /* Run main loop */
    ret = qd_daemon_run(daemon);
    
    /* Cleanup */
    qd_daemon_destroy(daemon);
    
    return ret == QD_OK ? 0 : 1;
}
