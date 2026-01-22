/*
 * QDaemon Meta Server - Example daemon using AF_QDMETA
 * Uses custom socket family for BOTH control AND data packets
 * NO netdev - pure socket-based communication
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>

#include <qdaemon/qdaemon.h>
#include <qdaemon/qd_handler.h>

#include "qd_meta_sock.h"

/* Server port */
#define META_SERVER_CTRL_PORT   5000
#define META_SERVER_DATA_PORT   5001

/* Control message commands */
enum {
    META_CMD_ECHO = 1,
    META_CMD_STATS,
    META_CMD_PING,
    META_CMD_CONFIG_SET,
    META_CMD_CONFIG_GET,
    META_CMD_SUBSCRIBE,
    META_CMD_UNSUBSCRIBE,
};

/* Data packet types */
enum {
    META_PKT_DATA = 100,
    META_PKT_KEEP_ALIVE,
    META_PKT_ACK,
};

/* Message header (common for control and data) */
typedef struct {
    uint32_t type;      /* Command or packet type */
    uint32_t seq;       /* Sequence number */
    uint32_t len;       /* Payload length */
    uint32_t flags;     /* Flags */
    uint8_t  data[];    /* Payload */
} __attribute__((packed)) meta_msg_t;

/* Statistics */
typedef struct {
    uint64_t ctrl_rx;
    uint64_t ctrl_tx;
    uint64_t data_rx;
    uint64_t data_tx;
    uint64_t bytes_rx;
    uint64_t bytes_tx;
    uint64_t errors;
} meta_stats_t;

/* Server state */
typedef struct {
    qd_daemon_t *daemon;
    qd_handler_table_t *ctrl_handlers;
    qd_handler_table_t *data_handlers;
    
    int ctrl_fd;        /* Control socket */
    int data_fd;        /* Data socket */
    
    meta_stats_t stats;
    uint32_t seq;
    
    void *event_ctrl;
    void *event_data;
} meta_server_t;

static meta_server_t g_server;

/*
 * Control Message Handlers
 */

static qd_handler_result_t handle_echo(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    meta_msg_t *msg = (meta_msg_t *)ctx->data;
    
    qd_log_debug("CTRL ECHO: seq=%u len=%u", msg->seq, msg->len);
    
    /* Echo back the same message */
    size_t resp_len = sizeof(meta_msg_t) + msg->len;
    meta_msg_t *resp = (meta_msg_t *)qd_handler_ctx_alloc_response(ctx, resp_len);
    if (!resp)
        return QD_HANDLER_ERROR;
    
    resp->type = META_CMD_ECHO;
    resp->seq = msg->seq;
    resp->len = msg->len;
    resp->flags = 0;
    memcpy(resp->data, msg->data, msg->len);
    
    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_stats(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    meta_msg_t *msg = (meta_msg_t *)ctx->data;
    
    qd_log_debug("CTRL STATS: seq=%u", msg->seq);
    
    /* Build stats response */
    size_t resp_len = sizeof(meta_msg_t) + sizeof(meta_stats_t);
    meta_msg_t *resp = (meta_msg_t *)qd_handler_ctx_alloc_response(ctx, resp_len);
    if (!resp)
        return QD_HANDLER_ERROR;
    
    resp->type = META_CMD_STATS;
    resp->seq = msg->seq;
    resp->len = sizeof(meta_stats_t);
    resp->flags = 0;
    memcpy(resp->data, &g_server.stats, sizeof(meta_stats_t));
    
    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_ping(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    meta_msg_t *msg = (meta_msg_t *)ctx->data;
    
    qd_log_debug("CTRL PING: seq=%u", msg->seq);
    
    /* Send PONG response */
    size_t resp_len = sizeof(meta_msg_t);
    meta_msg_t *resp = (meta_msg_t *)qd_handler_ctx_alloc_response(ctx, resp_len);
    if (!resp)
        return QD_HANDLER_ERROR;
    
    resp->type = META_CMD_PING;
    resp->seq = msg->seq;
    resp->len = 0;
    resp->flags = 1;  /* PONG flag */
    
    return QD_HANDLER_OK;
}

/* Control handler table */
static const qd_handler_entry_t ctrl_handlers[] = {
    QD_HANDLER(META_CMD_ECHO,   handle_echo),
    QD_HANDLER(META_CMD_STATS,  handle_stats),
    QD_HANDLER(META_CMD_PING,   handle_ping),
    QD_HANDLER_END()
};

/*
 * Data Packet Handlers
 */

static qd_handler_result_t handle_data_packet(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    meta_msg_t *msg = (meta_msg_t *)ctx->data;
    
    qd_log_debug("DATA PKT: seq=%u len=%u", msg->seq, msg->len);
    
    /* Echo data back with ACK */
    size_t resp_len = sizeof(meta_msg_t) + msg->len;
    meta_msg_t *resp = (meta_msg_t *)qd_handler_ctx_alloc_response(ctx, resp_len);
    if (!resp)
        return QD_HANDLER_ERROR;
    
    resp->type = META_PKT_ACK;
    resp->seq = msg->seq;
    resp->len = msg->len;
    resp->flags = 0;
    memcpy(resp->data, msg->data, msg->len);
    
    return QD_HANDLER_OK;
}

static qd_handler_result_t handle_keepalive(qd_handler_ctx_t *ctx, void *arg)
{
    (void)arg;
    meta_msg_t *msg = (meta_msg_t *)ctx->data;
    
    qd_log_trace("DATA KEEPALIVE: seq=%u", msg->seq);
    
    /* Send keepalive response */
    size_t resp_len = sizeof(meta_msg_t);
    meta_msg_t *resp = (meta_msg_t *)qd_handler_ctx_alloc_response(ctx, resp_len);
    if (!resp)
        return QD_HANDLER_ERROR;
    
    resp->type = META_PKT_KEEP_ALIVE;
    resp->seq = msg->seq;
    resp->len = 0;
    resp->flags = 1;  /* Response flag */
    
    return QD_HANDLER_OK;
}

/* Data handler table */
static const qd_handler_entry_t data_handlers[] = {
    QD_HANDLER(META_PKT_DATA,       handle_data_packet),
    QD_HANDLER(META_PKT_KEEP_ALIVE, handle_keepalive),
    QD_HANDLER_END()
};

/*
 * Socket event handlers
 */

static void process_message(int fd, qd_handler_table_t *handlers, 
                            meta_stats_t *rx_stat, meta_stats_t *tx_stat)
{
    uint8_t buf[4096];
    struct qdmeta_meta meta;
    struct sockaddr_qdmeta from;
    
    ssize_t n = qdmeta_recvmeta(fd, buf, sizeof(buf), &meta, &from);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            qd_log_error("recv error: %s", strerror(errno));
            g_server.stats.errors++;
        }
        return;
    }
    
    if (n < (ssize_t)sizeof(meta_msg_t)) {
        qd_log_warn("Short message: %zd bytes", n);
        return;
    }
    
    (*rx_stat)++;
    g_server.stats.bytes_rx += n;
    
    meta_msg_t *msg = (meta_msg_t *)buf;
    
    if (meta.have_ts) {
        qd_log_trace("Meta: ts=%lu.%09lu cpu=%u seq=%lu",
                     (unsigned long)meta.ts.sec, (unsigned long)meta.ts.nsec,
                     meta.cpu.cpu_id, (unsigned long)meta.seq.seq);
    }
    
    /* Dispatch to handler */
    qd_handler_ctx_t ctx;
    qd_handler_ctx_init(&ctx);
    ctx.cmd = msg->type;
    ctx.data = msg;
    ctx.len = n;
    ctx.seq = msg->seq;
    if (meta.have_ts)
        ctx.timestamp = meta.ts.sec * 1000000000ULL + meta.ts.nsec;
    
    qd_handler_result_t result = qd_handler_dispatch(handlers, &ctx);
    
    /* Send response if handler provided one */
    if (result == QD_HANDLER_OK && ctx.response && ctx.response_len > 0) {
        ssize_t sent = qdmeta_sendto(fd, ctx.response, ctx.response_len,
                                      from.sqm_port, from.sqm_addr);
        if (sent > 0) {
            (*tx_stat)++;
            g_server.stats.bytes_tx += sent;
        }
    }
    
    /* Free response if allocated */
    if (ctx.response)
        free(ctx.response);
}

static void on_ctrl_readable(int fd, uint32_t events, void *arg)
{
    (void)events;
    (void)arg;
    
    process_message(fd, g_server.ctrl_handlers,
                    &g_server.stats.ctrl_rx, &g_server.stats.ctrl_tx);
}

static void on_data_readable(int fd, uint32_t events, void *arg)
{
    (void)events;
    (void)arg;
    
    process_message(fd, g_server.data_handlers,
                    &g_server.stats.data_rx, &g_server.stats.data_tx);
}

/*
 * Daemon callbacks
 */

static int on_init(qd_daemon_t *daemon, void *arg)
{
    (void)arg;
    qd_log_info("Meta server initializing...");
    
    g_server.daemon = daemon;
    
    /* Create handler tables */
    g_server.ctrl_handlers = qd_handler_table_create("meta_ctrl");
    qd_handler_register_table(g_server.ctrl_handlers, ctrl_handlers);
    
    g_server.data_handlers = qd_handler_table_create("meta_data");
    qd_handler_register_table(g_server.data_handlers, data_handlers);
    
    /* Create control socket */
    g_server.ctrl_fd = qdmeta_socket(SOCK_DGRAM, QDMETA_PROTO_CTRL);
    if (g_server.ctrl_fd < 0) {
        qd_log_error("Failed to create control socket: %s", strerror(errno));
        qd_log_error("Is the qd_af_meta.ko kernel module loaded?");
        return -1;
    }
    
    /* Bind control socket */
    if (qdmeta_bind(g_server.ctrl_fd, META_SERVER_CTRL_PORT, 0, QDMETA_PROTO_CTRL) < 0) {
        qd_log_error("Failed to bind control socket: %s", strerror(errno));
        close(g_server.ctrl_fd);
        return -1;
    }
    
    /* Create data socket */
    g_server.data_fd = qdmeta_socket(SOCK_DGRAM, QDMETA_PROTO_DATA);
    if (g_server.data_fd < 0) {
        qd_log_error("Failed to create data socket: %s", strerror(errno));
        close(g_server.ctrl_fd);
        return -1;
    }
    
    /* Bind data socket */
    if (qdmeta_bind(g_server.data_fd, META_SERVER_DATA_PORT, 0, QDMETA_PROTO_DATA) < 0) {
        qd_log_error("Failed to bind data socket: %s", strerror(errno));
        close(g_server.data_fd);
        close(g_server.ctrl_fd);
        return -1;
    }
    
    /* Add sockets to event loop */
    qd_event_loop_t *loop = qd_daemon_get_loop(daemon);
    
    g_server.event_ctrl = qd_event_add(loop, g_server.ctrl_fd, 
                                        QD_EVENT_READ, on_ctrl_readable, NULL);
    g_server.event_data = qd_event_add(loop, g_server.data_fd,
                                        QD_EVENT_READ, on_data_readable, NULL);
    
    qd_log_info("Meta server initialized");
    qd_log_info("  Control socket: port %d (protocol %d)", 
                META_SERVER_CTRL_PORT, QDMETA_PROTO_CTRL);
    qd_log_info("  Data socket: port %d (protocol %d)", 
                META_SERVER_DATA_PORT, QDMETA_PROTO_DATA);
    
    return 0;
}

static void on_shutdown(qd_daemon_t *daemon, void *arg)
{
    (void)daemon;
    (void)arg;
    
    qd_log_info("Meta server shutting down...");
    
    /* Close sockets */
    if (g_server.ctrl_fd >= 0)
        close(g_server.ctrl_fd);
    if (g_server.data_fd >= 0)
        close(g_server.data_fd);
    
    /* Cleanup handler tables */
    if (g_server.ctrl_handlers)
        qd_handler_table_destroy(g_server.ctrl_handlers);
    if (g_server.data_handlers)
        qd_handler_table_destroy(g_server.data_handlers);
    
    qd_log_info("Stats: ctrl_rx=%lu ctrl_tx=%lu data_rx=%lu data_tx=%lu",
                (unsigned long)g_server.stats.ctrl_rx,
                (unsigned long)g_server.stats.ctrl_tx,
                (unsigned long)g_server.stats.data_rx,
                (unsigned long)g_server.stats.data_tx);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -f, --foreground   Run in foreground\n");
    printf("  -h, --help         Show this help\n");
    printf("\nRequires the qd_af_meta.ko kernel module to be loaded.\n");
    printf("Uses AF_QDMETA sockets for both control (port %d) and data (port %d).\n",
           META_SERVER_CTRL_PORT, META_SERVER_DATA_PORT);
}

int main(int argc, char **argv)
{
    memset(&g_server, 0, sizeof(g_server));
    g_server.ctrl_fd = -1;
    g_server.data_fd = -1;
    
    /* Parse arguments */
    qd_daemon_config_t config = QD_DAEMON_CONFIG_DEFAULT;
    config.name = "qd_meta_server";
    config.version = "1.0.0";
    config.description = "QDaemon Meta Socket Server";
    config.pid_file = "/tmp/qd_meta_server.pid";
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
