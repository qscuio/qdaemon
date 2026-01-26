/*
 * QDaemon DHCP CLI Client
 * Professional modal shell for DHCP daemon management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include <qdaemon/qd_cli.h>
#include "dhcp_ipc.h"
#include "dhcp_protocol.h"

/* Shell instance */
static qd_cli_shell_t *g_shell;

/* Modes */
static qd_cli_mode_t *g_mode_root;
static qd_cli_mode_t *g_mode_enable;
static qd_cli_mode_t *g_mode_config;
static qd_cli_mode_t *g_mode_dhcp_pool;
static qd_cli_mode_t *g_mode_dhcp_relay;

/* IPC connection */
static int g_sockfd = -1;
static uint32_t g_seq = 1;

/* Current pool name for pool mode */
static char g_current_pool[32];

/*
 * IPC Communication
 */

static int ipc_connect(void)
{
    if (g_sockfd >= 0)
        return 0;

    g_sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sockfd < 0) {
        fprintf(stderr, "Cannot create socket: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DHCP_IPC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(g_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to daemon: %s\n", strerror(errno));
        fprintf(stderr, "Is qd_dhcpd running?\n");
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }

    return 0;
}

static int ipc_send_recv(dhcp_ipc_cmd_t cmd, const void *req_payload, size_t req_len,
                         void **resp_payload, size_t *resp_len)
{
    if (ipc_connect() < 0)
        return -1;

    /* Build request */
    size_t msg_size = DHCP_IPC_MSG_SIZE(req_len);
    dhcp_ipc_msg_t *req = malloc(msg_size);
    if (!req)
        return -1;

    dhcp_ipc_init_msg(req, cmd, g_seq++);
    req->header.payload_len = req_len;
    if (req_payload && req_len > 0) {
        memcpy(req->payload, req_payload, req_len);
    }

    /* Send request */
    if (send(g_sockfd, req, msg_size, 0) != (ssize_t)msg_size) {
        free(req);
        return -1;
    }
    free(req);

    /* Receive response header */
    dhcp_ipc_header_t resp_hdr;
    if (recv(g_sockfd, &resp_hdr, sizeof(resp_hdr), MSG_WAITALL) != sizeof(resp_hdr)) {
        return -1;
    }

    if (!dhcp_ipc_validate_msg((dhcp_ipc_msg_t *)&resp_hdr)) {
        fprintf(stderr, "Invalid response from daemon\n");
        return -1;
    }

    /* Receive payload */
    if (resp_hdr.payload_len > 0 && resp_payload) {
        *resp_payload = malloc(resp_hdr.payload_len);
        if (!*resp_payload)
            return -1;

        if (recv(g_sockfd, *resp_payload, resp_hdr.payload_len, MSG_WAITALL) !=
            (ssize_t)resp_hdr.payload_len) {
            free(*resp_payload);
            *resp_payload = NULL;
            return -1;
        }
        if (resp_len)
            *resp_len = resp_hdr.payload_len;
    }

    return resp_hdr.status;
}

/*
 * Helper Functions - reserved for future use
 */
__attribute__((unused))
static void print_mac(qd_cli_ctx_t *ctx, const uint8_t *mac)
{
    ctx->print("%02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

__attribute__((unused))
static void print_ip(qd_cli_ctx_t *ctx, uint32_t ip)
{
    struct in_addr addr = { .s_addr = ip };
    ctx->print("%s", inet_ntoa(addr));
}

/*
 * Root Mode Commands
 */

static qd_cli_result_t cmd_enable(qd_cli_ctx_t *ctx)
{
    qd_cli_enter_mode(ctx->shell, g_mode_enable);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_show_leases(qd_cli_ctx_t *ctx)
{
    dhcp_ipc_lease_list_t *resp = NULL;
    size_t resp_len;

    int status = ipc_send_recv(DHCP_IPC_LEASE_LIST, NULL, 0, (void **)&resp, &resp_len);
    if (status != 0) {
        ctx->error("Failed to get lease list: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    if (!resp || resp->count == 0) {
        ctx->print("No active leases\n");
        free(resp);
        return QD_CLI_OK;
    }

    ctx->print("\n");
    ctx->print("%-15s  %-17s  %-8s  %-10s  %s\n",
               "IP Address", "MAC Address", "State", "Remaining", "Hostname");
    ctx->print("---------------  -----------------  --------  ----------  --------\n");

    for (uint32_t i = 0; i < resp->count; i++) {
        dhcp_ipc_lease_entry_t *e = &resp->entries[i];
        char ip_str[16], mac_str[18];

        struct in_addr addr = { .s_addr = e->ip };
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));

        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);

        /* Calculate remaining time */
        uint64_t now = (uint64_t)time(NULL) * 1000;
        uint64_t remaining = 0;
        if (e->expire_time > now)
            remaining = (e->expire_time - now) / 1000;

        ctx->print("%-15s  %-17s  %-8s  %6lus     %s\n",
                   ip_str, mac_str,
                   dhcp_lease_state_name(e->state),
                   (unsigned long)remaining,
                   e->hostname[0] ? e->hostname : "-");
    }

    ctx->print("\nTotal: %u leases\n", resp->count);
    free(resp);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_show_status(qd_cli_ctx_t *ctx)
{
    dhcp_ipc_server_status_t *resp = NULL;
    size_t resp_len;

    int status = ipc_send_recv(DHCP_IPC_SERVER_STATUS, NULL, 0, (void **)&resp, &resp_len);
    if (status != 0) {
        ctx->error("Failed to get server status: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("\nDHCP Server Status:\n");
    ctx->print("  Running:        %s\n", resp->running ? "Yes" : "No");
    ctx->print("  Server Mode:    %s\n", resp->server_enabled ? "Enabled" : "Disabled");
    ctx->print("  Relay Mode:     %s\n", resp->relay_enabled ? "Enabled" : "Disabled");

    struct in_addr addr = { .s_addr = resp->server_id };
    ctx->print("  Server ID:      %s\n", inet_ntoa(addr));
    ctx->print("  Uptime:         %u seconds\n", resp->uptime);
    ctx->print("  Pools:          %u\n", resp->num_pools);
    ctx->print("  Active Leases:  %u\n", resp->num_leases);

    ctx->print("\nStatistics:\n");
    ctx->print("  DISCOVER:  %lu\n", (unsigned long)resp->discovers);
    ctx->print("  OFFER:     %lu\n", (unsigned long)resp->offers);
    ctx->print("  REQUEST:   %lu\n", (unsigned long)resp->requests);
    ctx->print("  ACK:       %lu\n", (unsigned long)resp->acks);
    ctx->print("  NAK:       %lu\n", (unsigned long)resp->naks);
    ctx->print("  RELEASE:   %lu\n", (unsigned long)resp->releases);
    ctx->print("  DECLINE:   %lu\n", (unsigned long)resp->declines);

    free(resp);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_ping(qd_cli_ctx_t *ctx)
{
    int status = ipc_send_recv(DHCP_IPC_PING, NULL, 0, NULL, NULL);
    if (status == 0) {
        ctx->print("Daemon is responding\n");
        return QD_CLI_OK;
    } else {
        ctx->error("Daemon not responding\n");
        return QD_CLI_ERROR;
    }
}

/*
 * Enable Mode Commands
 */

static qd_cli_result_t cmd_disable(qd_cli_ctx_t *ctx)
{
    qd_cli_exit_mode(ctx->shell);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_configure(qd_cli_ctx_t *ctx)
{
    qd_cli_enter_mode(ctx->shell, g_mode_config);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_show_pools(qd_cli_ctx_t *ctx)
{
    dhcp_ipc_pool_list_t *resp = NULL;
    size_t resp_len;

    int status = ipc_send_recv(DHCP_IPC_POOL_LIST, NULL, 0, (void **)&resp, &resp_len);
    if (status != 0) {
        ctx->error("Failed to get pool list: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    if (!resp || resp->count == 0) {
        ctx->print("No pools configured\n");
        free(resp);
        return QD_CLI_OK;
    }

    for (uint32_t i = 0; i < resp->count; i++) {
        dhcp_ipc_pool_entry_t *p = &resp->pools[i];

        ctx->print("\nPool: %s\n", p->name);

        char ip_str[16];
        struct in_addr addr;

        addr.s_addr = p->network;
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        ctx->print("  Network:      %s", ip_str);

        addr.s_addr = p->netmask;
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        ctx->print(" / %s\n", ip_str);

        addr.s_addr = p->range_start;
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        ctx->print("  Range:        %s - ", ip_str);

        addr.s_addr = p->range_end;
        inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
        ctx->print("%s\n", ip_str);

        if (p->gateway) {
            addr.s_addr = p->gateway;
            inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
            ctx->print("  Gateway:      %s\n", ip_str);
        }

        if (p->num_dns > 0) {
            ctx->print("  DNS:          ");
            for (uint32_t j = 0; j < p->num_dns; j++) {
                addr.s_addr = p->dns_servers[j];
                inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
                ctx->print("%s%s", ip_str, j < p->num_dns - 1 ? ", " : "\n");
            }
        }

        if (p->domain[0])
            ctx->print("  Domain:       %s\n", p->domain);

        ctx->print("  Lease Time:   %u seconds\n", p->default_lease);
        ctx->print("  Utilization:  %u / %u (%.1f%%)\n",
                   p->allocated_ips, p->total_ips,
                   p->total_ips > 0 ? (100.0 * p->allocated_ips / p->total_ips) : 0.0);
    }

    free(resp);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_show_relay(qd_cli_ctx_t *ctx)
{
    dhcp_ipc_relay_status_t *resp = NULL;
    size_t resp_len;

    int status = ipc_send_recv(DHCP_IPC_RELAY_STATUS, NULL, 0, (void **)&resp, &resp_len);
    if (status != 0) {
        ctx->error("Failed to get relay status: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("\nDHCP Relay Status:\n");
    ctx->print("  Enabled:         %s\n", resp->enabled ? "Yes" : "No");
    ctx->print("  Option 82:       Insert=%s, Replace=%s\n",
               resp->option82_insert ? "Yes" : "No",
               resp->option82_replace ? "Yes" : "No");

    ctx->print("\nRelay Servers (%d):\n", resp->num_servers);
    for (int i = 0; i < resp->num_servers; i++) {
        struct in_addr addr = { .s_addr = resp->servers[i] };
        ctx->print("  %d. %s\n", i + 1, inet_ntoa(addr));
    }

    ctx->print("\nRelay Interfaces (%d):\n", resp->num_interfaces);
    for (int i = 0; i < resp->num_interfaces; i++) {
        if (resp->interfaces[i].name[0]) {
            struct in_addr addr = { .s_addr = resp->interfaces[i].giaddr };
            ctx->print("  %s: giaddr=%s %s\n",
                       resp->interfaces[i].name,
                       inet_ntoa(addr),
                       resp->interfaces[i].enabled ? "(enabled)" : "(disabled)");
        }
    }

    ctx->print("\nStatistics:\n");
    ctx->print("  Requests Forwarded:  %lu\n", (unsigned long)resp->requests_forwarded);
    ctx->print("  Replies Relayed:     %lu\n", (unsigned long)resp->replies_relayed);
    ctx->print("  Drops (Max Hops):    %lu\n", (unsigned long)resp->drops_max_hops);
    ctx->print("  Drops (No Server):   %lu\n", (unsigned long)resp->drops_no_server);

    free(resp);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_clear_lease(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: clear dhcp lease <ip|all>");
        return QD_CLI_ERROR;
    }

    dhcp_ipc_cmd_t cmd;
    dhcp_ipc_lease_clear_t req;
    memset(&req, 0, sizeof(req));

    if (strcmp(ctx->argv[0], "all") == 0) {
        cmd = DHCP_IPC_LEASE_CLEAR_ALL;
    } else {
        cmd = DHCP_IPC_LEASE_CLEAR_IP;
        if (inet_pton(AF_INET, ctx->argv[0], &req.ip) != 1) {
            ctx->error("Invalid IP address: %s", ctx->argv[0]);
            return QD_CLI_ERROR;
        }
    }

    int status = ipc_send_recv(cmd, &req, sizeof(req), NULL, NULL);
    if (status != 0) {
        ctx->error("Failed to clear lease: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("Lease(s) cleared\n");
    return QD_CLI_OK;
}

/*
 * Config Mode Commands
 */

static qd_cli_result_t cmd_ip_dhcp_pool(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: ip dhcp pool <name>");
        return QD_CLI_ERROR;
    }

    strncpy(g_current_pool, ctx->argv[0], sizeof(g_current_pool) - 1);
    qd_cli_enter_mode(ctx->shell, g_mode_dhcp_pool);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_ip_dhcp_relay(qd_cli_ctx_t *ctx)
{
    (void)ctx;
    qd_cli_enter_mode(ctx->shell, g_mode_dhcp_relay);
    return QD_CLI_MODE_CHANGE;
}

/*
 * Pool Mode Commands
 */

static qd_cli_result_t cmd_pool_network(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 2) {
        ctx->error("Usage: network <ip> <mask>");
        return QD_CLI_ERROR;
    }

    ctx->print("Setting network for pool %s: %s %s\n",
               g_current_pool, ctx->argv[0], ctx->argv[1]);

    /* TODO: Send to daemon */
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_pool_range(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 2) {
        ctx->error("Usage: range <start-ip> <end-ip>");
        return QD_CLI_ERROR;
    }

    dhcp_ipc_pool_range_t req;
    memset(&req, 0, sizeof(req));
    size_t pool_len = strlen(g_current_pool);
    if (pool_len >= sizeof(req.pool_name))
        pool_len = sizeof(req.pool_name) - 1;
    memcpy(req.pool_name, g_current_pool, pool_len);

    if (inet_pton(AF_INET, ctx->argv[0], &req.range_start) != 1) {
        ctx->error("Invalid start IP: %s", ctx->argv[0]);
        return QD_CLI_ERROR;
    }

    if (inet_pton(AF_INET, ctx->argv[1], &req.range_end) != 1) {
        ctx->error("Invalid end IP: %s", ctx->argv[1]);
        return QD_CLI_ERROR;
    }

    int status = ipc_send_recv(DHCP_IPC_POOL_SET_RANGE, &req, sizeof(req), NULL, NULL);
    if (status != 0) {
        ctx->error("Failed to set range: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("Range configured for pool %s\n", g_current_pool);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_pool_gateway(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: default-router <ip>");
        return QD_CLI_ERROR;
    }

    ctx->print("Setting gateway for pool %s: %s\n", g_current_pool, ctx->argv[0]);
    /* TODO: Send to daemon */
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_pool_dns(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: dns-server <ip> [ip2] [ip3] [ip4]");
        return QD_CLI_ERROR;
    }

    ctx->print("Setting DNS for pool %s:", g_current_pool);
    for (int i = 0; i < ctx->argc && i < 4; i++) {
        ctx->print(" %s", ctx->argv[i]);
    }
    ctx->print("\n");
    /* TODO: Send to daemon */
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_pool_domain(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: domain-name <domain>");
        return QD_CLI_ERROR;
    }

    ctx->print("Setting domain for pool %s: %s\n", g_current_pool, ctx->argv[0]);
    /* TODO: Send to daemon */
    return QD_CLI_OK;
}

/*
 * Relay Mode Commands
 */

static qd_cli_result_t cmd_relay_server(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: ip dhcp-server <ip>");
        return QD_CLI_ERROR;
    }

    dhcp_ipc_relay_server_t req;
    if (inet_pton(AF_INET, ctx->argv[0], &req.server_ip) != 1) {
        ctx->error("Invalid IP address: %s", ctx->argv[0]);
        return QD_CLI_ERROR;
    }

    int status = ipc_send_recv(DHCP_IPC_RELAY_ADD_SERVER, &req, sizeof(req), NULL, NULL);
    if (status != 0) {
        ctx->error("Failed to add server: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("Relay server %s added\n", ctx->argv[0]);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_relay_interface(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 2) {
        ctx->error("Usage: interface <name> <giaddr>");
        return QD_CLI_ERROR;
    }

    dhcp_ipc_relay_interface_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, ctx->argv[0], sizeof(req.name) - 1);

    if (inet_pton(AF_INET, ctx->argv[1], &req.giaddr) != 1) {
        ctx->error("Invalid giaddr: %s", ctx->argv[1]);
        return QD_CLI_ERROR;
    }

    int status = ipc_send_recv(DHCP_IPC_RELAY_ADD_INTERFACE, &req, sizeof(req), NULL, NULL);
    if (status != 0) {
        ctx->error("Failed to add interface: %s", dhcp_ipc_status_str(status));
        return QD_CLI_ERROR;
    }

    ctx->print("Relay interface %s added with giaddr %s\n", ctx->argv[0], ctx->argv[1]);
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_relay_option82(qd_cli_ctx_t *ctx)
{
    ctx->print("Option 82 insertion enabled\n");
    /* TODO: Send to daemon */
    return QD_CLI_OK;
}

/*
 * Command Registration
 */

static void register_commands(void)
{
    /* Root mode */
    g_mode_root = qd_cli_get_root_mode(g_shell);
    qd_cli_register_cmd(g_mode_root, "enable", cmd_enable, "Enter privileged mode");
    qd_cli_register_cmd(g_mode_root, "show dhcp leases", cmd_show_leases, "Show DHCP leases");
    qd_cli_register_cmd(g_mode_root, "show dhcp status", cmd_show_status, "Show server status");
    qd_cli_register_cmd(g_mode_root, "ping", cmd_ping, "Test daemon connectivity");

    /* Enable mode */
    g_mode_enable = qd_cli_mode_create(g_shell, "enable", "#", "Privileged EXEC");
    qd_cli_mode_set_parent(g_mode_enable, g_mode_root);
    qd_cli_register_cmd(g_mode_enable, "disable", cmd_disable, "Exit privileged mode");
    qd_cli_register_cmd(g_mode_enable, "configure terminal", cmd_configure, "Enter configuration mode");
    qd_cli_register_cmd(g_mode_enable, "show dhcp leases", cmd_show_leases, "Show DHCP leases");
    qd_cli_register_cmd(g_mode_enable, "show dhcp status", cmd_show_status, "Show server status");
    qd_cli_register_cmd(g_mode_enable, "show dhcp pool", cmd_show_pools, "Show pool configuration");
    qd_cli_register_cmd(g_mode_enable, "show dhcp relay", cmd_show_relay, "Show relay status");
    qd_cli_register_cmd(g_mode_enable, "clear dhcp lease", cmd_clear_lease, "Clear DHCP leases");

    /* Config mode */
    g_mode_config = qd_cli_mode_create(g_shell, "config", "(config)", "Configuration");
    qd_cli_mode_set_parent(g_mode_config, g_mode_enable);
    qd_cli_register_cmd(g_mode_config, "ip dhcp pool", cmd_ip_dhcp_pool, "Configure DHCP pool");
    qd_cli_register_cmd(g_mode_config, "ip dhcp relay", cmd_ip_dhcp_relay, "Configure relay agent");

    /* Pool mode */
    g_mode_dhcp_pool = qd_cli_mode_create(g_shell, "dhcp-pool", "(dhcp-pool)", "DHCP Pool Config");
    qd_cli_mode_set_parent(g_mode_dhcp_pool, g_mode_config);
    qd_cli_register_cmd(g_mode_dhcp_pool, "network", cmd_pool_network, "Set pool network");
    qd_cli_register_cmd(g_mode_dhcp_pool, "range", cmd_pool_range, "Set address range");
    qd_cli_register_cmd(g_mode_dhcp_pool, "default-router", cmd_pool_gateway, "Set default gateway");
    qd_cli_register_cmd(g_mode_dhcp_pool, "dns-server", cmd_pool_dns, "Set DNS servers");
    qd_cli_register_cmd(g_mode_dhcp_pool, "domain-name", cmd_pool_domain, "Set domain name");

    /* Relay mode */
    g_mode_dhcp_relay = qd_cli_mode_create(g_shell, "dhcp-relay", "(dhcp-relay)", "DHCP Relay Config");
    qd_cli_mode_set_parent(g_mode_dhcp_relay, g_mode_config);
    qd_cli_register_cmd(g_mode_dhcp_relay, "ip dhcp-server", cmd_relay_server, "Add relay server");
    qd_cli_register_cmd(g_mode_dhcp_relay, "interface", cmd_relay_interface, "Add relay interface");
    qd_cli_register_cmd(g_mode_dhcp_relay, "option-82 insert", cmd_relay_option82, "Enable option 82");
}

/*
 * Main
 */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    qd_cli_config_t config = QD_CLI_CONFIG_DEFAULT;
    config.name = "DHCP Shell";
    config.version = "1.0";
    config.base_prompt = "dhcp";
    config.history_file = ".dhcp_cli_history";

    g_shell = qd_cli_create(&config);
    if (!g_shell) {
        fprintf(stderr, "Failed to create CLI shell\n");
        return 1;
    }

    register_commands();

    printf("QDaemon DHCP CLI\n");
    printf("Type 'enable' for privileged mode, 'help' for commands\n\n");

    qd_cli_run(g_shell);

    qd_cli_destroy(g_shell);

    if (g_sockfd >= 0)
        close(g_sockfd);

    return 0;
}
