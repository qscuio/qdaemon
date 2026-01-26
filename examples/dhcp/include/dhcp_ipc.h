/*
 * DHCP IPC Protocol Definitions
 * Message types and structures for CLI and REST API communication
 */

#ifndef DHCP_IPC_H
#define DHCP_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include "dhcp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IPC Socket Path */
#define DHCP_IPC_SOCKET_PATH    "/tmp/qd_dhcpd.sock"

/* IPC Message Magic */
#define DHCP_IPC_MAGIC          0x44484350  /* "DHCP" */

/* IPC Version */
#define DHCP_IPC_VERSION        1

/*
 * IPC Command Types
 */
typedef enum {
    /* Lease commands (100-199) */
    DHCP_IPC_LEASE_LIST = 100,
    DHCP_IPC_LEASE_GET,
    DHCP_IPC_LEASE_CLEAR,
    DHCP_IPC_LEASE_CLEAR_IP,
    DHCP_IPC_LEASE_CLEAR_MAC,
    DHCP_IPC_LEASE_CLEAR_ALL,

    /* Pool commands (200-299) */
    DHCP_IPC_POOL_LIST = 200,
    DHCP_IPC_POOL_GET,
    DHCP_IPC_POOL_ADD,
    DHCP_IPC_POOL_DELETE,
    DHCP_IPC_POOL_STATS,
    DHCP_IPC_POOL_SET_RANGE,
    DHCP_IPC_POOL_SET_GATEWAY,
    DHCP_IPC_POOL_SET_DNS,
    DHCP_IPC_POOL_SET_DOMAIN,
    DHCP_IPC_POOL_ADD_RESERVATION,
    DHCP_IPC_POOL_DEL_RESERVATION,

    /* Server commands (300-399) */
    DHCP_IPC_SERVER_STATUS = 300,
    DHCP_IPC_SERVER_STATS,
    DHCP_IPC_SERVER_CONFIG_GET,
    DHCP_IPC_SERVER_CONFIG_SET,
    DHCP_IPC_SERVER_RELOAD,
    DHCP_IPC_SERVER_START,
    DHCP_IPC_SERVER_STOP,

    /* Relay commands (400-499) */
    DHCP_IPC_RELAY_STATUS = 400,
    DHCP_IPC_RELAY_STATS,
    DHCP_IPC_RELAY_ADD_SERVER,
    DHCP_IPC_RELAY_DEL_SERVER,
    DHCP_IPC_RELAY_ADD_INTERFACE,
    DHCP_IPC_RELAY_DEL_INTERFACE,
    DHCP_IPC_RELAY_SET_OPTION82,
    DHCP_IPC_RELAY_START,
    DHCP_IPC_RELAY_STOP,

    /* Debug commands (500-599) */
    DHCP_IPC_DEBUG_LEVEL = 500,
    DHCP_IPC_DEBUG_PACKET_DUMP,
    DHCP_IPC_DEBUG_STATS_RESET,

    /* Ping/Health (0) */
    DHCP_IPC_PING = 0,

} dhcp_ipc_cmd_t;

/*
 * IPC Status Codes
 */
typedef enum {
    DHCP_IPC_OK = 0,
    DHCP_IPC_ERR_INVALID = -1,
    DHCP_IPC_ERR_NOT_FOUND = -2,
    DHCP_IPC_ERR_EXISTS = -3,
    DHCP_IPC_ERR_FULL = -4,
    DHCP_IPC_ERR_FAILED = -5,
    DHCP_IPC_ERR_TIMEOUT = -6,
} dhcp_ipc_status_t;

/*
 * IPC Message Header
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;             /* DHCP_IPC_MAGIC */
    uint16_t version;           /* DHCP_IPC_VERSION */
    uint16_t cmd;               /* Command type */
    uint32_t seq;               /* Sequence number */
    int32_t  status;            /* Response status (0 = OK) */
    uint32_t payload_len;       /* Payload length */
} dhcp_ipc_header_t;

/*
 * IPC Message
 */
typedef struct {
    dhcp_ipc_header_t header;
    uint8_t           payload[];
} dhcp_ipc_msg_t;

#define DHCP_IPC_MSG_SIZE(payload_len) (sizeof(dhcp_ipc_header_t) + (payload_len))
#define DHCP_IPC_MAX_MSG_SIZE   (64 * 1024)

/*
 * Request/Response Structures
 */

/* Lease list response entry */
typedef struct __attribute__((packed)) {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  state;
    uint8_t  _pad;
    uint32_t lease_time;
    uint64_t start_time;
    uint64_t expire_time;
    char     hostname[64];
} dhcp_ipc_lease_entry_t;

/* Lease list response */
typedef struct __attribute__((packed)) {
    uint32_t count;
    dhcp_ipc_lease_entry_t entries[];
} dhcp_ipc_lease_list_t;

/* Lease clear request */
typedef struct __attribute__((packed)) {
    uint32_t ip;                /* For CLEAR_IP */
    uint8_t  mac[6];            /* For CLEAR_MAC */
} dhcp_ipc_lease_clear_t;

/* Pool entry */
typedef struct __attribute__((packed)) {
    char     name[32];
    uint32_t network;
    uint32_t netmask;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t gateway;
    uint32_t dns_servers[4];
    uint32_t num_dns;
    char     domain[64];
    uint32_t default_lease;
    uint32_t max_lease;
    /* Statistics */
    uint32_t total_ips;
    uint32_t allocated_ips;
    uint32_t available_ips;
    uint32_t reserved_ips;
} dhcp_ipc_pool_entry_t;

/* Pool list response */
typedef struct __attribute__((packed)) {
    uint32_t count;
    dhcp_ipc_pool_entry_t pools[];
} dhcp_ipc_pool_list_t;

/* Pool add request */
typedef struct __attribute__((packed)) {
    char     name[32];
    uint32_t network;
    uint32_t netmask;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t gateway;
    uint32_t dns_servers[4];
    uint32_t num_dns;
    char     domain[64];
    uint32_t default_lease;
} dhcp_ipc_pool_add_t;

/* Pool range request */
typedef struct __attribute__((packed)) {
    char     pool_name[32];
    uint32_t range_start;
    uint32_t range_end;
} dhcp_ipc_pool_range_t;

/* Pool reservation request */
typedef struct __attribute__((packed)) {
    char     pool_name[32];
    uint32_t ip;
    uint8_t  mac[6];
    char     hostname[64];
} dhcp_ipc_pool_reservation_t;

/* Server status response */
typedef struct __attribute__((packed)) {
    uint8_t  running;
    uint8_t  server_enabled;
    uint8_t  relay_enabled;
    uint8_t  _pad;
    uint32_t server_id;
    uint32_t uptime;
    uint32_t num_pools;
    uint32_t num_leases;
    /* Statistics */
    uint64_t discovers;
    uint64_t offers;
    uint64_t requests;
    uint64_t acks;
    uint64_t naks;
    uint64_t releases;
    uint64_t declines;
} dhcp_ipc_server_status_t;

/* Relay status response */
typedef struct __attribute__((packed)) {
    uint8_t  enabled;
    uint8_t  num_servers;
    uint8_t  num_interfaces;
    uint8_t  option82_insert;
    uint8_t  option82_replace;
    uint8_t  _pad[3];
    uint32_t servers[DHCP_RELAY_MAX_SERVERS];
    /* Interface list */
    struct __attribute__((packed)) {
        char     name[32];
        uint32_t giaddr;
        uint8_t  enabled;
        uint8_t  _pad[3];
    } interfaces[DHCP_RELAY_MAX_IFACES];
    /* Statistics */
    uint64_t requests_forwarded;
    uint64_t replies_relayed;
    uint64_t drops_max_hops;
    uint64_t drops_no_server;
} dhcp_ipc_relay_status_t;

/* Relay server add/del request */
typedef struct __attribute__((packed)) {
    uint32_t server_ip;
} dhcp_ipc_relay_server_t;

/* Relay interface add request */
typedef struct __attribute__((packed)) {
    char     name[32];
    uint32_t giaddr;
} dhcp_ipc_relay_interface_t;

/* Debug level request */
typedef struct __attribute__((packed)) {
    uint32_t level;             /* 0-7 */
} dhcp_ipc_debug_level_t;

/*
 * Helper Functions
 */

static inline void dhcp_ipc_init_msg(dhcp_ipc_msg_t *msg, dhcp_ipc_cmd_t cmd, uint32_t seq)
{
    msg->header.magic = DHCP_IPC_MAGIC;
    msg->header.version = DHCP_IPC_VERSION;
    msg->header.cmd = cmd;
    msg->header.seq = seq;
    msg->header.status = 0;
    msg->header.payload_len = 0;
}

static inline bool dhcp_ipc_validate_msg(const dhcp_ipc_msg_t *msg)
{
    return msg &&
           msg->header.magic == DHCP_IPC_MAGIC &&
           msg->header.version == DHCP_IPC_VERSION;
}

static inline const char *dhcp_ipc_status_str(dhcp_ipc_status_t status)
{
    switch (status) {
    case DHCP_IPC_OK:           return "OK";
    case DHCP_IPC_ERR_INVALID:  return "Invalid request";
    case DHCP_IPC_ERR_NOT_FOUND:return "Not found";
    case DHCP_IPC_ERR_EXISTS:   return "Already exists";
    case DHCP_IPC_ERR_FULL:     return "Resource full";
    case DHCP_IPC_ERR_FAILED:   return "Operation failed";
    case DHCP_IPC_ERR_TIMEOUT:  return "Timeout";
    default:                    return "Unknown error";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* DHCP_IPC_H */
