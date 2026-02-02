/*
 * QDaemon - Async File I/O
 * Thread-pool based asynchronous file operations
 */

#ifndef QD_FS_H
#define QD_FS_H

#include <sys/types.h>
#include "qdaemon/qd_event.h"

typedef struct qd_fs_req qd_fs_req_t;

/* Callback for file operations */
/* result is bytes read/written or -errno on failure */
typedef void (*qd_fs_cb_t)(qd_fs_req_t *req, ssize_t result);

struct qd_fs_req {
    qd_event_loop_t *loop;
    int fd;
    void *buf;
    size_t size;
    off_t offset;
    qd_fs_cb_t cb;
    void *data;      /* User data */
    ssize_t result;  /* Operation result */
    int error;       /* Saved errno */
    void *internal;  /* Private pointer for cleanup */
};

/* 
 * Async Read
 * Reads 'size' bytes from 'fd' at 'offset' into 'buf'.
 * callback is invoked on the event loop thread when done.
 */
int qd_fs_read(qd_event_loop_t *loop, int fd, void *buf, size_t size, off_t offset, 
               qd_fs_cb_t cb, void *data);

/* 
 * Async Write
 * Writes 'size' bytes from 'buf' to 'fd' at 'offset'.
 */
int qd_fs_write(qd_event_loop_t *loop, int fd, const void *buf, size_t size, off_t offset,
                qd_fs_cb_t cb, void *data);

/* Close async request (cleanup before callback if needed) */
void qd_fs_req_cancel(qd_fs_req_t *req);

/* Cleanup global fs thread pool resources */
void qd_fs_cleanup(void);

#endif /* QD_FS_H */
