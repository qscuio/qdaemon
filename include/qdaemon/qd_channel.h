/*
 * QDaemon - Channel API
 * MPSC (multi-producer, single-consumer) bounded queue with eventfd
 */

#ifndef QD_CHANNEL_H
#define QD_CHANNEL_H

#include <stddef.h>
#include <stdint.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qd_channel qd_channel_t;

/* Configuration */
typedef struct qd_channel_config {
    size_t capacity;        /* Max items (0 = unbounded) */
    size_t item_size;       /* Size of each item */
    int flags;              /* QD_CHAN_* flags */
} qd_channel_config_t;

#define QD_CHAN_NONBLOCK  0x01  /* send/recv never block */
#define QD_CHAN_OVERFLOW  0x02  /* drop oldest on overflow */

#define QD_CHANNEL_CONFIG_DEFAULT { \
    .capacity = 1024, \
    .item_size = sizeof(void*), \
    .flags = 0 \
}

/* Lifecycle */
qd_channel_t *qd_channel_create(const qd_channel_config_t *config);
void qd_channel_destroy(qd_channel_t *chan);

/* Core operations */
int qd_channel_send(qd_channel_t *chan, const void *item);     /* blocks if full */
int qd_channel_recv(qd_channel_t *chan, void *item);           /* blocks if empty */
int qd_channel_try_send(qd_channel_t *chan, const void *item); /* non-blocking */
int qd_channel_try_recv(qd_channel_t *chan, void *item);       /* non-blocking */

/* For event loop integration */
int qd_channel_fd(qd_channel_t *chan);  /* Returns pollable eventfd */
int qd_channel_capacity(qd_channel_t *chan);  /* Total capacity */
int qd_channel_size(qd_channel_t *chan);       /* Current item count */

#ifdef __cplusplus
}
#endif

#endif /* QD_CHANNEL_H */
