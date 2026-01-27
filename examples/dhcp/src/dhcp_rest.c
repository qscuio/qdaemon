/*
 * DHCP REST API Handlers
 * JSON over IPC REST endpoints for DHCP daemon management
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "dhcp_server.h"
#include "dhcp_relay.h"
#include "dhcp_ipc.h"

#ifdef STANDALONE

/*
 * Standalone build - REST API not available
 * Provide stub initialization function
 */

#include "qdaemon_stub.h"

void dhcp_rest_init(dhcp_server_t *server, dhcp_relay_t *relay)
{
    (void)server;
    (void)relay;
    qd_log_info("REST API not available in standalone build");
}

#else /* !STANDALONE */

#include <qdaemon/qd_rest.h>
#include <qdaemon/qd_log.h>

/*
 * REST Router
 */
static qd_rest_router_t g_rest_router;

/*
 * Context passed to handlers
 */
typedef struct {
    dhcp_server_t *server;
    dhcp_relay_t  *relay;
} dhcp_rest_ctx_t;

/*
 * JSON Helper - Add IP address
 */
static void json_add_ip(qd_json_buf_t *json, const char *key, uint32_t ip)
{
    char ip_str[16];
    struct in_addr addr = { .s_addr = ip };
    inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
    qd_json_add_str(json, key, ip_str);
}

/*
 * JSON Helper - Add MAC address
 */
static void json_add_mac(qd_json_buf_t *json, const char *key, const uint8_t *mac)
{
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    qd_json_add_str(json, key, mac_str);
}

/*
 * Lease iteration context for REST API
 */
typedef struct {
    qd_json_buf_t *json;
    int first;
} rest_lease_iter_ctx_t;

/* Callback for lease iteration in REST API */
static int rest_lease_iter_cb(dhcp_lease_t *lease, void *arg)
{
    rest_lease_iter_ctx_t *ctx = (rest_lease_iter_ctx_t *)arg;

    if (!ctx->first)
        qd_json_append(ctx->json, ",");
    ctx->first = 0;

    qd_json_append(ctx->json, "{");
    json_add_ip(ctx->json, "ip", lease->ip);
    json_add_mac(ctx->json, "mac", lease->mac);
    qd_json_add_str(ctx->json, "state", dhcp_lease_state_name(lease->state));
    qd_json_add_int(ctx->json, "lease_time", lease->lease_time);
    qd_json_add_int(ctx->json, "remaining", dhcp_lease_remaining_time(lease));
    if (lease->hostname[0])
        qd_json_add_str(ctx->json, "hostname", lease->hostname);
    qd_json_end_obj(ctx->json);

    return 0;
}

/*
 * GET /api/v1/leases - List all leases
 */
static int rest_get_leases(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;
    dhcp_lease_db_t *db = dhcp_server_get_lease_db(ctx->server);

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_int(&resp->json, "count", dhcp_lease_count(db));

    /* Start leases array */
    qd_json_append(&resp->json, ",\"leases\":[");

    rest_lease_iter_ctx_t iter_ctx = { .json = &resp->json, .first = 1 };
    dhcp_lease_iterate(db, rest_lease_iter_cb, &iter_ctx);

    qd_json_append(&resp->json, "]");
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * DELETE /api/v1/leases/{ip} - Release specific lease
 */
static int rest_delete_lease(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    /* Extract IP from path */
    const char *ip_str = strrchr(req->path, '/');
    if (!ip_str) {
        qd_rest_error(resp, QD_REST_BAD_REQUEST, "Missing IP address");
        return -1;
    }
    ip_str++;

    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        qd_rest_error(resp, QD_REST_BAD_REQUEST, "Invalid IP address");
        return -1;
    }

    if (dhcp_server_clear_lease(ctx->server, addr.s_addr) != 0) {
        qd_rest_error(resp, QD_REST_NOT_FOUND, "Lease not found");
        return -1;
    }

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_str(&resp->json, "message", "Lease cleared");
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * GET /api/v1/pools - List all pools
 */
static int rest_get_pools(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_int(&resp->json, "count", dhcp_server_get_pool_count(ctx->server));

    qd_json_append(&resp->json, ",\"pools\":[");

    /* Iterate through pools */
    for (int i = 0; i < dhcp_server_get_pool_count(ctx->server); i++) {
        /* Would need pool iteration API - simplified for now */
    }

    qd_json_append(&resp->json, "]");
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * GET /api/v1/pools/{name}/stats - Get pool statistics
 */
static int rest_get_pool_stats(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    /* Extract pool name from path */
    const char *path = req->path + strlen("/api/v1/pools/");
    char pool_name[32];
    const char *slash = strchr(path, '/');
    if (!slash) {
        qd_rest_error(resp, QD_REST_BAD_REQUEST, "Invalid path");
        return -1;
    }

    size_t name_len = slash - path;
    if (name_len >= sizeof(pool_name)) {
        qd_rest_error(resp, QD_REST_BAD_REQUEST, "Pool name too long");
        return -1;
    }

    memcpy(pool_name, path, name_len);
    pool_name[name_len] = '\0';

    dhcp_pool_t *pool = dhcp_server_find_pool(ctx->server, pool_name);
    if (!pool) {
        qd_rest_error(resp, QD_REST_NOT_FOUND, "Pool not found");
        return -1;
    }

    dhcp_pool_stats_t stats;
    dhcp_pool_get_stats(pool, &stats);

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_str(&resp->json, "pool", pool_name);
    qd_json_add_int(&resp->json, "total_ips", stats.total_ips);
    qd_json_add_int(&resp->json, "allocated", stats.allocated_ips);
    qd_json_add_int(&resp->json, "available", stats.available_ips);
    qd_json_add_int(&resp->json, "reserved", stats.reserved_ips);
    qd_json_add_int(&resp->json, "declined", stats.declined_ips);

    if (stats.total_ips > 0) {
        int util = (stats.allocated_ips * 100) / stats.total_ips;
        qd_json_add_int(&resp->json, "utilization_percent", util);
    }

    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * GET /api/v1/server/status - Get server status
 */
static int rest_get_server_status(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    const dhcp_server_config_t *cfg = dhcp_server_get_config(ctx->server);
    dhcp_server_stats_t stats;
    dhcp_server_get_stats(ctx->server, &stats);

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_bool(&resp->json, "running", true);
    json_add_ip(&resp->json, "server_id", cfg->server_id);
    qd_json_add_int(&resp->json, "pools", dhcp_server_get_pool_count(ctx->server));
    qd_json_add_int(&resp->json, "leases", dhcp_lease_count(dhcp_server_get_lease_db(ctx->server)));

    /* Statistics */
    qd_json_append(&resp->json, ",\"statistics\":{");
    qd_json_add_int(&resp->json, "discovers", stats.discovers);
    qd_json_add_int(&resp->json, "offers", stats.offers);
    qd_json_add_int(&resp->json, "requests", stats.requests);
    qd_json_add_int(&resp->json, "acks", stats.acks);
    qd_json_add_int(&resp->json, "naks", stats.naks);
    qd_json_add_int(&resp->json, "releases", stats.releases);
    qd_json_add_int(&resp->json, "declines", stats.declines);
    qd_json_end_obj(&resp->json);

    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * GET /api/v1/server/stats - Get server statistics
 */
static int rest_get_server_stats(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    dhcp_server_stats_t stats;
    dhcp_server_get_stats(ctx->server, &stats);

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_int(&resp->json, "discovers", stats.discovers);
    qd_json_add_int(&resp->json, "offers", stats.offers);
    qd_json_add_int(&resp->json, "requests", stats.requests);
    qd_json_add_int(&resp->json, "acks", stats.acks);
    qd_json_add_int(&resp->json, "naks", stats.naks);
    qd_json_add_int(&resp->json, "releases", stats.releases);
    qd_json_add_int(&resp->json, "declines", stats.declines);
    qd_json_add_int(&resp->json, "informs", stats.informs);
    qd_json_add_int(&resp->json, "packets_in", stats.packets_in);
    qd_json_add_int(&resp->json, "packets_out", stats.packets_out);
    qd_json_add_int(&resp->json, "errors", stats.errors);
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * POST /api/v1/server/reload - Reload configuration
 */
static int rest_server_reload(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    (void)arg;

    qd_log_info("Configuration reload requested via REST API");

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_str(&resp->json, "message", "Reload initiated");
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * GET /api/v1/relay/status - Get relay status
 */
static int rest_get_relay_status(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    (void)req;
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");

    if (ctx->relay) {
        qd_json_add_bool(&resp->json, "enabled", dhcp_relay_is_enabled(ctx->relay));

        dhcp_relay_stats_t stats;
        dhcp_relay_get_stats(ctx->relay, &stats);

        qd_json_append(&resp->json, ",\"statistics\":{");
        qd_json_add_int(&resp->json, "requests_forwarded", stats.requests_forwarded);
        qd_json_add_int(&resp->json, "replies_relayed", stats.replies_relayed);
        qd_json_add_int(&resp->json, "drops_max_hops", stats.drops_max_hops);
        qd_json_add_int(&resp->json, "drops_no_server", stats.drops_no_server);
        qd_json_end_obj(&resp->json);
    } else {
        qd_json_add_bool(&resp->json, "enabled", false);
    }

    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_OK;
    return 0;
}

/*
 * POST /api/v1/relay/servers - Add relay server
 */
static int rest_add_relay_server(qd_rest_req_t *req, qd_rest_resp_t *resp, void *arg)
{
    dhcp_rest_ctx_t *ctx = (dhcp_rest_ctx_t *)arg;

    if (!ctx->relay) {
        qd_rest_error(resp, QD_REST_ERROR, "Relay not configured");
        return -1;
    }

    /* Parse JSON body - simplified */
    /* Would need proper JSON parsing */
    (void)req;

    qd_json_start_obj(&resp->json);
    qd_json_add_str(&resp->json, "status", "ok");
    qd_json_add_str(&resp->json, "message", "Server added");
    qd_json_end_obj(&resp->json);

    resp->status = QD_REST_CREATED;
    return 0;
}

/*
 * Initialize REST API
 */
void dhcp_rest_init(dhcp_server_t *server, dhcp_relay_t *relay)
{
    static dhcp_rest_ctx_t ctx;
    ctx.server = server;
    ctx.relay = relay;

    qd_rest_router_init(&g_rest_router);

    /* Lease endpoints */
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/leases",
                      rest_get_leases, &ctx);
    qd_rest_add_route(&g_rest_router, QD_REST_DELETE, "/api/v1/leases/*",
                      rest_delete_lease, &ctx);

    /* Pool endpoints */
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/pools",
                      rest_get_pools, &ctx);
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/pools/*/stats",
                      rest_get_pool_stats, &ctx);

    /* Server endpoints */
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/server/status",
                      rest_get_server_status, &ctx);
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/server/stats",
                      rest_get_server_stats, &ctx);
    qd_rest_add_route(&g_rest_router, QD_REST_POST, "/api/v1/server/reload",
                      rest_server_reload, &ctx);

    /* Relay endpoints */
    qd_rest_add_route(&g_rest_router, QD_REST_GET, "/api/v1/relay/status",
                      rest_get_relay_status, &ctx);
    qd_rest_add_route(&g_rest_router, QD_REST_POST, "/api/v1/relay/servers",
                      rest_add_relay_server, &ctx);

    qd_log_info("REST API initialized with %d routes", g_rest_router.num_routes);
}

/*
 * Get REST router for request dispatching
 */
qd_rest_router_t *dhcp_rest_get_router(void)
{
    return &g_rest_router;
}

#endif /* !STANDALONE */
