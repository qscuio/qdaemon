/*
 * QDaemon - Netlink Implementation
 * Generic Netlink interface for kernel-userspace communication
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "qdaemon/qd_netlink.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

/* NLA_OK macro if not defined */
#ifndef NLA_OK
#define NLA_OK(nla,len) ((len) >= (int)sizeof(struct nlattr) && \
                         (nla)->nla_len >= sizeof(struct nlattr) && \
                         (nla)->nla_len <= (len))
#endif

/* Netlink message helpers */
#define NLMSG_TAIL(nmsg) \
    ((struct nlattr *)(((void *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static int nla_put(struct nlmsghdr *nlh, int type, int len, const void *data)
{
    struct nlattr *nla = NLMSG_TAIL(nlh);
    int payload_len = NLA_HDRLEN + len;
    int aligned_len = NLA_ALIGN(payload_len);

    nla->nla_type = type;
    nla->nla_len = payload_len;
    memcpy((char *)nla + NLA_HDRLEN, data, len);
    nlh->nlmsg_len += aligned_len;

    return 0;
}

static int nla_put_string(struct nlmsghdr *nlh, int type, const char *str)
{
    return nla_put(nlh, type, strlen(str) + 1, str);
}

static int __attribute__((unused)) nla_put_u32(struct nlmsghdr *nlh, int type, uint32_t val)
{
    return nla_put(nlh, type, sizeof(val), &val);
}

static int __attribute__((unused)) nla_put_u64(struct nlmsghdr *nlh, int type, uint64_t val)
{
    return nla_put(nlh, type, sizeof(val), &val);
}

static int __attribute__((unused)) nla_put_s32(struct nlmsghdr *nlh, int type, int32_t val)
{
    return nla_put(nlh, type, sizeof(val), &val);
}

qd_netlink_t *qd_netlink_create(const char *family_name)
{
    qd_netlink_config_t config = QD_NETLINK_CONFIG_DEFAULT;
    if (family_name)
        config.family_name = family_name;
    return qd_netlink_create_ex(&config);
}

qd_netlink_t *qd_netlink_create_ex(const qd_netlink_config_t *config)
{
    if (!config)
        return NULL;

    qd_netlink_t *nl = calloc(1, sizeof(qd_netlink_t));
    if (!nl)
        return NULL;

    if (config->family_name)
        strncpy(nl->family_name, config->family_name, sizeof(nl->family_name) - 1);
    else
        strncpy(nl->family_name, QD_NETLINK_FAMILY_NAME, sizeof(nl->family_name) - 1);

    nl->recv_buf_size = config->recv_buf_size > 0 ? config->recv_buf_size : 32768;
    nl->send_buf_size = config->send_buf_size > 0 ? config->send_buf_size : 32768;

    /* Allocate buffers */
    nl->recv_buf = malloc(nl->recv_buf_size);
    nl->send_buf = malloc(nl->send_buf_size);
    if (!nl->recv_buf || !nl->send_buf) {
        free(nl->recv_buf);
        free(nl->send_buf);
        free(nl);
        return NULL;
    }

    /* Create netlink socket */
    nl->sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nl->sock_fd == -1) {
        free(nl->recv_buf);
        free(nl->send_buf);
        free(nl);
        return NULL;
    }

    /* Set socket to non-blocking if configured */
    if (config->nonblocking) {
        int flags = fcntl(nl->sock_fd, F_GETFL, 0);
        fcntl(nl->sock_fd, F_SETFL, flags | O_NONBLOCK);
    }

    /* Bind socket */
    memset(&nl->addr, 0, sizeof(nl->addr));
    nl->addr.nl_family = AF_NETLINK;
    nl->addr.nl_pid = getpid();
    nl->addr.nl_groups = 0;

    if (bind(nl->sock_fd, (struct sockaddr *)&nl->addr, sizeof(nl->addr)) == -1) {
        close(nl->sock_fd);
        free(nl->recv_buf);
        free(nl->send_buf);
        free(nl);
        return NULL;
    }

    nl->pid = getpid();
    nl->seq = 1;

    pthread_mutex_init(&nl->lock, NULL);

    /* Resolve family ID */
    if (qd_netlink_resolve_family(nl) != QD_OK) {
        /* Family might not exist yet (kernel module not loaded) */
        /* Don't fail - allow operations that don't need the family */
    }

    return nl;
}

void qd_netlink_destroy(qd_netlink_t *nl)
{
    if (!nl)
        return;

    if (nl->sock_fd >= 0)
        close(nl->sock_fd);

    free(nl->recv_buf);
    free(nl->send_buf);
    pthread_mutex_destroy(&nl->lock);
    free(nl);
}

int qd_netlink_resolve_family(qd_netlink_t *nl)
{
    if (!nl)
        return QD_ERR_INVAL;

    struct {
        struct nlmsghdr nlh;
        struct genlmsghdr genlh;
        char attrs[256];
    } req;

    memset(&req, 0, sizeof(req));

    /* Build request */
    req.nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req.nlh.nlmsg_type = GENL_ID_CTRL;
    req.nlh.nlmsg_flags = NLM_F_REQUEST;
    req.nlh.nlmsg_seq = nl->seq++;
    req.nlh.nlmsg_pid = nl->pid;

    req.genlh.cmd = CTRL_CMD_GETFAMILY;
    req.genlh.version = 1;

    nla_put_string(&req.nlh, CTRL_ATTR_FAMILY_NAME, nl->family_name);

    /* Send request */
    struct sockaddr_nl dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;

    if (sendto(nl->sock_fd, &req, req.nlh.nlmsg_len, 0,
               (struct sockaddr *)&dest_addr, sizeof(dest_addr)) == -1) {
        return QD_ERR_IO;
    }

    /* Receive response */
    struct sockaddr_nl src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ssize_t len = recvfrom(nl->sock_fd, nl->recv_buf, nl->recv_buf_size, 0,
                           (struct sockaddr *)&src_addr, &addr_len);
    if (len <= 0)
        return QD_ERR_IO;

    /* Parse response */
    struct nlmsghdr *nlh = nl->recv_buf;
    if (!NLMSG_OK(nlh, (size_t)len))
        return QD_ERR_PROTO;

    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = NLMSG_DATA(nlh);
        if (err->error != 0)
            return QD_ERR_NOENT;
    }

    /* Extract family ID from attributes */
    struct genlmsghdr *genlh = NLMSG_DATA(nlh);
    struct nlattr *attr = (struct nlattr *)((char *)genlh + GENL_HDRLEN);
    int remaining = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

    while (NLA_OK(attr, remaining)) {
        if (attr->nla_type == CTRL_ATTR_FAMILY_ID) {
            nl->family_id = *(uint16_t *)((char *)attr + NLA_HDRLEN);
            return QD_OK;
        }
        attr = (struct nlattr *)((char *)attr + NLA_ALIGN(attr->nla_len));
        remaining -= NLA_ALIGN(attr->nla_len);
    }

    return QD_ERR_NOENT;
}

int qd_netlink_attach(qd_netlink_t *nl, qd_event_loop_t *loop)
{
    if (!nl || !loop)
        return QD_ERR_INVAL;

    nl->loop = loop;
    /* Event source would be added here for async handling */
    return QD_OK;
}

void qd_netlink_detach(qd_netlink_t *nl)
{
    if (!nl)
        return;
    nl->loop = NULL;
}

void qd_netlink_set_callback(qd_netlink_t *nl, qd_netlink_cb_t callback, void *arg)
{
    if (!nl)
        return;
    nl->msg_callback = callback;
    nl->msg_callback_arg = arg;
}

void qd_netlink_set_error_callback(qd_netlink_t *nl, qd_netlink_err_cb_t callback, void *arg)
{
    if (!nl)
        return;
    nl->err_callback = callback;
    nl->err_callback_arg = arg;
}

int qd_netlink_send(qd_netlink_t *nl, uint8_t cmd, void *data, size_t len)
{
    if (!nl)
        return QD_ERR_INVAL;

    if (nl->family_id == 0)
        return QD_ERR_NOENT;

    struct {
        struct nlmsghdr nlh;
        struct genlmsghdr genlh;
        char attrs[QD_NETLINK_MAX_PAYLOAD];
    } *req = nl->send_buf;

    memset(req, 0, sizeof(*req));

    /* Build message */
    req->nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req->nlh.nlmsg_type = nl->family_id;
    req->nlh.nlmsg_flags = NLM_F_REQUEST;
    req->nlh.nlmsg_seq = nl->seq++;
    req->nlh.nlmsg_pid = nl->pid;

    req->genlh.cmd = cmd;
    req->genlh.version = QD_NETLINK_VERSION;

    /* Add data attribute */
    if (data && len > 0) {
        nla_put(&req->nlh, QD_NL_ATTR_DATA, len, data);
    }

    /* Send */
    struct sockaddr_nl dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;

    pthread_mutex_lock(&nl->lock);
    ssize_t n = sendto(nl->sock_fd, req, req->nlh.nlmsg_len, 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    pthread_mutex_unlock(&nl->lock);

    if (n == -1)
        return QD_ERR_IO;

    nl->msgs_sent++;
    return QD_OK;
}

int qd_netlink_send_msg(qd_netlink_t *nl, qd_netlink_msg_t *msg)
{
    if (!nl || !msg)
        return QD_ERR_INVAL;

    if (nl->family_id == 0)
        return QD_ERR_NOENT;

    struct {
        struct nlmsghdr nlh;
        struct genlmsghdr genlh;
        char attrs[QD_NETLINK_MAX_PAYLOAD];
    } *req = nl->send_buf;

    memset(req, 0, sizeof(*req));

    /* Build message */
    req->nlh.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    req->nlh.nlmsg_type = nl->family_id;
    req->nlh.nlmsg_flags = NLM_F_REQUEST | msg->flags;
    req->nlh.nlmsg_seq = nl->seq++;
    req->nlh.nlmsg_pid = nl->pid;

    req->genlh.cmd = msg->cmd;
    req->genlh.version = QD_NETLINK_VERSION;

    /* Add attributes */
    for (int i = 0; i < msg->num_attrs; i++) {
        nla_put(&req->nlh, msg->attrs[i].type, msg->attrs[i].len, msg->attrs[i].data);
    }

    /* Send */
    struct sockaddr_nl dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;

    pthread_mutex_lock(&nl->lock);
    ssize_t n = sendto(nl->sock_fd, req, req->nlh.nlmsg_len, 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    pthread_mutex_unlock(&nl->lock);

    if (n == -1)
        return QD_ERR_IO;

    nl->msgs_sent++;
    return QD_OK;
}

int qd_netlink_recv(qd_netlink_t *nl, qd_netlink_msg_t *msg, int timeout_ms)
{
    if (!nl || !msg)
        return QD_ERR_INVAL;

    /* Poll for data */
    if (timeout_ms != 0) {
        struct pollfd pfd = { .fd = nl->sock_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret == 0)
            return QD_ERR_TIMEOUT;
        if (ret < 0)
            return QD_ERR_IO;
    }

    /* Receive */
    struct sockaddr_nl src_addr;
    socklen_t addr_len = sizeof(src_addr);

    ssize_t len = recvfrom(nl->sock_fd, nl->recv_buf, nl->recv_buf_size, 0,
                           (struct sockaddr *)&src_addr, &addr_len);
    if (len <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return QD_ERR_AGAIN;
        return QD_ERR_IO;
    }

    /* Parse response */
    struct nlmsghdr *nlh = nl->recv_buf;
    if (!NLMSG_OK(nlh, (size_t)len))
        return QD_ERR_PROTO;

    if (nlh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = NLMSG_DATA(nlh);
        nl->errors++;
        return err->error;
    }

    /* Parse generic netlink header */
    struct genlmsghdr *genlh = NLMSG_DATA(nlh);
    qd_netlink_msg_clear(msg);
    msg->cmd = genlh->cmd;
    msg->seq = nlh->nlmsg_seq;
    msg->pid = nlh->nlmsg_pid;

    /* Parse attributes */
    struct nlattr *attr = (struct nlattr *)((char *)genlh + GENL_HDRLEN);
    int remaining = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

    while (NLA_OK(attr, remaining) && msg->num_attrs < QD_NETLINK_MAX_ATTRS) {
        msg->attrs[msg->num_attrs].type = attr->nla_type;
        msg->attrs[msg->num_attrs].len = attr->nla_len - NLA_HDRLEN;
        msg->attrs[msg->num_attrs].data = (char *)attr + NLA_HDRLEN;
        msg->num_attrs++;

        int attr_len = NLA_ALIGN(attr->nla_len);
        attr = (struct nlattr *)((char *)attr + attr_len);
        remaining -= attr_len;
    }

    nl->msgs_received++;
    return QD_OK;
}

int qd_netlink_request(qd_netlink_t *nl, qd_netlink_msg_t *request,
                        qd_netlink_msg_t *response, int timeout_ms)
{
    int ret = qd_netlink_send_msg(nl, request);
    if (ret != QD_OK)
        return ret;

    return qd_netlink_recv(nl, response, timeout_ms);
}

int qd_netlink_process(qd_netlink_t *nl)
{
    if (!nl)
        return QD_ERR_INVAL;

    qd_netlink_msg_t msg;
    int count = 0;

    while (1) {
        int ret = qd_netlink_recv(nl, &msg, 0);
        if (ret == QD_ERR_AGAIN)
            break;
        if (ret != QD_OK)
            return ret;

        if (nl->msg_callback)
            nl->msg_callback(nl, &msg, nl->msg_callback_arg);

        count++;
    }

    return count;
}

int qd_netlink_subscribe(qd_netlink_t *nl, uint32_t group)
{
    if (!nl)
        return QD_ERR_INVAL;

    if (setsockopt(nl->sock_fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
                   &group, sizeof(group)) == -1) {
        return QD_ERR_IO;
    }

    nl->subscribed_groups |= (1 << group);
    return QD_OK;
}

int qd_netlink_unsubscribe(qd_netlink_t *nl, uint32_t group)
{
    if (!nl)
        return QD_ERR_INVAL;

    if (setsockopt(nl->sock_fd, SOL_NETLINK, NETLINK_DROP_MEMBERSHIP,
                   &group, sizeof(group)) == -1) {
        return QD_ERR_IO;
    }

    nl->subscribed_groups &= ~(1 << group);
    return QD_OK;
}

/*
 * Message Building
 */

void qd_netlink_msg_init(qd_netlink_msg_t *msg, uint8_t cmd)
{
    if (!msg)
        return;
    memset(msg, 0, sizeof(*msg));
    msg->cmd = cmd;
}

void qd_netlink_msg_clear(qd_netlink_msg_t *msg)
{
    if (!msg)
        return;
    memset(msg, 0, sizeof(*msg));
}

int qd_netlink_msg_add_attr(qd_netlink_msg_t *msg, uint16_t type,
                             const void *data, uint16_t len)
{
    if (!msg || msg->num_attrs >= QD_NETLINK_MAX_ATTRS)
        return QD_ERR_INVAL;

    msg->attrs[msg->num_attrs].type = type;
    msg->attrs[msg->num_attrs].len = len;
    msg->attrs[msg->num_attrs].data = (void *)data;
    msg->num_attrs++;

    return QD_OK;
}

int qd_netlink_msg_add_str(qd_netlink_msg_t *msg, uint16_t type, const char *str)
{
    if (!str)
        return QD_ERR_INVAL;
    return qd_netlink_msg_add_attr(msg, type, str, strlen(str) + 1);
}

int qd_netlink_msg_add_u32(qd_netlink_msg_t *msg, uint16_t type, uint32_t val)
{
    /* Note: caller must ensure value remains valid */
    return qd_netlink_msg_add_attr(msg, type, &val, sizeof(val));
}

int qd_netlink_msg_add_u64(qd_netlink_msg_t *msg, uint16_t type, uint64_t val)
{
    return qd_netlink_msg_add_attr(msg, type, &val, sizeof(val));
}

int qd_netlink_msg_add_s32(qd_netlink_msg_t *msg, uint16_t type, int32_t val)
{
    return qd_netlink_msg_add_attr(msg, type, &val, sizeof(val));
}

/*
 * Message Parsing
 */

qd_nl_attr_t *qd_netlink_msg_get_attr(qd_netlink_msg_t *msg, uint16_t type)
{
    if (!msg)
        return NULL;

    for (int i = 0; i < msg->num_attrs; i++) {
        if (msg->attrs[i].type == type)
            return &msg->attrs[i];
    }
    return NULL;
}

const char *qd_netlink_msg_get_str(qd_netlink_msg_t *msg, uint16_t type)
{
    qd_nl_attr_t *attr = qd_netlink_msg_get_attr(msg, type);
    if (!attr)
        return NULL;
    return attr->data;
}

int qd_netlink_msg_get_u32(qd_netlink_msg_t *msg, uint16_t type, uint32_t *val)
{
    qd_nl_attr_t *attr = qd_netlink_msg_get_attr(msg, type);
    if (!attr || attr->len < sizeof(uint32_t))
        return QD_ERR_NOENT;
    *val = *(uint32_t *)attr->data;
    return QD_OK;
}

int qd_netlink_msg_get_u64(qd_netlink_msg_t *msg, uint16_t type, uint64_t *val)
{
    qd_nl_attr_t *attr = qd_netlink_msg_get_attr(msg, type);
    if (!attr || attr->len < sizeof(uint64_t))
        return QD_ERR_NOENT;
    *val = *(uint64_t *)attr->data;
    return QD_OK;
}

int qd_netlink_msg_get_s32(qd_netlink_msg_t *msg, uint16_t type, int32_t *val)
{
    qd_nl_attr_t *attr = qd_netlink_msg_get_attr(msg, type);
    if (!attr || attr->len < sizeof(int32_t))
        return QD_ERR_NOENT;
    *val = *(int32_t *)attr->data;
    return QD_OK;
}

void *qd_netlink_msg_get_data(qd_netlink_msg_t *msg, uint16_t type, size_t *len)
{
    qd_nl_attr_t *attr = qd_netlink_msg_get_attr(msg, type);
    if (!attr)
        return NULL;
    if (len)
        *len = attr->len;
    return attr->data;
}

/*
 * Convenience Functions
 */

int qd_netlink_echo(qd_netlink_t *nl, const char *message, char *response, size_t len)
{
    if (!nl || !message)
        return QD_ERR_INVAL;

    qd_netlink_msg_t req, resp;
    qd_netlink_msg_init(&req, QD_NL_CMD_ECHO);
    qd_netlink_msg_add_str(&req, QD_NL_ATTR_MSG, message);

    int ret = qd_netlink_request(nl, &req, &resp, 5000);
    if (ret != QD_OK)
        return ret;

    const char *reply = qd_netlink_msg_get_str(&resp, QD_NL_ATTR_MSG);
    if (reply && response && len > 0) {
        strncpy(response, reply, len - 1);
        response[len - 1] = '\0';
    }

    return QD_OK;
}

int qd_netlink_get_info(qd_netlink_t *nl, char *info, size_t len)
{
    if (!nl)
        return QD_ERR_INVAL;

    qd_netlink_msg_t req, resp;
    qd_netlink_msg_init(&req, QD_NL_CMD_GET_INFO);

    int ret = qd_netlink_request(nl, &req, &resp, 5000);
    if (ret != QD_OK)
        return ret;

    const char *reply = qd_netlink_msg_get_str(&resp, QD_NL_ATTR_MSG);
    if (reply && info && len > 0) {
        strncpy(info, reply, len - 1);
        info[len - 1] = '\0';
    }

    return QD_OK;
}

int qd_netlink_register(qd_netlink_t *nl, const char *name)
{
    if (!nl || !name)
        return QD_ERR_INVAL;

    qd_netlink_msg_t req, resp;
    qd_netlink_msg_init(&req, QD_NL_CMD_REGISTER);
    qd_netlink_msg_add_str(&req, QD_NL_ATTR_NAME, name);
    qd_netlink_msg_add_u32(&req, QD_NL_ATTR_PID, getpid());

    return qd_netlink_request(nl, &req, &resp, 5000);
}

int qd_netlink_unregister(qd_netlink_t *nl)
{
    if (!nl)
        return QD_ERR_INVAL;

    qd_netlink_msg_t req, resp;
    qd_netlink_msg_init(&req, QD_NL_CMD_UNREGISTER);
    qd_netlink_msg_add_u32(&req, QD_NL_ATTR_PID, getpid());

    return qd_netlink_request(nl, &req, &resp, 5000);
}

void qd_netlink_stats(qd_netlink_t *nl, qd_netlink_stats_t *stats)
{
    if (!nl || !stats)
        return;

    stats->msgs_sent = nl->msgs_sent;
    stats->msgs_received = nl->msgs_received;
    stats->errors = nl->errors;
    stats->subscribed_groups = nl->subscribed_groups;
}
