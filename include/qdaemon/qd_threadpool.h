/*
 * QDaemon - Thread Pool API
 * Thread pool with work stealing for load balancing
 */

#ifndef QD_THREADPOOL_H
#define QD_THREADPOOL_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_task qd_task_t;
typedef struct qd_work_queue qd_work_queue_t;
typedef struct qd_threadpool qd_threadpool_t;
typedef struct qd_future qd_future_t;
typedef struct qd_mpmc_ring qd_mpmc_ring_t;

/* Task function type */
typedef void (*qd_task_fn_t)(void *arg);

/* Task completion callback */
typedef void (*qd_task_complete_fn_t)(void *arg, int status);

/* Task priority levels */
typedef enum {
    QD_PRIORITY_LOW = 0,
    QD_PRIORITY_NORMAL = 1,
    QD_PRIORITY_HIGH = 2,
    QD_PRIORITY_URGENT = 3,
    QD_PRIORITY_COUNT
} qd_priority_t;

/* Task flags */
#define QD_TASK_DETACHED  0x01  /* Task is detached, no future */
#define QD_TASK_AFFINITY  0x02  /* Pin to submitting thread */
#define QD_TASK_BLOCKING  0x04  /* Task may block */

/* Task structure */
struct qd_task {
    qd_task_fn_t func;           /* Function to execute */
    void *arg;                   /* Function argument */
    qd_task_complete_fn_t on_complete; /* Completion callback */
    void *complete_arg;          /* Completion callback argument */
    qd_priority_t priority;      /* Task priority */
    uint32_t flags;              /* Task flags */
    qd_future_t *future;         /* Associated future (if any) */
    struct qd_task *next;        /* Queue linkage */
};

/* Work-stealing deque (double-ended queue) */
typedef struct qd_wsdeque {
    qd_task_t **buffer;
    size_t capacity;
    _Atomic size_t top;          /* Steal from here */
    _Atomic size_t bottom;       /* Push/pop from here */
} qd_wsdeque_t;

/* Per-thread work queue */
struct qd_work_queue {
    qd_wsdeque_t deque;          /* Work-stealing deque */
    qd_task_t *priority_heads[QD_PRIORITY_COUNT]; /* Priority queues */
    qd_task_t *priority_tails[QD_PRIORITY_COUNT];
    pthread_mutex_t lock;        /* Lock for priority queues */
    _Atomic int count;           /* Total task count */
};

/* Thread pool configuration */
typedef struct qd_threadpool_config {
    int num_threads;             /* Number of worker threads (0 = auto) */
    int max_queue_size;          /* Max tasks per queue (0 = unlimited) */
    int enable_stealing;         /* Enable work stealing */
    int steal_threshold;         /* Min tasks before stealing */
    const char *name;            /* Pool name for debugging */
} qd_threadpool_config_t;

/* Default configuration */
#define QD_THREADPOOL_CONFIG_DEFAULT { \
    .num_threads = 0, \
    .max_queue_size = 0, \
    .enable_stealing = 1, \
    .steal_threshold = 2, \
    .name = "qd_threadpool" \
}

/* Worker thread state */
typedef struct qd_worker {
    pthread_t thread;
    int id;
    qd_threadpool_t *pool;
    qd_work_queue_t *queue;      /* Local work queue */
    qd_mpmc_ring_t *ext_ring;    /* External submission ring */
    _Atomic int running;
    _Atomic int idle;
    uint64_t tasks_executed;
    uint64_t tasks_stolen;
} qd_worker_t;

/* Thread pool structure */
struct qd_threadpool {
    qd_worker_t *workers;
    int num_workers;
    qd_work_queue_t *queues;     /* Per-worker queues */
    qd_work_queue_t global_queue; /* Global overflow queue */
    qd_mpmc_ring_t *global_ring;  /* Global lock-free ring */
    qd_mpmc_ring_t *ext_rings;    /* Per-worker external rings */
    _Atomic unsigned int rr_index; /* Round-robin index */

    int wake_fd;                 /* eventfd for worker wakeups */

    _Atomic int running;         /* Pool running state */
    _Atomic int shutdown_mode;   /* 0=none, 1=graceful, 2=immediate */
    _Atomic int paused;          /* Pause task execution */
    _Atomic int active_workers;  /* Currently working threads */
    _Atomic int pending_tasks;   /* Total pending tasks */

    pthread_mutex_t mutex;       /* Global mutex */
    pthread_cond_t cond;         /* Wake up idle workers */
    pthread_cond_t shutdown_cond; /* Shutdown completion */

    /* Configuration */
    int enable_stealing;
    int steal_threshold;
    int max_queue_size;
    char name[32];

    /* Statistics */
    _Atomic uint64_t total_submitted;
    _Atomic uint64_t total_completed;
    _Atomic uint64_t total_stolen;
};

/* Future for async task completion */
typedef enum {
    QD_FUTURE_PENDING = 0,
    QD_FUTURE_RUNNING,
    QD_FUTURE_COMPLETED,
    QD_FUTURE_CANCELLED,
    QD_FUTURE_ERROR
} qd_future_state_t;

struct qd_future {
    _Atomic int state;
    void *result;
    int error;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    _Atomic int refcount;
};

/*
 * Thread Pool API
 */

/* Create thread pool with default settings */
qd_threadpool_t *qd_threadpool_create(int num_threads);

/* Create thread pool with configuration */
qd_threadpool_t *qd_threadpool_create_ex(const qd_threadpool_config_t *config);

/* Destroy thread pool */
void qd_threadpool_destroy(qd_threadpool_t *pool);

/* Submit task to pool */
int qd_threadpool_submit(qd_threadpool_t *pool, qd_task_fn_t func, void *arg);

/* Submit task with priority */
int qd_threadpool_submit_priority(qd_threadpool_t *pool, qd_task_fn_t func,
                                   void *arg, qd_priority_t priority);

/* Submit task with completion callback */
int qd_threadpool_submit_callback(qd_threadpool_t *pool, qd_task_fn_t func,
                                   void *arg, qd_task_complete_fn_t on_complete,
                                   void *complete_arg);

/* Submit task and get future */
qd_future_t *qd_threadpool_submit_future(qd_threadpool_t *pool, qd_task_fn_t func, void *arg);

/* Submit batch of tasks */
int qd_threadpool_submit_batch(qd_threadpool_t *pool, qd_task_fn_t *funcs,
                                void **args, int count);

/* Shutdown thread pool */
typedef enum {
    QD_SHUTDOWN_GRACEFUL = 0,    /* Wait for pending tasks */
    QD_SHUTDOWN_IMMEDIATE = 1    /* Cancel pending tasks */
} qd_shutdown_mode_t;

void qd_threadpool_shutdown(qd_threadpool_t *pool, qd_shutdown_mode_t mode);

/* Wait for all pending tasks to complete */
int qd_threadpool_wait(qd_threadpool_t *pool, int timeout_ms);

/* Pause/resume task processing */
void qd_threadpool_pause(qd_threadpool_t *pool);
void qd_threadpool_resume(qd_threadpool_t *pool);

/* Get thread pool statistics */
typedef struct qd_threadpool_stats {
    int num_workers;
    int active_workers;
    int pending_tasks;
    uint64_t total_submitted;
    uint64_t total_completed;
    uint64_t total_stolen;
} qd_threadpool_stats_t;

void qd_threadpool_stats(qd_threadpool_t *pool, qd_threadpool_stats_t *stats);

/* Get current worker ID (-1 if not in worker thread) */
int qd_threadpool_worker_id(void);

/*
 * Future API
 */

/* Wait for future to complete */
int qd_future_wait(qd_future_t *future, int timeout_ms);

/* Get future state */
qd_future_state_t qd_future_state(qd_future_t *future);

/* Check if future is done */
int qd_future_is_done(qd_future_t *future);

/* Cancel future (if not started) */
int qd_future_cancel(qd_future_t *future);

/* Get result (sets error on future) */
void *qd_future_get(qd_future_t *future, int *error);

/* Release future reference */
void qd_future_release(qd_future_t *future);

/*
 * Task allocation (for custom task management)
 */
qd_task_t *qd_task_alloc(void);
void qd_task_free(qd_task_t *task);

/*
 * Work-stealing deque API (for advanced usage)
 */
int qd_wsdeque_init(qd_wsdeque_t *deque, size_t capacity);
void qd_wsdeque_destroy(qd_wsdeque_t *deque);
int qd_wsdeque_push(qd_wsdeque_t *deque, qd_task_t *task);
qd_task_t *qd_wsdeque_pop(qd_wsdeque_t *deque);
qd_task_t *qd_wsdeque_steal(qd_wsdeque_t *deque);

#ifdef __cplusplus
}
#endif

#endif /* QD_THREADPOOL_H */
