/*
 * DHCP BDE (Broadcom Device Environment) Style Transport Layer
 *
 * Kernel module providing:
 * - mmap-based shared memory for zero-copy data transfer
 * - ioctl-based control interface for DMA info, interrupt control
 * - Signal-based and poll/wait notification for event delivery
 * - Ring buffer TX/RX queues in shared memory
 *
 * Device: /dev/qdhcp_bde
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>

#include "qd_dhcp_bde.h"
#include "../include/dhcp_kmod.h"

/*
 * Global BDE state
 */
static struct qdhcp_bde_state bde_state;

/*
 * Forward declarations for command processing
 */
static int qdhcp_bde_process_cmd(struct qdhcp_bde_client *client,
                                  struct qdhcp_bde_slot_hdr *slot);

/*
 * Allocate shared memory for a client
 */
static int qdhcp_bde_alloc_shm(struct qdhcp_bde_client *client)
{
    unsigned long shm_size = QDHCP_BDE_DEFAULT_SHM_SIZE;
    unsigned long pages = (shm_size + PAGE_SIZE - 1) / PAGE_SIZE;
    struct qdhcp_bde_shm_hdr *hdr;
    void *mem;

    /* Allocate contiguous pages */
    mem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, get_order(pages * PAGE_SIZE));
    if (!mem)
        return -ENOMEM;

    client->shm_base = mem;
    client->shm_pages = pages;
    client->shm_phys = virt_to_phys(mem);

    /* Initialize header */
    hdr = (struct qdhcp_bde_shm_hdr *)mem;
    client->shm_hdr = hdr;

    hdr->magic = QDHCP_BDE_MAGIC;
    hdr->version = QDHCP_BDE_VERSION;
    hdr->flags = QDHCP_BDE_SHM_INITIALIZED;
    hdr->shm_size = shm_size;

    /* TX ring setup */
    hdr->tx_head = 0;
    hdr->tx_tail = 0;
    hdr->tx_slot_count = QDHCP_BDE_DEFAULT_SLOT_COUNT;
    hdr->tx_slot_size = QDHCP_BDE_DEFAULT_SLOT_SIZE;
    hdr->tx_ring_offset = QDHCP_BDE_DEFAULT_TX_OFFSET;

    /* RX ring setup */
    hdr->rx_head = 0;
    hdr->rx_tail = 0;
    hdr->rx_slot_count = QDHCP_BDE_DEFAULT_SLOT_COUNT;
    hdr->rx_slot_size = QDHCP_BDE_DEFAULT_SLOT_SIZE;
    hdr->rx_ring_offset = QDHCP_BDE_DEFAULT_RX_OFFSET;

    /* Ring pointers */
    client->tx_ring = (char *)mem + hdr->tx_ring_offset;
    client->rx_ring = (char *)mem + hdr->rx_ring_offset;

    /* Interrupt state */
    hdr->intr_enable = 0;
    hdr->intr_pending = 0;
    hdr->intr_signal = SIGUSR1;
    hdr->intr_count = 0;

    /* Statistics */
    hdr->tx_count = 0;
    hdr->rx_count = 0;

    pr_debug("qdhcp_bde: allocated %lu pages at phys 0x%llx\n",
             pages, (unsigned long long)client->shm_phys);

    return 0;
}

/*
 * Free shared memory for a client
 */
static void qdhcp_bde_free_shm(struct qdhcp_bde_client *client)
{
    if (client->shm_base) {
        free_pages((unsigned long)client->shm_base,
                   get_order(client->shm_pages * PAGE_SIZE));
        client->shm_base = NULL;
        client->shm_hdr = NULL;
        client->tx_ring = NULL;
        client->rx_ring = NULL;
    }
}

/*
 * Send signal to client if configured
 */
static void qdhcp_bde_signal_client(struct qdhcp_bde_client *client)
{
    if (!client->intr_enabled)
        return;

    if (client->signal_task && client->signal_num > 0) {
        send_sig(client->signal_num, client->signal_task, 0);
    }
}

/*
 * File operations
 */

static int qdhcp_bde_open(struct inode *inode, struct file *filp)
{
    struct qdhcp_bde_client *client;
    unsigned long flags;
    int ret;

    client = kzalloc(sizeof(*client), GFP_KERNEL);
    if (!client)
        return -ENOMEM;

    spin_lock_init(&client->lock);
    init_waitqueue_head(&client->rx_wait);
    init_waitqueue_head(&client->tx_wait);

    /* Allocate shared memory */
    ret = qdhcp_bde_alloc_shm(client);
    if (ret) {
        kfree(client);
        return ret;
    }

    /* Add to clients list */
    spin_lock_irqsave(&bde_state.clients_lock, flags);
    list_add(&client->list, &bde_state.clients);
    bde_state.client_count++;
    spin_unlock_irqrestore(&bde_state.clients_lock, flags);

    filp->private_data = client;

    pr_debug("qdhcp_bde: client opened\n");
    return 0;
}

static int qdhcp_bde_release(struct inode *inode, struct file *filp)
{
    struct qdhcp_bde_client *client = filp->private_data;
    unsigned long flags;

    if (!client)
        return 0;

    /* Remove from clients list */
    spin_lock_irqsave(&bde_state.clients_lock, flags);
    list_del(&client->list);
    bde_state.client_count--;
    spin_unlock_irqrestore(&bde_state.clients_lock, flags);

    /* Free shared memory */
    qdhcp_bde_free_shm(client);

    kfree(client);

    pr_debug("qdhcp_bde: client closed\n");
    return 0;
}

static int qdhcp_bde_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct qdhcp_bde_client *client = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;
    int ret;

    if (!client || !client->shm_base)
        return -EINVAL;

    /* Verify size */
    if (size > client->shm_pages * PAGE_SIZE)
        return -EINVAL;

    /* Offset not supported */
    if (vma->vm_pgoff != 0)
        return -EINVAL;

    pfn = virt_to_phys(client->shm_base) >> PAGE_SHIFT;

    /* Map the pages to userspace */
    ret = remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
    if (ret) {
        pr_err("qdhcp_bde: remap_pfn_range failed: %d\n", ret);
        return ret;
    }

    pr_debug("qdhcp_bde: mmap'd %lu bytes at pfn %lu\n", size, pfn);
    return 0;
}

static __poll_t qdhcp_bde_poll(struct file *filp, struct poll_table_struct *wait)
{
    struct qdhcp_bde_client *client = filp->private_data;
    struct qdhcp_bde_shm_hdr *hdr;
    __poll_t mask = 0;

    if (!client || !client->shm_hdr)
        return POLLERR;

    hdr = client->shm_hdr;

    poll_wait(filp, &client->rx_wait, wait);
    poll_wait(filp, &client->tx_wait, wait);

    /* Check RX ring for data */
    smp_rmb();
    if (!QDHCP_BDE_RING_EMPTY(hdr->rx_head, hdr->rx_tail))
        mask |= POLLIN | POLLRDNORM;

    /* TX ring always writable unless full */
    if (!QDHCP_BDE_RING_FULL(hdr->tx_head, hdr->tx_tail, hdr->tx_slot_count))
        mask |= POLLOUT | POLLWRNORM;

    return mask;
}

static long qdhcp_bde_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct qdhcp_bde_client *client = filp->private_data;
    struct qdhcp_bde_shm_hdr *hdr;
    unsigned long flags;
    void __user *argp = (void __user *)arg;
    int ret = 0;

    if (!client || !client->shm_hdr)
        return -EINVAL;

    hdr = client->shm_hdr;

    switch (cmd) {
    case QDHCP_BDE_IOC_GET_DMA_INFO: {
        struct qdhcp_bde_dma_info info;

        memset(&info, 0, sizeof(info));
        info.phys_addr = client->shm_phys;
        info.size = hdr->shm_size;
        info.tx_ring_offset = hdr->tx_ring_offset;
        info.rx_ring_offset = hdr->rx_ring_offset;
        info.tx_slot_count = hdr->tx_slot_count;
        info.tx_slot_size = hdr->tx_slot_size;
        info.rx_slot_count = hdr->rx_slot_count;
        info.rx_slot_size = hdr->rx_slot_size;

        if (copy_to_user(argp, &info, sizeof(info)))
            return -EFAULT;
        break;
    }

    case QDHCP_BDE_IOC_SET_INTR: {
        struct qdhcp_bde_intr_config config;

        if (copy_from_user(&config, argp, sizeof(config)))
            return -EFAULT;

        spin_lock_irqsave(&client->lock, flags);
        client->intr_enabled = config.enable ? true : false;
        client->signal_num = config.signal;
        if (config.enable)
            client->signal_task = current;
        else
            client->signal_task = NULL;

        hdr->intr_enable = config.enable;
        hdr->intr_signal = config.signal;
        spin_unlock_irqrestore(&client->lock, flags);

        pr_debug("qdhcp_bde: intr config: enable=%d signal=%d\n",
                 config.enable, config.signal);
        break;
    }

    case QDHCP_BDE_IOC_ENABLE_INTR:
        spin_lock_irqsave(&client->lock, flags);
        client->intr_enabled = true;
        client->signal_task = current;
        hdr->intr_enable = 1;
        spin_unlock_irqrestore(&client->lock, flags);
        break;

    case QDHCP_BDE_IOC_DISABLE_INTR:
        spin_lock_irqsave(&client->lock, flags);
        client->intr_enabled = false;
        client->signal_task = NULL;
        hdr->intr_enable = 0;
        spin_unlock_irqrestore(&client->lock, flags);
        break;

    case QDHCP_BDE_IOC_WAIT: {
        struct qdhcp_bde_wait_info info;
        long timeout;

        if (copy_from_user(&info, argp, sizeof(info)))
            return -EFAULT;

        timeout = info.timeout_ms ? msecs_to_jiffies(info.timeout_ms) : MAX_SCHEDULE_TIMEOUT;
        info.result = 0;

        if (info.events & QDHCP_BDE_WAIT_RX_READY) {
            ret = wait_event_interruptible_timeout(client->rx_wait,
                !QDHCP_BDE_RING_EMPTY(hdr->rx_head, hdr->rx_tail),
                timeout);
            if (ret > 0 || !QDHCP_BDE_RING_EMPTY(hdr->rx_head, hdr->rx_tail))
                info.result |= QDHCP_BDE_WAIT_RX_READY;
        }

        if (info.events & QDHCP_BDE_WAIT_TX_DONE) {
            ret = wait_event_interruptible_timeout(client->tx_wait,
                QDHCP_BDE_RING_EMPTY(hdr->tx_head, hdr->tx_tail),
                timeout);
            if (ret > 0 || QDHCP_BDE_RING_EMPTY(hdr->tx_head, hdr->tx_tail))
                info.result |= QDHCP_BDE_WAIT_TX_DONE;
        }

        if (copy_to_user(argp, &info, sizeof(info)))
            return -EFAULT;

        ret = (info.result != 0) ? 0 : -ETIMEDOUT;
        break;
    }

    case QDHCP_BDE_IOC_SUBSCRIBE:
        spin_lock_irqsave(&client->lock, flags);
        client->subscribed = true;
        hdr->flags |= QDHCP_BDE_SHM_SUBSCRIBED;
        spin_unlock_irqrestore(&client->lock, flags);
        pr_debug("qdhcp_bde: client subscribed\n");
        break;

    case QDHCP_BDE_IOC_UNSUBSCRIBE:
        spin_lock_irqsave(&client->lock, flags);
        client->subscribed = false;
        hdr->flags &= ~QDHCP_BDE_SHM_SUBSCRIBED;
        spin_unlock_irqrestore(&client->lock, flags);
        pr_debug("qdhcp_bde: client unsubscribed\n");
        break;

    case QDHCP_BDE_IOC_TX_NOTIFY:
        /* Process TX ring */
        ret = qdhcp_bde_process_tx(client);
        break;

    case QDHCP_BDE_IOC_RX_COMPLETE:
        /* User consumed RX data - wake up any waiters */
        wake_up_interruptible(&client->tx_wait);
        break;

    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

static const struct file_operations qdhcp_bde_fops = {
    .owner          = THIS_MODULE,
    .open           = qdhcp_bde_open,
    .release        = qdhcp_bde_release,
    .mmap           = qdhcp_bde_mmap,
    .poll           = qdhcp_bde_poll,
    .unlocked_ioctl = qdhcp_bde_ioctl,
    .compat_ioctl   = qdhcp_bde_ioctl,
};

/*
 * Process TX ring - called when user signals TX_NOTIFY
 * Reads messages from TX ring and processes them
 */
int qdhcp_bde_process_tx(struct qdhcp_bde_client *client)
{
    struct qdhcp_bde_shm_hdr *hdr;
    struct qdhcp_bde_slot_hdr *slot;
    u32 tail, head;
    int count = 0;
    int ret = 0;

    if (!client || !client->shm_hdr)
        return -EINVAL;

    hdr = client->shm_hdr;

    /* Read indices with barrier */
    smp_rmb();
    head = hdr->tx_head;
    tail = hdr->tx_tail;

    while (tail != head) {
        slot = QDHCP_BDE_SLOT_PTR(client->tx_ring, tail, hdr->tx_slot_size);

        /* Validate slot */
        if (slot->magic != QDHCP_BDE_SLOT_MAGIC) {
            pr_warn("qdhcp_bde: invalid slot magic at idx %u\n", tail);
            slot->flags |= QDHCP_BDE_SLOT_ERROR;
            goto next;
        }

        if (!(slot->flags & QDHCP_BDE_SLOT_READY)) {
            goto next;
        }

        /* Process the message */
        ret = qdhcp_bde_process_cmd(client, slot);
        if (ret < 0) {
            slot->flags |= QDHCP_BDE_SLOT_ERROR;
        }

        /* Clear ready flag, mark processed */
        slot->flags &= ~QDHCP_BDE_SLOT_READY;

        client->tx_messages++;
        client->tx_bytes += slot->len;
        hdr->tx_count++;
        count++;

next:
        tail = QDHCP_BDE_NEXT_IDX(tail, hdr->tx_slot_count);
    }

    /* Update tail with barrier */
    smp_wmb();
    hdr->tx_tail = tail;

    /* Wake up TX waiters if ring is now empty */
    if (QDHCP_BDE_RING_EMPTY(hdr->tx_head, hdr->tx_tail))
        wake_up_interruptible(&client->tx_wait);

    return count;
}

/*
 * Queue RX data to subscribed clients
 */
int qdhcp_bde_queue_rx(const void *data, size_t len, u8 msg_type, u32 seq)
{
    struct qdhcp_bde_client *client;
    struct qdhcp_bde_shm_hdr *hdr;
    struct qdhcp_bde_slot_hdr *slot;
    unsigned long flags;
    u32 head, next_head;
    int queued = 0;

    if (!bde_state.registered)
        return 0;

    if (len > QDHCP_BDE_SLOT_DATA_MAX)
        return -EMSGSIZE;

    spin_lock_irqsave(&bde_state.clients_lock, flags);

    list_for_each_entry(client, &bde_state.clients, list) {
        unsigned long client_flags;

        if (!client->subscribed || !client->shm_hdr)
            continue;

        hdr = client->shm_hdr;

        spin_lock_irqsave(&client->lock, client_flags);

        /* Check if RX ring has space */
        head = hdr->rx_head;
        next_head = QDHCP_BDE_NEXT_IDX(head, hdr->rx_slot_count);

        if (next_head == hdr->rx_tail) {
            /* Ring full - drop oldest */
            hdr->rx_tail = QDHCP_BDE_NEXT_IDX(hdr->rx_tail, hdr->rx_slot_count);
        }

        /* Get slot and fill it */
        slot = QDHCP_BDE_SLOT_PTR(client->rx_ring, head, hdr->rx_slot_size);
        slot->magic = QDHCP_BDE_SLOT_MAGIC;
        slot->len = len;
        slot->flags = QDHCP_BDE_SLOT_READY;
        slot->msg_type = msg_type;
        slot->seq = seq;

        memcpy(QDHCP_BDE_SLOT_DATA(slot), data, len);

        /* Update head with barrier */
        smp_wmb();
        hdr->rx_head = next_head;
        hdr->rx_count++;

        client->rx_messages++;
        client->rx_bytes += len;

        spin_unlock_irqrestore(&client->lock, client_flags);

        /* Wake up RX waiters */
        wake_up_interruptible(&client->rx_wait);

        /* Send signal if configured */
        qdhcp_bde_signal_client(client);

        queued++;
    }

    spin_unlock_irqrestore(&bde_state.clients_lock, flags);

    return queued;
}

/*
 * Process a command from TX ring slot
 * This is called for each message written by userspace
 */
static int qdhcp_bde_process_cmd(struct qdhcp_bde_client *client,
                                  struct qdhcp_bde_slot_hdr *slot)
{
    void *data = QDHCP_BDE_SLOT_DATA(slot);
    size_t len = slot->len;

    /* For now, just log the message */
    pr_debug("qdhcp_bde: TX msg type=%u len=%u seq=%u\n",
             slot->msg_type, slot->len, slot->seq);

    /* TODO: Hook into main DHCP command processing */
    /* The data contains a qd_dhcp_msg_hdr followed by payload */
    (void)data;
    (void)len;

    return 0;
}

/*
 * Wake up clients waiting for RX
 */
void qdhcp_bde_wake_rx(void)
{
    struct qdhcp_bde_client *client;
    unsigned long flags;

    spin_lock_irqsave(&bde_state.clients_lock, flags);
    list_for_each_entry(client, &bde_state.clients, list) {
        wake_up_interruptible(&client->rx_wait);
    }
    spin_unlock_irqrestore(&bde_state.clients_lock, flags);
}

/*
 * Wake up clients waiting for TX completion
 */
void qdhcp_bde_wake_tx(void)
{
    struct qdhcp_bde_client *client;
    unsigned long flags;

    spin_lock_irqsave(&bde_state.clients_lock, flags);
    list_for_each_entry(client, &bde_state.clients, list) {
        wake_up_interruptible(&client->tx_wait);
    }
    spin_unlock_irqrestore(&bde_state.clients_lock, flags);
}

/*
 * Check if BDE is available
 */
bool qdhcp_bde_is_available(void)
{
    return bde_state.registered;
}

/*
 * Get BDE statistics
 */
void qdhcp_bde_get_stats(u64 *tx_msgs, u64 *rx_msgs, u64 *tx_bytes, u64 *rx_bytes)
{
    struct qdhcp_bde_client *client;
    unsigned long flags;
    u64 tm = 0, rm = 0, tb = 0, rb = 0;

    spin_lock_irqsave(&bde_state.clients_lock, flags);
    list_for_each_entry(client, &bde_state.clients, list) {
        tm += client->tx_messages;
        rm += client->rx_messages;
        tb += client->tx_bytes;
        rb += client->rx_bytes;
    }
    spin_unlock_irqrestore(&bde_state.clients_lock, flags);

    if (tx_msgs) *tx_msgs = tm;
    if (rx_msgs) *rx_msgs = rm;
    if (tx_bytes) *tx_bytes = tb;
    if (rx_bytes) *rx_bytes = rb;
}

/*
 * Module initialization
 */
int qdhcp_bde_init(void)
{
    int ret;

    memset(&bde_state, 0, sizeof(bde_state));
    INIT_LIST_HEAD(&bde_state.clients);
    spin_lock_init(&bde_state.clients_lock);

    bde_state.miscdev.minor = MISC_DYNAMIC_MINOR;
    bde_state.miscdev.name = QDHCP_BDE_NAME;
    bde_state.miscdev.fops = &qdhcp_bde_fops;
    bde_state.miscdev.mode = 0666;

    ret = misc_register(&bde_state.miscdev);
    if (ret < 0) {
        pr_err("qdhcp_bde: misc_register failed: %d\n", ret);
        return ret;
    }

    bde_state.registered = true;
    pr_info("qdhcp_bde: registered /dev/%s\n", QDHCP_BDE_NAME);

    return 0;
}

/*
 * Module cleanup
 */
void qdhcp_bde_exit(void)
{
    if (bde_state.registered) {
        misc_deregister(&bde_state.miscdev);
        bde_state.registered = false;
        pr_info("qdhcp_bde: unregistered\n");
    }
}
