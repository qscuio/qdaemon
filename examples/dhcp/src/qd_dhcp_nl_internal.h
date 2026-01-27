/*
 * QDaemon DHCP Netlink Wrapper - Internal Header
 *
 * Internal structures not exposed to users.
 * This file should only be included by qd_dhcp_nl.c
 */

#ifndef QD_DHCP_NL_INTERNAL_H
#define QD_DHCP_NL_INTERNAL_H

#include "qd_dhcp_nl.h"
#include <linux/netlink.h>
#include <linux/genetlink.h>

/*
 * Constants
 */
#define QD_DHCP_NL_HANDLER_HASH_SIZE  32
#define QD_DHCP_NL_MSG_SIZE           4096
#define QD_DHCP_NL_BATCH_MAX_OPS      256
#define QD_DHCP_NL_TIMEOUT_MS         5000

/*
 * Handler node in hash table
 */
typedef struct qd_dhcp_nl_handler_node {
    qd_dhcp_cmd_t               cmd;
    qd_dhcp_nl_handler_fn_t     handler;
    void                       *arg;
    uint32_t                    flags;
    const char                 *name;
    struct qd_dhcp_nl_handler_node *next;

    /* Statistics */
    uint64_t call_count;
    uint64_t error_count;
} qd_dhcp_nl_handler_node_t;

/*
 * Batch operation entry
 */
typedef enum {
    QD_DHCP_BATCH_OP_ADD_IFACE,
    QD_DHCP_BATCH_OP_DEL_IFACE,
    QD_DHCP_BATCH_OP_SET_TRUSTED,
    QD_DHCP_BATCH_OP_ADD_BINDING,
    QD_DHCP_BATCH_OP_DEL_BINDING,
} qd_dhcp_batch_op_type_t;

typedef struct qd_dhcp_nl_batch_op {
    qd_dhcp_batch_op_type_t type;
    union {
        qd_dhcp_iface_t   iface;
        qd_dhcp_binding_t binding;
        struct {
            int32_t ifindex;
            bool    trusted;
        } trusted;
        struct {
            uint8_t mac[ETH_ALEN];
        } del_binding;
        int32_t ifindex;
    } data;
    struct qd_dhcp_nl_batch_op *next;
} qd_dhcp_nl_batch_op_t;

/*
 * Batch context structure
 */
struct qd_dhcp_nl_batch {
    qd_dhcp_nl_t          *nl;
    qd_dhcp_nl_batch_op_t *head;
    qd_dhcp_nl_batch_op_t *tail;
    int                    count;
};

/*
 * Main netlink context structure
 */
struct qd_dhcp_nl {
    /* Netlink socket */
    int                    sock_fd;
    int                    family_id;
    uint32_t               seq;
    uint32_t               pid;

    /* Multicast group */
    int                    mcgrp_id;
    bool                   subscribed;

    /* Event loop integration */
    qd_event_loop_t       *loop;
    bool                   attached;

    /* Handler hash table */
    qd_dhcp_nl_handler_node_t *handlers[QD_DHCP_NL_HANDLER_HASH_SIZE];
    int                    num_handlers;

    /* User data */
    void                  *user_data;

    /* Receive buffer */
    uint8_t                rx_buf[QD_DHCP_NL_MSG_SIZE];

    /* Temporary parsed data for handler context */
    qd_dhcp_binding_t      tmp_binding;
    qd_dhcp_iface_t        tmp_iface;
    qd_dhcp_stats_t        tmp_stats;
};

/*
 * Internal message builder
 */
typedef struct qd_dhcp_nl_msg {
    struct nlmsghdr        nlh;
    struct genlmsghdr      genl;
    uint8_t                attrs[QD_DHCP_NL_MSG_SIZE - sizeof(struct nlmsghdr) - sizeof(struct genlmsghdr)];
    size_t                 attrs_len;
} qd_dhcp_nl_msg_t;

/*
 * Internal functions
 */

/* Message builder functions */
void qd_dhcp_nl_msg_init(qd_dhcp_nl_msg_t *msg, qd_dhcp_nl_t *nl, uint8_t cmd);
int qd_dhcp_nl_msg_add_attr(qd_dhcp_nl_msg_t *msg, uint16_t type,
                             const void *data, size_t len);
int qd_dhcp_nl_msg_add_attr_u8(qd_dhcp_nl_msg_t *msg, uint16_t type, uint8_t val);
int qd_dhcp_nl_msg_add_attr_u32(qd_dhcp_nl_msg_t *msg, uint16_t type, uint32_t val);
int qd_dhcp_nl_msg_add_attr_s32(qd_dhcp_nl_msg_t *msg, uint16_t type, int32_t val);
int qd_dhcp_nl_msg_add_attr_string(qd_dhcp_nl_msg_t *msg, uint16_t type, const char *str);

/* Send/receive functions */
int qd_dhcp_nl_send(qd_dhcp_nl_t *nl, qd_dhcp_nl_msg_t *msg);
int qd_dhcp_nl_send_recv(qd_dhcp_nl_t *nl, qd_dhcp_nl_msg_t *msg,
                          void *resp_buf, size_t resp_size);

/* Attribute parsing */
typedef struct qd_dhcp_nl_attr {
    uint16_t type;
    uint16_t len;
    void    *data;
} qd_dhcp_nl_attr_t;

int qd_dhcp_nl_parse_attrs(const void *data, size_t len,
                            qd_dhcp_nl_attr_t *attrs, int max_attrs);
void *qd_dhcp_nl_find_attr(const qd_dhcp_nl_attr_t *attrs, int num_attrs,
                            uint16_t type, size_t *len);

/* Handler dispatch */
int qd_dhcp_nl_dispatch(qd_dhcp_nl_t *nl, uint8_t cmd,
                         const qd_dhcp_nl_attr_t *attrs, int num_attrs);

/* Hash function for handler table */
static inline uint32_t qd_dhcp_nl_hash_cmd(qd_dhcp_cmd_t cmd)
{
    return (uint32_t)cmd % QD_DHCP_NL_HANDLER_HASH_SIZE;
}

/* Family ID resolution */
int qd_dhcp_nl_resolve_family(qd_dhcp_nl_t *nl);
int qd_dhcp_nl_get_mcgrp_id(qd_dhcp_nl_t *nl, const char *group_name);

#endif /* QD_DHCP_NL_INTERNAL_H */
