/*
 * QDaemon Meta Socket - User-space header
 * Custom socket address family for control + data with meta-data
 * NO netdev - all communication via socket family
 */

#ifndef QD_META_SOCK_H
#define QD_META_SOCK_H

#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/types.h>

/* Address family - must match kernel module */
#define AF_QDMETA           45
#define PF_QDMETA           AF_QDMETA

/* Protocol types */
#define QDMETA_PROTO_CTRL   1       /* Control protocol */
#define QDMETA_PROTO_DATA   2       /* Data protocol */
#define QDMETA_PROTO_RAW    3       /* Raw protocol */

/* SCM types for ancillary data */
#define SCM_QDMETA_BASE         0x1000
#define SCM_QDMETA_TIMESTAMP    (SCM_QDMETA_BASE + 1)
#define SCM_QDMETA_CPU          (SCM_QDMETA_BASE + 2)
#define SCM_QDMETA_CREDS        (SCM_QDMETA_BASE + 3)
#define SCM_QDMETA_SEQNUM       (SCM_QDMETA_BASE + 4)
#define SCM_QDMETA_PKTTYPE      (SCM_QDMETA_BASE + 5)
#define SCM_QDMETA_CUSTOM       (SCM_QDMETA_BASE + 6)

/* Socket address structure */
struct sockaddr_qdmeta {
    sa_family_t             sqm_family;     /* AF_QDMETA */
    uint16_t                sqm_port;       /* Port number */
    uint32_t                sqm_addr;       /* Logical address */
    uint16_t                sqm_protocol;   /* Protocol type */
    uint8_t                 sqm_reserved[6];
};

/* Meta-data structures */
struct qdmeta_timestamp {
    uint64_t    sec;
    uint64_t    nsec;
    uint32_t    clock_id;
    uint32_t    flags;
};

struct qdmeta_cpu {
    uint32_t    cpu_id;
    uint32_t    numa_node;
};

struct qdmeta_creds {
    uint32_t    pid;
    uint32_t    uid;
    uint32_t    gid;
    uint32_t    flags;
};

struct qdmeta_seqnum {
    uint64_t    seq;
    uint32_t    flags;
    uint32_t    reserved;
};

struct qdmeta_pkttype {
    uint16_t    type;
    uint16_t    subtype;
    uint32_t    flags;
};

/* Received meta-data container */
struct qdmeta_meta {
    struct qdmeta_timestamp ts;
    struct qdmeta_cpu cpu;
    struct qdmeta_seqnum seq;
    uint32_t    have_ts : 1;
    uint32_t    have_cpu : 1;
    uint32_t    have_seq : 1;
};

/*
 * Helper functions
 */

/**
 * Create a QDMETA socket
 * @param type: SOCK_DGRAM or SOCK_SEQPACKET
 * @param protocol: QDMETA_PROTO_CTRL, QDMETA_PROTO_DATA, or QDMETA_PROTO_RAW
 * @return: socket fd or -1 on error
 */
static inline int qdmeta_socket(int type, int protocol)
{
    return socket(AF_QDMETA, type, protocol);
}

/**
 * Bind socket to address
 * @param fd: socket fd
 * @param port: port number (0 for auto-assign)
 * @param addr: logical address (0 for any)
 * @param protocol: protocol type
 * @return: 0 on success, -1 on error
 */
static inline int qdmeta_bind(int fd, uint16_t port, uint32_t addr, uint16_t protocol)
{
    struct sockaddr_qdmeta sqm = {
        .sqm_family = AF_QDMETA,
        .sqm_port = port,
        .sqm_addr = addr,
        .sqm_protocol = protocol,
    };
    return bind(fd, (struct sockaddr *)&sqm, sizeof(sqm));
}

/**
 * Connect socket to peer
 * @param fd: socket fd
 * @param port: peer port number
 * @param addr: peer logical address
 * @return: 0 on success, -1 on error
 */
static inline int qdmeta_connect(int fd, uint16_t port, uint32_t addr)
{
    struct sockaddr_qdmeta sqm = {
        .sqm_family = AF_QDMETA,
        .sqm_port = port,
        .sqm_addr = addr,
    };
    return connect(fd, (struct sockaddr *)&sqm, sizeof(sqm));
}

/**
 * Send data to connected peer
 * @param fd: socket fd
 * @param buf: data buffer
 * @param len: data length
 * @return: bytes sent or -1 on error
 */
static inline ssize_t qdmeta_send(int fd, const void *buf, size_t len)
{
    return send(fd, buf, len, 0);
}

/**
 * Send data to specific address
 * @param fd: socket fd
 * @param buf: data buffer
 * @param len: data length
 * @param port: destination port
 * @param addr: destination address
 * @return: bytes sent or -1 on error
 */
static inline ssize_t qdmeta_sendto(int fd, const void *buf, size_t len,
                                     uint16_t port, uint32_t addr)
{
    struct sockaddr_qdmeta dest = {
        .sqm_family = AF_QDMETA,
        .sqm_port = port,
        .sqm_addr = addr,
    };
    return sendto(fd, buf, len, 0, (struct sockaddr *)&dest, sizeof(dest));
}

/**
 * Parse control messages to extract meta-data
 */
static inline void qdmeta_parse_cmsg(struct msghdr *msg, struct qdmeta_meta *meta)
{
    struct cmsghdr *cmsg;
    
    memset(meta, 0, sizeof(*meta));
    
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET)
            continue;

        switch (cmsg->cmsg_type) {
        case SCM_QDMETA_TIMESTAMP:
            memcpy(&meta->ts, CMSG_DATA(cmsg), sizeof(meta->ts));
            meta->have_ts = 1;
            break;
        case SCM_QDMETA_CPU:
            memcpy(&meta->cpu, CMSG_DATA(cmsg), sizeof(meta->cpu));
            meta->have_cpu = 1;
            break;
        case SCM_QDMETA_SEQNUM:
            memcpy(&meta->seq, CMSG_DATA(cmsg), sizeof(meta->seq));
            meta->have_seq = 1;
            break;
        }
    }
}

/**
 * Receive data with meta-data
 * @param fd: socket fd
 * @param buf: buffer for data
 * @param len: buffer length
 * @param meta: output meta-data (can be NULL)
 * @param from: output source address (can be NULL)
 * @return: bytes received or -1 on error
 */
static inline ssize_t qdmeta_recvmeta(int fd, void *buf, size_t len,
                                       struct qdmeta_meta *meta,
                                       struct sockaddr_qdmeta *from)
{
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    char cmsg_buf[256];
    struct sockaddr_qdmeta addr;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
        .msg_name = &addr,
        .msg_namelen = sizeof(addr),
    };

    ssize_t ret = recvmsg(fd, &msg, 0);
    if (ret < 0)
        return ret;

    /* Parse meta-data */
    if (meta)
        qdmeta_parse_cmsg(&msg, meta);

    /* Copy source address */
    if (from)
        memcpy(from, &addr, sizeof(*from));

    return ret;
}

/**
 * Simple receive (no meta-data)
 */
static inline ssize_t qdmeta_recv(int fd, void *buf, size_t len)
{
    return recv(fd, buf, len, 0);
}

#endif /* QD_META_SOCK_H */
