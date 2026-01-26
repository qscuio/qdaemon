/*
 * DHCP Server Implementation
 * State machine based message processing
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <qdaemon/qd_log.h>
#include "dhcp_server.h"

/*
 * Server Context
 */
struct dhcp_server {
    dhcp_server_config_t config;

    /* Pools */
    dhcp_pool_t *pools[DHCP_SERVER_MAX_POOLS];
    int          num_pools;

    /* Lease database */
    dhcp_lease_db_t *lease_db;

    /* Handler table */
    qd_handler_table_t *handlers;

    /* Send callback */
    dhcp_server_send_fn send_fn;
    void               *send_arg;

    /* Event loop reference */
    qd_event_loop_t *loop;

    /* Statistics */
    dhcp_server_stats_t stats;

    /* Server MAC (for responses) */
    uint8_t server_mac[6];
};

/*
 * Forward declarations
 */
static int send_dhcp_response(dhcp_server_t *server, const dhcp_packet_t *request,
                               uint8_t msg_type, uint32_t offered_ip,
                               dhcp_pool_t *pool);

/*
 * Handler Table Definition
 */
static const qd_handler_entry_t server_handler_entries[] = {
    QD_HANDLER(DHCP_DISCOVER, dhcp_server_handle_discover),
    QD_HANDLER(DHCP_REQUEST,  dhcp_server_handle_request),
    QD_HANDLER(DHCP_RELEASE,  dhcp_server_handle_release),
    QD_HANDLER(DHCP_DECLINE,  dhcp_server_handle_decline),
    QD_HANDLER(DHCP_INFORM,   dhcp_server_handle_inform),
    QD_HANDLER_END()
};

/*
 * Lifecycle
 */

dhcp_server_t *dhcp_server_create(const dhcp_server_config_t *config)
{
    dhcp_server_t *server = calloc(1, sizeof(dhcp_server_t));
    if (!server)
        return NULL;

    if (config) {
        memcpy(&server->config, config, sizeof(dhcp_server_config_t));
    } else {
        dhcp_server_config_t defaults = DHCP_SERVER_CONFIG_DEFAULT;
        memcpy(&server->config, &defaults, sizeof(dhcp_server_config_t));
    }

    /* Create lease database */
    dhcp_lease_db_config_t db_config = DHCP_LEASE_DB_CONFIG_DEFAULT;
    db_config.offer_timeout = server->config.offer_timeout;
    if (server->config.lease_file) {
        db_config.enable_persistence = true;
        db_config.persistence_path = server->config.lease_file;
    }

    server->lease_db = dhcp_lease_db_create(&db_config);
    if (!server->lease_db) {
        free(server);
        return NULL;
    }

    /* Create handler table */
    server->handlers = qd_handler_table_create("dhcp_server");
    if (!server->handlers) {
        dhcp_lease_db_destroy(server->lease_db);
        free(server);
        return NULL;
    }

    /* Register handlers with server as argument */
    for (int i = 0; server_handler_entries[i].handler; i++) {
        qd_handler_register_fn(server->handlers,
                               server_handler_entries[i].cmd,
                               server_handler_entries[i].handler,
                               server, 0);
    }

    /* Generate server MAC (locally administered) */
    server->server_mac[0] = 0x02;
    for (int i = 1; i < 6; i++) {
        server->server_mac[i] = rand() & 0xff;
    }

    return server;
}

void dhcp_server_destroy(dhcp_server_t *server)
{
    if (!server)
        return;

    /* Save leases if configured */
    if (server->config.lease_file) {
        dhcp_server_save_leases(server);
    }

    /* Destroy pools */
    for (int i = 0; i < server->num_pools; i++) {
        dhcp_pool_destroy(server->pools[i]);
    }

    dhcp_lease_db_destroy(server->lease_db);
    qd_handler_table_destroy(server->handlers);
    free(server);
}

int dhcp_server_start(dhcp_server_t *server, qd_event_loop_t *loop)
{
    if (!server)
        return -1;

    server->loop = loop;

    /* Load saved leases */
    if (server->config.lease_file) {
        dhcp_server_load_leases(server);
    }

    qd_log_info("DHCP server started with %d pools", server->num_pools);

    return 0;
}

void dhcp_server_stop(dhcp_server_t *server)
{
    if (!server)
        return;

    /* Save leases */
    if (server->config.lease_file) {
        dhcp_server_save_leases(server);
    }

    server->loop = NULL;

    qd_log_info("DHCP server stopped");
}

/*
 * Configuration
 */

int dhcp_server_set_server_id(dhcp_server_t *server, uint32_t ip)
{
    if (!server)
        return -1;
    server->config.server_id = ip;
    return 0;
}

int dhcp_server_set_send_callback(dhcp_server_t *server,
                                   dhcp_server_send_fn fn, void *arg)
{
    if (!server)
        return -1;
    server->send_fn = fn;
    server->send_arg = arg;
    return 0;
}

const dhcp_server_config_t *dhcp_server_get_config(dhcp_server_t *server)
{
    if (!server)
        return NULL;
    return &server->config;
}

/*
 * Pool Management
 */

int dhcp_server_add_pool(dhcp_server_t *server, dhcp_pool_t *pool)
{
    if (!server || !pool)
        return -1;

    if (server->num_pools >= DHCP_SERVER_MAX_POOLS)
        return -1;

    server->pools[server->num_pools++] = pool;

    qd_log_info("Added pool '%s' to server", dhcp_pool_get_name(pool));

    return 0;
}

int dhcp_server_remove_pool(dhcp_server_t *server, const char *name)
{
    if (!server || !name)
        return -1;

    for (int i = 0; i < server->num_pools; i++) {
        if (strcmp(dhcp_pool_get_name(server->pools[i]), name) == 0) {
            dhcp_pool_destroy(server->pools[i]);
            /* Shift remaining pools */
            for (int j = i; j < server->num_pools - 1; j++) {
                server->pools[j] = server->pools[j + 1];
            }
            server->num_pools--;
            return 0;
        }
    }

    return -1;
}

dhcp_pool_t *dhcp_server_find_pool(dhcp_server_t *server, const char *name)
{
    if (!server || !name)
        return NULL;

    for (int i = 0; i < server->num_pools; i++) {
        if (strcmp(dhcp_pool_get_name(server->pools[i]), name) == 0)
            return server->pools[i];
    }

    return NULL;
}

dhcp_pool_t *dhcp_server_find_pool_for_ip(dhcp_server_t *server, uint32_t ip)
{
    if (!server)
        return NULL;

    for (int i = 0; i < server->num_pools; i++) {
        if (dhcp_pool_contains(server->pools[i], ip))
            return server->pools[i];
    }

    return NULL;
}

int dhcp_server_get_pool_count(dhcp_server_t *server)
{
    if (!server)
        return 0;
    return server->num_pools;
}

/*
 * Lease Management
 */

dhcp_lease_db_t *dhcp_server_get_lease_db(dhcp_server_t *server)
{
    if (!server)
        return NULL;
    return server->lease_db;
}

dhcp_lease_t *dhcp_server_find_lease_by_mac(dhcp_server_t *server, const uint8_t *mac)
{
    if (!server)
        return NULL;
    return dhcp_lease_find_by_mac(server->lease_db, mac);
}

dhcp_lease_t *dhcp_server_find_lease_by_ip(dhcp_server_t *server, uint32_t ip)
{
    if (!server)
        return NULL;
    return dhcp_lease_find_by_ip(server->lease_db, ip);
}

int dhcp_server_clear_lease(dhcp_server_t *server, uint32_t ip)
{
    if (!server)
        return -1;

    dhcp_lease_t *lease = dhcp_lease_find_by_ip(server->lease_db, ip);
    if (!lease)
        return -1;

    /* Release IP back to pool */
    dhcp_pool_t *pool = dhcp_server_find_pool_for_ip(server, ip);
    if (pool) {
        dhcp_pool_release(pool, ip);
    }

    return dhcp_lease_delete(server->lease_db, lease);
}

int dhcp_server_clear_all_leases(dhcp_server_t *server)
{
    if (!server)
        return -1;

    /* Reset all pools */
    for (int i = 0; i < server->num_pools; i++) {
        dhcp_pool_reset(server->pools[i]);
    }

    dhcp_lease_db_clear(server->lease_db);
    return 0;
}

/*
 * Find best pool for request
 */
static dhcp_pool_t *find_pool_for_request(dhcp_server_t *server,
                                           const dhcp_packet_t *pkt,
                                           const dhcp_rx_info_t *rx_info)
{
    (void)pkt;
    (void)rx_info;

    /* For now, return first pool with available IPs */
    for (int i = 0; i < server->num_pools; i++) {
        dhcp_pool_stats_t stats;
        dhcp_pool_get_stats(server->pools[i], &stats);
        if (stats.available_ips > 0)
            return server->pools[i];
    }

    return server->num_pools > 0 ? server->pools[0] : NULL;
}

/*
 * Build and send DHCP response
 */
static int send_dhcp_response(dhcp_server_t *server, const dhcp_packet_t *request,
                               uint8_t msg_type, uint32_t offered_ip,
                               dhcp_pool_t *pool)
{
    if (!server->send_fn)
        return -1;

    dhcp_packet_t reply;
    memset(&reply, 0, sizeof(reply));

    /* Build reply header */
    reply.op = DHCP_BOOTREPLY;
    reply.htype = DHCP_HTYPE_ETHER;
    reply.hlen = 6;
    reply.hops = 0;
    reply.xid = request->xid;
    reply.secs = 0;
    reply.flags = request->flags;
    reply.ciaddr = 0;
    reply.yiaddr = (msg_type == DHCP_NAK) ? 0 : offered_ip;
    reply.siaddr = server->config.server_id;
    reply.giaddr = request->giaddr;
    memcpy(reply.chaddr, request->chaddr, 16);
    reply.magic = htonl(DHCP_MAGIC_COOKIE);

    /* Build options */
    dhcp_option_list_t opts;
    dhcp_option_list_init(&opts);

    dhcp_option_add_msg_type(&opts, msg_type);
    dhcp_option_add_server_id(&opts, server->config.server_id);

    if (msg_type == DHCP_OFFER || msg_type == DHCP_ACK) {
        const dhcp_pool_config_t *pool_cfg = dhcp_pool_get_config(pool);

        /* Lease time */
        dhcp_option_add_lease_time(&opts, pool_cfg->default_lease);

        /* Renewal (T1) and Rebind (T2) times */
        dhcp_option_add_renewal_time(&opts, pool_cfg->default_lease / 2);
        dhcp_option_add_rebind_time(&opts, (pool_cfg->default_lease * 7) / 8);

        /* Network configuration */
        dhcp_option_add_subnet_mask(&opts, pool_cfg->netmask);

        if (pool_cfg->gateway) {
            dhcp_option_add_router(&opts, pool_cfg->gateway);
        }

        if (pool_cfg->num_dns > 0) {
            dhcp_option_add_dns(&opts, pool_cfg->dns_servers, pool_cfg->num_dns);
        }

        if (pool_cfg->domain[0]) {
            dhcp_option_add_domain(&opts, pool_cfg->domain);
        }

        if (pool_cfg->netmask) {
            /* Calculate broadcast address */
            uint32_t network = pool_cfg->network & pool_cfg->netmask;
            uint32_t broadcast = network | ~pool_cfg->netmask;
            dhcp_option_add_broadcast(&opts, broadcast);
        }
    } else if (msg_type == DHCP_NAK) {
        dhcp_option_add_message(&opts, "Requested address not available");
    }

    /* Finalize options */
    dhcp_option_list_finalize(&opts);
    memcpy(reply.options, opts.buffer, opts.offset);

    /* Determine destination */
    uint8_t dst_mac[6];
    uint32_t dst_ip;
    bool broadcast = false;

    if (request->giaddr != 0) {
        /* Relay agent - send to relay */
        memcpy(dst_mac, request->chaddr, 6);  /* Would need relay MAC */
        dst_ip = request->giaddr;
    } else if (request->ciaddr != 0) {
        /* Unicast to existing client IP */
        memcpy(dst_mac, request->chaddr, 6);
        dst_ip = request->ciaddr;
    } else if (ntohs(request->flags) & DHCP_FLAG_BROADCAST) {
        /* Client requested broadcast */
        memset(dst_mac, 0xff, 6);
        dst_ip = INADDR_BROADCAST;
        broadcast = true;
    } else {
        /* Try unicast to offered IP (client may not be ready) */
        memcpy(dst_mac, request->chaddr, 6);
        dst_ip = offered_ip;
        broadcast = true;  /* Many clients need broadcast anyway */
    }

    /* Send via callback */
    size_t pkt_len = sizeof(dhcp_packet_t) - DHCP_MAX_OPTIONS_SIZE + opts.offset;
    int ret = server->send_fn((uint8_t *)&reply, pkt_len,
                               dst_mac, dst_ip, broadcast, server->send_arg);

    /* Update statistics */
    if (ret == 0) {
        server->stats.packets_out++;
        switch (msg_type) {
        case DHCP_OFFER: server->stats.offers++; break;
        case DHCP_ACK:   server->stats.acks++; break;
        case DHCP_NAK:   server->stats.naks++; break;
        }
    }

    return ret;
}

/*
 * Message Handlers
 */

qd_handler_result_t dhcp_server_handle_discover(qd_handler_ctx_t *ctx, void *arg)
{
    dhcp_server_t *server = (dhcp_server_t *)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    server->stats.discovers++;
    server->stats.packets_in++;

    qd_log_debug("DHCP DISCOVER from %02x:%02x:%02x:%02x:%02x:%02x",
                 request->chaddr[0], request->chaddr[1], request->chaddr[2],
                 request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Find appropriate pool */
    dhcp_pool_t *pool = find_pool_for_request(server, request, NULL);
    if (!pool) {
        qd_log_warn("No pool available for DISCOVER");
        server->stats.errors++;
        return QD_HANDLER_ERROR;
    }

    /* Check for existing lease */
    dhcp_lease_t *lease = dhcp_lease_find_by_mac(server->lease_db, request->chaddr);
    uint32_t offered_ip = 0;

    if (lease && dhcp_lease_is_valid(lease)) {
        /* Offer same IP if lease still valid */
        offered_ip = lease->ip;
    } else {
        /* Check for reservation */
        const dhcp_reservation_t *res = dhcp_pool_find_reservation(pool, request->chaddr);
        if (res) {
            offered_ip = res->ip;
        } else {
            /* Allocate new IP */
            offered_ip = dhcp_pool_allocate(pool, request->chaddr);
        }
    }

    if (!offered_ip) {
        qd_log_warn("No IP available for client");
        server->stats.errors++;
        return QD_HANDLER_ERROR;
    }

    /* Create/update lease in OFFERED state */
    lease = dhcp_lease_create_offered(server->lease_db, offered_ip,
                                       request->chaddr, request->xid);
    if (!lease) {
        dhcp_pool_release(pool, offered_ip);
        server->stats.errors++;
        return QD_HANDLER_ERROR;
    }

    /* Extract hostname if provided */
    char hostname[64];
    if (dhcp_option_get_string_from_packet(request, DHCP_OPT_HOSTNAME,
                                            hostname, sizeof(hostname)) > 0) {
        dhcp_lease_set_hostname(lease, hostname);
    }

    /* Send OFFER */
    struct in_addr addr = { .s_addr = offered_ip };
    qd_log_info("DHCP OFFER: %s", inet_ntoa(addr));

    send_dhcp_response(server, request, DHCP_OFFER, offered_ip, pool);

    return QD_HANDLER_OK;
}

qd_handler_result_t dhcp_server_handle_request(qd_handler_ctx_t *ctx, void *arg)
{
    dhcp_server_t *server = (dhcp_server_t *)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    server->stats.requests++;
    server->stats.packets_in++;

    qd_log_debug("DHCP REQUEST from %02x:%02x:%02x:%02x:%02x:%02x",
                 request->chaddr[0], request->chaddr[1], request->chaddr[2],
                 request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Get requested IP */
    uint32_t requested_ip = dhcp_get_requested_ip(request);
    if (!requested_ip) {
        requested_ip = request->ciaddr;
    }

    /* Get server ID if specified */
    uint32_t server_id = dhcp_get_server_id(request);
    if (server_id && server_id != server->config.server_id) {
        /* Request is for different server - ignore */
        qd_log_debug("REQUEST for different server, ignoring");
        return QD_HANDLER_OK;
    }

    /* Find lease */
    dhcp_lease_t *lease = dhcp_lease_find_by_mac(server->lease_db, request->chaddr);
    dhcp_pool_t *pool = NULL;

    if (!lease) {
        /* No lease - check if this is a rebind/renew with ciaddr */
        if (request->ciaddr) {
            pool = dhcp_server_find_pool_for_ip(server, request->ciaddr);
            if (pool && dhcp_pool_is_available(pool, request->ciaddr)) {
                /* Create new lease for rebinding client */
                const dhcp_pool_config_t *cfg = dhcp_pool_get_config(pool);
                lease = dhcp_lease_create(server->lease_db, request->ciaddr,
                                          request->chaddr, cfg->default_lease);
            }
        }
    }

    if (!lease) {
        /* No lease and not a valid rebind - NAK */
        qd_log_warn("No lease found for REQUEST - sending NAK");
        pool = find_pool_for_request(server, request, NULL);
        if (pool) {
            send_dhcp_response(server, request, DHCP_NAK, 0, pool);
        }
        return QD_HANDLER_OK;
    }

    pool = dhcp_server_find_pool_for_ip(server, lease->ip);
    if (!pool) {
        qd_log_warn("No pool for lease IP");
        return QD_HANDLER_ERROR;
    }

    /* Validate requested IP matches lease */
    if (requested_ip && requested_ip != lease->ip) {
        qd_log_warn("Requested IP doesn't match lease - sending NAK");
        send_dhcp_response(server, request, DHCP_NAK, 0, pool);
        return QD_HANDLER_OK;
    }

    /* Bind the lease */
    const dhcp_pool_config_t *cfg = dhcp_pool_get_config(pool);
    dhcp_lease_bind(server->lease_db, lease, cfg->default_lease);

    /* Mark IP as used in pool */
    dhcp_pool_mark_used(pool, lease->ip);

    struct in_addr addr = { .s_addr = lease->ip };
    qd_log_info("DHCP ACK: %s", inet_ntoa(addr));

    send_dhcp_response(server, request, DHCP_ACK, lease->ip, pool);

    return QD_HANDLER_OK;
}

qd_handler_result_t dhcp_server_handle_release(qd_handler_ctx_t *ctx, void *arg)
{
    dhcp_server_t *server = (dhcp_server_t *)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    server->stats.releases++;
    server->stats.packets_in++;

    qd_log_info("DHCP RELEASE from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    dhcp_lease_t *lease = dhcp_lease_find_by_mac(server->lease_db, request->chaddr);
    if (lease) {
        /* Release IP back to pool */
        dhcp_pool_t *pool = dhcp_server_find_pool_for_ip(server, lease->ip);
        if (pool) {
            dhcp_pool_release(pool, lease->ip);
        }

        struct in_addr addr = { .s_addr = lease->ip };
        qd_log_info("Released IP: %s", inet_ntoa(addr));

        dhcp_lease_release(server->lease_db, lease);
    }

    return QD_HANDLER_OK;
}

qd_handler_result_t dhcp_server_handle_decline(qd_handler_ctx_t *ctx, void *arg)
{
    dhcp_server_t *server = (dhcp_server_t *)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    server->stats.declines++;
    server->stats.packets_in++;

    qd_log_warn("DHCP DECLINE from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    uint32_t declined_ip = dhcp_get_requested_ip(request);
    if (!declined_ip) {
        declined_ip = request->ciaddr;
    }

    if (declined_ip) {
        /* Mark IP as declined in pool */
        dhcp_pool_t *pool = dhcp_server_find_pool_for_ip(server, declined_ip);
        if (pool) {
            dhcp_pool_mark_declined(pool, declined_ip);
        }

        /* Update lease state */
        dhcp_lease_t *lease = dhcp_lease_find_by_ip(server->lease_db, declined_ip);
        if (lease) {
            dhcp_lease_decline(server->lease_db, lease);
        }

        struct in_addr addr = { .s_addr = declined_ip };
        qd_log_warn("IP %s marked as declined", inet_ntoa(addr));
    }

    return QD_HANDLER_OK;
}

qd_handler_result_t dhcp_server_handle_inform(qd_handler_ctx_t *ctx, void *arg)
{
    dhcp_server_t *server = (dhcp_server_t *)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    server->stats.informs++;
    server->stats.packets_in++;

    qd_log_info("DHCP INFORM from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Find pool for client's IP */
    dhcp_pool_t *pool = dhcp_server_find_pool_for_ip(server, request->ciaddr);
    if (!pool) {
        pool = find_pool_for_request(server, request, NULL);
    }

    if (!pool) {
        qd_log_warn("No pool for INFORM request");
        return QD_HANDLER_ERROR;
    }

    /* Send ACK with configuration but no IP assignment */
    send_dhcp_response(server, request, DHCP_ACK, request->ciaddr, pool);

    return QD_HANDLER_OK;
}

/*
 * Packet Processing
 */

int dhcp_server_process_packet(dhcp_server_t *server,
                                const dhcp_packet_t *pkt,
                                const dhcp_rx_info_t *rx_info)
{
    if (!server || !pkt)
        return -1;

    /* Verify DHCP magic cookie */
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE) {
        server->stats.errors++;
        return -1;
    }

    /* Get message type */
    int msg_type = dhcp_get_msg_type(pkt);
    if (msg_type < 0) {
        server->stats.unknown++;
        return -1;
    }

    /* Dispatch via handler table */
    qd_handler_ctx_t ctx;
    qd_handler_ctx_init(&ctx);
    ctx.cmd = msg_type;
    ctx.data = (void *)pkt;
    ctx.len = sizeof(*pkt);
    ctx.user_data = (void *)rx_info;

    return qd_handler_dispatch(server->handlers, &ctx);
}

/*
 * Handler Table
 */

qd_handler_table_t *dhcp_server_get_handlers(dhcp_server_t *server)
{
    if (!server)
        return NULL;
    return server->handlers;
}

/*
 * Statistics
 */

void dhcp_server_get_stats(dhcp_server_t *server, dhcp_server_stats_t *stats)
{
    if (!server || !stats)
        return;
    memcpy(stats, &server->stats, sizeof(*stats));
}

void dhcp_server_reset_stats(dhcp_server_t *server)
{
    if (!server)
        return;
    memset(&server->stats, 0, sizeof(server->stats));
}

/*
 * Utility
 */

int dhcp_server_save_leases(dhcp_server_t *server)
{
    if (!server || !server->config.lease_file)
        return -1;
    return dhcp_lease_db_save(server->lease_db, server->config.lease_file);
}

int dhcp_server_load_leases(dhcp_server_t *server)
{
    if (!server || !server->config.lease_file)
        return -1;
    return dhcp_lease_db_load(server->lease_db, server->config.lease_file);
}
