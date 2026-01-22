/*
 * QDaemon DHCP Protocol Definitions
 */

#ifndef QD_DHCP_PROTOCOL_H
#define QD_DHCP_PROTOCOL_H

#include <stdint.h>
#include <netinet/in.h>

/* DHCP Ports */
#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

/* DHCP Message Types */
typedef enum {
    DHCP_DISCOVER = 1,
    DHCP_OFFER    = 2,
    DHCP_REQUEST  = 3,
    DHCP_DECLINE  = 4,
    DHCP_ACK      = 5,
    DHCP_NAK      = 6,
    DHCP_RELEASE  = 7,
    DHCP_INFORM   = 8,
} dhcp_msg_type_t;

/* DHCP Operation codes */
#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2

/* DHCP Hardware Types */
#define DHCP_HTYPE_ETHER    1

/* DHCP Options */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN         15
#define DHCP_OPT_BROADCAST      28
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_RENEWAL_TIME   58
#define DHCP_OPT_REBIND_TIME    59
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

/* DHCP Magic Cookie */
#define DHCP_MAGIC_COOKIE       0x63825363

/* DHCP Packet Structure */
#pragma pack(push, 1)
typedef struct dhcp_packet {
    uint8_t  op;                /* Operation: 1=request, 2=reply */
    uint8_t  htype;             /* Hardware type */
    uint8_t  hlen;              /* Hardware address length */
    uint8_t  hops;              /* Hops */
    uint32_t xid;               /* Transaction ID */
    uint16_t secs;              /* Seconds elapsed */
    uint16_t flags;             /* Flags */
    uint32_t ciaddr;            /* Client IP address */
    uint32_t yiaddr;            /* Your IP address */
    uint32_t siaddr;            /* Server IP address */
    uint32_t giaddr;            /* Gateway IP address */
    uint8_t  chaddr[16];        /* Client hardware address */
    uint8_t  sname[64];         /* Server name */
    uint8_t  file[128];         /* Boot file name */
    uint32_t magic;             /* Magic cookie: 0x63825363 */
    uint8_t  options[308];      /* Options */
} dhcp_packet_t;
#pragma pack(pop)

/* Ethernet/IP/UDP headers for raw packet construction */
#pragma pack(push, 1)
typedef struct eth_header {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} eth_header_t;

typedef struct ip_header {
    uint8_t  ihl_version;
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
} ip_header_t;

typedef struct udp_header {
    uint16_t source;
    uint16_t dest;
    uint16_t len;
    uint16_t check;
} udp_header_t;
#pragma pack(pop)

/* Lease entry */
typedef struct dhcp_lease {
    uint32_t ip;                /* Leased IP address */
    uint8_t  mac[6];            /* Client MAC */
    uint32_t lease_time;        /* Lease duration (seconds) */
    uint64_t expire_time;       /* Expiration timestamp */
    char     hostname[64];      /* Client hostname */
    int      state;             /* Lease state */
    struct dhcp_lease *next;
} dhcp_lease_t;

/* Lease states */
#define LEASE_FREE      0
#define LEASE_OFFERED   1
#define LEASE_BOUND     2
#define LEASE_EXPIRED   3

/* DHCP Server Configuration */
typedef struct dhcp_config {
    uint32_t server_ip;         /* Server IP address */
    uint32_t subnet_mask;       /* Subnet mask */
    uint32_t gateway;           /* Default gateway */
    uint32_t dns_server;        /* DNS server */
    uint32_t pool_start;        /* IP pool start */
    uint32_t pool_end;          /* IP pool end */
    uint32_t default_lease;     /* Default lease time */
    uint32_t max_lease;         /* Maximum lease time */
    char     domain[64];        /* Domain name */
} dhcp_config_t;

/* Helper macros */
#define DHCP_OPTION_LEN(pkt, offset) ((pkt)->options[(offset) + 1])
#define DHCP_OPTION_DATA(pkt, offset) (&(pkt)->options[(offset) + 2])

/* Function prototypes */
static inline int dhcp_get_msg_type(const dhcp_packet_t *pkt)
{
    const uint8_t *opt = pkt->options;
    int i = 0;

    while (i < 308 && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_MSG_TYPE) {
            return opt[i + 2];
        }
        i += 2 + opt[i + 1];
    }
    return -1;
}

static inline uint32_t dhcp_get_requested_ip(const dhcp_packet_t *pkt)
{
    const uint8_t *opt = pkt->options;
    int i = 0;

    while (i < 308 && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_REQUESTED_IP && opt[i + 1] >= 4) {
            return *(uint32_t *)&opt[i + 2];
        }
        i += 2 + opt[i + 1];
    }
    return 0;
}

#endif /* QD_DHCP_PROTOCOL_H */
