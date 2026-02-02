/*
 * QDaemon - io_uring Backend
 * Native async I/O using Linux io_uring
 */

#ifdef QD_BACKEND_URING

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "qd_backend.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* io_uring backend context */
typedef struct qd_ur_backend {
    struct io_uring ring;
    int pending;
    uint32_t features;
    int queue_depth;
} qd_ur_backend_t;

static void *qd_ur_init(int queue_depth)
{
    qd_ur_backend_t *ur = qd_calloc(1, sizeof(*ur));
    if (!ur)
        return NULL;

    ur->queue_depth = queue_depth > 0 ? queue_depth : 256;

    struct io_uring_params params = {0};

    /* Try to enable kernel-side polling if available */
#ifdef QD_URING_HAS_SQPOLL
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 1000; /* 1 second idle before sleeping */
#endif

    int ret = io_uring_queue_init_params(ur->queue_depth, &ur->ring, &params);
    if (ret < 0) {
        /* Retry without SQPOLL */
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(ur->queue_depth, &ur->ring, &params);
        if (ret < 0) {
            qd_log_error("io_uring_queue_init failed: %s", strerror(-ret));
            qd_free(ur);
            return NULL;
        }
    }

    /* Record supported features */
    ur->features = QD_FEAT_ASYNC_IO;

#ifdef QD_URING_HAS_MULTISHOT_ACCEPT
    ur->features |= QD_FEAT_MULTISHOT_ACCEPT;
#endif
#ifdef QD_URING_HAS_MULTISHOT_RECV
    ur->features |= QD_FEAT_MULTISHOT_RECV;
#endif
    if (params.flags & IORING_SETUP_SQPOLL)
        ur->features |= QD_FEAT_SQPOLL;

    qd_log_debug("io_uring backend initialized, depth=%d, features=0x%x",
                 ur->queue_depth, ur->features);

    return ur;
}

static void qd_ur_destroy(void *ctx)
{
    qd_ur_backend_t *ur = ctx;
    if (!ur)
        return;

    io_uring_queue_exit(&ur->ring);
    qd_free(ur);
}

static int qd_ur_submit(void *ctx, qd_aio_req_t *req)
{
    qd_ur_backend_t *ur = ctx;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ur->ring);
    if (!sqe) {
        qd_log_warn("io_uring SQ full");
        return QD_ERR_BUSY;
    }

    switch (req->op) {
    case QD_OP_NOP:
        io_uring_prep_nop(sqe);
        break;

    case QD_OP_READ:
        if (req->offset > 0)
            io_uring_prep_read(sqe, req->fd, req->buf, req->len, req->offset);
        else
            io_uring_prep_read(sqe, req->fd, req->buf, req->len, 0);
        break;

    case QD_OP_WRITE:
        if (req->offset > 0)
            io_uring_prep_write(sqe, req->fd, req->buf, req->len, req->offset);
        else
            io_uring_prep_write(sqe, req->fd, req->buf, req->len, 0);
        break;

    case QD_OP_POLL:
        io_uring_prep_poll_add(sqe, req->fd, req->aux);
        break;

    case QD_OP_ACCEPT:
        if (req->flags & QD_AIO_FLAG_MULTISHOT) {
#ifdef QD_URING_HAS_MULTISHOT_ACCEPT
            io_uring_prep_multishot_accept(sqe, req->fd,
                                           (struct sockaddr *)req->buf,
                                           (socklen_t *)&req->len, 0);
#else
            io_uring_prep_accept(sqe, req->fd, (struct sockaddr *)req->buf,
                                (socklen_t *)&req->len, 0);
#endif
        } else {
            io_uring_prep_accept(sqe, req->fd, (struct sockaddr *)req->buf,
                                (socklen_t *)&req->len, 0);
        }
        break;

    case QD_OP_CONNECT:
        io_uring_prep_connect(sqe, req->fd, (struct sockaddr *)req->buf, req->len);
        break;

    case QD_OP_SEND:
        io_uring_prep_send(sqe, req->fd, req->buf, req->len, req->aux);
        break;

    case QD_OP_RECV:
        if (req->flags & QD_AIO_FLAG_MULTISHOT) {
#ifdef QD_URING_HAS_MULTISHOT_RECV
            io_uring_prep_recv_multishot(sqe, req->fd, req->buf, req->len, req->aux);
#else
            io_uring_prep_recv(sqe, req->fd, req->buf, req->len, req->aux);
#endif
        } else {
            io_uring_prep_recv(sqe, req->fd, req->buf, req->len, req->aux);
        }
        break;

    case QD_OP_CLOSE:
        io_uring_prep_close(sqe, req->fd);
        break;

    case QD_OP_TIMEOUT:
        {
            /* Convert ms to timespec - stored in user memory */
            struct __kernel_timespec *ts = (struct __kernel_timespec *)req->buf;
            if (ts) {
                ts->tv_sec = req->aux / 1000;
                ts->tv_nsec = (req->aux % 1000) * 1000000;
                io_uring_prep_timeout(sqe, ts, 0, 0);
            } else {
                qd_log_error("timeout requires buffer for timespec");
                return QD_ERR_INVAL;
            }
        }
        break;

    case QD_OP_CANCEL:
        io_uring_prep_cancel64(sqe, (uint64_t)(uintptr_t)req->user_data, 0);
        break;

    default:
        qd_log_error("unsupported operation: %d", req->op);
        return QD_ERR_NOSYS;
    }

    io_uring_sqe_set_data(sqe, req->user_data);

    int ret = io_uring_submit(&ur->ring);
    if (ret < 0) {
        qd_log_error("io_uring_submit failed: %s", strerror(-ret));
        return ret;
    }

    ur->pending++;
    return QD_OK;
}

static int qd_ur_submit_batch(void *ctx, qd_aio_req_t *reqs, int count)
{
    qd_ur_backend_t *ur = ctx;
    int prepared = 0;

    for (int i = 0; i < count; i++) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&ur->ring);
        if (!sqe)
            break;

        qd_aio_req_t *req = &reqs[i];

        /* Prepare SQE based on operation type (same as qd_ur_submit) */
        switch (req->op) {
        case QD_OP_READ:
            io_uring_prep_read(sqe, req->fd, req->buf, req->len, req->offset);
            break;
        case QD_OP_WRITE:
            io_uring_prep_write(sqe, req->fd, req->buf, req->len, req->offset);
            break;
        case QD_OP_ACCEPT:
            io_uring_prep_accept(sqe, req->fd, NULL, NULL, 0);
            break;
        case QD_OP_RECV:
            io_uring_prep_recv(sqe, req->fd, req->buf, req->len, req->aux);
            break;
        case QD_OP_SEND:
            io_uring_prep_send(sqe, req->fd, req->buf, req->len, req->aux);
            break;
        default:
            continue;
        }

        io_uring_sqe_set_data(sqe, req->user_data);
        prepared++;
    }

    if (prepared > 0) {
        int ret = io_uring_submit(&ur->ring);
        if (ret < 0)
            return ret;
        ur->pending += prepared;
    }

    return prepared;
}

static int qd_ur_wait(void *ctx, qd_completion_t *out, int max, int timeout_ms)
{
    qd_ur_backend_t *ur = ctx;
    struct io_uring_cqe *cqe;
    int count = 0;

    /* Set up timeout */
    struct __kernel_timespec ts;
    struct __kernel_timespec *pts = NULL;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        pts = &ts;
    }

    /* Wait for at least one completion */
    int ret = io_uring_wait_cqe_timeout(&ur->ring, &cqe, pts);
    if (ret < 0) {
        if (ret == -ETIME || ret == -EINTR)
            return 0;
        return ret;
    }

    /* Process first completion */
    out[count].user_data = io_uring_cqe_get_data(cqe);
    out[count].result = cqe->res;
    out[count].flags = 0;

    /* Check for multishot flag */
    if (cqe->flags & IORING_CQE_F_MORE)
        out[count].flags |= QD_CQE_FLAG_MORE;

    /* Determine operation type from result (heuristic) */
    out[count].op = QD_OP_NOP; /* Caller typically tracks this via user_data */

    io_uring_cqe_seen(&ur->ring, cqe);

    /* Only decrement pending if not multishot */
    if (!(out[count].flags & QD_CQE_FLAG_MORE))
        ur->pending--;

    count++;

    /* Peek for more completions */
    while (count < max) {
        ret = io_uring_peek_cqe(&ur->ring, &cqe);
        if (ret < 0)
            break;

        out[count].user_data = io_uring_cqe_get_data(cqe);
        out[count].result = cqe->res;
        out[count].flags = 0;

        if (cqe->flags & IORING_CQE_F_MORE)
            out[count].flags |= QD_CQE_FLAG_MORE;

        out[count].op = QD_OP_NOP;

        io_uring_cqe_seen(&ur->ring, cqe);

        if (!(out[count].flags & QD_CQE_FLAG_MORE))
            ur->pending--;

        count++;
    }

    return count;
}

static int qd_ur_pending(void *ctx)
{
    qd_ur_backend_t *ur = ctx;
    return ur->pending;
}

/* Export backend ops */
const qd_backend_ops_t qd_backend_uring = {
    .name         = "io_uring",
    .features     = QD_FEAT_ASYNC_IO | QD_FEAT_MULTISHOT_ACCEPT | QD_FEAT_MULTISHOT_RECV,
    .init         = qd_ur_init,
    .destroy      = qd_ur_destroy,
    .submit       = qd_ur_submit,
    .submit_batch = qd_ur_submit_batch,
    .wait         = qd_ur_wait,
    .pending      = qd_ur_pending,
};

#endif /* QD_BACKEND_URING */
