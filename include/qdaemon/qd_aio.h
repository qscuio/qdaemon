/*
 * QDaemon - Async I/O API
 * High-performance async I/O with io_uring/epoll backends
 */

#ifndef QD_AIO_H
#define QD_AIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include "qd_common.h"
#include "qd_aio_ops.h"
#include "qd_aio_features.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_aio_loop qd_aio_loop_t;
typedef struct qd_channel qd_channel_t;

/* Completion callback */
typedef void (*qd_aio_cb_t)(qd_completion_t *comp, void *arg);

/* Loop configuration */
typedef struct qd_aio_config {
    int queue_depth;          /* Submission queue depth (default: 256) */
    int batch_size;           /* Max completions per wait (default: 64) */
    qd_aio_cb_t callback;     /* Completion callback */
    void *callback_arg;       /* Callback argument */
} qd_aio_config_t;

#define QD_AIO_CONFIG_DEFAULT { \
    .queue_depth = 256, \
    .batch_size = 64, \
    .callback = NULL, \
    .callback_arg = NULL \
}

/*
 * Loop Lifecycle
 */

/* Create async I/O loop with default settings */
qd_aio_loop_t *qd_aio_create(void);

/* Create async I/O loop with configuration */
qd_aio_loop_t *qd_aio_create_ex(const qd_aio_config_t *config);

/* Destroy async I/O loop */
void qd_aio_destroy(qd_aio_loop_t *loop);

/*
 * Loop Execution
 */

/* Run loop (blocks until stopped) */
int qd_aio_run(qd_aio_loop_t *loop);

/* Run single iteration
 * timeout_ms: -1 for infinite, 0 for non-blocking, >0 for timeout
 * Returns: number of completions processed, or -errno on error
 */
int qd_aio_run_once(qd_aio_loop_t *loop, int timeout_ms);

/* Stop running loop */
void qd_aio_stop(qd_aio_loop_t *loop);

/* Check if loop is running */
int qd_aio_is_running(qd_aio_loop_t *loop);

/*
 * File I/O Operations
 */

/* Async read */
int qd_aio_read(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                uint64_t offset, void *user_data);

/* Async write */
int qd_aio_write(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                 uint64_t offset, void *user_data);

/* Async poll for events */
int qd_aio_poll(qd_aio_loop_t *loop, int fd, uint32_t events, void *user_data);

/* Async close */
int qd_aio_close(qd_aio_loop_t *loop, int fd, void *user_data);

/*
 * Network Operations
 */

/* Async accept */
int qd_aio_accept(qd_aio_loop_t *loop, int fd, struct sockaddr *addr,
                  socklen_t *addrlen, void *user_data);

/* Async connect */
int qd_aio_connect(qd_aio_loop_t *loop, int fd, const struct sockaddr *addr,
                   socklen_t addrlen, void *user_data);

/* Async send */
int qd_aio_send(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                int flags, void *user_data);

/* Async recv */
int qd_aio_recv(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                int flags, void *user_data);

/*
 * Timer Operations
 */

/* Add timeout (one-shot) */
int qd_aio_timeout(qd_aio_loop_t *loop, int timeout_ms, void *user_data);

/* Cancel pending operation by user_data */
int qd_aio_cancel(qd_aio_loop_t *loop, void *user_data);

/*
 * Multishot Operations (io_uring only, no-op on epoll)
 */

/* Multishot accept - keeps accepting until cancelled */
int qd_aio_accept_multi(qd_aio_loop_t *loop, int fd, void *user_data);

/* Multishot recv - keeps receiving until cancelled */
int qd_aio_recv_multi(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                      int flags, void *user_data);

/*
 * Backend Information
 */

/* Get backend name */
const char *qd_aio_backend_name(void);

/* Get backend features bitmask */
uint32_t qd_aio_backend_features(void);

/* Check if feature is supported */
static inline int qd_aio_has_feature(uint32_t feature)
{
    return (qd_aio_backend_features() & feature) != 0;
}

/*
 * Statistics
 */

typedef struct qd_aio_stats {
    uint64_t submit_count;    /* Operations submitted */
    uint64_t complete_count;  /* Operations completed */
    uint64_t error_count;     /* Operations that failed */
    int pending;              /* Currently pending */
} qd_aio_stats_t;

void qd_aio_stats(qd_aio_loop_t *loop, qd_aio_stats_t *stats);

/*
 * Channel Watching
 */

/* Watch a channel for readability - generates QD_OP_CHANNEL completions */
int qd_aio_channel_watch(qd_aio_loop_t *loop, qd_channel_t *chan,
                         void *user_data);

/* Stop watching a channel */
int qd_aio_channel_unwatch(qd_aio_loop_t *loop, qd_channel_t *chan);

#ifdef __cplusplus
}
#endif

#endif /* QD_AIO_H */
