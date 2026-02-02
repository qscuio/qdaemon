/*
 * QDaemon - Work Queue API
 * Libuv-style pattern for offloading work to background threads
 */

#ifndef QD_WORKQUEUE_H
#define QD_WORKQUEUE_H

#include <stddef.h>
#include <stdint.h>
#include "qd_common.h"
#include "qdaemon/qd_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_aio_loop qd_aio_loop_t;
typedef struct qd_threadpool qd_threadpool_t;

typedef struct qd_work qd_work_t;
typedef struct qd_workqueue qd_workqueue_t;

/* Work function signatures */
typedef void *(*qd_work_fn_t)(void *arg);
typedef void (*qd_after_work_fn_t)(void *result, int status, void *arg);

/* Status codes for work completion */
#define QD_WORK_OK        0       /* Success */
#define QD_WORK_CANCELLED -1      /* Cancelled before execution */
#define QD_WORK_TIMEOUT   -2      /* Work function took too long (optional) */
#define QD_WORK_ERROR     -3      /* Work function returned NULL with errno set */

/* Work request */
struct qd_work {
    qd_work_fn_t work;          /* Runs in worker thread */
    void *arg;                  /* Argument to work function */
    void *result;               /* Set by work function return */
    int status;                 /* 0 = success, -errno on error */
    uint64_t delay_ms;          /* Delay before execution (0 = immediate) */
    void *user_data;            /* Passed through to completion */
    qd_workqueue_t *wq;       /* Owning workqueue (internal) */
};

/* Create work queue bound to loop and pool */
qd_workqueue_t *qd_workqueue_create(qd_aio_loop_t *loop,
                                     qd_threadpool_t *pool,
                                     qd_after_work_fn_t on_complete,
                                     void *complete_arg);

void qd_workqueue_destroy(qd_workqueue_t *wq);

/* Submit work */
int qd_workqueue_submit(qd_workqueue_t *wq, qd_work_fn_t work, void *arg);

/* Submit with delay */
int qd_workqueue_submit_delayed(qd_workqueue_t *wq, qd_work_fn_t work,
                                 void *arg, uint64_t delay_ms);

/* Submit full work struct for advanced use */
int qd_workqueue_submit_work(qd_workqueue_t *wq, qd_work_t *work);

/* Pending work count */
int qd_workqueue_pending(qd_workqueue_t *wq);

/* Cancellation */
typedef struct qd_work_handle qd_work_handle_t;

/* Submit and get handle for cancellation */
qd_work_handle_t *qd_workqueue_submit_cancellable(qd_workqueue_t *wq,
                                                   qd_work_fn_t work,
                                                   void *arg);

/* Cancel pending work (no-op if already running/completed) */
int qd_workqueue_cancel(qd_work_handle_t *handle);

/* Release handle (must call even after cancel) */
void qd_work_handle_release(qd_work_handle_t *handle);

/* Internal channel access (for workqueue implementation) */
qd_channel_t *qd_workqueue_channel(qd_workqueue_t *wq);

#ifdef __cplusplus
}
#endif

#endif /* QD_WORKQUEUE_H */
