/*
 * QDaemon DHCP Protocol Definitions
 * RFC 2131 - Dynamic Host Configuration Protocol
 * RFC 3046 - DHCP Relay Agent Information Option
 */

#ifndef QD_DHCP_PROTOCOL_H
#define QD_DHCP_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    DHCP_FORCERENEW = 9,      /* RFC 3203 */
    DHCP_LEASEQUERY = 10,     /* RFC 4388 */
    DHCP_LEASEUNASSIGNED = 11,
    DHCP_LEASEUNKNOWN = 12,
    DHCP_LEASEACTIVE = 13,
} dhcp_msg_type_t;

/* DHCP Operation codes */
#define DHCP_BOOTREQUEST    1
#define DHCP_BOOTREPLY      2

/* DHCP Hardware Types */
#define DHCP_HTYPE_ETHER    1
#define DHCP_HTYPE_IEEE802  6
#define DHCP_HTYPE_FDDI     8

/* DHCP Flags */
#define DHCP_FLAG_BROADCAST 0x8000

/* DHCP Options - Standard (RFC 2132) */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_TIME_OFFSET    2
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_TIME_SERVER    4
#define DHCP_OPT_NAME_SERVER    5
#define DHCP_OPT_DNS            6
#define DHCP_OPT_LOG_SERVER     7
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN         15
#define DHCP_OPT_INTERFACE_MTU  26
#define DHCP_OPT_BROADCAST      28
#define DHCP_OPT_NIS_DOMAIN     40
#define DHCP_OPT_NIS_SERVERS    41
#define DHCP_OPT_NTP_SERVERS    42
#define DHCP_OPT_VENDOR_SPECIFIC 43
#define DHCP_OPT_NETBIOS_NS     44
#define DHCP_OPT_NETBIOS_DD     45
#define DHCP_OPT_NETBIOS_NODE   46
#define DHCP_OPT_NETBIOS_SCOPE  47
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_OVERLOAD       52
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_MESSAGE        56
#define DHCP_OPT_MAX_MSG_SIZE   57
#define DHCP_OPT_RENEWAL_TIME   58   /* T1 */
#define DHCP_OPT_REBIND_TIME    59   /* T2 */
#define DHCP_OPT_VENDOR_CLASS   60
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_TFTP_SERVER    66
#define DHCP_OPT_BOOTFILE       67

/* DHCP Options - Relay Agent (RFC 3046) */
#define DHCP_OPT_RELAY_AGENT    82

/* Relay Agent Information Sub-options (RFC 3046) */
#define DHCP_RAI_CIRCUIT_ID     1    /* Agent Circuit ID */
#define DHCP_RAI_REMOTE_ID      2    /* Agent Remote ID */
#define DHCP_RAI_LINK_SELECTION 5    /* Link Selection (RFC 3527) */
#define DHCP_RAI_SUBSCRIBER_ID  6    /* Subscriber ID (RFC 3993) */
#define DHCP_RAI_RADIUS         7    /* RADIUS Attributes (RFC 4014) */
#define DHCP_RAI_AUTH           8    /* Authentication (RFC 4030) */
#define DHCP_RAI_VSS            151  /* Virtual Subnet Selection (RFC 6607) */

/* DHCP Options - Other */
#define DHCP_OPT_CLASSLESS_ROUTE 121 /* RFC 3442 */
#define DHCP_OPT_END            255

/* DHCP Magic Cookie */
#define DHCP_MAGIC_COOKIE       0x63825363

/* Maximum sizes */
#define DHCP_MAX_OPTION_LEN     255
#define DHCP_MAX_OPTIONS_SIZE   308
#define DHCP_MIN_PACKET_SIZE    300
#define DHCP_MAX_PACKET_SIZE    576   /* Minimum MTU for IP */
#define DHCP_CHADDR_SIZE        16
#define DHCP_SNAME_SIZE         64
#define DHCP_FILE_SIZE          128

/* Relay agent limits */
#define DHCP_RELAY_MAX_HOPS     16
#define DHCP_RELAY_MAX_SERVERS  8
#define DHCP_RELAY_MAX_IFACES   16
#define DHCP_OPTION82_MAX_LEN   255

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

/*
 * Lease States - State machine for DHCP lease lifecycle
 */
typedef enum {
    DHCP_LEASE_FREE = 0,        /* IP available for allocation */
    DHCP_LEASE_OFFERED,         /* OFFER sent, awaiting REQUEST */
    DHCP_LEASE_BOUND,           /* ACK sent, lease active */
    DHCP_LEASE_EXPIRED,         /* Lease time exceeded */
    DHCP_LEASE_DECLINED,        /* Client declined (IP conflict) */
    DHCP_LEASE_RESERVED,        /* Static reservation */
} dhcp_lease_state_t;

/* Legacy macros for compatibility */
#define LEASE_FREE      DHCP_LEASE_FREE
#define LEASE_OFFERED   DHCP_LEASE_OFFERED
#define LEASE_BOUND     DHCP_LEASE_BOUND
#define LEASE_EXPIRED   DHCP_LEASE_EXPIRED

/*
 * DHCP Events - Triggers for state transitions
 */
typedef enum {
    DHCP_EVT_DISCOVER = 0,      /* DISCOVER received */
    DHCP_EVT_OFFER,             /* OFFER sent */
    DHCP_EVT_REQUEST,           /* REQUEST received */
    DHCP_EVT_ACK,               /* ACK sent */
    DHCP_EVT_NAK,               /* NAK sent */
    DHCP_EVT_RELEASE,           /* RELEASE received */
    DHCP_EVT_DECLINE,           /* DECLINE received */
    DHCP_EVT_EXPIRE,            /* Lease expired */
    DHCP_EVT_TIMEOUT,           /* Offer/other timeout */
    DHCP_EVT_RENEW,             /* Renewal request */
    DHCP_EVT_REBIND,            /* Rebind request */
} dhcp_event_t;

/*
 * Lease Entry - Forward declaration (full def in dhcp_lease.h)
 */
typedef struct dhcp_lease dhcp_lease_t;

/* Basic lease structure for backward compatibility */
struct dhcp_lease {
    uint32_t ip;                /* Leased IP address */
    uint8_t  mac[6];            /* Client MAC */
    uint8_t  client_id[64];     /* Client identifier */
    size_t   client_id_len;     /* Client ID length */
    uint32_t lease_time;        /* Lease duration (seconds) */
    uint64_t start_time;        /* Lease start timestamp (ms) */
    uint64_t expire_time;       /* Expiration timestamp (ms) */
    uint64_t offer_time;        /* Offer sent timestamp (ms) */
    char     hostname[64];      /* Client hostname */
    dhcp_lease_state_t state;   /* Current state */
    uint32_t xid;               /* Transaction ID */
    struct dhcp_lease *next;    /* Linked list next */
    struct dhcp_lease *hash_next; /* Hash table chain */
};

/*
 * Relay Circuit - Tracks relay agent session (RFC 3046)
 */
typedef struct dhcp_relay_circuit {
    uint32_t xid;               /* Transaction ID */
    uint8_t  client_mac[6];     /* Client hardware address */
    uint32_t giaddr;            /* Gateway IP (relay interface) */
    uint32_t server_ip;         /* Target DHCP server */
    int      client_ifindex;    /* Interface client is on */
    uint64_t created_time;      /* Circuit creation timestamp */
    uint8_t  option82[DHCP_OPTION82_MAX_LEN]; /* Relay agent info */
    size_t   option82_len;      /* Option 82 length */
    struct dhcp_relay_circuit *next;
} dhcp_relay_circuit_t;

/*
 * DHCP Server Configuration - Basic pool settings
 */
typedef struct dhcp_config {
    uint32_t server_ip;         /* Server IP address */
    uint32_t subnet_mask;       /* Subnet mask */
    uint32_t gateway;           /* Default gateway */
    uint32_t dns_server;        /* Primary DNS server */
    uint32_t dns_server2;       /* Secondary DNS server */
    uint32_t ntp_server;        /* NTP server */
    uint32_t pool_start;        /* IP pool start */
    uint32_t pool_end;          /* IP pool end */
    uint32_t default_lease;     /* Default lease time (seconds) */
    uint32_t max_lease;         /* Maximum lease time (seconds) */
    uint32_t min_lease;         /* Minimum lease time (seconds) */
    uint32_t offer_timeout;     /* Offer validity (seconds) */
    uint32_t renewal_time;      /* T1 - renewal time */
    uint32_t rebind_time;       /* T2 - rebind time */
    char     domain[64];        /* Domain name */
    bool     authoritative;     /* Authoritative for subnet */
} dhcp_config_t;

/*
 * Relay Agent Configuration - defined in dhcp_relay.h
 * Forward declaration for use in dhcp_protocol.h
 */
struct dhcp_relay_config;

/*
 * Relay Interface Configuration
 */
typedef struct dhcp_relay_interface {
    char     name[32];          /* Interface name */
    uint32_t giaddr;            /* Gateway address for this interface */
    int      ifindex;           /* Interface index */
    bool     enabled;           /* Relay enabled on interface */
} dhcp_relay_interface_t;

/*
 * Packet Reception Info - Metadata from kernel
 */
typedef struct dhcp_rx_info {
    int      ifindex;           /* Receiving interface */
    uint8_t  src_mac[6];        /* Source MAC address */
    uint8_t  dst_mac[6];        /* Destination MAC address */
    uint32_t src_ip;            /* Source IP */
    uint32_t dst_ip;            /* Destination IP */
    uint16_t src_port;          /* Source port */
    uint16_t dst_port;          /* Destination port */
    uint64_t timestamp;         /* Reception timestamp (ns) */
    bool     is_broadcast;      /* Received via broadcast */
} dhcp_rx_info_t;

/*
 * Server Statistics
 */
typedef struct dhcp_server_stats {
    uint64_t discovers;
    uint64_t offers;
    uint64_t requests;
    uint64_t acks;
    uint64_t naks;
    uint64_t declines;
    uint64_t releases;
    uint64_t informs;
    uint64_t unknown;
    uint64_t errors;
    uint64_t packets_in;
    uint64_t packets_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
} dhcp_server_stats_t;

/*
 * Relay Statistics
 */
typedef struct dhcp_relay_stats {
    uint64_t requests_forwarded;
    uint64_t replies_relayed;
    uint64_t drops_bad_giaddr;
    uint64_t drops_max_hops;
    uint64_t drops_timeout;
    uint64_t drops_no_server;
    uint64_t option82_added;
    uint64_t option82_stripped;
} dhcp_relay_stats_t;

/*
 * Pool Statistics
 */
typedef struct dhcp_pool_stats {
    uint32_t total_ips;
    uint32_t allocated_ips;
    uint32_t available_ips;
    uint32_t reserved_ips;
    uint32_t excluded_ips;
    uint32_t declined_ips;
} dhcp_pool_stats_t;

/* Helper macros */
#define DHCP_OPTION_LEN(pkt, offset) ((pkt)->options[(offset) + 1])
#define DHCP_OPTION_DATA(pkt, offset) (&(pkt)->options[(offset) + 2])

/*
 * State name lookup
 */
static inline const char *dhcp_lease_state_name(dhcp_lease_state_t state)
{
    switch (state) {
        case DHCP_LEASE_FREE:     return "FREE";
        case DHCP_LEASE_OFFERED:  return "OFFERED";
        case DHCP_LEASE_BOUND:    return "BOUND";
        case DHCP_LEASE_EXPIRED:  return "EXPIRED";
        case DHCP_LEASE_DECLINED: return "DECLINED";
        case DHCP_LEASE_RESERVED: return "RESERVED";
        default:                  return "UNKNOWN";
    }
}

static inline const char *dhcp_msg_type_name(dhcp_msg_type_t type)
{
    switch (type) {
        case DHCP_DISCOVER:    return "DISCOVER";
        case DHCP_OFFER:       return "OFFER";
        case DHCP_REQUEST:     return "REQUEST";
        case DHCP_DECLINE:     return "DECLINE";
        case DHCP_ACK:         return "ACK";
        case DHCP_NAK:         return "NAK";
        case DHCP_RELEASE:     return "RELEASE";
        case DHCP_INFORM:      return "INFORM";
        case DHCP_FORCERENEW:  return "FORCERENEW";
        case DHCP_LEASEQUERY:  return "LEASEQUERY";
        default:               return "UNKNOWN";
    }
}

/*
 * Option parsing helpers
 */
static inline int dhcp_get_msg_type(const dhcp_packet_t *pkt)
{
    const uint8_t *opt = pkt->options;
    int i = 0;

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_MSG_TYPE && opt[i + 1] >= 1) {
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

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_REQUESTED_IP && opt[i + 1] >= 4) {
            uint32_t ip;
            memcpy(&ip, &opt[i + 2], 4);
            return ip;
        }
        i += 2 + opt[i + 1];
    }
    return 0;
}

static inline uint32_t dhcp_get_server_id(const dhcp_packet_t *pkt)
{
    const uint8_t *opt = pkt->options;
    int i = 0;

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_SERVER_ID && opt[i + 1] >= 4) {
            uint32_t ip;
            memcpy(&ip, &opt[i + 2], 4);
            return ip;
        }
        i += 2 + opt[i + 1];
    }
    return 0;
}

static inline int dhcp_get_option(const dhcp_packet_t *pkt, uint8_t code,
                                   void *buf, size_t buflen)
{
    const uint8_t *opt = pkt->options;
    int i = 0;

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == code) {
            size_t len = opt[i + 1];
            if (buf && len <= buflen) {
                memcpy(buf, &opt[i + 2], len);
            }
            return (int)len;
        }
        i += 2 + opt[i + 1];
    }
    return -1;
}

/*
 * Check if packet has relay agent info (Option 82)
 */
static inline bool dhcp_has_option82(const dhcp_packet_t *pkt)
{
    return dhcp_get_option(pkt, DHCP_OPT_RELAY_AGENT, NULL, 0) > 0;
}

/*
 * MAC address formatting
 */
static inline void dhcp_mac_to_str(const uint8_t *mac, char *buf, size_t len)
{
    if (len >= 18) {
        snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* QD_DHCP_PROTOCOL_H */
