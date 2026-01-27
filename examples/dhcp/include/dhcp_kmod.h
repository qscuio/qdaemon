/*
 * DHCP Kernel Module - Shared Header
 * Metadata structure for kernel/userspace packet exchange via raw socket
 */

#ifndef DHCP_KMOD_H
#define DHCP_KMOD_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <linux/types.h>
#include <stddef.h>
#endif

/*
 * Device name
 */
#define QDHCP_DEV_NAME      "qdhcp0"

/*
 * Metadata magic number for validation
 */
#define QDHCP_META_MAGIC    0x51444850  /* "QDHP" */

/*
 * Metadata version
 */
#define QDHCP_META_VERSION  1

/*
 * Interception methods
 */
#define QDHCP_METHOD_DEV_ADD_PACK   0x01
#define QDHCP_METHOD_TC_INGRESS     0x02
#define QDHCP_METHOD_BOTH           0x03

/*
 * Packet direction
 */
#define QDHCP_DIR_RX        0   /* Kernel -> Userspace */
#define QDHCP_DIR_TX        1   /* Userspace -> Kernel */

/*
 * TX flags
 */
#define QDHCP_TX_BROADCAST  0x0001  /* Send as broadcast */
#define QDHCP_TX_UNICAST    0x0002  /* Send as unicast */

/*
 * DHCP Message Types
 */
#define DHCP_TYPE_DISCOVER  1
#define DHCP_TYPE_OFFER     2
#define DHCP_TYPE_REQUEST   3
#define DHCP_TYPE_DECLINE   4
#define DHCP_TYPE_ACK       5
#define DHCP_TYPE_NAK       6
#define DHCP_TYPE_RELEASE   7
#define DHCP_TYPE_INFORM    8

/*
 * Packet Metadata Header
 *
 * This header is prepended to every packet exchanged between
 * kernel and userspace via raw socket on qdhcp0.
 *
 * Format: [qdhcp_meta_hdr][Original Ethernet Frame]
 *
 * RX (Kernel -> Userspace):
 *   - ifindex: source interface where packet was captured
 *   - Userspace reads from raw socket bound to qdhcp0
 *
 * TX (Userspace -> Kernel):
 *   - ifindex: target interface to send packet out
 *   - Userspace writes to raw socket bound to qdhcp0
 */
struct qdhcp_meta_hdr {
    __u32 magic;            /* QDHCP_META_MAGIC */
    __u16 version;          /* QDHCP_META_VERSION */
    __u8  direction;        /* QDHCP_DIR_RX or QDHCP_DIR_TX */
    __u8  msg_type;         /* DHCP message type (1-8) */
    __s32 ifindex;          /* Interface index (source for RX, target for TX) */
    __u16 pkt_len;          /* Length of packet after this header */
    __u16 flags;            /* TX flags */
    __u8  src_mac[6];       /* Source MAC */
    __u8  dst_mac[6];       /* Destination MAC */
    __u8  trusted;          /* 1 if from trusted port (RX only) */
    __u8  method;           /* Interception method (RX only) */
    __u16 vlan_id;          /* VLAN ID (0 if untagged) */
    __u32 _reserved;
} __attribute__((packed));

#define QDHCP_META_HDR_SIZE sizeof(struct qdhcp_meta_hdr)

/*
 * Maximum packet size including metadata
 */
#define QDHCP_MAX_PKT_SIZE  (QDHCP_META_HDR_SIZE + 1518)

/*
 * Helper macros
 */
#define QDHCP_META_VALID(hdr) \
    ((hdr)->magic == QDHCP_META_MAGIC && (hdr)->version == QDHCP_META_VERSION)

/*
 * Netlink family name (for control commands only, not packet data)
 */
#define QDHCP_GENL_NAME     "QDHCP"
#define QDHCP_GENL_VERSION  1

/*
 * Netlink commands (control only)
 */
enum {
    QDHCP_CMD_UNSPEC,
    QDHCP_CMD_REGISTER,         /* Register daemon */
    QDHCP_CMD_UNREGISTER,       /* Unregister daemon */
    QDHCP_CMD_ADD_INTERFACE,    /* Add interface to monitor */
    QDHCP_CMD_DEL_INTERFACE,    /* Remove interface */
    QDHCP_CMD_SET_TRUSTED,      /* Set trusted port */
    QDHCP_CMD_ADD_BINDING,      /* Add binding entry */
    QDHCP_CMD_DEL_BINDING,      /* Delete binding entry */
    QDHCP_CMD_CLEAR_BINDINGS,   /* Clear all bindings */
    QDHCP_CMD_SET_METHOD,       /* Set interception method */
    QDHCP_CMD_GET_STATS,        /* Get statistics */
    QDHCP_CMD_NOTIFY_BINDING,   /* Binding change notification */
    QDHCP_CMD_NOTIFY_IFACE,     /* Interface change notification */
    __QDHCP_CMD_MAX,
};

/*
 * Event types for notifications
 */
enum {
    QDHCP_EVENT_NONE = 0,
    QDHCP_EVENT_BINDING_ADD,    /* Binding was added */
    QDHCP_EVENT_BINDING_DEL,    /* Binding was deleted */
    QDHCP_EVENT_IFACE_ADD,      /* Interface was added */
    QDHCP_EVENT_IFACE_DEL,      /* Interface was deleted */
};

/*
 * Netlink attributes
 */
enum {
    QDHCP_ATTR_UNSPEC,
    QDHCP_ATTR_IFINDEX,         /* Interface index */
    QDHCP_ATTR_IFNAME,          /* Interface name */
    QDHCP_ATTR_MAC,             /* MAC address */
    QDHCP_ATTR_IP,              /* IP address */
    QDHCP_ATTR_TRUSTED,         /* Trusted flag */
    QDHCP_ATTR_LEASE_TIME,      /* Lease time */
    QDHCP_ATTR_METHOD,          /* Interception method */
    QDHCP_ATTR_STATS,           /* Statistics blob */
    QDHCP_ATTR_EVENT_TYPE,      /* Event type for notifications */
    __QDHCP_ATTR_MAX,
};

/*
 * Multicast group name for notifications
 */
#define QDHCP_MCGRP_NAME        "qdhcp_events"

/*
 * Statistics structure
 */
struct qdhcp_stats {
    __u64 rx_packets;
    __u64 tx_packets;
    __u64 rx_bytes;
    __u64 tx_bytes;
    __u64 rx_dropped;
    __u64 tx_dropped;
    __u64 dhcp_discover;
    __u64 dhcp_offer;
    __u64 dhcp_request;
    __u64 dhcp_ack;
    __u64 dhcp_nak;
    __u64 dhcp_release;
    __u64 dhcp_decline;
    __u64 dev_pack_intercepts;
    __u64 tc_intercepts;
    __u64 binding_count;
};

#endif /* DHCP_KMOD_H */
