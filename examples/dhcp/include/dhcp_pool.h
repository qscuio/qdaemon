/*
 * DHCP IP Address Pool Management API
 * Efficient IP allocation using bitmap for O(1) allocate/release
 */

#ifndef DHCP_POOL_H
#define DHCP_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dhcp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum reservations per pool */
#define DHCP_POOL_MAX_RESERVATIONS  256
#define DHCP_POOL_MAX_EXCLUSIONS    32
#define DHCP_POOL_NAME_MAX          32

/*
 * IP Reservation Entry
 */
typedef struct dhcp_reservation {
    uint32_t ip;                    /* Reserved IP address */
    uint8_t  mac[6];                /* Client MAC address */
    char     hostname[64];          /* Optional hostname */
    bool     active;                /* Reservation active */
} dhcp_reservation_t;

/*
 * IP Exclusion Range
 */
typedef struct dhcp_exclusion {
    uint32_t start;                 /* Start IP */
    uint32_t end;                   /* End IP (inclusive) */
} dhcp_exclusion_t;

/*
 * Pool Configuration
 */
typedef struct dhcp_pool_config {
    char     name[DHCP_POOL_NAME_MAX];
    uint32_t network;               /* Network address */
    uint32_t netmask;               /* Subnet mask */
    uint32_t range_start;           /* First IP in dynamic range */
    uint32_t range_end;             /* Last IP in dynamic range */
    uint32_t gateway;               /* Default router */
    uint32_t dns_servers[4];        /* DNS servers */
    int      num_dns;
    uint32_t ntp_servers[4];        /* NTP servers */
    int      num_ntp;
    char     domain[64];            /* Domain name */
    uint32_t default_lease;         /* Default lease time (seconds) */
    uint32_t max_lease;             /* Maximum lease time (seconds) */
    uint32_t min_lease;             /* Minimum lease time (seconds) */
} dhcp_pool_config_t;

/* Default configuration */
#define DHCP_POOL_CONFIG_DEFAULT { \
    .name = "default",              \
    .default_lease = 3600,          \
    .max_lease = 86400,             \
    .min_lease = 300,               \
}

/*
 * Pool Context (opaque)
 */
typedef struct dhcp_pool dhcp_pool_t;

/*
 * Pool Lifecycle
 */
dhcp_pool_t *dhcp_pool_create(const dhcp_pool_config_t *config);
void dhcp_pool_destroy(dhcp_pool_t *pool);

/*
 * Pool Configuration
 */
int dhcp_pool_set_range(dhcp_pool_t *pool, uint32_t start, uint32_t end);
int dhcp_pool_set_network(dhcp_pool_t *pool, uint32_t network, uint32_t netmask);
int dhcp_pool_set_gateway(dhcp_pool_t *pool, uint32_t gateway);
int dhcp_pool_set_dns(dhcp_pool_t *pool, const uint32_t *servers, int count);
int dhcp_pool_set_domain(dhcp_pool_t *pool, const char *domain);
int dhcp_pool_set_lease_times(dhcp_pool_t *pool, uint32_t def, uint32_t max, uint32_t min);

/*
 * IP Allocation
 */
uint32_t dhcp_pool_allocate(dhcp_pool_t *pool, const uint8_t *mac);
uint32_t dhcp_pool_allocate_specific(dhcp_pool_t *pool, uint32_t ip);
int dhcp_pool_release(dhcp_pool_t *pool, uint32_t ip);
int dhcp_pool_mark_used(dhcp_pool_t *pool, uint32_t ip);
int dhcp_pool_mark_declined(dhcp_pool_t *pool, uint32_t ip);

/*
 * Reservations (Static Assignments)
 */
int dhcp_pool_add_reservation(dhcp_pool_t *pool, uint32_t ip,
                               const uint8_t *mac, const char *hostname);
int dhcp_pool_remove_reservation(dhcp_pool_t *pool, uint32_t ip);
int dhcp_pool_remove_reservation_by_mac(dhcp_pool_t *pool, const uint8_t *mac);
const dhcp_reservation_t *dhcp_pool_find_reservation(dhcp_pool_t *pool,
                                                      const uint8_t *mac);
const dhcp_reservation_t *dhcp_pool_find_reservation_by_ip(dhcp_pool_t *pool,
                                                            uint32_t ip);
int dhcp_pool_get_reservations(dhcp_pool_t *pool,
                                dhcp_reservation_t *out, int max_count);

/*
 * Exclusions (Ranges to Skip)
 */
int dhcp_pool_add_exclusion(dhcp_pool_t *pool, uint32_t start, uint32_t end);
int dhcp_pool_remove_exclusion(dhcp_pool_t *pool, uint32_t start);
int dhcp_pool_clear_exclusions(dhcp_pool_t *pool);

/*
 * Query
 */
bool dhcp_pool_contains(dhcp_pool_t *pool, uint32_t ip);
bool dhcp_pool_is_available(dhcp_pool_t *pool, uint32_t ip);
bool dhcp_pool_is_excluded(dhcp_pool_t *pool, uint32_t ip);
bool dhcp_pool_is_reserved(dhcp_pool_t *pool, uint32_t ip);
bool dhcp_pool_is_allocated(dhcp_pool_t *pool, uint32_t ip);
uint32_t dhcp_pool_get_next_available(dhcp_pool_t *pool);

/*
 * Pool Information
 */
const char *dhcp_pool_get_name(dhcp_pool_t *pool);
const dhcp_pool_config_t *dhcp_pool_get_config(dhcp_pool_t *pool);
void dhcp_pool_get_stats(dhcp_pool_t *pool, dhcp_pool_stats_t *stats);

/*
 * Iteration
 */
typedef int (*dhcp_pool_iter_fn)(uint32_t ip, bool allocated,
                                  bool reserved, void *arg);
void dhcp_pool_iterate(dhcp_pool_t *pool, dhcp_pool_iter_fn fn, void *arg);

/*
 * Utility
 */
uint32_t dhcp_pool_ip_count(dhcp_pool_t *pool);
void dhcp_pool_reset(dhcp_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* DHCP_POOL_H */
