/*
 * QDaemon - Channel Implementation
 * MPSC bounded queue with eventfd for poll integration
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

#include "qdaemon/qd_channel.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Channel structure */
struct qd_channel {
    /* Ring buffer */
    uint8_t *buffer;           /* Ring buffer storage */
    size_t capacity;           /* Total capacity in items */
    size_t item_size;         /* Size of each item */
    size_t mask;              /* Capacity-1 for power-of-2 optimization */

    /* Atomic indices for lock-free producer/consumer */
    _Atomic size_t head;      /* Consumer position */
    _Atomic size_t tail;      /* Producer position */

    /* Eventfd for notifications */
    int eventfd;              /* Eventfd for poll integration */
    _Atomic size_t pending;   /* Pending eventfd writes (to avoid overflow) */

    /* Synchronization for blocking operations */
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    /* Configuration */
    int flags;
    _Atomic int closed;
};

/* Check if n is a power of 2 */
static inline int is_power_of_two(size_t n)
{
    return (n & (n - 1)) == 0;
}

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
static void signal_event(qd_channel_t *chan)
{
    const uint64_t one = 1;
    if (atomic_fetch_add(&chan->pending, 1) == 0) {
        /* No pending signals, write to eventfd */
        int ret = write(chan->eventfd, &one, sizeof(one));
        (void)ret;  /* Ignore EAGAIN/EWOULDBLOCK if counter overflows */
    }
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
        chan->capacity = 1024;  /* Default */
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

    /* Create eventfd for notifications */
    chan->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (chan->eventfd < 0) {
        qd_log_error("eventfd create failed: %s", strerror(errno));
        qd_free(chan->buffer);
        qd_free(chan);
        return NULL;
    }

    /* Initialize synchronization */
    pthread_mutex_init(&chan->lock, NULL);
    pthread_cond_init(&chan->not_empty, NULL);
    pthread_cond_init(&chan->not_full, NULL);

    /* Initialize atomic indices */
    atomic_init(&chan->head, 0);
    atomic_init(&chan->tail, 0);
    atomic_init(&chan->pending, 0);
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

    qd_free(chan->buffer);
    qd_free(chan);
}

int qd_channel_send(qd_channel_t *chan, const void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }
    if (atomic_load(&chan->closed)) {
        return QD_ERR_IO;
    }

    /* Get current positions */
    size_t tail = atomic_load(&chan->tail);
    size_t head = atomic_load(&chan->head);
    size_t count = tail - head;

    /* Wait for space if not non-blocking */
    if (count >= chan->capacity) {
        if (chan->flags & QD_CHAN_NONBLOCK) {
            return QD_ERR_BUSY;
        }

        pthread_mutex_lock(&chan->lock);
        while (1) {
            tail = atomic_load(&chan->tail);
            head = atomic_load(&chan->head);
            count = tail - head;

            if (count < chan->capacity) {
                break;
            }
            if (atomic_load(&chan->closed)) {
                pthread_mutex_unlock(&chan->lock);
                return QD_ERR_IO;
            }
            pthread_cond_wait(&chan->not_full, &chan->lock);
        }
        pthread_mutex_unlock(&chan->lock);
    }

    /* Handle overflow by dropping oldest */
    if (chan->flags & QD_CHAN_OVERFLOW && count >= chan->capacity) {
        /* Drop oldest: advance head */
        atomic_fetch_add(&chan->head, 1);
    }

    /* Write item to ring buffer */
    size_t pos = tail & chan->mask;
    memcpy(chan->buffer + pos * chan->item_size, item, chan->item_size);

    /* Update tail (release semantics ensures write is visible before tail update) */
    atomic_thread_fence(memory_order_release);
    atomic_store(&chan->tail, tail + 1);

    /* Signal consumer */
    signal_event(chan);
    pthread_cond_signal(&chan->not_empty);

    return QD_OK;
}

int qd_channel_recv(qd_channel_t *chan, void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    /* Get current positions */
    size_t head = atomic_load(&chan->head);
    size_t tail = atomic_load(&chan->tail);

    /* Wait for data if not non-blocking */
    while (head >= tail) {
        if (chan->flags & QD_CHAN_NONBLOCK) {
            return QD_ERR_AGAIN;
        }
        if (atomic_load(&chan->closed)) {
            return QD_ERR_IO;
        }

        pthread_mutex_lock(&chan->lock);
        head = atomic_load(&chan->head);
        tail = atomic_load(&chan->tail);
        if (head >= tail && !atomic_load(&chan->closed)) {
            pthread_cond_wait(&chan->not_empty, &chan->lock);
        }
        pthread_mutex_unlock(&chan->lock);

        head = atomic_load(&chan->head);
        tail = atomic_load(&chan->tail);
    }

    /* Read item from ring buffer */
    size_t pos = head & chan->mask;
    memcpy(item, chan->buffer + pos * chan->item_size, chan->item_size);

    /* Update head */
    atomic_fetch_add(&chan->head, 1);

    /* Signal producer */
    pthread_cond_signal(&chan->not_full);

    return QD_OK;
}

int qd_channel_try_send(qd_channel_t *chan, const void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }
    if (atomic_load(&chan->closed)) {
        return QD_ERR_IO;
    }

    size_t tail = atomic_load(&chan->tail);
    size_t head = atomic_load(&chan->head);
    size_t count = tail - head;

    if (count >= chan->capacity) {
        /* Handle overflow */
        if (chan->flags & QD_CHAN_OVERFLOW) {
            atomic_fetch_add(&chan->head, 1);
        } else {
            return QD_ERR_BUSY;
        }
    }

    /* Write item */
    size_t pos = tail & chan->mask;
    memcpy(chan->buffer + pos * chan->item_size, item, chan->item_size);

    atomic_thread_fence(memory_order_release);
    atomic_store(&chan->tail, tail + 1);

    signal_event(chan);
    pthread_cond_signal(&chan->not_empty);

    return QD_OK;
}

int qd_channel_try_recv(qd_channel_t *chan, void *item)
{
    if (!chan || !item) {
        return QD_ERR_INVAL;
    }

    size_t head = atomic_load(&chan->head);
    size_t tail = atomic_load(&chan->tail);

    if (head >= tail) {
        if (atomic_load(&chan->closed)) {
            return QD_ERR_IO;
        }
        return QD_ERR_AGAIN;
    }

    /* Read item */
    size_t pos = head & chan->mask;
    memcpy(item, chan->buffer + pos * chan->item_size, chan->item_size);

    atomic_fetch_add(&chan->head, 1);

    pthread_cond_signal(&chan->not_full);

    return QD_OK;
}

int qd_channel_fd(qd_channel_t *chan)
{
    if (!chan) {
        return -1;
    }
    return chan->eventfd;
}

void qd_channel_ack(qd_channel_t *chan)
{
    if (!chan || chan->eventfd < 0) {
        return;
    }

    uint64_t val;
    /* Read from eventfd to clear it */
    while (read(chan->eventfd, &val, sizeof(val)) > 0) {
        /* Keep reading until EAGAIN if needed,
           but usually one read is enough for EFD_SEMAPHORE or default */
    }

    /* Reset pending count so next send will trigger eventfd write */
    atomic_store(&chan->pending, 0);
}

int qd_channel_capacity(qd_channel_t *chan)
{
    if (!chan) {
        return -1;
    }
    return (int)chan->capacity;
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
