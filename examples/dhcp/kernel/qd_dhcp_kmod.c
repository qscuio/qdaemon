/*
 * QDaemon DHCP Kernel Module
 *
 * Features:
 * - Virtual network device (qdhcp0) for userspace communication
 * - DHCP packet interception via:
 *   1. dev_add_pack() - Protocol handler for ETH_P_IP
 *   2. TC ingress hook - Traffic control classifier
 * - Metadata header prepended to packets for raw socket communication
 * - Direct TX via dev_queue_xmit() (bypasses TCP/IP stack)
 * - DHCP snooping binding table
 * - Netlink for control commands only
 *
 * Packet Flow:
 *   RX: Physical NIC -> intercept -> [META_HDR][ETH][IP][UDP][DHCP] -> qdhcp0 -> raw socket
 *   TX: raw socket -> qdhcp0 -> parse [META_HDR] -> dev_queue_xmit -> Physical NIC
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <net/genetlink.h>
#include <net/netlink.h>
#include <net/pkt_cls.h>
#include <net/sch_generic.h>

#include "../include/dhcp_kmod.h"
#include "qd_dhcp_bde.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QDaemon");
MODULE_DESCRIPTION("DHCP Virtual Network Device with Snooping Support");
MODULE_VERSION("2.0");

/*
 * Module parameters
 */
static int intercept_method = QDHCP_METHOD_BOTH;
module_param(intercept_method, int, 0644);
MODULE_PARM_DESC(intercept_method, "Interception method: 1=dev_add_pack, 2=TC, 3=both");

/*
 * Constants
 */
#define DHCP_SERVER_PORT     67
#define DHCP_CLIENT_PORT     68
#define BINDING_HASH_BITS    8
#define MAX_INTERFACES       32

/*
 * Char device wire format - matches userspace qd_dhcp_msg_hdr_t
 */
#define QDHCP_MSG_MAGIC       0x51444843  /* "QDHC" */
#define QDHCP_MSG_VERSION     1

/* Message flags */
#define QDHCP_MSG_FLAG_ACK        0x01  /* Request acknowledgment */
#define QDHCP_MSG_FLAG_RESPONSE   0x02  /* This is a response */
#define QDHCP_MSG_FLAG_ERROR      0x04  /* Error response */
#define QDHCP_MSG_FLAG_NOTIFY     0x20  /* Notification (no ACK expected) */

struct qdhcp_msg_hdr {
    __u32 magic;
    __u8  version;
    __u8  cmd;
    __u8  flags;
    __u8  reserved;
    __u32 seq;
    __u32 len;
    __s32 status;
} __packed;

#define QDHCP_MSG_HDR_SIZE    sizeof(struct qdhcp_msg_hdr)

/* Attribute header */
struct qdhcp_attr_hdr {
    __u16 type;
    __u16 len;    /* Including header */
} __packed;

#define QDHCP_ATTR_HDR_SIZE   sizeof(struct qdhcp_attr_hdr)
#define QDHCP_ATTR_ALIGN(len) (((len) + 3) & ~3)

/* IOCTL commands for char device */
#define QDHCP_IOC_MAGIC     'Q'
#define QDHCP_IOC_SUBSCRIBE   _IO(QDHCP_IOC_MAGIC, 1)
#define QDHCP_IOC_UNSUBSCRIBE _IO(QDHCP_IOC_MAGIC, 2)

/* Char device constants */
#define QDHCP_CHARDEV_MSG_QUEUE_MAX   64
#define QDHCP_CHARDEV_MSG_MAX_SIZE    4096

/*
 * Queued message for char device notification delivery
 */
struct qdhcp_queued_msg {
    struct list_head list;
    size_t           len;
    u8               data[];
};

/*
 * Per-client private data for char device
 */
struct qdhcp_chardev_client {
    struct list_head    list;           /* In clients list */
    spinlock_t          lock;
    bool                subscribed;     /* Receiving notifications? */
    struct list_head    msg_queue;      /* Queued messages */
    int                 queue_count;
    wait_queue_head_t   wait_queue;
};

/*
 * Binding Table Entry
 */
struct binding_entry {
    struct hlist_node hash_node;
    u8 mac[ETH_ALEN];
    __be32 ip;
    int ifindex;
    u32 lease_time;
    u64 expire_time_ns;
};

/*
 * Monitored Interface Entry
 */
struct monitored_iface {
    int ifindex;
    char ifname[IFNAMSIZ];
    bool trusted;
    bool active;
};

/*
 * Per-device private data
 */
struct qdhcp_priv {
    struct net_device *netdev;
    struct qdhcp_stats stats;
    spinlock_t lock;
    bool daemon_registered;

    /* Binding table */
    DECLARE_HASHTABLE(bindings, BINDING_HASH_BITS);
    spinlock_t bind_lock;
    int bind_count;

    /* Monitored interfaces */
    struct monitored_iface ifaces[MAX_INTERFACES];
    int iface_count;
    spinlock_t iface_lock;

    /* Interception method */
    u8 method;
};

/* Global state */
static struct net_device *qdhcp_netdev;
static struct genl_family qdhcp_genl_family;
static struct packet_type qdhcp_ptype;
static bool ptype_registered;

/* Char device global state */
static struct miscdevice qdhcp_chardev;
static bool chardev_registered;
static LIST_HEAD(chardev_clients);
static DEFINE_SPINLOCK(chardev_clients_lock);

/* Forward declarations for chardev notification functions */
static void qdhcp_chardev_notify_binding(const u8 *mac, __be32 ip, int ifindex,
                                          u32 lease_time, u8 event_type);
static void qdhcp_chardev_notify_iface(int ifindex, const char *ifname,
                                        bool trusted, u8 event_type);

/*
 * Multicast group for notifications
 */
static const struct genl_multicast_group qdhcp_mcgrps[] = {
    { .name = QDHCP_MCGRP_NAME },
};

enum qdhcp_mcgrp_ids {
    QDHCP_MCGRP_EVENTS,
};

/*
 * Hash function for MAC address
 */
static inline u32 mac_hash(const u8 *mac)
{
    return jhash(mac, ETH_ALEN, 0);
}

/*
 * Notification Functions
 */
static void qdhcp_notify_binding_change(const u8 *mac, __be32 ip, int ifindex,
                                         u32 lease_time, u8 event_type)
{
    struct sk_buff *skb;
    void *hdr;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb)
        return;

    hdr = genlmsg_put(skb, 0, 0, &qdhcp_genl_family, 0, QDHCP_CMD_NOTIFY_BINDING);
    if (!hdr) {
        nlmsg_free(skb);
        return;
    }

    if (nla_put_u8(skb, QDHCP_ATTR_EVENT_TYPE, event_type) ||
        nla_put(skb, QDHCP_ATTR_MAC, ETH_ALEN, mac) ||
        nla_put_u32(skb, QDHCP_ATTR_IP, ntohl(ip)) ||
        nla_put_s32(skb, QDHCP_ATTR_IFINDEX, ifindex) ||
        nla_put_u32(skb, QDHCP_ATTR_LEASE_TIME, lease_time)) {
        genlmsg_cancel(skb, hdr);
        nlmsg_free(skb);
        return;
    }

    genlmsg_end(skb, hdr);
    genlmsg_multicast(&qdhcp_genl_family, skb, 0, QDHCP_MCGRP_EVENTS, GFP_ATOMIC);
}

static void qdhcp_notify_iface_change(int ifindex, const char *ifname,
                                       bool trusted, u8 event_type)
{
    struct sk_buff *skb;
    void *hdr;

    skb = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
    if (!skb)
        return;

    hdr = genlmsg_put(skb, 0, 0, &qdhcp_genl_family, 0, QDHCP_CMD_NOTIFY_IFACE);
    if (!hdr) {
        nlmsg_free(skb);
        return;
    }

    if (nla_put_u8(skb, QDHCP_ATTR_EVENT_TYPE, event_type) ||
        nla_put_s32(skb, QDHCP_ATTR_IFINDEX, ifindex) ||
        nla_put_string(skb, QDHCP_ATTR_IFNAME, ifname) ||
        nla_put_u8(skb, QDHCP_ATTR_TRUSTED, trusted ? 1 : 0)) {
        genlmsg_cancel(skb, hdr);
        nlmsg_free(skb);
        return;
    }

    genlmsg_end(skb, hdr);
    genlmsg_multicast(&qdhcp_genl_family, skb, 0, QDHCP_MCGRP_EVENTS, GFP_ATOMIC);
}

/*
 * Binding Table Functions
 */
static struct binding_entry *binding_find(struct qdhcp_priv *priv, const u8 *mac)
{
    struct binding_entry *entry;
    u32 h = mac_hash(mac);

    hash_for_each_possible(priv->bindings, entry, hash_node, h) {
        if (ether_addr_equal(entry->mac, mac))
            return entry;
    }
    return NULL;
}

static int binding_add(struct qdhcp_priv *priv, const u8 *mac, __be32 ip,
                       int ifindex, u32 lease_time)
{
    struct binding_entry *entry;
    unsigned long flags;
    bool is_new = false;

    spin_lock_irqsave(&priv->bind_lock, flags);

    entry = binding_find(priv, mac);
    if (entry) {
        entry->ip = ip;
        entry->ifindex = ifindex;
        entry->lease_time = lease_time;
        entry->expire_time_ns = ktime_get_ns() + (u64)lease_time * NSEC_PER_SEC;
        spin_unlock_irqrestore(&priv->bind_lock, flags);
        /* Notify even for updates - via netlink and chardev */
        qdhcp_notify_binding_change(mac, ip, ifindex, lease_time,
                                    QDHCP_EVENT_BINDING_ADD);
        if (chardev_registered)
            qdhcp_chardev_notify_binding(mac, ip, ifindex, lease_time,
                                         QDHCP_EVENT_BINDING_ADD);
        return 0;
    }

    entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
    if (!entry) {
        spin_unlock_irqrestore(&priv->bind_lock, flags);
        return -ENOMEM;
    }

    memcpy(entry->mac, mac, ETH_ALEN);
    entry->ip = ip;
    entry->ifindex = ifindex;
    entry->lease_time = lease_time;
    entry->expire_time_ns = ktime_get_ns() + (u64)lease_time * NSEC_PER_SEC;

    hash_add(priv->bindings, &entry->hash_node, mac_hash(mac));
    priv->bind_count++;
    is_new = true;

    spin_unlock_irqrestore(&priv->bind_lock, flags);

    /* Send notification for new binding - via netlink and chardev */
    if (is_new) {
        qdhcp_notify_binding_change(mac, ip, ifindex, lease_time,
                                    QDHCP_EVENT_BINDING_ADD);
        if (chardev_registered)
            qdhcp_chardev_notify_binding(mac, ip, ifindex, lease_time,
                                         QDHCP_EVENT_BINDING_ADD);
    }
    return 0;
}

static int binding_del(struct qdhcp_priv *priv, const u8 *mac)
{
    struct binding_entry *entry;
    unsigned long flags;
    __be32 ip;
    int ifindex;
    u32 lease_time;

    spin_lock_irqsave(&priv->bind_lock, flags);

    entry = binding_find(priv, mac);
    if (!entry) {
        spin_unlock_irqrestore(&priv->bind_lock, flags);
        return -ENOENT;
    }

    /* Save info for notification */
    ip = entry->ip;
    ifindex = entry->ifindex;
    lease_time = entry->lease_time;

    hash_del(&entry->hash_node);
    priv->bind_count--;

    spin_unlock_irqrestore(&priv->bind_lock, flags);

    kfree(entry);

    /* Send notification - via netlink and chardev */
    qdhcp_notify_binding_change(mac, ip, ifindex, lease_time,
                                QDHCP_EVENT_BINDING_DEL);
    if (chardev_registered)
        qdhcp_chardev_notify_binding(mac, ip, ifindex, lease_time,
                                     QDHCP_EVENT_BINDING_DEL);
    return 0;
}

static void binding_clear_all(struct qdhcp_priv *priv)
{
    struct binding_entry *entry;
    struct hlist_node *tmp;
    unsigned long flags;
    int bkt;

    spin_lock_irqsave(&priv->bind_lock, flags);

    hash_for_each_safe(priv->bindings, bkt, tmp, entry, hash_node) {
        hash_del(&entry->hash_node);
        kfree(entry);
    }
    priv->bind_count = 0;

    spin_unlock_irqrestore(&priv->bind_lock, flags);
}

/*
 * Interface Management
 */
static struct monitored_iface *iface_find(struct qdhcp_priv *priv, int ifindex)
{
    int i;
    for (i = 0; i < priv->iface_count; i++) {
        if (priv->ifaces[i].ifindex == ifindex && priv->ifaces[i].active)
            return &priv->ifaces[i];
    }
    return NULL;
}

static bool iface_is_monitored(struct qdhcp_priv *priv, int ifindex)
{
    if (priv->iface_count == 0)
        return true;  /* Monitor all if none configured */
    return iface_find(priv, ifindex) != NULL;
}

static bool iface_is_trusted(struct qdhcp_priv *priv, int ifindex)
{
    struct monitored_iface *iface = iface_find(priv, ifindex);
    return iface ? iface->trusted : false;
}

static int iface_add(struct qdhcp_priv *priv, int ifindex, const char *name)
{
    unsigned long flags;
    int i;
    bool is_new = false;

    spin_lock_irqsave(&priv->iface_lock, flags);

    for (i = 0; i < priv->iface_count; i++) {
        if (priv->ifaces[i].ifindex == ifindex) {
            priv->ifaces[i].active = true;
            spin_unlock_irqrestore(&priv->iface_lock, flags);
            return 0;
        }
    }

    if (priv->iface_count >= MAX_INTERFACES) {
        spin_unlock_irqrestore(&priv->iface_lock, flags);
        return -ENOSPC;
    }

    priv->ifaces[priv->iface_count].ifindex = ifindex;
    strscpy(priv->ifaces[priv->iface_count].ifname, name, IFNAMSIZ);
    priv->ifaces[priv->iface_count].trusted = false;
    priv->ifaces[priv->iface_count].active = true;
    priv->iface_count++;
    is_new = true;

    spin_unlock_irqrestore(&priv->iface_lock, flags);

    pr_info("qdhcp: added interface %s (ifindex=%d)\n", name, ifindex);

    /* Send notification - via netlink and chardev */
    if (is_new) {
        qdhcp_notify_iface_change(ifindex, name, false, QDHCP_EVENT_IFACE_ADD);
        if (chardev_registered)
            qdhcp_chardev_notify_iface(ifindex, name, false,
                                       QDHCP_EVENT_IFACE_ADD);
    }

    return 0;
}

static int iface_del(struct qdhcp_priv *priv, int ifindex)
{
    struct monitored_iface *iface;
    unsigned long flags;
    char ifname[IFNAMSIZ];
    bool trusted;

    spin_lock_irqsave(&priv->iface_lock, flags);

    iface = iface_find(priv, ifindex);
    if (!iface) {
        spin_unlock_irqrestore(&priv->iface_lock, flags);
        return -ENOENT;
    }

    /* Save info for notification */
    strscpy(ifname, iface->ifname, IFNAMSIZ);
    trusted = iface->trusted;

    iface->active = false;

    spin_unlock_irqrestore(&priv->iface_lock, flags);

    /* Send notification - via netlink and chardev */
    qdhcp_notify_iface_change(ifindex, ifname, trusted, QDHCP_EVENT_IFACE_DEL);
    if (chardev_registered)
        qdhcp_chardev_notify_iface(ifindex, ifname, trusted,
                                   QDHCP_EVENT_IFACE_DEL);

    return 0;
}

/*
 * DHCP Message Type Extraction
 */
static u8 get_dhcp_msg_type(const u8 *dhcp_data, int len)
{
    const u8 *opts;
    int opts_len, i;

    if (len < 240)
        return 0;

    /* Verify DHCP magic cookie: 99.130.83.99 */
    if (dhcp_data[236] != 99 || dhcp_data[237] != 130 ||
        dhcp_data[238] != 83 || dhcp_data[239] != 99)
        return 0;

    opts = dhcp_data + 240;
    opts_len = len - 240;

    for (i = 0; i < opts_len; ) {
        u8 opt = opts[i];
        u8 opt_len;

        if (opt == 255) break;
        if (opt == 0) { i++; continue; }
        if (i + 1 >= opts_len) break;

        opt_len = opts[i + 1];
        if (i + 2 + opt_len > opts_len) break;

        if (opt == 53 && opt_len >= 1)
            return opts[i + 2];

        i += 2 + opt_len;
    }

    return 0;
}

static void update_dhcp_stats(struct qdhcp_priv *priv, u8 msg_type)
{
    switch (msg_type) {
    case DHCP_TYPE_DISCOVER: priv->stats.dhcp_discover++; break;
    case DHCP_TYPE_OFFER:    priv->stats.dhcp_offer++; break;
    case DHCP_TYPE_REQUEST:  priv->stats.dhcp_request++; break;
    case DHCP_TYPE_ACK:      priv->stats.dhcp_ack++; break;
    case DHCP_TYPE_NAK:      priv->stats.dhcp_nak++; break;
    case DHCP_TYPE_RELEASE:  priv->stats.dhcp_release++; break;
    case DHCP_TYPE_DECLINE:  priv->stats.dhcp_decline++; break;
    }
}

/*
 * Check if packet is DHCP (UDP port 67 or 68)
 */
static bool is_dhcp_packet(struct sk_buff *skb, u16 *sport, u16 *dport)
{
    struct iphdr *iph;
    struct udphdr *udph;
    int ip_hdr_len;

    if (!pskb_may_pull(skb, sizeof(struct iphdr)))
        return false;

    iph = ip_hdr(skb);
    if (iph->protocol != IPPROTO_UDP)
        return false;

    ip_hdr_len = iph->ihl * 4;
    if (!pskb_may_pull(skb, ip_hdr_len + sizeof(struct udphdr)))
        return false;

    udph = (struct udphdr *)((u8 *)iph + ip_hdr_len);
    *sport = ntohs(udph->source);
    *dport = ntohs(udph->dest);

    return (*sport == DHCP_SERVER_PORT || *sport == DHCP_CLIENT_PORT ||
            *dport == DHCP_SERVER_PORT || *dport == DHCP_CLIENT_PORT);
}

/*
 * Forward DHCP packet to userspace via qdhcp0
 * Prepends metadata header to the original ethernet frame
 */
static int forward_to_userspace(struct qdhcp_priv *priv, struct sk_buff *orig_skb,
                                int src_ifindex, u8 method)
{
    struct sk_buff *skb;
    struct qdhcp_meta_hdr *meta;
    struct ethhdr *orig_eth;
    struct iphdr *iph;
    struct udphdr *udph;
    const u8 *dhcp_data;
    int dhcp_len, eth_len, total_len;
    u8 msg_type;

    /* Get ethernet header */
    if (!skb_mac_header_was_set(orig_skb)) {
        priv->stats.rx_dropped++;
        return -EINVAL;
    }

    orig_eth = eth_hdr(orig_skb);
    eth_len = orig_skb->len + (orig_skb->data - skb_mac_header(orig_skb));

    /* Get DHCP payload for message type */
    iph = ip_hdr(orig_skb);
    udph = (struct udphdr *)((u8 *)iph + iph->ihl * 4);
    dhcp_data = (u8 *)udph + sizeof(struct udphdr);
    dhcp_len = ntohs(udph->len) - sizeof(struct udphdr);

    msg_type = get_dhcp_msg_type(dhcp_data, dhcp_len);
    update_dhcp_stats(priv, msg_type);

    /* Allocate skb: metadata + original ethernet frame */
    total_len = QDHCP_META_HDR_SIZE + eth_len;
    skb = netdev_alloc_skb(priv->netdev, total_len + NET_IP_ALIGN);
    if (!skb) {
        priv->stats.rx_dropped++;
        return -ENOMEM;
    }

    skb_reserve(skb, NET_IP_ALIGN);

    /* Build metadata header */
    meta = skb_put(skb, QDHCP_META_HDR_SIZE);
    memset(meta, 0, QDHCP_META_HDR_SIZE);
    meta->magic = QDHCP_META_MAGIC;
    meta->version = QDHCP_META_VERSION;
    meta->direction = QDHCP_DIR_RX;
    meta->msg_type = msg_type;
    meta->ifindex = src_ifindex;
    meta->pkt_len = eth_len;
    meta->trusted = iface_is_trusted(priv, src_ifindex) ? 1 : 0;
    meta->method = method;
    memcpy(meta->src_mac, orig_eth->h_source, ETH_ALEN);
    memcpy(meta->dst_mac, orig_eth->h_dest, ETH_ALEN);

    if (skb_vlan_tag_present(orig_skb))
        meta->vlan_id = skb_vlan_tag_get(orig_skb);

    /* Copy original ethernet frame */
    skb_put_data(skb, skb_mac_header(orig_skb), eth_len);

    /* Set up skb for qdhcp0 */
    skb_reset_mac_header(skb);
    skb->protocol = htons(ETH_P_IP);
    skb->dev = priv->netdev;
    skb->ip_summed = CHECKSUM_UNNECESSARY;

    /* Inject into qdhcp0 - userspace reads via raw socket */
    netif_rx(skb);

    priv->stats.rx_packets++;
    priv->stats.rx_bytes += total_len;

    pr_debug("qdhcp: RX ifindex=%d type=%u len=%d method=%u\n",
             src_ifindex, msg_type, eth_len, method);

    return 0;
}

/*
 * Method 1: dev_add_pack() Protocol Handler
 */
static int ptype_handler(struct sk_buff *skb, struct net_device *dev,
                         struct packet_type *pt, struct net_device *orig_dev)
{
    struct qdhcp_priv *priv;
    u16 sport, dport;

    if (!qdhcp_netdev)
        goto pass;

    priv = netdev_priv(qdhcp_netdev);

    if (!(priv->method & QDHCP_METHOD_DEV_ADD_PACK))
        goto pass;

    /* Don't intercept from qdhcp0 itself */
    if (orig_dev == qdhcp_netdev)
        goto pass;

    if (!iface_is_monitored(priv, orig_dev->ifindex))
        goto pass;

    if (!is_dhcp_packet(skb, &sport, &dport))
        goto pass;

    priv->stats.dev_pack_intercepts++;
    forward_to_userspace(priv, skb, orig_dev->ifindex, QDHCP_METHOD_DEV_ADD_PACK);

pass:
    kfree_skb(skb);
    return NET_RX_SUCCESS;
}

/*
 * Method 2: TC Ingress Classifier
 */
static int tc_classify(struct sk_buff *skb, const struct tcf_proto *tp,
                       struct tcf_result *res)
{
    struct qdhcp_priv *priv;
    u16 sport = 0, dport = 0;

    if (!qdhcp_netdev)
        return TC_ACT_OK;

    priv = netdev_priv(qdhcp_netdev);

    if (!(priv->method & QDHCP_METHOD_TC_INGRESS))
        return TC_ACT_OK;

    if (skb->protocol != htons(ETH_P_IP))
        return TC_ACT_OK;

    if (skb->dev == qdhcp_netdev)
        return TC_ACT_OK;

    if (!iface_is_monitored(priv, skb->dev->ifindex))
        return TC_ACT_OK;

    if (!pskb_may_pull(skb, sizeof(struct iphdr)))
        return TC_ACT_OK;

    skb_reset_network_header(skb);

    if (!is_dhcp_packet(skb, &sport, &dport))
        return TC_ACT_OK;

    priv->stats.tc_intercepts++;
    forward_to_userspace(priv, skb, skb->dev->ifindex, QDHCP_METHOD_TC_INGRESS);

    return TC_ACT_OK;
}

static int tc_init(struct tcf_proto *tp)
{
    return 0;
}

static void tc_destroy(struct tcf_proto *tp, bool rtnl_held,
                       struct netlink_ext_ack *extack)
{
}

static struct tcf_proto_ops qdhcp_tc_ops __read_mostly = {
    .kind       = "qdhcp",
    .classify   = tc_classify,
    .init       = tc_init,
    .destroy    = tc_destroy,
    .owner      = THIS_MODULE,
};

/*
 * Network device operations
 */
static int qdhcp_dev_open(struct net_device *dev)
{
    netif_start_queue(dev);
    pr_info("qdhcp: device %s opened\n", dev->name);
    return 0;
}

static int qdhcp_dev_stop(struct net_device *dev)
{
    netif_stop_queue(dev);
    pr_info("qdhcp: device %s stopped\n", dev->name);
    return 0;
}

/*
 * TX from userspace: parse metadata header and forward to physical interface
 */
static netdev_tx_t qdhcp_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct qdhcp_priv *priv = netdev_priv(dev);
    struct qdhcp_meta_hdr *meta;
    struct net_device *out_dev;
    struct sk_buff *nskb;
    int pkt_offset;

    /* Validate minimum length */
    if (skb->len < QDHCP_META_HDR_SIZE) {
        pr_debug("qdhcp: TX skb too short (%d)\n", skb->len);
        goto drop;
    }

    /* Get metadata header */
    meta = (struct qdhcp_meta_hdr *)skb->data;

    /* Validate magic */
    if (meta->magic != QDHCP_META_MAGIC) {
        pr_debug("qdhcp: TX invalid magic 0x%08x\n", meta->magic);
        goto drop;
    }

    /* Get target interface */
    out_dev = dev_get_by_index(&init_net, meta->ifindex);
    if (!out_dev) {
        pr_debug("qdhcp: TX target ifindex %d not found\n", meta->ifindex);
        goto drop;
    }

    /* Skip metadata, get packet */
    pkt_offset = QDHCP_META_HDR_SIZE;
    if (skb->len < pkt_offset + meta->pkt_len) {
        pr_debug("qdhcp: TX packet length mismatch\n");
        dev_put(out_dev);
        goto drop;
    }

    /* Allocate new skb for the actual packet */
    nskb = netdev_alloc_skb(out_dev, meta->pkt_len + NET_IP_ALIGN);
    if (!nskb) {
        dev_put(out_dev);
        goto drop;
    }

    skb_reserve(nskb, NET_IP_ALIGN);
    skb_put_data(nskb, skb->data + pkt_offset, meta->pkt_len);

    /* Set up for transmission */
    nskb->dev = out_dev;
    skb_reset_mac_header(nskb);
    nskb->protocol = eth_type_trans(nskb, out_dev);

    pr_debug("qdhcp: TX ifindex=%d len=%d\n", meta->ifindex, meta->pkt_len);

    /* Send via dev_queue_xmit - bypasses routing */
    if (dev_queue_xmit(nskb) != NET_XMIT_SUCCESS) {
        spin_lock(&priv->lock);
        priv->stats.tx_dropped++;
        spin_unlock(&priv->lock);
    } else {
        spin_lock(&priv->lock);
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += meta->pkt_len;
        spin_unlock(&priv->lock);
    }

    dev_put(out_dev);
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;

drop:
    spin_lock(&priv->lock);
    priv->stats.tx_dropped++;
    spin_unlock(&priv->lock);
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static void qdhcp_dev_get_stats64(struct net_device *dev,
                                   struct rtnl_link_stats64 *stats)
{
    struct qdhcp_priv *priv = netdev_priv(dev);

    spin_lock(&priv->lock);
    stats->rx_packets = priv->stats.rx_packets;
    stats->tx_packets = priv->stats.tx_packets;
    stats->rx_bytes = priv->stats.rx_bytes;
    stats->tx_bytes = priv->stats.tx_bytes;
    stats->rx_dropped = priv->stats.rx_dropped;
    stats->tx_dropped = priv->stats.tx_dropped;
    spin_unlock(&priv->lock);
}

static int qdhcp_dev_change_mtu(struct net_device *dev, int new_mtu)
{
    if (new_mtu < 68 || new_mtu > 9000)
        return -EINVAL;
    dev->mtu = new_mtu;
    return 0;
}

static const struct net_device_ops qdhcp_netdev_ops = {
    .ndo_open       = qdhcp_dev_open,
    .ndo_stop       = qdhcp_dev_stop,
    .ndo_start_xmit = qdhcp_dev_xmit,
    .ndo_get_stats64 = qdhcp_dev_get_stats64,
    .ndo_change_mtu = qdhcp_dev_change_mtu,
};

static void qdhcp_dev_setup(struct net_device *dev)
{
    ether_setup(dev);
    dev->netdev_ops = &qdhcp_netdev_ops;
    dev->needs_free_netdev = true;
    eth_hw_addr_random(dev);
    dev->flags |= IFF_NOARP;
    dev->features |= NETIF_F_HW_CSUM;
    dev->mtu = 1500 + QDHCP_META_HDR_SIZE;  /* Allow metadata + full frame */
}

/*
 * ==========================================================================
 * Char Device Operations
 * ==========================================================================
 */

/* Forward declaration for command processing */
static int qdhcp_chardev_process_cmd(struct qdhcp_chardev_client *client,
                                      const struct qdhcp_msg_hdr *hdr,
                                      const u8 *payload, size_t payload_len,
                                      u8 *resp_buf, size_t resp_buf_size);

/*
 * Queue a message to be delivered to subscribed clients
 */
static void qdhcp_chardev_queue_notification(const void *data, size_t len)
{
    struct qdhcp_chardev_client *client;
    unsigned long flags;

    spin_lock_irqsave(&chardev_clients_lock, flags);

    list_for_each_entry(client, &chardev_clients, list) {
        unsigned long client_flags;
        struct qdhcp_queued_msg *msg;

        spin_lock_irqsave(&client->lock, client_flags);

        if (!client->subscribed) {
            spin_unlock_irqrestore(&client->lock, client_flags);
            continue;
        }

        /* Drop oldest if queue full */
        if (client->queue_count >= QDHCP_CHARDEV_MSG_QUEUE_MAX) {
            struct qdhcp_queued_msg *oldest;
            oldest = list_first_entry(&client->msg_queue,
                                       struct qdhcp_queued_msg, list);
            list_del(&oldest->list);
            kfree(oldest);
            client->queue_count--;
        }

        /* Allocate and queue new message */
        msg = kmalloc(sizeof(*msg) + len, GFP_ATOMIC);
        if (msg) {
            msg->len = len;
            memcpy(msg->data, data, len);
            list_add_tail(&msg->list, &client->msg_queue);
            client->queue_count++;
            wake_up_interruptible(&client->wait_queue);
        }

        spin_unlock_irqrestore(&client->lock, client_flags);
    }

    spin_unlock_irqrestore(&chardev_clients_lock, flags);
}

/*
 * Build and queue a binding change notification
 */
static void qdhcp_chardev_notify_binding(const u8 *mac, __be32 ip, int ifindex,
                                          u32 lease_time, u8 event_type)
{
    u8 buf[256];
    struct qdhcp_msg_hdr *hdr = (struct qdhcp_msg_hdr *)buf;
    u8 *ptr = buf + QDHCP_MSG_HDR_SIZE;
    size_t payload_len = 0;
    struct qdhcp_attr_hdr *attr;
    size_t attr_space;

    memset(hdr, 0, QDHCP_MSG_HDR_SIZE);
    hdr->magic = QDHCP_MSG_MAGIC;
    hdr->version = QDHCP_MSG_VERSION;
    hdr->cmd = QDHCP_CMD_NOTIFY_BINDING;
    hdr->flags = QDHCP_MSG_FLAG_NOTIFY;
    hdr->seq = 0;
    hdr->status = 0;

    /* Add event type attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_EVENT_TYPE;
    attr->len = QDHCP_ATTR_HDR_SIZE + 1;
    *(ptr + QDHCP_ATTR_HDR_SIZE) = event_type;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add MAC attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_MAC;
    attr->len = QDHCP_ATTR_HDR_SIZE + ETH_ALEN;
    memcpy(ptr + QDHCP_ATTR_HDR_SIZE, mac, ETH_ALEN);
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add IP attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_IP;
    attr->len = QDHCP_ATTR_HDR_SIZE + 4;
    *(__u32 *)(ptr + QDHCP_ATTR_HDR_SIZE) = ntohl(ip);
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add ifindex attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_IFINDEX;
    attr->len = QDHCP_ATTR_HDR_SIZE + 4;
    *(__s32 *)(ptr + QDHCP_ATTR_HDR_SIZE) = ifindex;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add lease_time attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_LEASE_TIME;
    attr->len = QDHCP_ATTR_HDR_SIZE + 4;
    *(__u32 *)(ptr + QDHCP_ATTR_HDR_SIZE) = lease_time;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    hdr->len = payload_len;

    qdhcp_chardev_queue_notification(buf, QDHCP_MSG_HDR_SIZE + payload_len);
}

/*
 * Build and queue an interface change notification
 */
static void qdhcp_chardev_notify_iface(int ifindex, const char *ifname,
                                        bool trusted, u8 event_type)
{
    u8 buf[256];
    struct qdhcp_msg_hdr *hdr = (struct qdhcp_msg_hdr *)buf;
    u8 *ptr = buf + QDHCP_MSG_HDR_SIZE;
    size_t payload_len = 0;
    struct qdhcp_attr_hdr *attr;
    size_t attr_space;
    size_t name_len;

    memset(hdr, 0, QDHCP_MSG_HDR_SIZE);
    hdr->magic = QDHCP_MSG_MAGIC;
    hdr->version = QDHCP_MSG_VERSION;
    hdr->cmd = QDHCP_CMD_NOTIFY_IFACE;
    hdr->flags = QDHCP_MSG_FLAG_NOTIFY;
    hdr->seq = 0;
    hdr->status = 0;

    /* Add event type attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_EVENT_TYPE;
    attr->len = QDHCP_ATTR_HDR_SIZE + 1;
    *(ptr + QDHCP_ATTR_HDR_SIZE) = event_type;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add ifindex attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_IFINDEX;
    attr->len = QDHCP_ATTR_HDR_SIZE + 4;
    *(__s32 *)(ptr + QDHCP_ATTR_HDR_SIZE) = ifindex;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    /* Add ifname attribute */
    if (ifname && ifname[0]) {
        name_len = strlen(ifname) + 1;
        attr = (struct qdhcp_attr_hdr *)ptr;
        attr->type = QDHCP_ATTR_IFNAME;
        attr->len = QDHCP_ATTR_HDR_SIZE + name_len;
        memcpy(ptr + QDHCP_ATTR_HDR_SIZE, ifname, name_len);
        attr_space = QDHCP_ATTR_ALIGN(attr->len);
        ptr += attr_space;
        payload_len += attr_space;
    }

    /* Add trusted attribute */
    attr = (struct qdhcp_attr_hdr *)ptr;
    attr->type = QDHCP_ATTR_TRUSTED;
    attr->len = QDHCP_ATTR_HDR_SIZE + 1;
    *(ptr + QDHCP_ATTR_HDR_SIZE) = trusted ? 1 : 0;
    attr_space = QDHCP_ATTR_ALIGN(attr->len);
    ptr += attr_space;
    payload_len += attr_space;

    hdr->len = payload_len;

    qdhcp_chardev_queue_notification(buf, QDHCP_MSG_HDR_SIZE + payload_len);
}

/*
 * Parse attributes from message payload
 */
static int qdhcp_chardev_find_attr(const u8 *payload, size_t len,
                                    u16 type, void **data, size_t *data_len)
{
    const u8 *ptr = payload;
    size_t remaining = len;

    while (remaining >= QDHCP_ATTR_HDR_SIZE) {
        const struct qdhcp_attr_hdr *attr = (const struct qdhcp_attr_hdr *)ptr;
        size_t padded_len = QDHCP_ATTR_ALIGN(attr->len);

        if (attr->len < QDHCP_ATTR_HDR_SIZE || padded_len > remaining)
            break;

        if (attr->type == type) {
            *data = (void *)(ptr + QDHCP_ATTR_HDR_SIZE);
            *data_len = attr->len - QDHCP_ATTR_HDR_SIZE;
            return 0;
        }

        ptr += padded_len;
        remaining -= padded_len;
    }

    return -ENOENT;
}

/*
 * Process a command from char device client
 */
static int qdhcp_chardev_process_cmd(struct qdhcp_chardev_client *client,
                                      const struct qdhcp_msg_hdr *hdr,
                                      const u8 *payload, size_t payload_len,
                                      u8 *resp_buf, size_t resp_buf_size)
{
    struct qdhcp_priv *priv;
    struct qdhcp_msg_hdr *resp_hdr;
    int ret = 0;
    void *data;
    size_t data_len;

    if (!qdhcp_netdev)
        return -ENODEV;

    priv = netdev_priv(qdhcp_netdev);
    resp_hdr = (struct qdhcp_msg_hdr *)resp_buf;

    /* Build response header */
    memset(resp_hdr, 0, QDHCP_MSG_HDR_SIZE);
    resp_hdr->magic = QDHCP_MSG_MAGIC;
    resp_hdr->version = QDHCP_MSG_VERSION;
    resp_hdr->cmd = hdr->cmd;
    resp_hdr->flags = QDHCP_MSG_FLAG_RESPONSE;
    resp_hdr->seq = hdr->seq;
    resp_hdr->len = 0;
    resp_hdr->status = 0;

    switch (hdr->cmd) {
    case QDHCP_CMD_REGISTER:
        spin_lock(&priv->lock);
        priv->daemon_registered = true;
        spin_unlock(&priv->lock);
        pr_info("qdhcp: daemon registered (chardev)\n");
        break;

    case QDHCP_CMD_UNREGISTER:
        spin_lock(&priv->lock);
        priv->daemon_registered = false;
        spin_unlock(&priv->lock);
        pr_info("qdhcp: daemon unregistered (chardev)\n");
        break;

    case QDHCP_CMD_ADD_INTERFACE: {
        __s32 ifindex = 0;
        char ifname[IFNAMSIZ] = {0};

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IFINDEX, &data, &data_len) == 0 &&
            data_len >= sizeof(__s32)) {
            ifindex = *(__s32 *)data;
        } else {
            ret = -EINVAL;
            break;
        }

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IFNAME, &data, &data_len) == 0) {
            strscpy(ifname, data, min(data_len + 1, (size_t)IFNAMSIZ));
        }

        ret = iface_add(priv, ifindex, ifname);
        break;
    }

    case QDHCP_CMD_DEL_INTERFACE: {
        __s32 ifindex = 0;

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IFINDEX, &data, &data_len) == 0 &&
            data_len >= sizeof(__s32)) {
            ifindex = *(__s32 *)data;
        } else {
            ret = -EINVAL;
            break;
        }

        ret = iface_del(priv, ifindex);
        break;
    }

    case QDHCP_CMD_SET_TRUSTED: {
        __s32 ifindex = 0;
        u8 trusted = 0;
        struct monitored_iface *iface;
        unsigned long flags;

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IFINDEX, &data, &data_len) == 0 &&
            data_len >= sizeof(__s32)) {
            ifindex = *(__s32 *)data;
        } else {
            ret = -EINVAL;
            break;
        }

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_TRUSTED, &data, &data_len) == 0 &&
            data_len >= 1) {
            trusted = *(u8 *)data;
        }

        spin_lock_irqsave(&priv->iface_lock, flags);
        iface = iface_find(priv, ifindex);
        if (!iface) {
            spin_unlock_irqrestore(&priv->iface_lock, flags);
            ret = -ENOENT;
            break;
        }
        iface->trusted = trusted ? true : false;
        spin_unlock_irqrestore(&priv->iface_lock, flags);
        break;
    }

    case QDHCP_CMD_ADD_BINDING: {
        u8 mac[ETH_ALEN];
        __be32 ip = 0;
        __s32 ifindex = 0;
        __u32 lease_time = 3600;

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_MAC, &data, &data_len) == 0 &&
            data_len >= ETH_ALEN) {
            memcpy(mac, data, ETH_ALEN);
        } else {
            ret = -EINVAL;
            break;
        }

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IP, &data, &data_len) == 0 &&
            data_len >= sizeof(__u32)) {
            ip = htonl(*(__u32 *)data);
        } else {
            ret = -EINVAL;
            break;
        }

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_IFINDEX, &data, &data_len) == 0 &&
            data_len >= sizeof(__s32)) {
            ifindex = *(__s32 *)data;
        }

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_LEASE_TIME, &data, &data_len) == 0 &&
            data_len >= sizeof(__u32)) {
            lease_time = *(__u32 *)data;
        }

        ret = binding_add(priv, mac, ip, ifindex, lease_time);
        break;
    }

    case QDHCP_CMD_DEL_BINDING: {
        u8 mac[ETH_ALEN];

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_MAC, &data, &data_len) == 0 &&
            data_len >= ETH_ALEN) {
            memcpy(mac, data, ETH_ALEN);
        } else {
            ret = -EINVAL;
            break;
        }

        ret = binding_del(priv, mac);
        break;
    }

    case QDHCP_CMD_CLEAR_BINDINGS:
        binding_clear_all(priv);
        break;

    case QDHCP_CMD_SET_METHOD: {
        u8 method = 0;

        if (qdhcp_chardev_find_attr(payload, payload_len,
                                     QDHCP_ATTR_METHOD, &data, &data_len) == 0 &&
            data_len >= 1) {
            method = *(u8 *)data;
        } else {
            ret = -EINVAL;
            break;
        }

        priv->method = method;
        pr_info("qdhcp: method set to 0x%02x (chardev)\n", method);
        break;
    }

    case QDHCP_CMD_GET_STATS: {
        struct qdhcp_stats stats;
        struct qdhcp_attr_hdr *attr;
        u8 *ptr;

        spin_lock(&priv->lock);
        memcpy(&stats, &priv->stats, sizeof(stats));
        stats.binding_count = priv->bind_count;
        spin_unlock(&priv->lock);

        /* Add stats attribute to response */
        ptr = resp_buf + QDHCP_MSG_HDR_SIZE;
        attr = (struct qdhcp_attr_hdr *)ptr;
        attr->type = QDHCP_ATTR_STATS;
        attr->len = QDHCP_ATTR_HDR_SIZE + sizeof(stats);
        memcpy(ptr + QDHCP_ATTR_HDR_SIZE, &stats, sizeof(stats));
        resp_hdr->len = QDHCP_ATTR_ALIGN(attr->len);
        break;
    }

    default:
        ret = -EOPNOTSUPP;
        break;
    }

    if (ret < 0) {
        resp_hdr->flags |= QDHCP_MSG_FLAG_ERROR;
        resp_hdr->status = ret;
    }

    return QDHCP_MSG_HDR_SIZE + resp_hdr->len;
}

/*
 * Char device file operations
 */
static int qdhcp_chardev_open(struct inode *inode, struct file *filp)
{
    struct qdhcp_chardev_client *client;
    unsigned long flags;

    client = kzalloc(sizeof(*client), GFP_KERNEL);
    if (!client)
        return -ENOMEM;

    spin_lock_init(&client->lock);
    INIT_LIST_HEAD(&client->msg_queue);
    init_waitqueue_head(&client->wait_queue);

    spin_lock_irqsave(&chardev_clients_lock, flags);
    list_add(&client->list, &chardev_clients);
    spin_unlock_irqrestore(&chardev_clients_lock, flags);

    filp->private_data = client;
    pr_debug("qdhcp: chardev opened\n");
    return 0;
}

static int qdhcp_chardev_release(struct inode *inode, struct file *filp)
{
    struct qdhcp_chardev_client *client = filp->private_data;
    struct qdhcp_queued_msg *msg, *tmp;
    unsigned long flags;

    if (!client)
        return 0;

    spin_lock_irqsave(&chardev_clients_lock, flags);
    list_del(&client->list);
    spin_unlock_irqrestore(&chardev_clients_lock, flags);

    /* Free queued messages */
    spin_lock_irqsave(&client->lock, flags);
    list_for_each_entry_safe(msg, tmp, &client->msg_queue, list) {
        list_del(&msg->list);
        kfree(msg);
    }
    spin_unlock_irqrestore(&client->lock, flags);

    kfree(client);
    pr_debug("qdhcp: chardev closed\n");
    return 0;
}

static ssize_t qdhcp_chardev_read(struct file *filp, char __user *buf,
                                   size_t count, loff_t *ppos)
{
    struct qdhcp_chardev_client *client = filp->private_data;
    struct qdhcp_queued_msg *msg;
    unsigned long flags;
    ssize_t ret;

    if (!client)
        return -EINVAL;

    /* Wait for message if blocking */
    if (list_empty(&client->msg_queue)) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        ret = wait_event_interruptible(client->wait_queue,
                                        !list_empty(&client->msg_queue));
        if (ret)
            return ret;
    }

    spin_lock_irqsave(&client->lock, flags);

    if (list_empty(&client->msg_queue)) {
        spin_unlock_irqrestore(&client->lock, flags);
        return -EAGAIN;
    }

    msg = list_first_entry(&client->msg_queue, struct qdhcp_queued_msg, list);

    if (count < msg->len) {
        spin_unlock_irqrestore(&client->lock, flags);
        return -EINVAL;
    }

    list_del(&msg->list);
    client->queue_count--;

    spin_unlock_irqrestore(&client->lock, flags);

    ret = msg->len;
    if (copy_to_user(buf, msg->data, msg->len))
        ret = -EFAULT;

    kfree(msg);
    return ret;
}

static ssize_t qdhcp_chardev_write(struct file *filp, const char __user *buf,
                                    size_t count, loff_t *ppos)
{
    struct qdhcp_chardev_client *client = filp->private_data;
    u8 *msg_buf = NULL;
    u8 *resp_buf = NULL;
    struct qdhcp_msg_hdr *hdr;
    struct qdhcp_queued_msg *resp_msg;
    unsigned long flags;
    ssize_t ret;
    int resp_len;

    if (!client)
        return -EINVAL;

    if (count < QDHCP_MSG_HDR_SIZE || count > QDHCP_CHARDEV_MSG_MAX_SIZE)
        return -EINVAL;

    msg_buf = kmalloc(count, GFP_KERNEL);
    resp_buf = kmalloc(QDHCP_CHARDEV_MSG_MAX_SIZE, GFP_KERNEL);

    if (!msg_buf || !resp_buf) {
        ret = -ENOMEM;
        goto out;
    }

    if (copy_from_user(msg_buf, buf, count)) {
        ret = -EFAULT;
        goto out;
    }

    hdr = (struct qdhcp_msg_hdr *)msg_buf;

    /* Validate message */
    if (hdr->magic != QDHCP_MSG_MAGIC || hdr->version != QDHCP_MSG_VERSION) {
        ret = -EPROTO;
        goto out;
    }

    if (QDHCP_MSG_HDR_SIZE + hdr->len > count) {
        ret = -EINVAL;
        goto out;
    }

    /* Process command */
    resp_len = qdhcp_chardev_process_cmd(client, hdr,
                                          msg_buf + QDHCP_MSG_HDR_SIZE, hdr->len,
                                          resp_buf, QDHCP_CHARDEV_MSG_MAX_SIZE);

    if (resp_len < 0) {
        ret = resp_len;
        goto out;
    }

    /* Queue response if ACK requested */
    if (hdr->flags & QDHCP_MSG_FLAG_ACK) {
        resp_msg = kmalloc(sizeof(*resp_msg) + resp_len, GFP_KERNEL);
        if (!resp_msg) {
            ret = -ENOMEM;
            goto out;
        }

        resp_msg->len = resp_len;
        memcpy(resp_msg->data, resp_buf, resp_len);

        spin_lock_irqsave(&client->lock, flags);
        list_add_tail(&resp_msg->list, &client->msg_queue);
        client->queue_count++;
        spin_unlock_irqrestore(&client->lock, flags);

        wake_up_interruptible(&client->wait_queue);
    }

    ret = count;

out:
    kfree(msg_buf);
    kfree(resp_buf);
    return ret;
}

static __poll_t qdhcp_chardev_poll(struct file *filp,
                                    struct poll_table_struct *wait)
{
    struct qdhcp_chardev_client *client = filp->private_data;
    __poll_t mask = 0;
    unsigned long flags;

    if (!client)
        return POLLERR;

    poll_wait(filp, &client->wait_queue, wait);

    spin_lock_irqsave(&client->lock, flags);
    if (!list_empty(&client->msg_queue))
        mask |= POLLIN | POLLRDNORM;
    spin_unlock_irqrestore(&client->lock, flags);

    /* Always writable */
    mask |= POLLOUT | POLLWRNORM;

    return mask;
}

static long qdhcp_chardev_ioctl(struct file *filp, unsigned int cmd,
                                 unsigned long arg)
{
    struct qdhcp_chardev_client *client = filp->private_data;
    unsigned long flags;

    if (!client)
        return -EINVAL;

    switch (cmd) {
    case QDHCP_IOC_SUBSCRIBE:
        spin_lock_irqsave(&client->lock, flags);
        client->subscribed = true;
        spin_unlock_irqrestore(&client->lock, flags);
        pr_debug("qdhcp: chardev client subscribed\n");
        return 0;

    case QDHCP_IOC_UNSUBSCRIBE:
        spin_lock_irqsave(&client->lock, flags);
        client->subscribed = false;
        spin_unlock_irqrestore(&client->lock, flags);
        pr_debug("qdhcp: chardev client unsubscribed\n");
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations qdhcp_chardev_fops = {
    .owner          = THIS_MODULE,
    .open           = qdhcp_chardev_open,
    .release        = qdhcp_chardev_release,
    .read           = qdhcp_chardev_read,
    .write          = qdhcp_chardev_write,
    .poll           = qdhcp_chardev_poll,
    .unlocked_ioctl = qdhcp_chardev_ioctl,
    .compat_ioctl   = qdhcp_chardev_ioctl,
};

/*
 * Netlink attribute policy
 */
static struct nla_policy qdhcp_genl_policy[__QDHCP_ATTR_MAX] = {
    [QDHCP_ATTR_IFINDEX]    = { .type = NLA_S32 },
    [QDHCP_ATTR_IFNAME]     = { .type = NLA_NUL_STRING, .len = IFNAMSIZ },
    [QDHCP_ATTR_MAC]        = { .type = NLA_BINARY, .len = ETH_ALEN },
    [QDHCP_ATTR_IP]         = { .type = NLA_U32 },
    [QDHCP_ATTR_TRUSTED]    = { .type = NLA_U8 },
    [QDHCP_ATTR_LEASE_TIME] = { .type = NLA_U32 },
    [QDHCP_ATTR_METHOD]     = { .type = NLA_U8 },
    [QDHCP_ATTR_STATS]      = { .type = NLA_BINARY },
    [QDHCP_ATTR_EVENT_TYPE] = { .type = NLA_U8 },
};

/*
 * Netlink command handlers
 */
static int qdhcp_cmd_register(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev)
        return -ENODEV;

    priv = netdev_priv(qdhcp_netdev);
    spin_lock(&priv->lock);
    priv->daemon_registered = true;
    spin_unlock(&priv->lock);

    pr_info("qdhcp: daemon registered\n");
    return 0;
}

static int qdhcp_cmd_unregister(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev)
        return -ENODEV;

    priv = netdev_priv(qdhcp_netdev);
    spin_lock(&priv->lock);
    priv->daemon_registered = false;
    spin_unlock(&priv->lock);

    pr_info("qdhcp: daemon unregistered\n");
    return 0;
}

static int qdhcp_cmd_add_interface(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    int ifindex;
    const char *ifname = "";

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_IFINDEX])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    ifindex = nla_get_s32(info->attrs[QDHCP_ATTR_IFINDEX]);

    if (info->attrs[QDHCP_ATTR_IFNAME])
        ifname = nla_data(info->attrs[QDHCP_ATTR_IFNAME]);

    return iface_add(priv, ifindex, ifname);
}

static int qdhcp_cmd_del_interface(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_IFINDEX])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    return iface_del(priv, nla_get_s32(info->attrs[QDHCP_ATTR_IFINDEX]));
}

static int qdhcp_cmd_set_trusted(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    struct monitored_iface *iface;
    int ifindex;
    unsigned long flags;

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_IFINDEX] ||
        !info->attrs[QDHCP_ATTR_TRUSTED])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    ifindex = nla_get_s32(info->attrs[QDHCP_ATTR_IFINDEX]);

    spin_lock_irqsave(&priv->iface_lock, flags);
    iface = iface_find(priv, ifindex);
    if (!iface) {
        spin_unlock_irqrestore(&priv->iface_lock, flags);
        return -ENOENT;
    }
    iface->trusted = nla_get_u8(info->attrs[QDHCP_ATTR_TRUSTED]) ? true : false;
    spin_unlock_irqrestore(&priv->iface_lock, flags);

    return 0;
}

static int qdhcp_cmd_add_binding(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    int ifindex = 0;
    u32 lease_time = 3600;

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_MAC] ||
        !info->attrs[QDHCP_ATTR_IP])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);

    if (info->attrs[QDHCP_ATTR_IFINDEX])
        ifindex = nla_get_s32(info->attrs[QDHCP_ATTR_IFINDEX]);
    if (info->attrs[QDHCP_ATTR_LEASE_TIME])
        lease_time = nla_get_u32(info->attrs[QDHCP_ATTR_LEASE_TIME]);

    return binding_add(priv,
                       nla_data(info->attrs[QDHCP_ATTR_MAC]),
                       htonl(nla_get_u32(info->attrs[QDHCP_ATTR_IP])),
                       ifindex, lease_time);
}

static int qdhcp_cmd_del_binding(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_MAC])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    return binding_del(priv, nla_data(info->attrs[QDHCP_ATTR_MAC]));
}

static int qdhcp_cmd_clear_bindings(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev)
        return -ENODEV;

    priv = netdev_priv(qdhcp_netdev);
    binding_clear_all(priv);
    return 0;
}

static int qdhcp_cmd_set_method(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;

    if (!qdhcp_netdev || !info->attrs[QDHCP_ATTR_METHOD])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    priv->method = nla_get_u8(info->attrs[QDHCP_ATTR_METHOD]);

    pr_info("qdhcp: method set to 0x%02x\n", priv->method);
    return 0;
}

static int qdhcp_cmd_get_stats(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    struct sk_buff *reply;
    struct qdhcp_stats stats;
    void *hdr;

    if (!qdhcp_netdev)
        return -ENODEV;

    priv = netdev_priv(qdhcp_netdev);

    reply = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
    if (!reply)
        return -ENOMEM;

    hdr = genlmsg_put(reply, info->snd_portid, info->snd_seq,
                      &qdhcp_genl_family, 0, QDHCP_CMD_GET_STATS);
    if (!hdr) {
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    spin_lock(&priv->lock);
    memcpy(&stats, &priv->stats, sizeof(stats));
    stats.binding_count = priv->bind_count;
    spin_unlock(&priv->lock);

    if (nla_put(reply, QDHCP_ATTR_STATS, sizeof(stats), &stats)) {
        genlmsg_cancel(reply, hdr);
        nlmsg_free(reply);
        return -EMSGSIZE;
    }

    genlmsg_end(reply, hdr);
    return genlmsg_reply(reply, info);
}

/* Netlink operations */
static const struct genl_ops qdhcp_genl_ops[] = {
    { .cmd = QDHCP_CMD_REGISTER,       .doit = qdhcp_cmd_register,       .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_UNREGISTER,     .doit = qdhcp_cmd_unregister,     .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_ADD_INTERFACE,  .doit = qdhcp_cmd_add_interface,  .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_DEL_INTERFACE,  .doit = qdhcp_cmd_del_interface,  .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_SET_TRUSTED,    .doit = qdhcp_cmd_set_trusted,    .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_ADD_BINDING,    .doit = qdhcp_cmd_add_binding,    .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_DEL_BINDING,    .doit = qdhcp_cmd_del_binding,    .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_CLEAR_BINDINGS, .doit = qdhcp_cmd_clear_bindings, .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_SET_METHOD,     .doit = qdhcp_cmd_set_method,     .policy = qdhcp_genl_policy },
    { .cmd = QDHCP_CMD_GET_STATS,      .doit = qdhcp_cmd_get_stats,      .policy = qdhcp_genl_policy },
};

static struct genl_family qdhcp_genl_family = {
    .name       = QDHCP_GENL_NAME,
    .version    = QDHCP_GENL_VERSION,
    .maxattr    = __QDHCP_ATTR_MAX - 1,
    .ops        = qdhcp_genl_ops,
    .n_ops      = ARRAY_SIZE(qdhcp_genl_ops),
    .mcgrps     = qdhcp_mcgrps,
    .n_mcgrps   = ARRAY_SIZE(qdhcp_mcgrps),
    .module     = THIS_MODULE,
};

/*
 * Module init/exit
 */
static int __init qdhcp_init(void)
{
    struct qdhcp_priv *priv;
    int ret;

    pr_info("qdhcp: loading (method=0x%02x)\n", intercept_method);

    /* Create network device */
    qdhcp_netdev = alloc_netdev(sizeof(struct qdhcp_priv),
                                 QDHCP_DEV_NAME, NET_NAME_UNKNOWN,
                                 qdhcp_dev_setup);
    if (!qdhcp_netdev)
        return -ENOMEM;

    priv = netdev_priv(qdhcp_netdev);
    priv->netdev = qdhcp_netdev;
    spin_lock_init(&priv->lock);
    spin_lock_init(&priv->bind_lock);
    spin_lock_init(&priv->iface_lock);
    hash_init(priv->bindings);
    priv->method = intercept_method;

    ret = register_netdev(qdhcp_netdev);
    if (ret) {
        pr_err("qdhcp: register_netdev failed: %d\n", ret);
        free_netdev(qdhcp_netdev);
        return ret;
    }

    ret = genl_register_family(&qdhcp_genl_family);
    if (ret) {
        pr_err("qdhcp: genl_register_family failed: %d\n", ret);
        unregister_netdev(qdhcp_netdev);
        return ret;
    }

    /* Register protocol handler (Method 1) */
    if (intercept_method & QDHCP_METHOD_DEV_ADD_PACK) {
        qdhcp_ptype.type = htons(ETH_P_IP);
        qdhcp_ptype.func = ptype_handler;
        qdhcp_ptype.dev = NULL;
        dev_add_pack(&qdhcp_ptype);
        ptype_registered = true;
        pr_info("qdhcp: dev_add_pack registered\n");
    }

    /* Register TC classifier (Method 2) */
    if (intercept_method & QDHCP_METHOD_TC_INGRESS) {
        ret = register_tcf_proto_ops(&qdhcp_tc_ops);
        if (ret < 0)
            pr_warn("qdhcp: TC classifier failed: %d\n", ret);
        else
            pr_info("qdhcp: TC classifier registered\n");
    }

    /* Register char device for alternative transport */
    qdhcp_chardev.minor = MISC_DYNAMIC_MINOR;
    qdhcp_chardev.name = QDHCP_CHARDEV_NAME;
    qdhcp_chardev.fops = &qdhcp_chardev_fops;
    qdhcp_chardev.mode = 0666;

    ret = misc_register(&qdhcp_chardev);
    if (ret < 0) {
        pr_warn("qdhcp: misc_register failed: %d\n", ret);
        /* Non-fatal - continue without chardev */
    } else {
        chardev_registered = true;
        pr_info("qdhcp: char device %s registered\n", QDHCP_CHARDEV_NAME);
    }

    /* Register BDE transport (mmap-based shared memory) */
    ret = qdhcp_bde_init();
    if (ret < 0) {
        pr_warn("qdhcp: BDE transport init failed: %d\n", ret);
        /* Non-fatal - continue without BDE */
    } else {
        pr_info("qdhcp: BDE transport registered\n");
    }

    pr_info("qdhcp: loaded, device %s created\n", QDHCP_DEV_NAME);
    return 0;
}

static void __exit qdhcp_exit(void)
{
    struct qdhcp_priv *priv;

    /* Unregister BDE transport */
    qdhcp_bde_exit();

    /* Unregister char device */
    if (chardev_registered) {
        misc_deregister(&qdhcp_chardev);
        chardev_registered = false;
        pr_info("qdhcp: char device unregistered\n");
    }

    if (intercept_method & QDHCP_METHOD_TC_INGRESS)
        unregister_tcf_proto_ops(&qdhcp_tc_ops);

    if (ptype_registered)
        dev_remove_pack(&qdhcp_ptype);

    genl_unregister_family(&qdhcp_genl_family);

    if (qdhcp_netdev) {
        priv = netdev_priv(qdhcp_netdev);
        binding_clear_all(priv);
        unregister_netdev(qdhcp_netdev);
    }

    pr_info("qdhcp: unloaded\n");
}

module_init(qdhcp_init);
module_exit(qdhcp_exit);
