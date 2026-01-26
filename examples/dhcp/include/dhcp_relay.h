/*
 * DHCP Relay Agent API (RFC 2131, RFC 3046)
 * Implements DHCP relay with Option 82 support
 */

#ifndef DHCP_RELAY_H
#define DHCP_RELAY_H

#include <stdint.h>
#include <stdbool.h>
#include <qdaemon/qd_event.h>
#include "dhcp_protocol.h"
#include "dhcp_option.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward declarations
 */
typedef struct dhcp_relay dhcp_relay_t;

/*
 * Relay States
 */
typedef enum {
    DHCP_RELAY_IDLE = 0,        /* Listening for packets */
    DHCP_RELAY_FORWARDING,      /* Forwarding request to server */
    DHCP_RELAY_RELAYING,        /* Relaying reply to client */
} dhcp_relay_state_t;

/*
 * Circuit ID Type (how to format circuit-id sub-option)
 */
typedef enum {
    DHCP_CIRCUIT_ID_IFNAME = 0, /* Interface name */
    DHCP_CIRCUIT_ID_IFINDEX,    /* Interface index */
    DHCP_CIRCUIT_ID_VLAN,       /* VLAN ID */
    DHCP_CIRCUIT_ID_CUSTOM,     /* Custom format */
} dhcp_circuit_id_type_t;

/*
 * Remote ID Type (how to format remote-id sub-option)
 */
typedef enum {
    DHCP_REMOTE_ID_MAC = 0,     /* Relay MAC address */
    DHCP_REMOTE_ID_HOSTNAME,    /* Relay hostname */
    DHCP_REMOTE_ID_CUSTOM,      /* Custom format */
} dhcp_remote_id_type_t;

/*
 * Relay Interface
 */
typedef struct dhcp_relay_iface {
    char        name[32];       /* Interface name */
    int         ifindex;        /* Interface index */
    uint32_t    giaddr;         /* Gateway address for this interface */
    bool        enabled;        /* Relay enabled */
    uint64_t    rx_packets;     /* Packets received */
    uint64_t    tx_packets;     /* Packets transmitted */
} dhcp_relay_iface_t;

/*
 * Relay Configuration
 */
typedef struct dhcp_relay_config {
    uint32_t    server_addrs[DHCP_RELAY_MAX_SERVERS];
    int         num_servers;
    uint32_t    response_timeout;   /* Timeout in seconds */
    int         max_hops;           /* Maximum hop count */
    bool        insert_option82;    /* Add relay agent info */
    bool        replace_option82;   /* Replace existing option 82 */
    dhcp_circuit_id_type_t circuit_id_type;
    dhcp_remote_id_type_t  remote_id_type;
    uint8_t     custom_circuit_id[64];
    size_t      custom_circuit_id_len;
    uint8_t     custom_remote_id[64];
    size_t      custom_remote_id_len;
} dhcp_relay_config_t;

#define DHCP_RELAY_CONFIG_DEFAULT { \
    .response_timeout = 4,          \
    .max_hops = DHCP_RELAY_MAX_HOPS,\
    .insert_option82 = true,        \
    .replace_option82 = false,      \
    .circuit_id_type = DHCP_CIRCUIT_ID_IFNAME, \
    .remote_id_type = DHCP_REMOTE_ID_MAC,      \
}

/*
 * Packet send callback
 */
typedef int (*dhcp_relay_send_fn)(const uint8_t *packet, size_t len,
                                   uint32_t dst_ip, uint16_t dst_port,
                                   int ifindex, void *arg);

/*
 * Relay Lifecycle
 */
dhcp_relay_t *dhcp_relay_create(const dhcp_relay_config_t *config);
void dhcp_relay_destroy(dhcp_relay_t *relay);
int dhcp_relay_start(dhcp_relay_t *relay, qd_event_loop_t *loop);
void dhcp_relay_stop(dhcp_relay_t *relay);

/*
 * Configuration
 */
int dhcp_relay_add_server(dhcp_relay_t *relay, uint32_t server_ip);
int dhcp_relay_remove_server(dhcp_relay_t *relay, uint32_t server_ip);
int dhcp_relay_clear_servers(dhcp_relay_t *relay);
int dhcp_relay_get_servers(dhcp_relay_t *relay, uint32_t *servers, int max_count);

int dhcp_relay_add_interface(dhcp_relay_t *relay, const char *ifname, uint32_t giaddr);
int dhcp_relay_remove_interface(dhcp_relay_t *relay, const char *ifname);
const dhcp_relay_iface_t *dhcp_relay_get_interface(dhcp_relay_t *relay, const char *ifname);
int dhcp_relay_get_interfaces(dhcp_relay_t *relay, dhcp_relay_iface_t *ifaces, int max_count);

int dhcp_relay_set_send_callback(dhcp_relay_t *relay, dhcp_relay_send_fn fn, void *arg);
void dhcp_relay_set_option82(dhcp_relay_t *relay, bool insert, bool replace);
void dhcp_relay_set_max_hops(dhcp_relay_t *relay, int max_hops);

/*
 * Packet Processing
 */
int dhcp_relay_process_client_packet(dhcp_relay_t *relay,
                                      dhcp_packet_t *pkt,
                                      size_t pkt_len,
                                      int ifindex);
int dhcp_relay_process_server_packet(dhcp_relay_t *relay,
                                      dhcp_packet_t *pkt,
                                      size_t pkt_len);

/*
 * Option 82 Handling
 */
int dhcp_relay_add_option82(dhcp_relay_t *relay, dhcp_packet_t *pkt, int ifindex);
int dhcp_relay_strip_option82(dhcp_relay_t *relay, dhcp_packet_t *pkt);

/*
 * Statistics
 */
void dhcp_relay_get_stats(dhcp_relay_t *relay, dhcp_relay_stats_t *stats);
void dhcp_relay_reset_stats(dhcp_relay_t *relay);

/*
 * Utility
 */
const char *dhcp_relay_state_name(dhcp_relay_state_t state);
bool dhcp_relay_is_enabled(dhcp_relay_t *relay);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_RELAY_H */
