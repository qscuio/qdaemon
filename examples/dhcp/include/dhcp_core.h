/*
 * dhcp_core.h - Core Data Structures and Interfaces for qd_dhcpd
 *
 * This header defines the central data structures and architectural interfaces
 * for the high-performance DHCP server (qd_dhcpd). It serves as the bridge
 * between the network I/O layer, the storage backend, and the lease management
 * logic.
 */

#ifndef DHCP_CORE_H
#define DHCP_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Include DHCP protocol definitions for types */
#include "dhcp_protocol.h"

/* Include native qdaemon data structures */
#include <qd_hash.h>
#include <qd_list.h>

/**
 * dhcp_storage_ops_t - Storage Backend Interface
 */
typedef struct dhcp_storage_ops_s {
    const char *name;
    int (*init)(void *ctx, const char *conn_str);
    int (*get_lease)(void *ctx, const uint8_t *mac, uint8_t mac_len, dhcp_lease_t **out_lease);
    int (*save_lease)(void *ctx, const dhcp_lease_t *lease);
    int (*delete_lease)(void *ctx, const uint8_t *mac, uint8_t mac_len);
    void (*close)(void *ctx);
} dhcp_storage_ops_t;

/**
 * dhcp_hook_t - Event Hook Interface
 */
typedef enum {
    DHCP_HOOK_RX_PACKET,   /* Raw packet received */
    DHCP_HOOK_DISCOVER,    /* Valid DISCOVER processed */
    DHCP_HOOK_REQUEST,     /* Valid REQUEST processed */
    DHCP_HOOK_LEASE_NEW,   /* New lease assigned */
    DHCP_HOOK_LEASE_EXPIRE /* Lease expired */
} dhcp_hook_type_t;

typedef struct dhcp_hook_s {
    const char *name;
    dhcp_hook_type_t type;
    int (*callback)(void *core_ctx, void *packet_or_lease, void *user_data);
    void *user_data;
} dhcp_hook_t;

/**
 * dhcp_core_t - Central System Context
 */
typedef struct dhcp_core_s {
    /* Global Configuration */
    dhcp_config_t *config;

    /* Lease Management
     * ----------------
     * leases: Hash map mapping MAC addresses (as hex strings) to dhcp_lease_t*.
     */
    qd_hashmap_t leases;

    /* IP Address Pools
     * ----------------
     * pools: Hash map mapping Pool Names to dhcp_pool_t*.
     */
    qd_hashmap_t pools;

    /* Storage Subsystem */
    const dhcp_storage_ops_t *storage;
    void *storage_ctx;

    /* Hook Registry
     * -------------
     * List of registered hooks (wrapped in internal nodes).
     */
    struct qd_list_head hooks;

    /* Runtime Statistics */
    struct {
        uint64_t pkts_processed;
        uint64_t pkts_dropped;
        uint64_t leases_active;
    } stats;

    /* Interface socket reference */
    int sock_fd;

} dhcp_core_t;

/* Forward declaration for pool type */
#ifndef DHCP_POOL_H
struct dhcp_pool;
typedef struct dhcp_pool dhcp_pool_t;
#endif

/* Core Lifecycle API */
dhcp_core_t *dhcp_core_create(const char *config_path);
void dhcp_core_destroy(dhcp_core_t *core);
int dhcp_core_run(dhcp_core_t *core);

/* Storage Backend API */
int dhcp_core_set_storage(dhcp_core_t *core, const dhcp_storage_ops_t *ops, void *ctx);

/* Hook Management API */
int dhcp_core_register_hook(dhcp_core_t *core, const dhcp_hook_t *hook);
int dhcp_core_unregister_hook(dhcp_core_t *core, const char *name);
int dhcp_core_dispatch_hook(dhcp_core_t *core, dhcp_hook_type_t type, void *data);

/* Pool Management API */
int dhcp_core_add_pool(dhcp_core_t *core, dhcp_pool_t *pool);
dhcp_pool_t *dhcp_core_find_pool(dhcp_core_t *core, const char *name);
dhcp_pool_t *dhcp_core_find_pool_for_ip(dhcp_core_t *core, uint32_t ip);

/* Lease Management API */
int dhcp_core_add_lease(dhcp_core_t *core, dhcp_lease_t *lease);
dhcp_lease_t *dhcp_core_find_lease_by_mac(dhcp_core_t *core, const uint8_t *mac);
int dhcp_core_remove_lease(dhcp_core_t *core, const uint8_t *mac);

/* Statistics API */
void dhcp_core_update_stats(dhcp_core_t *core, int pkts_processed, int pkts_dropped);
void dhcp_core_get_stats(dhcp_core_t *core, uint64_t *processed, uint64_t *dropped, uint64_t *active);

/* File storage backend */
extern const dhcp_storage_ops_t dhcp_storage_file_ops;

#endif /* DHCP_CORE_H */
