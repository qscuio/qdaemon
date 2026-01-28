/*
 * DHCP BDE Kernel Module - Internal Header
 *
 * Kernel-internal declarations for the BDE transport layer.
 */

#ifndef QD_DHCP_BDE_H
#define QD_DHCP_BDE_H

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/list.h>

#include "../include/dhcp_bde.h"

/*
 * Per-client BDE state
 */
struct qdhcp_bde_client {
    struct list_head        list;           /* In global clients list */
    spinlock_t              lock;           /* Client state lock */

    /* Shared memory */
    void                   *shm_base;       /* Kernel virtual address */
    unsigned long           shm_pages;      /* Number of pages */
    phys_addr_t             shm_phys;       /* Physical address */
    struct qdhcp_bde_shm_hdr *shm_hdr;      /* Header pointer */

    /* Ring buffer pointers */
    void                   *tx_ring;        /* TX ring base */
    void                   *rx_ring;        /* RX ring base */

    /* Wait queues */
    wait_queue_head_t       rx_wait;        /* Wait for RX data */
    wait_queue_head_t       tx_wait;        /* Wait for TX completion */

    /* Signal delivery */
    struct task_struct     *signal_task;    /* Task to signal */
    int                     signal_num;     /* Signal number */

    /* State flags */
    bool                    subscribed;     /* Receiving notifications */
    bool                    intr_enabled;   /* Interrupts enabled */

    /* Statistics */
    u64                     tx_messages;
    u64                     rx_messages;
    u64                     tx_bytes;
    u64                     rx_bytes;
};

/*
 * BDE module global state
 */
struct qdhcp_bde_state {
    struct miscdevice       miscdev;        /* Character device */
    bool                    registered;     /* Device registered */

    /* Client management */
    struct list_head        clients;        /* List of clients */
    spinlock_t              clients_lock;   /* Clients list lock */
    int                     client_count;   /* Number of clients */

    /* Module reference */
    struct module          *owner;
};

/*
 * BDE module initialization/cleanup functions
 * Called from main kernel module (qd_dhcp_kmod.c)
 */
int qdhcp_bde_init(void);
void qdhcp_bde_exit(void);

/*
 * Queue a notification message to all subscribed BDE clients
 * Called when kernel needs to send data to userspace
 */
int qdhcp_bde_queue_rx(const void *data, size_t len, u8 msg_type, u32 seq);

/*
 * Process TX messages from a client
 * Called when userspace signals TX_NOTIFY
 */
int qdhcp_bde_process_tx(struct qdhcp_bde_client *client);

/*
 * Wake up clients waiting for events
 */
void qdhcp_bde_wake_rx(void);
void qdhcp_bde_wake_tx(void);

/*
 * Check if BDE transport is available
 */
bool qdhcp_bde_is_available(void);

/*
 * Get BDE statistics
 */
void qdhcp_bde_get_stats(u64 *tx_msgs, u64 *rx_msgs, u64 *tx_bytes, u64 *rx_bytes);

#endif /* QD_DHCP_BDE_H */
