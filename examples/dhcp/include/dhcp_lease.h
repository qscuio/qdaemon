/*
 * DHCP Lease Database API
 * Hash table based lease storage with timer integration for expiration
 */

#ifndef DHCP_LEASE_H
#define DHCP_LEASE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dhcp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hash table size (must be power of 2) */
#define DHCP_LEASE_HASH_SIZE    256

/* Maximum leases */
#define DHCP_LEASE_MAX          4096

/*
 * Forward declarations
 */
typedef struct dhcp_lease_db dhcp_lease_db_t;
typedef struct qd_timer_wheel qd_timer_wheel_t;  /* From qdaemon */

/*
 * Lease Expiration Callback
 */
typedef void (*dhcp_lease_expire_cb_t)(dhcp_lease_t *lease, void *arg);

/*
 * Lease Database Configuration
 */
typedef struct dhcp_lease_db_config {
    uint32_t offer_timeout;         /* Offer validity in seconds */
    uint32_t cleanup_interval;      /* Expired lease cleanup interval */
    bool     enable_persistence;    /* Save/restore leases */
    const char *persistence_path;   /* Lease file path */
} dhcp_lease_db_config_t;

#define DHCP_LEASE_DB_CONFIG_DEFAULT { \
    .offer_timeout = 60,               \
    .cleanup_interval = 300,           \
    .enable_persistence = false,       \
    .persistence_path = NULL,          \
}

/*
 * Lease Database Lifecycle
 */
dhcp_lease_db_t *dhcp_lease_db_create(const dhcp_lease_db_config_t *config);
dhcp_lease_db_t *dhcp_lease_db_create_with_timer(const dhcp_lease_db_config_t *config,
                                                  qd_timer_wheel_t *wheel);
void dhcp_lease_db_destroy(dhcp_lease_db_t *db);

/*
 * Lease Creation/Deletion
 */
dhcp_lease_t *dhcp_lease_create(dhcp_lease_db_t *db, uint32_t ip,
                                 const uint8_t *mac, uint32_t lease_time);
dhcp_lease_t *dhcp_lease_create_offered(dhcp_lease_db_t *db, uint32_t ip,
                                         const uint8_t *mac, uint32_t xid);
int dhcp_lease_delete(dhcp_lease_db_t *db, dhcp_lease_t *lease);
int dhcp_lease_delete_by_ip(dhcp_lease_db_t *db, uint32_t ip);
int dhcp_lease_delete_by_mac(dhcp_lease_db_t *db, const uint8_t *mac);

/*
 * Lease Lookup
 */
dhcp_lease_t *dhcp_lease_find_by_mac(dhcp_lease_db_t *db, const uint8_t *mac);
dhcp_lease_t *dhcp_lease_find_by_ip(dhcp_lease_db_t *db, uint32_t ip);
dhcp_lease_t *dhcp_lease_find_by_client_id(dhcp_lease_db_t *db,
                                            const uint8_t *client_id, size_t len);
dhcp_lease_t *dhcp_lease_find_by_xid(dhcp_lease_db_t *db, uint32_t xid);

/*
 * Lease State Management
 */
int dhcp_lease_bind(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t lease_time);
int dhcp_lease_release(dhcp_lease_db_t *db, dhcp_lease_t *lease);
int dhcp_lease_decline(dhcp_lease_db_t *db, dhcp_lease_t *lease);
int dhcp_lease_renew(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t new_lease_time);
int dhcp_lease_expire(dhcp_lease_db_t *db, dhcp_lease_t *lease);

/*
 * Table-Based State Machine
 *
 * The state machine uses a 2D transition table indexed by [current_state][event].
 * Each cell contains:
 *   - next_state: The state to transition to (or DHCP_LEASE_INVALID for no transition)
 *   - action: Optional action function to call during transition
 *   - flags: Transition flags (e.g., log, notify)
 */

/* Number of states and events for table dimensions */
#define DHCP_LEASE_NUM_STATES   6   /* FREE, OFFERED, BOUND, EXPIRED, DECLINED, RESERVED */
#define DHCP_LEASE_NUM_EVENTS   11  /* DISCOVER, OFFER, REQUEST, ACK, NAK, RELEASE, DECLINE, EXPIRE, TIMEOUT, RENEW, REBIND */

/* Invalid state marker for "no transition" */
#define DHCP_LEASE_INVALID      ((dhcp_lease_state_t)-1)

/* Transition action function type */
typedef int (*dhcp_lease_action_fn)(dhcp_lease_db_t *db, dhcp_lease_t *lease, void *ctx);

/* State entry/exit action function type */
typedef void (*dhcp_lease_state_action_fn)(dhcp_lease_db_t *db, dhcp_lease_t *lease);

/* Transition flags */
typedef enum {
    DHCP_TRANS_NONE     = 0,
    DHCP_TRANS_LOG      = 0x01,   /* Log this transition */
    DHCP_TRANS_NOTIFY   = 0x02,   /* Notify callback */
    DHCP_TRANS_PERSIST  = 0x04,   /* Persist lease after transition */
} dhcp_trans_flags_t;

/* State transition table entry */
typedef struct dhcp_lease_trans {
    dhcp_lease_state_t   next_state;    /* Next state (DHCP_LEASE_INVALID = no transition) */
    dhcp_lease_action_fn action;        /* Action to execute during transition */
    dhcp_trans_flags_t   flags;         /* Transition flags */
} dhcp_lease_trans_t;

/* State descriptor */
typedef struct dhcp_lease_state_desc {
    const char               *name;         /* State name for debugging */
    dhcp_lease_state_action_fn on_enter;    /* Called when entering state */
    dhcp_lease_state_action_fn on_exit;     /* Called when leaving state */
} dhcp_lease_state_desc_t;

/*
 * State Machine API
 */

/* Get the state transition table (for inspection/debugging) */
const dhcp_lease_trans_t *dhcp_lease_get_trans_table(void);

/* Get state descriptor */
const dhcp_lease_state_desc_t *dhcp_lease_get_state_desc(dhcp_lease_state_t state);

/* Execute state transition with context */
int dhcp_lease_transition_ex(dhcp_lease_db_t *db, dhcp_lease_t *lease,
                              dhcp_event_t event, void *ctx);

/*
 * State Transition (Simple API)
 */
int dhcp_lease_transition(dhcp_lease_db_t *db, dhcp_lease_t *lease, dhcp_event_t event);
bool dhcp_lease_can_transition(dhcp_lease_t *lease, dhcp_event_t event);
dhcp_lease_state_t dhcp_lease_next_state(dhcp_lease_state_t current, dhcp_event_t event);

/* Debug helper - get event name */
const char *dhcp_event_name(dhcp_event_t event);

/*
 * Lease Update
 */
int dhcp_lease_set_hostname(dhcp_lease_t *lease, const char *hostname);
int dhcp_lease_set_client_id(dhcp_lease_t *lease, const uint8_t *client_id, size_t len);
int dhcp_lease_update_time(dhcp_lease_db_t *db, dhcp_lease_t *lease, uint32_t new_lease_time);

/*
 * Lease Query
 */
bool dhcp_lease_is_expired(const dhcp_lease_t *lease);
bool dhcp_lease_is_valid(const dhcp_lease_t *lease);
uint64_t dhcp_lease_remaining_time(const dhcp_lease_t *lease);
int dhcp_lease_count(dhcp_lease_db_t *db);
int dhcp_lease_count_by_state(dhcp_lease_db_t *db, dhcp_lease_state_t state);

/*
 * Iteration
 */
typedef int (*dhcp_lease_iter_fn)(dhcp_lease_t *lease, void *arg);
void dhcp_lease_iterate(dhcp_lease_db_t *db, dhcp_lease_iter_fn fn, void *arg);
void dhcp_lease_iterate_by_state(dhcp_lease_db_t *db, dhcp_lease_state_t state,
                                  dhcp_lease_iter_fn fn, void *arg);

/*
 * Expiration Handling
 */
void dhcp_lease_db_set_expire_callback(dhcp_lease_db_t *db,
                                        dhcp_lease_expire_cb_t cb, void *arg);
int dhcp_lease_db_process_expirations(dhcp_lease_db_t *db);
int dhcp_lease_db_cleanup_expired(dhcp_lease_db_t *db);

/*
 * Persistence
 */
int dhcp_lease_db_save(dhcp_lease_db_t *db, const char *path);
int dhcp_lease_db_load(dhcp_lease_db_t *db, const char *path);

/*
 * Utility
 */
void dhcp_lease_db_clear(dhcp_lease_db_t *db);
void dhcp_lease_to_string(const dhcp_lease_t *lease, char *buf, size_t len);

/*
 * Statistics
 */
typedef struct dhcp_lease_db_stats {
    int total_leases;
    int free_leases;
    int offered_leases;
    int bound_leases;
    int expired_leases;
    int declined_leases;
    int reserved_leases;
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
} dhcp_lease_db_stats_t;

void dhcp_lease_db_get_stats(dhcp_lease_db_t *db, dhcp_lease_db_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_LEASE_H */
