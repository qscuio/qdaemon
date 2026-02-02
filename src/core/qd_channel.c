/*
 * QDaemon - Channel Implementation
 * Lock-free MPSC bounded queue with eventfd for poll integration
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>

#include "qdaemon/qd_channel.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Channel structure */
struct qd_channel {
    /* Ring buffer */
    uint8_t *buffer;           /* Ring buffer storage */
    size_t capacity;           /* Total capacity in items */
    size_t item_size;          /* Size of each item */
    size_t mask;               /* Capacity-1 for power-of-2 optimization */

    /* Atomic indices for lock-free producer/consumer */
    _Atomic size_t head;       /* Consumer position */
    _Atomic size_t tail;       /* Producer position (reservation point) */

    /* Committed flags for lock-free MPSC */
    _Atomic uint8_t *committed; /* Per-slot: 1 = data written, ready to read */

    /* Eventfd for notifications */
    int eventfd;               /* Eventfd for poll integration */

    /* Synchronization for blocking operations only */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    /* Configuration */
    int flags;
    _Atomic int closed;
};

/* Round up to next power of 2 */
static size_t round_up_pow2(size_t n)
{
    if (n == 0)
        return 1;

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    n |= n >> 32;
#endif
    n++;

    return n;
}

/* Write to eventfd (non-blocking) */
static inline void signal_event(qd_channel_t *chan)
{
    const uint64_t one = 1;
    int ret = write(chan->eventfd, &one, sizeof(one));
    (void)ret;
}

/* Spin pause for busy-wait loops */
static inline void cpu_pause(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    sched_yield();
#endif
}

qd_channel_t *qd_channel_create(const qd_channel_config_t *config)
{
    if (!config || config->item_size == 0) {
        return NULL;
    }

    qd_channel_t *chan = qd_calloc(1, sizeof(*chan));
    if (!chan) {
        return NULL;
    }

    /* Initialize configuration */
    chan->item_size = config->item_size;
    chan->flags = config->flags;

    /* Round capacity up to power of 2 for fast modulo */
    if (config->capacity == 0) {
        chan->capacity = 1024;
    } else {
        chan->capacity = round_up_pow2(config->capacity);
    }
    chan->mask = chan->capacity - 1;

    /* Allocate ring buffer */
    chan->buffer = qd_malloc(chan->capacity * chan->item_size);
    if (!chan->buffer) {
        qd_free(chan);
        return NULL;
    }

    /* Allocate committed flags array */
    chan->committed = qd_calloc(chan->capacity, sizeof(_Atomic uint8_t));
    if (!chan->committed) {
        qd_free(chan->buffer);
        qd_free(chan);
        return NULL;
    }

    /* Create eventfd for notifications */
    chan->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (chan->eventfd < 0) {
        qd_log_error("eventfd create failed: %s", strerror(errno));
        qd_free(chan->committed);
        qd_free(chan->buffer);
        qd_free(chan);
        return NULL;
    }

    /* Initialize synchronization (for blocking ops only) */
    pthread_mutex_init(&chan->lock, NULL);
    pthread_cond_init(&chan->not_empty, NULL);
    pthread_cond_init(&chan->not_full, NULL);

    /* Initialize atomic indices */
    atomic_init(&chan->head, 0);
    atomic_init(&chan->tail, 0);
    atomic_init(&chan->closed, 0);

    qd_log_debug("Created channel: capacity=%zu, item_size=%zu, flags=0x%x",
                 chan->capacity, chan->item_size, chan->flags);

    return chan;
}

void qd_channel_destroy(qd_channel_t *chan)
{
    if (!chan) {
        return;
    }

    if (chan->eventfd >= 0) {
        close(chan->eventfd);
    }

    pthread_mutex_destroy(&chan->lock);
    pthread_cond_destroy(&chan->not_empty);
    pthread_cond_destroy(&chan->not_full);

    qd_free(chan->committed);
    qd_free(chan->buffer);
    qd_free(chan);
}

/*
 * Lock-free MPSC try_send
 * Algorithm:
 * 1. CAS to reserve slot (increment tail)
 * 2. Write data to reserved slot
 * 3. Mark slot as committed
 * 4. Signal if was empty
 */
int qd_channel_try_send(qd_channel_t *chan, const void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    if (atomic_load_explicit(&chan->closed, memory_order_relaxed)) {
        return QD_ERR_IO;
    }

    size_t tail, head, count;

    /* CAS loop to reserve a slot */
    do {
        tail = atomic_load_explicit(&chan->tail, memory_order_relaxed);
        head = atomic_load_explicit(&chan->head, memory_order_acquire);
        count = tail - head;

        if (count >= chan->capacity) {
            /* Full */
            if (chan->flags & QD_CHAN_OVERFLOW) {
                /* Overflow mode - fall back to locked path for safe head advance */
                pthread_mutex_lock(&chan->lock);
                tail = atomic_load(&chan->tail);
                head = atomic_load(&chan->head);
                if (tail - head >= chan->capacity) {
                    /* Advance head, dropping oldest */
                    size_t drop_pos = head & chan->mask;
                    atomic_store_explicit(&chan->committed[drop_pos], 0, memory_order_relaxed);
                    atomic_fetch_add(&chan->head, 1);
                }
                /* Now do normal send under lock */
                size_t pos = tail & chan->mask;
                memcpy(chan->buffer + pos * chan->item_size, item, chan->item_size);
                atomic_store_explicit(&chan->committed[pos], 1, memory_order_release);
                atomic_store(&chan->tail, tail + 1);
                int was_empty = (tail == head);
                pthread_mutex_unlock(&chan->lock);
                if (was_empty) {
                    signal_event(chan);
                }
                return QD_OK;
            }
            return QD_ERR_BUSY;
        }
    } while (!atomic_compare_exchange_weak_explicit(
                &chan->tail, &tail, tail + 1,
                memory_order_acq_rel, memory_order_relaxed));

    /* We reserved slot at 'tail' - write data */
    size_t pos = tail & chan->mask;
    memcpy(chan->buffer + pos * chan->item_size, item, chan->item_size);

    /* Mark as committed */
    atomic_store_explicit(&chan->committed[pos], 1, memory_order_release);

    /* Signal only on empty -> non-empty transition */
    if (tail == head) {
        signal_event(chan);
    }

    return QD_OK;
}

/*
 * Lock-free single-consumer try_recv
 * Must wait for committed flag in case producer reserved but hasn't written yet
 */
int qd_channel_try_recv(qd_channel_t *chan, void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    if (atomic_load_explicit(&chan->closed, memory_order_relaxed)) {
        return QD_ERR_IO;
    }

    size_t head = atomic_load_explicit(&chan->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&chan->tail, memory_order_acquire);

    if (head >= tail) {
        return QD_ERR_AGAIN;
    }

    size_t pos = head & chan->mask;

    /* Wait for data to be committed (producer may have reserved but not written) */
    int spins = 0;
    while (!atomic_load_explicit(&chan->committed[pos], memory_order_acquire)) {
        if (++spins > 1000) {
            cpu_pause();
        }
        if (spins > 10000) {
            sched_yield();
            spins = 0;
        }
    }

    /* Read data */
    memcpy(item, chan->buffer + pos * chan->item_size, chan->item_size);

    /* Clear committed flag for this slot */
    atomic_store_explicit(&chan->committed[pos], 0, memory_order_release);

    /* Advance head */
    atomic_store_explicit(&chan->head, head + 1, memory_order_release);

    return QD_OK;
}

int qd_channel_try_recv_batch(qd_channel_t *chan, void *items, int max_items)
{
    if (!chan || !items || max_items <= 0) {
        return QD_ERR_INVAL;
    }

    if (atomic_load_explicit(&chan->closed, memory_order_relaxed)) {
        return QD_ERR_IO;
    }

    size_t head = atomic_load_explicit(&chan->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&chan->tail, memory_order_acquire);

    if (head >= tail) {
        return 0;
    }

    size_t available = tail - head;
    size_t to_read = (available < (size_t)max_items) ? available : (size_t)max_items;
    uint8_t *dst = (uint8_t *)items;
    int processed = 0;

    for (size_t i = 0; i < to_read; i++) {
        size_t pos = (head + i) & chan->mask;
        int spins = 0;

        while (!atomic_load_explicit(&chan->committed[pos], memory_order_acquire)) {
            if (++spins > 1000) {
                cpu_pause();
            }
            if (spins > 10000) {
                break;
            }
        }

        if (!atomic_load_explicit(&chan->committed[pos], memory_order_acquire)) {
            break;
        }

        memcpy(dst + (processed * chan->item_size),
               chan->buffer + pos * chan->item_size,
               chan->item_size);

        atomic_store_explicit(&chan->committed[pos], 0, memory_order_release);
        processed++;
    }

    if (processed > 0) {
        atomic_store_explicit(&chan->head, head + (size_t)processed, memory_order_release);
    }

    return processed;
}

/* Blocking send - uses lock for wait */
int qd_channel_send(qd_channel_t *chan, const void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    /* Fast path: try lock-free first */
    int ret = qd_channel_try_send(chan, item);
    if (ret != QD_ERR_BUSY) {
        return ret;
    }

    /* Slow path: need to wait for space */
    if (chan->flags & QD_CHAN_NONBLOCK) {
        return QD_ERR_BUSY;
    }

    pthread_mutex_lock(&chan->lock);

    while (1) {
        if (atomic_load(&chan->closed)) {
            pthread_mutex_unlock(&chan->lock);
            return QD_ERR_IO;
        }

        /* Try again under lock */
        ret = qd_channel_try_send(chan, item);
        if (ret != QD_ERR_BUSY) {
            pthread_mutex_unlock(&chan->lock);
            return ret;
        }

        /* Wait for space */
        pthread_cond_wait(&chan->not_full, &chan->lock);
    }
}

/* Blocking recv */
int qd_channel_recv(qd_channel_t *chan, void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    /* Fast path: try lock-free first */
    int ret = qd_channel_try_recv(chan, item);
    if (ret != QD_ERR_AGAIN) {
        return ret;
    }

    /* Slow path: need to wait for data */
    if (chan->flags & QD_CHAN_NONBLOCK) {
        return QD_ERR_AGAIN;
    }

    pthread_mutex_lock(&chan->lock);

    while (1) {
        if (atomic_load(&chan->closed)) {
            pthread_mutex_unlock(&chan->lock);
            return QD_ERR_IO;
        }

        ret = qd_channel_try_recv(chan, item);
        if (ret != QD_ERR_AGAIN) {
            pthread_mutex_unlock(&chan->lock);
            return ret;
        }

        pthread_cond_wait(&chan->not_empty, &chan->lock);
    }
}

int qd_channel_fd(qd_channel_t *chan)
{
    return chan ? chan->eventfd : -1;
}

void qd_channel_ack(qd_channel_t *chan)
{
    if (!chan || chan->eventfd < 0) {
        return;
    }

    uint64_t val;
    while (read(chan->eventfd, &val, sizeof(val)) > 0) {
        /* Drain eventfd */
    }
}

void qd_channel_close(qd_channel_t *chan)
{
    if (!chan) {
        return;
    }

    atomic_store(&chan->closed, 1);

    pthread_mutex_lock(&chan->lock);
    pthread_cond_broadcast(&chan->not_empty);
    pthread_cond_broadcast(&chan->not_full);
    pthread_mutex_unlock(&chan->lock);
}

int qd_channel_capacity(qd_channel_t *chan)
{
    return chan ? (int)chan->capacity : -1;
}

int qd_channel_size(qd_channel_t *chan)
{
    if (!chan) {
        return -1;
    }
    size_t tail = atomic_load(&chan->tail);
    size_t head = atomic_load(&chan->head);
    return (int)(tail - head);
}

size_t qd_channel_item_size(qd_channel_t *chan)
{
    return chan ? chan->item_size : 0;
}
