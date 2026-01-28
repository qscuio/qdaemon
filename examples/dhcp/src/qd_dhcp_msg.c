/*
 * QDaemon DHCP Messaging - Implementation
 *
 * Transport-agnostic messaging layer with reliable delivery and batch support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>

#include "qd_dhcp_msg_internal.h"

#ifdef STANDALONE
#include "qdaemon_stub.h"
#else
#include <qdaemon/qdaemon.h>
#endif

/*
 * ==========================================================================
 * Error and command strings
 * ==========================================================================
 */

static const char *error_strings[] = {
    [0]  = "Success",
    [1]  = "Generic error",
    [2]  = "Out of memory",
    [3]  = "Invalid argument",
    [4]  = "Timeout",
    [5]  = "Not found",
    [6]  = "Full",
    [7]  = "I/O error",
    [8]  = "Protocol error",
    [9]  = "Resource busy",
    [10] = "Not connected",
    [11] = "Retry limit exceeded",
    [12] = "Operation aborted",
};

static const char *cmd_strings[] = {
    [QD_DHCP_CMD_NONE]           = "NONE",
    [QD_DHCP_CMD_REGISTER]       = "REGISTER",
    [QD_DHCP_CMD_UNREGISTER]     = "UNREGISTER",
    [QD_DHCP_CMD_ADD_INTERFACE]  = "ADD_INTERFACE",
    [QD_DHCP_CMD_DEL_INTERFACE]  = "DEL_INTERFACE",
    [QD_DHCP_CMD_SET_TRUSTED]    = "SET_TRUSTED",
    [QD_DHCP_CMD_ADD_BINDING]    = "ADD_BINDING",
    [QD_DHCP_CMD_DEL_BINDING]    = "DEL_BINDING",
    [QD_DHCP_CMD_CLEAR_BINDINGS] = "CLEAR_BINDINGS",
    [QD_DHCP_CMD_SET_METHOD]     = "SET_METHOD",
    [QD_DHCP_CMD_GET_STATS]      = "GET_STATS",
    [QD_DHCP_CMD_NOTIFY_BINDING] = "NOTIFY_BINDING",
    [QD_DHCP_CMD_NOTIFY_IFACE]   = "NOTIFY_IFACE",
};

static const char *event_strings[] = {
    [QD_DHCP_EVENT_NONE]         = "NONE",
    [QD_DHCP_EVENT_BINDING_ADD]  = "BINDING_ADD",
    [QD_DHCP_EVENT_BINDING_DEL]  = "BINDING_DEL",
    [QD_DHCP_EVENT_IFACE_ADD]    = "IFACE_ADD",
    [QD_DHCP_EVENT_IFACE_DEL]    = "IFACE_DEL",
    [QD_DHCP_EVENT_CONNECTED]    = "CONNECTED",
    [QD_DHCP_EVENT_DISCONNECTED] = "DISCONNECTED",
};

/*
 * ==========================================================================
 * Utility Functions
 * ==========================================================================
 */

const char *qd_dhcp_strerror(int error)
{
    int idx = -error;
    if (idx >= 0 && idx < (int)(sizeof(error_strings) / sizeof(error_strings[0])))
        return error_strings[idx];
    return "Unknown error";
}

const char *qd_dhcp_cmd_str(qd_dhcp_cmd_t cmd)
{
    if (cmd < __QD_DHCP_CMD_MAX)
        return cmd_strings[cmd];
    return "UNKNOWN";
}

const char *qd_dhcp_event_str(qd_dhcp_event_t event)
{
    if (event <= QD_DHCP_EVENT_DISCONNECTED)
        return event_strings[event];
    return "UNKNOWN";
}

char *qd_dhcp_mac_str(const uint8_t mac[ETH_ALEN], char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

char *qd_dhcp_ip_str(uint32_t ip, char *buf, size_t len)
{
    struct in_addr addr = { .s_addr = ip };
    inet_ntop(AF_INET, &addr, buf, len);
    return buf;
}

int qd_dhcp_parse_mac(const char *str, uint8_t mac[ETH_ALEN])
{
    unsigned int tmp[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5]) != 6)
        return QD_DHCP_ERR_INVALID;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)tmp[i];
    return QD_DHCP_OK;
}

int qd_dhcp_parse_ip(const char *str, uint32_t *ip)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, str, &addr) != 1)
        return QD_DHCP_ERR_INVALID;
    *ip = addr.s_addr;
    return QD_DHCP_OK;
}

uint64_t qd_dhcp_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * ==========================================================================
 * Message Builder Functions
 * ==========================================================================
 */

void qd_dhcp_msg_builder_init(qd_dhcp_msg_builder_t *builder,
                               uint8_t *buf, size_t buf_size)
{
    builder->buf = buf;
    builder->buf_size = buf_size;
    builder->len = 0;
    builder->hdr = NULL;
}

void qd_dhcp_msg_builder_reset(qd_dhcp_msg_builder_t *builder,
                                qd_dhcp_cmd_t cmd, uint32_t seq, uint8_t flags)
{
    if (builder->buf_size < QD_DHCP_MSG_HDR_SIZE)
        return;

    builder->hdr = (qd_dhcp_msg_hdr_t *)builder->buf;
    memset(builder->hdr, 0, QD_DHCP_MSG_HDR_SIZE);

    builder->hdr->magic = QD_DHCP_MSG_MAGIC;
    builder->hdr->version = QD_DHCP_MSG_VERSION;
    builder->hdr->cmd = (uint8_t)cmd;
    builder->hdr->flags = flags;
    builder->hdr->seq = seq;
    builder->hdr->len = 0;
    builder->hdr->status = 0;

    builder->len = QD_DHCP_MSG_HDR_SIZE;
}

int qd_dhcp_msg_builder_add_attr(qd_dhcp_msg_builder_t *builder,
                                  uint16_t type, const void *data, size_t len)
{
    size_t attr_len = QD_DHCP_ATTR_HDR_SIZE + len;
    size_t padded_len = QD_DHCP_ATTR_ALIGN(attr_len);

    if (builder->len + padded_len > builder->buf_size)
        return QD_DHCP_ERR_NOMEM;

    qd_dhcp_attr_hdr_t *attr = (qd_dhcp_attr_hdr_t *)(builder->buf + builder->len);
    attr->type = type;
    attr->len = attr_len;

    if (data && len > 0)
        memcpy((uint8_t *)attr + QD_DHCP_ATTR_HDR_SIZE, data, len);

    /* Zero padding */
    if (padded_len > attr_len)
        memset(builder->buf + builder->len + attr_len, 0, padded_len - attr_len);

    builder->len += padded_len;

    return QD_DHCP_OK;
}

int qd_dhcp_msg_builder_add_u8(qd_dhcp_msg_builder_t *builder,
                                uint16_t type, uint8_t val)
{
    return qd_dhcp_msg_builder_add_attr(builder, type, &val, sizeof(val));
}

int qd_dhcp_msg_builder_add_u32(qd_dhcp_msg_builder_t *builder,
                                 uint16_t type, uint32_t val)
{
    return qd_dhcp_msg_builder_add_attr(builder, type, &val, sizeof(val));
}

int qd_dhcp_msg_builder_add_s32(qd_dhcp_msg_builder_t *builder,
                                 uint16_t type, int32_t val)
{
    return qd_dhcp_msg_builder_add_attr(builder, type, &val, sizeof(val));
}

int qd_dhcp_msg_builder_add_string(qd_dhcp_msg_builder_t *builder,
                                    uint16_t type, const char *str)
{
    return qd_dhcp_msg_builder_add_attr(builder, type, str, strlen(str) + 1);
}

size_t qd_dhcp_msg_builder_finish(qd_dhcp_msg_builder_t *builder)
{
    if (builder->hdr)
        builder->hdr->len = builder->len - QD_DHCP_MSG_HDR_SIZE;
    return builder->len;
}

/*
 * ==========================================================================
 * Message Parsing Functions
 * ==========================================================================
 */

int qd_dhcp_msg_validate(const void *data, size_t len)
{
    if (len < QD_DHCP_MSG_HDR_SIZE)
        return QD_DHCP_ERR_PROTO;

    const qd_dhcp_msg_hdr_t *hdr = data;

    if (hdr->magic != QD_DHCP_MSG_MAGIC)
        return QD_DHCP_ERR_PROTO;

    if (hdr->version != QD_DHCP_MSG_VERSION)
        return QD_DHCP_ERR_PROTO;

    if (QD_DHCP_MSG_HDR_SIZE + hdr->len > len)
        return QD_DHCP_ERR_PROTO;

    return QD_DHCP_OK;
}

int qd_dhcp_msg_parse_attrs(const void *data, size_t len,
                             qd_dhcp_attr_t *attrs, int max_attrs)
{
    const uint8_t *ptr = data;
    size_t remaining = len;
    int count = 0;

    while (remaining >= QD_DHCP_ATTR_HDR_SIZE && count < max_attrs) {
        const qd_dhcp_attr_hdr_t *attr = (const qd_dhcp_attr_hdr_t *)ptr;
        size_t padded_len = QD_DHCP_ATTR_ALIGN(attr->len);

        if (attr->len < QD_DHCP_ATTR_HDR_SIZE || padded_len > remaining)
            break;

        attrs[count].type = attr->type;
        attrs[count].len = attr->len - QD_DHCP_ATTR_HDR_SIZE;
        attrs[count].data = (void *)(ptr + QD_DHCP_ATTR_HDR_SIZE);
        count++;

        ptr += padded_len;
        remaining -= padded_len;
    }

    return count;
}

void *qd_dhcp_msg_find_attr(const qd_dhcp_attr_t *attrs, int num_attrs,
                             uint16_t type, size_t *len)
{
    for (int i = 0; i < num_attrs; i++) {
        if (attrs[i].type == type) {
            if (len)
                *len = attrs[i].len;
            return attrs[i].data;
        }
    }
    return NULL;
}

/*
 * ==========================================================================
 * Netlink Transport Implementation
 * ==========================================================================
 */

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include "dhcp_kmod.h"

typedef struct {
    int         sock_fd;
    int         family_id;
    int         mcgrp_id;
    uint32_t    pid;
    uint32_t    seq;
} netlink_priv_t;

static int netlink_open(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;

    priv->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (priv->sock_fd < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();

    if (bind(priv->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        transport->last_error = errno;
        close(priv->sock_fd);
        priv->sock_fd = -1;
        return QD_DHCP_ERR_IO;
    }

    priv->pid = addr.nl_pid;

    /* Resolve family ID */
    struct {
        struct nlmsghdr nlh;
        struct genlmsghdr genl;
        struct nlattr nla;
        char name[GENL_NAMSIZ];
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = GENL_ID_CTRL;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_seq = ++priv->seq;
    req.nlh.nlmsg_pid = priv->pid;

    req.genl.cmd = CTRL_CMD_GETFAMILY;
    req.genl.version = 1;

    req.nla.nla_len = NLA_HDRLEN + strlen(QDHCP_GENL_NAME) + 1;
    req.nla.nla_type = CTRL_ATTR_FAMILY_NAME;
    strncpy(req.name, QDHCP_GENL_NAME, GENL_NAMSIZ - 1);

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    if (sendto(priv->sock_fd, &req, req.nlh.nlmsg_len, 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        transport->last_error = errno;
        close(priv->sock_fd);
        priv->sock_fd = -1;
        return QD_DHCP_ERR_IO;
    }

    uint8_t rx_buf[4096];
    socklen_t addr_len = sizeof(addr);
    ssize_t recv_len = recvfrom(priv->sock_fd, rx_buf, sizeof(rx_buf), 0,
                                 (struct sockaddr *)&addr, &addr_len);

    if (recv_len < 0) {
        transport->last_error = errno;
        close(priv->sock_fd);
        priv->sock_fd = -1;
        return QD_DHCP_ERR_IO;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)rx_buf;
    if (!NLMSG_OK(nlh, (size_t)recv_len) || nlh->nlmsg_type != GENL_ID_CTRL) {
        close(priv->sock_fd);
        priv->sock_fd = -1;
        return QD_DHCP_ERR_PROTO;
    }

    /* Parse response to get family ID */
    struct genlmsghdr *genl = NLMSG_DATA(nlh);
    void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
    size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

    const uint8_t *ptr = attr_data;
    size_t remaining = attr_len;

    while (remaining >= NLA_HDRLEN) {
        struct nlattr *nla = (struct nlattr *)ptr;
        size_t nla_padded = NLA_ALIGN(nla->nla_len);

        if (nla->nla_len < NLA_HDRLEN || nla_padded > remaining)
            break;

        if (nla->nla_type == CTRL_ATTR_FAMILY_ID) {
            priv->family_id = *(uint16_t *)((uint8_t *)nla + NLA_HDRLEN);
        } else if (nla->nla_type == CTRL_ATTR_MCAST_GROUPS) {
            /* Parse nested multicast group attributes */
            const uint8_t *grp_ptr = (uint8_t *)nla + NLA_HDRLEN;
            size_t grp_remaining = nla->nla_len - NLA_HDRLEN;

            while (grp_remaining >= NLA_HDRLEN) {
                struct nlattr *grp_nla = (struct nlattr *)grp_ptr;
                size_t grp_padded = NLA_ALIGN(grp_nla->nla_len);

                if (grp_nla->nla_len < NLA_HDRLEN || grp_padded > grp_remaining)
                    break;

                /* Parse nested group entry */
                const uint8_t *entry_ptr = (uint8_t *)grp_nla + NLA_HDRLEN;
                size_t entry_remaining = grp_nla->nla_len - NLA_HDRLEN;
                char *grp_name = NULL;
                uint32_t grp_id = 0;

                while (entry_remaining >= NLA_HDRLEN) {
                    struct nlattr *entry_nla = (struct nlattr *)entry_ptr;
                    size_t entry_padded = NLA_ALIGN(entry_nla->nla_len);

                    if (entry_nla->nla_len < NLA_HDRLEN || entry_padded > entry_remaining)
                        break;

                    if (entry_nla->nla_type == CTRL_ATTR_MCAST_GRP_NAME) {
                        grp_name = (char *)entry_nla + NLA_HDRLEN;
                    } else if (entry_nla->nla_type == CTRL_ATTR_MCAST_GRP_ID) {
                        grp_id = *(uint32_t *)((uint8_t *)entry_nla + NLA_HDRLEN);
                    }

                    entry_ptr += entry_padded;
                    entry_remaining -= entry_padded;
                }

                if (grp_name && strcmp(grp_name, QDHCP_MCGRP_NAME) == 0) {
                    priv->mcgrp_id = grp_id;
                }

                grp_ptr += grp_padded;
                grp_remaining -= grp_padded;
            }
        }

        ptr += nla_padded;
        remaining -= nla_padded;
    }

    if (priv->family_id <= 0) {
        close(priv->sock_fd);
        priv->sock_fd = -1;
        return QD_DHCP_ERR_NOT_FOUND;
    }

    return QD_DHCP_OK;
}

static void netlink_close(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;
    if (priv->sock_fd >= 0) {
        close(priv->sock_fd);
        priv->sock_fd = -1;
    }
}

static int netlink_get_fd(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;
    return priv->sock_fd;
}

static int netlink_send(qd_dhcp_transport_t *transport,
                         const void *data, size_t len)
{
    netlink_priv_t *priv = transport->priv;

    if (priv->sock_fd < 0)
        return QD_DHCP_ERR_NOCONN;

    /* Validate and extract our message */
    const qd_dhcp_msg_hdr_t *msg_hdr = data;
    if (len < QD_DHCP_MSG_HDR_SIZE || msg_hdr->magic != QD_DHCP_MSG_MAGIC)
        return QD_DHCP_ERR_INVALID;

    /* Build netlink message */
    uint8_t nl_buf[4096];
    struct nlmsghdr *nlh = (struct nlmsghdr *)nl_buf;
    struct genlmsghdr *genl = NLMSG_DATA(nlh);

    size_t payload_len = len - QD_DHCP_MSG_HDR_SIZE;
    size_t nl_payload_len = GENL_HDRLEN + payload_len;

    nlh->nlmsg_len = NLMSG_LENGTH(nl_payload_len);
    nlh->nlmsg_type = priv->family_id;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    if (msg_hdr->flags & QD_DHCP_MSG_FLAG_ACK)
        nlh->nlmsg_flags |= NLM_F_ACK;
    nlh->nlmsg_seq = msg_hdr->seq;
    nlh->nlmsg_pid = priv->pid;

    genl->cmd = msg_hdr->cmd;
    genl->version = QDHCP_GENL_VERSION;

    /* Copy attributes */
    if (payload_len > 0) {
        memcpy((uint8_t *)genl + GENL_HDRLEN,
               (const uint8_t *)data + QD_DHCP_MSG_HDR_SIZE,
               payload_len);
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    ssize_t sent = sendto(priv->sock_fd, nl_buf, nlh->nlmsg_len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));

    if (sent < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    return QD_DHCP_OK;
}

static int netlink_recv(qd_dhcp_transport_t *transport,
                         void *buf, size_t buf_size, int flags)
{
    netlink_priv_t *priv = transport->priv;

    if (priv->sock_fd < 0)
        return QD_DHCP_ERR_NOCONN;

    int recv_flags = 0;
    if (flags & QD_DHCP_RECV_NONBLOCK)
        recv_flags |= MSG_DONTWAIT;
    if (flags & QD_DHCP_RECV_PEEK)
        recv_flags |= MSG_PEEK;

    uint8_t nl_buf[4096];
    struct sockaddr_nl addr;
    socklen_t addr_len = sizeof(addr);

    ssize_t recv_len = recvfrom(priv->sock_fd, nl_buf, sizeof(nl_buf),
                                 recv_flags, (struct sockaddr *)&addr, &addr_len);

    if (recv_len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)nl_buf;
    if (!NLMSG_OK(nlh, (size_t)recv_len))
        return QD_DHCP_ERR_PROTO;

    /* Handle error responses */
    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = NLMSG_DATA(nlh);

        /* Build our response message */
        qd_dhcp_msg_hdr_t *msg_hdr = buf;
        if (buf_size < QD_DHCP_MSG_HDR_SIZE)
            return QD_DHCP_ERR_NOMEM;

        msg_hdr->magic = QD_DHCP_MSG_MAGIC;
        msg_hdr->version = QD_DHCP_MSG_VERSION;
        msg_hdr->cmd = QD_DHCP_CMD_NONE;
        msg_hdr->flags = QD_DHCP_MSG_FLAG_RESPONSE;
        if (err->error != 0)
            msg_hdr->flags |= QD_DHCP_MSG_FLAG_ERROR;
        msg_hdr->seq = nlh->nlmsg_seq;
        msg_hdr->len = 0;
        msg_hdr->status = err->error;

        return QD_DHCP_MSG_HDR_SIZE;
    }

    /* Handle regular message */
    if (nlh->nlmsg_type == (uint16_t)priv->family_id) {
        struct genlmsghdr *genl = NLMSG_DATA(nlh);
        void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
        size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

        size_t msg_len = QD_DHCP_MSG_HDR_SIZE + attr_len;
        if (msg_len > buf_size)
            return QD_DHCP_ERR_NOMEM;

        /* Build our message from netlink message */
        qd_dhcp_msg_hdr_t *msg_hdr = buf;
        msg_hdr->magic = QD_DHCP_MSG_MAGIC;
        msg_hdr->version = QD_DHCP_MSG_VERSION;
        msg_hdr->cmd = genl->cmd;
        msg_hdr->flags = QD_DHCP_MSG_FLAG_RESPONSE;
        msg_hdr->seq = nlh->nlmsg_seq;
        msg_hdr->len = attr_len;
        msg_hdr->status = 0;

        /* Copy attributes */
        if (attr_len > 0)
            memcpy((uint8_t *)buf + QD_DHCP_MSG_HDR_SIZE, attr_data, attr_len);

        return msg_len;
    }

    return 0;
}

static int netlink_subscribe(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;

    if (priv->sock_fd < 0)
        return QD_DHCP_ERR_NOCONN;

    if (priv->mcgrp_id <= 0)
        return QD_DHCP_ERR_NOT_FOUND;

    if (setsockopt(priv->sock_fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                   &priv->mcgrp_id, sizeof(priv->mcgrp_id)) < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    return QD_DHCP_OK;
}

static int netlink_unsubscribe(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;

    if (priv->sock_fd < 0)
        return QD_DHCP_ERR_NOCONN;

    if (priv->mcgrp_id <= 0)
        return QD_DHCP_ERR_NOT_FOUND;

    if (setsockopt(priv->sock_fd, SOL_NETLINK, NETLINK_DROP_MEMBERSHIP,
                   &priv->mcgrp_id, sizeof(priv->mcgrp_id)) < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    return QD_DHCP_OK;
}

static bool netlink_is_connected(qd_dhcp_transport_t *transport)
{
    netlink_priv_t *priv = transport->priv;
    return priv->sock_fd >= 0 && priv->family_id > 0;
}

static const char *netlink_strerror(qd_dhcp_transport_t *transport, int error)
{
    (void)error;
    return strerror(transport->last_error);
}

static const qd_dhcp_transport_ops_t netlink_ops = {
    .name         = "netlink",
    .open         = netlink_open,
    .close        = netlink_close,
    .get_fd       = netlink_get_fd,
    .send         = netlink_send,
    .recv         = netlink_recv,
    .subscribe    = netlink_subscribe,
    .unsubscribe  = netlink_unsubscribe,
    .is_connected = netlink_is_connected,
    .strerror     = netlink_strerror,
};

qd_dhcp_transport_t *qd_dhcp_transport_netlink_create(void)
{
    qd_dhcp_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport)
        return NULL;

    netlink_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        free(transport);
        return NULL;
    }

    priv->sock_fd = -1;
    transport->ops = &netlink_ops;
    transport->priv = priv;

    return transport;
}

void qd_dhcp_transport_destroy(qd_dhcp_transport_t *transport)
{
    if (!transport)
        return;

    if (transport->ops->close)
        transport->ops->close(transport);

    free(transport->priv);
    free(transport);
}

/*
 * ==========================================================================
 * Character Device Transport Implementation
 * ==========================================================================
 */

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <poll.h>

typedef struct {
    int         fd;
    char        path[256];
    bool        subscribed;
} chardev_priv_t;

/* IOCTL commands for the char device */
#define QDHCP_IOC_MAGIC     'Q'
#define QDHCP_IOC_SUBSCRIBE   _IO(QDHCP_IOC_MAGIC, 1)
#define QDHCP_IOC_UNSUBSCRIBE _IO(QDHCP_IOC_MAGIC, 2)

static int chardev_open(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;

    priv->fd = open(priv->path, O_RDWR | O_CLOEXEC);
    if (priv->fd < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    /* Set non-blocking mode */
    int flags = fcntl(priv->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(priv->fd, F_SETFL, flags | O_NONBLOCK);
    }

    return QD_DHCP_OK;
}

static void chardev_close(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;
    if (priv->fd >= 0) {
        close(priv->fd);
        priv->fd = -1;
    }
}

static int chardev_get_fd(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;
    return priv->fd;
}

static int chardev_send(qd_dhcp_transport_t *transport,
                         const void *data, size_t len)
{
    chardev_priv_t *priv = transport->priv;

    if (priv->fd < 0)
        return QD_DHCP_ERR_NOCONN;

    /* Validate message */
    const qd_dhcp_msg_hdr_t *msg_hdr = data;
    if (len < QD_DHCP_MSG_HDR_SIZE || msg_hdr->magic != QD_DHCP_MSG_MAGIC)
        return QD_DHCP_ERR_INVALID;

    /* Write the message directly - char device uses same wire format */
    ssize_t written = write(priv->fd, data, len);
    if (written < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    if ((size_t)written != len) {
        transport->last_error = EIO;
        return QD_DHCP_ERR_IO;
    }

    return QD_DHCP_OK;
}

static int chardev_recv(qd_dhcp_transport_t *transport,
                         void *buf, size_t buf_size, int flags)
{
    chardev_priv_t *priv = transport->priv;

    if (priv->fd < 0)
        return QD_DHCP_ERR_NOCONN;

    /* Handle blocking vs non-blocking */
    if (!(flags & QD_DHCP_RECV_NONBLOCK)) {
        /* Wait for data with poll */
        struct pollfd pfd = {
            .fd = priv->fd,
            .events = POLLIN,
        };
        int ret = poll(&pfd, 1, 5000);  /* 5 second timeout */
        if (ret < 0) {
            transport->last_error = errno;
            return QD_DHCP_ERR_IO;
        }
        if (ret == 0) {
            return 0;  /* Timeout, no data */
        }
    }

    /* Read message */
    ssize_t recv_len = read(priv->fd, buf, buf_size);
    if (recv_len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    if (recv_len == 0)
        return 0;

    /* Validate message header */
    if ((size_t)recv_len < QD_DHCP_MSG_HDR_SIZE)
        return QD_DHCP_ERR_PROTO;

    qd_dhcp_msg_hdr_t *hdr = buf;
    if (hdr->magic != QD_DHCP_MSG_MAGIC)
        return QD_DHCP_ERR_PROTO;

    return recv_len;
}

static int chardev_subscribe(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;

    if (priv->fd < 0)
        return QD_DHCP_ERR_NOCONN;

    if (ioctl(priv->fd, QDHCP_IOC_SUBSCRIBE, NULL) < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    priv->subscribed = true;
    return QD_DHCP_OK;
}

static int chardev_unsubscribe(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;

    if (priv->fd < 0)
        return QD_DHCP_ERR_NOCONN;

    if (ioctl(priv->fd, QDHCP_IOC_UNSUBSCRIBE, NULL) < 0) {
        transport->last_error = errno;
        return QD_DHCP_ERR_IO;
    }

    priv->subscribed = false;
    return QD_DHCP_OK;
}

static bool chardev_is_connected(qd_dhcp_transport_t *transport)
{
    chardev_priv_t *priv = transport->priv;
    return priv->fd >= 0;
}

static const char *chardev_strerror(qd_dhcp_transport_t *transport, int error)
{
    (void)error;
    return strerror(transport->last_error);
}

static const qd_dhcp_transport_ops_t chardev_ops = {
    .name         = "chardev",
    .open         = chardev_open,
    .close        = chardev_close,
    .get_fd       = chardev_get_fd,
    .send         = chardev_send,
    .recv         = chardev_recv,
    .subscribe    = chardev_subscribe,
    .unsubscribe  = chardev_unsubscribe,
    .is_connected = chardev_is_connected,
    .strerror     = chardev_strerror,
};

qd_dhcp_transport_t *qd_dhcp_transport_chardev_create(const char *path)
{
    qd_dhcp_transport_t *transport = calloc(1, sizeof(*transport));
    if (!transport)
        return NULL;

    chardev_priv_t *priv = calloc(1, sizeof(*priv));
    if (!priv) {
        free(transport);
        return NULL;
    }

    priv->fd = -1;
    strncpy(priv->path, path ? path : QDHCP_CHARDEV_PATH, sizeof(priv->path) - 1);

    transport->ops = &chardev_ops;
    transport->priv = priv;

    return transport;
}

/*
 * ==========================================================================
 * Handler Management
 * ==========================================================================
 */

static qd_dhcp_handler_node_t *find_handler(qd_dhcp_conn_t *conn, qd_dhcp_cmd_t cmd)
{
    uint32_t hash = qd_dhcp_hash_cmd(cmd);
    qd_dhcp_handler_node_t *node = conn->handlers[hash];

    while (node) {
        if (node->cmd == cmd)
            return node;
        node = node->next;
    }
    return NULL;
}

int qd_dhcp_register_handler(qd_dhcp_conn_t *conn,
                              qd_dhcp_cmd_t cmd,
                              qd_dhcp_handler_fn_t handler,
                              void *arg,
                              uint32_t flags)
{
    if (!conn || !handler)
        return QD_DHCP_ERR_INVALID;

    pthread_mutex_lock(&conn->handler_lock);

    if (find_handler(conn, cmd)) {
        pthread_mutex_unlock(&conn->handler_lock);
        return QD_DHCP_ERR_INVALID;
    }

    qd_dhcp_handler_node_t *node = calloc(1, sizeof(*node));
    if (!node) {
        pthread_mutex_unlock(&conn->handler_lock);
        return QD_DHCP_ERR_NOMEM;
    }

    node->cmd = cmd;
    node->handler = handler;
    node->arg = arg;
    node->flags = flags;

    uint32_t hash = qd_dhcp_hash_cmd(cmd);
    node->next = conn->handlers[hash];
    conn->handlers[hash] = node;
    conn->num_handlers++;

    pthread_mutex_unlock(&conn->handler_lock);
    return QD_DHCP_OK;
}

int qd_dhcp_register_handlers(qd_dhcp_conn_t *conn,
                               const qd_dhcp_handler_entry_t *handlers)
{
    if (!conn || !handlers)
        return QD_DHCP_ERR_INVALID;

    for (const qd_dhcp_handler_entry_t *e = handlers; e->handler; e++) {
        int ret = qd_dhcp_register_handler(conn, e->cmd, e->handler,
                                            e->arg, e->flags);
        if (ret != QD_DHCP_OK)
            return ret;

        qd_dhcp_handler_node_t *node = find_handler(conn, e->cmd);
        if (node)
            node->name = e->name;
    }

    return QD_DHCP_OK;
}

int qd_dhcp_unregister_handler(qd_dhcp_conn_t *conn, qd_dhcp_cmd_t cmd)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    pthread_mutex_lock(&conn->handler_lock);

    uint32_t hash = qd_dhcp_hash_cmd(cmd);
    qd_dhcp_handler_node_t **pp = &conn->handlers[hash];

    while (*pp) {
        if ((*pp)->cmd == cmd) {
            qd_dhcp_handler_node_t *node = *pp;
            *pp = node->next;
            free(node);
            conn->num_handlers--;
            pthread_mutex_unlock(&conn->handler_lock);
            return QD_DHCP_OK;
        }
        pp = &(*pp)->next;
    }

    pthread_mutex_unlock(&conn->handler_lock);
    return QD_DHCP_ERR_NOT_FOUND;
}

/*
 * ==========================================================================
 * Handler Dispatch
 * ==========================================================================
 */

int qd_dhcp_dispatch(qd_dhcp_conn_t *conn, const qd_dhcp_msg_hdr_t *hdr,
                      const qd_dhcp_attr_t *attrs, int num_attrs)
{
    pthread_mutex_lock(&conn->handler_lock);
    qd_dhcp_handler_node_t *node = find_handler(conn, (qd_dhcp_cmd_t)hdr->cmd);
    pthread_mutex_unlock(&conn->handler_lock);

    if (!node)
        return QD_DHCP_ERR_NOT_FOUND;

    /* Build context */
    qd_dhcp_msg_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cmd = (qd_dhcp_cmd_t)hdr->cmd;
    ctx.user_data = conn->user_data;
    ctx.seq = hdr->seq;
    ctx.status = hdr->status;

    /* Parse event type if present */
    uint8_t *event_type = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                                 QD_DHCP_ATTR_EVENT_TYPE, NULL);
    if (event_type)
        ctx.event = (qd_dhcp_event_t)*event_type;

    /* Parse binding data if present */
    uint8_t *mac = qd_dhcp_msg_find_attr(attrs, num_attrs, QD_DHCP_ATTR_MAC, NULL);
    uint32_t *ip = qd_dhcp_msg_find_attr(attrs, num_attrs, QD_DHCP_ATTR_IP, NULL);

    if (mac) {
        memcpy(conn->tmp_binding.mac, mac, ETH_ALEN);
        if (ip)
            conn->tmp_binding.ip = htonl(*ip);

        int32_t *ifindex = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                                  QD_DHCP_ATTR_IFINDEX, NULL);
        if (ifindex)
            conn->tmp_binding.ifindex = *ifindex;

        uint32_t *lease = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                                 QD_DHCP_ATTR_LEASE_TIME, NULL);
        if (lease)
            conn->tmp_binding.lease_time = *lease;

        ctx.binding = &conn->tmp_binding;
    }

    /* Parse interface data if present */
    int32_t *ifindex = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                              QD_DHCP_ATTR_IFINDEX, NULL);
    char *ifname = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                          QD_DHCP_ATTR_IFNAME, NULL);

    if (ifindex || ifname) {
        if (ifindex)
            conn->tmp_iface.ifindex = *ifindex;
        if (ifname)
            strncpy(conn->tmp_iface.ifname, ifname, IFNAMSIZ - 1);

        uint8_t *trusted = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                                  QD_DHCP_ATTR_TRUSTED, NULL);
        if (trusted)
            conn->tmp_iface.trusted = *trusted ? true : false;

        ctx.iface = &conn->tmp_iface;
    }

    /* Parse stats if present */
    size_t stats_len;
    void *stats_data = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                              QD_DHCP_ATTR_STATS, &stats_len);
    if (stats_data && stats_len == sizeof(conn->tmp_stats)) {
        memcpy(&conn->tmp_stats, stats_data, sizeof(conn->tmp_stats));
        ctx.stats = &conn->tmp_stats;
    }

    /* Call handler */
    node->call_count++;
    qd_dhcp_handler_result_t result = node->handler(&ctx, node->arg);

    if (result == QD_DHCP_HANDLER_ERROR)
        node->error_count++;

    return (int)result;
}

/*
 * ==========================================================================
 * Reliable Messaging
 * ==========================================================================
 */

static qd_dhcp_pending_msg_t *find_pending_slot(qd_dhcp_conn_t *conn)
{
    for (int i = 0; i < QD_DHCP_PENDING_MAX; i++) {
        if (!conn->pending[i].in_use)
            return &conn->pending[i];
    }
    return NULL;
}

static qd_dhcp_pending_msg_t *find_pending_seq(qd_dhcp_conn_t *conn, uint32_t seq)
{
    for (int i = 0; i < QD_DHCP_PENDING_MAX; i++) {
        if (conn->pending[i].in_use && conn->pending[i].seq == seq)
            return &conn->pending[i];
    }
    return NULL;
}

int qd_dhcp_send_reliable(qd_dhcp_conn_t *conn,
                           const void *data, size_t len,
                           const qd_dhcp_request_opts_t *opts)
{
    if (!conn || !data || len < QD_DHCP_MSG_HDR_SIZE)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_hdr_t *hdr = (qd_dhcp_msg_hdr_t *)data;

    /* Determine timeout and retry settings */
    uint32_t timeout = conn->config.timeout_ms;
    uint32_t retry_count = conn->config.retry_count;
    uint32_t retry_interval = conn->config.retry_interval_ms;
    bool no_ack = false;

    if (opts) {
        if (opts->timeout_ms > 0)
            timeout = opts->timeout_ms;
        if (opts->retry_count > 0)
            retry_count = opts->retry_count;
        no_ack = opts->no_ack;
    }

    /* Set ACK flag if we want acknowledgment */
    if (!no_ack)
        hdr->flags |= QD_DHCP_MSG_FLAG_ACK;

    /* Send the message */
    int ret = conn->transport->ops->send(conn->transport, data, len);
    if (ret != QD_DHCP_OK)
        return ret;

    pthread_mutex_lock(&conn->stats_lock);
    conn->stats.msgs_sent++;
    conn->stats.bytes_sent += len;
    pthread_mutex_unlock(&conn->stats_lock);

    if (no_ack)
        return QD_DHCP_OK;

    /* Track pending message for reliable delivery */
    pthread_mutex_lock(&conn->pending_lock);

    qd_dhcp_pending_msg_t *pending = find_pending_slot(conn);
    if (!pending) {
        pthread_mutex_unlock(&conn->pending_lock);
        return QD_DHCP_ERR_FULL;
    }

    pending->seq = hdr->seq;
    pending->send_time_ms = qd_dhcp_time_ms();
    pending->timeout_ms = timeout;
    pending->retry_count = retry_count;
    pending->retry_interval = retry_interval;
    pending->cmd = (qd_dhcp_cmd_t)hdr->cmd;
    pending->in_use = true;

    /* Save message for retransmission */
    pending->msg_data = malloc(len);
    if (pending->msg_data) {
        memcpy(pending->msg_data, data, len);
        pending->msg_len = len;
    }

    conn->pending_count++;
    pthread_mutex_unlock(&conn->pending_lock);

    /* Wait for ACK */
    return qd_dhcp_wait_ack(conn, hdr->seq, timeout);
}

int qd_dhcp_wait_ack(qd_dhcp_conn_t *conn, uint32_t seq, uint32_t timeout_ms)
{
    uint64_t start = qd_dhcp_time_ms();
    uint64_t deadline = start + timeout_ms;

    while (qd_dhcp_time_ms() < deadline) {
        /* Process incoming messages */
        int ret = conn->transport->ops->recv(conn->transport,
                                              conn->rx_buf, conn->rx_buf_size,
                                              QD_DHCP_RECV_NONBLOCK);

        if (ret > 0) {
            pthread_mutex_lock(&conn->stats_lock);
            conn->stats.msgs_received++;
            conn->stats.bytes_received += ret;
            pthread_mutex_unlock(&conn->stats_lock);

            /* Validate and process */
            if (qd_dhcp_msg_validate(conn->rx_buf, ret) == QD_DHCP_OK) {
                qd_dhcp_msg_hdr_t *hdr = (qd_dhcp_msg_hdr_t *)conn->rx_buf;

                if (hdr->flags & QD_DHCP_MSG_FLAG_RESPONSE) {
                    qd_dhcp_process_ack(conn, hdr->seq, hdr->status);

                    if (hdr->seq == seq) {
                        /* This is our ACK */
                        if (hdr->flags & QD_DHCP_MSG_FLAG_ERROR)
                            return hdr->status;
                        return QD_DHCP_OK;
                    }
                }
            }
        } else if (ret < 0 && ret != QD_DHCP_ERR_IO) {
            return ret;
        }

        /* Check for retransmission */
        qd_dhcp_check_timeouts(conn);

        /* Small sleep to avoid busy-waiting */
        usleep(1000);
    }

    pthread_mutex_lock(&conn->stats_lock);
    conn->stats.timeouts++;
    pthread_mutex_unlock(&conn->stats_lock);

    return QD_DHCP_ERR_TIMEOUT;
}

void qd_dhcp_process_ack(qd_dhcp_conn_t *conn, uint32_t seq, int status)
{
    pthread_mutex_lock(&conn->pending_lock);

    qd_dhcp_pending_msg_t *pending = find_pending_seq(conn, seq);
    if (pending) {
        if (status == 0) {
            pthread_mutex_lock(&conn->stats_lock);
            conn->stats.acks_received++;
            pthread_mutex_unlock(&conn->stats_lock);
        } else {
            pthread_mutex_lock(&conn->stats_lock);
            conn->stats.naks_received++;
            pthread_mutex_unlock(&conn->stats_lock);
        }

        free(pending->msg_data);
        pending->msg_data = NULL;
        pending->in_use = false;
        conn->pending_count--;
    }

    pthread_mutex_unlock(&conn->pending_lock);
}

void qd_dhcp_check_timeouts(qd_dhcp_conn_t *conn)
{
    uint64_t now = qd_dhcp_time_ms();

    pthread_mutex_lock(&conn->pending_lock);

    for (int i = 0; i < QD_DHCP_PENDING_MAX; i++) {
        qd_dhcp_pending_msg_t *pending = &conn->pending[i];
        if (!pending->in_use)
            continue;

        uint64_t elapsed = now - pending->send_time_ms;

        if (elapsed >= pending->timeout_ms) {
            if (pending->retry_count > 0 && pending->msg_data) {
                /* Retransmit */
                pending->retry_count--;
                pending->send_time_ms = now;

                conn->transport->ops->send(conn->transport,
                                           pending->msg_data,
                                           pending->msg_len);

                pthread_mutex_lock(&conn->stats_lock);
                conn->stats.msgs_retried++;
                pthread_mutex_unlock(&conn->stats_lock);
            } else {
                /* Give up */
                pthread_mutex_lock(&conn->stats_lock);
                conn->stats.msgs_failed++;
                pthread_mutex_unlock(&conn->stats_lock);

                free(pending->msg_data);
                pending->msg_data = NULL;
                pending->in_use = false;
                conn->pending_count--;
            }
        }
    }

    pthread_mutex_unlock(&conn->pending_lock);
}

/*
 * ==========================================================================
 * Connection Management
 * ==========================================================================
 */

qd_dhcp_conn_t *qd_dhcp_connect(const qd_dhcp_conn_config_t *config)
{
    qd_dhcp_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;

    /* Apply configuration */
    if (config) {
        conn->config = *config;
    } else {
        qd_dhcp_conn_config_t default_config = QD_DHCP_CONN_CONFIG_DEFAULT;
        conn->config = default_config;
    }

    /* Initialize mutexes */
    pthread_mutex_init(&conn->seq_lock, NULL);
    pthread_mutex_init(&conn->pending_lock, NULL);
    pthread_mutex_init(&conn->handler_lock, NULL);
    pthread_mutex_init(&conn->stats_lock, NULL);

    /* Allocate buffers */
    conn->rx_buf_size = conn->config.rx_buffer_size;
    conn->tx_buf_size = conn->config.tx_buffer_size;
    conn->rx_buf = malloc(conn->rx_buf_size);
    conn->tx_buf = malloc(conn->tx_buf_size);

    if (!conn->rx_buf || !conn->tx_buf) {
        free(conn->rx_buf);
        free(conn->tx_buf);
        pthread_mutex_destroy(&conn->seq_lock);
        pthread_mutex_destroy(&conn->pending_lock);
        pthread_mutex_destroy(&conn->handler_lock);
        pthread_mutex_destroy(&conn->stats_lock);
        free(conn);
        return NULL;
    }

    /* Create transport based on configuration */
    qd_dhcp_transport_type_t transport_type = conn->config.transport;

    /* Auto-detect: try chardev first (if path exists), then netlink */
    if (transport_type == QD_DHCP_TRANSPORT_AUTO) {
        const char *chardev_path = conn->config.chardev_path;
        if (!chardev_path)
            chardev_path = QDHCP_CHARDEV_PATH;

        if (access(chardev_path, R_OK | W_OK) == 0) {
            transport_type = QD_DHCP_TRANSPORT_CHARDEV;
        } else {
            transport_type = QD_DHCP_TRANSPORT_NETLINK;
        }
    }

    switch (transport_type) {
    case QD_DHCP_TRANSPORT_CHARDEV:
        conn->transport = qd_dhcp_transport_chardev_create(conn->config.chardev_path);
        break;

    case QD_DHCP_TRANSPORT_NETLINK:
    default:
        conn->transport = qd_dhcp_transport_netlink_create();
        break;
    }

    if (!conn->transport) {
        free(conn->rx_buf);
        free(conn->tx_buf);
        pthread_mutex_destroy(&conn->seq_lock);
        pthread_mutex_destroy(&conn->pending_lock);
        pthread_mutex_destroy(&conn->handler_lock);
        pthread_mutex_destroy(&conn->stats_lock);
        free(conn);
        return NULL;
    }

    /* Open transport connection */
    int ret = conn->transport->ops->open(conn->transport);
    if (ret != QD_DHCP_OK) {
        /* If chardev fails, fall back to netlink (for AUTO mode) */
        if (conn->config.transport == QD_DHCP_TRANSPORT_AUTO &&
            transport_type == QD_DHCP_TRANSPORT_CHARDEV) {
            qd_dhcp_transport_destroy(conn->transport);
            conn->transport = qd_dhcp_transport_netlink_create();
            if (conn->transport) {
                ret = conn->transport->ops->open(conn->transport);
            }
        }

        if (ret != QD_DHCP_OK) {
            qd_dhcp_transport_destroy(conn->transport);
            free(conn->rx_buf);
            free(conn->tx_buf);
            pthread_mutex_destroy(&conn->seq_lock);
            pthread_mutex_destroy(&conn->pending_lock);
            pthread_mutex_destroy(&conn->handler_lock);
            pthread_mutex_destroy(&conn->stats_lock);
            free(conn);
            return NULL;
        }
    }

    /* Notify connection */
    if (conn->config.on_connect)
        conn->config.on_connect(conn, conn->config.callback_arg);

    return conn;
}

void qd_dhcp_disconnect(qd_dhcp_conn_t *conn)
{
    if (!conn)
        return;

    /* Detach from event loop */
    if (conn->attached)
        qd_dhcp_detach(conn);

    /* Free pending messages */
    pthread_mutex_lock(&conn->pending_lock);
    for (int i = 0; i < QD_DHCP_PENDING_MAX; i++) {
        if (conn->pending[i].msg_data) {
            free(conn->pending[i].msg_data);
            conn->pending[i].msg_data = NULL;
        }
    }
    pthread_mutex_unlock(&conn->pending_lock);

    /* Free handlers */
    pthread_mutex_lock(&conn->handler_lock);
    for (int i = 0; i < QD_DHCP_HANDLER_HASH_SIZE; i++) {
        qd_dhcp_handler_node_t *node = conn->handlers[i];
        while (node) {
            qd_dhcp_handler_node_t *next = node->next;
            free(node);
            node = next;
        }
        conn->handlers[i] = NULL;
    }
    pthread_mutex_unlock(&conn->handler_lock);

    /* Notify disconnection */
    if (conn->config.on_disconnect)
        conn->config.on_disconnect(conn, 0, conn->config.callback_arg);

    /* Close and destroy transport */
    qd_dhcp_transport_destroy(conn->transport);

    /* Free buffers */
    free(conn->rx_buf);
    free(conn->tx_buf);

    /* Destroy mutexes */
    pthread_mutex_destroy(&conn->seq_lock);
    pthread_mutex_destroy(&conn->pending_lock);
    pthread_mutex_destroy(&conn->handler_lock);
    pthread_mutex_destroy(&conn->stats_lock);

    free(conn);
}

bool qd_dhcp_is_connected(qd_dhcp_conn_t *conn)
{
    if (!conn || !conn->transport)
        return false;
    return conn->transport->ops->is_connected(conn->transport);
}

int qd_dhcp_get_fd(qd_dhcp_conn_t *conn)
{
    if (!conn || !conn->transport)
        return -1;
    return conn->transport->ops->get_fd(conn->transport);
}

void qd_dhcp_set_user_data(qd_dhcp_conn_t *conn, void *user_data)
{
    if (conn)
        conn->user_data = user_data;
}

void *qd_dhcp_get_user_data(qd_dhcp_conn_t *conn)
{
    return conn ? conn->user_data : NULL;
}

void qd_dhcp_get_conn_stats(qd_dhcp_conn_t *conn, qd_dhcp_conn_stats_t *stats)
{
    if (!conn || !stats)
        return;

    pthread_mutex_lock(&conn->stats_lock);
    *stats = conn->stats;
    pthread_mutex_unlock(&conn->stats_lock);
}

/*
 * ==========================================================================
 * Event Loop Integration
 * ==========================================================================
 */

static void on_conn_readable(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    qd_dhcp_conn_t *conn = arg;
    qd_dhcp_process(conn);
}

int qd_dhcp_attach(qd_dhcp_conn_t *conn, qd_event_loop_t *loop)
{
    if (!conn || !loop || conn->attached)
        return QD_DHCP_ERR_INVALID;

    int fd = qd_dhcp_get_fd(conn);
    if (fd < 0)
        return QD_DHCP_ERR_NOCONN;

    if (qd_event_add(loop, fd, QD_EVENT_READ, on_conn_readable, conn) != 0)
        return QD_DHCP_ERR;

    conn->loop = loop;
    conn->attached = true;
    return QD_DHCP_OK;
}

int qd_dhcp_detach(qd_dhcp_conn_t *conn)
{
    if (!conn || !conn->attached)
        return QD_DHCP_ERR_INVALID;

    int fd = qd_dhcp_get_fd(conn);
    if (fd >= 0)
        qd_event_del_fd(conn->loop, fd);

    conn->loop = NULL;
    conn->attached = false;
    return QD_DHCP_OK;
}

int qd_dhcp_subscribe(qd_dhcp_conn_t *conn)
{
    if (!conn || !conn->transport)
        return QD_DHCP_ERR_INVALID;

    int ret = conn->transport->ops->subscribe(conn->transport);
    if (ret == QD_DHCP_OK)
        conn->subscribed = true;
    return ret;
}

int qd_dhcp_unsubscribe(qd_dhcp_conn_t *conn)
{
    if (!conn || !conn->transport)
        return QD_DHCP_ERR_INVALID;

    int ret = conn->transport->ops->unsubscribe(conn->transport);
    if (ret == QD_DHCP_OK)
        conn->subscribed = false;
    return ret;
}

int qd_dhcp_process(qd_dhcp_conn_t *conn)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    int count = 0;

    while (1) {
        int ret = conn->transport->ops->recv(conn->transport,
                                              conn->rx_buf, conn->rx_buf_size,
                                              QD_DHCP_RECV_NONBLOCK);

        if (ret <= 0)
            break;

        pthread_mutex_lock(&conn->stats_lock);
        conn->stats.msgs_received++;
        conn->stats.bytes_received += ret;
        pthread_mutex_unlock(&conn->stats_lock);

        if (qd_dhcp_msg_validate(conn->rx_buf, ret) != QD_DHCP_OK)
            continue;

        qd_dhcp_msg_hdr_t *hdr = (qd_dhcp_msg_hdr_t *)conn->rx_buf;

        /* Handle ACK responses */
        if (hdr->flags & QD_DHCP_MSG_FLAG_RESPONSE) {
            qd_dhcp_process_ack(conn, hdr->seq, hdr->status);
        }

        /* Parse attributes and dispatch to handlers */
        qd_dhcp_attr_t attrs[32];
        int num_attrs = qd_dhcp_msg_parse_attrs(
            conn->rx_buf + QD_DHCP_MSG_HDR_SIZE,
            hdr->len,
            attrs, 32);

        qd_dhcp_dispatch(conn, hdr, attrs, num_attrs);
        count++;
    }

    /* Check for retransmissions */
    qd_dhcp_check_timeouts(conn);

    return count;
}

/*
 * ==========================================================================
 * Synchronous Request API
 * ==========================================================================
 */

static uint32_t get_next_seq(qd_dhcp_conn_t *conn)
{
    pthread_mutex_lock(&conn->seq_lock);
    uint32_t seq = ++conn->seq_next;
    pthread_mutex_unlock(&conn->seq_lock);
    return seq;
}

int qd_dhcp_register(qd_dhcp_conn_t *conn)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_REGISTER,
                               get_next_seq(conn), 0);
    size_t len = qd_dhcp_msg_builder_finish(&builder);

    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_unregister(qd_dhcp_conn_t *conn)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_UNREGISTER,
                               get_next_seq(conn), 0);
    size_t len = qd_dhcp_msg_builder_finish(&builder);

    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_add_interface(qd_dhcp_conn_t *conn, const qd_dhcp_iface_t *iface)
{
    if (!conn || !iface)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_ADD_INTERFACE,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_s32(&builder, QD_DHCP_ATTR_IFINDEX, iface->ifindex);
    if (iface->ifname[0])
        qd_dhcp_msg_builder_add_string(&builder, QD_DHCP_ATTR_IFNAME, iface->ifname);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_del_interface(qd_dhcp_conn_t *conn, int32_t ifindex)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_DEL_INTERFACE,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_s32(&builder, QD_DHCP_ATTR_IFINDEX, ifindex);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_set_trusted(qd_dhcp_conn_t *conn, int32_t ifindex, bool trusted)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_SET_TRUSTED,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_s32(&builder, QD_DHCP_ATTR_IFINDEX, ifindex);
    qd_dhcp_msg_builder_add_u8(&builder, QD_DHCP_ATTR_TRUSTED, trusted ? 1 : 0);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_add_binding(qd_dhcp_conn_t *conn, const qd_dhcp_binding_t *binding)
{
    if (!conn || !binding)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_ADD_BINDING,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_attr(&builder, QD_DHCP_ATTR_MAC, binding->mac, ETH_ALEN);
    qd_dhcp_msg_builder_add_u32(&builder, QD_DHCP_ATTR_IP, ntohl(binding->ip));

    if (binding->ifindex)
        qd_dhcp_msg_builder_add_s32(&builder, QD_DHCP_ATTR_IFINDEX, binding->ifindex);
    if (binding->lease_time)
        qd_dhcp_msg_builder_add_u32(&builder, QD_DHCP_ATTR_LEASE_TIME, binding->lease_time);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_del_binding(qd_dhcp_conn_t *conn, const uint8_t mac[ETH_ALEN])
{
    if (!conn || !mac)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_DEL_BINDING,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_attr(&builder, QD_DHCP_ATTR_MAC, mac, ETH_ALEN);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_clear_bindings(qd_dhcp_conn_t *conn)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_CLEAR_BINDINGS,
                               get_next_seq(conn), 0);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_set_method(qd_dhcp_conn_t *conn, uint8_t method)
{
    if (!conn)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_SET_METHOD,
                               get_next_seq(conn), 0);

    qd_dhcp_msg_builder_add_u8(&builder, QD_DHCP_ATTR_METHOD, method);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    return qd_dhcp_send_reliable(conn, conn->tx_buf, len, NULL);
}

int qd_dhcp_get_stats(qd_dhcp_conn_t *conn, qd_dhcp_stats_t *stats)
{
    if (!conn || !stats)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_msg_builder_t builder;
    qd_dhcp_msg_builder_init(&builder, conn->tx_buf, conn->tx_buf_size);
    qd_dhcp_msg_builder_reset(&builder, QD_DHCP_CMD_GET_STATS,
                               get_next_seq(conn), 0);

    size_t len = qd_dhcp_msg_builder_finish(&builder);
    uint32_t seq = ((qd_dhcp_msg_hdr_t *)conn->tx_buf)->seq;

    int ret = conn->transport->ops->send(conn->transport, conn->tx_buf, len);
    if (ret != QD_DHCP_OK)
        return ret;

    /* Wait for response with stats */
    uint64_t deadline = qd_dhcp_time_ms() + conn->config.timeout_ms;

    while (qd_dhcp_time_ms() < deadline) {
        ret = conn->transport->ops->recv(conn->transport,
                                          conn->rx_buf, conn->rx_buf_size,
                                          QD_DHCP_RECV_NONBLOCK);

        if (ret > 0 && qd_dhcp_msg_validate(conn->rx_buf, ret) == QD_DHCP_OK) {
            qd_dhcp_msg_hdr_t *hdr = (qd_dhcp_msg_hdr_t *)conn->rx_buf;

            if (hdr->seq == seq) {
                if (hdr->flags & QD_DHCP_MSG_FLAG_ERROR)
                    return hdr->status;

                /* Parse stats from response */
                qd_dhcp_attr_t attrs[16];
                int num_attrs = qd_dhcp_msg_parse_attrs(
                    conn->rx_buf + QD_DHCP_MSG_HDR_SIZE,
                    hdr->len, attrs, 16);

                size_t stats_len;
                void *stats_data = qd_dhcp_msg_find_attr(attrs, num_attrs,
                                                          QD_DHCP_ATTR_STATS, &stats_len);

                if (stats_data && stats_len == sizeof(*stats)) {
                    memcpy(stats, stats_data, sizeof(*stats));
                    return QD_DHCP_OK;
                }

                return QD_DHCP_ERR_PROTO;
            }
        }

        usleep(1000);
    }

    return QD_DHCP_ERR_TIMEOUT;
}

/*
 * ==========================================================================
 * Batch Operations API
 * ==========================================================================
 */

qd_dhcp_batch_t *qd_dhcp_batch_create(qd_dhcp_conn_t *conn, uint32_t flags)
{
    if (!conn)
        return NULL;

    qd_dhcp_batch_t *batch = calloc(1, sizeof(*batch));
    if (!batch)
        return NULL;

    batch->conn = conn;
    batch->flags = flags;
    batch->batch_id = ++conn->batch_id_next;

    return batch;
}

void qd_dhcp_batch_destroy(qd_dhcp_batch_t *batch)
{
    if (!batch)
        return;

    qd_dhcp_batch_clear(batch);
    free(batch);
}

static int batch_add_op(qd_dhcp_batch_t *batch, qd_dhcp_batch_op_t *op)
{
    if (batch->count >= QD_DHCP_BATCH_MAX_OPS) {
        free(op);
        return QD_DHCP_ERR_FULL;
    }

    op->next = NULL;

    if (batch->tail) {
        batch->tail->next = op;
        batch->tail = op;
    } else {
        batch->head = batch->tail = op;
    }

    batch->count++;
    return QD_DHCP_OK;
}

int qd_dhcp_batch_add_interface(qd_dhcp_batch_t *batch,
                                 const qd_dhcp_iface_t *iface)
{
    if (!batch || !iface)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_ERR_NOMEM;

    op->type = QD_DHCP_BATCH_OP_ADD_IFACE;
    memcpy(&op->data.iface, iface, sizeof(op->data.iface));

    return batch_add_op(batch, op);
}

int qd_dhcp_batch_del_interface(qd_dhcp_batch_t *batch, int32_t ifindex)
{
    if (!batch)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_ERR_NOMEM;

    op->type = QD_DHCP_BATCH_OP_DEL_IFACE;
    op->data.ifindex = ifindex;

    return batch_add_op(batch, op);
}

int qd_dhcp_batch_add_binding(qd_dhcp_batch_t *batch,
                               const qd_dhcp_binding_t *binding)
{
    if (!batch || !binding)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_ERR_NOMEM;

    op->type = QD_DHCP_BATCH_OP_ADD_BINDING;
    memcpy(&op->data.binding, binding, sizeof(op->data.binding));

    return batch_add_op(batch, op);
}

int qd_dhcp_batch_del_binding(qd_dhcp_batch_t *batch,
                               const uint8_t mac[ETH_ALEN])
{
    if (!batch || !mac)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_ERR_NOMEM;

    op->type = QD_DHCP_BATCH_OP_DEL_BINDING;
    memcpy(op->data.del_binding.mac, mac, ETH_ALEN);

    return batch_add_op(batch, op);
}

int qd_dhcp_batch_set_trusted(qd_dhcp_batch_t *batch,
                               int32_t ifindex, bool trusted)
{
    if (!batch)
        return QD_DHCP_ERR_INVALID;

    qd_dhcp_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_ERR_NOMEM;

    op->type = QD_DHCP_BATCH_OP_SET_TRUSTED;
    op->data.trusted.ifindex = ifindex;
    op->data.trusted.trusted = trusted;

    return batch_add_op(batch, op);
}

int qd_dhcp_batch_execute(qd_dhcp_batch_t *batch,
                           qd_dhcp_batch_result_t *result)
{
    if (!batch)
        return QD_DHCP_ERR_INVALID;

    int total = batch->count;
    int succeeded = 0;
    int failed = 0;
    int *errors = NULL;
    bool rolled_back = false;

    if (result) {
        errors = calloc(total, sizeof(int));
        if (!errors && total > 0)
            return QD_DHCP_ERR_NOMEM;
    }

    int idx = 0;
    for (qd_dhcp_batch_op_t *op = batch->head; op; op = op->next, idx++) {
        int ret = QD_DHCP_OK;

        switch (op->type) {
        case QD_DHCP_BATCH_OP_ADD_IFACE:
            ret = qd_dhcp_add_interface(batch->conn, &op->data.iface);
            break;

        case QD_DHCP_BATCH_OP_DEL_IFACE:
            ret = qd_dhcp_del_interface(batch->conn, op->data.ifindex);
            break;

        case QD_DHCP_BATCH_OP_SET_TRUSTED:
            ret = qd_dhcp_set_trusted(batch->conn,
                                       op->data.trusted.ifindex,
                                       op->data.trusted.trusted);
            break;

        case QD_DHCP_BATCH_OP_ADD_BINDING:
            ret = qd_dhcp_add_binding(batch->conn, &op->data.binding);
            break;

        case QD_DHCP_BATCH_OP_DEL_BINDING:
            ret = qd_dhcp_del_binding(batch->conn, op->data.del_binding.mac);
            break;
        }

        if (errors)
            errors[idx] = ret;

        if (ret == QD_DHCP_OK) {
            succeeded++;
        } else {
            failed++;

            /* Handle atomic batch - abort on first error */
            if ((batch->flags & QD_DHCP_BATCH_ATOMIC) &&
                !(batch->flags & QD_DHCP_BATCH_CONTINUE)) {
                /* Mark remaining as aborted */
                for (int i = idx + 1; i < total; i++) {
                    if (errors)
                        errors[i] = QD_DHCP_ERR_ABORTED;
                }
                rolled_back = true;
                break;
            }
        }
    }

    if (result) {
        result->total = total;
        result->succeeded = succeeded;
        result->failed = failed;
        result->errors = errors;
        result->flags = batch->flags;
        result->rolled_back = rolled_back;
    } else {
        free(errors);
    }

    qd_dhcp_batch_clear(batch);

    return (failed > 0) ? QD_DHCP_ERR : QD_DHCP_OK;
}

int qd_dhcp_batch_count(qd_dhcp_batch_t *batch)
{
    return batch ? batch->count : 0;
}

void qd_dhcp_batch_clear(qd_dhcp_batch_t *batch)
{
    if (!batch)
        return;

    qd_dhcp_batch_op_t *op = batch->head;
    while (op) {
        qd_dhcp_batch_op_t *next = op->next;
        free(op);
        op = next;
    }

    batch->head = NULL;
    batch->tail = NULL;
    batch->count = 0;
}

void qd_dhcp_batch_result_free(qd_dhcp_batch_result_t *result)
{
    if (result && result->errors) {
        free(result->errors);
        result->errors = NULL;
    }
}
