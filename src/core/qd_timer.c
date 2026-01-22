/*
 * QDaemon - Timer Wheel Implementation
 * Hashed hierarchical timer wheel for efficient timer management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "qdaemon/qd_timer.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_list.h"
#include "../util/qd_atomic.h"

/* Calculate slot for a given expiration time */
static void calculate_slot(qd_timer_wheel_t *wheel, uint64_t expires,
                           int *level, int *slot)
{
    uint64_t delta = expires - wheel->current_tick;

    if (delta < QD_TIMER_WHEEL_SIZE) {
        /* Level 0: 0-255 ticks */
        *level = 0;
        *slot = (wheel->current_tick + delta) & QD_TIMER_WHEEL_MASK;
    } else if (delta < (1UL << (2 * QD_TIMER_WHEEL_BITS))) {
        /* Level 1: 256 - 65535 ticks */
        *level = 1;
        *slot = ((wheel->current_tick + delta) >> QD_TIMER_WHEEL_BITS) & QD_TIMER_WHEEL_MASK;
    } else if (delta < (1UL << (3 * QD_TIMER_WHEEL_BITS))) {
        /* Level 2 */
        *level = 2;
        *slot = ((wheel->current_tick + delta) >> (2 * QD_TIMER_WHEEL_BITS)) & QD_TIMER_WHEEL_MASK;
    } else {
        /* Level 3 */
        *level = 3;
        *slot = ((wheel->current_tick + delta) >> (3 * QD_TIMER_WHEEL_BITS)) & QD_TIMER_WHEEL_MASK;
    }
}

/* Add timer to appropriate wheel slot */
static void add_timer_internal(qd_timer_wheel_t *wheel, qd_timer_t *timer)
{
    int level, slot;
    calculate_slot(wheel, timer->expires, &level, &slot);

    timer->wheel_level = level;
    timer->slot = slot;

    qd_list_add_tail(&timer->list, &wheel->wheels[level][slot]);
}

/* Remove timer from wheel */
static void remove_timer_internal(qd_timer_t *timer)
{
    if (timer->slot >= 0) {
        qd_list_del_init(&timer->list);
        timer->slot = -1;
        timer->wheel_level = -1;
    }
}

qd_timer_wheel_t *qd_timer_wheel_create(uint64_t tick_interval_ms)
{
    qd_timer_wheel_t *wheel = calloc(1, sizeof(qd_timer_wheel_t));
    if (!wheel)
        return NULL;

    wheel->tick_interval_ms = tick_interval_ms > 0 ? tick_interval_ms : 1;
    wheel->current_tick = 0;
    wheel->base_time = qd_time_now();

    /* Initialize all wheel slots */
    for (int level = 0; level < QD_TIMER_WHEEL_LEVELS; level++) {
        for (int slot = 0; slot < QD_TIMER_WHEEL_SIZE; slot++) {
            qd_list_init(&wheel->wheels[level][slot]);
        }
    }

    /* Initialize expired list */
    qd_list_init(&wheel->expired);

    pthread_mutex_init(&wheel->lock, NULL);
    qd_atomic_init(&wheel->num_timers, 0);

    return wheel;
}

void qd_timer_wheel_destroy(qd_timer_wheel_t *wheel)
{
    if (!wheel)
        return;

    pthread_mutex_lock(&wheel->lock);

    /* Free all timers in wheel */
    for (int level = 0; level < QD_TIMER_WHEEL_LEVELS; level++) {
        for (int slot = 0; slot < QD_TIMER_WHEEL_SIZE; slot++) {
            struct qd_list_head *list = &wheel->wheels[level][slot];
            while (!qd_list_empty(list)) {
                qd_timer_t *timer = qd_list_first_entry(list, qd_timer_t, list);
                qd_list_del(&timer->list);
                /* Don't free timers - they may be embedded or owned by caller */
            }
        }
    }

    pthread_mutex_unlock(&wheel->lock);
    pthread_mutex_destroy(&wheel->lock);

    free(wheel);
}

/* Cascade timers from higher level to lower level */
static void cascade_timers(qd_timer_wheel_t *wheel, int level)
{
    if (level >= QD_TIMER_WHEEL_LEVELS)
        return;

    int slot = (wheel->current_tick >> (level * QD_TIMER_WHEEL_BITS)) & QD_TIMER_WHEEL_MASK;
    struct qd_list_head *list = &wheel->wheels[level][slot];

    struct qd_list_head tmp;
    qd_list_init(&tmp);
    qd_list_splice(list, &tmp);

    while (!qd_list_empty(&tmp)) {
        qd_timer_t *timer = qd_list_first_entry(&tmp, qd_timer_t, list);
        qd_list_del_init(&timer->list);
        timer->slot = -1;

        /* Re-add to appropriate slot */
        add_timer_internal(wheel, timer);
    }
}

int qd_timer_wheel_tick(qd_timer_wheel_t *wheel)
{
    if (!wheel)
        return 0;

    /* Calculate how many ticks have passed */
    qd_time_t now = qd_time_now();
    uint64_t elapsed_ms = now - wheel->base_time;
    uint64_t target_tick = elapsed_ms / wheel->tick_interval_ms;

    if (target_tick <= wheel->current_tick)
        return 0;

    int expired_count = 0;

    pthread_mutex_lock(&wheel->lock);

    while (wheel->current_tick < target_tick) {
        /* Process current slot in level 0 */
        int slot = wheel->current_tick & QD_TIMER_WHEEL_MASK;
        struct qd_list_head *list = &wheel->wheels[0][slot];

        /* Move expired timers to expired list */
        while (!qd_list_empty(list)) {
            qd_timer_t *timer = qd_list_first_entry(list, qd_timer_t, list);
            qd_list_del_init(&timer->list);
            timer->slot = -1;

            if (timer->expires <= wheel->current_tick) {
                qd_list_add_tail(&timer->list, &wheel->expired);
                expired_count++;
            } else {
                /* Timer not yet expired, re-add */
                add_timer_internal(wheel, timer);
            }
        }

        wheel->current_tick++;
        wheel->tick_count++;

        /* Cascade timers from higher levels when necessary */
        if ((wheel->current_tick & QD_TIMER_WHEEL_MASK) == 0) {
            cascade_timers(wheel, 1);
            if ((wheel->current_tick & ((1 << (2 * QD_TIMER_WHEEL_BITS)) - 1)) == 0) {
                cascade_timers(wheel, 2);
                if ((wheel->current_tick & ((1 << (3 * QD_TIMER_WHEEL_BITS)) - 1)) == 0) {
                    cascade_timers(wheel, 3);
                }
            }
        }
    }

    /* Process expired timers */
    struct qd_list_head expired;
    qd_list_init(&expired);
    qd_list_splice(&wheel->expired, &expired);

    pthread_mutex_unlock(&wheel->lock);

    /* Call callbacks outside of lock */
    while (!qd_list_empty(&expired)) {
        qd_timer_t *timer = qd_list_first_entry(&expired, qd_timer_t, list);
        qd_list_del_init(&timer->list);

        qd_atomic_store(&timer->state, QD_TIMER_RUNNING);

        if (timer->callback)
            timer->callback(timer, timer->arg);

        wheel->timers_expired++;

        /* Handle periodic timers */
        if (timer->flags & QD_TIMER_PERIODIC) {
            pthread_mutex_lock(&wheel->lock);
            timer->expires = wheel->current_tick + timer->interval;
            add_timer_internal(wheel, timer);
            qd_atomic_store(&timer->state, QD_TIMER_SCHEDULED);
            pthread_mutex_unlock(&wheel->lock);
        } else {
            qd_atomic_store(&timer->state, QD_TIMER_IDLE);
            qd_atomic_fetch_sub(&wheel->num_timers, 1);
        }
    }

    return expired_count;
}

int qd_timer_wheel_tick_to(qd_timer_wheel_t *wheel, qd_time_t now)
{
    if (!wheel)
        return 0;

    wheel->base_time = now - (wheel->current_tick * wheel->tick_interval_ms);
    return qd_timer_wheel_tick(wheel);
}

int64_t qd_timer_wheel_next_timeout(qd_timer_wheel_t *wheel)
{
    if (!wheel)
        return -1;

    if (qd_atomic_load(&wheel->num_timers) == 0)
        return -1;

    pthread_mutex_lock(&wheel->lock);

    /* Check level 0 first */
    for (int i = 0; i < QD_TIMER_WHEEL_SIZE; i++) {
        int slot = (wheel->current_tick + i) & QD_TIMER_WHEEL_MASK;
        if (!qd_list_empty(&wheel->wheels[0][slot])) {
            pthread_mutex_unlock(&wheel->lock);
            return i * wheel->tick_interval_ms;
        }
    }

    /* Check higher levels - return a reasonable upper bound */
    pthread_mutex_unlock(&wheel->lock);
    return wheel->tick_interval_ms;
}

qd_timer_t *qd_timer_create(qd_timer_wheel_t *wheel, qd_timer_cb_t callback, void *arg)
{
    if (!wheel)
        return NULL;

    qd_timer_t *timer = calloc(1, sizeof(qd_timer_t));
    if (!timer)
        return NULL;

    qd_timer_init(timer, wheel, callback, arg);
    return timer;
}

void qd_timer_init(qd_timer_t *timer, qd_timer_wheel_t *wheel,
                   qd_timer_cb_t callback, void *arg)
{
    if (!timer)
        return;

    memset(timer, 0, sizeof(*timer));
    timer->wheel = wheel;
    timer->callback = callback;
    timer->arg = arg;
    timer->slot = -1;
    timer->wheel_level = -1;
    qd_list_init(&timer->list);
    qd_atomic_init(&timer->state, QD_TIMER_IDLE);
}

void qd_timer_destroy(qd_timer_t *timer)
{
    if (!timer)
        return;

    qd_timer_stop(timer);
    free(timer);
}

int qd_timer_start(qd_timer_t *timer, uint64_t delay_ms)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    qd_timer_wheel_t *wheel = timer->wheel;

    pthread_mutex_lock(&wheel->lock);

    /* Remove if already scheduled */
    remove_timer_internal(timer);

    /* Calculate expiration */
    uint64_t delay_ticks = delay_ms / wheel->tick_interval_ms;
    if (delay_ticks == 0)
        delay_ticks = 1;

    timer->expires = wheel->current_tick + delay_ticks;
    timer->interval = delay_ticks;
    timer->flags &= ~QD_TIMER_PERIODIC;

    add_timer_internal(wheel, timer);
    qd_atomic_store(&timer->state, QD_TIMER_SCHEDULED);
    qd_atomic_fetch_add(&wheel->num_timers, 1);
    wheel->timers_added++;

    pthread_mutex_unlock(&wheel->lock);

    return QD_OK;
}

int qd_timer_start_periodic(qd_timer_t *timer, uint64_t interval_ms)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    qd_timer_wheel_t *wheel = timer->wheel;

    pthread_mutex_lock(&wheel->lock);

    remove_timer_internal(timer);

    uint64_t interval_ticks = interval_ms / wheel->tick_interval_ms;
    if (interval_ticks == 0)
        interval_ticks = 1;

    timer->expires = wheel->current_tick + interval_ticks;
    timer->interval = interval_ticks;
    timer->flags |= QD_TIMER_PERIODIC;

    add_timer_internal(wheel, timer);
    qd_atomic_store(&timer->state, QD_TIMER_SCHEDULED);
    qd_atomic_fetch_add(&wheel->num_timers, 1);
    wheel->timers_added++;

    pthread_mutex_unlock(&wheel->lock);

    return QD_OK;
}

int qd_timer_start_at(qd_timer_t *timer, qd_time_t when)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    qd_time_t now = qd_time_now();
    if (when <= now)
        return qd_timer_start(timer, 0);

    return qd_timer_start(timer, when - now);
}

int qd_timer_stop(qd_timer_t *timer)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    qd_timer_wheel_t *wheel = timer->wheel;

    pthread_mutex_lock(&wheel->lock);

    if (timer->slot >= 0) {
        remove_timer_internal(timer);
        qd_atomic_fetch_sub(&wheel->num_timers, 1);
        wheel->timers_cancelled++;
    }

    qd_atomic_store(&timer->state, QD_TIMER_CANCELLED);

    pthread_mutex_unlock(&wheel->lock);

    return QD_OK;
}

int qd_timer_restart(qd_timer_t *timer)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    if (timer->flags & QD_TIMER_PERIODIC) {
        return qd_timer_start_periodic(timer, timer->interval * timer->wheel->tick_interval_ms);
    } else {
        return qd_timer_start(timer, timer->interval * timer->wheel->tick_interval_ms);
    }
}

int qd_timer_set_delay(qd_timer_t *timer, uint64_t delay_ms)
{
    if (!timer || !timer->wheel)
        return QD_ERR_INVAL;

    uint64_t delay_ticks = delay_ms / timer->wheel->tick_interval_ms;
    if (delay_ticks == 0)
        delay_ticks = 1;

    timer->interval = delay_ticks;

    if (qd_atomic_load(&timer->state) == QD_TIMER_SCHEDULED) {
        return qd_timer_restart(timer);
    }

    return QD_OK;
}

int qd_timer_is_active(qd_timer_t *timer)
{
    if (!timer)
        return 0;
    return qd_atomic_load(&timer->state) == QD_TIMER_SCHEDULED;
}

int64_t qd_timer_remaining(qd_timer_t *timer)
{
    if (!timer || !timer->wheel)
        return -1;

    if (qd_atomic_load(&timer->state) != QD_TIMER_SCHEDULED)
        return -1;

    qd_timer_wheel_t *wheel = timer->wheel;

    pthread_mutex_lock(&wheel->lock);
    int64_t remaining = (int64_t)(timer->expires - wheel->current_tick);
    pthread_mutex_unlock(&wheel->lock);

    if (remaining < 0)
        remaining = 0;

    return remaining * wheel->tick_interval_ms;
}

qd_timer_t *qd_timer_add(qd_timer_wheel_t *wheel, uint64_t delay_ms,
                          qd_timer_cb_t callback, void *arg)
{
    qd_timer_t *timer = qd_timer_create(wheel, callback, arg);
    if (!timer)
        return NULL;

    if (qd_timer_start(timer, delay_ms) != QD_OK) {
        qd_timer_destroy(timer);
        return NULL;
    }

    return timer;
}

qd_timer_t *qd_timer_add_periodic(qd_timer_wheel_t *wheel, uint64_t interval_ms,
                                   qd_timer_cb_t callback, void *arg)
{
    qd_timer_t *timer = qd_timer_create(wheel, callback, arg);
    if (!timer)
        return NULL;

    if (qd_timer_start_periodic(timer, interval_ms) != QD_OK) {
        qd_timer_destroy(timer);
        return NULL;
    }

    return timer;
}

void qd_timer_cancel(qd_timer_t *timer)
{
    qd_timer_stop(timer);
}

void qd_timer_wheel_stats(qd_timer_wheel_t *wheel, qd_timer_wheel_stats_t *stats)
{
    if (!wheel || !stats)
        return;

    pthread_mutex_lock(&wheel->lock);

    stats->timers_added = wheel->timers_added;
    stats->timers_expired = wheel->timers_expired;
    stats->timers_cancelled = wheel->timers_cancelled;
    stats->tick_count = wheel->tick_count;
    stats->num_timers = qd_atomic_load(&wheel->num_timers);
    stats->current_tick = wheel->current_tick;

    pthread_mutex_unlock(&wheel->lock);
}

int qd_timer_wheel_attach(qd_timer_wheel_t *wheel, qd_event_loop_t *loop)
{
    /* Timer wheel is automatically managed by event loop's timerfd */
    QD_UNUSED(wheel);
    QD_UNUSED(loop);
    return QD_OK;
}

void qd_timer_wheel_detach(qd_timer_wheel_t *wheel)
{
    QD_UNUSED(wheel);
}
