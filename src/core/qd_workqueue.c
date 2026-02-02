/*
 * QDaemon - Work Queue Implementation
 * High-performance libuv-style pattern with slab pooling
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "qdaemon/qd_workqueue.h"
#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_aio.h"
#include "qdaemon/qd_channel.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/*
 * Combined work item - single allocation for everything
 * This replaces the old work_wrapper_t + qd_work_t pair
 */
typedef struct qd_work_item {
    /* Work function and args */
    qd_work_fn_t work_fn;
    void *work_arg;

    /* Result and status */
    void *result;
    _Atomic int status;

    /* Ownership */
    qd_workqueue_t *wq;
    void *user_data;

    /* For delayed work */
    uint64_t delay_ms;
} qd_work_item_t;

/* Work handle for cancellation */
struct qd_work_handle {
    qd_work_item_t *item;
    _Atomic int cancelled;
    qd_workqueue_t *wq;
};

/* Channel result type - just a pointer now */
typedef struct result_struct {
    qd_work_item_t *item;
    int status;
} result_struct_t;

/* Work queue structure */
struct qd_workqueue {
    qd_aio_loop_t *loop;
    qd_threadpool_t *pool;
    qd_channel_t *channel;
    qd_after_work_fn_t on_complete;
    void *complete_arg;
    _Atomic int pending;
    _Atomic int inflight;
    _Atomic int draining;

    /* Slab cache for work items */
    qd_slab_cache_t *item_cache;
};

/* Global slab cache for work items (shared across workqueues for efficiency) */
static qd_slab_cache_t *g_work_item_cache = NULL;
static pthread_once_t g_cache_init_once = PTHREAD_ONCE_INIT;

static void init_global_cache(void)
{
    g_work_item_cache = qd_slab_cache_create("qd_work_item",
                                             sizeof(qd_work_item_t),
                                             0,
                                             QD_SLAB_ZERO);
}

static inline qd_work_item_t *work_item_alloc(qd_workqueue_t *wq)
{
    (void)wq;
    pthread_once(&g_cache_init_once, init_global_cache);
    if (g_work_item_cache) {
        return qd_slab_alloc(g_work_item_cache);
    }
    return qd_calloc(1, sizeof(qd_work_item_t));
}

static inline void work_item_free(qd_workqueue_t *wq, qd_work_item_t *item)
{
    (void)wq;
    if (g_work_item_cache) {
        qd_slab_free(g_work_item_cache, item);
    } else {
        qd_free(item);
    }
}

/* Thread pool wrapper - runs on worker thread */
static void work_item_execute(void *arg)
{
    qd_work_item_t *item = arg;

    /* Validate item */
    if (!item || !item->work_fn || !item->wq) {
        qd_log_error("Invalid work item: item=%p, work_fn=%p, wq=%p",
                     (void*)item, item ? (void*)item->work_fn : NULL,
                     item ? (void*)item->wq : NULL);
        return;
    }

    /* Check if cancelled */
    if (atomic_load_explicit(&item->status, memory_order_relaxed) == QD_WORK_CANCELLED) {
        return;
    }

    /* Run actual work function */
    errno = 0;
    item->result = item->work_fn(item->work_arg);

    if (item->result == NULL && errno != 0) {
        atomic_store(&item->status, QD_WORK_ERROR);
    } else {
        atomic_store(&item->status, QD_WORK_OK);
    }
}

/* Completion callback on worker thread */
static void work_item_complete(void *arg, int status)
{
    (void)status;
    qd_work_item_t *item = arg;
    qd_workqueue_t *wq = item->wq;

    /* Don't send to channel if we're draining */
    if (atomic_load_explicit(&wq->draining, memory_order_relaxed)) {
        atomic_fetch_sub(&wq->inflight, 1);
        work_item_free(wq, item);
        return;
    }

    /* Push completion to channel */
    result_struct_t result = {
        .item = item,
        .status = atomic_load(&item->status)
    };

    qd_channel_try_send(wq->channel, &result);
    atomic_fetch_sub(&wq->inflight, 1);
}

/* Drain channel and call completions (called from event loop context) */
static void drain_channel_completions(qd_workqueue_t *wq)
{
    enum { QD_WQ_BATCH = 128 };
    result_struct_t results[QD_WQ_BATCH];

    for (;;) {
        int count = qd_channel_try_recv_batch(wq->channel, results, QD_WQ_BATCH);
        if (count <= 0) {
            qd_channel_ack(wq->channel);
            if (qd_channel_size(wq->channel) == 0) {
                break;
            }
            continue;
        }

        for (int i = 0; i < count; i++) {
            result_struct_t *result = &results[i];
            qd_work_item_t *item = result->item;

            if (item->delay_ms > 0) {
                /* Delayed work - handled elsewhere */
                continue;
            }

            /* Check if cancelled before calling callback */
            int item_status = atomic_load(&item->status);
            if (item_status == QD_WORK_CANCELLED) {
                work_item_free(wq, item);
            } else if (wq->on_complete) {
                wq->on_complete(item->result, item_status, item->user_data);
                work_item_free(wq, item);
            } else {
                work_item_free(wq, item);
            }

            atomic_fetch_sub(&wq->pending, 1);
        }
    }
}

void qd_workqueue_process(qd_workqueue_t *wq)
{
    if (wq) {
        drain_channel_completions(wq);
    }
}

void qd_workqueue_handle_completion(qd_completion_t *comp)
{
    if (!comp)
        return;

    if (comp->op == QD_OP_CHANNEL) {
        qd_workqueue_t *wq = (qd_workqueue_t *)comp->user_data;
        qd_workqueue_process(wq);
        return;
    }

    if (comp->op == QD_OP_TIMEOUT) {
        qd_work_item_t *item = (qd_work_item_t *)comp->user_data;
        if (!item || !item->wq)
            return;

        qd_workqueue_t *wq = item->wq;
        int ret = qd_threadpool_submit_callback(wq->pool, work_item_execute, item,
                                                 work_item_complete, item);
        if (ret != QD_OK) {
            atomic_fetch_sub(&wq->pending, 1);
            atomic_fetch_sub(&wq->inflight, 1);
            work_item_free(wq, item);
        }
    }
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
    atomic_init(&wq->inflight, 0);
    atomic_init(&wq->draining, 0);

    /* Initialize global slab cache */
    pthread_once(&g_cache_init_once, init_global_cache);

    /* Create channel for completions */
    qd_channel_config_t chan_config = {
        .capacity = 4096,
        .item_size = sizeof(result_struct_t),
        .flags = 0
    };
    wq->channel = qd_channel_create(&chan_config);
    if (!wq->channel) {
        qd_free(wq);
        return NULL;
    }

    /* Register channel with event loop */
    if (qd_aio_channel_watch(loop, wq->channel, wq) != QD_OK) {
        qd_log_error("Failed to watch workqueue channel");
        qd_channel_destroy(wq->channel);
        qd_free(wq);
        return NULL;
    }

    qd_log_debug("Created workqueue: loop=%p, pool=%p", (void*)loop, (void*)pool);

    return wq;
}

void qd_workqueue_destroy(qd_workqueue_t *wq)
{
    if (!wq) {
        return;
    }

    qd_aio_channel_unwatch(wq->loop, wq->channel);
    atomic_store(&wq->draining, 1);

    /* Wait for inflight tasks */
    while (atomic_load(&wq->inflight) > 0) {
        usleep(1000);
    }

    drain_channel_completions(wq);

    qd_channel_destroy(wq->channel);
    qd_free(wq);
}

int qd_workqueue_submit(qd_workqueue_t *wq, qd_work_fn_t work, void *arg)
{
    if (!wq) {
        return QD_ERR_INVAL;
    }

    qd_work_item_t *item = work_item_alloc(wq);
    if (!item) {
        return QD_ERR_NOMEM;
    }

    item->work_fn = work;
    item->work_arg = arg;
    item->result = NULL;
    atomic_init(&item->status, QD_WORK_OK);
    item->delay_ms = 0;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    atomic_fetch_add(&wq->pending, 1);
    atomic_fetch_add(&wq->inflight, 1);

    int ret = qd_threadpool_submit_callback(wq->pool, work_item_execute, item,
                                             work_item_complete, item);
    if (ret != QD_OK) {
        atomic_fetch_sub(&wq->pending, 1);
        atomic_fetch_sub(&wq->inflight, 1);
        work_item_free(wq, item);
    }

    return ret;
}

int qd_workqueue_submit_batch(qd_workqueue_t *wq,
                               qd_work_fn_t *funcs,
                               void **args,
                               int count)
{
    if (!wq || !funcs || count <= 0) {
        return QD_ERR_INVAL;
    }

    int submitted = 0;

    /* Pre-allocate all items */
    qd_work_item_t **items = alloca(count * sizeof(qd_work_item_t *));
    for (int i = 0; i < count; i++) {
        items[i] = work_item_alloc(wq);
        if (!items[i]) {
            /* Free already allocated */
            for (int j = 0; j < i; j++) {
                work_item_free(wq, items[j]);
            }
            return QD_ERR_NOMEM;
        }

        items[i]->work_fn = funcs[i];
        items[i]->work_arg = args ? args[i] : NULL;
        items[i]->result = NULL;
        atomic_init(&items[i]->status, QD_WORK_OK);
        items[i]->delay_ms = 0;
        items[i]->user_data = wq->complete_arg;
        items[i]->wq = wq;
    }

    /* Submit all to threadpool */
    atomic_fetch_add(&wq->pending, count);
    atomic_fetch_add(&wq->inflight, count);

    for (int i = 0; i < count; i++) {
        int ret = qd_threadpool_submit_callback(wq->pool, work_item_execute, items[i],
                                                 work_item_complete, items[i]);
        if (ret == QD_OK) {
            submitted++;
        } else {
            atomic_fetch_sub(&wq->pending, 1);
            atomic_fetch_sub(&wq->inflight, 1);
            work_item_free(wq, items[i]);
        }
    }

    return submitted;
}

int qd_workqueue_submit_delayed(qd_workqueue_t *wq, qd_work_fn_t work,
                                 void *arg, uint64_t delay_ms)
{
    if (!wq) {
        return QD_ERR_INVAL;
    }

    qd_work_item_t *item = work_item_alloc(wq);
    if (!item) {
        return QD_ERR_NOMEM;
    }

    item->work_fn = work;
    item->work_arg = arg;
    item->result = NULL;
    atomic_init(&item->status, QD_WORK_OK);
    item->delay_ms = delay_ms;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    atomic_fetch_add(&wq->pending, 1);
    atomic_fetch_add(&wq->inflight, 1);

    int ret = qd_aio_timeout(wq->loop, (int)delay_ms, item);
    if (ret != QD_OK) {
        atomic_fetch_sub(&wq->pending, 1);
        atomic_fetch_sub(&wq->inflight, 1);
        work_item_free(wq, item);
    }
    return ret;
}

int qd_workqueue_submit_work(qd_workqueue_t *wq, qd_work_t *work)
{
    if (!wq || !work) {
        return QD_ERR_INVAL;
    }

    /* Convert legacy qd_work_t to internal qd_work_item_t */
    qd_work_item_t *item = work_item_alloc(wq);
    if (!item) {
        return QD_ERR_NOMEM;
    }

    item->work_fn = work->work;
    item->work_arg = work->arg;
    item->result = NULL;
    atomic_init(&item->status, QD_WORK_OK);
    item->delay_ms = work->delay_ms;
    item->user_data = work->user_data ? work->user_data : wq->complete_arg;
    item->wq = wq;

    atomic_fetch_add(&wq->pending, 1);
    atomic_fetch_add(&wq->inflight, 1);

    if (item->delay_ms > 0) {
        int ret = qd_aio_timeout(wq->loop, (int)item->delay_ms, item);
        if (ret != QD_OK) {
            atomic_fetch_sub(&wq->pending, 1);
            atomic_fetch_sub(&wq->inflight, 1);
            work_item_free(wq, item);
        }
        return ret;
    }

    int ret = qd_threadpool_submit_callback(wq->pool, work_item_execute, item,
                                             work_item_complete, item);
    if (ret != QD_OK) {
        atomic_fetch_sub(&wq->pending, 1);
        atomic_fetch_sub(&wq->inflight, 1);
        work_item_free(wq, item);
    }

    return ret;
}

int qd_workqueue_pending(qd_workqueue_t *wq)
{
    return wq ? atomic_load(&wq->pending) : 0;
}

/* Cancellation implementation */
qd_work_handle_t *qd_workqueue_submit_cancellable(qd_workqueue_t *wq,
                                                   qd_work_fn_t work,
                                                   void *arg)
{
    if (!wq) {
        return NULL;
    }

    qd_work_item_t *item = work_item_alloc(wq);
    if (!item) {
        return NULL;
    }

    qd_work_handle_t *handle = qd_calloc(1, sizeof(*handle));
    if (!handle) {
        work_item_free(wq, item);
        return NULL;
    }

    item->work_fn = work;
    item->work_arg = arg;
    item->result = NULL;
    atomic_init(&item->status, QD_WORK_OK);
    item->delay_ms = 0;
    item->user_data = wq->complete_arg;
    item->wq = wq;

    handle->item = item;
    atomic_init(&handle->cancelled, 0);
    handle->wq = wq;

    atomic_fetch_add(&wq->pending, 1);
    atomic_fetch_add(&wq->inflight, 1);

    int ret = qd_threadpool_submit_callback(wq->pool, work_item_execute, item,
                                             work_item_complete, item);
    if (ret != QD_OK) {
        atomic_fetch_sub(&wq->pending, 1);
        atomic_fetch_sub(&wq->inflight, 1);
        work_item_free(wq, item);
        qd_free(handle);
        return NULL;
    }

    return handle;
}

int qd_workqueue_cancel(qd_work_handle_t *handle)
{
    if (!handle || !handle->item) {
        return QD_ERR_INVAL;
    }

    atomic_store(&handle->cancelled, 1);
    atomic_store(&handle->item->status, QD_WORK_CANCELLED);

    return QD_OK;
}

void qd_work_handle_release(qd_work_handle_t *handle)
{
    if (handle) {
        qd_free(handle);
    }
}

qd_channel_t *qd_workqueue_channel(qd_workqueue_t *wq)
{
    return wq ? wq->channel : NULL;
}
