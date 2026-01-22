/*
 * QDaemon DHCP Kernel Module - Custom Network Device
 * Virtual network device for DHCP packet capture/injection
 * Uses netlink to communicate with userspace daemon
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/genetlink.h>
#include <net/netlink.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QDaemon");
MODULE_DESCRIPTION("DHCP Virtual Network Device with Netlink Interface");
MODULE_VERSION("1.0");

#define QDHCP_DEV_NAME       "qdhcp0"
#define QDHCP_GENL_NAME      "QDHCP"
#define QDHCP_GENL_VERSION   1
#define QDHCP_MC_GROUP       "packets"

/* Netlink commands */
enum {
    QDHCP_CMD_UNSPEC,
    QDHCP_CMD_REGISTER,      /* Register userspace daemon */
    QDHCP_CMD_UNREGISTER,    /* Unregister daemon */
    QDHCP_CMD_PACKET_UP,     /* Packet from kernel to user */
    QDHCP_CMD_PACKET_DOWN,   /* Packet from user to kernel */
    QDHCP_CMD_SET_CONFIG,    /* Configure device */
    QDHCP_CMD_GET_STATS,     /* Get statistics */
    __QDHCP_CMD_MAX,
};
#define QDHCP_CMD_MAX (__QDHCP_CMD_MAX - 1)

/* Netlink attributes */
enum {
    QDHCP_ATTR_UNSPEC,
    QDHCP_ATTR_PACKET,       /* Raw packet data */
    QDHCP_ATTR_IFINDEX,      /* Interface index */
    QDHCP_ATTR_TIMESTAMP,    /* Packet timestamp */
    QDHCP_ATTR_MAC_SRC,      /* Source MAC */
    QDHCP_ATTR_MAC_DST,      /* Destination MAC */
    QDHCP_ATTR_PKT_TYPE,     /* Packet type (DHCP msg type) */
    QDHCP_ATTR_MTU,          /* MTU setting */
    QDHCP_ATTR_STATS,        /* Statistics */
    __QDHCP_ATTR_MAX,
};
#define QDHCP_ATTR_MAX (__QDHCP_ATTR_MAX - 1)

/* Statistics structure */
struct qdhcp_stats {
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_bytes;
    u64 tx_bytes;
    u64 rx_dropped;
    u64 tx_dropped;
    u64 dhcp_discover;
    u64 dhcp_offer;
    u64 dhcp_request;
    u64 dhcp_ack;
    u64 dhcp_nak;
    u64 dhcp_release;
};

/* Per-device private data */
struct qdhcp_priv {
    struct net_device *netdev;
    struct qdhcp_stats stats;
    u32 daemon_pid;           /* PID of registered daemon */
    u32 daemon_portid;        /* Netlink port ID */
    spinlock_t lock;
    bool daemon_registered;
};

/* Global state */
static struct net_device *qdhcp_netdev;
static struct genl_family qdhcp_genl_family;

/* Netlink attribute policy */
static struct nla_policy qdhcp_genl_policy[QDHCP_ATTR_MAX + 1] = {
    [QDHCP_ATTR_PACKET]    = { .type = NLA_BINARY, .len = 1500 },
    [QDHCP_ATTR_IFINDEX]   = { .type = NLA_U32 },
    [QDHCP_ATTR_TIMESTAMP] = { .type = NLA_U64 },
    [QDHCP_ATTR_MAC_SRC]   = { .type = NLA_BINARY, .len = ETH_ALEN },
    [QDHCP_ATTR_MAC_DST]   = { .type = NLA_BINARY, .len = ETH_ALEN },
    [QDHCP_ATTR_PKT_TYPE]  = { .type = NLA_U8 },
    [QDHCP_ATTR_MTU]       = { .type = NLA_U32 },
    [QDHCP_ATTR_STATS]     = { .type = NLA_BINARY },
};

/* Multicast group */
static struct genl_multicast_group qdhcp_genl_mcgrps[] = {
    { .name = QDHCP_MC_GROUP, },
};

/*
 * Forward packets to userspace daemon via netlink
 */
static int qdhcp_forward_to_user(struct net_device *dev, struct sk_buff *skb)
{
    struct qdhcp_priv *priv = netdev_priv(dev);
    struct sk_buff *nlskb;
    void *hdr;
    ktime_t now;

    if (!priv->daemon_registered)
        return -ENOENT;

    nlskb = genlmsg_new(NLMSG_GOODSIZE + skb->len, GFP_ATOMIC);
    if (!nlskb)
        return -ENOMEM;

    hdr = genlmsg_put(nlskb, 0, 0, &qdhcp_genl_family, 0, QDHCP_CMD_PACKET_UP);
    if (!hdr) {
        nlmsg_free(nlskb);
        return -EMSGSIZE;
    }

    /* Add packet data */
    if (nla_put(nlskb, QDHCP_ATTR_PACKET, skb->len, skb->data))
        goto nla_fail;

    /* Add metadata */
    if (nla_put_u32(nlskb, QDHCP_ATTR_IFINDEX, dev->ifindex))
        goto nla_fail;

    now = ktime_get_real();
    if (nla_put_u64_64bit(nlskb, QDHCP_ATTR_TIMESTAMP, ktime_to_ns(now), 0))
        goto nla_fail;

    /* Add source MAC from ethernet header */
    if (skb->len >= ETH_HLEN) {
        struct ethhdr *eth = (struct ethhdr *)skb->data;
        if (nla_put(nlskb, QDHCP_ATTR_MAC_SRC, ETH_ALEN, eth->h_source))
            goto nla_fail;
        if (nla_put(nlskb, QDHCP_ATTR_MAC_DST, ETH_ALEN, eth->h_dest))
            goto nla_fail;
    }

    genlmsg_end(nlskb, hdr);

    /* Send to daemon */
    return genlmsg_unicast(dev_net(dev), nlskb, priv->daemon_portid);

nla_fail:
    genlmsg_cancel(nlskb, hdr);
    nlmsg_free(nlskb);
    return -EMSGSIZE;
}

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

static netdev_tx_t qdhcp_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct qdhcp_priv *priv = netdev_priv(dev);
    int ret;

    spin_lock(&priv->lock);
    priv->stats.tx_packets++;
    priv->stats.tx_bytes += skb->len;
    spin_unlock(&priv->lock);

    /* Forward to userspace */
    ret = qdhcp_forward_to_user(dev, skb);
    if (ret < 0) {
        spin_lock(&priv->lock);
        priv->stats.tx_dropped++;
        spin_unlock(&priv->lock);
    }

    /* Free the skb - we've copied the data */
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
    if (new_mtu < 68 || new_mtu > 1500)
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

    /* Generate random MAC */
    eth_hw_addr_random(dev);

    /* Set flags */
    dev->flags |= IFF_NOARP;
    dev->features |= NETIF_F_HW_CSUM;
}

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
    priv->daemon_pid = info->snd_portid;
    priv->daemon_portid = info->snd_portid;
    priv->daemon_registered = true;
    spin_unlock(&priv->lock);

    pr_info("qdhcp: daemon registered (portid=%u)\n", info->snd_portid);
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
    priv->daemon_portid = 0;
    priv->daemon_pid = 0;
    spin_unlock(&priv->lock);

    pr_info("qdhcp: daemon unregistered\n");
    return 0;
}

/* Handle packet from userspace to inject into network */
static int qdhcp_cmd_packet_down(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    struct sk_buff *txskb;
    void *pkt_data;
    int pkt_len;

    if (!qdhcp_netdev)
        return -ENODEV;

    if (!info->attrs[QDHCP_ATTR_PACKET])
        return -EINVAL;

    priv = netdev_priv(qdhcp_netdev);
    pkt_data = nla_data(info->attrs[QDHCP_ATTR_PACKET]);
    pkt_len = nla_len(info->attrs[QDHCP_ATTR_PACKET]);

    /* Allocate skb and copy packet data */
    txskb = netdev_alloc_skb(qdhcp_netdev, pkt_len + NET_IP_ALIGN);
    if (!txskb)
        return -ENOMEM;

    skb_reserve(txskb, NET_IP_ALIGN);
    skb_put_data(txskb, pkt_data, pkt_len);

    /* Set up for reception */
    txskb->dev = qdhcp_netdev;
    txskb->protocol = eth_type_trans(txskb, qdhcp_netdev);
    txskb->ip_summed = CHECKSUM_UNNECESSARY;

    spin_lock(&priv->lock);
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += pkt_len;
    spin_unlock(&priv->lock);

    /* Hand to network stack */
    netif_rx(txskb);

    return 0;
}

static int qdhcp_cmd_get_stats(struct sk_buff *skb, struct genl_info *info)
{
    struct qdhcp_priv *priv;
    struct sk_buff *reply;
    void *hdr;
    struct qdhcp_stats stats;

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
    {
        .cmd = QDHCP_CMD_REGISTER,
        .flags = 0,
        .policy = qdhcp_genl_policy,
        .doit = qdhcp_cmd_register,
    },
    {
        .cmd = QDHCP_CMD_UNREGISTER,
        .flags = 0,
        .policy = qdhcp_genl_policy,
        .doit = qdhcp_cmd_unregister,
    },
    {
        .cmd = QDHCP_CMD_PACKET_DOWN,
        .flags = 0,
        .policy = qdhcp_genl_policy,
        .doit = qdhcp_cmd_packet_down,
    },
    {
        .cmd = QDHCP_CMD_GET_STATS,
        .flags = 0,
        .policy = qdhcp_genl_policy,
        .doit = qdhcp_cmd_get_stats,
    },
};

/* Generic netlink family */
static struct genl_family qdhcp_genl_family = {
    .name = QDHCP_GENL_NAME,
    .version = QDHCP_GENL_VERSION,
    .maxattr = QDHCP_ATTR_MAX,
    .ops = qdhcp_genl_ops,
    .n_ops = ARRAY_SIZE(qdhcp_genl_ops),
    .mcgrps = qdhcp_genl_mcgrps,
    .n_mcgrps = ARRAY_SIZE(qdhcp_genl_mcgrps),
    .module = THIS_MODULE,
};

/*
 * Module init/exit
 */

static int __init qdhcp_init(void)
{
    struct qdhcp_priv *priv;
    int ret;

    /* Create network device */
    qdhcp_netdev = alloc_netdev(sizeof(struct qdhcp_priv),
                                 QDHCP_DEV_NAME, NET_NAME_UNKNOWN,
                                 qdhcp_dev_setup);
    if (!qdhcp_netdev) {
        pr_err("qdhcp: failed to allocate netdev\n");
        return -ENOMEM;
    }

    priv = netdev_priv(qdhcp_netdev);
    priv->netdev = qdhcp_netdev;
    spin_lock_init(&priv->lock);

    /* Register network device */
    ret = register_netdev(qdhcp_netdev);
    if (ret) {
        pr_err("qdhcp: failed to register netdev: %d\n", ret);
        free_netdev(qdhcp_netdev);
        return ret;
    }

    /* Register netlink family */
    ret = genl_register_family(&qdhcp_genl_family);
    if (ret) {
        pr_err("qdhcp: failed to register netlink family: %d\n", ret);
        unregister_netdev(qdhcp_netdev);
        return ret;
    }

    pr_info("qdhcp: module loaded, device %s created\n", QDHCP_DEV_NAME);
    return 0;
}

static void __exit qdhcp_exit(void)
{
    genl_unregister_family(&qdhcp_genl_family);

    if (qdhcp_netdev) {
        unregister_netdev(qdhcp_netdev);
    }

    pr_info("qdhcp: module unloaded\n");
}

module_init(qdhcp_init);
module_exit(qdhcp_exit);
