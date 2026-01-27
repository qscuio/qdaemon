/*
 * DHCP Server Module API
 * State machine based DHCP server implementation
 */

#ifndef DHCP_SERVER_H
#define DHCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#ifdef STANDALONE
#include "qdaemon_stub.h"
#else
#include <qdaemon/qd_handler.h>
#include <qdaemon/qd_event.h>
#endif
#include "dhcp_protocol.h"
#include "dhcp_pool.h"
#include "dhcp_lease.h"
#include "dhcp_option.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum pools per server */
#define DHCP_SERVER_MAX_POOLS   16

/*
 * Forward declarations
 */
typedef struct dhcp_server dhcp_server_t;

/*
 * Server Configuration
 */
typedef struct dhcp_server_config {
    uint32_t server_id;             /* Server identifier (IP) */
    uint32_t default_lease_time;    /* Default lease in seconds */
    uint32_t max_lease_time;        /* Maximum lease in seconds */
    uint32_t min_lease_time;        /* Minimum lease in seconds */
    uint32_t offer_timeout;         /* Offer validity in seconds */
    bool     authoritative;         /* Authoritative for subnets */
    bool     echo_client_id;        /* Echo client-id in replies */
    const char *lease_file;         /* Lease persistence file */
} dhcp_server_config_t;

#define DHCP_SERVER_CONFIG_DEFAULT { \
    .default_lease_time = 3600,      \
    .max_lease_time = 86400,         \
    .min_lease_time = 300,           \
    .offer_timeout = 60,             \
    .authoritative = true,           \
    .echo_client_id = true,          \
    .lease_file = NULL,              \
}

/*
 * Packet send callback
 */
typedef int (*dhcp_server_send_fn)(const uint8_t *packet, size_t len,
                                    const uint8_t *dst_mac, uint32_t dst_ip,
                                    bool broadcast, void *arg);

/*
 * Server Lifecycle
 */
dhcp_server_t *dhcp_server_create(const dhcp_server_config_t *config);
void dhcp_server_destroy(dhcp_server_t *server);
int dhcp_server_start(dhcp_server_t *server, qd_event_loop_t *loop);
void dhcp_server_stop(dhcp_server_t *server);

/*
 * Configuration
 */
int dhcp_server_set_server_id(dhcp_server_t *server, uint32_t ip);
int dhcp_server_set_send_callback(dhcp_server_t *server,
                                   dhcp_server_send_fn fn, void *arg);
const dhcp_server_config_t *dhcp_server_get_config(dhcp_server_t *server);

/*
 * Pool Management
 */
int dhcp_server_add_pool(dhcp_server_t *server, dhcp_pool_t *pool);
int dhcp_server_remove_pool(dhcp_server_t *server, const char *name);
dhcp_pool_t *dhcp_server_find_pool(dhcp_server_t *server, const char *name);
dhcp_pool_t *dhcp_server_find_pool_for_ip(dhcp_server_t *server, uint32_t ip);
int dhcp_server_get_pool_count(dhcp_server_t *server);

/*
 * Lease Management
 */
dhcp_lease_db_t *dhcp_server_get_lease_db(dhcp_server_t *server);
dhcp_lease_t *dhcp_server_find_lease_by_mac(dhcp_server_t *server, const uint8_t *mac);
dhcp_lease_t *dhcp_server_find_lease_by_ip(dhcp_server_t *server, uint32_t ip);
int dhcp_server_clear_lease(dhcp_server_t *server, uint32_t ip);
int dhcp_server_clear_all_leases(dhcp_server_t *server);

/*
 * Packet Processing
 */
int dhcp_server_process_packet(dhcp_server_t *server,
                                const dhcp_packet_t *pkt,
                                const dhcp_rx_info_t *rx_info);

/*
 * Handler Table Integration
 * Returns static handler table for use with qd_handler framework
 */
qd_handler_table_t *dhcp_server_get_handlers(dhcp_server_t *server);

/*
 * Individual Message Handlers
 * Can be used directly or via handler table
 */
qd_handler_result_t dhcp_server_handle_discover(qd_handler_ctx_t *ctx, void *arg);
qd_handler_result_t dhcp_server_handle_request(qd_handler_ctx_t *ctx, void *arg);
qd_handler_result_t dhcp_server_handle_release(qd_handler_ctx_t *ctx, void *arg);
qd_handler_result_t dhcp_server_handle_decline(qd_handler_ctx_t *ctx, void *arg);
qd_handler_result_t dhcp_server_handle_inform(qd_handler_ctx_t *ctx, void *arg);

/*
 * Statistics
 */
void dhcp_server_get_stats(dhcp_server_t *server, dhcp_server_stats_t *stats);
void dhcp_server_reset_stats(dhcp_server_t *server);

/*
 * Utility
 */
int dhcp_server_save_leases(dhcp_server_t *server);
int dhcp_server_load_leases(dhcp_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_SERVER_H */
