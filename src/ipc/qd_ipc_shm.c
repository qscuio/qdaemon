/*
 * QDaemon - Shared Memory Ring Buffer Implementation
 * High-throughput inter-process communication
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#include "qdaemon/qd_ipc.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

#define QD_SHM_MAGIC    0x51445348  /* "QDSH" */
#define QD_SHM_VERSION  1

/* Futex-based waiting */
static int futex_wait(volatile int *addr, int val, const struct timespec *timeout)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, timeout, NULL, 0);
}

static int futex_wake(volatile int *addr, int count)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, count, NULL, NULL, 0);
}

qd_shm_ring_t *qd_shm_ring_create(const char *name, size_t size)
{
    if (!name || size == 0)
        return NULL;

    /* Round up size to page boundary */
    long page_size = sysconf(_SC_PAGESIZE);
    size = QD_ALIGN(size, page_size);

    /* Total size includes header */
    size_t total_size = sizeof(qd_shm_ring_header_t) + size;
    total_size = QD_ALIGN(total_size, page_size);

    qd_shm_ring_t *ring = calloc(1, sizeof(qd_shm_ring_t));
    if (!ring)
        return NULL;

    strncpy(ring->name, name, sizeof(ring->name) - 1);
    ring->is_creator = 1;

    /* Create shared memory object */
    char shm_name[128];
    snprintf(shm_name, sizeof(shm_name), "/qd_%s", name);

    ring->fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (ring->fd == -1) {
        if (errno == EEXIST) {
            /* Already exists, unlink and retry */
            shm_unlink(shm_name);
            ring->fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
        }
        if (ring->fd == -1) {
            free(ring);
            return NULL;
        }
    }

    /* Set size */
    if (ftruncate(ring->fd, total_size) == -1) {
        close(ring->fd);
        shm_unlink(shm_name);
        free(ring);
        return NULL;
    }

    /* Map memory */
    ring->addr = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
    if (ring->addr == MAP_FAILED) {
        close(ring->fd);
        shm_unlink(shm_name);
        free(ring);
        return NULL;
    }

    ring->total_size = total_size;
    ring->header = ring->addr;
    ring->data = (uint8_t *)ring->addr + sizeof(qd_shm_ring_header_t);
    ring->data_size = size;

    /* Initialize header */
    ring->header->magic = QD_SHM_MAGIC;
    ring->header->version = QD_SHM_VERSION;
    ring->header->size = size;
    qd_atomic_init(&ring->header->write_pos, 0);
    qd_atomic_init(&ring->header->read_pos, 0);
    qd_atomic_init(&ring->header->writer_waiting, 0);
    qd_atomic_init(&ring->header->reader_waiting, 0);

    return ring;
}

qd_shm_ring_t *qd_shm_ring_attach(const char *name)
{
    if (!name)
        return NULL;

    qd_shm_ring_t *ring = calloc(1, sizeof(qd_shm_ring_t));
    if (!ring)
        return NULL;

    strncpy(ring->name, name, sizeof(ring->name) - 1);
    ring->is_creator = 0;

    /* Open shared memory object */
    char shm_name[128];
    snprintf(shm_name, sizeof(shm_name), "/qd_%s", name);

    ring->fd = shm_open(shm_name, O_RDWR, 0);
    if (ring->fd == -1) {
        free(ring);
        return NULL;
    }

    /* Get size */
    struct stat st;
    if (fstat(ring->fd, &st) == -1) {
        close(ring->fd);
        free(ring);
        return NULL;
    }

    ring->total_size = st.st_size;

    /* Map memory */
    ring->addr = mmap(NULL, ring->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
    if (ring->addr == MAP_FAILED) {
        close(ring->fd);
        free(ring);
        return NULL;
    }

    ring->header = ring->addr;
    ring->data = (uint8_t *)ring->addr + sizeof(qd_shm_ring_header_t);
    ring->data_size = ring->header->size;

    /* Validate header */
    if (ring->header->magic != QD_SHM_MAGIC ||
        ring->header->version != QD_SHM_VERSION) {
        munmap(ring->addr, ring->total_size);
        close(ring->fd);
        free(ring);
        return NULL;
    }

    return ring;
}

void qd_shm_ring_destroy(qd_shm_ring_t *ring)
{
    if (!ring)
        return;

    if (ring->addr) {
        munmap(ring->addr, ring->total_size);
    }

    if (ring->fd >= 0) {
        close(ring->fd);
    }

    /* Unlink if we created it */
    if (ring->is_creator) {
        char shm_name[128];
        snprintf(shm_name, sizeof(shm_name), "/qd_%s", ring->name);
        shm_unlink(shm_name);
    }

    free(ring);
}

size_t qd_shm_ring_write_available(qd_shm_ring_t *ring)
{
    if (!ring)
        return 0;

    size_t write_pos = qd_atomic_load(&ring->header->write_pos);
    size_t read_pos = qd_atomic_load(&ring->header->read_pos);

    if (write_pos >= read_pos) {
        return ring->data_size - (write_pos - read_pos) - 1;
    } else {
        return read_pos - write_pos - 1;
    }
}

size_t qd_shm_ring_read_available(qd_shm_ring_t *ring)
{
    if (!ring)
        return 0;

    size_t write_pos = qd_atomic_load(&ring->header->write_pos);
    size_t read_pos = qd_atomic_load(&ring->header->read_pos);

    if (write_pos >= read_pos) {
        return write_pos - read_pos;
    } else {
        return ring->data_size - read_pos + write_pos;
    }
}

int qd_shm_ring_is_empty(qd_shm_ring_t *ring)
{
    if (!ring)
        return 1;
    return qd_atomic_load(&ring->header->write_pos) == qd_atomic_load(&ring->header->read_pos);
}

int qd_shm_ring_is_full(qd_shm_ring_t *ring)
{
    return qd_shm_ring_write_available(ring) == 0;
}

int qd_shm_ring_write(qd_shm_ring_t *ring, const void *data, size_t len)
{
    return qd_shm_ring_write_timeout(ring, data, len, -1);
}

int qd_shm_ring_write_timeout(qd_shm_ring_t *ring, const void *data,
                               size_t len, int timeout_ms)
{
    if (!ring || !data || len == 0)
        return QD_ERR_INVAL;

    if (len > ring->data_size - 1)
        return QD_ERR_INVAL;

    const uint8_t *src = data;
    qd_time_t start = qd_time_now();

    while (1) {
        size_t available = qd_shm_ring_write_available(ring);

        if (available >= len) {
            /* We have enough space */
            size_t write_pos = qd_atomic_load(&ring->header->write_pos);

            /* Write data (may wrap around) */
            size_t first_part = ring->data_size - write_pos;
            if (first_part >= len) {
                memcpy(ring->data + write_pos, src, len);
            } else {
                memcpy(ring->data + write_pos, src, first_part);
                memcpy(ring->data, src + first_part, len - first_part);
            }

            /* Update write position */
            qd_memory_barrier();
            size_t new_pos = (write_pos + len) % ring->data_size;
            qd_atomic_store(&ring->header->write_pos, new_pos);

            /* Wake reader if waiting */
            if (qd_atomic_load(&ring->header->reader_waiting)) {
                futex_wake((int *)&ring->header->reader_waiting, 1);
            }

            return QD_OK;
        }

        /* Check timeout */
        if (timeout_ms == 0)
            return QD_ERR_AGAIN;

        if (timeout_ms > 0) {
            qd_time_t elapsed = qd_time_now() - start;
            if (elapsed >= (qd_time_t)timeout_ms)
                return QD_ERR_TIMEOUT;
        }

        /* Wait for space */
        qd_atomic_store(&ring->header->writer_waiting, 1);

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1000000  /* 1ms */
        };
        futex_wait((int *)&ring->header->writer_waiting, 1, &ts);

        qd_atomic_store(&ring->header->writer_waiting, 0);
    }
}

int qd_shm_ring_read(qd_shm_ring_t *ring, void *data, size_t len)
{
    return qd_shm_ring_read_timeout(ring, data, len, -1);
}

int qd_shm_ring_read_timeout(qd_shm_ring_t *ring, void *data,
                              size_t len, int timeout_ms)
{
    if (!ring || !data || len == 0)
        return QD_ERR_INVAL;

    uint8_t *dst = data;
    qd_time_t start = qd_time_now();

    while (1) {
        size_t available = qd_shm_ring_read_available(ring);

        if (available >= len) {
            /* We have enough data */
            size_t read_pos = qd_atomic_load(&ring->header->read_pos);

            /* Read data (may wrap around) */
            size_t first_part = ring->data_size - read_pos;
            if (first_part >= len) {
                memcpy(dst, ring->data + read_pos, len);
            } else {
                memcpy(dst, ring->data + read_pos, first_part);
                memcpy(dst + first_part, ring->data, len - first_part);
            }

            /* Update read position */
            qd_memory_barrier();
            size_t new_pos = (read_pos + len) % ring->data_size;
            qd_atomic_store(&ring->header->read_pos, new_pos);

            /* Wake writer if waiting */
            if (qd_atomic_load(&ring->header->writer_waiting)) {
                futex_wake((int *)&ring->header->writer_waiting, 1);
            }

            return QD_OK;
        }

        /* Check timeout */
        if (timeout_ms == 0)
            return QD_ERR_AGAIN;

        if (timeout_ms > 0) {
            qd_time_t elapsed = qd_time_now() - start;
            if (elapsed >= (qd_time_t)timeout_ms)
                return QD_ERR_TIMEOUT;
        }

        /* Wait for data */
        qd_atomic_store(&ring->header->reader_waiting, 1);

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1000000  /* 1ms */
        };
        futex_wait((int *)&ring->header->reader_waiting, 1, &ts);

        qd_atomic_store(&ring->header->reader_waiting, 0);
    }
}
