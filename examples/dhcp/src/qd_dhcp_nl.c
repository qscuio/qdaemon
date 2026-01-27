/*
 * QDaemon DHCP Netlink Wrapper - Implementation
 *
 * Clean wrapper API for Generic Netlink communication with the DHCP
 * kernel module. Handles socket creation, family resolution, message
 * building, attribute parsing, and handler dispatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "qd_dhcp_nl_internal.h"
#include "dhcp_kmod.h"

#ifdef STANDALONE
#include "qdaemon_stub.h"
#else
#include <qdaemon/qdaemon.h>
#endif

/*
 * Error strings
 */
static const char *error_strings[] = {
    [0] = "Success",
    [1] = "Generic error",
    [2] = "Out of memory",
    [3] = "Invalid argument",
    [4] = "Timeout",
    [5] = "Not found",
    [6] = "Full",
};

/*
 * Command strings
 */
static const char *cmd_strings[] = {
    [QD_DHCP_CMD_UNSPEC]         = "UNSPEC",
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

/*
 * Event strings
 */
static const char *event_strings[] = {
    [QD_DHCP_EVENT_NONE]        = "NONE",
    [QD_DHCP_EVENT_BINDING_ADD] = "BINDING_ADD",
    [QD_DHCP_EVENT_BINDING_DEL] = "BINDING_DEL",
    [QD_DHCP_EVENT_IFACE_ADD]   = "IFACE_ADD",
    [QD_DHCP_EVENT_IFACE_DEL]   = "IFACE_DEL",
};

/*
 * ==========================================================================
 * Utility Functions
 * ==========================================================================
 */

const char *qd_dhcp_nl_strerror(int error)
{
    int idx = -error;
    if (idx >= 0 && idx < (int)(sizeof(error_strings) / sizeof(error_strings[0])))
        return error_strings[idx];
    return "Unknown error";
}

const char *qd_dhcp_nl_cmd_str(qd_dhcp_cmd_t cmd)
{
    if (cmd < __QD_DHCP_CMD_MAX)
        return cmd_strings[cmd];
    return "UNKNOWN";
}

const char *qd_dhcp_nl_event_str(qd_dhcp_event_t event)
{
    if (event <= QD_DHCP_EVENT_IFACE_DEL)
        return event_strings[event];
    return "UNKNOWN";
}

char *qd_dhcp_nl_mac_str(const uint8_t mac[ETH_ALEN], char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

char *qd_dhcp_nl_ip_str(uint32_t ip, char *buf, size_t len)
{
    struct in_addr addr = { .s_addr = ip };
    inet_ntop(AF_INET, &addr, buf, len);
    return buf;
}

/*
 * ==========================================================================
 * Message Builder Functions
 * ==========================================================================
 */

void qd_dhcp_nl_msg_init(qd_dhcp_nl_msg_t *msg, qd_dhcp_nl_t *nl, uint8_t cmd)
{
    memset(msg, 0, sizeof(*msg));

    msg->nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    msg->nlh.nlmsg_type = nl->family_id;
    msg->nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    msg->nlh.nlmsg_seq = ++nl->seq;
    msg->nlh.nlmsg_pid = nl->pid;

    msg->genl.cmd = cmd;
    msg->genl.version = QDHCP_GENL_VERSION;

    msg->attrs_len = 0;
}

int qd_dhcp_nl_msg_add_attr(qd_dhcp_nl_msg_t *msg, uint16_t type,
                             const void *data, size_t len)
{
    struct nlattr *nla;
    size_t nla_len = NLA_HDRLEN + len;
    size_t nla_padded = NLA_ALIGN(nla_len);

    if (msg->attrs_len + nla_padded > sizeof(msg->attrs))
        return QD_DHCP_NL_NOMEM;

    nla = (struct nlattr *)(msg->attrs + msg->attrs_len);
    nla->nla_len = nla_len;
    nla->nla_type = type;

    if (data && len > 0)
        memcpy((uint8_t *)nla + NLA_HDRLEN, data, len);

    msg->attrs_len += nla_padded;
    msg->nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + msg->attrs_len);

    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_msg_add_attr_u8(qd_dhcp_nl_msg_t *msg, uint16_t type, uint8_t val)
{
    return qd_dhcp_nl_msg_add_attr(msg, type, &val, sizeof(val));
}

int qd_dhcp_nl_msg_add_attr_u32(qd_dhcp_nl_msg_t *msg, uint16_t type, uint32_t val)
{
    return qd_dhcp_nl_msg_add_attr(msg, type, &val, sizeof(val));
}

int qd_dhcp_nl_msg_add_attr_s32(qd_dhcp_nl_msg_t *msg, uint16_t type, int32_t val)
{
    return qd_dhcp_nl_msg_add_attr(msg, type, &val, sizeof(val));
}

int qd_dhcp_nl_msg_add_attr_string(qd_dhcp_nl_msg_t *msg, uint16_t type, const char *str)
{
    return qd_dhcp_nl_msg_add_attr(msg, type, str, strlen(str) + 1);
}

/*
 * ==========================================================================
 * Attribute Parsing Functions
 * ==========================================================================
 */

int qd_dhcp_nl_parse_attrs(const void *data, size_t len,
                            qd_dhcp_nl_attr_t *attrs, int max_attrs)
{
    const uint8_t *ptr = data;
    size_t remaining = len;
    int count = 0;

    while (remaining >= NLA_HDRLEN && count < max_attrs) {
        const struct nlattr *nla = (const struct nlattr *)ptr;
        size_t nla_len = NLA_ALIGN(nla->nla_len);

        if (nla->nla_len < NLA_HDRLEN || nla_len > remaining)
            break;

        attrs[count].type = nla->nla_type;
        attrs[count].len = nla->nla_len - NLA_HDRLEN;
        attrs[count].data = (void *)(ptr + NLA_HDRLEN);
        count++;

        ptr += nla_len;
        remaining -= nla_len;
    }

    return count;
}

void *qd_dhcp_nl_find_attr(const qd_dhcp_nl_attr_t *attrs, int num_attrs,
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
 * Send/Receive Functions
 * ==========================================================================
 */

int qd_dhcp_nl_send(qd_dhcp_nl_t *nl, qd_dhcp_nl_msg_t *msg)
{
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    ssize_t ret = sendto(nl->sock_fd, &msg->nlh, msg->nlh.nlmsg_len, 0,
                         (struct sockaddr *)&addr, sizeof(addr));

    if (ret < 0)
        return QD_DHCP_NL_ERROR;

    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_send_recv(qd_dhcp_nl_t *nl, qd_dhcp_nl_msg_t *msg,
                          void *resp_buf, size_t resp_size)
{
    int ret = qd_dhcp_nl_send(nl, msg);
    if (ret != QD_DHCP_NL_OK)
        return ret;

    /* Wait for response */
    struct sockaddr_nl addr;
    socklen_t addr_len = sizeof(addr);

    ssize_t recv_len = recvfrom(nl->sock_fd, nl->rx_buf, sizeof(nl->rx_buf), 0,
                                 (struct sockaddr *)&addr, &addr_len);

    if (recv_len < 0)
        return QD_DHCP_NL_ERROR;

    /* Parse response */
    struct nlmsghdr *nlh = (struct nlmsghdr *)nl->rx_buf;

    if (!NLMSG_OK(nlh, (size_t)recv_len))
        return QD_DHCP_NL_INVALID;

    /* Check for error response */
    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = NLMSG_DATA(nlh);
        if (err->error == 0) {
            /* ACK - success */
            return QD_DHCP_NL_OK;
        }
        return err->error;
    }

    /* Check for expected family response */
    if (nlh->nlmsg_type != (uint16_t)nl->family_id)
        return QD_DHCP_NL_INVALID;

    /* Copy response data if buffer provided */
    if (resp_buf && resp_size > 0) {
        struct genlmsghdr *genl = NLMSG_DATA(nlh);
        void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
        size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

        if (attr_len > resp_size)
            attr_len = resp_size;

        memcpy(resp_buf, attr_data, attr_len);
    }

    return QD_DHCP_NL_OK;
}

/*
 * ==========================================================================
 * Family ID Resolution
 * ==========================================================================
 */

int qd_dhcp_nl_resolve_family(qd_dhcp_nl_t *nl)
{
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
    req.nlh.nlmsg_seq = ++nl->seq;
    req.nlh.nlmsg_pid = nl->pid;

    req.genl.cmd = CTRL_CMD_GETFAMILY;
    req.genl.version = 1;

    req.nla.nla_len = NLA_HDRLEN + strlen(QDHCP_GENL_NAME) + 1;
    req.nla.nla_type = CTRL_ATTR_FAMILY_NAME;
    strncpy(req.name, QDHCP_GENL_NAME, GENL_NAMSIZ - 1);

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    if (sendto(nl->sock_fd, &req, req.nlh.nlmsg_len, 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return QD_DHCP_NL_ERROR;

    socklen_t addr_len = sizeof(addr);
    ssize_t recv_len = recvfrom(nl->sock_fd, nl->rx_buf, sizeof(nl->rx_buf), 0,
                                 (struct sockaddr *)&addr, &addr_len);

    if (recv_len < 0)
        return QD_DHCP_NL_ERROR;

    struct nlmsghdr *nlh = (struct nlmsghdr *)nl->rx_buf;
    if (!NLMSG_OK(nlh, (size_t)recv_len) || nlh->nlmsg_type != GENL_ID_CTRL)
        return QD_DHCP_NL_INVALID;

    struct genlmsghdr *genl = NLMSG_DATA(nlh);
    void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
    size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

    qd_dhcp_nl_attr_t attrs[32];
    int num_attrs = qd_dhcp_nl_parse_attrs(attr_data, attr_len, attrs, 32);

    uint16_t *family_id = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                                CTRL_ATTR_FAMILY_ID, NULL);
    if (!family_id)
        return QD_DHCP_NL_NOT_FOUND;

    nl->family_id = *family_id;

    /* Also look for multicast group ID */
    for (int i = 0; i < num_attrs; i++) {
        if (attrs[i].type == CTRL_ATTR_MCAST_GROUPS) {
            /* Parse nested group attributes */
            qd_dhcp_nl_attr_t group_attrs[16];
            int num_group_attrs = qd_dhcp_nl_parse_attrs(
                attrs[i].data, attrs[i].len, group_attrs, 16);

            for (int j = 0; j < num_group_attrs; j++) {
                qd_dhcp_nl_attr_t nested_attrs[4];
                int num_nested = qd_dhcp_nl_parse_attrs(
                    group_attrs[j].data, group_attrs[j].len, nested_attrs, 4);

                char *grp_name = qd_dhcp_nl_find_attr(nested_attrs, num_nested,
                                                       CTRL_ATTR_MCAST_GRP_NAME, NULL);
                uint32_t *grp_id = qd_dhcp_nl_find_attr(nested_attrs, num_nested,
                                                         CTRL_ATTR_MCAST_GRP_ID, NULL);

                if (grp_name && grp_id && strcmp(grp_name, QDHCP_MCGRP_NAME) == 0) {
                    nl->mcgrp_id = *grp_id;
                    break;
                }
            }
        }
    }

    return QD_DHCP_NL_OK;
}

/*
 * ==========================================================================
 * Handler Management
 * ==========================================================================
 */

static qd_dhcp_nl_handler_node_t *find_handler(qd_dhcp_nl_t *nl, qd_dhcp_cmd_t cmd)
{
    uint32_t hash = qd_dhcp_nl_hash_cmd(cmd);
    qd_dhcp_nl_handler_node_t *node = nl->handlers[hash];

    while (node) {
        if (node->cmd == cmd)
            return node;
        node = node->next;
    }
    return NULL;
}

int qd_dhcp_nl_register_handler(qd_dhcp_nl_t *nl,
                                 qd_dhcp_cmd_t cmd,
                                 qd_dhcp_nl_handler_fn_t handler,
                                 void *arg,
                                 uint32_t flags)
{
    if (!nl || !handler)
        return QD_DHCP_NL_INVALID;

    /* Check if already registered */
    if (find_handler(nl, cmd))
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_handler_node_t *node = calloc(1, sizeof(*node));
    if (!node)
        return QD_DHCP_NL_NOMEM;

    node->cmd = cmd;
    node->handler = handler;
    node->arg = arg;
    node->flags = flags;

    uint32_t hash = qd_dhcp_nl_hash_cmd(cmd);
    node->next = nl->handlers[hash];
    nl->handlers[hash] = node;
    nl->num_handlers++;

    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_register_handlers(qd_dhcp_nl_t *nl,
                                  const qd_dhcp_nl_handler_entry_t *handlers)
{
    if (!nl || !handlers)
        return QD_DHCP_NL_INVALID;

    for (const qd_dhcp_nl_handler_entry_t *e = handlers; e->handler; e++) {
        int ret = qd_dhcp_nl_register_handler(nl, e->cmd, e->handler,
                                               e->arg, e->flags);
        if (ret != QD_DHCP_NL_OK)
            return ret;

        /* Store name in the node */
        qd_dhcp_nl_handler_node_t *node = find_handler(nl, e->cmd);
        if (node)
            node->name = e->name;
    }

    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_unregister_handler(qd_dhcp_nl_t *nl, qd_dhcp_cmd_t cmd)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    uint32_t hash = qd_dhcp_nl_hash_cmd(cmd);
    qd_dhcp_nl_handler_node_t **pp = &nl->handlers[hash];

    while (*pp) {
        if ((*pp)->cmd == cmd) {
            qd_dhcp_nl_handler_node_t *node = *pp;
            *pp = node->next;
            free(node);
            nl->num_handlers--;
            return QD_DHCP_NL_OK;
        }
        pp = &(*pp)->next;
    }

    return QD_DHCP_NL_NOT_FOUND;
}

/*
 * ==========================================================================
 * Handler Dispatch
 * ==========================================================================
 */

int qd_dhcp_nl_dispatch(qd_dhcp_nl_t *nl, uint8_t cmd,
                         const qd_dhcp_nl_attr_t *attrs, int num_attrs)
{
    qd_dhcp_nl_handler_node_t *node = find_handler(nl, (qd_dhcp_cmd_t)cmd);
    if (!node)
        return QD_DHCP_NL_NOT_FOUND;

    /* Build context */
    qd_dhcp_nl_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cmd = (qd_dhcp_cmd_t)cmd;
    ctx.user_data = nl->user_data;

    /* Parse event type if present */
    uint8_t *event_type = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                                QDHCP_ATTR_EVENT_TYPE, NULL);
    if (event_type)
        ctx.event = (qd_dhcp_event_t)*event_type;

    /* Parse binding data if present */
    uint8_t *mac = qd_dhcp_nl_find_attr(attrs, num_attrs, QDHCP_ATTR_MAC, NULL);
    uint32_t *ip = qd_dhcp_nl_find_attr(attrs, num_attrs, QDHCP_ATTR_IP, NULL);

    if (mac) {
        memcpy(nl->tmp_binding.mac, mac, ETH_ALEN);
        if (ip)
            nl->tmp_binding.ip = htonl(*ip);

        int32_t *ifindex = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                                 QDHCP_ATTR_IFINDEX, NULL);
        if (ifindex)
            nl->tmp_binding.ifindex = *ifindex;

        uint32_t *lease = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                                QDHCP_ATTR_LEASE_TIME, NULL);
        if (lease)
            nl->tmp_binding.lease_time = *lease;

        ctx.binding = &nl->tmp_binding;
    }

    /* Parse interface data if present */
    int32_t *ifindex = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                             QDHCP_ATTR_IFINDEX, NULL);
    char *ifname = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                         QDHCP_ATTR_IFNAME, NULL);

    if (ifindex || ifname) {
        if (ifindex)
            nl->tmp_iface.ifindex = *ifindex;
        if (ifname)
            strncpy(nl->tmp_iface.ifname, ifname, IFNAMSIZ - 1);

        uint8_t *trusted = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                                 QDHCP_ATTR_TRUSTED, NULL);
        if (trusted)
            nl->tmp_iface.trusted = *trusted ? true : false;

        ctx.iface = &nl->tmp_iface;
    }

    /* Parse stats if present */
    size_t stats_len;
    void *stats_data = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                             QDHCP_ATTR_STATS, &stats_len);
    if (stats_data && stats_len == sizeof(struct qdhcp_stats)) {
        memcpy(&nl->tmp_stats, stats_data, sizeof(nl->tmp_stats));
        ctx.stats = &nl->tmp_stats;
    }

    /* Call handler */
    node->call_count++;
    qd_dhcp_nl_handler_result_t result = node->handler(&ctx, node->arg);

    if (result == QD_DHCP_NL_HANDLER_ERROR)
        node->error_count++;

    return (int)result;
}

/*
 * ==========================================================================
 * Connection Management
 * ==========================================================================
 */

qd_dhcp_nl_t *qd_dhcp_nl_create(void)
{
    qd_dhcp_nl_t *nl = calloc(1, sizeof(*nl));
    if (!nl)
        return NULL;

    /* Create netlink socket */
    nl->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl->sock_fd < 0) {
        free(nl);
        return NULL;
    }

    /* Bind socket */
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();

    if (bind(nl->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(nl->sock_fd);
        free(nl);
        return NULL;
    }

    nl->pid = addr.nl_pid;

    /* Resolve family ID */
    if (qd_dhcp_nl_resolve_family(nl) != QD_DHCP_NL_OK) {
        close(nl->sock_fd);
        free(nl);
        return NULL;
    }

    return nl;
}

void qd_dhcp_nl_destroy(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return;

    /* Detach from event loop */
    if (nl->attached)
        qd_dhcp_nl_detach(nl);

    /* Free handlers */
    for (int i = 0; i < QD_DHCP_NL_HANDLER_HASH_SIZE; i++) {
        qd_dhcp_nl_handler_node_t *node = nl->handlers[i];
        while (node) {
            qd_dhcp_nl_handler_node_t *next = node->next;
            free(node);
            node = next;
        }
    }

    /* Close socket */
    if (nl->sock_fd >= 0)
        close(nl->sock_fd);

    free(nl);
}

int qd_dhcp_nl_get_fd(qd_dhcp_nl_t *nl)
{
    return nl ? nl->sock_fd : -1;
}

bool qd_dhcp_nl_is_connected(qd_dhcp_nl_t *nl)
{
    return nl && nl->sock_fd >= 0 && nl->family_id > 0;
}

void qd_dhcp_nl_set_user_data(qd_dhcp_nl_t *nl, void *user_data)
{
    if (nl)
        nl->user_data = user_data;
}

void *qd_dhcp_nl_get_user_data(qd_dhcp_nl_t *nl)
{
    return nl ? nl->user_data : NULL;
}

/*
 * ==========================================================================
 * Event Loop Integration
 * ==========================================================================
 */

static void on_netlink_readable(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    qd_dhcp_nl_t *nl = arg;
    qd_dhcp_nl_process(nl);
}

int qd_dhcp_nl_attach(qd_dhcp_nl_t *nl, qd_event_loop_t *loop)
{
    if (!nl || !loop || nl->attached)
        return QD_DHCP_NL_INVALID;

    if (qd_event_add(loop, nl->sock_fd, QD_EVENT_READ,
                     on_netlink_readable, nl) != 0)
        return QD_DHCP_NL_ERROR;

    nl->loop = loop;
    nl->attached = true;
    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_detach(qd_dhcp_nl_t *nl)
{
    if (!nl || !nl->attached)
        return QD_DHCP_NL_INVALID;

    qd_event_del_fd(nl->loop, nl->sock_fd);
    nl->loop = NULL;
    nl->attached = false;
    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_subscribe(qd_dhcp_nl_t *nl)
{
    if (!nl || nl->mcgrp_id <= 0)
        return QD_DHCP_NL_INVALID;

    if (setsockopt(nl->sock_fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                   &nl->mcgrp_id, sizeof(nl->mcgrp_id)) < 0)
        return QD_DHCP_NL_ERROR;

    nl->subscribed = true;
    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_unsubscribe(qd_dhcp_nl_t *nl)
{
    if (!nl || !nl->subscribed)
        return QD_DHCP_NL_INVALID;

    if (setsockopt(nl->sock_fd, SOL_NETLINK, NETLINK_DROP_MEMBERSHIP,
                   &nl->mcgrp_id, sizeof(nl->mcgrp_id)) < 0)
        return QD_DHCP_NL_ERROR;

    nl->subscribed = false;
    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_process(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    struct sockaddr_nl addr;
    socklen_t addr_len = sizeof(addr);
    int count = 0;

    while (1) {
        ssize_t recv_len = recvfrom(nl->sock_fd, nl->rx_buf, sizeof(nl->rx_buf),
                                     MSG_DONTWAIT, (struct sockaddr *)&addr, &addr_len);

        if (recv_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return QD_DHCP_NL_ERROR;
        }

        struct nlmsghdr *nlh = (struct nlmsghdr *)nl->rx_buf;
        if (!NLMSG_OK(nlh, (size_t)recv_len))
            continue;

        if (nlh->nlmsg_type != (uint16_t)nl->family_id)
            continue;

        struct genlmsghdr *genl = NLMSG_DATA(nlh);
        void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
        size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

        qd_dhcp_nl_attr_t attrs[32];
        int num_attrs = qd_dhcp_nl_parse_attrs(attr_data, attr_len, attrs, 32);

        qd_dhcp_nl_dispatch(nl, genl->cmd, attrs, num_attrs);
        count++;
    }

    return count;
}

/*
 * ==========================================================================
 * Synchronous Request API
 * ==========================================================================
 */

int qd_dhcp_nl_register(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_REGISTER);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_unregister(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_UNREGISTER);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_add_interface(qd_dhcp_nl_t *nl, const qd_dhcp_iface_t *iface)
{
    if (!nl || !iface)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_ADD_INTERFACE);

    qd_dhcp_nl_msg_add_attr_s32(&msg, QDHCP_ATTR_IFINDEX, iface->ifindex);

    if (iface->ifname[0])
        qd_dhcp_nl_msg_add_attr_string(&msg, QDHCP_ATTR_IFNAME, iface->ifname);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_del_interface(qd_dhcp_nl_t *nl, int32_t ifindex)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_DEL_INTERFACE);

    qd_dhcp_nl_msg_add_attr_s32(&msg, QDHCP_ATTR_IFINDEX, ifindex);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_set_trusted(qd_dhcp_nl_t *nl, int32_t ifindex, bool trusted)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_SET_TRUSTED);

    qd_dhcp_nl_msg_add_attr_s32(&msg, QDHCP_ATTR_IFINDEX, ifindex);
    qd_dhcp_nl_msg_add_attr_u8(&msg, QDHCP_ATTR_TRUSTED, trusted ? 1 : 0);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_add_binding(qd_dhcp_nl_t *nl, const qd_dhcp_binding_t *binding)
{
    if (!nl || !binding)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_ADD_BINDING);

    qd_dhcp_nl_msg_add_attr(&msg, QDHCP_ATTR_MAC, binding->mac, ETH_ALEN);
    qd_dhcp_nl_msg_add_attr_u32(&msg, QDHCP_ATTR_IP, ntohl(binding->ip));

    if (binding->ifindex)
        qd_dhcp_nl_msg_add_attr_s32(&msg, QDHCP_ATTR_IFINDEX, binding->ifindex);

    if (binding->lease_time)
        qd_dhcp_nl_msg_add_attr_u32(&msg, QDHCP_ATTR_LEASE_TIME, binding->lease_time);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_del_binding(qd_dhcp_nl_t *nl, const uint8_t mac[ETH_ALEN])
{
    if (!nl || !mac)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_DEL_BINDING);

    qd_dhcp_nl_msg_add_attr(&msg, QDHCP_ATTR_MAC, mac, ETH_ALEN);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_clear_bindings(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_CLEAR_BINDINGS);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_set_method(qd_dhcp_nl_t *nl, uint8_t method)
{
    if (!nl)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_SET_METHOD);

    qd_dhcp_nl_msg_add_attr_u8(&msg, QDHCP_ATTR_METHOD, method);

    return qd_dhcp_nl_send_recv(nl, &msg, NULL, 0);
}

int qd_dhcp_nl_get_stats(qd_dhcp_nl_t *nl, qd_dhcp_stats_t *stats)
{
    if (!nl || !stats)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_msg_t msg;
    qd_dhcp_nl_msg_init(&msg, nl, QDHCP_CMD_GET_STATS);

    /* Send request and receive response */
    int ret = qd_dhcp_nl_send(nl, &msg);
    if (ret != QD_DHCP_NL_OK)
        return ret;

    /* Wait for response */
    struct sockaddr_nl addr;
    socklen_t addr_len = sizeof(addr);

    ssize_t recv_len = recvfrom(nl->sock_fd, nl->rx_buf, sizeof(nl->rx_buf), 0,
                                 (struct sockaddr *)&addr, &addr_len);

    if (recv_len < 0)
        return QD_DHCP_NL_ERROR;

    struct nlmsghdr *nlh = (struct nlmsghdr *)nl->rx_buf;
    if (!NLMSG_OK(nlh, (size_t)recv_len))
        return QD_DHCP_NL_INVALID;

    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = NLMSG_DATA(nlh);
        return err->error;
    }

    if (nlh->nlmsg_type != (uint16_t)nl->family_id)
        return QD_DHCP_NL_INVALID;

    struct genlmsghdr *genl = NLMSG_DATA(nlh);
    void *attr_data = (uint8_t *)genl + GENL_HDRLEN;
    size_t attr_len = nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;

    qd_dhcp_nl_attr_t attrs[16];
    int num_attrs = qd_dhcp_nl_parse_attrs(attr_data, attr_len, attrs, 16);

    size_t stats_len;
    void *stats_data = qd_dhcp_nl_find_attr(attrs, num_attrs,
                                             QDHCP_ATTR_STATS, &stats_len);
    if (!stats_data || stats_len != sizeof(struct qdhcp_stats))
        return QD_DHCP_NL_INVALID;

    /* Copy kernel stats structure to user stats structure */
    struct qdhcp_stats *kstats = stats_data;
    stats->rx_packets = kstats->rx_packets;
    stats->tx_packets = kstats->tx_packets;
    stats->rx_bytes = kstats->rx_bytes;
    stats->tx_bytes = kstats->tx_bytes;
    stats->rx_dropped = kstats->rx_dropped;
    stats->tx_dropped = kstats->tx_dropped;
    stats->dhcp_discover = kstats->dhcp_discover;
    stats->dhcp_offer = kstats->dhcp_offer;
    stats->dhcp_request = kstats->dhcp_request;
    stats->dhcp_ack = kstats->dhcp_ack;
    stats->dhcp_nak = kstats->dhcp_nak;
    stats->dhcp_release = kstats->dhcp_release;
    stats->dhcp_decline = kstats->dhcp_decline;
    stats->dev_pack_intercepts = kstats->dev_pack_intercepts;
    stats->tc_intercepts = kstats->tc_intercepts;
    stats->binding_count = kstats->binding_count;

    return QD_DHCP_NL_OK;
}

/*
 * ==========================================================================
 * Batch Operations API
 * ==========================================================================
 */

qd_dhcp_nl_batch_t *qd_dhcp_nl_batch_create(qd_dhcp_nl_t *nl)
{
    if (!nl)
        return NULL;

    qd_dhcp_nl_batch_t *batch = calloc(1, sizeof(*batch));
    if (!batch)
        return NULL;

    batch->nl = nl;
    return batch;
}

void qd_dhcp_nl_batch_destroy(qd_dhcp_nl_batch_t *batch)
{
    if (!batch)
        return;

    qd_dhcp_nl_batch_clear(batch);
    free(batch);
}

static int batch_add_op(qd_dhcp_nl_batch_t *batch, qd_dhcp_nl_batch_op_t *op)
{
    if (batch->count >= QD_DHCP_NL_BATCH_MAX_OPS) {
        free(op);
        return QD_DHCP_NL_FULL;
    }

    op->next = NULL;

    if (batch->tail) {
        batch->tail->next = op;
        batch->tail = op;
    } else {
        batch->head = batch->tail = op;
    }

    batch->count++;
    return QD_DHCP_NL_OK;
}

int qd_dhcp_nl_batch_add_interface(qd_dhcp_nl_batch_t *batch,
                                    const qd_dhcp_iface_t *iface)
{
    if (!batch || !iface)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_NL_NOMEM;

    op->type = QD_DHCP_BATCH_OP_ADD_IFACE;
    memcpy(&op->data.iface, iface, sizeof(op->data.iface));

    return batch_add_op(batch, op);
}

int qd_dhcp_nl_batch_del_interface(qd_dhcp_nl_batch_t *batch, int32_t ifindex)
{
    if (!batch)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_NL_NOMEM;

    op->type = QD_DHCP_BATCH_OP_DEL_IFACE;
    op->data.ifindex = ifindex;

    return batch_add_op(batch, op);
}

int qd_dhcp_nl_batch_add_binding(qd_dhcp_nl_batch_t *batch,
                                  const qd_dhcp_binding_t *binding)
{
    if (!batch || !binding)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_NL_NOMEM;

    op->type = QD_DHCP_BATCH_OP_ADD_BINDING;
    memcpy(&op->data.binding, binding, sizeof(op->data.binding));

    return batch_add_op(batch, op);
}

int qd_dhcp_nl_batch_del_binding(qd_dhcp_nl_batch_t *batch,
                                  const uint8_t mac[ETH_ALEN])
{
    if (!batch || !mac)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_NL_NOMEM;

    op->type = QD_DHCP_BATCH_OP_DEL_BINDING;
    memcpy(op->data.del_binding.mac, mac, ETH_ALEN);

    return batch_add_op(batch, op);
}

int qd_dhcp_nl_batch_set_trusted(qd_dhcp_nl_batch_t *batch,
                                  int32_t ifindex, bool trusted)
{
    if (!batch)
        return QD_DHCP_NL_INVALID;

    qd_dhcp_nl_batch_op_t *op = calloc(1, sizeof(*op));
    if (!op)
        return QD_DHCP_NL_NOMEM;

    op->type = QD_DHCP_BATCH_OP_SET_TRUSTED;
    op->data.trusted.ifindex = ifindex;
    op->data.trusted.trusted = trusted;

    return batch_add_op(batch, op);
}

int qd_dhcp_nl_batch_execute(qd_dhcp_nl_batch_t *batch,
                              qd_dhcp_nl_batch_result_t *result)
{
    if (!batch)
        return QD_DHCP_NL_INVALID;

    int total = batch->count;
    int succeeded = 0;
    int failed = 0;
    int *errors = NULL;

    if (result) {
        errors = calloc(total, sizeof(int));
        if (!errors && total > 0)
            return QD_DHCP_NL_NOMEM;
    }

    int idx = 0;
    for (qd_dhcp_nl_batch_op_t *op = batch->head; op; op = op->next, idx++) {
        int ret = QD_DHCP_NL_OK;

        switch (op->type) {
        case QD_DHCP_BATCH_OP_ADD_IFACE:
            ret = qd_dhcp_nl_add_interface(batch->nl, &op->data.iface);
            break;

        case QD_DHCP_BATCH_OP_DEL_IFACE:
            ret = qd_dhcp_nl_del_interface(batch->nl, op->data.ifindex);
            break;

        case QD_DHCP_BATCH_OP_SET_TRUSTED:
            ret = qd_dhcp_nl_set_trusted(batch->nl,
                                          op->data.trusted.ifindex,
                                          op->data.trusted.trusted);
            break;

        case QD_DHCP_BATCH_OP_ADD_BINDING:
            ret = qd_dhcp_nl_add_binding(batch->nl, &op->data.binding);
            break;

        case QD_DHCP_BATCH_OP_DEL_BINDING:
            ret = qd_dhcp_nl_del_binding(batch->nl, op->data.del_binding.mac);
            break;
        }

        if (ret == QD_DHCP_NL_OK) {
            succeeded++;
        } else {
            failed++;
        }

        if (errors)
            errors[idx] = ret;
    }

    if (result) {
        result->total = total;
        result->succeeded = succeeded;
        result->failed = failed;
        result->errors = errors;
    } else {
        free(errors);
    }

    /* Clear the batch after execution */
    qd_dhcp_nl_batch_clear(batch);

    return (failed > 0) ? QD_DHCP_NL_ERROR : QD_DHCP_NL_OK;
}

int qd_dhcp_nl_batch_count(qd_dhcp_nl_batch_t *batch)
{
    return batch ? batch->count : 0;
}

void qd_dhcp_nl_batch_clear(qd_dhcp_nl_batch_t *batch)
{
    if (!batch)
        return;

    qd_dhcp_nl_batch_op_t *op = batch->head;
    while (op) {
        qd_dhcp_nl_batch_op_t *next = op->next;
        free(op);
        op = next;
    }

    batch->head = NULL;
    batch->tail = NULL;
    batch->count = 0;
}
