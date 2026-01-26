/*
 * DHCP IP Address Pool Management Implementation
 * Uses bitmap for efficient O(1) allocation and release
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "dhcp_pool.h"

/* Bitmap word type */
typedef uint64_t bitmap_word_t;
#define BITS_PER_WORD (sizeof(bitmap_word_t) * 8)

/*
 * Pool Structure
 */
struct dhcp_pool {
    dhcp_pool_config_t config;

    /* Bitmap for IP allocation status */
    bitmap_word_t *bitmap;          /* 1 = allocated/unavailable, 0 = free */
    size_t        bitmap_size;      /* Size in words */
    uint32_t      pool_size;        /* Total IPs in range */

    /* Quick allocation hint */
    uint32_t      next_free_hint;   /* Offset from range_start */

    /* Reservations */
    dhcp_reservation_t reservations[DHCP_POOL_MAX_RESERVATIONS];
    int                num_reservations;

    /* Exclusions */
    dhcp_exclusion_t   exclusions[DHCP_POOL_MAX_EXCLUSIONS];
    int                num_exclusions;

    /* Statistics */
    uint32_t      allocated_count;
    uint32_t      declined_count;
};

/*
 * Bitmap helpers
 */

static inline void bitmap_set(bitmap_word_t *bitmap, uint32_t bit)
{
    bitmap[bit / BITS_PER_WORD] |= (1ULL << (bit % BITS_PER_WORD));
}

static inline void bitmap_clear(bitmap_word_t *bitmap, uint32_t bit)
{
    bitmap[bit / BITS_PER_WORD] &= ~(1ULL << (bit % BITS_PER_WORD));
}

static inline bool bitmap_test(const bitmap_word_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / BITS_PER_WORD] & (1ULL << (bit % BITS_PER_WORD))) != 0;
}

static uint32_t bitmap_find_first_zero(const bitmap_word_t *bitmap,
                                        size_t words, uint32_t start_bit)
{
    uint32_t start_word = start_bit / BITS_PER_WORD;
    uint32_t bit_in_word = start_bit % BITS_PER_WORD;

    for (size_t w = start_word; w < words; w++) {
        bitmap_word_t word = bitmap[w];

        /* Skip bits before start_bit in first word */
        if (w == start_word && bit_in_word > 0) {
            word |= ((1ULL << bit_in_word) - 1);
        }

        if (word != ~0ULL) {
            /* Find first zero bit */
            for (uint32_t b = (w == start_word ? bit_in_word : 0);
                 b < BITS_PER_WORD; b++) {
                if (!(word & (1ULL << b))) {
                    return w * BITS_PER_WORD + b;
                }
            }
        }
    }

    return (uint32_t)-1;  /* No free bit found */
}

/*
 * IP to offset conversion
 */

static inline uint32_t ip_to_offset(dhcp_pool_t *pool, uint32_t ip)
{
    uint32_t start_host = ntohl(pool->config.range_start);
    uint32_t ip_host = ntohl(ip);
    return ip_host - start_host;
}

static inline uint32_t offset_to_ip(dhcp_pool_t *pool, uint32_t offset)
{
    uint32_t start_host = ntohl(pool->config.range_start);
    return htonl(start_host + offset);
}

/*
 * Pool Lifecycle
 */

dhcp_pool_t *dhcp_pool_create(const dhcp_pool_config_t *config)
{
    if (!config)
        return NULL;

    dhcp_pool_t *pool = calloc(1, sizeof(dhcp_pool_t));
    if (!pool)
        return NULL;

    memcpy(&pool->config, config, sizeof(dhcp_pool_config_t));

    /* Calculate pool size */
    uint32_t start = ntohl(config->range_start);
    uint32_t end = ntohl(config->range_end);

    if (end < start) {
        free(pool);
        return NULL;
    }

    pool->pool_size = end - start + 1;

    /* Allocate bitmap */
    pool->bitmap_size = (pool->pool_size + BITS_PER_WORD - 1) / BITS_PER_WORD;
    pool->bitmap = calloc(pool->bitmap_size, sizeof(bitmap_word_t));
    if (!pool->bitmap) {
        free(pool);
        return NULL;
    }

    pool->next_free_hint = 0;

    return pool;
}

void dhcp_pool_destroy(dhcp_pool_t *pool)
{
    if (!pool)
        return;

    free(pool->bitmap);
    free(pool);
}

/*
 * Pool Configuration
 */

int dhcp_pool_set_range(dhcp_pool_t *pool, uint32_t start, uint32_t end)
{
    if (!pool)
        return -1;

    uint32_t start_h = ntohl(start);
    uint32_t end_h = ntohl(end);

    if (end_h < start_h)
        return -1;

    uint32_t new_size = end_h - start_h + 1;
    size_t new_bitmap_size = (new_size + BITS_PER_WORD - 1) / BITS_PER_WORD;

    /* Reallocate bitmap if needed */
    if (new_bitmap_size != pool->bitmap_size) {
        bitmap_word_t *new_bitmap = calloc(new_bitmap_size, sizeof(bitmap_word_t));
        if (!new_bitmap)
            return -1;

        free(pool->bitmap);
        pool->bitmap = new_bitmap;
        pool->bitmap_size = new_bitmap_size;
    } else {
        memset(pool->bitmap, 0, pool->bitmap_size * sizeof(bitmap_word_t));
    }

    pool->config.range_start = start;
    pool->config.range_end = end;
    pool->pool_size = new_size;
    pool->next_free_hint = 0;
    pool->allocated_count = 0;

    return 0;
}

int dhcp_pool_set_network(dhcp_pool_t *pool, uint32_t network, uint32_t netmask)
{
    if (!pool)
        return -1;
    pool->config.network = network;
    pool->config.netmask = netmask;
    return 0;
}

int dhcp_pool_set_gateway(dhcp_pool_t *pool, uint32_t gateway)
{
    if (!pool)
        return -1;
    pool->config.gateway = gateway;
    return 0;
}

int dhcp_pool_set_dns(dhcp_pool_t *pool, const uint32_t *servers, int count)
{
    if (!pool || count < 0 || count > 4)
        return -1;
    memcpy(pool->config.dns_servers, servers, count * sizeof(uint32_t));
    pool->config.num_dns = count;
    return 0;
}

int dhcp_pool_set_domain(dhcp_pool_t *pool, const char *domain)
{
    if (!pool)
        return -1;
    strncpy(pool->config.domain, domain ? domain : "", sizeof(pool->config.domain) - 1);
    return 0;
}

int dhcp_pool_set_lease_times(dhcp_pool_t *pool, uint32_t def, uint32_t max, uint32_t min)
{
    if (!pool)
        return -1;
    pool->config.default_lease = def;
    pool->config.max_lease = max;
    pool->config.min_lease = min;
    return 0;
}

/*
 * Check if IP is excluded
 */

static bool is_excluded(dhcp_pool_t *pool, uint32_t ip)
{
    uint32_t ip_h = ntohl(ip);

    for (int i = 0; i < pool->num_exclusions; i++) {
        uint32_t start_h = ntohl(pool->exclusions[i].start);
        uint32_t end_h = ntohl(pool->exclusions[i].end);
        if (ip_h >= start_h && ip_h <= end_h)
            return true;
    }
    return false;
}

/*
 * IP Allocation
 */

uint32_t dhcp_pool_allocate(dhcp_pool_t *pool, const uint8_t *mac)
{
    if (!pool)
        return 0;

    /* Check for existing reservation */
    if (mac) {
        const dhcp_reservation_t *res = dhcp_pool_find_reservation(pool, mac);
        if (res) {
            /* Mark reserved IP as allocated */
            if (dhcp_pool_contains(pool, res->ip)) {
                uint32_t offset = ip_to_offset(pool, res->ip);
                if (!bitmap_test(pool->bitmap, offset)) {
                    bitmap_set(pool->bitmap, offset);
                    pool->allocated_count++;
                }
            }
            return res->ip;
        }
    }

    /* Find first free IP using hint */
    uint32_t offset = bitmap_find_first_zero(pool->bitmap, pool->bitmap_size,
                                              pool->next_free_hint);

    /* Wrap around if needed */
    if (offset >= pool->pool_size && pool->next_free_hint > 0) {
        offset = bitmap_find_first_zero(pool->bitmap, pool->bitmap_size, 0);
    }

    if (offset >= pool->pool_size)
        return 0;  /* No free IP */

    uint32_t ip = offset_to_ip(pool, offset);

    /* Skip excluded IPs */
    while (is_excluded(pool, ip)) {
        bitmap_set(pool->bitmap, offset);  /* Mark as unavailable */
        offset = bitmap_find_first_zero(pool->bitmap, pool->bitmap_size, offset + 1);
        if (offset >= pool->pool_size)
            return 0;
        ip = offset_to_ip(pool, offset);
    }

    /* Allocate */
    bitmap_set(pool->bitmap, offset);
    pool->allocated_count++;
    pool->next_free_hint = offset + 1;

    return ip;
}

uint32_t dhcp_pool_allocate_specific(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return 0;

    if (is_excluded(pool, ip))
        return 0;

    uint32_t offset = ip_to_offset(pool, ip);

    if (bitmap_test(pool->bitmap, offset))
        return 0;  /* Already allocated */

    bitmap_set(pool->bitmap, offset);
    pool->allocated_count++;

    return ip;
}

int dhcp_pool_release(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return -1;

    uint32_t offset = ip_to_offset(pool, ip);

    if (!bitmap_test(pool->bitmap, offset))
        return 0;  /* Already free */

    bitmap_clear(pool->bitmap, offset);
    pool->allocated_count--;

    /* Update hint to prefer lower addresses */
    if (offset < pool->next_free_hint)
        pool->next_free_hint = offset;

    return 0;
}

int dhcp_pool_mark_used(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return -1;

    uint32_t offset = ip_to_offset(pool, ip);

    if (!bitmap_test(pool->bitmap, offset)) {
        bitmap_set(pool->bitmap, offset);
        pool->allocated_count++;
    }

    return 0;
}

int dhcp_pool_mark_declined(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return -1;

    uint32_t offset = ip_to_offset(pool, ip);
    bitmap_set(pool->bitmap, offset);

    pool->declined_count++;

    return 0;
}

/*
 * Reservations
 */

int dhcp_pool_add_reservation(dhcp_pool_t *pool, uint32_t ip,
                               const uint8_t *mac, const char *hostname)
{
    if (!pool || !mac)
        return -1;

    if (pool->num_reservations >= DHCP_POOL_MAX_RESERVATIONS)
        return -1;

    /* Check for duplicate */
    for (int i = 0; i < pool->num_reservations; i++) {
        if (pool->reservations[i].ip == ip ||
            memcmp(pool->reservations[i].mac, mac, 6) == 0) {
            return -1;  /* Duplicate */
        }
    }

    dhcp_reservation_t *res = &pool->reservations[pool->num_reservations];
    res->ip = ip;
    memcpy(res->mac, mac, 6);
    if (hostname) {
        strncpy(res->hostname, hostname, sizeof(res->hostname) - 1);
    }
    res->active = true;

    pool->num_reservations++;

    return 0;
}

int dhcp_pool_remove_reservation(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool)
        return -1;

    for (int i = 0; i < pool->num_reservations; i++) {
        if (pool->reservations[i].ip == ip) {
            /* Move last entry to this slot */
            if (i < pool->num_reservations - 1) {
                pool->reservations[i] = pool->reservations[pool->num_reservations - 1];
            }
            pool->num_reservations--;
            return 0;
        }
    }

    return -1;
}

int dhcp_pool_remove_reservation_by_mac(dhcp_pool_t *pool, const uint8_t *mac)
{
    if (!pool || !mac)
        return -1;

    for (int i = 0; i < pool->num_reservations; i++) {
        if (memcmp(pool->reservations[i].mac, mac, 6) == 0) {
            if (i < pool->num_reservations - 1) {
                pool->reservations[i] = pool->reservations[pool->num_reservations - 1];
            }
            pool->num_reservations--;
            return 0;
        }
    }

    return -1;
}

const dhcp_reservation_t *dhcp_pool_find_reservation(dhcp_pool_t *pool,
                                                      const uint8_t *mac)
{
    if (!pool || !mac)
        return NULL;

    for (int i = 0; i < pool->num_reservations; i++) {
        if (memcmp(pool->reservations[i].mac, mac, 6) == 0)
            return &pool->reservations[i];
    }

    return NULL;
}

const dhcp_reservation_t *dhcp_pool_find_reservation_by_ip(dhcp_pool_t *pool,
                                                            uint32_t ip)
{
    if (!pool)
        return NULL;

    for (int i = 0; i < pool->num_reservations; i++) {
        if (pool->reservations[i].ip == ip)
            return &pool->reservations[i];
    }

    return NULL;
}

int dhcp_pool_get_reservations(dhcp_pool_t *pool,
                                dhcp_reservation_t *out, int max_count)
{
    if (!pool || !out)
        return -1;

    int count = pool->num_reservations < max_count ?
                pool->num_reservations : max_count;
    memcpy(out, pool->reservations, count * sizeof(dhcp_reservation_t));

    return count;
}

/*
 * Exclusions
 */

int dhcp_pool_add_exclusion(dhcp_pool_t *pool, uint32_t start, uint32_t end)
{
    if (!pool)
        return -1;

    if (pool->num_exclusions >= DHCP_POOL_MAX_EXCLUSIONS)
        return -1;

    pool->exclusions[pool->num_exclusions].start = start;
    pool->exclusions[pool->num_exclusions].end = end;
    pool->num_exclusions++;

    /* Mark excluded IPs in bitmap */
    if (dhcp_pool_contains(pool, start)) {
        uint32_t start_h = ntohl(start);
        uint32_t end_h = ntohl(end);
        uint32_t range_start_h = ntohl(pool->config.range_start);
        uint32_t range_end_h = ntohl(pool->config.range_end);

        for (uint32_t ip_h = start_h; ip_h <= end_h; ip_h++) {
            if (ip_h >= range_start_h && ip_h <= range_end_h) {
                uint32_t offset = ip_h - range_start_h;
                if (!bitmap_test(pool->bitmap, offset)) {
                    bitmap_set(pool->bitmap, offset);
                }
            }
        }
    }

    return 0;
}

int dhcp_pool_remove_exclusion(dhcp_pool_t *pool, uint32_t start)
{
    if (!pool)
        return -1;

    for (int i = 0; i < pool->num_exclusions; i++) {
        if (pool->exclusions[i].start == start) {
            if (i < pool->num_exclusions - 1) {
                pool->exclusions[i] = pool->exclusions[pool->num_exclusions - 1];
            }
            pool->num_exclusions--;
            return 0;
        }
    }

    return -1;
}

int dhcp_pool_clear_exclusions(dhcp_pool_t *pool)
{
    if (!pool)
        return -1;
    pool->num_exclusions = 0;
    return 0;
}

/*
 * Query
 */

bool dhcp_pool_contains(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool)
        return false;

    uint32_t ip_h = ntohl(ip);
    uint32_t start_h = ntohl(pool->config.range_start);
    uint32_t end_h = ntohl(pool->config.range_end);

    return ip_h >= start_h && ip_h <= end_h;
}

bool dhcp_pool_is_available(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return false;

    if (is_excluded(pool, ip))
        return false;

    uint32_t offset = ip_to_offset(pool, ip);
    return !bitmap_test(pool->bitmap, offset);
}

bool dhcp_pool_is_excluded(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool)
        return false;
    return is_excluded(pool, ip);
}

bool dhcp_pool_is_reserved(dhcp_pool_t *pool, uint32_t ip)
{
    return dhcp_pool_find_reservation_by_ip(pool, ip) != NULL;
}

bool dhcp_pool_is_allocated(dhcp_pool_t *pool, uint32_t ip)
{
    if (!pool || !dhcp_pool_contains(pool, ip))
        return false;

    uint32_t offset = ip_to_offset(pool, ip);
    return bitmap_test(pool->bitmap, offset);
}

uint32_t dhcp_pool_get_next_available(dhcp_pool_t *pool)
{
    if (!pool)
        return 0;

    uint32_t offset = bitmap_find_first_zero(pool->bitmap, pool->bitmap_size, 0);

    if (offset >= pool->pool_size)
        return 0;

    uint32_t ip = offset_to_ip(pool, offset);

    /* Skip excluded */
    while (is_excluded(pool, ip)) {
        offset++;
        if (offset >= pool->pool_size)
            return 0;
        ip = offset_to_ip(pool, offset);
        if (bitmap_test(pool->bitmap, offset))
            return dhcp_pool_get_next_available(pool);  /* Recursion to find next */
    }

    return ip;
}

/*
 * Pool Information
 */

const char *dhcp_pool_get_name(dhcp_pool_t *pool)
{
    if (!pool)
        return "";
    return pool->config.name;
}

const dhcp_pool_config_t *dhcp_pool_get_config(dhcp_pool_t *pool)
{
    if (!pool)
        return NULL;
    return &pool->config;
}

void dhcp_pool_get_stats(dhcp_pool_t *pool, dhcp_pool_stats_t *stats)
{
    if (!pool || !stats)
        return;

    memset(stats, 0, sizeof(*stats));

    stats->total_ips = pool->pool_size;
    stats->allocated_ips = pool->allocated_count;
    stats->available_ips = pool->pool_size - pool->allocated_count;
    stats->reserved_ips = pool->num_reservations;
    stats->declined_ips = pool->declined_count;

    /* Count excluded IPs */
    for (int i = 0; i < pool->num_exclusions; i++) {
        uint32_t start_h = ntohl(pool->exclusions[i].start);
        uint32_t end_h = ntohl(pool->exclusions[i].end);
        stats->excluded_ips += end_h - start_h + 1;
    }
}

/*
 * Iteration
 */

void dhcp_pool_iterate(dhcp_pool_t *pool, dhcp_pool_iter_fn fn, void *arg)
{
    if (!pool || !fn)
        return;

    for (uint32_t offset = 0; offset < pool->pool_size; offset++) {
        uint32_t ip = offset_to_ip(pool, offset);
        bool allocated = bitmap_test(pool->bitmap, offset);
        bool reserved = dhcp_pool_is_reserved(pool, ip);

        if (fn(ip, allocated, reserved, arg) != 0)
            break;
    }
}

/*
 * Utility
 */

uint32_t dhcp_pool_ip_count(dhcp_pool_t *pool)
{
    if (!pool)
        return 0;
    return pool->pool_size;
}

void dhcp_pool_reset(dhcp_pool_t *pool)
{
    if (!pool)
        return;

    memset(pool->bitmap, 0, pool->bitmap_size * sizeof(bitmap_word_t));
    pool->allocated_count = 0;
    pool->declined_count = 0;
    pool->next_free_hint = 0;
}
