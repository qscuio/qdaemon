/*
 * QDaemon DHCP Server - Main Daemon
 * Uses QDaemon framework with custom netdev kernel module
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include <qdaemon/qdaemon.h>
#include <qdaemon/qd_handler.h>
#include <qdaemon/qd_netlink.h>

#include "dhcp_protocol.h"

/* Netlink commands matching kernel module */
enum {
    QDHCP_CMD_UNSPEC,
    QDHCP_CMD_REGISTER,
    QDHCP_CMD_UNREGISTER,
    QDHCP_CMD_PACKET_UP,
    QDHCP_CMD_PACKET_DOWN,
    QDHCP_CMD_SET_CONFIG,
    QDHCP_CMD_GET_STATS,
};

/* Netlink attributes matching kernel module */
enum {
    QDHCP_ATTR_UNSPEC,
    QDHCP_ATTR_PACKET,
    QDHCP_ATTR_IFINDEX,
    QDHCP_ATTR_TIMESTAMP,
    QDHCP_ATTR_MAC_SRC,
    QDHCP_ATTR_MAC_DST,
    QDHCP_ATTR_PKT_TYPE,
    QDHCP_ATTR_MTU,
    QDHCP_ATTR_STATS,
};

/* DHCP Server State */
typedef struct {
    qd_daemon_t *daemon;
    qd_netlink_t *netlink;
    qd_handler_table_t *handlers;
    
    dhcp_config_t config;
    dhcp_lease_t *leases;
    int num_leases;
    
    uint8_t server_mac[6];
} qd_dhcpd_t;

static qd_dhcpd_t g_dhcpd;

/*
 * Lease Management
 */

static dhcp_lease_t *lease_find_by_mac(const uint8_t *mac)
{
    dhcp_lease_t *lease = g_dhcpd.leases;
    while (lease) {
        if (memcmp(lease->mac, mac, 6) == 0)
            return lease;
        lease = lease->next;
    }
    return NULL;
}

static dhcp_lease_t *lease_find_by_ip(uint32_t ip)
{
    dhcp_lease_t *lease = g_dhcpd.leases;
    while (lease) {
        if (lease->ip == ip)
            return lease;
        lease = lease->next;
    }
    return NULL;
}

static uint32_t lease_allocate_ip(const uint8_t *mac)
{
    /* Check if client already has a lease */
    dhcp_lease_t *existing = lease_find_by_mac(mac);
    if (existing && existing->state != LEASE_EXPIRED)
        return existing->ip;

    /* Find free IP in pool */
    for (uint32_t ip = g_dhcpd.config.pool_start; 
         ip <= g_dhcpd.config.pool_end; ip++) {
        uint32_t net_ip = htonl(ip);
        dhcp_lease_t *lease = lease_find_by_ip(net_ip);
        if (!lease || lease->state == LEASE_FREE || lease->state == LEASE_EXPIRED)
            return net_ip;
    }

    return 0; /* No free IP */
}

static dhcp_lease_t *lease_create(uint32_t ip, const uint8_t *mac, uint32_t lease_time)
{
    dhcp_lease_t *lease = calloc(1, sizeof(dhcp_lease_t));
    if (!lease)
        return NULL;

    lease->ip = ip;
    memcpy(lease->mac, mac, 6);
    lease->lease_time = lease_time;
    lease->expire_time = qd_time_now() + (lease_time * 1000);
    lease->state = LEASE_OFFERED;

    /* Add to list */
    lease->next = g_dhcpd.leases;
    g_dhcpd.leases = lease;
    g_dhcpd.num_leases++;

    return lease;
}

/*
 * DHCP Packet Building
 */

static int build_dhcp_reply(dhcp_packet_t *reply, const dhcp_packet_t *request,
                            uint8_t msg_type, uint32_t offered_ip)
{
    memset(reply, 0, sizeof(*reply));

    reply->op = DHCP_BOOTREPLY;
    reply->htype = DHCP_HTYPE_ETHER;
    reply->hlen = 6;
    reply->xid = request->xid;
    reply->yiaddr = offered_ip;
    reply->siaddr = g_dhcpd.config.server_ip;
    memcpy(reply->chaddr, request->chaddr, 16);
    reply->magic = htonl(DHCP_MAGIC_COOKIE);

    /* Build options */
    uint8_t *opt = reply->options;
    int idx = 0;

    /* Message type */
    opt[idx++] = DHCP_OPT_MSG_TYPE;
    opt[idx++] = 1;
    opt[idx++] = msg_type;

    /* Server identifier */
    opt[idx++] = DHCP_OPT_SERVER_ID;
    opt[idx++] = 4;
    memcpy(&opt[idx], &g_dhcpd.config.server_ip, 4);
    idx += 4;

    /* Lease time */
    opt[idx++] = DHCP_OPT_LEASE_TIME;
    opt[idx++] = 4;
    uint32_t lease_time = htonl(g_dhcpd.config.default_lease);
    memcpy(&opt[idx], &lease_time, 4);
    idx += 4;

    /* Subnet mask */
    opt[idx++] = DHCP_OPT_SUBNET_MASK;
    opt[idx++] = 4;
    memcpy(&opt[idx], &g_dhcpd.config.subnet_mask, 4);
    idx += 4;

    /* Router */
    if (g_dhcpd.config.gateway) {
        opt[idx++] = DHCP_OPT_ROUTER;
        opt[idx++] = 4;
        memcpy(&opt[idx], &g_dhcpd.config.gateway, 4);
        idx += 4;
    }

    /* DNS */
    if (g_dhcpd.config.dns_server) {
        opt[idx++] = DHCP_OPT_DNS;
        opt[idx++] = 4;
        memcpy(&opt[idx], &g_dhcpd.config.dns_server, 4);
        idx += 4;
    }

    /* End */
    opt[idx++] = DHCP_OPT_END;

    return sizeof(dhcp_packet_t) - 308 + idx;
}

/*
 * Send packet via kernel netdev
 */

static int send_dhcp_packet(dhcp_packet_t *pkt, size_t len,
                            const uint8_t *dst_mac, uint32_t dst_ip)
{
    /* Build full ethernet/IP/UDP packet */
    uint8_t packet[1500];
    size_t offset = 0;

    /* Ethernet header */
    eth_header_t *eth = (eth_header_t *)packet;
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, g_dhcpd.server_mac, 6);
    eth->type = htons(0x0800); /* IP */
    offset += sizeof(eth_header_t);

    /* IP header */
    ip_header_t *ip = (ip_header_t *)(packet + offset);
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->tot_len = htons(sizeof(ip_header_t) + sizeof(udp_header_t) + len);
    ip->id = htons(rand());
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = 17; /* UDP */
    ip->saddr = g_dhcpd.config.server_ip;
    ip->daddr = dst_ip;
    ip->check = 0; /* Let kernel calculate */
    offset += sizeof(ip_header_t);

    /* UDP header */
    udp_header_t *udp = (udp_header_t *)(packet + offset);
    udp->source = htons(DHCP_SERVER_PORT);
    udp->dest = htons(DHCP_CLIENT_PORT);
    udp->len = htons(sizeof(udp_header_t) + len);
    udp->check = 0;
    offset += sizeof(udp_header_t);

    /* DHCP payload */
    memcpy(packet + offset, pkt, len);
    offset += len;

    /* Send via netlink to kernel module */
    qd_netlink_msg_t msg;
    qd_netlink_msg_init(&msg, QDHCP_CMD_PACKET_DOWN);
    qd_netlink_msg_add_attr(&msg, QDHCP_ATTR_PACKET, packet, offset);

    return qd_netlink_send_msg(g_dhcpd.netlink, &msg);
}

/*
 * DHCP Message Handlers
 */

static qd_handler_result_t handle_discover(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    qd_log_info("DHCP DISCOVER from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Allocate IP address */
    uint32_t offered_ip = lease_allocate_ip(request->chaddr);
    if (!offered_ip) {
        qd_log_warn("No IP available for client");
        return QD_HANDLER_ERROR;
    }

    /* Create lease */
    dhcp_lease_t *lease = lease_find_by_ip(offered_ip);
    if (!lease) {
        lease = lease_create(offered_ip, request->chaddr, g_dhcpd.config.default_lease);
        if (!lease)
            return QD_HANDLER_ERROR;
    }
    lease->state = LEASE_OFFERED;

    /* Build and send OFFER */
    dhcp_packet_t reply;
    int len = build_dhcp_reply(&reply, request, DHCP_OFFER, offered_ip);

    /* Send to broadcast or unicast */
    uint8_t dst_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    send_dhcp_packet(&reply, len, dst_mac, INADDR_BROADCAST);

    struct in_addr addr = { .s_addr = offered_ip };
    qd_log_info("DHCP OFFER sent: %s", inet_ntoa(addr));

    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_request(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    qd_log_info("DHCP REQUEST from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Find requested IP */
    uint32_t requested_ip = dhcp_get_requested_ip(request);
    if (!requested_ip)
        requested_ip = request->ciaddr;

    /* Find lease */
    dhcp_lease_t *lease = lease_find_by_mac(request->chaddr);
    
    uint8_t msg_type;
    if (lease && lease->ip == requested_ip) {
        /* ACK the request */
        lease->state = LEASE_BOUND;
        lease->expire_time = qd_time_now() + (lease->lease_time * 1000);
        msg_type = DHCP_ACK;
        qd_log_info("DHCP ACK");
    } else {
        /* NAK - client requested wrong IP */
        msg_type = DHCP_NAK;
        requested_ip = 0;
        qd_log_warn("DHCP NAK - invalid request");
    }

    /* Build and send reply */
    dhcp_packet_t reply;
    int len = build_dhcp_reply(&reply, request, msg_type, requested_ip);

    uint8_t dst_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    send_dhcp_packet(&reply, len, dst_mac, INADDR_BROADCAST);

    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_release(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    qd_log_info("DHCP RELEASE from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    dhcp_lease_t *lease = lease_find_by_mac(request->chaddr);
    if (lease) {
        lease->state = LEASE_FREE;
        struct in_addr addr = { .s_addr = lease->ip };
        qd_log_info("Released IP: %s", inet_ntoa(addr));
    }

    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_decline(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    qd_log_warn("DHCP DECLINE from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Mark IP as problematic */
    uint32_t declined_ip = dhcp_get_requested_ip(request);
    dhcp_lease_t *lease = lease_find_by_ip(declined_ip);
    if (lease) {
        lease->state = LEASE_EXPIRED;
    }

    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_inform(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_packet_t *request = (dhcp_packet_t *)ctx->data;

    qd_log_info("DHCP INFORM from %02x:%02x:%02x:%02x:%02x:%02x",
                request->chaddr[0], request->chaddr[1], request->chaddr[2],
                request->chaddr[3], request->chaddr[4], request->chaddr[5]);

    /* Send ACK with network config but no IP */
    dhcp_packet_t reply;
    int len = build_dhcp_reply(&reply, request, DHCP_ACK, request->ciaddr);

    send_dhcp_packet(&reply, len, request->chaddr, request->ciaddr);

    return QD_HANDLER_OK;
}

/* DHCP handler table */
static const qd_handler_entry_t dhcp_handlers[] = {
    QD_HANDLER(DHCP_DISCOVER, handle_discover),
    QD_HANDLER(DHCP_REQUEST,  handle_request),
    QD_HANDLER(DHCP_RELEASE,  handle_release),
    QD_HANDLER(DHCP_DECLINE,  handle_decline),
    QD_HANDLER(DHCP_INFORM,   handle_inform),
    QD_HANDLER_END()
};

/*
 * Netlink packet handler
 */

static void on_packet_received(qd_netlink_t *nl, qd_netlink_msg_t *msg, void *arg)
{
    (void)nl;
    (void)arg;

    if (msg->cmd != QDHCP_CMD_PACKET_UP)
        return;

    /* Get packet data */
    size_t pkt_len;
    void *pkt_data = qd_netlink_msg_get_data(msg, QDHCP_ATTR_PACKET, &pkt_len);
    if (!pkt_data || pkt_len < sizeof(eth_header_t) + sizeof(ip_header_t) + 
                                sizeof(udp_header_t) + sizeof(dhcp_packet_t)) {
        return;
    }

    /* Skip ethernet, IP, UDP headers to get DHCP packet */
    size_t header_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);
    dhcp_packet_t *dhcp = (dhcp_packet_t *)((uint8_t *)pkt_data + header_len);

    /* Verify DHCP magic */
    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE)
        return;

    /* Get message type and dispatch */
    int msg_type = dhcp_get_msg_type(dhcp);
    if (msg_type < 0)
        return;

    qd_handler_ctx_t ctx;
    qd_handler_ctx_init(&ctx);
    ctx.cmd = msg_type;
    ctx.data = dhcp;
    ctx.len = pkt_len - header_len;

    qd_handler_dispatch(g_dhcpd.handlers, &ctx);
}

/*
 * Daemon callbacks
 */

static int on_init(qd_daemon_t *daemon, void *arg)
{
    (void)arg;
    qd_log_info("DHCP server initializing...");

    g_dhcpd.daemon = daemon;

    /* Create handler table */
    g_dhcpd.handlers = qd_handler_table_create("dhcp");
    qd_handler_register_table(g_dhcpd.handlers, dhcp_handlers);

    /* Connect to kernel module */
    g_dhcpd.netlink = qd_netlink_create("QDHCP");
    if (!g_dhcpd.netlink) {
        qd_log_error("Failed to connect to QDHCP kernel module");
        qd_log_error("Make sure to load the module: insmod qd_dhcp_kmod.ko");
        return -1;
    }

    /* Set packet callback */
    qd_netlink_set_callback(g_dhcpd.netlink, on_packet_received, NULL);

    /* Register with kernel module */
    qd_netlink_msg_t msg;
    qd_netlink_msg_init(&msg, QDHCP_CMD_REGISTER);
    if (qd_netlink_send_msg(g_dhcpd.netlink, &msg) != QD_OK) {
        qd_log_error("Failed to register with kernel module");
        return -1;
    }

    qd_log_info("DHCP server initialized, listening on qdhcp0");
    return 0;
}

static void on_shutdown(qd_daemon_t *daemon, void *arg)
{
    (void)daemon;
    (void)arg;
    qd_log_info("DHCP server shutting down...");

    /* Unregister from kernel */
    if (g_dhcpd.netlink) {
        qd_netlink_msg_t msg;
        qd_netlink_msg_init(&msg, QDHCP_CMD_UNREGISTER);
        qd_netlink_send_msg(g_dhcpd.netlink, &msg);
        qd_netlink_destroy(g_dhcpd.netlink);
    }

    /* Cleanup handler table */
    if (g_dhcpd.handlers)
        qd_handler_table_destroy(g_dhcpd.handlers);

    /* Free leases */
    dhcp_lease_t *lease = g_dhcpd.leases;
    while (lease) {
        dhcp_lease_t *next = lease->next;
        free(lease);
        lease = next;
    }
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -f, --foreground   Run in foreground\n");
    printf("  -c <config>        Configuration file\n");
    printf("  -h, --help         Show this help\n");
}

int main(int argc, char **argv)
{
    /* Default configuration */
    memset(&g_dhcpd, 0, sizeof(g_dhcpd));
    
    /* Default DHCP settings - 192.168.100.0/24 */
    inet_pton(AF_INET, "192.168.100.1", &g_dhcpd.config.server_ip);
    inet_pton(AF_INET, "255.255.255.0", &g_dhcpd.config.subnet_mask);
    inet_pton(AF_INET, "192.168.100.1", &g_dhcpd.config.gateway);
    inet_pton(AF_INET, "8.8.8.8", &g_dhcpd.config.dns_server);
    g_dhcpd.config.pool_start = ntohl(inet_addr("192.168.100.10"));
    g_dhcpd.config.pool_end = ntohl(inet_addr("192.168.100.250"));
    g_dhcpd.config.default_lease = 3600;  /* 1 hour */
    g_dhcpd.config.max_lease = 86400;     /* 24 hours */

    /* Generate server MAC */
    g_dhcpd.server_mac[0] = 0x02;  /* Locally administered */
    for (int i = 1; i < 6; i++)
        g_dhcpd.server_mac[i] = rand() & 0xff;

    /* Parse arguments */
    qd_daemon_config_t config = QD_DAEMON_CONFIG_DEFAULT;
    config.name = "qd_dhcpd";
    config.version = "1.0.0";
    config.description = "QDaemon DHCP Server";
    config.pid_file = "/tmp/qd_dhcpd.pid";
    config.log_to_stderr = 1;
    config.log_level = QD_LOG_DEBUG;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            config.foreground = 1;
        }
    }

    /* Create daemon */
    qd_daemon_t *daemon = qd_daemon_create(&config);
    if (!daemon) {
        fprintf(stderr, "Failed to create daemon\n");
        return 1;
    }

    qd_daemon_set_init_callback(daemon, on_init, NULL);
    qd_daemon_set_shutdown_callback(daemon, on_shutdown, NULL);

    /* Initialize and run */
    if (qd_daemon_init(daemon) != QD_OK) {
        qd_daemon_destroy(daemon);
        return 1;
    }

    qd_daemon_run(daemon);
    qd_daemon_destroy(daemon);

    return 0;
}
