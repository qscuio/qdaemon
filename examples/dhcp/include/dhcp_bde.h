/*
 * DHCP BDE (Broadcom Device Environment) Style Transport Layer
 *
 * Shared header for kernel and userspace BDE transport implementation.
 * Provides mmap-based shared memory for zero-copy data transfer with
 * ioctl-based control interface.
 */

#ifndef DHCP_BDE_H
#define DHCP_BDE_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <linux/types.h>
#include <linux/ioctl.h>
#include <stddef.h>
#endif

/*
 * Device names
 */
#define QDHCP_BDE_NAME          "qdhcp_bde"
#define QDHCP_BDE_PATH          "/dev/qdhcp_bde"

/*
 * Magic numbers
 */
#define QDHCP_BDE_MAGIC         0x42444553  /* "BDES" - BDE Shared memory */
#define QDHCP_BDE_SLOT_MAGIC    0x534C4F54  /* "SLOT" - Ring buffer slot */

/*
 * BDE version
 */
#define QDHCP_BDE_VERSION       1

/*
 * Ring buffer configuration defaults
 */
#define QDHCP_BDE_DEFAULT_SLOT_SIZE     4096    /* 4KB per slot */
#define QDHCP_BDE_DEFAULT_SLOT_COUNT    16      /* 16 slots per ring */
#define QDHCP_BDE_DEFAULT_RING_SIZE     (QDHCP_BDE_DEFAULT_SLOT_SIZE * QDHCP_BDE_DEFAULT_SLOT_COUNT)

/*
 * Shared memory layout:
 *   Offset 0:       struct qdhcp_bde_shm_hdr (64 bytes)
 *   Offset 64:      TX Ring (default 64KB)
 *   Offset 64K+64:  RX Ring (default 64KB)
 *
 * Total default size: 128KB + 64 bytes header
 */
#define QDHCP_BDE_SHM_HDR_SIZE          64
#define QDHCP_BDE_DEFAULT_TX_OFFSET     QDHCP_BDE_SHM_HDR_SIZE
#define QDHCP_BDE_DEFAULT_RX_OFFSET     (QDHCP_BDE_DEFAULT_TX_OFFSET + QDHCP_BDE_DEFAULT_RING_SIZE)
#define QDHCP_BDE_DEFAULT_SHM_SIZE      (QDHCP_BDE_DEFAULT_RX_OFFSET + QDHCP_BDE_DEFAULT_RING_SIZE)

/*
 * Slot flags
 */
#define QDHCP_BDE_SLOT_READY    0x01    /* Slot contains valid data */
#define QDHCP_BDE_SLOT_BUSY     0x02    /* Slot is being processed */
#define QDHCP_BDE_SLOT_ERROR    0x04    /* Error occurred processing slot */
#define QDHCP_BDE_SLOT_NOTIFY   0x08    /* Notification slot */

/*
 * Shared memory header flags
 */
#define QDHCP_BDE_SHM_INITIALIZED   0x01    /* Memory has been initialized */
#define QDHCP_BDE_SHM_CONNECTED     0x02    /* Client connected */
#define QDHCP_BDE_SHM_SUBSCRIBED    0x04    /* Client subscribed to notifications */

/*
 * Wait event flags (for QDHCP_BDE_IOC_WAIT)
 */
#define QDHCP_BDE_WAIT_RX_READY     0x01    /* Wait for RX data available */
#define QDHCP_BDE_WAIT_TX_DONE      0x02    /* Wait for TX completion */
#define QDHCP_BDE_WAIT_ANY          0x03    /* Wait for any event */

/*
 * Interrupt/signal configuration flags
 */
#define QDHCP_BDE_INTR_ENABLE       0x01    /* Interrupts enabled */
#define QDHCP_BDE_INTR_SIGNAL       0x02    /* Use signal delivery */
#define QDHCP_BDE_INTR_POLL         0x04    /* Use poll/wait queue */

/*
 * Shared Memory Header (64 bytes)
 *
 * Located at offset 0 of the mmap'd region.
 * All offsets are relative to the start of the shared memory.
 */
struct qdhcp_bde_shm_hdr {
    /* Header validation (16 bytes) */
    __u32 magic;                /* QDHCP_BDE_MAGIC */
    __u8  version;              /* QDHCP_BDE_VERSION */
    __u8  flags;                /* SHM flags */
    __u16 reserved1;
    __u32 shm_size;             /* Total shared memory size */
    __u32 reserved2;

    /* TX Ring (user -> kernel) (12 bytes) */
    __u32 tx_head;              /* Producer index (user updates) */
    __u32 tx_tail;              /* Consumer index (kernel updates) */
    __u16 tx_slot_count;        /* Number of TX slots */
    __u16 tx_slot_size;         /* Size of each TX slot */

    /* RX Ring (kernel -> user) (12 bytes) */
    __u32 rx_head;              /* Producer index (kernel updates) */
    __u32 rx_tail;              /* Consumer index (user updates) */
    __u16 rx_slot_count;        /* Number of RX slots */
    __u16 rx_slot_size;         /* Size of each RX slot */

    /* Ring offsets (8 bytes) */
    __u32 tx_ring_offset;       /* Offset to TX ring from start */
    __u32 rx_ring_offset;       /* Offset to RX ring from start */

    /* Interrupt/notification state (8 bytes) */
    __u8  intr_enable;          /* Interrupt enable flags */
    __u8  intr_pending;         /* Pending interrupt flags */
    __u16 intr_signal;          /* Signal number for notification */
    __u32 intr_count;           /* Interrupt counter */

    /* Statistics (8 bytes) */
    __u32 tx_count;             /* Total TX messages */
    __u32 rx_count;             /* Total RX messages */
} __attribute__((packed));

/*
 * Ring Buffer Slot Header
 *
 * Each slot in TX/RX ring starts with this header.
 * Data follows immediately after the header.
 */
struct qdhcp_bde_slot_hdr {
    __u32 magic;                /* QDHCP_BDE_SLOT_MAGIC */
    __u16 len;                  /* Data length (excluding header) */
    __u8  flags;                /* Slot flags */
    __u8  msg_type;             /* Message type (from qd_dhcp_msg_hdr) */
    __u32 seq;                  /* Sequence number */
    __u32 reserved;
} __attribute__((packed));

#define QDHCP_BDE_SLOT_HDR_SIZE sizeof(struct qdhcp_bde_slot_hdr)
#define QDHCP_BDE_SLOT_DATA_MAX (QDHCP_BDE_DEFAULT_SLOT_SIZE - QDHCP_BDE_SLOT_HDR_SIZE)

/*
 * IOCTL structures
 */

/*
 * DMA/shared memory information (returned by GET_DMA_INFO)
 */
struct qdhcp_bde_dma_info {
    __u64 phys_addr;            /* Physical address of shared memory */
    __u32 size;                 /* Total size of shared memory */
    __u32 tx_ring_offset;       /* Offset to TX ring */
    __u32 rx_ring_offset;       /* Offset to RX ring */
    __u16 tx_slot_count;        /* Number of TX slots */
    __u16 tx_slot_size;         /* TX slot size */
    __u16 rx_slot_count;        /* Number of RX slots */
    __u16 rx_slot_size;         /* RX slot size */
    __u32 reserved[4];
};

/*
 * Interrupt configuration (for SET_INTR)
 */
struct qdhcp_bde_intr_config {
    __u8  enable;               /* Enable interrupts */
    __u8  flags;                /* Interrupt flags (SIGNAL, POLL) */
    __u16 signal;               /* Signal number (if SIGNAL flag set) */
    __u32 reserved;
};

/*
 * Wait operation info (for WAIT ioctl)
 */
struct qdhcp_bde_wait_info {
    __u32 events;               /* Events to wait for (WAIT_RX_READY, etc) */
    __u32 timeout_ms;           /* Timeout in milliseconds (0 = infinite) */
    __u32 result;               /* Events that occurred */
    __u32 reserved;
};

/*
 * IOCTL commands
 *
 * All commands use 'B' (0x42) as the magic number for BDE.
 */
#define QDHCP_BDE_IOC_MAGIC         'B'

/* Get DMA/shared memory information */
#define QDHCP_BDE_IOC_GET_DMA_INFO  _IOR(QDHCP_BDE_IOC_MAGIC, 1, struct qdhcp_bde_dma_info)

/* Set interrupt configuration */
#define QDHCP_BDE_IOC_SET_INTR      _IOW(QDHCP_BDE_IOC_MAGIC, 2, struct qdhcp_bde_intr_config)

/* Enable interrupts */
#define QDHCP_BDE_IOC_ENABLE_INTR   _IO(QDHCP_BDE_IOC_MAGIC, 3)

/* Disable interrupts */
#define QDHCP_BDE_IOC_DISABLE_INTR  _IO(QDHCP_BDE_IOC_MAGIC, 4)

/* Wait for events */
#define QDHCP_BDE_IOC_WAIT          _IOWR(QDHCP_BDE_IOC_MAGIC, 5, struct qdhcp_bde_wait_info)

/* Subscribe to notifications */
#define QDHCP_BDE_IOC_SUBSCRIBE     _IO(QDHCP_BDE_IOC_MAGIC, 6)

/* Unsubscribe from notifications */
#define QDHCP_BDE_IOC_UNSUBSCRIBE   _IO(QDHCP_BDE_IOC_MAGIC, 7)

/* Notify kernel of TX data ready */
#define QDHCP_BDE_IOC_TX_NOTIFY     _IO(QDHCP_BDE_IOC_MAGIC, 8)

/* Notify kernel that RX data was consumed */
#define QDHCP_BDE_IOC_RX_COMPLETE   _IO(QDHCP_BDE_IOC_MAGIC, 9)

/*
 * Helper macros for ring buffer operations
 */

/* Get pointer to slot in ring buffer */
#define QDHCP_BDE_SLOT_PTR(ring_base, slot_idx, slot_size) \
    ((struct qdhcp_bde_slot_hdr *)((char *)(ring_base) + ((slot_idx) * (slot_size))))

/* Get pointer to slot data */
#define QDHCP_BDE_SLOT_DATA(slot) \
    ((void *)((char *)(slot) + QDHCP_BDE_SLOT_HDR_SIZE))

/* Calculate next index with wrap-around */
#define QDHCP_BDE_NEXT_IDX(idx, count) \
    (((idx) + 1) % (count))

/* Check if ring is empty */
#define QDHCP_BDE_RING_EMPTY(head, tail) \
    ((head) == (tail))

/* Check if ring is full */
#define QDHCP_BDE_RING_FULL(head, tail, count) \
    (QDHCP_BDE_NEXT_IDX(head, count) == (tail))

/* Calculate number of used slots */
#define QDHCP_BDE_RING_USED(head, tail, count) \
    (((head) >= (tail)) ? ((head) - (tail)) : ((count) - (tail) + (head)))

/* Calculate number of free slots */
#define QDHCP_BDE_RING_FREE(head, tail, count) \
    ((count) - 1 - QDHCP_BDE_RING_USED(head, tail, count))

/*
 * Memory barrier macros for proper ordering
 */
#ifdef __KERNEL__
#define QDHCP_BDE_WRITE_BARRIER()   smp_wmb()
#define QDHCP_BDE_READ_BARRIER()    smp_rmb()
#define QDHCP_BDE_FULL_BARRIER()    smp_mb()
#else
#define QDHCP_BDE_WRITE_BARRIER()   __sync_synchronize()
#define QDHCP_BDE_READ_BARRIER()    __sync_synchronize()
#define QDHCP_BDE_FULL_BARRIER()    __sync_synchronize()
#endif

#endif /* DHCP_BDE_H */
