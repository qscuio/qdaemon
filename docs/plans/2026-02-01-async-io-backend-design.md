# Async I/O Backend Design

## Overview

Add a separate async I/O module (`qd_aio`) supporting both io_uring and epoll backends with compile-time selection. The existing sync event loop (`qd_event`) remains unchanged.

## Requirements

- Support both io_uring (modern kernels) and epoll (older kernels)
- Compile-time backend selection via Makefile variable
- Completely separate module from existing sync event loop
- Full-featured io_uring: read/write, accept/connect, send/recv, multishot
- Consistent `qd_` prefix naming convention
- Use project's memory (`qd_memory.h`) and logging (`qd_log.h`) APIs

## Architecture

```
include/qdaemon/
├── qd_event.h          # Existing sync event loop (unchanged)
├── qd_aio.h            # Async I/O public API
├── qd_aio_ops.h        # Operation types
└── qd_aio_features.h   # Feature detection flags

src/core/
├── qd_event.c          # Existing sync (unchanged)
├── qd_aio.c            # Async loop implementation
└── backend/
    ├── qd_backend.h        # Backend vtable
    ├── qd_backend_epoll.c  # Epoll backend (proactor emulation)
    └── qd_backend_uring.c  # io_uring backend
```

## Backend VTable

```c
typedef enum {
    QD_OP_NOP = 0,
    QD_OP_READ,
    QD_OP_WRITE,
    QD_OP_POLL,
    QD_OP_ACCEPT,
    QD_OP_CONNECT,
    QD_OP_SEND,
    QD_OP_RECV,
    QD_OP_CLOSE,
    QD_OP_TIMEOUT,
    QD_OP_CANCEL,
} qd_op_type_t;

typedef struct qd_completion {
    void *user_data;
    int32_t result;
    uint32_t flags;
    qd_op_type_t op;
} qd_completion_t;

typedef struct qd_aio_req {
    qd_op_type_t op;
    int fd;
    void *buf;
    size_t len;
    uint64_t offset;
    void *user_data;
    uint32_t flags;
} qd_aio_req_t;

typedef struct qd_backend_ops {
    const char *name;
    uint32_t features;

    void *(*init)(void);
    void (*destroy)(void *ctx);
    int (*submit)(void *ctx, qd_aio_req_t *req);
    int (*submit_batch)(void *ctx, qd_aio_req_t *reqs, int count);
    int (*wait)(void *ctx, qd_completion_t *out, int max, int timeout_ms);
    int (*pending)(void *ctx);
} qd_backend_ops_t;
```

## Public API (qd_aio.h)

```c
typedef struct qd_aio_loop qd_aio_loop_t;
typedef void (*qd_aio_cb_t)(qd_completion_t *comp, void *arg);

typedef struct qd_aio_config {
    int queue_depth;
    int batch_size;
    qd_aio_cb_t callback;
    void *callback_arg;
} qd_aio_config_t;

#define QD_AIO_CONFIG_DEFAULT { .queue_depth = 256, .batch_size = 64 }

/* Loop lifecycle */
qd_aio_loop_t *qd_aio_create(void);
qd_aio_loop_t *qd_aio_create_ex(const qd_aio_config_t *config);
void qd_aio_destroy(qd_aio_loop_t *loop);

/* Run loop */
int qd_aio_run(qd_aio_loop_t *loop);
int qd_aio_run_once(qd_aio_loop_t *loop, int timeout_ms);
void qd_aio_stop(qd_aio_loop_t *loop);

/* Submit operations */
int qd_aio_read(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                uint64_t offset, void *user_data);
int qd_aio_write(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                 uint64_t offset, void *user_data);
int qd_aio_accept(qd_aio_loop_t *loop, int fd, struct sockaddr *addr,
                  socklen_t *addrlen, void *user_data);
int qd_aio_connect(qd_aio_loop_t *loop, int fd, const struct sockaddr *addr,
                   socklen_t addrlen, void *user_data);
int qd_aio_send(qd_aio_loop_t *loop, int fd, const void *buf, size_t len,
                int flags, void *user_data);
int qd_aio_recv(qd_aio_loop_t *loop, int fd, void *buf, size_t len,
                int flags, void *user_data);
int qd_aio_poll(qd_aio_loop_t *loop, int fd, uint32_t events, void *user_data);
int qd_aio_close(qd_aio_loop_t *loop, int fd, void *user_data);
int qd_aio_timeout(qd_aio_loop_t *loop, int timeout_ms, void *user_data);
int qd_aio_cancel(qd_aio_loop_t *loop, void *user_data);

/* Multishot (io_uring only) */
int qd_aio_accept_multi(qd_aio_loop_t *loop, int fd, void *user_data);
int qd_aio_recv_multi(qd_aio_loop_t *loop, int fd, void *user_data);

/* Backend info */
const char *qd_aio_backend_name(void);
uint32_t qd_aio_backend_features(void);
```

## io_uring Backend

Function naming: `qd_ur_init`, `qd_ur_destroy`, `qd_ur_submit`, `qd_ur_submit_batch`, `qd_ur_wait`, `qd_ur_pending`

Features:
- Full liburing integration
- Compile-time feature detection for multishot accept/recv
- SQPOLL support where available
- Zero-copy operations where supported

## Epoll Backend

Function naming: `qd_ep_init`, `qd_ep_destroy`, `qd_ep_submit`, `qd_ep_submit_batch`, `qd_ep_wait`, `qd_ep_pending`

Proactor emulation:
- Maintains pending operations queue
- Registers fd with epoll on submit
- Performs sync I/O when poll indicates readiness
- Returns completion with result

## Build Configuration

```makefile
BACKEND ?= epoll

ifeq ($(BACKEND), uring)
    CFLAGS += -DQD_BACKEND_URING
    LDFLAGS += -luring
    BACKEND_SRC = $(SRCDIR)/core/backend/qd_backend_uring.c
else
    CFLAGS += -DQD_BACKEND_EPOLL
    BACKEND_SRC = $(SRCDIR)/core/backend/qd_backend_epoll.c
endif

AIO_SRCS = $(SRCDIR)/core/qd_aio.c $(BACKEND_SRC)
```

## Feature Flags (qd_aio_features.h)

```c
#define QD_FEAT_ASYNC_IO          (1 << 0)
#define QD_FEAT_MULTISHOT_ACCEPT  (1 << 1)
#define QD_FEAT_MULTISHOT_RECV    (1 << 2)
#define QD_FEAT_ZERO_COPY         (1 << 3)
#define QD_FEAT_SQPOLL            (1 << 4)
```

## Error Codes

```c
#define QD_AIO_OK           0
#define QD_AIO_ERR_NOMEM   -1
#define QD_AIO_ERR_INVAL   -2
#define QD_AIO_ERR_BUSY    -3
#define QD_AIO_ERR_NOSYS   -4
```

## Files to Create/Modify

### New Files
- `include/qdaemon/qd_aio.h`
- `include/qdaemon/qd_aio_ops.h`
- `include/qdaemon/qd_aio_features.h`
- `src/core/qd_aio.c`
- `src/core/backend/qd_backend_uring.c`

### Modified Files
- `src/core/backend/qd_backend.h` - Update vtable
- `src/core/backend/qd_backend_epoll.c` - Implement proactor emulation
- `Makefile` - Add backend selection

### Unchanged Files
- `include/qdaemon/qd_event.h`
- `src/core/qd_event.c`
