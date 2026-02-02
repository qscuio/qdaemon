/*
 * QDaemon - Async I/O Loop Implementation
 * Main async loop using backend abstraction
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>

#include "qdaemon/qd_aio.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"
#include "qdaemon/qd_channel.h"
#include "backend/qd_backend.h"

/* Async I/O loop structure */
struct qd_aio_loop {
    const qd_backend_ops_t *backend;
    void *backend_ctx;

    qd_aio_cb_t callback;
    void *callback_arg;

    atomic_int running;
    int batch_size;

    /* Statistics */
    uint64_t submit_count;
    uint64_t complete_count;
    uint64_t error_count;
};

qd_aio_loop_t *qd_aio_create(void)
{
    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    return qd_aio_create_ex(&config);
}

qd_aio_loop_t *qd_aio_create_ex(const qd_aio_config_t *config)
{
    if (!config)
        return NULL;

    qd_aio_loop_t *loop = qd_calloc(1, sizeof(*loop));
    if (!loop)
        return NULL;

    loop->backend = QD_BACKEND_DEFAULT;
    loop->backend_ctx = loop->backend->init(config->queue_depth);
    if (!loop->backend_ctx) {
        qd_log_error("Failed to initialize %s backend", loop->backend->name);
        qd_free(loop);
        return NULL;
    }

    loop->callback = config->callback;
    loop->callback_arg = config->callback_arg;
    loop->batch_size = config->batch_size > 0 ? config->batch_size : 64;

    atomic_init(&loop->running, 0);

    qd_log_info("Async I/O loop created with %s backend", loop->backend->name);

    return loop;
}

void qd_aio_destroy(qd_aio_loop_t *loop)
{
    if (!loop)
        return;

    qd_aio_stop(loop);

    if (loop->backend && loop->backend_ctx)
        loop->backend->destroy(loop->backend_ctx);

    qd_free(loop);
}

int qd_aio_run_once(qd_aio_loop_t *loop, int timeout_ms)
{
    if (!loop || !loop->backend_ctx)
        return QD_ERR_INVAL;

    /* Allocate completions on stack for small batches, heap for large */
    qd_completion_t stack_completions[64];
    qd_completion_t *completions;

    if (loop->batch_size <= 64) {
        completions = stack_completions;
    } else {
        completions = qd_malloc(loop->batch_size * sizeof(*completions));
        if (!completions)
            return QD_ERR_NOMEM;
    }

    int count = loop->backend->wait(loop->backend_ctx, completions,
                                     loop->batch_size, timeout_ms);

    if (count < 0) {
        if (completions != stack_completions)
            qd_free(completions);
        return count;
    }

    /* Process completions */
    for (int i = 0; i < count; i++) {
        if (completions[i].result < 0)
            loop->error_count++;

        if (loop->callback)
            loop->callback(&completions[i], loop->callback_arg);

        loop->complete_count++;
    }

    if (completions != stack_completions)
        qd_free(completions);

    return count;
}

int qd_aio_run(qd_aio_loop_t *loop)
{
    if (!loop)
        return QD_ERR_INVAL;

    atomic_store(&loop->running, 1);

    while (atomic_load(&loop->running)) {
        int ret = qd_aio_run_once(loop, 1000); /* 1 second timeout */
        if (ret < 0 && ret != -EINTR && ret != -ETIME) {
            qd_log_error("qd_aio_run_once failed: %d", ret);
            break;
        }
    }

    return QD_OK;
}

void qd_aio_stop(qd_aio_loop_t *loop)
{
    if (loop)
        atomic_store(&loop->running, 0);
}

int qd_aio_is_running(qd_aio_loop_t *loop)
{
    return loop ? atomic_load(&loop->running) : 0;
}

/* Helper to submit operation */
static int qd_aio_submit(qd_aio_loop_t *loop, qd_aio_req_t *req)
{
    if (!loop || !loop->backend_ctx)
        return QD_ERR_INVAL;

    int ret = loop->backend->submit(loop->backend_ctx, req);
    if (ret == QD_OK)
        loop->submit_count++;

    return ret;
}

/*
 * File I/O Operations
 */

int qd_aio_read(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                uint64_t offset, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_READ,
        .fd = fd,
        .buf = buf,
        .len = len,
        .offset = offset,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_write(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                 uint64_t offset, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_WRITE,
        .fd = fd,
        .buf = (void *)buf,
        .len = len,
        .offset = offset,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_poll(qd_aio_loop_t *loop, int fd, uint32_t events, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_POLL,
        .fd = fd,
        .aux = (int)events,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_close(qd_aio_loop_t *loop, int fd, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_CLOSE,
        .fd = fd,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

/*
 * Network Operations
 */

int qd_aio_accept(qd_aio_loop_t *loop, int fd, struct sockaddr *addr,
                  socklen_t *addrlen, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_ACCEPT,
        .fd = fd,
        .buf = addr,
        .len = addrlen ? *addrlen : 0,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_connect(qd_aio_loop_t *loop, int fd, const struct sockaddr *addr,
                   socklen_t addrlen, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_CONNECT,
        .fd = fd,
        .buf = (void *)addr,
        .len = addrlen,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_send(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                int flags, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_SEND,
        .fd = fd,
        .buf = (void *)buf,
        .len = len,
        .aux = flags,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_recv(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                int flags, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_RECV,
        .fd = fd,
        .buf = buf,
        .len = len,
        .aux = flags,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

/*
 * Timer Operations
 */

int qd_aio_timeout(qd_aio_loop_t *loop, int timeout_ms, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_TIMEOUT,
        .aux = timeout_ms,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_cancel(qd_aio_loop_t *loop, void *user_data)
{
    qd_aio_req_t req = {
        .op = QD_OP_CANCEL,
        .user_data = user_data,
        .flags = 0,
    };
    return qd_aio_submit(loop, &req);
}

/*
 * Multishot Operations
 */

int qd_aio_accept_multi(qd_aio_loop_t *loop, int fd, void *user_data)
{
    if (!(qd_aio_backend_features() & QD_FEAT_MULTISHOT_ACCEPT)) {
        qd_log_warn("multishot accept not supported, falling back to regular accept");
        return qd_aio_accept(loop, fd, NULL, NULL, user_data);
    }

    qd_aio_req_t req = {
        .op = QD_OP_ACCEPT,
        .fd = fd,
        .user_data = user_data,
        .flags = QD_AIO_FLAG_MULTISHOT,
    };
    return qd_aio_submit(loop, &req);
}

int qd_aio_recv_multi(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                      int flags, void *user_data)
{
    if (!(qd_aio_backend_features() & QD_FEAT_MULTISHOT_RECV)) {
        qd_log_warn("multishot recv not supported, falling back to regular recv");
        return qd_aio_recv(loop, fd, buf, len, flags, user_data);
    }

    qd_aio_req_t req = {
        .op = QD_OP_RECV,
        .fd = fd,
        .buf = buf,
        .len = len,
        .aux = flags,
        .user_data = user_data,
        .flags = QD_AIO_FLAG_MULTISHOT,
    };
    return qd_aio_submit(loop, &req);
}

/*
 * Backend Information
 */

const char *qd_aio_backend_name(void)
{
    return QD_BACKEND_DEFAULT->name;
}

uint32_t qd_aio_backend_features(void)
{
    return QD_BACKEND_DEFAULT->features;
}

/*
 * Statistics
 */

void qd_aio_stats(qd_aio_loop_t *loop, qd_aio_stats_t *stats)
{
    if (!loop || !stats)
        return;

    stats->submit_count = loop->submit_count;
    stats->complete_count = loop->complete_count;
    stats->error_count = loop->error_count;
    stats->pending = loop->backend->pending(loop->backend_ctx);
}

/*
 * Channel watching
 */
int qd_aio_channel_watch(qd_aio_loop_t *loop, qd_channel_t *chan,
                         void *user_data)
{
    if (!loop || !chan) {
        return QD_ERR_INVAL;
    }

    return loop->backend->watch_fd(loop->backend_ctx,
                                   qd_channel_fd(chan), user_data);
}

int qd_aio_channel_unwatch(qd_aio_loop_t *loop, qd_channel_t *chan)
{
    if (!loop || !chan) {
        return QD_ERR_INVAL;
    }

    return loop->backend->unwatch_fd(loop->backend_ctx,
                                     qd_channel_fd(chan));
}
