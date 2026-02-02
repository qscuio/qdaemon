/*
 * QDaemon - Async File I/O Implementation
 * Uses Thread Pool to offload blocking I/O
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "qdaemon/qd_fs.h"
#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Internal task structure wrapper */
typedef struct fs_task {
    qd_fs_req_t req;
    int type; /* 0=READ, 1=WRITE */
    qd_event_loop_t *loop;  /* Event loop for callback dispatch */
} fs_task_t;

/* Global thread pool for fs operations (lazily initialized) */
static qd_threadpool_t *g_fs_pool = NULL;
static pthread_mutex_t g_fs_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Get or create the global fs thread pool */
static qd_threadpool_t *get_fs_pool(void)
{
    if (g_fs_pool) {
        return g_fs_pool;
    }

    pthread_mutex_lock(&g_fs_pool_lock);
    if (!g_fs_pool) {
        /* Default: 4 workers for fs operations */
        g_fs_pool = qd_threadpool_create(4);
        if (g_fs_pool) {
            qd_log_debug("Created global fs thread pool with 4 workers");
        } else {
            qd_log_error("Failed to create global fs thread pool");
        }
    }
    pthread_mutex_unlock(&g_fs_pool_lock);

    return g_fs_pool;
}

/* Cleanup the global fs thread pool */
void qd_fs_cleanup(void)
{
    pthread_mutex_lock(&g_fs_pool_lock);
    if (g_fs_pool) {
        qd_threadpool_destroy(g_fs_pool);
        g_fs_pool = NULL;
        qd_log_debug("Destroyed global fs thread pool");
    }
    pthread_mutex_unlock(&g_fs_pool_lock);
}

/*
 * Worker function (Runs on Thread Pool)
 */
static void fs_worker(void *arg)
{
    fs_task_t *task = arg;

    if (task->type == 0) { /* READ */
        task->req.result = pread(task->req.fd, task->req.buf,
                                task->req.size, task->req.offset);
    } else { /* WRITE */
        task->req.result = pwrite(task->req.fd, task->req.buf,
                                 task->req.size, task->req.offset);
    }

    if (task->req.result < 0) {
        task->req.error = errno;
    } else {
        task->req.error = 0;
    }
}

/* Completion on Main Thread (invoked via qd_event_defer) */
static void fs_complete_main(void *arg)
{
    fs_task_t *task = arg;
    if (task->req.cb) {
        task->req.cb(&task->req, task->req.result);
    }
    qd_free(task);
}

/*
 * Thread Pool Completion Hook
 * This runs on the WORKER thread after fs_worker returns.
 * We use qd_event_defer to safely invoke the callback on the main loop.
 */
static void worker_complete_shim(void *arg, int status)
{
    (void)status;
    fs_task_t *task = arg;

    if (!task->loop) {
        /* No event loop - run callback directly (warning: may not be thread-safe) */
        qd_log_warn("fs task completed without event loop, calling directly");
        fs_complete_main(arg);
        return;
    }

    /* Defer completion to main loop thread */
    if (qd_event_defer(task->loop, fs_complete_main, task) != QD_OK) {
        qd_log_error("Failed to defer fs completion callback");
        /* Fallback: free task */
        qd_free(task);
    }
}

int qd_fs_read(qd_event_loop_t *loop, int fd, void *buf, size_t size, off_t offset,
               qd_fs_cb_t cb, void *data)
{
    qd_threadpool_t *pool = get_fs_pool();
    if (!pool) {
        return QD_ERR_NOMEM;
    }

    fs_task_t *task = qd_calloc(1, sizeof(fs_task_t));
    if (!task) {
        return QD_ERR_NOMEM;
    }

    task->req.loop = loop;
    task->req.fd = fd;
    task->req.buf = buf;
    task->req.size = size;
    task->req.offset = offset;
    task->req.cb = cb;
    task->req.data = data;
    task->type = 0; /* READ */
    task->loop = loop;

    /* Submit to thread pool with completion callback */
    return qd_threadpool_submit_callback(pool, fs_worker, task,
                                         worker_complete_shim, task);
}

int qd_fs_write(qd_event_loop_t *loop, int fd, const void *buf, size_t size, off_t offset,
                qd_fs_cb_t cb, void *data)
{
    qd_threadpool_t *pool = get_fs_pool();
    if (!pool) {
        return QD_ERR_NOMEM;
    }

    fs_task_t *task = qd_calloc(1, sizeof(fs_task_t));
    if (!task) {
        return QD_ERR_NOMEM;
    }

    task->req.loop = loop;
    task->req.fd = fd;
    task->req.buf = (void*)buf; /* Cast away const for storage */
    task->req.size = size;
    task->req.offset = offset;
    task->req.cb = cb;
    task->req.data = data;
    task->type = 1; /* WRITE */
    task->loop = loop;

    return qd_threadpool_submit_callback(pool, fs_worker, task,
                                         worker_complete_shim, task);
}

void qd_fs_req_cancel(qd_fs_req_t *req)
{
    if (req) {
        req->cb = NULL; /* Detach callback */
    }
}
