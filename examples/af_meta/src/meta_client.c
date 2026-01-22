/*
 * QDaemon Meta Client - Example client for AF_QDMETA
 * Uses custom socket family for control and data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "qd_meta_sock.h"

/* Server ports (must match server) */
#define META_SERVER_CTRL_PORT   5000
#define META_SERVER_DATA_PORT   5001

/* Control message commands */
enum {
    META_CMD_ECHO = 1,
    META_CMD_STATS,
    META_CMD_PING,
};

/* Data packet types */
enum {
    META_PKT_DATA = 100,
    META_PKT_KEEP_ALIVE,
    META_PKT_ACK,
};

/* Message header */
typedef struct {
    uint32_t type;
    uint32_t seq;
    uint32_t len;
    uint32_t flags;
    uint8_t  data[];
} __attribute__((packed)) meta_msg_t;

/* Statistics structure (from server) */
typedef struct {
    uint64_t ctrl_rx;
    uint64_t ctrl_tx;
    uint64_t data_rx;
    uint64_t data_tx;
    uint64_t bytes_rx;
    uint64_t bytes_tx;
    uint64_t errors;
} meta_stats_t;

static uint32_t g_seq = 1;

static void print_meta(const struct qdmeta_meta *meta)
{
    if (meta->have_ts) {
        printf("  Timestamp: %lu.%09lu (clock %u)\n",
               (unsigned long)meta->ts.sec, (unsigned long)meta->ts.nsec,
               meta->ts.clock_id);
    }
    if (meta->have_cpu) {
        printf("  CPU: %u (NUMA node %u)\n", meta->cpu.cpu_id, meta->cpu.numa_node);
    }
    if (meta->have_seq) {
        printf("  Sequence: %lu\n", (unsigned long)meta->seq.seq);
    }
}

static int do_echo(int fd, const char *message)
{
    size_t msg_len = strlen(message);
    size_t total_len = sizeof(meta_msg_t) + msg_len;
    
    meta_msg_t *msg = malloc(total_len);
    if (!msg) return -1;
    
    msg->type = META_CMD_ECHO;
    msg->seq = g_seq++;
    msg->len = msg_len;
    msg->flags = 0;
    memcpy(msg->data, message, msg_len);
    
    printf("Sending ECHO to control port %d: \"%s\"\n", META_SERVER_CTRL_PORT, message);
    
    if (qdmeta_sendto(fd, msg, total_len, META_SERVER_CTRL_PORT, 0) < 0) {
        perror("sendto");
        free(msg);
        return -1;
    }
    
    /* Receive response */
    uint8_t buf[4096];
    struct qdmeta_meta meta;
    
    ssize_t n = qdmeta_recvmeta(fd, buf, sizeof(buf), &meta, NULL);
    if (n < 0) {
        perror("recv");
        free(msg);
        return -1;
    }
    
    meta_msg_t *resp = (meta_msg_t *)buf;
    printf("Received ECHO response:\n");
    printf("  Message: \"%.*s\"\n", (int)resp->len, resp->data);
    print_meta(&meta);
    
    free(msg);
    return 0;
}

static int do_ping(int fd, int count)
{
    printf("Sending %d PINGs to control port %d...\n", count, META_SERVER_CTRL_PORT);
    
    for (int i = 0; i < count; i++) {
        meta_msg_t msg = {
            .type = META_CMD_PING,
            .seq = g_seq++,
            .len = 0,
            .flags = 0,
        };
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        if (qdmeta_sendto(fd, &msg, sizeof(msg), META_SERVER_CTRL_PORT, 0) < 0) {
            perror("sendto");
            return -1;
        }
        
        uint8_t buf[256];
        struct qdmeta_meta meta;
        
        ssize_t n = qdmeta_recvmeta(fd, buf, sizeof(buf), &meta, NULL);
        if (n < 0) {
            perror("recv");
            return -1;
        }
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        
        double rtt = (end.tv_sec - start.tv_sec) * 1000000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000.0;
        
        printf("PONG: seq=%u cpu=%u rtt=%.1f us\n", 
               msg.seq, meta.cpu.cpu_id, rtt);
        
        if (i < count - 1)
            usleep(100000);  /* 100ms between pings */
    }
    
    return 0;
}

static int do_stats(int fd)
{
    meta_msg_t msg = {
        .type = META_CMD_STATS,
        .seq = g_seq++,
        .len = 0,
        .flags = 0,
    };
    
    printf("Requesting STATS from control port %d...\n", META_SERVER_CTRL_PORT);
    
    if (qdmeta_sendto(fd, &msg, sizeof(msg), META_SERVER_CTRL_PORT, 0) < 0) {
        perror("sendto");
        return -1;
    }
    
    uint8_t buf[4096];
    struct qdmeta_meta meta;
    
    ssize_t n = qdmeta_recvmeta(fd, buf, sizeof(buf), &meta, NULL);
    if (n < 0) {
        perror("recv");
        return -1;
    }
    
    meta_msg_t *resp = (meta_msg_t *)buf;
    if (resp->len >= sizeof(meta_stats_t)) {
        meta_stats_t *stats = (meta_stats_t *)resp->data;
        printf("Server Statistics:\n");
        printf("  ctrl_rx:  %lu\n", (unsigned long)stats->ctrl_rx);
        printf("  ctrl_tx:  %lu\n", (unsigned long)stats->ctrl_tx);
        printf("  data_rx:  %lu\n", (unsigned long)stats->data_rx);
        printf("  data_tx:  %lu\n", (unsigned long)stats->data_tx);
        printf("  bytes_rx: %lu\n", (unsigned long)stats->bytes_rx);
        printf("  bytes_tx: %lu\n", (unsigned long)stats->bytes_tx);
        printf("  errors:   %lu\n", (unsigned long)stats->errors);
    }
    
    print_meta(&meta);
    return 0;
}

static int do_data(int fd, size_t size)
{
    size_t total_len = sizeof(meta_msg_t) + size;
    meta_msg_t *msg = malloc(total_len);
    if (!msg) return -1;
    
    msg->type = META_PKT_DATA;
    msg->seq = g_seq++;
    msg->len = size;
    msg->flags = 0;
    
    /* Fill with test pattern */
    for (size_t i = 0; i < size; i++)
        msg->data[i] = (uint8_t)(i & 0xFF);
    
    printf("Sending DATA (%zu bytes) to data port %d...\n", size, META_SERVER_DATA_PORT);
    
    if (qdmeta_sendto(fd, msg, total_len, META_SERVER_DATA_PORT, 0) < 0) {
        perror("sendto");
        free(msg);
        return -1;
    }
    
    /* Receive ACK */
    uint8_t buf[4096];
    struct qdmeta_meta meta;
    
    ssize_t n = qdmeta_recvmeta(fd, buf, sizeof(buf), &meta, NULL);
    if (n < 0) {
        perror("recv");
        free(msg);
        return -1;
    }
    
    meta_msg_t *resp = (meta_msg_t *)buf;
    printf("Received ACK:\n");
    printf("  Type: %u, Seq: %u, Len: %u\n", resp->type, resp->seq, resp->len);
    print_meta(&meta);
    
    /* Verify data */
    if (resp->len == size) {
        int match = 1;
        for (size_t i = 0; i < size && match; i++) {
            if (resp->data[i] != (uint8_t)(i & 0xFF))
                match = 0;
        }
        printf("  Data integrity: %s\n", match ? "OK" : "MISMATCH");
    }
    
    free(msg);
    return 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [args]\n", prog);
    printf("Commands:\n");
    printf("  echo <message>     Send echo request via control channel\n");
    printf("  ping [count]       Send ping requests via control channel\n");
    printf("  stats              Request server statistics\n");
    printf("  data [size]        Send test data packet via data channel\n");
    printf("\nRequires the qd_af_meta.ko kernel module and meta_server running.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *cmd = argv[1];
    
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    /* Create socket */
    int fd = qdmeta_socket(SOCK_DGRAM, QDMETA_PROTO_CTRL);
    if (fd < 0) {
        perror("socket");
        fprintf(stderr, "Is the qd_af_meta.ko kernel module loaded?\n");
        return 1;
    }
    
    /* Bind to ephemeral port */
    if (qdmeta_bind(fd, 0, 0, QDMETA_PROTO_CTRL) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }
    
    printf("Connected via AF_QDMETA socket\n\n");
    
    int ret = 0;
    if (strcmp(cmd, "echo") == 0) {
        const char *message = (argc > 2) ? argv[2] : "Hello, Meta!";
        ret = do_echo(fd, message);
    } else if (strcmp(cmd, "ping") == 0) {
        int count = (argc > 2) ? atoi(argv[2]) : 3;
        ret = do_ping(fd, count);
    } else if (strcmp(cmd, "stats") == 0) {
        ret = do_stats(fd);
    } else if (strcmp(cmd, "data") == 0) {
        size_t size = (argc > 2) ? (size_t)atoi(argv[2]) : 64;
        ret = do_data(fd, size);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        ret = 1;
    }
    
    close(fd);
    return ret;
}
