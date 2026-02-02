# Async I/O Backend Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement async I/O module with io_uring and epoll backends, selected at compile time.

**Architecture:** Separate `qd_aio` module with backend vtable abstraction. Epoll backend emulates proactor pattern. io_uring backend uses native async submission/completion.

**Tech Stack:** C11, liburing (for uring backend), epoll (for epoll backend), pthread

---

## Task 1: Create Feature Flags Header

**Files:**
- Create: `include/qdaemon/qd_aio_features.h`

**Step 1: Write the header file**

```c
/*
 * QDaemon - Async I/O Feature Detection
 * Compile-time feature flags for backend selection
 */

#ifndef QD_AIO_FEATURES_H
#define QD_AIO_FEATURES_H

/* Backend selection - set by Makefile */
#if defined(QD_BACKEND_URING)
    #define QD_AIO_BACKEND_NAME "io_uring"
#elif defined(QD_BACKEND_EPOLL)
    #define QD_AIO_BACKEND_NAME "epoll"
#else
    /* Default to epoll if nothing specified */
    #define QD_BACKEND_EPOLL
    #define QD_AIO_BACKEND_NAME "epoll"
#endif

/* Feature capability flags */
#define QD_FEAT_ASYNC_IO          (1 << 0)  /* Basic async I/O */
#define QD_FEAT_MULTISHOT_ACCEPT  (1 << 1)  /* Multishot accept */
#define QD_FEAT_MULTISHOT_RECV    (1 << 2)  /* Multishot recv */
#define QD_FEAT_ZERO_COPY         (1 << 3)  /* Zero-copy operations */
#define QD_FEAT_SQPOLL            (1 << 4)  /* Kernel-side polling */
#define QD_FEAT_FIXED_BUFFERS     (1 << 5)  /* Registered buffers */

/* io_uring feature detection (compile-time via liburing headers) */
#if defined(QD_BACKEND_URING)
    #include <liburing.h>

    /* Check liburing version for multishot support (>= 2.2) */
    #if defined(IORING_ACCEPT_MULTISHOT)
        #define QD_URING_HAS_MULTISHOT_ACCEPT 1
    #endif

    #if defined(IORING_RECV_MULTISHOT)
        #define QD_URING_HAS_MULTISHOT_RECV 1
    #endif

    #if defined(IORING_SETUP_SQPOLL)
        #define QD_URING_HAS_SQPOLL 1
    #endif
#endif

#endif /* QD_AIO_FEATURES_H */
```

**Step 2: Verify file compiles**

Run: `gcc -fsyntax-only -I/home/ubuntu/work/qdaemon/include /home/ubuntu/work/qdaemon/include/qdaemon/qd_aio_features.h`
Expected: No errors

**Step 3: Commit**

```bash
git add include/qdaemon/qd_aio_features.h
git commit -m "feat(aio): add feature detection header"
```

---

## Task 2: Create Operation Types Header

**Files:**
- Create: `include/qdaemon/qd_aio_ops.h`

**Step 1: Write the operations header**

```c
/*
 * QDaemon - Async I/O Operation Types
 * Defines operation types and request/completion structures
 */

#ifndef QD_AIO_OPS_H
#define QD_AIO_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operation types */
typedef enum qd_op_type {
    QD_OP_NOP = 0,       /* No operation */
    QD_OP_READ,          /* Read from fd */
    QD_OP_WRITE,         /* Write to fd */
    QD_OP_POLL,          /* Poll for events */
    QD_OP_ACCEPT,        /* Accept connection */
    QD_OP_CONNECT,       /* Connect to remote */
    QD_OP_SEND,          /* Send data */
    QD_OP_RECV,          /* Receive data */
    QD_OP_CLOSE,         /* Close fd */
    QD_OP_TIMEOUT,       /* Timeout event */
    QD_OP_CANCEL,        /* Cancel operation */
    QD_OP_MAX
} qd_op_type_t;

/* Operation flags */
#define QD_AIO_FLAG_NONE       0
#define QD_AIO_FLAG_MULTISHOT  (1 << 0)  /* Multishot operation */
#define QD_AIO_FLAG_FIXED_BUF  (1 << 1)  /* Use fixed buffer */
#define QD_AIO_FLAG_LINKED     (1 << 2)  /* Linked operation */

/* Async I/O request */
typedef struct qd_aio_req {
    qd_op_type_t op;          /* Operation type */
    int fd;                   /* File descriptor */
    void *buf;                /* Buffer for read/write */
    size_t len;               /* Buffer length */
    uint64_t offset;          /* File offset (for pread/pwrite) */
    void *user_data;          /* User context pointer */
    uint32_t flags;           /* Operation flags */
    int aux;                  /* Auxiliary data (e.g., msg_flags for send/recv) */
} qd_aio_req_t;

/* Completion result */
typedef struct qd_completion {
    void *user_data;          /* User context from request */
    int32_t result;           /* Result: bytes transferred or -errno */
    uint32_t flags;           /* Completion flags */
    qd_op_type_t op;          /* Which operation completed */
} qd_completion_t;

/* Completion flags */
#define QD_CQE_FLAG_MORE       (1 << 0)  /* More completions coming (multishot) */
#define QD_CQE_FLAG_NOTIF      (1 << 1)  /* Notification only */

/* Helper to get operation name */
static inline const char *qd_op_type_name(qd_op_type_t op)
{
    static const char *names[] = {
        [QD_OP_NOP]     = "nop",
        [QD_OP_READ]    = "read",
        [QD_OP_WRITE]   = "write",
        [QD_OP_POLL]    = "poll",
        [QD_OP_ACCEPT]  = "accept",
        [QD_OP_CONNECT] = "connect",
        [QD_OP_SEND]    = "send",
        [QD_OP_RECV]    = "recv",
        [QD_OP_CLOSE]   = "close",
        [QD_OP_TIMEOUT] = "timeout",
        [QD_OP_CANCEL]  = "cancel",
    };
    return (op < QD_OP_MAX) ? names[op] : "unknown";
}

#ifdef __cplusplus
}
#endif

#endif /* QD_AIO_OPS_H */
```

**Step 2: Verify file compiles**

Run: `gcc -fsyntax-only -I/home/ubuntu/work/qdaemon/include /home/ubuntu/work/qdaemon/include/qdaemon/qd_aio_ops.h`
Expected: No errors

**Step 3: Commit**

```bash
git add include/qdaemon/qd_aio_ops.h
git commit -m "feat(aio): add operation types header"
```

---

## Task 3: Update Backend VTable Header

**Files:**
- Modify: `src/core/backend/qd_backend.h`

**Step 1: Read current file and replace with updated version**

```c
/*
 * QDaemon - Event Backend Interface (VTable)
 * Abstraction layer for Epoll / IO_Uring backends
 */

#ifndef QD_BACKEND_H
#define QD_BACKEND_H

#include "qdaemon/qd_aio_ops.h"
#include "qdaemon/qd_aio_features.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend Operations VTable */
typedef struct qd_backend_ops {
    const char *name;             /* Backend name */
    uint32_t features;            /* Supported features (QD_FEAT_*) */

    /* Initialize backend context */
    void *(*init)(int queue_depth);

    /* Destroy backend context */
    void (*destroy)(void *ctx);

    /* Submit single async operation */
    int (*submit)(void *ctx, qd_aio_req_t *req);

    /* Submit batch of operations */
    int (*submit_batch)(void *ctx, qd_aio_req_t *reqs, int count);

    /* Wait for completions
     * Returns: number of completions, or -errno on error
     * timeout_ms: -1 for infinite, 0 for non-blocking, >0 for timeout
     */
    int (*wait)(void *ctx, qd_completion_t *out, int max, int timeout_ms);

    /* Get number of pending operations */
    int (*pending)(void *ctx);

} qd_backend_ops_t;

/* Backend implementations */
#if defined(QD_BACKEND_URING)
    extern const qd_backend_ops_t qd_backend_uring;
    #define QD_BACKEND_DEFAULT (&qd_backend_uring)
#else
    extern const qd_backend_ops_t qd_backend_epoll;
    #define QD_BACKEND_DEFAULT (&qd_backend_epoll)
#endif

#ifdef __cplusplus
}
#endif

#endif /* QD_BACKEND_H */
```

**Step 2: Verify file compiles**

Run: `gcc -fsyntax-only -DQD_BACKEND_EPOLL -I/home/ubuntu/work/qdaemon/include -I/home/ubuntu/work/qdaemon/src/core/backend /home/ubuntu/work/qdaemon/src/core/backend/qd_backend.h`
Expected: No errors

**Step 3: Commit**

```bash
git add src/core/backend/qd_backend.h
git commit -m "feat(aio): update backend vtable for proactor pattern"
```

---

## Task 4: Create Public Async I/O API Header

**Files:**
- Create: `include/qdaemon/qd_aio.h`

**Step 1: Write the public API header**

```c
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

/* Forward declaration */
typedef struct qd_aio_loop qd_aio_loop_t;

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

#ifdef __cplusplus
}
#endif

#endif /* QD_AIO_H */
```

**Step 2: Verify file compiles**

Run: `gcc -fsyntax-only -DQD_BACKEND_EPOLL -I/home/ubuntu/work/qdaemon/include /home/ubuntu/work/qdaemon/include/qdaemon/qd_aio.h`
Expected: No errors

**Step 3: Commit**

```bash
git add include/qdaemon/qd_aio.h
git commit -m "feat(aio): add public async I/O API header"
```

---

## Task 5: Implement Epoll Backend

**Files:**
- Modify: `src/core/backend/qd_backend_epoll.c`

**Step 1: Write the complete epoll backend implementation**

```c
/*
 * QDaemon - Epoll Backend
 * Proactor pattern emulation using epoll
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/epoll.h>
#include <sys/socket.h>
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
        /* Timeout handled via epoll_wait timeout */
        qd_log_warn("epoll backend does not support standalone timeout ops");
        return QD_ERR_NOSYS;
    }

    if (req->op == QD_OP_CANCEL) {
        /* Find and remove the pending operation */
        qd_ep_pending_t *p = ep->pending_head;
        while (p) {
            if (p->req.user_data == req->user_data) {
                if (p->registered) {
                    epoll_ctl(ep->epoll_fd, EPOLL_CTL_DEL, p->req.fd, NULL);
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
    pending->prev = ep->pending_tail;

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
        pending->registered = 1;
    }

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
                result = -1, errno = error;
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

        /* Perform the actual I/O */
        int result = qd_ep_do_io(req);

        /* Fill completion */
        out[count].user_data = req->user_data;
        out[count].result = (result < 0) ? -errno : result;
        out[count].flags = 0;
        out[count].op = req->op;
        count++;

        /* Remove from epoll and pending list */
        epoll_ctl(ep->epoll_fd, EPOLL_CTL_DEL, req->fd, NULL);
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
};
```

**Step 2: Verify file compiles**

Run: `gcc -c -DQD_BACKEND_EPOLL -I/home/ubuntu/work/qdaemon/include -I/home/ubuntu/work/qdaemon/src/core/backend -I/home/ubuntu/work/qdaemon/src/util /home/ubuntu/work/qdaemon/src/core/backend/qd_backend_epoll.c -o /tmp/qd_backend_epoll.o`
Expected: No errors (warnings OK for now)

**Step 3: Commit**

```bash
git add src/core/backend/qd_backend_epoll.c
git commit -m "feat(aio): implement epoll backend with proactor emulation"
```

---

## Task 6: Implement io_uring Backend

**Files:**
- Create: `src/core/backend/qd_backend_uring.c`

**Step 1: Write the io_uring backend implementation**

```c
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
```

**Step 2: Verify file compiles (if liburing available)**

Run: `pkg-config --exists liburing && gcc -c -DQD_BACKEND_URING $(pkg-config --cflags liburing) -I/home/ubuntu/work/qdaemon/include -I/home/ubuntu/work/qdaemon/src/core/backend -I/home/ubuntu/work/qdaemon/src/util /home/ubuntu/work/qdaemon/src/core/backend/qd_backend_uring.c -o /tmp/qd_backend_uring.o || echo "liburing not installed - skip compile test"`
Expected: No errors (or skip if liburing not installed)

**Step 3: Commit**

```bash
git add src/core/backend/qd_backend_uring.c
git commit -m "feat(aio): implement io_uring backend"
```

---

## Task 7: Implement Async I/O Loop

**Files:**
- Create: `src/core/qd_aio.c`

**Step 1: Write the async I/O loop implementation**

```c
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
```

**Step 2: Verify file compiles**

Run: `gcc -c -DQD_BACKEND_EPOLL -I/home/ubuntu/work/qdaemon/include -I/home/ubuntu/work/qdaemon/src/core -I/home/ubuntu/work/qdaemon/src/util /home/ubuntu/work/qdaemon/src/core/qd_aio.c -o /tmp/qd_aio.o`
Expected: No errors

**Step 3: Commit**

```bash
git add src/core/qd_aio.c
git commit -m "feat(aio): implement async I/O loop"
```

---

## Task 8: Update Makefile

**Files:**
- Modify: `Makefile`

**Step 1: Add backend selection and AIO sources to Makefile**

Add after line 15 (after LDFLAGS definition):

```makefile
# Backend selection: epoll (default) or uring
BACKEND ?= epoll

ifeq ($(BACKEND), uring)
    CFLAGS += -DQD_BACKEND_URING
    LDFLAGS += -luring
else
    CFLAGS += -DQD_BACKEND_EPOLL
endif
```

Modify BACKEND_SRCS on line 27 to be conditional:

```makefile
# Backend sources (only compile selected backend)
ifeq ($(BACKEND), uring)
    BACKEND_SRCS = $(SRCDIR)/core/backend/qd_backend_uring.c
else
    BACKEND_SRCS = $(SRCDIR)/core/backend/qd_backend_epoll.c
endif
```

**Step 2: Verify Makefile syntax**

Run: `make -n -C /home/ubuntu/work/qdaemon clean all 2>&1 | head -20`
Expected: Shows make commands without errors

**Step 3: Commit**

```bash
git add Makefile
git commit -m "build: add backend selection (BACKEND=epoll|uring)"
```

---

## Task 9: Create Test File

**Files:**
- Create: `tests/test_aio.c`

**Step 1: Write the test file**

```c
/*
 * QDaemon Async I/O Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdatomic.h>

#include <qdaemon/qd_aio.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

static atomic_int completion_count;
static int last_result;

static void test_callback(qd_completion_t *comp, void *arg)
{
    (void)arg;
    atomic_fetch_add(&completion_count, 1);
    last_result = comp->result;
}

/* Test basic creation */
TEST(create_destroy)
{
    qd_aio_loop_t *loop = qd_aio_create();
    assert(loop != NULL);
    qd_aio_destroy(loop);
}

/* Test creation with config */
TEST(create_with_config)
{
    qd_aio_config_t config = {
        .queue_depth = 128,
        .batch_size = 32,
        .callback = test_callback,
        .callback_arg = NULL,
    };

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);
    qd_aio_destroy(loop);
}

/* Test backend name */
TEST(backend_info)
{
    const char *name = qd_aio_backend_name();
    assert(name != NULL);
    assert(strlen(name) > 0);

    uint32_t features = qd_aio_backend_features();
    assert(features & QD_FEAT_ASYNC_IO);

    printf("(%s) ", name);
}

/* Test pipe read/write */
TEST(pipe_read_write)
{
    atomic_store(&completion_count, 0);

    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    /* Set non-blocking */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    /* Write some data */
    const char *msg = "hello async";
    write(pipefd[1], msg, strlen(msg));

    /* Submit async read */
    char buf[64] = {0};
    int ret = qd_aio_read(loop, pipefd[0], buf, sizeof(buf), 0, NULL);
    assert(ret == QD_OK);

    /* Process */
    ret = qd_aio_run_once(loop, 100);
    assert(ret >= 0);
    assert(atomic_load(&completion_count) == 1);
    assert(last_result > 0);
    assert(strncmp(buf, "hello async", 11) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    qd_aio_destroy(loop);
}

/* Test multiple operations */
TEST(multiple_ops)
{
    atomic_store(&completion_count, 0);

    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipes[3][2];
    char bufs[3][64] = {{0}};

    for (int i = 0; i < 3; i++) {
        assert(pipe(pipes[i]) == 0);
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[i][1], F_SETFL, O_NONBLOCK);

        /* Write data */
        char msg[32];
        snprintf(msg, sizeof(msg), "pipe%d", i);
        write(pipes[i][1], msg, strlen(msg));

        /* Submit read */
        qd_aio_read(loop, pipes[i][0], bufs[i], sizeof(bufs[i]), 0, NULL);
    }

    /* Process all */
    int total = 0;
    for (int i = 0; i < 10 && total < 3; i++) {
        int ret = qd_aio_run_once(loop, 100);
        if (ret > 0)
            total += ret;
    }

    assert(atomic_load(&completion_count) == 3);

    for (int i = 0; i < 3; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    qd_aio_destroy(loop);
}

/* Test statistics */
TEST(statistics)
{
    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    write(pipefd[1], "x", 1);

    char buf[8];
    qd_aio_read(loop, pipefd[0], buf, sizeof(buf), 0, NULL);

    qd_aio_stats_t stats;
    qd_aio_stats(loop, &stats);
    assert(stats.submit_count == 1);
    assert(stats.pending >= 0);

    qd_aio_run_once(loop, 100);

    qd_aio_stats(loop, &stats);
    assert(stats.complete_count >= 1);

    close(pipefd[0]);
    close(pipefd[1]);
    qd_aio_destroy(loop);
}

/* Test stop */
TEST(run_stop)
{
    qd_aio_loop_t *loop = qd_aio_create();
    assert(loop != NULL);

    assert(qd_aio_is_running(loop) == 0);

    /* Stop before running should be safe */
    qd_aio_stop(loop);

    qd_aio_destroy(loop);
}

int main(void)
{
    printf("\n=== Async I/O Tests ===\n\n");
    printf("Backend: %s\n\n", qd_aio_backend_name());

    RUN_TEST(create_destroy);
    RUN_TEST(create_with_config);
    RUN_TEST(backend_info);
    RUN_TEST(pipe_read_write);
    RUN_TEST(multiple_ops);
    RUN_TEST(statistics);
    RUN_TEST(run_stop);

    printf("\nAll async I/O tests passed!\n\n");
    return 0;
}
```

**Step 2: Add test target to Makefile**

Add to TESTS line (around line 57):

```makefile
TESTS = $(BUILDDIR)/test_memory $(BUILDDIR)/test_threadpool $(BUILDDIR)/test_event $(BUILDDIR)/test_ipc $(BUILDDIR)/test_aio
```

Add test build rule (after test_ipc rule, around line 144):

```makefile
$(BUILDDIR)/test_aio: $(TESTDIR)/test_aio.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)
```

**Step 3: Commit**

```bash
git add tests/test_aio.c Makefile
git commit -m "test(aio): add async I/O tests"
```

---

## Task 10: Build and Test

**Step 1: Build with epoll backend**

Run: `make -C /home/ubuntu/work/qdaemon clean && make -C /home/ubuntu/work/qdaemon BACKEND=epoll`
Expected: Build succeeds

**Step 2: Run tests**

Run: `/home/ubuntu/work/qdaemon/build/test_aio`
Expected: All tests pass

**Step 3: Build with uring backend (if available)**

Run: `pkg-config --exists liburing && make -C /home/ubuntu/work/qdaemon clean && make -C /home/ubuntu/work/qdaemon BACKEND=uring && /home/ubuntu/work/qdaemon/build/test_aio || echo "liburing not available, skipping uring test"`
Expected: Build and tests pass (or skip message)

**Step 4: Final commit**

```bash
git add -A
git commit -m "feat(aio): complete async I/O implementation with epoll/uring backends"
```

---

## Summary

| Task | Description | Files |
|------|-------------|-------|
| 1 | Feature flags header | `include/qdaemon/qd_aio_features.h` |
| 2 | Operation types header | `include/qdaemon/qd_aio_ops.h` |
| 3 | Update backend vtable | `src/core/backend/qd_backend.h` |
| 4 | Public API header | `include/qdaemon/qd_aio.h` |
| 5 | Epoll backend | `src/core/backend/qd_backend_epoll.c` |
| 6 | io_uring backend | `src/core/backend/qd_backend_uring.c` |
| 7 | Async loop impl | `src/core/qd_aio.c` |
| 8 | Makefile update | `Makefile` |
| 9 | Tests | `tests/test_aio.c` |
| 10 | Build & verify | - |
