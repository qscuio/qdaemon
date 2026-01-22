/*
 * QDaemon Custom Address Family (AF_QDMETA)
 * Kernel module providing custom socket address family for:
 * - Control messages with meta-data
 * - Data/packet transmission with meta-data
 * - SCM ancillary messages for timestamps, CPU info, etc.
 * 
 * NO netdev - all communication via custom socket family
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <net/sock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QDaemon");
MODULE_DESCRIPTION("Custom Socket Address Family with Meta-data Support");
MODULE_VERSION("1.0");

/* Address family number */
#define AF_QDMETA           45
#define PF_QDMETA           AF_QDMETA

/* Socket protocol types */
#define QDMETA_PROTO_CTRL   1       /* Control protocol */
#define QDMETA_PROTO_DATA   2       /* Data protocol */
#define QDMETA_PROTO_RAW    3       /* Raw protocol */

/* SCM (Socket Control Message) types for meta-data */
#define SCM_QDMETA_BASE         0x1000
#define SCM_QDMETA_TIMESTAMP    (SCM_QDMETA_BASE + 1)
#define SCM_QDMETA_CPU          (SCM_QDMETA_BASE + 2)
#define SCM_QDMETA_CREDS        (SCM_QDMETA_BASE + 3)
#define SCM_QDMETA_SEQNUM       (SCM_QDMETA_BASE + 4)
#define SCM_QDMETA_PKTTYPE      (SCM_QDMETA_BASE + 5)
#define SCM_QDMETA_CUSTOM       (SCM_QDMETA_BASE + 6)

/* Socket address structure */
struct sockaddr_qdmeta {
    __kernel_sa_family_t    sqm_family;     /* AF_QDMETA */
    __u16                   sqm_port;       /* Port number */
    __u32                   sqm_addr;       /* Logical address */
    __u16                   sqm_protocol;   /* Protocol type */
    __u8                    sqm_reserved[6];
};

/* Meta-data structures */
struct qdmeta_timestamp {
    __u64   sec;
    __u64   nsec;
    __u32   clock_id;       /* CLOCK_REALTIME, CLOCK_MONOTONIC, etc */
    __u32   flags;
};

struct qdmeta_cpu {
    __u32   cpu_id;
    __u32   numa_node;
};

struct qdmeta_creds {
    __u32   pid;
    __u32   uid;
    __u32   gid;
    __u32   flags;
};

struct qdmeta_seqnum {
    __u64   seq;
    __u32   flags;
    __u32   reserved;
};

struct qdmeta_pkttype {
    __u16   type;           /* Packet/message type */
    __u16   subtype;
    __u32   flags;
};

/* Per-socket private data */
struct qdmeta_sock {
    struct sock sk;
    struct list_head list;
    
    u16 protocol;               /* Protocol type */
    u16 port;                   /* Bound port */
    u32 addr;                   /* Logical address */
    
    /* Connected peer (for SOCK_STREAM/SOCK_SEQPACKET) */
    u16 peer_port;
    u32 peer_addr;
    bool connected;
    
    /* Pending connections (for listening sockets) */
    struct list_head accept_queue;
    int accept_queue_len;
    int max_accept_queue;
    
    /* RX queue */
    struct sk_buff_head rx_queue;
    wait_queue_head_t rx_wait;
    
    /* Sequence number for meta-data */
    u64 tx_seq;
    u64 rx_seq;
    
    /* Statistics */
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_bytes;
    u64 tx_bytes;
    
    /* Configuration */
    u32 flags;
    bool bound;
};

/* Port allocation */
#define QDMETA_PORT_MIN     1024
#define QDMETA_PORT_MAX     65535
static DEFINE_SPINLOCK(qdmeta_port_lock);
static u16 qdmeta_next_port = QDMETA_PORT_MIN;

/* Socket registry (for routing) */
static LIST_HEAD(qdmeta_sockets);
static DEFINE_SPINLOCK(qdmeta_sockets_lock);

/* Protocol and family structures */
static struct proto qdmeta_proto;
static const struct proto_ops qdmeta_dgram_ops;
static const struct proto_ops qdmeta_stream_ops;
static struct net_proto_family qdmeta_family;

/* Forward declarations */
static int qdmeta_create(struct net *net, struct socket *sock, 
                         int protocol, int kern);

/*
 * Helper functions
 */

static inline struct qdmeta_sock *qdmeta_sk(const struct sock *sk)
{
    return (struct qdmeta_sock *)sk;
}

/* Allocate ephemeral port */
static u16 qdmeta_alloc_port(void)
{
    u16 port;
    
    spin_lock(&qdmeta_port_lock);
    port = qdmeta_next_port++;
    if (qdmeta_next_port > QDMETA_PORT_MAX)
        qdmeta_next_port = QDMETA_PORT_MIN;
    spin_unlock(&qdmeta_port_lock);
    
    return port;
}

/* Find socket by port */
static struct qdmeta_sock *qdmeta_find_socket(u16 port, u32 addr)
{
    struct qdmeta_sock *qsk;
    
    spin_lock(&qdmeta_sockets_lock);
    list_for_each_entry(qsk, &qdmeta_sockets, list) {
        if (qsk->port == port && (qsk->addr == addr || qsk->addr == 0)) {
            spin_unlock(&qdmeta_sockets_lock);
            return qsk;
        }
    }
    spin_unlock(&qdmeta_sockets_lock);
    
    return NULL;
}

/* Deliver packet to socket */
static int qdmeta_deliver(struct qdmeta_sock *qsk, struct sk_buff *skb)
{
    if (skb_queue_len(&qsk->rx_queue) > 1024) {
        /* Queue full */
        return -ENOBUFS;
    }
    
    skb_queue_tail(&qsk->rx_queue, skb);
    qsk->rx_packets++;
    qsk->rx_bytes += skb->len;
    wake_up_interruptible(&qsk->rx_wait);
    
    return 0;
}

/*
 * Socket operations
 */

static int qdmeta_release(struct socket *sock)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk;

    if (!sk)
        return 0;

    qsk = qdmeta_sk(sk);

    /* Remove from global list */
    spin_lock(&qdmeta_sockets_lock);
    list_del(&qsk->list);
    spin_unlock(&qdmeta_sockets_lock);

    /* Free RX queue */
    skb_queue_purge(&qsk->rx_queue);

    sock_orphan(sk);
    sock->sk = NULL;
    sock_put(sk);

    return 0;
}

static int qdmeta_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    struct sockaddr_qdmeta *sqm = (struct sockaddr_qdmeta *)addr;

    if (addr_len < sizeof(*sqm))
        return -EINVAL;

    if (sqm->sqm_family != AF_QDMETA)
        return -EINVAL;

    lock_sock(sk);
    
    /* Check if port is already in use */
    if (sqm->sqm_port != 0) {
        struct qdmeta_sock *existing = qdmeta_find_socket(sqm->sqm_port, sqm->sqm_addr);
        if (existing && existing != qsk) {
            release_sock(sk);
            return -EADDRINUSE;
        }
        qsk->port = sqm->sqm_port;
    } else {
        qsk->port = qdmeta_alloc_port();
    }
    
    qsk->addr = sqm->sqm_addr;
    qsk->protocol = sqm->sqm_protocol;
    qsk->bound = true;
    
    release_sock(sk);
    return 0;
}

static int qdmeta_connect(struct socket *sock, struct sockaddr *addr,
                          int addr_len, int flags)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    struct sockaddr_qdmeta *sqm = (struct sockaddr_qdmeta *)addr;

    if (addr_len < sizeof(*sqm))
        return -EINVAL;

    if (sqm->sqm_family != AF_QDMETA)
        return -EINVAL;

    lock_sock(sk);
    
    /* Auto-bind if not bound */
    if (!qsk->bound) {
        qsk->port = qdmeta_alloc_port();
        qsk->bound = true;
    }
    
    qsk->peer_port = sqm->sqm_port;
    qsk->peer_addr = sqm->sqm_addr;
    qsk->connected = true;
    
    release_sock(sk);
    return 0;
}

static int qdmeta_getname(struct socket *sock, struct sockaddr *addr, int peer)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    struct sockaddr_qdmeta *sqm = (struct sockaddr_qdmeta *)addr;

    if (peer) {
        if (!qsk->connected)
            return -ENOTCONN;
        sqm->sqm_port = qsk->peer_port;
        sqm->sqm_addr = qsk->peer_addr;
    } else {
        sqm->sqm_port = qsk->port;
        sqm->sqm_addr = qsk->addr;
    }
    
    sqm->sqm_family = AF_QDMETA;
    sqm->sqm_protocol = qsk->protocol;

    return sizeof(*sqm);
}

static __poll_t qdmeta_poll(struct file *file, struct socket *sock, poll_table *wait)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    __poll_t mask = 0;

    poll_wait(file, &qsk->rx_wait, wait);

    if (!skb_queue_empty(&qsk->rx_queue))
        mask |= EPOLLIN | EPOLLRDNORM;

    mask |= EPOLLOUT | EPOLLWRNORM;

    return mask;
}

/* Build control message with meta-data */
static void qdmeta_build_cmsg(struct msghdr *msg, struct sk_buff *skb, 
                               struct qdmeta_sock *qsk)
{
    struct qdmeta_timestamp ts;
    struct qdmeta_cpu cpu;
    struct qdmeta_seqnum seq;

    /* Timestamp */
    if (skb->tstamp) {
        struct timespec64 kts = ktime_to_timespec64(skb->tstamp);
        ts.sec = kts.tv_sec;
        ts.nsec = kts.tv_nsec;
        ts.clock_id = CLOCK_REALTIME;
        ts.flags = 0;
        put_cmsg(msg, SOL_SOCKET, SCM_QDMETA_TIMESTAMP, sizeof(ts), &ts);
    }

    /* CPU info */
    cpu.cpu_id = raw_smp_processor_id();
    cpu.numa_node = cpu_to_node(cpu.cpu_id);
    put_cmsg(msg, SOL_SOCKET, SCM_QDMETA_CPU, sizeof(cpu), &cpu);

    /* Sequence number */
    seq.seq = qsk->rx_seq++;
    seq.flags = 0;
    seq.reserved = 0;
    put_cmsg(msg, SOL_SOCKET, SCM_QDMETA_SEQNUM, sizeof(seq), &seq);
}

static int qdmeta_recvmsg(struct socket *sock, struct msghdr *msg, 
                          size_t len, int flags)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    struct sk_buff *skb;
    int err = 0;
    size_t copied;

    if (flags & MSG_OOB)
        return -EOPNOTSUPP;

    lock_sock(sk);

    /* Wait for data */
    while (skb_queue_empty(&qsk->rx_queue)) {
        if (flags & MSG_DONTWAIT) {
            err = -EAGAIN;
            goto out;
        }

        release_sock(sk);
        err = wait_event_interruptible(qsk->rx_wait,
                                       !skb_queue_empty(&qsk->rx_queue));
        lock_sock(sk);
        if (err)
            goto out;
    }

    skb = skb_dequeue(&qsk->rx_queue);
    if (!skb) {
        err = -EAGAIN;
        goto out;
    }

    copied = min(len, (size_t)skb->len);
    err = skb_copy_datagram_msg(skb, 0, msg, copied);
    if (err)
        goto out_free;

    /* Build control message with meta-data */
    qdmeta_build_cmsg(msg, skb, qsk);

    /* Set source address in msg_name */
    if (msg->msg_name) {
        struct sockaddr_qdmeta *sqm = msg->msg_name;
        /* Source info would be stored in skb->cb */
        sqm->sqm_family = AF_QDMETA;
        sqm->sqm_port = 0;  /* TODO: extract from skb */
        sqm->sqm_addr = 0;
        msg->msg_namelen = sizeof(*sqm);
    }

    if (flags & MSG_TRUNC)
        copied = skb->len;

    err = copied;

out_free:
    kfree_skb(skb);
out:
    release_sock(sk);
    return err;
}

static int qdmeta_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
    struct sock *sk = sock->sk;
    struct qdmeta_sock *qsk = qdmeta_sk(sk);
    struct sockaddr_qdmeta *dest = NULL;
    struct qdmeta_sock *target;
    struct sk_buff *skb;
    int err;

    lock_sock(sk);

    /* Get destination */
    if (msg->msg_name) {
        dest = msg->msg_name;
        if (msg->msg_namelen < sizeof(*dest)) {
            err = -EINVAL;
            goto out;
        }
        if (dest->sqm_family != AF_QDMETA) {
            err = -EINVAL;
            goto out;
        }
    } else if (qsk->connected) {
        /* Use connected peer */
    } else {
        err = -EDESTADDRREQ;
        goto out;
    }

    /* Auto-bind if not bound */
    if (!qsk->bound) {
        qsk->port = qdmeta_alloc_port();
        qsk->bound = true;
    }

    /* Find target socket */
    if (dest) {
        target = qdmeta_find_socket(dest->sqm_port, dest->sqm_addr);
    } else {
        target = qdmeta_find_socket(qsk->peer_port, qsk->peer_addr);
    }

    if (!target) {
        err = -ECONNREFUSED;
        goto out;
    }

    /* Allocate skb */
    skb = alloc_skb(len, GFP_KERNEL);
    if (!skb) {
        err = -ENOMEM;
        goto out;
    }

    /* Copy data from user */
    err = memcpy_from_msg(skb_put(skb, len), msg, len);
    if (err) {
        kfree_skb(skb);
        goto out;
    }

    /* Set timestamp */
    skb->tstamp = ktime_get_real();

    /* Store source info in skb control block */
    /* (Could use skb->cb for this) */

    /* Deliver to target */
    err = qdmeta_deliver(target, skb);
    if (err) {
        kfree_skb(skb);
        goto out;
    }

    qsk->tx_packets++;
    qsk->tx_bytes += len;
    qsk->tx_seq++;
    err = len;

out:
    release_sock(sk);
    return err;
}

static const struct proto_ops qdmeta_dgram_ops = {
    .family     = PF_QDMETA,
    .owner      = THIS_MODULE,
    .release    = qdmeta_release,
    .bind       = qdmeta_bind,
    .connect    = qdmeta_connect,
    .socketpair = sock_no_socketpair,
    .accept     = sock_no_accept,
    .getname    = qdmeta_getname,
    .poll       = qdmeta_poll,
    .ioctl      = sock_no_ioctl,
    .listen     = sock_no_listen,
    .shutdown   = sock_no_shutdown,
    .sendmsg    = qdmeta_sendmsg,
    .recvmsg    = qdmeta_recvmsg,
    .mmap       = sock_no_mmap,
};

/* Socket creation */
static int qdmeta_create(struct net *net, struct socket *sock, 
                         int protocol, int kern)
{
    struct sock *sk;
    struct qdmeta_sock *qsk;

    if (sock->type != SOCK_DGRAM && sock->type != SOCK_SEQPACKET)
        return -ESOCKTNOSUPPORT;

    if (protocol < 0 || protocol > 255)
        return -EINVAL;

    sk = sk_alloc(net, PF_QDMETA, GFP_KERNEL, &qdmeta_proto, kern);
    if (!sk)
        return -ENOMEM;

    sock_init_data(sock, sk);
    sock->ops = &qdmeta_dgram_ops;

    qsk = qdmeta_sk(sk);
    qsk->protocol = protocol;
    INIT_LIST_HEAD(&qsk->list);
    INIT_LIST_HEAD(&qsk->accept_queue);
    skb_queue_head_init(&qsk->rx_queue);
    init_waitqueue_head(&qsk->rx_wait);

    /* Add to global list */
    spin_lock(&qdmeta_sockets_lock);
    list_add(&qsk->list, &qdmeta_sockets);
    spin_unlock(&qdmeta_sockets_lock);

    return 0;
}

static struct net_proto_family qdmeta_family = {
    .family = PF_QDMETA,
    .create = qdmeta_create,
    .owner  = THIS_MODULE,
};

static struct proto qdmeta_proto = {
    .name     = "QDMETA",
    .owner    = THIS_MODULE,
    .obj_size = sizeof(struct qdmeta_sock),
};

/*
 * Module init/exit
 */

static int __init qdmeta_init(void)
{
    int err;

    /* Register protocol */
    err = proto_register(&qdmeta_proto, 1);
    if (err) {
        pr_err("qdmeta: failed to register proto: %d\n", err);
        return err;
    }

    /* Register socket family */
    err = sock_register(&qdmeta_family);
    if (err) {
        pr_err("qdmeta: failed to register socket family: %d\n", err);
        proto_unregister(&qdmeta_proto);
        return err;
    }

    pr_info("qdmeta: module loaded, AF_QDMETA=%d\n", AF_QDMETA);
    pr_info("qdmeta: socket(AF_QDMETA, SOCK_DGRAM, protocol)\n");
    return 0;
}

static void __exit qdmeta_exit(void)
{
    sock_unregister(PF_QDMETA);
    proto_unregister(&qdmeta_proto);

    pr_info("qdmeta: module unloaded\n");
}

module_init(qdmeta_init);
module_exit(qdmeta_exit);
