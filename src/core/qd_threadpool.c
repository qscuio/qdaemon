/*
 * QDaemon - Thread Pool Implementation
 * Thread pool with work stealing for load balancing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "qdaemon/qd_threadpool.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

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

    while (qd_atomic_load(&pool->running) &&
           qd_atomic_load(&pool->shutdown_mode) == 0) {

        qd_task_t *task = NULL;

        /* Try local queue first */
        task = work_queue_pop(worker->queue);

        /* Try global queue */
        if (!task) {
            task = work_queue_pop(&pool->global_queue);
        }

        /* Try stealing from other workers */
        if (!task && pool->enable_stealing) {
            for (int i = 0; i < pool->num_workers && !task; i++) {
                if (i != worker->id) {
                    int victim = (worker->id + i) % pool->num_workers;
                    task = work_queue_steal(&pool->queues[victim]);
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

            pthread_mutex_lock(&pool->mutex);
            if (qd_atomic_load(&pool->running) &&
                qd_atomic_load(&pool->pending_tasks) == 0 &&
                qd_atomic_load(&pool->shutdown_mode) == 0) {
                pthread_cond_wait(&pool->cond, &pool->mutex);
            }
            pthread_mutex_unlock(&pool->mutex);
        }
    }

    qd_atomic_store(&worker->running, 0);
    return NULL;
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

    /* Initialize configuration */
    pool->num_workers = num_threads;
    pool->enable_stealing = config->enable_stealing;
    pool->steal_threshold = config->steal_threshold;
    pool->max_queue_size = config->max_queue_size;

    if (config->name)
        snprintf(pool->name, sizeof(pool->name), "%s", config->name);
    else
        snprintf(pool->name, sizeof(pool->name), "threadpool");

    /* Initialize synchronization */
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->shutdown_cond, NULL);

    /* Allocate workers */
    pool->workers = calloc(num_threads, sizeof(qd_worker_t));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    /* Allocate per-worker queues */
    pool->queues = calloc(num_threads, sizeof(qd_work_queue_t));
    if (!pool->queues) {
        free(pool->workers);
        free(pool);
        return NULL;
    }

    /* Initialize global queue */
    work_queue_init(&pool->global_queue);

    /* Initialize workers and their queues */
    qd_atomic_store(&pool->running, 1);

    for (int i = 0; i < num_threads; i++) {
        work_queue_init(&pool->queues[i]);

        pool->workers[i].id = i;
        pool->workers[i].pool = pool;
        pool->workers[i].queue = &pool->queues[i];
        qd_atomic_store(&pool->workers[i].running, 1);
        qd_atomic_store(&pool->workers[i].idle, 1);

        if (pthread_create(&pool->workers[i].thread, NULL, worker_thread, &pool->workers[i]) != 0) {
            /* Failed to create thread, cleanup */
            qd_atomic_store(&pool->running, 0);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread, NULL);
            }
            for (int j = 0; j <= i; j++) {
                work_queue_destroy(&pool->queues[j]);
            }
            work_queue_destroy(&pool->global_queue);
            free(pool->queues);
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }

    return pool;
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

    /* Try to submit to worker queue if we're in a worker thread */
    int worker_id = tls_worker_id;
    qd_work_queue_t *queue;

    if (worker_id >= 0 && worker_id < pool->num_workers) {
        queue = &pool->queues[worker_id];
    } else {
        /* Submit to global queue */
        queue = &pool->global_queue;
    }

    int result = work_queue_push(queue, task, (worker_id >= 0));
    if (result != 0) {
        /* Try global queue as fallback */
        result = work_queue_push(&pool->global_queue, task, 0);
        if (result != 0) {
            qd_task_free(task);
            return QD_ERR_BUSY;
        }
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);
    qd_atomic_fetch_add(&pool->pending_tasks, 1);

    /* Wake up a worker */
    pthread_mutex_lock(&pool->mutex);
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

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

    int result = work_queue_push(&pool->global_queue, task, 0);
    if (result != 0) {
        qd_task_free(task);
        return QD_ERR_BUSY;
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);
    qd_atomic_fetch_add(&pool->pending_tasks, 1);

    pthread_mutex_lock(&pool->mutex);
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

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

    int result = work_queue_push(&pool->global_queue, task, 0);
    if (result != 0) {
        qd_task_free(task);
        qd_future_release(future);  /* Task's reference */
        qd_future_release(future);  /* Caller's reference */
        return NULL;
    }

    qd_atomic_fetch_add(&pool->total_submitted, 1);
    qd_atomic_fetch_add(&pool->pending_tasks, 1);

    pthread_mutex_lock(&pool->mutex);
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

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

        if (work_queue_push(&pool->global_queue, task, 0) != 0) {
            qd_task_free(task);
            break;
        }

        submitted++;
    }

    if (submitted > 0) {
        qd_atomic_fetch_add(&pool->total_submitted, submitted);
        qd_atomic_fetch_add(&pool->pending_tasks, submitted);

        /* Wake up workers */
        pthread_mutex_lock(&pool->mutex);
        pthread_cond_broadcast(&pool->cond);
        pthread_mutex_unlock(&pool->mutex);
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
    pthread_mutex_lock(&pool->mutex);
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
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
    qd_atomic_store(&pool->running, 0);
}

void qd_threadpool_resume(qd_threadpool_t *pool)
{
    if (!pool)
        return;
    qd_atomic_store(&pool->running, 1);

    pthread_mutex_lock(&pool->mutex);
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
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
