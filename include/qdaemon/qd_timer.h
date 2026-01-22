/*
 * QDaemon - Timer Wheel API
 * Hashed hierarchical timer wheel for efficient timer management
 */

#ifndef QD_TIMER_H
#define QD_TIMER_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "qd_common.h"
#include "../src/util/qd_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_timer qd_timer_t;
typedef struct qd_timer_wheel qd_timer_wheel_t;
typedef struct qd_event_loop qd_event_loop_t;

/* Timer callback function */
typedef void (*qd_timer_cb_t)(qd_timer_t *timer, void *arg);

/* Timer flags */
typedef enum {
    QD_TIMER_ONESHOT  = 0x00,    /* One-shot timer (default) */
    QD_TIMER_PERIODIC = 0x01,    /* Repeating timer */
    QD_TIMER_ACTIVE   = 0x02,    /* Timer is active */
    QD_TIMER_PENDING  = 0x04,    /* Timer is pending execution */
} qd_timer_flags_t;

/* Timer state */
typedef enum {
    QD_TIMER_IDLE = 0,
    QD_TIMER_SCHEDULED,
    QD_TIMER_RUNNING,
    QD_TIMER_CANCELLED,
} qd_timer_state_t;

/* Timer structure */
struct qd_timer {
    uint64_t expires;            /* Expiration time (absolute ticks) */
    uint64_t interval;           /* Interval for periodic timers (ticks) */
    qd_timer_cb_t callback;      /* Timer callback */
    void *arg;                   /* Callback argument */
    uint32_t flags;              /* Timer flags */
    _Atomic int state;           /* Timer state */
    int slot;                    /* Current wheel slot (-1 if not scheduled) */
    int wheel_level;             /* Wheel level */
    struct qd_list_head list;    /* List linkage */
    qd_timer_wheel_t *wheel;     /* Owning wheel */
};

/* Timer wheel configuration */
#define QD_TIMER_WHEEL_BITS      8                          /* Bits per wheel level */
#define QD_TIMER_WHEEL_SIZE      (1 << QD_TIMER_WHEEL_BITS) /* Slots per wheel (256) */
#define QD_TIMER_WHEEL_MASK      (QD_TIMER_WHEEL_SIZE - 1)
#define QD_TIMER_WHEEL_LEVELS    4                          /* Number of wheel levels */

/* With 1ms tick and 4 levels of 256 slots:
 * Level 0: 0-255ms
 * Level 1: 256ms - 65.5s
 * Level 2: 65.5s - 4.6h
 * Level 3: 4.6h - 49.7 days
 */

/* Timer wheel structure */
struct qd_timer_wheel {
    uint64_t current_tick;       /* Current tick count */
    uint64_t tick_interval_ms;   /* Milliseconds per tick */
    qd_time_t base_time;         /* Base time for tick calculations */

    /* Hierarchical wheel arrays */
    struct qd_list_head wheels[QD_TIMER_WHEEL_LEVELS][QD_TIMER_WHEEL_SIZE];

    /* Pending expired timers */
    struct qd_list_head expired;

    /* Lock for thread safety */
    pthread_mutex_t lock;

    /* Statistics */
    uint64_t timers_added;
    uint64_t timers_expired;
    uint64_t timers_cancelled;
    uint64_t tick_count;

    /* Timer count */
    _Atomic int num_timers;
};

/*
 * Timer Wheel API
 */

/* Create timer wheel */
qd_timer_wheel_t *qd_timer_wheel_create(uint64_t tick_interval_ms);

/* Destroy timer wheel */
void qd_timer_wheel_destroy(qd_timer_wheel_t *wheel);

/* Process timers (call this periodically, typically from event loop) */
int qd_timer_wheel_tick(qd_timer_wheel_t *wheel);

/* Process timers with custom current time */
int qd_timer_wheel_tick_to(qd_timer_wheel_t *wheel, qd_time_t now);

/* Get time until next timer expires (for poll timeout) */
int64_t qd_timer_wheel_next_timeout(qd_timer_wheel_t *wheel);

/*
 * Timer API
 */

/* Create timer */
qd_timer_t *qd_timer_create(qd_timer_wheel_t *wheel, qd_timer_cb_t callback, void *arg);

/* Initialize timer (for embedded timers) */
void qd_timer_init(qd_timer_t *timer, qd_timer_wheel_t *wheel,
                   qd_timer_cb_t callback, void *arg);

/* Destroy timer */
void qd_timer_destroy(qd_timer_t *timer);

/* Start one-shot timer with delay in milliseconds */
int qd_timer_start(qd_timer_t *timer, uint64_t delay_ms);

/* Start periodic timer */
int qd_timer_start_periodic(qd_timer_t *timer, uint64_t interval_ms);

/* Start timer at absolute time */
int qd_timer_start_at(qd_timer_t *timer, qd_time_t when);

/* Stop/cancel timer */
int qd_timer_stop(qd_timer_t *timer);

/* Restart timer with same settings */
int qd_timer_restart(qd_timer_t *timer);

/* Modify timer delay (must be stopped or will restart) */
int qd_timer_set_delay(qd_timer_t *timer, uint64_t delay_ms);

/* Check if timer is active */
int qd_timer_is_active(qd_timer_t *timer);

/* Get remaining time in milliseconds */
int64_t qd_timer_remaining(qd_timer_t *timer);

/*
 * Convenience functions (using default wheel from event loop)
 */

/* Add one-shot timer */
qd_timer_t *qd_timer_add(qd_timer_wheel_t *wheel, uint64_t delay_ms,
                          qd_timer_cb_t callback, void *arg);

/* Add periodic timer */
qd_timer_t *qd_timer_add_periodic(qd_timer_wheel_t *wheel, uint64_t interval_ms,
                                   qd_timer_cb_t callback, void *arg);

/* Cancel timer */
void qd_timer_cancel(qd_timer_t *timer);

/*
 * Statistics
 */

typedef struct qd_timer_wheel_stats {
    uint64_t timers_added;
    uint64_t timers_expired;
    uint64_t timers_cancelled;
    uint64_t tick_count;
    int num_timers;
    uint64_t current_tick;
} qd_timer_wheel_stats_t;

void qd_timer_wheel_stats(qd_timer_wheel_t *wheel, qd_timer_wheel_stats_t *stats);

/*
 * Integration with event loop
 */

/* Attach timer wheel to event loop */
int qd_timer_wheel_attach(qd_timer_wheel_t *wheel, qd_event_loop_t *loop);

/* Detach timer wheel from event loop */
void qd_timer_wheel_detach(qd_timer_wheel_t *wheel);

#ifdef __cplusplus
}
#endif

#endif /* QD_TIMER_H */
