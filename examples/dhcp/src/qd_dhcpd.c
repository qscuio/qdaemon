/*
 * QDaemon DHCP Server - Main Daemon
 * Professional DHCP server with relay support, CLI, and REST API
 *
 * Uses raw socket bound to qdhcp0 virtual device for packet I/O.
 * Kernel module intercepts DHCP packets and forwards them via qdhcp0
 * with metadata header prepended.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#ifdef STANDALONE
#include "qdaemon_stub.h"
#else
#include <qdaemon/qdaemon.h>
#include <qdaemon/qd_handler.h>
#include <qdaemon/qd_netlink.h>
#include <qdaemon/qd_ipc.h>
#endif

#include "dhcp_protocol.h"
#include "dhcp_server.h"
#include "dhcp_relay.h"
#include "dhcp_pool.h"
#include "dhcp_lease.h"
#include "dhcp_ipc.h"
#include "dhcp_kmod.h"
#include "qd_dhcp_nl.h"

/*
 * Daemon Context
 */
typedef struct {
    /* Framework components */
    qd_daemon_t        *daemon;
    qd_event_loop_t    *loop;
    qd_dhcp_nl_t       *dhcp_nl;       /* DHCP netlink wrapper */

    /* Raw socket for packet I/O on qdhcp0 */
    int                 raw_sock;
    int                 qdhcp_ifindex;

    /* DHCP components */
    dhcp_server_t      *server;
    dhcp_relay_t       *relay;

    /* IPC Server */
    int                 ipc_fd;
    qd_handler_table_t *ipc_handlers;

    /* Configuration */
    struct {
        bool            server_enabled;
        bool            relay_enabled;
        bool            foreground;
        char            config_file[256];
        char            lease_file[256];
    } config;

    /* Runtime state */
    uint8_t             server_mac[6];
    uint64_t            start_time;

    /* Packet buffer */
    uint8_t             pkt_buf[QDHCP_MAX_PKT_SIZE];

} qd_dhcpd_t;

static qd_dhcpd_t g_dhcpd;

/*
 * Forward declarations
 */
static int send_dhcp_packet_cb(const uint8_t *packet, size_t len,
                                const uint8_t *dst_mac, uint32_t dst_ip,
                                bool broadcast, void *arg);
static void on_raw_socket_readable(int fd, uint32_t events, void *arg);
static int setup_raw_socket(void);

/* REST API initialization (from dhcp_rest.c) */
extern void dhcp_rest_init(dhcp_server_t *server, dhcp_relay_t *relay);

/*
 * IPC Handlers
 */

static qd_handler_result_t ipc_handle_ping(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    ctx->status = DHCP_IPC_OK;
    return QD_HANDLER_OK;
}

/* Callback for lease iteration */
static int lease_list_iter_cb(dhcp_lease_t *lease, void *arg)
{
    dhcp_ipc_lease_list_t *resp = (dhcp_ipc_lease_list_t *)arg;
    dhcp_ipc_lease_entry_t *e = &resp->entries[resp->count];
    e->ip = lease->ip;
    memcpy(e->mac, lease->mac, 6);
    e->state = lease->state;
    e->lease_time = lease->lease_time;
    e->start_time = lease->start_time;
    e->expire_time = lease->expire_time;
    memset(e->hostname, 0, sizeof(e->hostname));
    memcpy(e->hostname, lease->hostname,
           strlen(lease->hostname) < sizeof(e->hostname) - 1 ?
           strlen(lease->hostname) : sizeof(e->hostname) - 1);
    resp->count++;
    return 0;
}

static qd_handler_result_t ipc_handle_lease_list(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    dhcp_lease_db_t *db = dhcp_server_get_lease_db(g_dhcpd.server);

    /* Count leases */
    int count = dhcp_lease_count(db);

    /* Allocate response */
    size_t resp_size = sizeof(dhcp_ipc_lease_list_t) +
                       count * sizeof(dhcp_ipc_lease_entry_t);
    dhcp_ipc_lease_list_t *resp = qd_handler_ctx_alloc_response(ctx, resp_size);
    if (!resp) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_ERROR;
    }

    resp->count = 0;

    /* Fill in lease entries */
    dhcp_lease_iterate(db, lease_list_iter_cb, resp);

    ctx->response_len = sizeof(dhcp_ipc_lease_list_t) +
                        resp->count * sizeof(dhcp_ipc_lease_entry_t);
    ctx->status = DHCP_IPC_OK;

    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_lease_clear_all(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    dhcp_server_clear_all_leases(g_dhcpd.server);

    ctx->status = DHCP_IPC_OK;
    qd_log_info("All leases cleared via IPC");

    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_lease_clear_ip(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    dhcp_ipc_lease_clear_t *req = (dhcp_ipc_lease_clear_t *)ctx->data;

    if (dhcp_server_clear_lease(g_dhcpd.server, req->ip) != 0) {
        ctx->status = DHCP_IPC_ERR_NOT_FOUND;
        return QD_HANDLER_OK;
    }

    struct in_addr addr = { .s_addr = req->ip };
    qd_log_info("Lease cleared via IPC: %s", inet_ntoa(addr));

    ctx->status = DHCP_IPC_OK;
    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_server_status(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    dhcp_ipc_server_status_t *resp = qd_handler_ctx_alloc_response(ctx, sizeof(*resp));
    if (!resp) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_ERROR;
    }

    memset(resp, 0, sizeof(*resp));

    resp->running = 1;
    resp->server_enabled = g_dhcpd.config.server_enabled;
    resp->relay_enabled = g_dhcpd.config.relay_enabled;

    const dhcp_server_config_t *cfg = dhcp_server_get_config(g_dhcpd.server);
    resp->server_id = cfg->server_id;

    uint64_t now = qd_time_now();
    resp->uptime = (now - g_dhcpd.start_time) / 1000;

    resp->num_pools = dhcp_server_get_pool_count(g_dhcpd.server);
    resp->num_leases = dhcp_lease_count(dhcp_server_get_lease_db(g_dhcpd.server));

    dhcp_server_stats_t stats;
    dhcp_server_get_stats(g_dhcpd.server, &stats);
    resp->discovers = stats.discovers;
    resp->offers = stats.offers;
    resp->requests = stats.requests;
    resp->acks = stats.acks;
    resp->naks = stats.naks;
    resp->releases = stats.releases;
    resp->declines = stats.declines;

    ctx->response_len = sizeof(*resp);
    ctx->status = DHCP_IPC_OK;

    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_relay_status(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    dhcp_ipc_relay_status_t *resp = qd_handler_ctx_alloc_response(ctx, sizeof(*resp));
    if (!resp) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_ERROR;
    }

    memset(resp, 0, sizeof(*resp));

    if (g_dhcpd.relay) {
        resp->enabled = dhcp_relay_is_enabled(g_dhcpd.relay);

        /* Use local array to avoid packed member address warning */
        uint32_t servers[DHCP_RELAY_MAX_SERVERS];
        resp->num_servers = dhcp_relay_get_servers(g_dhcpd.relay,
                                                    servers,
                                                    DHCP_RELAY_MAX_SERVERS);
        memcpy(resp->servers, servers, sizeof(resp->servers));

        dhcp_relay_iface_t ifaces[DHCP_RELAY_MAX_IFACES];
        resp->num_interfaces = dhcp_relay_get_interfaces(g_dhcpd.relay,
                                                          ifaces,
                                                          DHCP_RELAY_MAX_IFACES);
        for (int i = 0; i < resp->num_interfaces; i++) {
            memset(resp->interfaces[i].name, 0, sizeof(resp->interfaces[i].name));
            size_t name_len = strlen(ifaces[i].name);
            if (name_len >= sizeof(resp->interfaces[i].name))
                name_len = sizeof(resp->interfaces[i].name) - 1;
            memcpy(resp->interfaces[i].name, ifaces[i].name, name_len);
            resp->interfaces[i].giaddr = ifaces[i].giaddr;
            resp->interfaces[i].enabled = ifaces[i].enabled;
        }

        dhcp_relay_stats_t stats;
        dhcp_relay_get_stats(g_dhcpd.relay, &stats);
        resp->requests_forwarded = stats.requests_forwarded;
        resp->replies_relayed = stats.replies_relayed;
        resp->drops_max_hops = stats.drops_max_hops;
        resp->drops_no_server = stats.drops_no_server;
    }

    ctx->response_len = sizeof(*resp);
    ctx->status = DHCP_IPC_OK;

    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_relay_add_server(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    if (!g_dhcpd.relay) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_OK;
    }

    dhcp_ipc_relay_server_t *req = (dhcp_ipc_relay_server_t *)ctx->data;

    if (dhcp_relay_add_server(g_dhcpd.relay, req->server_ip) != 0) {
        ctx->status = DHCP_IPC_ERR_FULL;
        return QD_HANDLER_OK;
    }

    ctx->status = DHCP_IPC_OK;
    return QD_HANDLER_OK;
}

static qd_handler_result_t ipc_handle_relay_add_interface(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;

    if (!g_dhcpd.relay) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_OK;
    }

    dhcp_ipc_relay_interface_t *req = (dhcp_ipc_relay_interface_t *)ctx->data;

    if (dhcp_relay_add_interface(g_dhcpd.relay, req->name, req->giaddr) != 0) {
        ctx->status = DHCP_IPC_ERR_FAILED;
        return QD_HANDLER_OK;
    }

    ctx->status = DHCP_IPC_OK;
    return QD_HANDLER_OK;
}

/* IPC Handler Table */
static const qd_handler_entry_t ipc_handler_entries[] = {
    QD_HANDLER(DHCP_IPC_PING,                 ipc_handle_ping),
    QD_HANDLER(DHCP_IPC_LEASE_LIST,           ipc_handle_lease_list),
    QD_HANDLER(DHCP_IPC_LEASE_CLEAR_ALL,      ipc_handle_lease_clear_all),
    QD_HANDLER(DHCP_IPC_LEASE_CLEAR_IP,       ipc_handle_lease_clear_ip),
    QD_HANDLER(DHCP_IPC_SERVER_STATUS,        ipc_handle_server_status),
    QD_HANDLER(DHCP_IPC_RELAY_STATUS,         ipc_handle_relay_status),
    QD_HANDLER(DHCP_IPC_RELAY_ADD_SERVER,     ipc_handle_relay_add_server),
    QD_HANDLER(DHCP_IPC_RELAY_ADD_INTERFACE,  ipc_handle_relay_add_interface),
    QD_HANDLER_END()
};

/*
 * Netlink Notification Handlers
 */

static qd_dhcp_nl_handler_result_t on_binding_add(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    (void)arg;
    const qd_dhcp_binding_t *b = ctx->binding;
    if (!b) return QD_DHCP_NL_HANDLER_ERROR;

    char mac_str[18], ip_str[16];
    qd_dhcp_nl_mac_str(b->mac, mac_str, sizeof(mac_str));
    qd_dhcp_nl_ip_str(b->ip, ip_str, sizeof(ip_str));

    qd_log_info("Kernel binding added: %s -> %s (ifindex=%d, lease=%u)",
                mac_str, ip_str, b->ifindex, b->lease_time);

    return QD_DHCP_NL_HANDLER_OK;
}

static qd_dhcp_nl_handler_result_t on_binding_del(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    (void)arg;
    const qd_dhcp_binding_t *b = ctx->binding;
    if (!b) return QD_DHCP_NL_HANDLER_ERROR;

    char mac_str[18], ip_str[16];
    qd_dhcp_nl_mac_str(b->mac, mac_str, sizeof(mac_str));
    qd_dhcp_nl_ip_str(b->ip, ip_str, sizeof(ip_str));

    qd_log_info("Kernel binding removed: %s -> %s", mac_str, ip_str);

    return QD_DHCP_NL_HANDLER_OK;
}

static qd_dhcp_nl_handler_result_t on_iface_add(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    (void)arg;
    const qd_dhcp_iface_t *iface = ctx->iface;
    if (!iface) return QD_DHCP_NL_HANDLER_ERROR;

    qd_log_info("Kernel interface added: %s (ifindex=%d, trusted=%d)",
                iface->ifname, iface->ifindex, iface->trusted);

    return QD_DHCP_NL_HANDLER_OK;
}

static qd_dhcp_nl_handler_result_t on_iface_del(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    (void)arg;
    const qd_dhcp_iface_t *iface = ctx->iface;
    if (!iface) return QD_DHCP_NL_HANDLER_ERROR;

    qd_log_info("Kernel interface removed: %s (ifindex=%d)",
                iface->ifname, iface->ifindex);

    return QD_DHCP_NL_HANDLER_OK;
}

/*
 * Dispatch notification based on event type
 */
static qd_dhcp_nl_handler_result_t on_binding_notify(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    if (ctx->event == QD_DHCP_EVENT_BINDING_ADD)
        return on_binding_add(ctx, arg);
    else if (ctx->event == QD_DHCP_EVENT_BINDING_DEL)
        return on_binding_del(ctx, arg);
    return QD_DHCP_NL_HANDLER_OK;
}

static qd_dhcp_nl_handler_result_t on_iface_notify(qd_dhcp_nl_ctx_t *ctx, void *arg)
{
    if (ctx->event == QD_DHCP_EVENT_IFACE_ADD)
        return on_iface_add(ctx, arg);
    else if (ctx->event == QD_DHCP_EVENT_IFACE_DEL)
        return on_iface_del(ctx, arg);
    return QD_DHCP_NL_HANDLER_OK;
}

/* Netlink notification handler table */
static const qd_dhcp_nl_handler_entry_t nl_handler_entries[] = {
    QD_DHCP_NL_HANDLER(QD_DHCP_CMD_NOTIFY_BINDING, on_binding_notify),
    QD_DHCP_NL_HANDLER(QD_DHCP_CMD_NOTIFY_IFACE, on_iface_notify),
    QD_DHCP_NL_HANDLER_END()
};

/*
 * IPC Server
 */

static void on_ipc_readable(int fd, uint32_t events, void *arg)
{
    (void)events;
    (void)arg;

    /* Accept new connection */
    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0)
        return;

    /* Receive request */
    dhcp_ipc_header_t header;
    if (recv(client_fd, &header, sizeof(header), MSG_WAITALL) != sizeof(header)) {
        close(client_fd);
        return;
    }

    if (header.magic != DHCP_IPC_MAGIC) {
        close(client_fd);
        return;
    }

    /* Receive payload */
    void *payload = NULL;
    if (header.payload_len > 0) {
        payload = malloc(header.payload_len);
        if (!payload || recv(client_fd, payload, header.payload_len, MSG_WAITALL) !=
            (ssize_t)header.payload_len) {
            free(payload);
            close(client_fd);
            return;
        }
    }

    /* Dispatch handler */
    qd_handler_ctx_t ctx;
    qd_handler_ctx_init(&ctx);
    ctx.cmd = header.cmd;
    ctx.data = payload;
    ctx.len = header.payload_len;
    ctx.seq = header.seq;

    qd_handler_dispatch(g_dhcpd.ipc_handlers, &ctx);

    /* Send response */
    dhcp_ipc_header_t resp_header = {
        .magic = DHCP_IPC_MAGIC,
        .version = DHCP_IPC_VERSION,
        .cmd = header.cmd,
        .seq = header.seq,
        .status = ctx.status,
        .payload_len = ctx.response_len,
    };

    send(client_fd, &resp_header, sizeof(resp_header), 0);
    if (ctx.response && ctx.response_len > 0) {
        send(client_fd, ctx.response, ctx.response_len, 0);
    }

    free(payload);
    free(ctx.response);
    close(client_fd);
}

static int setup_ipc_server(void)
{
    /* Create Unix socket */
    g_dhcpd.ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_dhcpd.ipc_fd < 0) {
        qd_log_error("Cannot create IPC socket: %s", strerror(errno));
        return -1;
    }

    /* Remove existing socket file */
    unlink(DHCP_IPC_SOCKET_PATH);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, DHCP_IPC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_dhcpd.ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        qd_log_error("Cannot bind IPC socket: %s", strerror(errno));
        close(g_dhcpd.ipc_fd);
        return -1;
    }

    if (listen(g_dhcpd.ipc_fd, 5) < 0) {
        qd_log_error("Cannot listen on IPC socket: %s", strerror(errno));
        close(g_dhcpd.ipc_fd);
        return -1;
    }

    /* Create handler table */
    g_dhcpd.ipc_handlers = qd_handler_table_create("dhcp_ipc");
    qd_handler_register_table(g_dhcpd.ipc_handlers, ipc_handler_entries);

    /* Register with event loop (loop already set up by raw socket init) */
    qd_event_add(g_dhcpd.loop, g_dhcpd.ipc_fd, QD_EVENT_READ, on_ipc_readable, NULL);

    qd_log_info("IPC server listening on %s", DHCP_IPC_SOCKET_PATH);

    return 0;
}

/*
 * Raw Socket Setup - Bind to qdhcp0 virtual device
 */

static int setup_raw_socket(void)
{
    /* Create raw socket */
    g_dhcpd.raw_sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (g_dhcpd.raw_sock < 0) {
        qd_log_error("Cannot create raw socket: %s", strerror(errno));
        return -1;
    }

    /* Get interface index for qdhcp0 */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, QDHCP_DEV_NAME, IFNAMSIZ - 1);

    if (ioctl(g_dhcpd.raw_sock, SIOCGIFINDEX, &ifr) < 0) {
        qd_log_error("Cannot get interface index for %s: %s",
                     QDHCP_DEV_NAME, strerror(errno));
        qd_log_error("Make sure to load the module: insmod qd_dhcp_kmod.ko");
        close(g_dhcpd.raw_sock);
        g_dhcpd.raw_sock = -1;
        return -1;
    }
    g_dhcpd.qdhcp_ifindex = ifr.ifr_ifindex;

    /* Bind to the qdhcp0 interface */
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = g_dhcpd.qdhcp_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(g_dhcpd.raw_sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        qd_log_error("Cannot bind raw socket to %s: %s",
                     QDHCP_DEV_NAME, strerror(errno));
        close(g_dhcpd.raw_sock);
        g_dhcpd.raw_sock = -1;
        return -1;
    }

    /* Bring interface up */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, QDHCP_DEV_NAME, IFNAMSIZ - 1);
    if (ioctl(g_dhcpd.raw_sock, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(g_dhcpd.raw_sock, SIOCSIFFLAGS, &ifr);
    }

    qd_log_info("Raw socket bound to %s (ifindex=%d)",
                QDHCP_DEV_NAME, g_dhcpd.qdhcp_ifindex);

    return 0;
}

/*
 * Packet Send Callback - Send via raw socket with metadata header
 */

static int send_dhcp_packet_cb(const uint8_t *packet, size_t len,
                                const uint8_t *dst_mac, uint32_t dst_ip,
                                bool broadcast, void *arg)
{
    int ifindex = (arg != NULL) ? *(int *)arg : 0;

    if (g_dhcpd.raw_sock < 0)
        return -1;

    /* Build full packet with metadata header */
    uint8_t tx_buf[QDHCP_MAX_PKT_SIZE];
    size_t offset = 0;

    /* Metadata header */
    struct qdhcp_meta_hdr *meta = (struct qdhcp_meta_hdr *)tx_buf;
    memset(meta, 0, sizeof(*meta));
    meta->magic = QDHCP_META_MAGIC;
    meta->version = QDHCP_META_VERSION;
    meta->direction = QDHCP_DIR_TX;
    meta->ifindex = ifindex;
    meta->flags = broadcast ? QDHCP_TX_BROADCAST : QDHCP_TX_UNICAST;

    /* Set MAC addresses */
    memcpy(meta->src_mac, g_dhcpd.server_mac, 6);
    if (broadcast) {
        memset(meta->dst_mac, 0xff, 6);
    } else {
        memcpy(meta->dst_mac, dst_mac, 6);
    }

    offset += sizeof(struct qdhcp_meta_hdr);

    /* Ethernet header */
    eth_header_t *eth = (eth_header_t *)(tx_buf + offset);
    memcpy(eth->dst, meta->dst_mac, 6);
    memcpy(eth->src, meta->src_mac, 6);
    eth->type = htons(0x0800);
    offset += sizeof(eth_header_t);

    /* IP header */
    ip_header_t *ip = (ip_header_t *)(tx_buf + offset);
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->tot_len = htons(sizeof(ip_header_t) + sizeof(udp_header_t) + len);
    ip->id = htons(rand());
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = 17;  /* UDP */
    ip->check = 0;

    const dhcp_server_config_t *cfg = dhcp_server_get_config(g_dhcpd.server);
    ip->saddr = cfg->server_id;
    ip->daddr = broadcast ? INADDR_BROADCAST : dst_ip;
    offset += sizeof(ip_header_t);

    /* UDP header */
    udp_header_t *udp = (udp_header_t *)(tx_buf + offset);
    udp->source = htons(DHCP_SERVER_PORT);
    udp->dest = htons(DHCP_CLIENT_PORT);
    udp->len = htons(sizeof(udp_header_t) + len);
    udp->check = 0;
    offset += sizeof(udp_header_t);

    /* DHCP payload */
    memcpy(tx_buf + offset, packet, len);
    offset += len;

    /* Update packet length in metadata */
    meta->pkt_len = offset - sizeof(struct qdhcp_meta_hdr);

    /* Send via raw socket to qdhcp0 */
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = g_dhcpd.qdhcp_ifindex;
    sll.sll_halen = 6;
    memcpy(sll.sll_addr, meta->dst_mac, 6);

    ssize_t sent = sendto(g_dhcpd.raw_sock, tx_buf, offset, 0,
                          (struct sockaddr *)&sll, sizeof(sll));
    if (sent < 0) {
        qd_log_error("Failed to send packet: %s", strerror(errno));
        return -1;
    }

    qd_log_debug("TX: %zu bytes to ifindex %d", offset, ifindex);
    return 0;
}

/*
 * Packet Reception - Handle packets from raw socket on qdhcp0
 */

static void on_raw_socket_readable(int fd, uint32_t events, void *arg)
{
    (void)events;
    (void)arg;

    /* Read packet with metadata header */
    ssize_t recv_len = recv(fd, g_dhcpd.pkt_buf, sizeof(g_dhcpd.pkt_buf), 0);
    if (recv_len < (ssize_t)sizeof(struct qdhcp_meta_hdr)) {
        return;
    }

    /* Parse metadata header */
    struct qdhcp_meta_hdr *meta = (struct qdhcp_meta_hdr *)g_dhcpd.pkt_buf;

    /* Validate metadata */
    if (!QDHCP_META_VALID(meta)) {
        qd_log_debug("RX: Invalid metadata magic/version");
        return;
    }

    /* Only process RX direction (kernel to userspace) */
    if (meta->direction != QDHCP_DIR_RX) {
        return;
    }

    /* Get packet data after metadata */
    uint8_t *pkt_data = g_dhcpd.pkt_buf + sizeof(struct qdhcp_meta_hdr);
    size_t pkt_len = meta->pkt_len;

    if (pkt_len < sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t)) {
        qd_log_debug("RX: Packet too short");
        return;
    }

    /* Skip Ethernet/IP/UDP headers to get DHCP payload */
    size_t header_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);
    if (pkt_len < header_len + sizeof(dhcp_packet_t)) {
        qd_log_debug("RX: DHCP packet too short");
        return;
    }

    dhcp_packet_t *dhcp = (dhcp_packet_t *)(pkt_data + header_len);

    /* Verify DHCP magic */
    if (ntohl(dhcp->magic) != DHCP_MAGIC_COOKIE) {
        qd_log_debug("RX: Invalid DHCP magic");
        return;
    }

    /* Extract RX info from metadata and packet headers */
    dhcp_rx_info_t rx_info;
    memset(&rx_info, 0, sizeof(rx_info));

    rx_info.ifindex = meta->ifindex;
    memcpy(rx_info.src_mac, meta->src_mac, 6);
    memcpy(rx_info.dst_mac, meta->dst_mac, 6);

    ip_header_t *ip = (ip_header_t *)(pkt_data + sizeof(eth_header_t));
    rx_info.src_ip = ip->saddr;
    rx_info.dst_ip = ip->daddr;
    rx_info.is_broadcast = (meta->dst_mac[0] == 0xff);

    qd_log_debug("RX: DHCP %s from %02x:%02x:%02x:%02x:%02x:%02x on ifindex %d (method=%d, trusted=%d)",
                 dhcp_msg_type_name(meta->msg_type),
                 meta->src_mac[0], meta->src_mac[1], meta->src_mac[2],
                 meta->src_mac[3], meta->src_mac[4], meta->src_mac[5],
                 meta->ifindex, meta->method, meta->trusted);

    /* Store ifindex for send callback */
    int ifindex = meta->ifindex;

    /* Check if this is a relay packet or direct client request */
    if (dhcp->giaddr != 0 && g_dhcpd.relay) {
        /* Server reply from upstream - relay to client */
        dhcp_relay_process_server_packet(g_dhcpd.relay, dhcp, pkt_len - header_len);
    } else if (g_dhcpd.config.relay_enabled && g_dhcpd.relay && dhcp->op == DHCP_BOOTREQUEST) {
        /* Client request - check if we should relay */
        dhcp_relay_process_client_packet(g_dhcpd.relay, dhcp,
                                          pkt_len - header_len, ifindex);
    } else if (g_dhcpd.config.server_enabled) {
        /* Process as server - pass ifindex in arg for send callback */
        dhcp_server_set_send_callback(g_dhcpd.server, send_dhcp_packet_cb, &ifindex);
        dhcp_server_process_packet(g_dhcpd.server, dhcp, &rx_info);
    }
}

/*
 * Daemon Callbacks
 */

static int on_init(qd_daemon_t *daemon, void *arg)
{
    (void)arg;

    qd_log_info("DHCP daemon initializing...");

    g_dhcpd.daemon = daemon;
    g_dhcpd.start_time = qd_time_now();

    /* Create DHCP server */
    dhcp_server_config_t server_cfg = DHCP_SERVER_CONFIG_DEFAULT;
    inet_pton(AF_INET, "192.168.100.1", &server_cfg.server_id);

    g_dhcpd.server = dhcp_server_create(&server_cfg);
    if (!g_dhcpd.server) {
        qd_log_error("Failed to create DHCP server");
        return -1;
    }

    /* Set send callback */
    dhcp_server_set_send_callback(g_dhcpd.server, send_dhcp_packet_cb, NULL);

    /* Create default pool */
    dhcp_pool_config_t pool_cfg = DHCP_POOL_CONFIG_DEFAULT;
    strcpy(pool_cfg.name, "default");
    inet_pton(AF_INET, "192.168.100.0", &pool_cfg.network);
    inet_pton(AF_INET, "255.255.255.0", &pool_cfg.netmask);
    inet_pton(AF_INET, "192.168.100.10", &pool_cfg.range_start);
    inet_pton(AF_INET, "192.168.100.250", &pool_cfg.range_end);
    inet_pton(AF_INET, "192.168.100.1", &pool_cfg.gateway);
    inet_pton(AF_INET, "8.8.8.8", &pool_cfg.dns_servers[0]);
    pool_cfg.num_dns = 1;
    strcpy(pool_cfg.domain, "local");
    pool_cfg.default_lease = 3600;
    pool_cfg.max_lease = 86400;

    dhcp_pool_t *pool = dhcp_pool_create(&pool_cfg);
    if (pool) {
        dhcp_server_add_pool(g_dhcpd.server, pool);
    }

    /* Create relay agent */
    dhcp_relay_config_t relay_cfg = DHCP_RELAY_CONFIG_DEFAULT;
    g_dhcpd.relay = dhcp_relay_create(&relay_cfg);

    /* Setup raw socket for packet I/O on qdhcp0 */
    if (setup_raw_socket() != 0) {
        qd_log_error("Failed to setup raw socket");
        qd_log_error("Make sure to load the module: insmod qd_dhcp_kmod.ko");
        return -1;
    }

    /* Register raw socket with event loop */
    g_dhcpd.loop = qd_daemon_get_loop(g_dhcpd.daemon);
    qd_event_add(g_dhcpd.loop, g_dhcpd.raw_sock, QD_EVENT_READ,
                 on_raw_socket_readable, NULL);

    /* Connect to kernel module via netlink wrapper API */
    g_dhcpd.dhcp_nl = qd_dhcp_nl_create();
    if (!g_dhcpd.dhcp_nl) {
        qd_log_warn("DHCP netlink connection failed - control commands unavailable");
        /* Continue anyway - raw socket is sufficient for basic operation */
    } else {
        /* Set user data for handlers */
        qd_dhcp_nl_set_user_data(g_dhcpd.dhcp_nl, &g_dhcpd);

        /* Register notification handlers */
        if (qd_dhcp_nl_register_handlers(g_dhcpd.dhcp_nl, nl_handler_entries) != QD_DHCP_NL_OK) {
            qd_log_warn("Failed to register netlink handlers");
        }

        /* Attach to event loop */
        if (qd_dhcp_nl_attach(g_dhcpd.dhcp_nl, g_dhcpd.loop) != QD_DHCP_NL_OK) {
            qd_log_warn("Failed to attach netlink to event loop");
        }

        /* Subscribe to kernel notifications */
        if (qd_dhcp_nl_subscribe(g_dhcpd.dhcp_nl) != QD_DHCP_NL_OK) {
            qd_log_debug("Multicast subscription not available");
        }

        /* Register daemon with kernel */
        if (qd_dhcp_nl_register(g_dhcpd.dhcp_nl) != QD_DHCP_NL_OK) {
            qd_log_warn("Failed to register with kernel module");
        }
    }

    /* Setup IPC server */
    if (setup_ipc_server() != 0) {
        qd_log_error("Failed to setup IPC server");
        return -1;
    }

    /* Initialize REST API */
    dhcp_rest_init(g_dhcpd.server, g_dhcpd.relay);

    /* Generate server MAC */
    g_dhcpd.server_mac[0] = 0x02;  /* Locally administered */
    for (int i = 1; i < 6; i++)
        g_dhcpd.server_mac[i] = rand() & 0xff;

    g_dhcpd.config.server_enabled = true;
    g_dhcpd.config.relay_enabled = false;

    qd_log_info("DHCP daemon initialized successfully");
    qd_log_info("  Server mode: enabled");
    qd_log_info("  Relay mode: disabled");
    qd_log_info("  Pools: %d", dhcp_server_get_pool_count(g_dhcpd.server));

    return 0;
}

static void on_shutdown(qd_daemon_t *daemon, void *arg)
{
    (void)daemon;
    (void)arg;

    qd_log_info("DHCP daemon shutting down...");

    /* Close raw socket */
    if (g_dhcpd.raw_sock >= 0) {
        qd_event_del_fd(g_dhcpd.loop, g_dhcpd.raw_sock);
        close(g_dhcpd.raw_sock);
        g_dhcpd.raw_sock = -1;
    }

    /* Close IPC */
    if (g_dhcpd.ipc_fd >= 0) {
        qd_event_del_fd(g_dhcpd.loop, g_dhcpd.ipc_fd);
        close(g_dhcpd.ipc_fd);
        unlink(DHCP_IPC_SOCKET_PATH);
    }

    if (g_dhcpd.ipc_handlers)
        qd_handler_table_destroy(g_dhcpd.ipc_handlers);

    /* Unregister from kernel via netlink wrapper */
    if (g_dhcpd.dhcp_nl) {
        qd_dhcp_nl_unregister(g_dhcpd.dhcp_nl);
        qd_dhcp_nl_destroy(g_dhcpd.dhcp_nl);
        g_dhcpd.dhcp_nl = NULL;
    }

    /* Destroy DHCP components */
    if (g_dhcpd.relay)
        dhcp_relay_destroy(g_dhcpd.relay);

    if (g_dhcpd.server)
        dhcp_server_destroy(g_dhcpd.server);

    qd_log_info("DHCP daemon shutdown complete");
}

/*
 * Usage
 */

static void print_usage(const char *prog)
{
    printf("QDaemon DHCP Server v1.0\n");
    printf("\n");
    printf("Usage: %s [options]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -f, --foreground   Run in foreground\n");
    printf("  -c <file>          Configuration file\n");
    printf("  -l <file>          Lease file path\n");
    printf("  -h, --help         Show this help\n");
    printf("\n");
    printf("Modes:\n");
    printf("  Server mode: Provides DHCP addresses to clients\n");
    printf("  Relay mode:  Forwards DHCP requests to upstream server\n");
    printf("\n");
    printf("Use qd_dhcp_cli for runtime management.\n");
}

/*
 * Main
 */

int main(int argc, char **argv)
{
    memset(&g_dhcpd, 0, sizeof(g_dhcpd));
    g_dhcpd.ipc_fd = -1;
    g_dhcpd.raw_sock = -1;

    /* Default configuration */
    strcpy(g_dhcpd.config.lease_file, "/var/lib/qd_dhcpd/leases");

    /* Daemon configuration */
    qd_daemon_config_t config = QD_DAEMON_CONFIG_DEFAULT;
    config.name = "qd_dhcpd";
    config.version = "1.0.0";
    config.description = "QDaemon Professional DHCP Server";
    config.pid_file = "/tmp/qd_dhcpd.pid";
    config.log_to_stderr = 1;
    config.log_level = QD_LOG_DEBUG;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--foreground") == 0) {
            config.foreground = 1;
            g_dhcpd.config.foreground = true;
        }
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            strncpy(g_dhcpd.config.config_file, argv[++i],
                    sizeof(g_dhcpd.config.config_file) - 1);
        }
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            strncpy(g_dhcpd.config.lease_file, argv[++i],
                    sizeof(g_dhcpd.config.lease_file) - 1);
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
