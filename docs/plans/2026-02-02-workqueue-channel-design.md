# Libuv-Style Work Queue with Channel-Based Completion

**Date:** 2026-02-02
**Status:** Approved

## Overview

Add libuv-style pattern resolution for offloading blocking I/O and CPU-intensive work to background worker threads, with completion notifications delivered through the async event loop.

**Use cases:**
- Blocking file system operations
- CPU-intensive compute (hashing, compression, encryption)
- Mixed I/O and CPU workloads
- Delayed/scheduled background tasks

## Architecture

Channel-based approach for maximum flexibility and composability.

```
┌──────────────────────────────────────────────────────────────┐
│                      qd_workqueue_t                          │
│                                                              │
│  submit()──►┌─────────────┐    ┌─────────────┐               │
│             │ Timer Wheel │───►│ Threadpool  │               │
│  delayed()─►│ (delay_ms)  │    │  (workers)  │               │
│             └─────────────┘    └──────┬──────┘               │
│                    ▲                  │                      │
│                    │                  ▼                      │
│             ┌──────┴──────┐    ┌─────────────┐               │
│             │ aio_timeout │    │  Channel    │──► eventfd    │
│             │ (reschedule)│    │ (results)   │               │
│             └─────────────┘    └─────────────┘               │
└──────────────────────────────────────────────────────────────┘
                                        │
                                        ▼
                              ┌─────────────────┐
                              │  qd_aio_loop_t  │
                              │   (poll wait)   │
                              └────────┬────────┘
                                       │
                                       ▼
                              on_complete(result, status)
```

## Component 1: Channel (`qd_channel_t`)

Thread-safe MPSC (multi-producer, single-consumer) bounded queue with eventfd for poll integration.

### API

```c
typedef struct qd_channel qd_channel_t;

/* Configuration */
typedef struct qd_channel_config {
    size_t capacity;        /* Max items (0 = unbounded) */
    size_t item_size;       /* Size of each item */
    int flags;              /* QD_CHAN_* flags */
} qd_channel_config_t;

#define QD_CHAN_NONBLOCK  0x01  /* send/recv never block */
#define QD_CHAN_OVERFLOW  0x02  /* drop oldest on overflow */

/* Lifecycle */
qd_channel_t *qd_channel_create(const qd_channel_config_t *config);
void qd_channel_destroy(qd_channel_t *chan);

/* Core operations */
int qd_channel_send(qd_channel_t *chan, const void *item);     /* blocks if full */
int qd_channel_recv(qd_channel_t *chan, void *item);           /* blocks if empty */
int qd_channel_try_send(qd_channel_t *chan, const void *item); /* non-blocking */
int qd_channel_try_recv(qd_channel_t *chan, void *item);       /* non-blocking */

/* For event loop integration */
int qd_channel_fd(qd_channel_t *chan);  /* Returns pollable fd */
```

### Internal Structure

- Ring buffer with atomic head/tail indices
- eventfd signaled on each send
- Mutex for blocking operations
- Condition variables for blocking send/recv

## Component 2: Event Loop Integration

New operation type and watcher API for channels.

### API

```c
/* New operation type in qd_aio_ops.h */
QD_OP_CHANNEL = 20,  /* Channel readable */

/* Register channel with event loop */
int qd_aio_channel_watch(qd_aio_loop_t *loop, qd_channel_t *chan,
                         void *user_data);

/* Stop watching channel */
int qd_aio_channel_unwatch(qd_aio_loop_t *loop, qd_channel_t *chan);
```

### Completion Handling

```c
void my_callback(qd_completion_t *comp, void *arg) {
    if (comp->op == QD_OP_CHANNEL) {
        qd_channel_t *chan = comp->user_data;

        /* Drain all available items */
        my_result_t result;
        while (qd_channel_try_recv(chan, &result) == QD_OK) {
            handle_result(&result);
        }
    }
}
```

Uses level-triggered mode - keeps firing while channel has items.

## Component 3: Work Queue (`qd_workqueue_t`)

Ties channels and threadpool together for work submission and completion.

### API

```c
typedef struct qd_work qd_work_t;
typedef struct qd_workqueue qd_workqueue_t;

/* Work function signatures */
typedef void *(*qd_work_fn_t)(void *arg);           /* Returns result */
typedef void (*qd_after_work_fn_t)(void *result, int status, void *arg);

/* Work request */
struct qd_work {
    qd_work_fn_t work;          /* Runs in worker thread */
    void *arg;                  /* Argument to work function */
    void *result;               /* Set by work function return */
    int status;                 /* 0 = success, -errno on error */
    uint64_t delay_ms;          /* Delay before execution (0 = immediate) */
    void *user_data;            /* Passed through to completion */
};

/* Create work queue bound to loop and pool */
qd_workqueue_t *qd_workqueue_create(qd_aio_loop_t *loop,
                                     qd_threadpool_t *pool,
                                     qd_after_work_fn_t on_complete,
                                     void *complete_arg);

void qd_workqueue_destroy(qd_workqueue_t *wq);

/* Submit work */
int qd_workqueue_submit(qd_workqueue_t *wq, qd_work_fn_t work, void *arg);

/* Submit with delay */
int qd_workqueue_submit_delayed(qd_workqueue_t *wq, qd_work_fn_t work,
                                 void *arg, uint64_t delay_ms);

/* Submit full work struct for advanced use */
int qd_workqueue_submit_work(qd_workqueue_t *wq, qd_work_t *work);

/* Pending work count */
int qd_workqueue_pending(qd_workqueue_t *wq);
```

### Usage Example

```c
void *hash_file(void *arg) {
    const char *path = arg;
    return compute_sha256(path);  /* CPU intensive */
}

void on_hash_done(void *result, int status, void *arg) {
    if (status == 0)
        printf("Hash: %s\n", (char *)result);
    free(result);
}

/* Setup */
qd_workqueue_t *wq = qd_workqueue_create(loop, pool, on_hash_done, NULL);

/* Submit CPU work from event loop */
qd_workqueue_submit(wq, hash_file, "/path/to/file");

/* Or with 5 second delay */
qd_workqueue_submit_delayed(wq, cleanup_temp_files, NULL, 5000);
```

## Component 4: Timer Wheel

Hierarchical timer wheel (like Linux kernel) for O(1) delayed task scheduling.

- Resolution: 1ms granularity
- Driven by `qd_aio_timeout()` - reschedules next expiry after each batch
- Exposed via `qd_timer.h` for general use

### Work Lifecycle

1. `submit()` → immediate dispatch to threadpool
2. `submit_delayed()` → insert into timer wheel → timer fires → dispatch
3. Worker executes `work_fn()`, captures return value
4. Worker pushes `qd_work_t` to channel
5. Channel signals eventfd
6. Event loop wakes, calls `on_complete()`

## Error Handling & Cancellation

### Status Codes

```c
#define QD_WORK_OK        0      /* Success */
#define QD_WORK_CANCELLED -1     /* Cancelled before execution */
#define QD_WORK_TIMEOUT   -2     /* Work function took too long (optional) */
#define QD_WORK_ERROR     -3     /* Work function returned NULL with errno set */
```

### Cancellation API

```c
typedef struct qd_work_handle qd_work_handle_t;

/* Submit and get handle for cancellation */
qd_work_handle_t *qd_workqueue_submit_cancellable(qd_workqueue_t *wq,
                                                   qd_work_fn_t work,
                                                   void *arg);

/* Cancel pending work (no-op if already running/completed) */
int qd_workqueue_cancel(qd_work_handle_t *handle);

/* Release handle (must call even after cancel) */
void qd_work_handle_release(qd_work_handle_t *handle);
```

### Cancellation Semantics

- If work is still in timer wheel → removed, `on_complete` called with `QD_WORK_CANCELLED`
- If work is queued in threadpool → marked cancelled, skipped when dequeued
- If work is already running → cannot cancel, runs to completion
- `on_complete` is **always** called exactly once (success, error, or cancelled)

### Error Example

```c
void *risky_work(void *arg) {
    int fd = open(arg, O_RDONLY);
    if (fd < 0)
        return NULL;  /* status = QD_WORK_ERROR, errno preserved */
    /* ... */
    return result;
}

void on_complete(void *result, int status, void *arg) {
    if (status == QD_WORK_ERROR) {
        qd_log_perror("Work failed");
    } else if (status == QD_WORK_CANCELLED) {
        qd_log_warn("Work was cancelled");
    }
}
```

## File Structure

### New Files

```
include/qdaemon/
├── qd_channel.h          # Channel API
└── qd_workqueue.h        # Work queue API

src/core/
├── qd_channel.c          # Channel implementation (ring buffer + eventfd)
├── qd_workqueue.c        # Work queue implementation
└── qd_timer.c            # Hierarchical timer wheel for delayed work

tests/
├── test_channel.c        # Channel unit tests
└── test_workqueue.c      # Work queue integration tests
```

### Modifications to Existing Files

- `include/qdaemon/qd_aio_ops.h` → add `QD_OP_CHANNEL`
- `src/core/qd_aio.c` → add `qd_aio_channel_watch()` and `qd_aio_channel_unwatch()`

## Dependencies

| Component | Depends On |
|-----------|------------|
| `qd_channel` | `qd_memory`, `qd_log` |
| `qd_workqueue` | `qd_channel`, `qd_threadpool`, `qd_aio`, `qd_log` |
| `qd_timer` | `qd_memory` (internal) |

## Implementation Order

1. `qd_channel` - standalone, can be tested independently
2. `qd_aio` integration - add `QD_OP_CHANNEL` and watch/unwatch
3. `qd_timer` - internal component
4. `qd_workqueue` - ties everything together
5. Tests for each component

## Limitations

- **Backend Support**: `qd_workqueue_submit_delayed()` relies on `QD_OP_TIMEOUT`. If the underlying AIO backend (e.g., current epoll implementation) does not support this operation, delayed submission will fail with `QD_ERR_NOSYS`.
- **Cancellation**: Running work cannot be interrupted; cancellation only affects pending work.

## Performance Considerations

- **Lock Contention**: The channel uses a mutex for blocking operations but uses atomic indices for the ring buffer state. `try_send` and `try_recv` minimize locking overhead.
- **Memory Allocation**: Work items are allocated on the heap. For high-throughput scenarios, a slab allocator or object pool for `qd_work_t` would reduce allocator pressure.
- **Event Loop Overhead**: Each batch of completions signals the eventfd once. The `drain_channel_completions` function processes all available items in a single event loop iteration to amortize the wake-up cost.
- **False Sharing**: `qd_channel_t` structure layout should ensure that producer (`tail`) and consumer (`head`) indices are on separate cache lines to prevent false sharing in high-contention scenarios.

## Future Improvements

- **Lock-free Channel**: Replace the mutex-protected ring buffer with a fully lock-free MPSC queue (e.g., Vyukov's bounded MPSC) for lower latency.
- **Work Object Pooling**: Add a built-in object pool for `qd_work_t` structures to minimize `malloc`/`free` overhead.
- **Priority Support**: Add support for high-priority work items that bypass the normal queue.
- **Affinity Control**: Allow pinning worker threads to specific CPU cores and submitting work to specific threads.

## Verification

The implementation should be verified with:
1.  **Unit Tests**: Isolated tests for `qd_channel` (overflow, blocking, non-blocking) and `qd_timer_wheel` (precision, ordering).
2.  **Integration Tests**: Verify `qd_workqueue` correctly offloads work and triggers completion callbacks on the event loop thread.
3.  **Stress Tests**: High-concurrency producer/consumer tests to catch race conditions in the channel or cancellation logic.
4.  **Leak Detection**: Run tests under Valgrind/ASan to ensure no memory leaks in the work lifecycle, especially during cancellation.
