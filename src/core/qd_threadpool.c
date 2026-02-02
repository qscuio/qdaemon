/*
 * QDaemon - Thread Pool Implementation
 * Thread pool with work stealing for load balancing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

/*
 * Lock-free bounded MPMC ring for external submissions.
 * Based on Dmitry Vyukov's MPMC queue algorithm.
 */
typedef struct qd_mpmc_cell {
    _Atomic size_t seq;
    qd_task_t *task;
} qd_mpmc_cell_t;

struct qd_mpmc_ring {
    qd_mpmc_cell_t *cells;
    size_t capacity;
    size_t mask;
    _Atomic size_t head;
    _Atomic size_t tail;
};

static int qd_mpmc_ring_init(qd_mpmc_ring_t *ring, size_t capacity)
{
    if (!ring || capacity == 0)
        return -1;

    capacity = qd_next_pow2_64(capacity);
    ring->cells = qd_calloc(capacity, sizeof(*ring->cells));
    if (!ring->cells)
        return -1;

    ring->capacity = capacity;
    ring->mask = capacity - 1;
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);

    for (size_t i = 0; i < capacity; i++) {
        atomic_init(&ring->cells[i].seq, i);
        ring->cells[i].task = NULL;
    }

    return 0;
}

static void qd_mpmc_ring_destroy(qd_mpmc_ring_t *ring)
{
    if (!ring)
        return;
    qd_free(ring->cells);
    ring->cells = NULL;
    ring->capacity = 0;
    ring->mask = 0;
}

static QD_MAYBE_UNUSED int qd_mpmc_ring_push(qd_mpmc_ring_t *ring, qd_task_t *task)
{
    qd_mpmc_cell_t *cell;
    size_t pos = atomic_load_explicit(&ring->tail, memory_order_relaxed);

    for (;;) {
        cell = &ring->cells[pos & ring->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &ring->tail, &pos, pos + 1,
                    memory_order_acq_rel, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return QD_ERR_BUSY; /* full */
        } else {
            pos = atomic_load_explicit(&ring->tail, memory_order_relaxed);
        }
    }

    cell->task = task;
    atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
    return QD_OK;
}

static int qd_mpmc_ring_pop(qd_mpmc_ring_t *ring, qd_task_t **task_out)
{
    qd_mpmc_cell_t *cell;
    size_t pos = atomic_load_explicit(&ring->head, memory_order_relaxed);

    for (;;) {
        cell = &ring->cells[pos & ring->mask];
        size_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &ring->head, &pos, pos + 1,
                    memory_order_acq_rel, memory_order_relaxed)) {
                break;
            }
        } else if (dif < 0) {
            return QD_ERR_AGAIN; /* empty */
        } else {
            pos = atomic_load_explicit(&ring->head, memory_order_relaxed);
        }
    }

    *task_out = cell->task;
    atomic_store_explicit(&cell->seq, pos + ring->capacity, memory_order_release);
    return QD_OK;
}

static inline void qd_pool_wake(qd_threadpool_t *pool, uint64_t count)
{
    if (!pool || pool->wake_fd < 0 || count == 0)
        return;
    uint64_t val = count;
    ssize_t ret = write(pool->wake_fd, &val, sizeof(val));
    (void)ret;
}

/* Thread-local worker ID */
static __thread int tls_worker_id = -1;

/* Task slab cache */
static qd_slab_cache_t *task_cache = NULL;
static pthread_once_t task_cache_once = PTHREAD_ONCE_INIT;

static void init_task_cache(void)
{
    task_cache = qd_slab_cache_create("qd_task", sizeof(qd_task_t), 0, 0);
}

/*
 * Work-stealing deque implementation
 */

int qd_wsdeque_init(qd_wsdeque_t *deque, size_t capacity)
{
    if (!deque || capacity == 0)
        return -1;

    /* Round up to power of 2 */
    capacity = qd_next_pow2_64(capacity);

    deque->buffer = calloc(capacity, sizeof(qd_task_t *));
    if (!deque->buffer)
        return -1;

    deque->capacity = capacity;
    qd_atomic_init(&deque->top, 0);
    qd_atomic_init(&deque->bottom, 0);

    return 0;
}

void qd_wsdeque_destroy(qd_wsdeque_t *deque)
{
    if (!deque)
        return;

    free(deque->buffer);
    deque->buffer = NULL;
    deque->capacity = 0;
}

/* Push task to bottom (owner only) */
int qd_wsdeque_push(qd_wsdeque_t *deque, qd_task_t *task)
{
    size_t bottom = qd_atomic_load_explicit(&deque->bottom, QD_MEMORY_ORDER_RELAXED);
    size_t top = qd_atomic_load_explicit(&deque->top, QD_MEMORY_ORDER_ACQUIRE);

    size_t size = bottom - top;
    if (size >= deque->capacity - 1) {
        /* Deque is full */
        return -1;
    }

    deque->buffer[bottom & (deque->capacity - 1)] = task;
    qd_memory_barrier();
    qd_atomic_store_explicit(&deque->bottom, bottom + 1, QD_MEMORY_ORDER_RELEASE);

    return 0;
}

/* Pop task from bottom (owner only) */
qd_task_t *qd_wsdeque_pop(qd_wsdeque_t *deque)
{
    size_t bottom = qd_atomic_load_explicit(&deque->bottom, QD_MEMORY_ORDER_RELAXED);
    if (bottom == 0)
        return NULL;

    bottom--;
    qd_atomic_store_explicit(&deque->bottom, bottom, QD_MEMORY_ORDER_RELAXED);
    qd_memory_barrier();

    size_t top = qd_atomic_load_explicit(&deque->top, QD_MEMORY_ORDER_RELAXED);

    if (top <= bottom) {
        /* Non-empty */
        qd_task_t *task = deque->buffer[bottom & (deque->capacity - 1)];

        if (top == bottom) {
            /* Last element - race with stealers */
            if (!qd_atomic_compare_exchange_strong_explicit(&deque->top, &top, top + 1,
                                                            QD_MEMORY_ORDER_SEQ_CST,
                                                            QD_MEMORY_ORDER_RELAXED)) {
                /* Lost race */
                task = NULL;
            }
            qd_atomic_store_explicit(&deque->bottom, bottom + 1, QD_MEMORY_ORDER_RELAXED);
        }
        return task;
    } else {
        /* Empty */
        qd_atomic_store_explicit(&deque->bottom, bottom + 1, QD_MEMORY_ORDER_RELAXED);
        return NULL;
    }
}

/* Steal task from top (thieves) */
qd_task_t *qd_wsdeque_steal(qd_wsdeque_t *deque)
{
    size_t top = qd_atomic_load_explicit(&deque->top, QD_MEMORY_ORDER_ACQUIRE);
    qd_memory_barrier();
    size_t bottom = qd_atomic_load_explicit(&deque->bottom, QD_MEMORY_ORDER_ACQUIRE);

    if (top >= bottom) {
        /* Empty */
        return NULL;
    }

    qd_task_t *task = deque->buffer[top & (deque->capacity - 1)];

    if (!qd_atomic_compare_exchange_strong_explicit(&deque->top, &top, top + 1,
                                                     QD_MEMORY_ORDER_SEQ_CST,
                                                     QD_MEMORY_ORDER_RELAXED)) {
        /* Lost race */
        return NULL;
    }

    return task;
}

/*
 * Task allocation
 */

qd_task_t *qd_task_alloc(void)
{
    pthread_once(&task_cache_once, init_task_cache);

    qd_task_t *task;
    if (task_cache) {
        task = qd_slab_alloc(task_cache);
    } else {
        task = malloc(sizeof(qd_task_t));
    }

    if (task)
        memset(task, 0, sizeof(qd_task_t));

    return task;
}

void qd_task_free(qd_task_t *task)
{
    if (!task)
        return;

    if (task_cache) {
        qd_slab_free(task_cache, task);
    } else {
        free(task);
    }
}

/*
 * Work queue operations
 */

static void work_queue_init(qd_work_queue_t *queue)
{
    qd_wsdeque_init(&queue->deque, 1024);
    for (int i = 0; i < QD_PRIORITY_COUNT; i++) {
        queue->priority_heads[i] = NULL;
        queue->priority_tails[i] = NULL;
    }
    pthread_mutex_init(&queue->lock, NULL);
    qd_atomic_init(&queue->count, 0);
}

static void work_queue_destroy(qd_work_queue_t *queue)
{
    qd_wsdeque_destroy(&queue->deque);

    /* Free any remaining tasks in priority queues */
    for (int i = 0; i < QD_PRIORITY_COUNT; i++) {
        qd_task_t *task = queue->priority_heads[i];
        while (task) {
            qd_task_t *next = task->next;
            qd_task_free(task);
            task = next;
        }
    }

    pthread_mutex_destroy(&queue->lock);
}

static int work_queue_push(qd_work_queue_t *queue, qd_task_t *task, int use_deque)
{
    int result;

    if (use_deque && task->priority == QD_PRIORITY_NORMAL) {
        /* Use work-stealing deque for normal priority */
        result = qd_wsdeque_push(&queue->deque, task);
    } else {
        /* Use priority queue */
        pthread_mutex_lock(&queue->lock);
        int prio = task->priority;
        task->next = NULL;

        if (queue->priority_tails[prio]) {
            queue->priority_tails[prio]->next = task;
            queue->priority_tails[prio] = task;
        } else {
            queue->priority_heads[prio] = task;
            queue->priority_tails[prio] = task;
        }
        pthread_mutex_unlock(&queue->lock);
        result = 0;
    }

    if (result == 0)
        qd_atomic_fetch_add(&queue->count, 1);

    return result;
}

static qd_task_t *work_queue_pop(qd_work_queue_t *queue)
{
    qd_task_t *task = NULL;

    /* Check priority queues first (high to low) */
    pthread_mutex_lock(&queue->lock);
    for (int i = QD_PRIORITY_COUNT - 1; i >= 0; i--) {
        if (queue->priority_heads[i]) {
            task = queue->priority_heads[i];
            queue->priority_heads[i] = task->next;
            if (!queue->priority_heads[i])
                queue->priority_tails[i] = NULL;
            task->next = NULL;
            pthread_mutex_unlock(&queue->lock);
            qd_atomic_fetch_sub(&queue->count, 1);
            return task;
        }
    }
    pthread_mutex_unlock(&queue->lock);

    /* Try work-stealing deque */
    task = qd_wsdeque_pop(&queue->deque);
    if (task)
        qd_atomic_fetch_sub(&queue->count, 1);

    return task;
}

static qd_task_t *work_queue_pop_priority(qd_work_queue_t *queue)
{
    qd_task_t *task = NULL;

    pthread_mutex_lock(&queue->lock);
    for (int i = QD_PRIORITY_COUNT - 1; i >= 0; i--) {
        if (queue->priority_heads[i]) {
            task = queue->priority_heads[i];
            queue->priority_heads[i] = task->next;
            if (!queue->priority_heads[i])
                queue->priority_tails[i] = NULL;
            task->next = NULL;
            pthread_mutex_unlock(&queue->lock);
            qd_atomic_fetch_sub(&queue->count, 1);
            return task;
        }
    }
    pthread_mutex_unlock(&queue->lock);

    return NULL;
}

static qd_task_t *work_queue_steal(qd_work_queue_t *queue)
{
    qd_task_t *task = qd_wsdeque_steal(&queue->deque);
    if (task)
        qd_atomic_fetch_sub(&queue->count, 1);
    return task;
}

/*
 * Future implementation
 */

static qd_future_t *future_create(void)
{
    qd_future_t *future = malloc(sizeof(qd_future_t));
    if (!future)
        return NULL;

    qd_atomic_init(&future->state, QD_FUTURE_PENDING);
    future->result = NULL;
    future->error = 0;
    pthread_mutex_init(&future->mutex, NULL);
    pthread_cond_init(&future->cond, NULL);
    qd_atomic_init(&future->refcount, 1);

    return future;
}

static void future_destroy(qd_future_t *future)
{
    pthread_mutex_destroy(&future->mutex);
    pthread_cond_destroy(&future->cond);
    free(future);
}

int qd_future_wait(qd_future_t *future, int timeout_ms)
{
    if (!future)
        return QD_ERR_INVAL;

    pthread_mutex_lock(&future->mutex);

    while (qd_atomic_load(&future->state) < QD_FUTURE_COMPLETED) {
        if (timeout_ms < 0) {
            pthread_cond_wait(&future->cond, &future->mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }

            int ret = pthread_cond_timedwait(&future->cond, &future->mutex, &ts);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&future->mutex);
                return QD_ERR_TIMEOUT;
            }
        }
    }

    pthread_mutex_unlock(&future->mutex);
    return QD_OK;
}

qd_future_state_t qd_future_state(qd_future_t *future)
{
    if (!future)
        return QD_FUTURE_ERROR;
    return qd_atomic_load(&future->state);
}

int qd_future_is_done(qd_future_t *future)
{
    if (!future)
        return 1;
    return qd_atomic_load(&future->state) >= QD_FUTURE_COMPLETED;
}

int qd_future_cancel(qd_future_t *future)
{
    if (!future)
        return QD_ERR_INVAL;

    int expected = QD_FUTURE_PENDING;
    if (qd_atomic_compare_exchange_strong(&future->state, &expected, QD_FUTURE_CANCELLED)) {
        pthread_mutex_lock(&future->mutex);
        pthread_cond_broadcast(&future->cond);
        pthread_mutex_unlock(&future->mutex);
        return QD_OK;
    }

    return QD_ERR_BUSY;
}

void *qd_future_get(qd_future_t *future, int *error)
{
    if (!future) {
        if (error) *error = QD_ERR_INVAL;
        return NULL;
    }

    if (error)
        *error = future->error;

    return future->result;
}

void qd_future_release(qd_future_t *future)
{
    if (!future)
        return;

    if (qd_atomic_fetch_sub(&future->refcount, 1) == 1) {
        future_destroy(future);
    }
}

/*
 * Worker thread
 */

static void *worker_thread(void *arg)
{
    qd_worker_t *worker = arg;
    qd_threadpool_t *pool = worker->pool;

    tls_worker_id = worker->id;

    while (1) {
        int shutdown = qd_atomic_load(&pool->shutdown_mode);
        if (shutdown == QD_SHUTDOWN_IMMEDIATE + 1)
            break;

        if (shutdown == QD_SHUTDOWN_GRACEFUL + 1 &&
            qd_atomic_load(&pool->pending_tasks) == 0)
            break;

        if (shutdown == 0 && qd_atomic_load(&pool->paused)) {
            qd_atomic_store(&worker->idle, 1);
            uint64_t val;
            ssize_t ret = read(pool->wake_fd, &val, sizeof(val));
            if (ret < 0 && errno != EINTR) {
                /* Ignore wake errors and retry */
            }
            continue;
        }

        qd_task_t *task = NULL;

        /* Try local queue first */
        task = work_queue_pop(worker->queue);

        /* Try global priority queue */
        if (!task) {
            task = work_queue_pop_priority(&pool->global_queue);
        }

        /* Try external ring for this worker */
        if (!task && worker->ext_ring) {
            if (qd_mpmc_ring_pop(worker->ext_ring, &task) != QD_OK) {
                task = NULL;
            }
        }

        /* Try global ring */
        if (!task && pool->global_ring) {
            if (qd_mpmc_ring_pop(pool->global_ring, &task) != QD_OK) {
                task = NULL;
            }
        }

        /* Try stealing from other workers */
        if (!task && pool->enable_stealing) {
            for (int i = 0; i < pool->num_workers && !task; i++) {
                if (i != worker->id) {
                    int victim = (worker->id + i) % pool->num_workers;
                    if (pool->workers[victim].ext_ring) {
                        if (qd_mpmc_ring_pop(pool->workers[victim].ext_ring, &task) != QD_OK) {
                            task = NULL;
                        }
                    }
                    if (!task) {
                        task = work_queue_steal(&pool->queues[victim]);
                    }
                    if (task) {
                        worker->tasks_stolen++;
                        qd_atomic_fetch_add(&pool->total_stolen, 1);
                    }
                }
            }
        }

        if (task) {
            qd_atomic_store(&worker->idle, 0);
            qd_atomic_fetch_add(&pool->active_workers, 1);

            /* Execute task */
            if (task->future) {
                qd_atomic_store(&task->future->state, QD_FUTURE_RUNNING);
            }

            if (task->func) {
                task->func(task->arg);
            }

            /* Complete task */
            if (task->future) {
                pthread_mutex_lock(&task->future->mutex);
                qd_atomic_store(&task->future->state, QD_FUTURE_COMPLETED);
                pthread_cond_broadcast(&task->future->cond);
                pthread_mutex_unlock(&task->future->mutex);
            }

            if (task->on_complete) {
                task->on_complete(task->complete_arg, 0);
            }

            worker->tasks_executed++;
            qd_atomic_fetch_add(&pool->total_completed, 1);
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);

            /* Free task if detached */
            if (task->flags & QD_TASK_DETACHED) {
                qd_task_free(task);
            }

            qd_atomic_fetch_sub(&pool->active_workers, 1);
        } else {
            /* No work available, wait */
            qd_atomic_store(&worker->idle, 1);

            shutdown = qd_atomic_load(&pool->shutdown_mode);
            if (shutdown == QD_SHUTDOWN_IMMEDIATE + 1)
                break;

            if (shutdown == QD_SHUTDOWN_GRACEFUL + 1 &&
                qd_atomic_load(&pool->pending_tasks) == 0)
                break;

            uint64_t val;
            ssize_t ret = read(pool->wake_fd, &val, sizeof(val));
            if (ret < 0 && errno != EINTR) {
                /* Ignore wake errors and retry */
            }
        }
    }

    qd_atomic_store(&worker->running, 0);
    return NULL;
}

static int pool_submit_external(qd_threadpool_t *pool, qd_task_t *task)
{
    if (!pool || !task || pool->num_workers <= 0)
        return QD_ERR_INVAL;

    if (pool->global_ring) {
        if (qd_mpmc_ring_push(pool->global_ring, task) == QD_OK)
            return QD_OK;
    }

    if (pool->ext_rings && pool->num_workers > 0) {
        unsigned int idx = qd_atomic_fetch_add(&pool->rr_index, 1);
        qd_mpmc_ring_t *ring = &pool->ext_rings[idx % (unsigned int)pool->num_workers];
        if (qd_mpmc_ring_push(ring, task) == QD_OK)
            return QD_OK;
    }

    if (work_queue_push(&pool->global_queue, task, 0) == 0)
        return QD_OK;

    return QD_ERR_BUSY;
}

/*
 * Thread pool API
 */

qd_threadpool_t *qd_threadpool_create(int num_threads)
{
    qd_threadpool_config_t config = QD_THREADPOOL_CONFIG_DEFAULT;
    config.num_threads = num_threads;
    return qd_threadpool_create_ex(&config);
}

qd_threadpool_t *qd_threadpool_create_ex(const qd_threadpool_config_t *config)
{
    if (!config)
        return NULL;

    int num_threads = config->num_threads;
    if (num_threads <= 0) {
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_threads <= 0)
            num_threads = 4;
    }

    qd_threadpool_t *pool = calloc(1, sizeof(qd_threadpool_t));
    if (!pool)
        return NULL;
    pool->wake_fd = -1;

    /* Initialize configuration */
    pool->num_workers = num_threads;
    pool->enable_stealing = config->enable_stealing;
    pool->steal_threshold = config->steal_threshold;
    pool->max_queue_size = config->max_queue_size;

    if (config->name)
        snprintf(pool->name, sizeof(pool->name), "%s", config->name);
    else
        snprintf(pool->name, sizeof(pool->name), "threadpool");

    int global_queue_inited = 0;
    int ext_rings_inited = 0;

    /* Initialize synchronization */
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->shutdown_cond, NULL);

    /* Allocate workers */
    pool->workers = calloc(num_threads, sizeof(qd_worker_t));
    if (!pool->workers)
        goto fail_pool;

    /* Allocate per-worker queues */
    pool->queues = calloc(num_threads, sizeof(qd_work_queue_t));
    if (!pool->queues)
        goto fail_workers;

    /* Initialize global priority queue */
    work_queue_init(&pool->global_queue);
    global_queue_inited = 1;

    /* Initialize external rings */
    size_t ring_capacity = pool->max_queue_size > 0 ? (size_t)pool->max_queue_size : 1024;
    size_t global_capacity = ring_capacity * (size_t)num_threads;
    if (global_capacity < ring_capacity)
        global_capacity = ring_capacity;

    pool->ext_rings = calloc(num_threads, sizeof(*pool->ext_rings));
    if (!pool->ext_rings)
        goto fail_queues;

    for (int i = 0; i < num_threads; i++) {
        if (qd_mpmc_ring_init(&pool->ext_rings[i], ring_capacity) != 0)
            goto fail_ext_rings;
    }
    ext_rings_inited = 1;

    pool->global_ring = qd_calloc(1, sizeof(*pool->global_ring));
    if (!pool->global_ring)
        goto fail_ext_rings;
    if (qd_mpmc_ring_init(pool->global_ring, global_capacity) != 0)
        goto fail_global_ring;

    /* Initialize eventfd for worker wakeups */
    pool->wake_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (pool->wake_fd < 0)
        goto fail_global_ring;

    qd_atomic_init(&pool->rr_index, 0);

    /* Initialize workers and their queues */
    qd_atomic_store(&pool->running, 1);
    qd_atomic_store(&pool->paused, 0);

    for (int i = 0; i < num_threads; i++) {
        work_queue_init(&pool->queues[i]);

        pool->workers[i].id = i;
        pool->workers[i].pool = pool;
        pool->workers[i].queue = &pool->queues[i];
        pool->workers[i].ext_ring = &pool->ext_rings[i];
        qd_atomic_store(&pool->workers[i].running, 1);
        qd_atomic_store(&pool->workers[i].idle, 1);

        if (pthread_create(&pool->workers[i].thread, NULL, worker_thread, &pool->workers[i]) != 0) {
            qd_atomic_store(&pool->running, 0);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread, NULL);
            }
            for (int j = 0; j <= i; j++) {
                work_queue_destroy(&pool->queues[j]);
            }
            goto fail_threads;
        }
    }

    return pool;

fail_threads:
    if (pool->wake_fd >= 0) {
        close(pool->wake_fd);
        pool->wake_fd = -1;
    }
fail_global_ring:
    if (pool->global_ring) {
        qd_mpmc_ring_destroy(pool->global_ring);
        qd_free(pool->global_ring);
        pool->global_ring = NULL;
    }
fail_ext_rings:
    if (pool->ext_rings) {
        if (ext_rings_inited) {
            for (int i = 0; i < num_threads; i++) {
                qd_mpmc_ring_destroy(&pool->ext_rings[i]);
            }
        }
        free(pool->ext_rings);
        pool->ext_rings = NULL;
    }
fail_queues:
    if (global_queue_inited)
        work_queue_destroy(&pool->global_queue);
    if (pool->queues)
        free(pool->queues);
fail_workers:
    if (pool->workers)
        free(pool->workers);
fail_pool:
    pthread_cond_destroy(&pool->shutdown_cond);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
    return NULL;
}

void qd_threadpool_destroy(qd_threadpool_t *pool)
{
    if (!pool)
        return;

    /* Shutdown if not already */
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_GRACEFUL);

    /* Wait for all workers */
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i].thread, NULL);
    }

    /* Cleanup */
    for (int i = 0; i < pool->num_workers; i++) {
        work_queue_destroy(&pool->queues[i]);
    }
    work_queue_destroy(&pool->global_queue);

    if (pool->wake_fd >= 0) {
        close(pool->wake_fd);
        pool->wake_fd = -1;
    }

    if (pool->global_ring) {
        qd_mpmc_ring_destroy(pool->global_ring);
        qd_free(pool->global_ring);
        pool->global_ring = NULL;
    }

    if (pool->ext_rings) {
        for (int i = 0; i < pool->num_workers; i++) {
            qd_mpmc_ring_destroy(&pool->ext_rings[i]);
        }
        free(pool->ext_rings);
        pool->ext_rings = NULL;
    }

    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pthread_cond_destroy(&pool->shutdown_cond);

    free(pool->queues);
    free(pool->workers);
    free(pool);
}

int qd_threadpool_submit(qd_threadpool_t *pool, qd_task_fn_t func, void *arg)
{
    return qd_threadpool_submit_priority(pool, func, arg, QD_PRIORITY_NORMAL);
}

int qd_threadpool_submit_priority(qd_threadpool_t *pool, qd_task_fn_t func,
                                   void *arg, qd_priority_t priority)
{
    if (!pool || !func)
        return QD_ERR_INVAL;

    if (!qd_atomic_load(&pool->running))
        return QD_ERR_BUSY;

    qd_task_t *task = qd_task_alloc();
    if (!task)
        return QD_ERR_NOMEM;

    task->func = func;
    task->arg = arg;
    task->priority = priority;
    task->flags = QD_TASK_DETACHED;
    task->future = NULL;
    task->on_complete = NULL;
    task->next = NULL;

    int worker_id = tls_worker_id;

    qd_atomic_fetch_add(&pool->pending_tasks, 1);

    if (worker_id >= 0 && worker_id < pool->num_workers) {
        qd_work_queue_t *queue = &pool->queues[worker_id];
        int use_deque = (priority == QD_PRIORITY_NORMAL);
        if (work_queue_push(queue, task, use_deque) != 0) {
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);
            qd_task_free(task);
            return QD_ERR_BUSY;
        }
    } else {
        if (priority == QD_PRIORITY_NORMAL) {
            if (pool_submit_external(pool, task) != QD_OK) {
                qd_atomic_fetch_sub(&pool->pending_tasks, 1);
                qd_task_free(task);
                return QD_ERR_BUSY;
            }
        } else {
            if (work_queue_push(&pool->global_queue, task, 0) != 0) {
                qd_atomic_fetch_sub(&pool->pending_tasks, 1);
                qd_task_free(task);
                return QD_ERR_BUSY;
            }
        }
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);

    /* Wake up a worker */
    qd_pool_wake(pool, 1);

    return QD_OK;
}

int qd_threadpool_submit_callback(qd_threadpool_t *pool, qd_task_fn_t func,
                                   void *arg, qd_task_complete_fn_t on_complete,
                                   void *complete_arg)
{
    if (!pool || !func)
        return QD_ERR_INVAL;

    if (!qd_atomic_load(&pool->running))
        return QD_ERR_BUSY;

    qd_task_t *task = qd_task_alloc();
    if (!task)
        return QD_ERR_NOMEM;

    task->func = func;
    task->arg = arg;
    task->priority = QD_PRIORITY_NORMAL;
    task->flags = QD_TASK_DETACHED;
    task->future = NULL;
    task->on_complete = on_complete;
    task->complete_arg = complete_arg;
    task->next = NULL;

    int worker_id = tls_worker_id;
    qd_atomic_fetch_add(&pool->pending_tasks, 1);
    if (worker_id >= 0 && worker_id < pool->num_workers) {
        qd_work_queue_t *queue = &pool->queues[worker_id];
        if (work_queue_push(queue, task, 1) != 0) {
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);
            qd_task_free(task);
            return QD_ERR_BUSY;
        }
    } else {
        if (pool_submit_external(pool, task) != QD_OK) {
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);
            qd_task_free(task);
            return QD_ERR_BUSY;
        }
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);

    qd_pool_wake(pool, 1);

    return QD_OK;
}

qd_future_t *qd_threadpool_submit_future(qd_threadpool_t *pool, qd_task_fn_t func, void *arg)
{
    if (!pool || !func)
        return NULL;

    if (!qd_atomic_load(&pool->running))
        return NULL;

    qd_future_t *future = future_create();
    if (!future)
        return NULL;

    qd_task_t *task = qd_task_alloc();
    if (!task) {
        qd_future_release(future);
        return NULL;
    }

    /* Increment refcount for task holding reference */
    qd_atomic_fetch_add(&future->refcount, 1);

    task->func = func;
    task->arg = arg;
    task->priority = QD_PRIORITY_NORMAL;
    task->flags = QD_TASK_DETACHED;
    task->future = future;
    task->on_complete = NULL;
    task->next = NULL;

    int worker_id = tls_worker_id;
    qd_atomic_fetch_add(&pool->pending_tasks, 1);
    if (worker_id >= 0 && worker_id < pool->num_workers) {
        qd_work_queue_t *queue = &pool->queues[worker_id];
        if (work_queue_push(queue, task, 1) != 0) {
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);
            qd_task_free(task);
            qd_future_release(future);  /* Task's reference */
            qd_future_release(future);  /* Caller's reference */
            return NULL;
        }
    } else {
        if (pool_submit_external(pool, task) != QD_OK) {
            qd_atomic_fetch_sub(&pool->pending_tasks, 1);
            qd_task_free(task);
            qd_future_release(future);  /* Task's reference */
            qd_future_release(future);  /* Caller's reference */
            return NULL;
        }
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);

    qd_pool_wake(pool, 1);

    return future;
}

int qd_threadpool_submit_batch(qd_threadpool_t *pool, qd_task_fn_t *funcs,
                                void **args, int count)
{
    if (!pool || !funcs || count <= 0)
        return QD_ERR_INVAL;

    if (!qd_atomic_load(&pool->running))
        return QD_ERR_BUSY;

    int submitted = 0;
    int worker_id = tls_worker_id;

    for (int i = 0; i < count; i++) {
        if (!funcs[i])
            continue;

        qd_task_t *task = qd_task_alloc();
        if (!task)
            break;

        task->func = funcs[i];
        task->arg = args ? args[i] : NULL;
        task->priority = QD_PRIORITY_NORMAL;
        task->flags = QD_TASK_DETACHED;
        task->future = NULL;
        task->on_complete = NULL;
        task->next = NULL;

        qd_atomic_fetch_add(&pool->pending_tasks, 1);
        if (worker_id >= 0 && worker_id < pool->num_workers) {
            qd_work_queue_t *queue = &pool->queues[worker_id];
            if (work_queue_push(queue, task, 1) != 0) {
                if (pool_submit_external(pool, task) != QD_OK) {
                    qd_atomic_fetch_sub(&pool->pending_tasks, 1);
                    qd_task_free(task);
                    break;
                }
            }
        } else {
            if (pool_submit_external(pool, task) != QD_OK) {
                qd_atomic_fetch_sub(&pool->pending_tasks, 1);
                qd_task_free(task);
                break;
            }
        }

        submitted++;
    }

    if (submitted > 0) {
        qd_atomic_fetch_add(&pool->total_submitted, submitted);
        /* Wake up workers */
        qd_pool_wake(pool, (uint64_t)submitted);
    }

    return submitted;
}

void qd_threadpool_shutdown(qd_threadpool_t *pool, qd_shutdown_mode_t mode)
{
    if (!pool)
        return;

    qd_atomic_store(&pool->shutdown_mode, mode + 1);
    qd_atomic_store(&pool->running, 0);

    /* Wake up all workers */
    qd_pool_wake(pool, (uint64_t)pool->num_workers);
}

int qd_threadpool_wait(qd_threadpool_t *pool, int timeout_ms)
{
    if (!pool)
        return QD_ERR_INVAL;

    qd_time_t start = qd_time_now();

    while (qd_atomic_load(&pool->pending_tasks) > 0) {
        if (timeout_ms >= 0) {
            qd_time_t elapsed = qd_time_now() - start;
            if (elapsed >= (qd_time_t)timeout_ms)
                return QD_ERR_TIMEOUT;
        }
        usleep(1000);  /* 1ms */
    }

    return QD_OK;
}

void qd_threadpool_pause(qd_threadpool_t *pool)
{
    if (!pool)
        return;
    qd_atomic_store(&pool->paused, 1);
    qd_pool_wake(pool, (uint64_t)pool->num_workers);
}

void qd_threadpool_resume(qd_threadpool_t *pool)
{
    if (!pool)
        return;
    qd_atomic_store(&pool->paused, 0);
    qd_pool_wake(pool, (uint64_t)pool->num_workers);
}

void qd_threadpool_stats(qd_threadpool_t *pool, qd_threadpool_stats_t *stats)
{
    if (!pool || !stats)
        return;

    stats->num_workers = pool->num_workers;
    stats->active_workers = qd_atomic_load(&pool->active_workers);
    stats->pending_tasks = qd_atomic_load(&pool->pending_tasks);
    stats->total_submitted = qd_atomic_load(&pool->total_submitted);
    stats->total_completed = qd_atomic_load(&pool->total_completed);
    stats->total_stolen = qd_atomic_load(&pool->total_stolen);
}

int qd_threadpool_worker_id(void)
{
    return tls_worker_id;
}
