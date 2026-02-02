/*
 * QDaemon - Epoll Backend
 * Proactor pattern emulation using epoll
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "qd_backend.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Pending operation node */
typedef struct qd_ep_pending {
    qd_aio_req_t req;
    struct qd_ep_pending *next;
    struct qd_ep_pending *prev;
    int registered;               /* Is fd registered with epoll */
    int is_watch;                /* Is this a watched fd (for channel watch) */
    void *watch_user_data;       /* user_data for watched fd completion */
} qd_ep_pending_t;

/* Epoll backend context */
typedef struct qd_ep_backend {
    int epoll_fd;
    struct epoll_event *events;
    int max_events;
    qd_ep_pending_t *pending_head;
    qd_ep_pending_t *pending_tail;
    int pending_count;
} qd_ep_backend_t;

/* Convert operation to epoll events */
static uint32_t qd_ep_op_to_events(qd_op_type_t op)
{
    switch (op) {
    case QD_OP_READ:
    case QD_OP_RECV:
    case QD_OP_ACCEPT:
        return EPOLLIN;
    case QD_OP_WRITE:
    case QD_OP_SEND:
    case QD_OP_CONNECT:
        return EPOLLOUT;
    case QD_OP_POLL:
        return EPOLLIN | EPOLLOUT;
    case QD_OP_CHANNEL:
        return EPOLLIN;
    default:
        return 0;
    }
}

/* Find pending op by fd */
static qd_ep_pending_t *qd_ep_find_pending(qd_ep_backend_t *ep, int fd)
{
    qd_ep_pending_t *p = ep->pending_head;
    while (p) {
        if (p->req.fd == fd)
            return p;
        p = p->next;
    }
    return NULL;
}

/* Remove pending from list */
static void qd_ep_remove_pending(qd_ep_backend_t *ep, qd_ep_pending_t *p)
{
    if (p->prev)
        p->prev->next = p->next;
    else
        ep->pending_head = p->next;

    if (p->next)
        p->next->prev = p->prev;
    else
        ep->pending_tail = p->prev;

    ep->pending_count--;
}

static void *qd_ep_init(int queue_depth)
{
    qd_ep_backend_t *ep = qd_calloc(1, sizeof(*ep));
    if (!ep)
        return NULL;

    ep->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ep->epoll_fd == -1) {
        qd_log_error("epoll_create1 failed: %s", strerror(errno));
        qd_free(ep);
        return NULL;
    }

    ep->max_events = queue_depth > 0 ? queue_depth : 256;
    ep->events = qd_calloc(ep->max_events, sizeof(struct epoll_event));
    if (!ep->events) {
        close(ep->epoll_fd);
        qd_free(ep);
        return NULL;
    }

    qd_log_debug("epoll backend initialized, max_events=%d", ep->max_events);
    return ep;
}

static void qd_ep_destroy(void *ctx)
{
    qd_ep_backend_t *ep = ctx;
    if (!ep)
        return;

    /* Free all pending operations */
    qd_ep_pending_t *p = ep->pending_head;
    while (p) {
        qd_ep_pending_t *next = p->next;
        qd_free(p);
        p = next;
    }

    if (ep->epoll_fd != -1)
        close(ep->epoll_fd);

    qd_free(ep->events);
    qd_free(ep);
}

static int qd_ep_submit(void *ctx, qd_aio_req_t *req)
{
    qd_ep_backend_t *ep = ctx;

    /* Handle special operations that don't need epoll */
    if (req->op == QD_OP_NOP) {
        return QD_OK;
    }

    if (req->op == QD_OP_CLOSE) {
        /* Close is synchronous */
        close(req->fd);
        return QD_OK;
    }

    if (req->op == QD_OP_TIMEOUT) {
        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0)
            return -errno;

        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = req->aux / 1000;
        its.it_value.tv_nsec = (req->aux % 1000) * 1000000;
        if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
            int err = -errno;
            close(tfd);
            return err;
        }

        req->fd = tfd;
    }

    if (req->op == QD_OP_CANCEL) {
        /* Find and remove pending operation */
        qd_ep_pending_t *p = ep->pending_head;
        while (p) {
            if (p->req.user_data == req->user_data) {
                if (p->registered) {
                    epoll_ctl(ep->epoll_fd, EPOLL_CTL_DEL, p->req.fd, NULL);
                }
                if (p->req.op == QD_OP_TIMEOUT) {
                    close(p->req.fd);
                }
                qd_ep_remove_pending(ep, p);
                qd_free(p);
                return QD_OK;
            }
            p = p->next;
        }
        return QD_ERR_NOENT;
    }

    /* Create pending operation */
    qd_ep_pending_t *pending = qd_calloc(1, sizeof(*pending));
    if (!pending)
        return QD_ERR_NOMEM;

    pending->req = *req;
    pending->next = NULL;
    pending->prev = NULL;
    pending->registered = 0;

    /* Add to pending list */
    if (ep->pending_tail)
        ep->pending_tail->next = pending;
    else
        ep->pending_head = pending;
    ep->pending_tail = pending;
    ep->pending_count++;

    /* Register with epoll */
    uint32_t events = qd_ep_op_to_events(req->op);
    if (events) {
        struct epoll_event ev = {
            .events = events | EPOLLONESHOT,
            .data.ptr = pending
        };

        int op = EPOLL_CTL_ADD;
        qd_ep_pending_t *existing = qd_ep_find_pending(ep, req->fd);
        if (existing && existing != pending && existing->registered) {
            op = EPOLL_CTL_MOD;
        }

        if (epoll_ctl(ep->epoll_fd, op, req->fd, &ev) == -1) {
            if (errno == EEXIST) {
                /* Try MOD instead */
                if (epoll_ctl(ep->epoll_fd, EPOLL_CTL_MOD, req->fd, &ev) == -1) {
                    qd_log_error("epoll_ctl MOD failed: %s", strerror(errno));
                    qd_ep_remove_pending(ep, pending);
                    qd_free(pending);
                    return -errno;
                }
            } else {
                qd_log_error("epoll_ctl ADD failed: %s", strerror(errno));
                qd_ep_remove_pending(ep, pending);
                qd_free(pending);
                return -errno;
            }
        }
    }

    pending->registered = 1;
    return QD_OK;
}

static int qd_ep_submit_batch(void *ctx, qd_aio_req_t *reqs, int count)
{
    int submitted = 0;
    for (int i = 0; i < count; i++) {
        int ret = qd_ep_submit(ctx, &reqs[i]);
        if (ret < 0)
            return ret;
        submitted++;
    }
    return submitted;
}

/* Perform the actual I/O operation */
static int qd_ep_do_io(qd_aio_req_t *req)
{
    int result;

    switch (req->op) {
    case QD_OP_READ:
        if (req->offset > 0)
            result = pread(req->fd, req->buf, req->len, req->offset);
        else
            result = read(req->fd, req->buf, req->len);
        break;

    case QD_OP_WRITE:
        if (req->offset > 0)
            result = pwrite(req->fd, req->buf, req->len, req->offset);
        else
            result = write(req->fd, req->buf, req->len);
        break;

    case QD_OP_ACCEPT:
        result = accept4(req->fd, (struct sockaddr *)req->buf,
                        (socklen_t *)&req->len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        break;

    case QD_OP_CONNECT:
        /* Connect is already in progress, check result */
        {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(req->fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
                result = -1;
            else if (error)
                result = -1;
            else
                result = 0;
        }
        break;

    case QD_OP_SEND:
        result = send(req->fd, req->buf, req->len, req->aux);
        break;

    case QD_OP_RECV:
        result = recv(req->fd, req->buf, req->len, req->aux);
        break;

    case QD_OP_POLL:
        /* Poll completed, just return success */
        result = 0;
        break;

    case QD_OP_TIMEOUT:
        {
            uint64_t expirations = 0;
            result = read(req->fd, &expirations, sizeof(expirations));
            if (result < 0) {
                /* If already drained, treat as success */
                if (errno == EAGAIN)
                    result = 0;
            } else {
                result = 0;
            }
        }
        break;

    case QD_OP_CHANNEL:
        /* Channel operation - handled specially in wait */
        result = 0;
        break;

    default:
        result = -1;
        errno = ENOTSUP;
        break;
    }

    return result;
}

static int qd_ep_wait(void *ctx, qd_completion_t *out, int max, int timeout_ms)
{
    qd_ep_backend_t *ep = ctx;

    int nfds = epoll_wait(ep->epoll_fd, ep->events,
                          QD_MIN(max, ep->max_events), timeout_ms);

    if (nfds < 0) {
        if (errno == EINTR)
            return 0;
        return -errno;
    }

    int count = 0;
    for (int i = 0; i < nfds && count < max; i++) {
        qd_ep_pending_t *pending = ep->events[i].data.ptr;
        if (!pending)
            continue;

        qd_aio_req_t *req = &pending->req;

        /* Handle watched file descriptors (channel watch) */
        if (pending->is_watch) {
            /* Channel/eventfd is readable - generate completion for user to drain */
            out[count].user_data = pending->watch_user_data;
            out[count].result = 0;
            out[count].flags = 0;
            out[count].op = QD_OP_CHANNEL;
            count++;
            /* Keep watching - don't remove from epoll */
            continue;
        }

        /* Regular I/O operations */
        int result = qd_ep_do_io(req);

        /* Fill completion */
        out[count].user_data = req->user_data;
        out[count].result = (result < 0) ? -errno : result;
        out[count].flags = 0;
        out[count].op = req->op;
        count++;

        /* Remove from epoll and pending list */
        epoll_ctl(ep->epoll_fd, EPOLL_CTL_DEL, req->fd, NULL);
        if (req->op == QD_OP_TIMEOUT) {
            close(req->fd);
        }
        qd_ep_remove_pending(ep, pending);
        qd_free(pending);
    }

    return count;
}

static int qd_ep_pending(void *ctx)
{
    qd_ep_backend_t *ep = ctx;
    return ep->pending_count;
}

/* Watch arbitrary file descriptor */
static int qd_ep_watch_fd(void *ctx, int fd, void *user_data)
{
    qd_ep_backend_t *ep = ctx;

    /* Create a pending node for the watched fd */
    qd_ep_pending_t *pending = qd_calloc(1, sizeof(*pending));
    if (!pending)
        return QD_ERR_NOMEM;

    pending->req.op = QD_OP_CHANNEL;
    pending->req.fd = fd;
    pending->req.user_data = user_data;
    pending->is_watch = 1;
    pending->watch_user_data = user_data;

    /* Add to pending list */
    if (ep->pending_tail)
        ep->pending_tail->next = pending;
    else
        ep->pending_head = pending;
    ep->pending_tail = pending;
    ep->pending_count++;

    /* Register with epoll */
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = pending
    };

    if (epoll_ctl(ep->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        qd_ep_remove_pending(ep, pending);
        qd_free(pending);
        return -errno;
    }

    pending->registered = 1;
    return QD_OK;
}

/* Unwatch file descriptor */
static int qd_ep_unwatch_fd(void *ctx, int fd)
{
    qd_ep_backend_t *ep = ctx;

    /* Find and remove pending operation */
    qd_ep_pending_t *p = ep->pending_head;
    while (p) {
        if (p->req.fd == fd && p->is_watch) {
            if (p->registered) {
                epoll_ctl(ep->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
            }
            qd_ep_remove_pending(ep, p);
            qd_free(p);
            return QD_OK;
        }
        p = p->next;
    }

    return QD_ERR_NOENT;
}

/* Export backend ops */
const qd_backend_ops_t qd_backend_epoll = {
    .name         = "epoll",
    .features     = QD_FEAT_ASYNC_IO,
    .init         = qd_ep_init,
    .destroy      = qd_ep_destroy,
    .submit       = qd_ep_submit,
    .submit_batch = qd_ep_submit_batch,
    .wait         = qd_ep_wait,
    .pending      = qd_ep_pending,
    .watch_fd     = qd_ep_watch_fd,
    .unwatch_fd   = qd_ep_unwatch_fd,
};
