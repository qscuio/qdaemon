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

    /* Wait for completions */
    /* Returns: number of completions, or -errno on error */
    /* timeout_ms: -1 for infinite, 0 for non-blocking, >0 for timeout */
    int (*wait)(void *ctx, qd_completion_t *out, int max, int timeout_ms);

    /* Get number of pending operations */
    int (*pending)(void *ctx);

    /* Watch arbitrary file descriptor for read readiness */
    int (*watch_fd)(void *ctx, int fd, void *user_data);

    /* Unwatch file descriptor */
    int (*unwatch_fd)(void *ctx, int fd);
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
