/*
 * DHCP Lease Database Implementation
 * Hash table with MAC and IP indexes for O(1) lookup
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <time.h>

#include "dhcp_lease.h"

/* Get current time in milliseconds */
static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Lease Database Structure
 */
struct dhcp_lease_db {
    dhcp_lease_db_config_t config;

    /* Hash table by MAC address */
    dhcp_lease_t *mac_hash[DHCP_LEASE_HASH_SIZE];

    /* Hash table by IP address */
    dhcp_lease_t *ip_hash[DHCP_LEASE_HASH_SIZE];

    /* All leases linked list */
    dhcp_lease_t *all_leases;
    int           lease_count;

    /* Expiration callback */
    dhcp_lease_expire_cb_t expire_cb;
    void                  *expire_cb_arg;

    /* Statistics */
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
};

/*
 * Hash Functions
 */

static uint32_t hash_mac(const uint8_t *mac)
{
    /* Simple DJB2 hash */
    uint32_t hash = 5381;
    for (int i = 0; i < 6; i++) {
        hash = ((hash << 5) + hash) + mac[i];
    }
    return hash & (DHCP_LEASE_HASH_SIZE - 1);
}

/* Reserved for future IP-based hash table optimization */
__attribute__((unused))
static uint32_t hash_ip(uint32_t ip)
{
    /* Mix bits of IP address */
    ip ^= (ip >> 16);
    ip *= 0x85ebca6b;
    ip ^= (ip >> 13);
    ip *= 0xc2b2ae35;
    ip ^= (ip >> 16);
    return ip & (DHCP_LEASE_HASH_SIZE - 1);
}

/*
 * Lifecycle
 */

dhcp_lease_db_t *dhcp_lease_db_create(const dhcp_lease_db_config_t *config)
{
    return dhcp_lease_db_create_with_timer(config, NULL);
}

dhcp_lease_db_t *dhcp_lease_db_create_with_timer(const dhcp_lease_db_config_t *config,
                                                  qd_timer_wheel_t *wheel)
{
    (void)wheel;  /* Timer integration TODO */

    dhcp_lease_db_t *db = calloc(1, sizeof(dhcp_lease_db_t));
    if (!db)
        return NULL;

    if (config) {
        memcpy(&db->config, config, sizeof(dhcp_lease_db_config_t));
    } else {
        dhcp_lease_db_config_t defaults = DHCP_LEASE_DB_CONFIG_DEFAULT;
        memcpy(&db->config, &defaults, sizeof(dhcp_lease_db_config_t));
    }

    return db;
}

void dhcp_lease_db_destroy(dhcp_lease_db_t *db)
{
    if (!db)
        return;

    /* Free all leases */
    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;
        free(lease);
        lease = next;
    }

    free(db);
}

/*
 * Internal: Add lease to hash tables
 */

static void lease_add_to_hashes(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    /* Add to MAC hash */
    uint32_t mac_idx = hash_mac(lease->mac);
    lease->hash_next = db->mac_hash[mac_idx];
    db->mac_hash[mac_idx] = lease;

    /* Add to all leases list */
    lease->next = db->all_leases;
    db->all_leases = lease;
}

/*
 * Note: IP-based hash lookup is done directly in dhcp_lease_find_by_ip
 * via scanning all leases. A separate IP hash could be added for
 * performance if needed, but would require separate hash chain pointers.
 */

/*
 * Internal: Remove lease from hash tables
 */

static void lease_remove_from_hashes(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    /* Remove from MAC hash */
    uint32_t mac_idx = hash_mac(lease->mac);
    dhcp_lease_t **pp = &db->mac_hash[mac_idx];
    while (*pp) {
        if (*pp == lease) {
            *pp = lease->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }

    /* Remove from all leases list */
    dhcp_lease_t **p = &db->all_leases;
    while (*p) {
        if (*p == lease) {
            *p = lease->next;
            break;
        }
        p = &(*p)->next;
    }
}

/*
 * Lease Creation/Deletion
 */

dhcp_lease_t *dhcp_lease_create(dhcp_lease_db_t *db, uint32_t ip,
                                 const uint8_t *mac, uint32_t lease_time)
{
    if (!db || !mac)
        return NULL;

    if (db->lease_count >= DHCP_LEASE_MAX)
        return NULL;

    /* Check for existing lease with same MAC */
    dhcp_lease_t *existing = dhcp_lease_find_by_mac(db, mac);
    if (existing) {
        /* Update existing lease */
        existing->ip = ip;
        existing->lease_time = lease_time;
        existing->start_time = get_time_ms();
        existing->expire_time = existing->start_time + (lease_time * 1000ULL);
        existing->state = DHCP_LEASE_BOUND;
        return existing;
    }

    /* Create new lease */
    dhcp_lease_t *lease = calloc(1, sizeof(dhcp_lease_t));
    if (!lease)
        return NULL;

    lease->ip = ip;
    memcpy(lease->mac, mac, 6);
    lease->lease_time = lease_time;
    lease->start_time = get_time_ms();
    lease->expire_time = lease->start_time + (lease_time * 1000ULL);
    lease->state = DHCP_LEASE_BOUND;

    lease_add_to_hashes(db, lease);
    db->lease_count++;

    return lease;
}

dhcp_lease_t *dhcp_lease_create_offered(dhcp_lease_db_t *db, uint32_t ip,
                                         const uint8_t *mac, uint32_t xid)
{
    if (!db || !mac)
        return NULL;

    /* Check for existing lease with same MAC */
    dhcp_lease_t *existing = dhcp_lease_find_by_mac(db, mac);
    if (existing) {
        /* Update to offered state */
        existing->ip = ip;
        existing->xid = xid;
        existing->offer_time = get_time_ms();
        existing->expire_time = existing->offer_time +
                               (db->config.offer_timeout * 1000ULL);
        existing->state = DHCP_LEASE_OFFERED;
        return existing;
    }

    if (db->lease_count >= DHCP_LEASE_MAX)
        return NULL;

    /* Create new lease in offered state */
    dhcp_lease_t *lease = calloc(1, sizeof(dhcp_lease_t));
    if (!lease)
        return NULL;

    lease->ip = ip;
    memcpy(lease->mac, mac, 6);
    lease->xid = xid;
    lease->offer_time = get_time_ms();
    lease->expire_time = lease->offer_time + (db->config.offer_timeout * 1000ULL);
    lease->state = DHCP_LEASE_OFFERED;

    lease_add_to_hashes(db, lease);
    db->lease_count++;

    return lease;
}

int dhcp_lease_delete(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    if (!db || !lease)
        return -1;

    lease_remove_from_hashes(db, lease);
    db->lease_count--;
    free(lease);

    return 0;
}

int dhcp_lease_delete_by_ip(dhcp_lease_db_t *db, uint32_t ip)
{
    dhcp_lease_t *lease = dhcp_lease_find_by_ip(db, ip);
    if (!lease)
        return -1;
    return dhcp_lease_delete(db, lease);
}

int dhcp_lease_delete_by_mac(dhcp_lease_db_t *db, const uint8_t *mac)
{
    dhcp_lease_t *lease = dhcp_lease_find_by_mac(db, mac);
    if (!lease)
        return -1;
    return dhcp_lease_delete(db, lease);
}

/*
 * Lease Lookup
 */

dhcp_lease_t *dhcp_lease_find_by_mac(dhcp_lease_db_t *db, const uint8_t *mac)
{
    if (!db || !mac)
        return NULL;

    db->lookups++;

    uint32_t idx = hash_mac(mac);
    dhcp_lease_t *lease = db->mac_hash[idx];

    while (lease) {
        if (memcmp(lease->mac, mac, 6) == 0) {
            db->hits++;
            return lease;
        }
        lease = lease->hash_next;
    }

    db->misses++;
    return NULL;
}

dhcp_lease_t *dhcp_lease_find_by_ip(dhcp_lease_db_t *db, uint32_t ip)
{
    if (!db)
        return NULL;

    db->lookups++;

    /* Linear search through all leases (could optimize with IP hash) */
    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        if (lease->ip == ip) {
            db->hits++;
            return lease;
        }
        lease = lease->next;
    }

    db->misses++;
    return NULL;
}

dhcp_lease_t *dhcp_lease_find_by_client_id(dhcp_lease_db_t *db,
                                            const uint8_t *client_id, size_t len)
{
    if (!db || !client_id || len == 0)
        return NULL;

    db->lookups++;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        if (lease->client_id_len == len &&
            memcmp(lease->client_id, client_id, len) == 0) {
            db->hits++;
            return lease;
        }
        lease = lease->next;
    }

    db->misses++;
    return NULL;
}

dhcp_lease_t *dhcp_lease_find_by_xid(dhcp_lease_db_t *db, uint32_t xid)
{
    if (!db)
        return NULL;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        if (lease->xid == xid && lease->state == DHCP_LEASE_OFFERED)
            return lease;
        lease = lease->next;
    }

    return NULL;
}

/*
 * Lease State Management
 */

int dhcp_lease_bind(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t lease_time)
{
    if (!db || !lease)
        return -1;

    if (lease->state != DHCP_LEASE_OFFERED &&
        lease->state != DHCP_LEASE_BOUND)
        return -1;

    lease->lease_time = lease_time;
    lease->start_time = get_time_ms();
    lease->expire_time = lease->start_time + (lease_time * 1000ULL);
    lease->state = DHCP_LEASE_BOUND;

    return 0;
}

int dhcp_lease_release(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    if (!db || !lease)
        return -1;

    lease->state = DHCP_LEASE_FREE;
    lease->expire_time = 0;

    return 0;
}

int dhcp_lease_decline(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    if (!db || !lease)
        return -1;

    lease->state = DHCP_LEASE_DECLINED;

    return 0;
}

int dhcp_lease_renew(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t new_lease_time)
{
    if (!db || !lease)
        return -1;

    if (lease->state != DHCP_LEASE_BOUND)
        return -1;

    lease->lease_time = new_lease_time;
    lease->start_time = get_time_ms();
    lease->expire_time = lease->start_time + (new_lease_time * 1000ULL);

    return 0;
}

int dhcp_lease_expire(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    if (!db || !lease)
        return -1;

    lease->state = DHCP_LEASE_EXPIRED;

    /* Call expiration callback if set */
    if (db->expire_cb)
        db->expire_cb(lease, db->expire_cb_arg);

    return 0;
}

/*
 * =============================================================================
 * Table-Based State Machine Implementation
 * =============================================================================
 *
 * The state machine is implemented using a 2D transition table indexed by
 * [current_state][event]. This provides O(1) lookup for transitions and
 * makes the state machine behavior explicit and verifiable.
 *
 * State Diagram:
 *
 *                    DISCOVER
 *     +-------+  ----------------->  +----------+
 *     | FREE  |                      | OFFERED  |
 *     +-------+  <-----------------  +----------+
 *         ^           TIMEOUT             |
 *         |                               | REQUEST
 *         | RELEASE                       v
 *         |                          +----------+
 *         +------------------------- |  BOUND   |
 *         |                          +----------+
 *         |                               |
 *         |  TIMEOUT                      | EXPIRE
 *         |                               v
 *     +----------+    DISCOVER      +----------+
 *     | DECLINED | <--------------- | EXPIRED  |
 *     +----------+                  +----------+
 *
 * =============================================================================
 */

/*
 * Forward declarations for action functions
 */
static int action_start_offer(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_bind_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_release_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_expire_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_decline_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_renew_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_timeout_offer(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);
static int action_reclaim_expired(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);

/*
 * State entry/exit handlers
 */
static void on_enter_free(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_exit_free(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_enter_offered(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_exit_offered(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_enter_bound(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_exit_bound(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_enter_expired(dhcp_lease_db_t *db, dhcp_lease_t *lease);
static void on_enter_declined(dhcp_lease_db_t *db, dhcp_lease_t *lease);

/*
 * Shorthand macros for transition table entries
 */
#define TRANS(next, act, flg)  { (next), (act), (flg) }
#define NO_TRANS               { DHCP_LEASE_INVALID, NULL, DHCP_TRANS_NONE }
#define TRANS_LOG(next, act)   TRANS(next, act, DHCP_TRANS_LOG)
#define TRANS_PERSIST(next, act) TRANS(next, act, DHCP_TRANS_LOG | DHCP_TRANS_PERSIST)

/*
 * 2D State Transition Table
 *
 * Rows: Current state (DHCP_LEASE_NUM_STATES)
 * Cols: Event (DHCP_LEASE_NUM_EVENTS)
 *
 * Each cell contains: { next_state, action_function, flags }
 */
static const dhcp_lease_trans_t g_trans_table[DHCP_LEASE_NUM_STATES][DHCP_LEASE_NUM_EVENTS] = {
    /* DHCP_LEASE_FREE (0) */
    [DHCP_LEASE_FREE] = {
        [DHCP_EVT_DISCOVER] = TRANS_LOG(DHCP_LEASE_OFFERED, action_start_offer),
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = NO_TRANS,
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = NO_TRANS,
        [DHCP_EVT_DECLINE]  = NO_TRANS,
        [DHCP_EVT_EXPIRE]   = NO_TRANS,
        [DHCP_EVT_TIMEOUT]  = NO_TRANS,
        [DHCP_EVT_RENEW]    = NO_TRANS,
        [DHCP_EVT_REBIND]   = NO_TRANS,
    },

    /* DHCP_LEASE_OFFERED (1) */
    [DHCP_LEASE_OFFERED] = {
        [DHCP_EVT_DISCOVER] = TRANS_LOG(DHCP_LEASE_OFFERED, action_start_offer),  /* Re-offer */
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = TRANS_PERSIST(DHCP_LEASE_BOUND, action_bind_lease),
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = TRANS_LOG(DHCP_LEASE_FREE, action_release_lease),
        [DHCP_EVT_DECLINE]  = TRANS_LOG(DHCP_LEASE_DECLINED, action_decline_lease),
        [DHCP_EVT_EXPIRE]   = NO_TRANS,
        [DHCP_EVT_TIMEOUT]  = TRANS_LOG(DHCP_LEASE_FREE, action_timeout_offer),
        [DHCP_EVT_RENEW]    = NO_TRANS,
        [DHCP_EVT_REBIND]   = NO_TRANS,
    },

    /* DHCP_LEASE_BOUND (2) */
    [DHCP_LEASE_BOUND] = {
        [DHCP_EVT_DISCOVER] = NO_TRANS,  /* Client should use REQUEST */
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = TRANS_PERSIST(DHCP_LEASE_BOUND, action_renew_lease),  /* Renewal */
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = TRANS_PERSIST(DHCP_LEASE_FREE, action_release_lease),
        [DHCP_EVT_DECLINE]  = TRANS_LOG(DHCP_LEASE_DECLINED, action_decline_lease),
        [DHCP_EVT_EXPIRE]   = TRANS_PERSIST(DHCP_LEASE_EXPIRED, action_expire_lease),
        [DHCP_EVT_TIMEOUT]  = NO_TRANS,
        [DHCP_EVT_RENEW]    = TRANS_PERSIST(DHCP_LEASE_BOUND, action_renew_lease),
        [DHCP_EVT_REBIND]   = TRANS_PERSIST(DHCP_LEASE_BOUND, action_renew_lease),
    },

    /* DHCP_LEASE_EXPIRED (3) */
    [DHCP_LEASE_EXPIRED] = {
        [DHCP_EVT_DISCOVER] = TRANS_LOG(DHCP_LEASE_OFFERED, action_start_offer),
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = NO_TRANS,  /* Must go through DISCOVER first */
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = NO_TRANS,
        [DHCP_EVT_DECLINE]  = NO_TRANS,
        [DHCP_EVT_EXPIRE]   = NO_TRANS,
        [DHCP_EVT_TIMEOUT]  = TRANS_LOG(DHCP_LEASE_FREE, action_reclaim_expired),
        [DHCP_EVT_RENEW]    = NO_TRANS,
        [DHCP_EVT_REBIND]   = NO_TRANS,
    },

    /* DHCP_LEASE_DECLINED (4) */
    [DHCP_LEASE_DECLINED] = {
        [DHCP_EVT_DISCOVER] = NO_TRANS,  /* IP is poisoned, don't reuse */
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = NO_TRANS,
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = NO_TRANS,
        [DHCP_EVT_DECLINE]  = NO_TRANS,
        [DHCP_EVT_EXPIRE]   = NO_TRANS,
        [DHCP_EVT_TIMEOUT]  = TRANS_LOG(DHCP_LEASE_FREE, action_reclaim_expired),  /* Admin clear */
        [DHCP_EVT_RENEW]    = NO_TRANS,
        [DHCP_EVT_REBIND]   = NO_TRANS,
    },

    /* DHCP_LEASE_RESERVED (5) */
    [DHCP_LEASE_RESERVED] = {
        [DHCP_EVT_DISCOVER] = TRANS_LOG(DHCP_LEASE_OFFERED, action_start_offer),
        [DHCP_EVT_OFFER]    = NO_TRANS,
        [DHCP_EVT_REQUEST]  = TRANS_PERSIST(DHCP_LEASE_BOUND, action_bind_lease),
        [DHCP_EVT_ACK]      = NO_TRANS,
        [DHCP_EVT_NAK]      = NO_TRANS,
        [DHCP_EVT_RELEASE]  = TRANS_PERSIST(DHCP_LEASE_RESERVED, action_release_lease),  /* Back to reserved */
        [DHCP_EVT_DECLINE]  = NO_TRANS,  /* Reserved IPs can't be declined */
        [DHCP_EVT_EXPIRE]   = TRANS_PERSIST(DHCP_LEASE_RESERVED, action_expire_lease),   /* Back to reserved */
        [DHCP_EVT_TIMEOUT]  = TRANS_LOG(DHCP_LEASE_RESERVED, action_timeout_offer),
        [DHCP_EVT_RENEW]    = TRANS_PERSIST(DHCP_LEASE_BOUND, action_renew_lease),
        [DHCP_EVT_REBIND]   = TRANS_PERSIST(DHCP_LEASE_BOUND, action_renew_lease),
    },
};

/*
 * State Descriptors - metadata for each state
 */
static const dhcp_lease_state_desc_t g_state_descs[DHCP_LEASE_NUM_STATES] = {
    [DHCP_LEASE_FREE] = {
        .name     = "FREE",
        .on_enter = on_enter_free,
        .on_exit  = on_exit_free,
    },
    [DHCP_LEASE_OFFERED] = {
        .name     = "OFFERED",
        .on_enter = on_enter_offered,
        .on_exit  = on_exit_offered,
    },
    [DHCP_LEASE_BOUND] = {
        .name     = "BOUND",
        .on_enter = on_enter_bound,
        .on_exit  = on_exit_bound,
    },
    [DHCP_LEASE_EXPIRED] = {
        .name     = "EXPIRED",
        .on_enter = on_enter_expired,
        .on_exit  = NULL,
    },
    [DHCP_LEASE_DECLINED] = {
        .name     = "DECLINED",
        .on_enter = on_enter_declined,
        .on_exit  = NULL,
    },
    [DHCP_LEASE_RESERVED] = {
        .name     = "RESERVED",
        .on_enter = NULL,
        .on_exit  = NULL,
    },
};

/*
 * Event name lookup table
 */
static const char *g_event_names[DHCP_LEASE_NUM_EVENTS] = {
    [DHCP_EVT_DISCOVER] = "DISCOVER",
    [DHCP_EVT_OFFER]    = "OFFER",
    [DHCP_EVT_REQUEST]  = "REQUEST",
    [DHCP_EVT_ACK]      = "ACK",
    [DHCP_EVT_NAK]      = "NAK",
    [DHCP_EVT_RELEASE]  = "RELEASE",
    [DHCP_EVT_DECLINE]  = "DECLINE",
    [DHCP_EVT_EXPIRE]   = "EXPIRE",
    [DHCP_EVT_TIMEOUT]  = "TIMEOUT",
    [DHCP_EVT_RENEW]    = "RENEW",
    [DHCP_EVT_REBIND]   = "REBIND",
};

/*
 * =============================================================================
 * Action Functions - Execute during state transitions
 * =============================================================================
 */

static int action_start_offer(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;
    (void)ctx;

    lease->offer_time = get_time_ms();
    /* Offer timeout will be set by caller */
    return 0;
}

static int action_bind_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;

    /* ctx may contain lease_time override */
    uint32_t lease_time = ctx ? *(uint32_t *)ctx : lease->lease_time;

    lease->lease_time = lease_time;
    lease->start_time = get_time_ms();
    lease->expire_time = lease->start_time + (lease_time * 1000ULL);

    return 0;
}

static int action_release_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;
    (void)ctx;

    lease->expire_time = 0;
    lease->start_time = 0;

    return 0;
}

static int action_expire_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)ctx;

    /* Notify expiration callback */
    if (db->expire_cb)
        db->expire_cb(lease, db->expire_cb_arg);

    return 0;
}

static int action_decline_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;
    (void)ctx;

    /* Mark IP as problematic - set a long decline timeout */
    lease->expire_time = get_time_ms() + (3600 * 1000ULL);  /* 1 hour decline period */

    return 0;
}

static int action_renew_lease(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;

    /* ctx may contain new lease_time */
    uint32_t lease_time = ctx ? *(uint32_t *)ctx : lease->lease_time;

    lease->lease_time = lease_time;
    lease->start_time = get_time_ms();
    lease->expire_time = lease->start_time + (lease_time * 1000ULL);

    return 0;
}

static int action_timeout_offer(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;
    (void)ctx;

    /* Clear offer-related fields */
    lease->offer_time = 0;
    lease->xid = 0;

    return 0;
}

static int action_reclaim_expired(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx)
{
    (void)db;
    (void)ctx;

    /* Clear all lease data for reuse */
    memset(lease->mac, 0, 6);
    memset(lease->client_id, 0, sizeof(lease->client_id));
    lease->client_id_len = 0;
    lease->hostname[0] = '\0';
    lease->expire_time = 0;
    lease->start_time = 0;
    lease->xid = 0;

    return 0;
}

/*
 * =============================================================================
 * State Entry/Exit Handlers
 * =============================================================================
 */

static void on_enter_free(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    lease->expire_time = 0;
}

static void on_exit_free(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    (void)lease;
    /* Nothing specific to do */
}

static void on_enter_offered(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    lease->offer_time = get_time_ms();
}

static void on_exit_offered(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    (void)lease;
    /* Clear offer timer would be done here if using timers */
}

static void on_enter_bound(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    (void)lease;
    /* Lease timer setup would be done here */
}

static void on_exit_bound(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    (void)lease;
    /* Lease timer cleanup would be done here */
}

static void on_enter_expired(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    if (db->expire_cb)
        db->expire_cb(lease, db->expire_cb_arg);
}

static void on_enter_declined(dhcp_lease_db_t *db, dhcp_lease_t *lease)
{
    (void)db;
    /* Set decline probation period */
    lease->expire_time = get_time_ms() + (3600 * 1000ULL);  /* 1 hour */
}

/*
 * =============================================================================
 * Public State Machine API
 * =============================================================================
 */

const dhcp_lease_trans_t *dhcp_lease_get_trans_table(void)
{
    return &g_trans_table[0][0];
}

const dhcp_lease_state_desc_t *dhcp_lease_get_state_desc(dhcp_lease_state_t state)
{
    if (state >= DHCP_LEASE_NUM_STATES)
        return NULL;
    return &g_state_descs[state];
}

dhcp_lease_state_t dhcp_lease_next_state(dhcp_lease_state_t current, dhcp_event_t event)
{
    if (current >= DHCP_LEASE_NUM_STATES || event >= DHCP_LEASE_NUM_EVENTS)
        return DHCP_LEASE_INVALID;

    return g_trans_table[current][event].next_state;
}

bool dhcp_lease_can_transition(dhcp_lease_t *lease, dhcp_event_t event)
{
    if (!lease || lease->state >= DHCP_LEASE_NUM_STATES || event >= DHCP_LEASE_NUM_EVENTS)
        return false;

    return g_trans_table[lease->state][event].next_state != DHCP_LEASE_INVALID;
}

int dhcp_lease_transition_ex(dhcp_lease_db_t *db, dhcp_lease_t *lease,
                              dhcp_event_t event, void *ctx)
{
    if (!db || !lease)
        return -1;

    if (lease->state >= DHCP_LEASE_NUM_STATES || event >= DHCP_LEASE_NUM_EVENTS)
        return -1;

    /* Look up transition in table */
    const dhcp_lease_trans_t *trans = &g_trans_table[lease->state][event];

    /* Check if transition is valid */
    if (trans->next_state == DHCP_LEASE_INVALID)
        return -1;

    dhcp_lease_state_t old_state = lease->state;
    dhcp_lease_state_t new_state = trans->next_state;

    /* Call exit handler for old state */
    const dhcp_lease_state_desc_t *old_desc = &g_state_descs[old_state];
    if (old_desc->on_exit)
        old_desc->on_exit(db, lease);

    /* Execute transition action */
    if (trans->action) {
        int ret = trans->action(db, lease, ctx);
        if (ret != 0)
            return ret;
    }

    /* Update state */
    lease->state = new_state;

    /* Call entry handler for new state */
    const dhcp_lease_state_desc_t *new_desc = &g_state_descs[new_state];
    if (new_desc->on_enter)
        new_desc->on_enter(db, lease);

    return 0;
}

int dhcp_lease_transition(dhcp_lease_db_t *db, dhcp_lease_t *lease, dhcp_event_t event)
{
    return dhcp_lease_transition_ex(db, lease, event, NULL);
}

/*
 * Debug helper - get event name
 */
const char *dhcp_event_name(dhcp_event_t event)
{
    if (event >= DHCP_LEASE_NUM_EVENTS)
        return "UNKNOWN";
    return g_event_names[event];
}

/*
 * Lease Update
 */

int dhcp_lease_set_hostname(dhcp_lease_t *lease, const char *hostname)
{
    if (!lease)
        return -1;

    if (hostname) {
        strncpy(lease->hostname, hostname, sizeof(lease->hostname) - 1);
        lease->hostname[sizeof(lease->hostname) - 1] = '\0';
    } else {
        lease->hostname[0] = '\0';
    }

    return 0;
}

int dhcp_lease_set_client_id(dhcp_lease_t *lease, const uint8_t *client_id, size_t len)
{
    if (!lease)
        return -1;

    if (client_id && len > 0) {
        if (len > sizeof(lease->client_id))
            len = sizeof(lease->client_id);
        memcpy(lease->client_id, client_id, len);
        lease->client_id_len = len;
    } else {
        lease->client_id_len = 0;
    }

    return 0;
}

int dhcp_lease_update_time(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t new_lease_time)
{
    (void)db;
    if (!lease)
        return -1;

    lease->lease_time = new_lease_time;
    lease->expire_time = lease->start_time + (new_lease_time * 1000ULL);

    return 0;
}

/*
 * Lease Query
 */

bool dhcp_lease_is_expired(const dhcp_lease_t *lease)
{
    if (!lease)
        return true;

    if (lease->state == DHCP_LEASE_FREE ||
        lease->state == DHCP_LEASE_EXPIRED)
        return true;

    uint64_t now = get_time_ms();
    return now >= lease->expire_time;
}

bool dhcp_lease_is_valid(const dhcp_lease_t *lease)
{
    if (!lease)
        return false;

    return (lease->state == DHCP_LEASE_BOUND ||
            lease->state == DHCP_LEASE_OFFERED) &&
           !dhcp_lease_is_expired(lease);
}

uint64_t dhcp_lease_remaining_time(const dhcp_lease_t *lease)
{
    if (!lease || dhcp_lease_is_expired(lease))
        return 0;

    uint64_t now = get_time_ms();
    if (now >= lease->expire_time)
        return 0;

    return (lease->expire_time - now) / 1000;  /* Return seconds */
}

int dhcp_lease_count(dhcp_lease_db_t *db)
{
    if (!db)
        return 0;
    return db->lease_count;
}

int dhcp_lease_count_by_state(dhcp_lease_db_t *db, dhcp_lease_state_t state)
{
    if (!db)
        return 0;

    int count = 0;
    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        if (lease->state == state)
            count++;
        lease = lease->next;
    }

    return count;
}

/*
 * Iteration
 */

void dhcp_lease_iterate(dhcp_lease_db_t *db, dhcp_lease_iter_fn fn, void *arg)
{
    if (!db || !fn)
        return;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;  /* Save in case callback deletes */
        if (fn(lease, arg) != 0)
            break;
        lease = next;
    }
}

void dhcp_lease_iterate_by_state(dhcp_lease_db_t *db, dhcp_lease_state_t state,
                                  dhcp_lease_iter_fn fn, void *arg)
{
    if (!db || !fn)
        return;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;
        if (lease->state == state) {
            if (fn(lease, arg) != 0)
                break;
        }
        lease = next;
    }
}

/*
 * Expiration Handling
 */

void dhcp_lease_db_set_expire_callback(dhcp_lease_db_t *db,
                                        dhcp_lease_expire_cb_t cb, void *arg)
{
    if (!db)
        return;
    db->expire_cb = cb;
    db->expire_cb_arg = arg;
}

int dhcp_lease_db_process_expirations(dhcp_lease_db_t *db)
{
    if (!db)
        return -1;

    int expired_count = 0;
    uint64_t now = get_time_ms();

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;

        if ((lease->state == DHCP_LEASE_BOUND ||
             lease->state == DHCP_LEASE_OFFERED) &&
            now >= lease->expire_time) {
            dhcp_lease_expire(db, lease);
            expired_count++;
        }

        lease = next;
    }

    return expired_count;
}

int dhcp_lease_db_cleanup_expired(dhcp_lease_db_t *db)
{
    if (!db)
        return -1;

    int cleaned = 0;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;

        if (lease->state == DHCP_LEASE_EXPIRED ||
            lease->state == DHCP_LEASE_FREE) {
            dhcp_lease_delete(db, lease);
            cleaned++;
        }

        lease = next;
    }

    return cleaned;
}

/*
 * Persistence
 */

int dhcp_lease_db_save(dhcp_lease_db_t *db, const char *path)
{
    if (!db || !path)
        return -1;

    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    /* Write header */
    fprintf(f, "# DHCP Lease Database\n");
    fprintf(f, "# IP MAC State LeaseTime StartTime ExpireTime Hostname\n");

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        char ip_str[16];
        struct in_addr addr = { .s_addr = lease->ip };
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));

        fprintf(f, "%s %02x:%02x:%02x:%02x:%02x:%02x %d %u %lu %lu %s\n",
                ip_str,
                lease->mac[0], lease->mac[1], lease->mac[2],
                lease->mac[3], lease->mac[4], lease->mac[5],
                lease->state,
                lease->lease_time,
                (unsigned long)lease->start_time,
                (unsigned long)lease->expire_time,
                lease->hostname[0] ? lease->hostname : "-");

        lease = lease->next;
    }

    fclose(f);
    return 0;
}

int dhcp_lease_db_load(dhcp_lease_db_t *db, const char *path)
{
    if (!db || !path)
        return -1;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[512];
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;

        char ip_str[16], mac_str[18], hostname[64];
        int state;
        uint32_t lease_time;
        unsigned long start_time, expire_time;

        int n = sscanf(line, "%15s %17s %d %u %lu %lu %63s",
                       ip_str, mac_str, &state, &lease_time,
                       &start_time, &expire_time, hostname);

        if (n < 6)
            continue;

        /* Parse IP */
        struct in_addr addr;
        if (inet_pton(AF_INET, ip_str, &addr) != 1)
            continue;

        /* Parse MAC */
        uint8_t mac[6];
        if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6)
            continue;

        /* Create lease */
        dhcp_lease_t *lease = dhcp_lease_create(db, addr.s_addr, mac, lease_time);
        if (lease) {
            lease->state = state;
            lease->start_time = start_time;
            lease->expire_time = expire_time;
            if (n >= 7 && strcmp(hostname, "-") != 0) {
                dhcp_lease_set_hostname(lease, hostname);
            }
            loaded++;
        }
    }

    fclose(f);
    return loaded;
}

/*
 * Utility
 */

void dhcp_lease_db_clear(dhcp_lease_db_t *db)
{
    if (!db)
        return;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;
        free(lease);
        lease = next;
    }

    memset(db->mac_hash, 0, sizeof(db->mac_hash));
    memset(db->ip_hash, 0, sizeof(db->ip_hash));
    db->all_leases = NULL;
    db->lease_count = 0;
}

void dhcp_lease_to_string(const dhcp_lease_t *lease, char *buf, size_t len)
{
    if (!lease || !buf || len == 0)
        return;

    char ip_str[16];
    struct in_addr addr = { .s_addr = lease->ip };
    inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));

    snprintf(buf, len, "IP=%s MAC=%02x:%02x:%02x:%02x:%02x:%02x State=%s "
             "Remaining=%lus Host=%s",
             ip_str,
             lease->mac[0], lease->mac[1], lease->mac[2],
             lease->mac[3], lease->mac[4], lease->mac[5],
             dhcp_lease_state_name(lease->state),
             (unsigned long)dhcp_lease_remaining_time(lease),
             lease->hostname[0] ? lease->hostname : "(none)");
}

/*
 * Statistics
 */

void dhcp_lease_db_get_stats(dhcp_lease_db_t *db, dhcp_lease_db_stats_t *stats)
{
    if (!db || !stats)
        return;

    memset(stats, 0, sizeof(*stats));

    stats->total_leases = db->lease_count;
    stats->lookups = db->lookups;
    stats->hits = db->hits;
    stats->misses = db->misses;

    dhcp_lease_t *lease = db->all_leases;
    while (lease) {
        switch (lease->state) {
        case DHCP_LEASE_FREE:     stats->free_leases++; break;
        case DHCP_LEASE_OFFERED:  stats->offered_leases++; break;
        case DHCP_LEASE_BOUND:    stats->bound_leases++; break;
        case DHCP_LEASE_EXPIRED:  stats->expired_leases++; break;
        case DHCP_LEASE_DECLINED: stats->declined_leases++; break;
        case DHCP_LEASE_RESERVED: stats->reserved_leases++; break;
        }
        lease = lease->next;
    }
}
