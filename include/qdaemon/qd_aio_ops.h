/*
 * QDaemon - Async I/O Operation Types
 * Defines operation types and request/completion structures
 */

#ifndef QD_AIO_OPS_H
#define QD_AIO_OPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Operation types */
typedef enum qd_op_type {
    QD_OP_NOP = 0,       /* No operation */
    QD_OP_READ,          /* Read from fd */
    QD_OP_WRITE,         /* Write to fd */
    QD_OP_POLL,          /* Poll for events */
    QD_OP_ACCEPT,        /* Accept connection */
    QD_OP_CONNECT,       /* Connect to remote */
    QD_OP_SEND,          /* Send data */
    QD_OP_RECV,          /* Receive data */
    QD_OP_CLOSE,         /* Close fd */
    QD_OP_TIMEOUT,       /* Timeout event */
    QD_OP_CANCEL,        /* Cancel operation */
    QD_OP_MAX
} qd_op_type_t;

/* Operation flags */
#define QD_AIO_FLAG_NONE       0
#define QD_AIO_FLAG_MULTISHOT  (1 << 0)  /* Multishot operation */
#define QD_AIO_FLAG_FIXED_BUF  (1 << 1)  /* Use fixed buffer */
#define QD_AIO_FLAG_LINKED     (1 << 2)  /* Linked operation */

/* Async I/O request */
typedef struct qd_aio_req {
    qd_op_type_t op;          /* Operation type */
    int fd;                   /* File descriptor */
    void *buf;                /* Buffer for read/write */
    size_t len;               /* Buffer length */
    uint64_t offset;          /* File offset (for pread/pwrite) */
    void *user_data;          /* User context pointer */
    uint32_t flags;           /* Operation flags */
    int aux;                  /* Auxiliary data (e.g., msg_flags for send/recv) */
} qd_aio_req_t;

/* Completion result */
typedef struct qd_completion {
    void *user_data;          /* User context from request */
    int32_t result;           /* Result: bytes transferred or -errno */
    uint32_t flags;           /* Completion flags */
    qd_op_type_t op;          /* Which operation completed */
} qd_completion_t;

/* Completion flags */
#define QD_CQE_FLAG_MORE       (1 << 0)  /* More completions coming (multishot) */
#define QD_CQE_FLAG_NOTIF      (1 << 1)  /* Notification only */

/* Helper to get operation name */
static inline const char *qd_op_type_name(qd_op_type_t op)
{
    static const char *names[] = {
        [QD_OP_NOP]     = "nop",
        [QD_OP_READ]    = "read",
        [QD_OP_WRITE]   = "write",
        [QD_OP_POLL]    = "poll",
        [QD_OP_ACCEPT]  = "accept",
        [QD_OP_CONNECT] = "connect",
        [QD_OP_SEND]    = "send",
        [QD_OP_RECV]    = "recv",
        [QD_OP_CLOSE]   = "close",
        [QD_OP_TIMEOUT] = "timeout",
        [QD_OP_CANCEL]  = "cancel",
    };
    return (op < QD_OP_MAX) ? names[op] : "unknown";
}

#ifdef __cplusplus
}
#endif

#endif /* QD_AIO_OPS_H */
