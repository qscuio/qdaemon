/*
 * QDaemon - Health Monitoring Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>

#include "qdaemon/qd_daemon.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_timer.h"
#include "qdaemon/qd_log.h"

static qd_time_t g_start_time = 0;

void qd_health_init(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_start_time = (qd_time_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static size_t get_memory_usage(void)
{
    FILE *fp = fopen("/proc/self/statm", "r");
    if (!fp) return 0;

    long total = 0, resident = 0;
    if (fscanf(fp, "%ld %ld", &total, &resident) != 2) resident = 0;
    fclose(fp);

    return (size_t)(resident * sysconf(_SC_PAGESIZE));
}

int qd_daemon_health_status(qd_daemon_t *daemon, qd_health_status_t *status)
{
    if (!daemon || !status)
        return QD_ERR_INVAL;

    memset(status, 0, sizeof(*status));

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    qd_time_t now = (qd_time_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    status->healthy = daemon->healthy;
    status->uptime = now - g_start_time;
    status->last_check = daemon->last_health_check;
    status->memory_used = get_memory_usage();
    status->active_connections = 0;
    status->pending_tasks = 0;

    if (status->healthy)
        snprintf(status->message, sizeof(status->message), "OK");
    else
        snprintf(status->message, sizeof(status->message), "Unhealthy");

    return QD_OK;
}

int qd_daemon_health_check(qd_daemon_t *daemon)
{
    if (!daemon)
        return QD_ERR_INVAL;

    int healthy = 1;

    /* Call user health callback if set */
    if (daemon->callbacks.on_health_check) {
        daemon->callbacks.on_health_check(daemon, &healthy,
                                          daemon->callbacks.on_health_check_arg);
    }

    /* Update state */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    daemon->last_health_check = (qd_time_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    daemon->healthy = healthy;

    if (!healthy) {
        qd_log_warn("Health check failed");
    }

    return healthy ? QD_OK : QD_ERR_SYSTEM;
}

static void health_check_timer_cb(qd_timer_t *timer, void *arg)
{
    (void)timer;
    qd_daemon_t *daemon = arg;
    qd_daemon_health_check(daemon);
}

int qd_health_start_timer(qd_daemon_t *daemon, qd_timer_wheel_t *wheel)
{
    if (!daemon || !wheel || daemon->config.health_check_interval <= 0)
        return QD_OK;

    uint64_t interval_ms = daemon->config.health_check_interval * 1000;
    qd_timer_add_periodic(wheel, interval_ms, health_check_timer_cb, daemon);

    qd_log_debug("Health check timer started (interval: %ds)",
                 daemon->config.health_check_interval);

    return QD_OK;
}
