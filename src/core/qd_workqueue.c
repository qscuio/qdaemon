/*
 * QDaemon - Work Queue Implementation
 * Libuv-style pattern for offloading work to background threads
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

#include "qdaemon/qd_workqueue.h"
#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_aio.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Work wrapper for threadpool compatibility */
typedef struct work_wrapper {
    qd_work_fn_t work_fn;
    void *work_arg;
    qd_work_t *orig_work;  /* Original work struct for callback */
    qd_workqueue_t *wq;
} work_wrapper_t;

/* Work handle for cancellation */
struct qd_work_handle {
    qd_work_t *work;
    _Atomic int cancelled;
    qd_workqueue_t *wq;
};

/* Channel result type (work + status) */
typedef struct result_struct {
    qd_work_t *work;
    int status;
} result_struct_t;

/* Work queue structure */
struct qd_workqueue {
    qd_aio_loop_t *loop;
    qd_threadpool_t *pool;
    qd_channel_t *channel;        /* Completion channel */
    qd_after_work_fn_t on_complete;
    void *complete_arg;
    _Atomic int pending;
    _Atomic int draining;  /* Are we draining for destruction? */
};

/* Thread pool wrapper - runs on worker thread */
static void work_wrapper_fn(void *arg)
{
    work_wrapper_t *wrapper = arg;
    qd_work_t *work = wrapper->orig_work;

    /* Check if cancelled */
    if (atomic_load(&work->status) == QD_WORK_CANCELLED) {
        return;
    }

    /* Run actual work function */
    errno = 0;
    work->result = wrapper->work_fn(wrapper->work_arg);

    if (work->result == NULL && errno != 0) {
        work->status = QD_WORK_ERROR;
    } else {
        work->status = QD_WORK_OK;
    }
}

/* Completion callback on worker thread */
static void work_complete_callback(void *arg, int status)
{
    (void)status;
    work_wrapper_t *wrapper = arg;
    qd_work_t *work = wrapper->orig_work;

    /* Don't send to channel if we're draining */
    if (atomic_load(&wrapper->wq->draining)) {
        qd_free(wrapper);
        return;
    }

    /* Push completion to channel */
    result_struct_t result = {
        .work = work,
        .status = work->status
    };

    qd_channel_send(wrapper->wq->channel, &result);

    /* Free wrapper, but not original work (freed in channel drain) */
    qd_free(wrapper);
}

/* Drain channel and call completions (called from event loop context) */
static void drain_channel_completions(qd_workqueue_t *wq)
{
    result_struct_t result;
    while (qd_channel_try_recv(wq->channel, &result) == QD_OK) {
        qd_work_t *work = result.work;

        if (work->delay_ms > 0) {
            /* Delayed work - freed in delayed_trampoline */
            continue;
        }

        /* Check if cancelled before calling callback */
        if (atomic_load(&work->status) == QD_WORK_CANCELLED) {
            qd_free(work);
        } else if (wq->on_complete) {
            wq->on_complete(work->result, result.status, work->user_data);
            qd_free(work);
        }

        atomic_fetch_sub(&wq->pending, 1);
    }
}

/* Delayed work wrapper for timer completion */
__attribute__((used)) static void delayed_trampoline(qd_completion_t *comp, void *arg)
{
    (void)comp;
    qd_work_t *work = arg;
    qd_workqueue_t *wq = work->wq;

    /* Check if cancelled */
    if (atomic_load(&work->status) == QD_WORK_CANCELLED) {
        qd_free(work);
        return;
    }

    /* Don't submit if we're draining */
    if (atomic_load(&wq->draining)) {
        qd_free(work);
        return;
    }

    /* Submit to thread pool immediately */
    work_wrapper_t *wrapper = qd_calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        qd_free(work);
        return;
    }

    wrapper->work_fn = (qd_work_fn_t)(uintptr_t)work->work;
    wrapper->work_arg = work->arg;
    wrapper->orig_work = work;
    wrapper->wq = wq;

    qd_threadpool_submit_callback(wq->pool, work_wrapper_fn, wrapper,
                                         work_complete_callback, wrapper);
}

qd_workqueue_t *qd_workqueue_create(qd_aio_loop_t *loop,
                                     qd_threadpool_t *pool,
                                     qd_after_work_fn_t on_complete,
                                     void *complete_arg)
{
    if (!loop || !pool) {
        return NULL;
    }

    qd_workqueue_t *wq = qd_calloc(1, sizeof(*wq));
    if (!wq) {
        return NULL;
    }

    wq->loop = loop;
    wq->pool = pool;
    wq->on_complete = on_complete;
    wq->complete_arg = complete_arg;

    atomic_init(&wq->pending, 0);
    atomic_init(&wq->draining, 0);

    /* Create channel for completions (holds result_struct_t) */
    qd_channel_config_t chan_config = {
        .capacity = 1024,
        .item_size = sizeof(result_struct_t),
        .flags = QD_CHAN_NONBLOCK
    };
    wq->channel = qd_channel_create(&chan_config);
    if (!wq->channel) {
        qd_free(wq);
        return NULL;
    }

    qd_log_debug("Created workqueue: loop=%p, pool=%p", loop, pool);

    return wq;
}

void qd_workqueue_destroy(qd_workqueue_t *wq)
{
    if (!wq) {
        return;
    }

    /* Set draining flag to stop new completions */
    atomic_store(&wq->draining, 1);

    /* Drain any pending completions */
    drain_channel_completions(wq);

    qd_channel_destroy(wq->channel);
    qd_free(wq);
}

int qd_workqueue_submit(qd_workqueue_t *wq, qd_work_fn_t work, void *arg)
{
    if (!wq) {
        return QD_ERR_INVAL;
    }

    qd_work_t *item = qd_calloc(1, sizeof(*item));
    if (!item) {
        return QD_ERR_NOMEM;
    }

    /* Store work function in result pointer (hacky but works for our use case) */
    item->work = (qd_work_fn_t)(uintptr_t)work;
    item->arg = arg;
    item->result = NULL;
    item->status = QD_WORK_OK;
    item->delay_ms = 0;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    atomic_fetch_add(&wq->pending, 1);

    /* Create wrapper for threadpool submission */
    work_wrapper_t *wrapper = qd_calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        qd_free(item);
        return QD_ERR_NOMEM;
    }

    wrapper->work_fn = (qd_work_fn_t)(uintptr_t)item->work;
    wrapper->work_arg = item->arg;
    wrapper->orig_work = item;
    wrapper->wq = wq;

    return qd_threadpool_submit_callback(wq->pool, work_wrapper_fn, wrapper,
                                         work_complete_callback, wrapper);
}

int qd_workqueue_submit_delayed(qd_workqueue_t *wq, qd_work_fn_t work,
                                 void *arg, uint64_t delay_ms)
{
    if (!wq) {
        return QD_ERR_INVAL;
    }

    qd_work_t *item = qd_calloc(1, sizeof(*item));
    if (!item) {
        return QD_ERR_NOMEM;
    }

    item->work = work;
    item->arg = arg;
    item->result = NULL;
    item->status = QD_WORK_OK;
    item->delay_ms = delay_ms;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    atomic_fetch_add(&wq->pending, 1);

    /* Submit delayed work via AIO timeout */
    return qd_aio_timeout(wq->loop, (int)delay_ms, item);
}

int qd_workqueue_submit_work(qd_workqueue_t *wq, qd_work_t *work)
{
    if (!work) {
        return QD_ERR_INVAL;
    }

    work->wq = wq;
    if (work->user_data == NULL) {
        work->user_data = wq->complete_arg;
    }

    atomic_fetch_add(&wq->pending, 1);

    if (work->delay_ms > 0) {
        /* Delayed submission via timeout */
        return qd_aio_timeout(wq->loop, (int)work->delay_ms, work);
    }

    /* Immediate submission */
    work_wrapper_t *wrapper = qd_calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        return QD_ERR_NOMEM;
    }

    wrapper->work_fn = (qd_work_fn_t)(uintptr_t)work->work;
    wrapper->work_arg = work->arg;
    wrapper->orig_work = work;
    wrapper->wq = wq;

    return qd_threadpool_submit_callback(wq->pool, work_wrapper_fn, wrapper,
                                         work_complete_callback, wrapper);
}

int qd_workqueue_pending(qd_workqueue_t *wq)
{
    return atomic_load(&wq->pending);
}

/* Cancellation implementation */
qd_work_handle_t *qd_workqueue_submit_cancellable(qd_workqueue_t *wq,
                                                   qd_work_fn_t work,
                                                   void *arg)
{
    qd_work_t *item = qd_calloc(1, sizeof(*item));
    if (!item) {
        return NULL;
    }

    qd_work_handle_t *handle = qd_calloc(1, sizeof(*handle));
    if (!handle) {
        qd_free(item);
        return NULL;
    }

    item->work = (qd_work_fn_t)(uintptr_t)work;
    item->arg = arg;
    item->result = NULL;
    item->status = QD_WORK_OK;
    item->delay_ms = 0;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    handle->work = item;
    atomic_init(&handle->cancelled, 0);
    handle->wq = wq;

    atomic_fetch_add(&wq->pending, 1);

    work_wrapper_t *wrapper = qd_calloc(1, sizeof(*wrapper));
    if (!wrapper) {
        qd_free(item);
        qd_free(handle);
        return NULL;
    }

    wrapper->work_fn = (qd_work_fn_t)(uintptr_t)item->work;
    wrapper->work_arg = item->arg;
    wrapper->orig_work = item;
    wrapper->wq = wq;

    return handle;
}

int qd_workqueue_cancel(qd_work_handle_t *handle)
{
    if (!handle || !handle->work) {
        return QD_ERR_INVAL;
    }

    atomic_store(&handle->cancelled, 1);
    handle->work->status = QD_WORK_CANCELLED;

    return QD_OK;
}

void qd_work_handle_release(qd_work_handle_t *handle)
{
    if (!handle) {
        return;
    }

    /* Don't free work here - it's freed after completion */
    qd_free(handle);
}

qd_channel_t *qd_workqueue_channel(qd_workqueue_t *wq)
{
    return wq ? wq->channel : NULL;
}
