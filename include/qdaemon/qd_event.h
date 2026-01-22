/*
 * QDaemon - Event Loop API
 * High-performance epoll-based event loop
 */

#ifndef QD_EVENT_H
#define QD_EVENT_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_event_loop qd_event_loop_t;
typedef struct qd_event_source qd_event_source_t;
typedef struct qd_timer_wheel qd_timer_wheel_t;

/* Event flags */
typedef enum {
    QD_EVENT_READ    = 0x01,     /* Ready for reading */
    QD_EVENT_WRITE   = 0x02,     /* Ready for writing */
    QD_EVENT_ERROR   = 0x04,     /* Error condition */
    QD_EVENT_HANGUP  = 0x08,     /* Hangup occurred */
    QD_EVENT_RDHUP   = 0x10,     /* Peer closed connection */
    QD_EVENT_EDGE    = 0x20,     /* Edge-triggered mode */
    QD_EVENT_ONESHOT = 0x40,     /* One-shot mode */
} qd_event_flags_t;

/* Event callback function */
typedef void (*qd_event_cb_t)(int fd, uint32_t events, void *arg);

/* Idle callback function (called when no events) */
typedef void (*qd_idle_cb_t)(qd_event_loop_t *loop, void *arg);

/* Pre/post poll callbacks */
typedef void (*qd_poll_cb_t)(qd_event_loop_t *loop, void *arg);

/* Signal callback function */
typedef void (*qd_signal_cb_t)(int signo, void *arg);

/* Event source types */
typedef enum {
    QD_SOURCE_FD = 0,            /* File descriptor */
    QD_SOURCE_TIMER,             /* Timer */
    QD_SOURCE_SIGNAL,            /* Signal */
    QD_SOURCE_ASYNC,             /* Async notification */
} qd_source_type_t;

/* Event source structure */
struct qd_event_source {
    int fd;                      /* File descriptor */
    qd_source_type_t type;       /* Source type */
    uint32_t events;             /* Registered events */
    uint32_t active_events;      /* Currently active events */
    qd_event_cb_t callback;      /* Event callback */
    void *arg;                   /* Callback argument */
    qd_event_loop_t *loop;       /* Owning loop */
    int registered;              /* Is registered with epoll */
    int enabled;                 /* Is enabled */
    struct qd_event_source *next; /* List linkage */
    struct qd_event_source *prev;
};

/* Signal handler entry */
typedef struct qd_signal_handler {
    int signo;
    qd_signal_cb_t callback;
    void *arg;
    struct qd_signal_handler *next;
} qd_signal_handler_t;

/* Async notification for cross-thread wakeup */
typedef struct qd_async {
    int fd;                      /* eventfd for wakeup */
    qd_event_source_t *source;   /* Event source */
    void (*callback)(void *arg); /* Callback function */
    void *arg;
} qd_async_t;

/* Pending work item for deferred execution */
typedef struct qd_pending_work {
    void (*func)(void *arg);
    void *arg;
    struct qd_pending_work *next;
} qd_pending_work_t;

/* Event loop configuration */
typedef struct qd_event_loop_config {
    int max_events;              /* Max events per poll (default: 1024) */
    int timeout_ms;              /* Default poll timeout (default: -1) */
    int enable_signals;          /* Handle signals (default: 1) */
    const char *name;            /* Loop name for debugging */
} qd_event_loop_config_t;

#define QD_EVENT_LOOP_CONFIG_DEFAULT { \
    .max_events = 1024, \
    .timeout_ms = -1, \
    .enable_signals = 1, \
    .name = "qd_event_loop" \
}

/* Event loop state */
typedef enum {
    QD_LOOP_STOPPED = 0,
    QD_LOOP_RUNNING,
    QD_LOOP_STOPPING,
} qd_loop_state_t;

/* Event loop structure */
struct qd_event_loop {
    int epoll_fd;                /* epoll file descriptor */
    int signal_fd;               /* signalfd for signal handling */
    int timer_fd;                /* timerfd for timer wheel */
    int wakeup_fd;               /* eventfd for async wakeup */

    _Atomic int state;           /* Loop state */
    _Atomic int iteration;       /* Current iteration number */

    qd_event_source_t *sources;  /* Registered event sources */
    int num_sources;

    qd_timer_wheel_t *timer_wheel; /* Timer wheel */
    qd_signal_handler_t *signal_handlers; /* Signal handlers */

    /* Pending work queue */
    qd_pending_work_t *pending_head;
    qd_pending_work_t *pending_tail;
    pthread_mutex_t pending_lock;

    /* Callbacks */
    qd_idle_cb_t idle_callback;
    void *idle_arg;
    qd_poll_cb_t pre_poll_callback;
    void *pre_poll_arg;
    qd_poll_cb_t post_poll_callback;
    void *post_poll_arg;

    /* Configuration */
    int max_events;
    int default_timeout;
    char name[32];

    /* Statistics */
    uint64_t poll_count;
    uint64_t event_count;
    uint64_t timeout_count;

    /* Thread info */
    pthread_t owner_thread;
    int thread_bound;

    /* Internal */
    pthread_mutex_t lock;
    void *epoll_events;          /* struct epoll_event array */
};

/*
 * Event Loop API
 */

/* Create event loop with default settings */
qd_event_loop_t *qd_event_loop_create(void);

/* Create event loop with configuration */
qd_event_loop_t *qd_event_loop_create_ex(const qd_event_loop_config_t *config);

/* Destroy event loop */
void qd_event_loop_destroy(qd_event_loop_t *loop);

/* Run event loop (blocks until stopped) */
int qd_event_loop_run(qd_event_loop_t *loop);

/* Run single iteration */
int qd_event_loop_run_once(qd_event_loop_t *loop, int timeout_ms);

/* Stop event loop */
void qd_event_loop_stop(qd_event_loop_t *loop);

/* Check if running */
int qd_event_loop_is_running(qd_event_loop_t *loop);

/* Wake up event loop from another thread */
void qd_event_loop_wakeup(qd_event_loop_t *loop);

/*
 * Event Source Management
 */

/* Add file descriptor to event loop */
qd_event_source_t *qd_event_add(qd_event_loop_t *loop, int fd, uint32_t events,
                                 qd_event_cb_t callback, void *arg);

/* Modify event flags */
int qd_event_mod(qd_event_loop_t *loop, qd_event_source_t *source, uint32_t events);

/* Remove event source */
int qd_event_del(qd_event_loop_t *loop, qd_event_source_t *source);

/* Enable/disable event source */
int qd_event_enable(qd_event_source_t *source);
int qd_event_disable(qd_event_source_t *source);

/* Remove by fd */
int qd_event_del_fd(qd_event_loop_t *loop, int fd);

/*
 * Signal Handling
 */

/* Add signal handler */
int qd_event_add_signal(qd_event_loop_t *loop, int signo, qd_signal_cb_t callback, void *arg);

/* Remove signal handler */
int qd_event_del_signal(qd_event_loop_t *loop, int signo);

/*
 * Deferred Execution
 */

/* Schedule work to run in event loop thread */
int qd_event_defer(qd_event_loop_t *loop, void (*func)(void *arg), void *arg);

/* Schedule work to run after timeout */
int qd_event_defer_timeout(qd_event_loop_t *loop, void (*func)(void *arg),
                            void *arg, int timeout_ms);

/*
 * Async Notification
 */

/* Create async notifier */
qd_async_t *qd_async_create(qd_event_loop_t *loop, void (*callback)(void *arg), void *arg);

/* Destroy async notifier */
void qd_async_destroy(qd_async_t *async);

/* Send async notification */
int qd_async_send(qd_async_t *async);

/*
 * Callbacks
 */

/* Set idle callback (called when no events) */
void qd_event_set_idle_callback(qd_event_loop_t *loop, qd_idle_cb_t callback, void *arg);

/* Set pre-poll callback */
void qd_event_set_pre_poll_callback(qd_event_loop_t *loop, qd_poll_cb_t callback, void *arg);

/* Set post-poll callback */
void qd_event_set_post_poll_callback(qd_event_loop_t *loop, qd_poll_cb_t callback, void *arg);

/*
 * Statistics
 */

typedef struct qd_event_loop_stats {
    uint64_t poll_count;
    uint64_t event_count;
    uint64_t timeout_count;
    int num_sources;
    int state;
    int iteration;
} qd_event_loop_stats_t;

void qd_event_loop_stats(qd_event_loop_t *loop, qd_event_loop_stats_t *stats);

/*
 * Utility Functions
 */

/* Set fd to non-blocking mode */
int qd_set_nonblocking(int fd);

/* Set fd to close-on-exec */
int qd_set_cloexec(int fd);

/* Create non-blocking socket pair */
int qd_socketpair(int fds[2]);

/* Get current event loop (for current thread) */
qd_event_loop_t *qd_event_loop_current(void);

#ifdef __cplusplus
}
#endif

#endif /* QD_EVENT_H */
