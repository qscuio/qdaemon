/*
 * DHCP File-based Storage Backend
 * Implements dhcp_storage_ops_t with append-only log format
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include <qdaemon/qd_log.h>
#include <qd_hash.h>

#include "dhcp_core.h"
#include "dhcp_protocol.h"

#define FILE_STORAGE_VERSION "1"
#define MAX_LINE_LEN 512

/*
 * File storage context
 */
typedef struct file_storage_ctx {
    char *file_path;
    FILE *log_fp;          /* Append-only log file */
    qd_hashmap_t lease_cache; /* In-memory cache of current leases (MAC hex -> cached_lease_t*) */
} file_storage_ctx_t;

/*
 * Cached lease entry (for in-memory state)
 */
typedef struct cached_lease {
    uint8_t mac[6];
    uint32_t ip;
    uint32_t lease_time;
    time_t start_time;
    char hostname[64];
} cached_lease_t;

/*
 * Forward declarations
 */
static int file_storage_init(void *ctx, const char *conn_str);
static int file_storage_get_lease(void *ctx, const uint8_t *mac, uint8_t mac_len,
                                   dhcp_lease_t **out_lease);
static int file_storage_save_lease(void *ctx, const dhcp_lease_t *lease);
static int file_storage_delete_lease(void *ctx, const uint8_t *mac, uint8_t mac_len);
static void file_storage_close(void *ctx);

/*
 * Storage operations vtable
 */
const dhcp_storage_ops_t dhcp_storage_file_ops = {
    .name = "file",
    .init = file_storage_init,
    .get_lease = file_storage_get_lease,
    .save_lease = file_storage_save_lease,
    .delete_lease = file_storage_delete_lease,
    .close = file_storage_close,
};

/*
 * Helper to convert MAC to string key
 */
static void mac_to_key(const uint8_t *mac, char *key)
{
    sprintf(key, "%02x%02x%02x%02x%02x%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * Create file storage context
 */
file_storage_ctx_t *dhcp_storage_file_create(const char *path)
{
    file_storage_ctx_t *ctx = calloc(1, sizeof(file_storage_ctx_t));
    if (!ctx) return NULL;

    if (path) {
        ctx->file_path = strdup(path);
        if (!ctx->file_path) {
            free(ctx);
            return NULL;
        }
    }

    if (qd_hashmap_init(&ctx->lease_cache, 256) != 0) {
        free(ctx->file_path);
        free(ctx);
        return NULL;
    }

    return ctx;
}

/*
 * Helper: Parse MAC address from string
 */
static int parse_mac(const char *str, uint8_t *mac)
{
    unsigned int m[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)m[i];
    }
    return 0;
}

/*
 * Helper: Format MAC address to string
 */
static void format_mac(const uint8_t *mac, char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * Helper: Parse a log line and update cache
 */
static int parse_log_line(file_storage_ctx_t *ctx, const char *line)
{
    char action;
    char mac_str[20];
    char ip_str[20];
    uint32_t lease_time;
    char hostname[64] = {0};
    time_t timestamp;

    /* Parse: TIMESTAMP|ACTION|MAC|IP|LEASE_TIME|HOSTNAME */
    int n = sscanf(line, "%ld|%c|%17[^|]|%15[^|]|%u|%63[^\n]",
                   &timestamp, &action, mac_str, ip_str, &lease_time, hostname);

    if (n < 5) {
        return -1;  /* Invalid line format */
    }

    uint8_t mac[6];
    if (parse_mac(mac_str, mac) != 0) {
        return -1;
    }
    
    char key[13];
    mac_to_key(mac, key);

    if (action == 'A') {
        /* Add or update lease */
        cached_lease_t *cached = (cached_lease_t *)qd_hashmap_get(&ctx->lease_cache, key);
        if (!cached) {
            cached = calloc(1, sizeof(cached_lease_t));
            if (!cached) return -1;
            memcpy(cached->mac, mac, 6);
            qd_hashmap_put(&ctx->lease_cache, key, cached);
        }

        cached->ip = inet_addr(ip_str);
        cached->lease_time = lease_time;
        cached->start_time = timestamp;
        if (hostname[0]) {
            size_t len = strlen(hostname);
            if (len >= sizeof(cached->hostname)) len = sizeof(cached->hostname) - 1;
            memcpy(cached->hostname, hostname, len);
            cached->hostname[len] = '\0';
        }

    } else if (action == 'D') {
        /* Delete lease */
        cached_lease_t *cached = (cached_lease_t *)qd_hashmap_remove(&ctx->lease_cache, key);
        if (cached) {
            free(cached);
        }
    }

    return 0;
}

/*
 * Helper: Replay log file to reconstruct state
 */
static int replay_log(file_storage_ctx_t *ctx)
{
    FILE *fp = fopen(ctx->file_path, "r");
    if (!fp) {
        if (errno == ENOENT) {
            /* File doesn't exist - start fresh */
            return 0;
        }
        qd_log_error("Failed to open log file: %s", strerror(errno));
        return -1;
    }

    char line[MAX_LINE_LEN];
    int count = 0;
    int errors = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Skip empty lines and comments */
        if (line[0] == '\n' || line[0] == '#') continue;

        if (parse_log_line(ctx, line) != 0) {
            errors++;
        } else {
            count++;
        }
    }

    fclose(fp);
    qd_log_info("Replayed %d log entries (%d errors)", count, errors);
    return 0;
}

/*
 * Storage operations implementation
 */

static int file_storage_init(void *ctx, const char *conn_str)
{
    file_storage_ctx_t *fctx = (file_storage_ctx_t *)ctx;
    if (!fctx) return -1;

    /* Use conn_str as file path if provided */
    if (conn_str && !fctx->file_path) {
        fctx->file_path = strdup(conn_str);
        if (!fctx->file_path) return -1;
    }

    if (!fctx->file_path) {
        qd_log_error("No file path specified for storage");
        return -1;
    }

    /* Replay existing log to reconstruct state */
    if (replay_log(fctx) != 0) {
        return -1;
    }

    /* Open log file for appending */
    fctx->log_fp = fopen(fctx->file_path, "a");
    if (!fctx->log_fp) {
        qd_log_error("Failed to open log file for writing: %s", strerror(errno));
        return -1;
    }

    /* Set line buffering for durability */
    setvbuf(fctx->log_fp, NULL, _IOLBF, 0);

    qd_log_info("File storage initialized: %s", fctx->file_path);
    return 0;
}

static int file_storage_get_lease(void *ctx, const uint8_t *mac, uint8_t mac_len,
                                   dhcp_lease_t **out_lease)
{
    file_storage_ctx_t *fctx = (file_storage_ctx_t *)ctx;
    if (!fctx || !mac || mac_len < 6 || !out_lease) return -1;
    
    char key[13];
    mac_to_key(mac, key);

    cached_lease_t *cached = (cached_lease_t *)qd_hashmap_get(&fctx->lease_cache, key);
    if (!cached) {
        *out_lease = NULL;
        return 0;  /* Not found, but not an error */
    }

    /* Allocate and populate lease structure */
    dhcp_lease_t *lease = calloc(1, sizeof(dhcp_lease_t));
    if (!lease) return -1;

    memcpy(lease->mac, cached->mac, 6);
    lease->ip = cached->ip;
    lease->lease_time = cached->lease_time;
    lease->start_time = cached->start_time * 1000;  /* Convert to ms */
    lease->expire_time = lease->start_time + (lease->lease_time * 1000ULL);
    size_t hlen = strlen(cached->hostname);
    if (hlen >= sizeof(lease->hostname)) hlen = sizeof(lease->hostname) - 1;
    memcpy(lease->hostname, cached->hostname, hlen);
    lease->hostname[hlen] = '\0';

    /* Check if lease is still valid */
    time_t now = time(NULL);
    if (now > cached->start_time + cached->lease_time) {
        lease->state = DHCP_LEASE_EXPIRED;
    } else {
        lease->state = DHCP_LEASE_BOUND;
    }

    *out_lease = lease;
    return 0;
}

static int file_storage_save_lease(void *ctx, const dhcp_lease_t *lease)
{
    file_storage_ctx_t *fctx = (file_storage_ctx_t *)ctx;
    if (!fctx || !lease || !fctx->log_fp) return -1;

    char mac_str[20];
    struct in_addr addr;
    addr.s_addr = lease->ip;

    format_mac(lease->mac, mac_str, sizeof(mac_str));

    /* Write log entry: TIMESTAMP|A|MAC|IP|LEASE_TIME|HOSTNAME */
    time_t now = time(NULL);
    if (fprintf(fctx->log_fp, "%ld|A|%s|%s|%u|%s\n",
                now, mac_str, inet_ntoa(addr),
                lease->lease_time, lease->hostname) < 0) {
        qd_log_error("Failed to write lease to log: %s", strerror(errno));
        return -1;
    }
    fflush(fctx->log_fp);

    /* Update in-memory cache */
    char key[13];
    mac_to_key(lease->mac, key);
    
    cached_lease_t *cached = (cached_lease_t *)qd_hashmap_get(&fctx->lease_cache, key);
    if (!cached) {
        cached = calloc(1, sizeof(cached_lease_t));
        if (!cached) return -1;
        memcpy(cached->mac, lease->mac, 6);
        qd_hashmap_put(&fctx->lease_cache, key, cached);
    }

    cached->ip = lease->ip;
    cached->lease_time = lease->lease_time;
    cached->start_time = now;
    size_t slen = strlen(lease->hostname);
    if (slen >= sizeof(cached->hostname)) slen = sizeof(cached->hostname) - 1;
    memcpy(cached->hostname, lease->hostname, slen);
    cached->hostname[slen] = '\0';

    return 0;
}

static int file_storage_delete_lease(void *ctx, const uint8_t *mac, uint8_t mac_len)
{
    file_storage_ctx_t *fctx = (file_storage_ctx_t *)ctx;
    if (!fctx || !mac || mac_len < 6 || !fctx->log_fp) return -1;

    char mac_str[20];
    format_mac(mac, mac_str, sizeof(mac_str));

    /* Write delete log entry: TIMESTAMP|D|MAC|0.0.0.0|0| */
    time_t now = time(NULL);
    if (fprintf(fctx->log_fp, "%ld|D|%s|0.0.0.0|0|\n", now, mac_str) < 0) {
        qd_log_error("Failed to write delete to log: %s", strerror(errno));
        return -1;
    }
    fflush(fctx->log_fp);

    /* Remove from in-memory cache */
    char key[13];
    mac_to_key(mac, key);
    
    cached_lease_t *cached = (cached_lease_t *)qd_hashmap_remove(&fctx->lease_cache, key);
    if (cached) {
        free(cached);
    }

    return 0;
}

static void file_storage_close(void *ctx)
{
    file_storage_ctx_t *fctx = (file_storage_ctx_t *)ctx;
    if (!fctx) return;

    if (fctx->log_fp) {
        fclose(fctx->log_fp);
        fctx->log_fp = NULL;
    }

    /* Free all cached entries */
    qd_hash_entry_t *entry;
    qd_hashmap_for_each(entry, &fctx->lease_cache) {
        cached_lease_t *cached = (cached_lease_t *)entry->value;
        free(cached);
    }
    qd_hashmap_destroy(&fctx->lease_cache);

    free(fctx->file_path);
    free(fctx);

    qd_log_info("File storage closed");
}

/*
 * Utility: Compact the log file
 * Rewrites the log with only current lease state
 */
int dhcp_storage_file_compact(file_storage_ctx_t *ctx)
{
    if (!ctx || !ctx->file_path) return -1;

    char temp_path[256];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", ctx->file_path);

    FILE *tmp_fp = fopen(temp_path, "w");
    if (!tmp_fp) {
        qd_log_error("Failed to create temp file for compaction: %s", strerror(errno));
        return -1;
    }

    /* Write header */
    fprintf(tmp_fp, "# DHCP Lease Log v%s (compacted)\n", FILE_STORAGE_VERSION);

    /* Iterate cache and write current state */
    qd_hash_entry_t *entry;
    qd_hashmap_for_each(entry, &ctx->lease_cache) {
        cached_lease_t *cached = (cached_lease_t *)entry->value;
        
        char mac_str[20];
        struct in_addr addr;
        addr.s_addr = cached->ip;
        format_mac(cached->mac, mac_str, sizeof(mac_str));
        
        fprintf(tmp_fp, "%ld|A|%s|%s|%u|%s\n",
                cached->start_time, mac_str, inet_ntoa(addr),
                cached->lease_time, cached->hostname);
    }

    fclose(tmp_fp);
    
    /* Rename temp to actual */
    if (rename(temp_path, ctx->file_path) != 0) {
        qd_log_error("Failed to rename compacted file: %s", strerror(errno));
        remove(temp_path);
        return -1;
    }

    qd_log_info("Log compaction completed");
    return 0;
}
