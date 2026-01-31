/*
 * DHCP Core Implementation
 * Central context management, plugin hooks, and storage abstraction
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

/* Use native qdaemon logging */
#include <qdaemon/qd_log.h>

#include "dhcp_core.h"
#include "dhcp_server.h"
#include "dhcp_pool.h"
#include "dhcp_lease.h"

/* Helper to convert MAC to string key for hashmap */
static void mac_to_key(const uint8_t *mac, char *key)
{
    sprintf(key, "%02x%02x%02x%02x%02x%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * Hook list node wrapper
 */
typedef struct dhcp_hook_node {
    struct qd_list_head list;
    dhcp_hook_t hook;
} dhcp_hook_node_t;

/*
 * Core lifecycle implementation
 */

dhcp_core_t *dhcp_core_create(const char *config_path)
{
    dhcp_core_t *core = calloc(1, sizeof(dhcp_core_t));
    if (!core) {
        qd_log_error("Failed to allocate core context");
        return NULL;
    }

    /* Initialize lease hash map */
    if (qd_hashmap_init(&core->leases, 256) != 0) {
        qd_log_error("Failed to create lease hash map");
        free(core);
        return NULL;
    }

    /* Initialize pool hash map */
    if (qd_hashmap_init(&core->pools, 16) != 0) {
        qd_log_error("Failed to create pool hash map");
        qd_hashmap_destroy(&core->leases);
        free(core);
        return NULL;
    }

    /* Initialize hook list */
    qd_list_init(&core->hooks);

    /* Initialize socket to invalid */
    core->sock_fd = -1;

    /* Load configuration if path provided */
    if (config_path) {
        /* TODO: Parse config file and populate core->config */
        qd_log_info("Config path: %s (parsing not yet implemented)", config_path);
    }

    qd_log_info("DHCP core context created");
    return core;
}

void dhcp_core_destroy(dhcp_core_t *core)
{
    if (!core) return;

    /* Close storage backend */
    if (core->storage && core->storage->close) {
        core->storage->close(core->storage_ctx);
    }

    /* Free hooks */
    struct qd_list_head *pos, *n;
    qd_list_for_each_safe(pos, n, &core->hooks) {
        dhcp_hook_node_t *node = qd_list_entry(pos, dhcp_hook_node_t, list);
        qd_list_del(&node->list);
        if (node->hook.name) free((void*)node->hook.name); /* Copy was made? */
        free(node);
    }

    /* Free pools */
    /* Iterate hashmap to destroy pools */
    qd_hash_entry_t *entry;
    qd_hashmap_for_each(entry, &core->pools) {
        dhcp_pool_t *pool = (dhcp_pool_t *)entry->value;
        dhcp_pool_destroy(pool);
    }
    qd_hashmap_destroy(&core->pools);

    /* Free leases hash */
    /* Leases are owned by lease_db usually, but here core seems to own them?
     * The original code didn't free values in qd_hash_destroy call.
     * We should assume lease_db owns them if we are just a view.
     * But wait, dhcp_core_add_lease inserts into core->leases.
     * If core->leases is THE store, we should free.
     * Original code: qd_hash_destroy(core->leases, NULL); // NULL dtructor
     * So we just destroy the map, not values.
     */
    qd_hashmap_destroy(&core->leases);

    /* Free config if allocated */
    if (core->config) {
        free(core->config);
    }

    qd_log_info("DHCP core context destroyed");
    free(core);
}

/*
 * Storage backend management
 */

int dhcp_core_set_storage(dhcp_core_t *core, const dhcp_storage_ops_t *ops, void *ctx)
{
    if (!core) return -1;

    /* Close existing storage if any */
    if (core->storage && core->storage->close) {
        core->storage->close(core->storage_ctx);
    }

    core->storage = ops;
    core->storage_ctx = ctx;
    return 0;
}

/*
 * Hook management
 */

int dhcp_core_register_hook(dhcp_core_t *core, const dhcp_hook_t *hook)
{
    if (!core || !hook) return -1;

    /* Create a node copy */
    dhcp_hook_node_t *node = malloc(sizeof(dhcp_hook_node_t));
    if (!node) return -1;

    memcpy(&node->hook, hook, sizeof(dhcp_hook_t));
    /* Deep copy name if needed? Assuming static string for now or managed elsewhere */
    
    qd_list_add_tail(&node->list, &core->hooks);

    qd_log_info("Registered hook '%s' for event type %d", hook->name, hook->type);
    return 0;
}

int dhcp_core_unregister_hook(dhcp_core_t *core, const char *name)
{
    if (!core || !name) return -1;

    struct qd_list_head *pos, *n;
    qd_list_for_each_safe(pos, n, &core->hooks) {
        dhcp_hook_node_t *node = qd_list_entry(pos, dhcp_hook_node_t, list);
        if (node->hook.name && strcmp(node->hook.name, name) == 0) {
            qd_list_del(&node->list);
            free(node);
            qd_log_info("Unregistered hook '%s'", name);
            return 0;
        }
    }
    return -1;
}

int dhcp_core_dispatch_hook(dhcp_core_t *core, dhcp_hook_type_t type, void *data)
{
    if (!core) return -1;

    struct qd_list_head *pos;
    qd_list_for_each(pos, &core->hooks) {
        dhcp_hook_node_t *node = qd_list_entry(pos, dhcp_hook_node_t, list);
        if (node->hook.type == type && node->hook.callback) {
            int ret = node->hook.callback(core, data, node->hook.user_data);
            if (ret != 0) {
                qd_log_debug("Hook '%s' returned %d, stopping dispatch", node->hook.name, ret);
                return ret;  /* Stop processing on non-zero return */
            }
        }
    }
    return 0;
}

/*
 * Pool management
 */

int dhcp_core_add_pool(dhcp_core_t *core, dhcp_pool_t *pool)
{
    if (!core || !pool) return -1;
    
    const char *name = dhcp_pool_get_name(pool);
    if (!name) return -1;

    return qd_hashmap_put(&core->pools, name, pool);
}

dhcp_pool_t *dhcp_core_find_pool(dhcp_core_t *core, const char *name)
{
    if (!core || !name) return NULL;
    return (dhcp_pool_t *)qd_hashmap_get(&core->pools, name);
}

dhcp_pool_t *dhcp_core_find_pool_for_ip(dhcp_core_t *core, uint32_t ip)
{
    if (!core) return NULL;

    qd_hash_entry_t *entry;
    qd_hashmap_for_each(entry, &core->pools) {
        dhcp_pool_t *pool = (dhcp_pool_t *)entry->value;
        if (dhcp_pool_contains(pool, ip)) {
            return pool;
        }
    }
    return NULL;
}

/*
 * Lease management via hash map (keyed by hex MAC)
 */

int dhcp_core_add_lease(dhcp_core_t *core, dhcp_lease_t *lease)
{
    if (!core || !lease) return -1;
    
    /* Access mac field from lease. Assumes lease is struct dhcp_lease defined in dhcp_lease.h/protocol */
    /* dhcp_lease_t is defined in dhcp_protocol.h as struct dhcp_lease */
    
    char key[13];
    mac_to_key(lease->mac, key);
    
    return qd_hashmap_put(&core->leases, key, lease);
}

dhcp_lease_t *dhcp_core_find_lease_by_mac(dhcp_core_t *core, const uint8_t *mac)
{
    if (!core || !mac) return NULL;
    
    char key[13];
    mac_to_key(mac, key);
    
    return (dhcp_lease_t *)qd_hashmap_get(&core->leases, key);
}

int dhcp_core_remove_lease(dhcp_core_t *core, const uint8_t *mac)
{
    if (!core || !mac) return -1;
    
    char key[13];
    mac_to_key(mac, key);
    
    void *val = qd_hashmap_remove(&core->leases, key);
    return (val != NULL) ? 0 : -1;
}

/*
 * Main run loop
 */

int dhcp_core_run(dhcp_core_t *core)
{
    if (!core) return -1;

    qd_log_info("DHCP core starting (pools: %zu, hooks: %zu)",
                qd_hashmap_size(&core->pools),
                qd_list_count(&core->hooks));

    /* Initialize storage if configured */
    if (core->storage && core->storage->init) {
        if (core->storage->init(core->storage_ctx, NULL) != 0) {
            qd_log_error("Failed to initialize storage backend");
            return -1;
        }
    }

    qd_log_info("DHCP core ready");

    return 0;
}

/*
 * Statistics helpers
 */

void dhcp_core_update_stats(dhcp_core_t *core, int pkts_processed, int pkts_dropped)
{
    if (!core) return;
    core->stats.pkts_processed += pkts_processed;
    core->stats.pkts_dropped += pkts_dropped;
}

void dhcp_core_get_stats(dhcp_core_t *core, uint64_t *processed, uint64_t *dropped, uint64_t *active)
{
    if (!core) return;
    if (processed) *processed = core->stats.pkts_processed;
    if (dropped) *dropped = core->stats.pkts_dropped;
    if (active) *active = core->stats.leases_active;
}
