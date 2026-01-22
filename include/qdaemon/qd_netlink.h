/*
 * QDaemon - Kernel IPC API (Netlink)
 * Generic Netlink interface for kernel-userspace communication
 */

#ifndef QD_NETLINK_H
#define QD_NETLINK_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_netlink qd_netlink_t;
typedef struct qd_netlink_msg qd_netlink_msg_t;
typedef struct qd_event_loop qd_event_loop_t;

/* Netlink constants */
#define QD_NETLINK_FAMILY_NAME   "QDAEMON"
#define QD_NETLINK_VERSION       1
#define QD_NETLINK_MULTICAST_GRP "qd_events"

/* Maximum sizes */
#define QD_NETLINK_MAX_PAYLOAD   4096
#define QD_NETLINK_MAX_ATTRS     32

/* QDaemon netlink commands */
typedef enum {
    QD_NL_CMD_UNSPEC = 0,
    QD_NL_CMD_ECHO,              /* Echo request/response */
    QD_NL_CMD_REGISTER,          /* Register with kernel module */
    QD_NL_CMD_UNREGISTER,        /* Unregister */
    QD_NL_CMD_GET_INFO,          /* Get kernel module info */
    QD_NL_CMD_SET_CONFIG,        /* Set configuration */
    QD_NL_CMD_GET_CONFIG,        /* Get configuration */
    QD_NL_CMD_NOTIFY,            /* Notification from kernel */
    QD_NL_CMD_SUBSCRIBE,         /* Subscribe to events */
    QD_NL_CMD_UNSUBSCRIBE,       /* Unsubscribe from events */
    __QD_NL_CMD_MAX
} qd_nl_cmd_t;
#define QD_NL_CMD_MAX (__QD_NL_CMD_MAX - 1)

/* QDaemon netlink attributes */
typedef enum {
    QD_NL_ATTR_UNSPEC = 0,
    QD_NL_ATTR_MSG,              /* Generic message (string) */
    QD_NL_ATTR_DATA,             /* Binary data */
    QD_NL_ATTR_PID,              /* Process ID */
    QD_NL_ATTR_NAME,             /* Name (string) */
    QD_NL_ATTR_VALUE,            /* Value (u64) */
    QD_NL_ATTR_FLAGS,            /* Flags (u32) */
    QD_NL_ATTR_STATUS,           /* Status code (s32) */
    QD_NL_ATTR_TIMESTAMP,        /* Timestamp (u64) */
    QD_NL_ATTR_EVENT_TYPE,       /* Event type (u32) */
    QD_NL_ATTR_EVENT_DATA,       /* Event data (binary) */
    __QD_NL_ATTR_MAX
} qd_nl_attr_type_t;
#define QD_NL_ATTR_MAX (__QD_NL_ATTR_MAX - 1)

/* QDaemon multicast groups */
typedef enum {
    QD_NL_MCGRP_EVENTS = 0,      /* General events */
    __QD_NL_MCGRP_MAX
} qd_nl_mcgrp_t;

/* Netlink attribute structure */
typedef struct qd_nl_attr {
    uint16_t type;
    uint16_t len;
    void *data;
} qd_nl_attr_t;

/* Netlink message structure */
struct qd_netlink_msg {
    uint8_t cmd;                 /* Command */
    uint16_t flags;              /* Message flags */
    uint32_t seq;                /* Sequence number */
    uint32_t pid;                /* Port ID */
    qd_nl_attr_t attrs[QD_NETLINK_MAX_ATTRS]; /* Attributes */
    int num_attrs;
    void *payload;               /* Raw payload (for internal use) */
    size_t payload_len;
};

/* Netlink callback for received messages */
typedef void (*qd_netlink_cb_t)(qd_netlink_t *nl, qd_netlink_msg_t *msg, void *arg);

/* Netlink error callback */
typedef void (*qd_netlink_err_cb_t)(qd_netlink_t *nl, int error, void *arg);

/* Netlink configuration */
typedef struct qd_netlink_config {
    const char *family_name;     /* Netlink family name */
    size_t recv_buf_size;        /* Receive buffer size */
    size_t send_buf_size;        /* Send buffer size */
    int nonblocking;             /* Non-blocking mode */
} qd_netlink_config_t;

#define QD_NETLINK_CONFIG_DEFAULT { \
    .family_name = QD_NETLINK_FAMILY_NAME, \
    .recv_buf_size = 32768, \
    .send_buf_size = 32768, \
    .nonblocking = 1 \
}

/* Netlink handle */
struct qd_netlink {
    int sock_fd;                 /* Socket file descriptor */
    uint16_t family_id;          /* Generic netlink family ID */
    uint32_t seq;                /* Sequence number */
    uint32_t pid;                /* Port ID (process ID) */
    struct sockaddr_nl addr;     /* Local address */

    /* Configuration */
    char family_name[64];
    size_t recv_buf_size;
    size_t send_buf_size;

    /* Buffers */
    void *recv_buf;
    void *send_buf;

    /* Event loop integration */
    qd_event_loop_t *loop;
    void *event_source;

    /* Callbacks */
    qd_netlink_cb_t msg_callback;
    void *msg_callback_arg;
    qd_netlink_err_cb_t err_callback;
    void *err_callback_arg;

    /* Multicast subscriptions */
    uint32_t subscribed_groups;

    /* Statistics */
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t errors;

    pthread_mutex_t lock;
};

/*
 * Netlink API
 */

/* Create netlink connection */
qd_netlink_t *qd_netlink_create(const char *family_name);

/* Create netlink connection with configuration */
qd_netlink_t *qd_netlink_create_ex(const qd_netlink_config_t *config);

/* Destroy netlink connection */
void qd_netlink_destroy(qd_netlink_t *nl);

/* Resolve family ID (called automatically on create) */
int qd_netlink_resolve_family(qd_netlink_t *nl);

/* Attach to event loop */
int qd_netlink_attach(qd_netlink_t *nl, qd_event_loop_t *loop);

/* Detach from event loop */
void qd_netlink_detach(qd_netlink_t *nl);

/* Set message callback */
void qd_netlink_set_callback(qd_netlink_t *nl, qd_netlink_cb_t callback, void *arg);

/* Set error callback */
void qd_netlink_set_error_callback(qd_netlink_t *nl, qd_netlink_err_cb_t callback, void *arg);

/*
 * Message Sending
 */

/* Send raw message */
int qd_netlink_send(qd_netlink_t *nl, uint8_t cmd, void *data, size_t len);

/* Send message with attributes */
int qd_netlink_send_msg(qd_netlink_t *nl, qd_netlink_msg_t *msg);

/* Send and wait for response (blocking) */
int qd_netlink_request(qd_netlink_t *nl, qd_netlink_msg_t *request,
                        qd_netlink_msg_t *response, int timeout_ms);

/* Receive message (blocking) */
int qd_netlink_recv(qd_netlink_t *nl, qd_netlink_msg_t *msg, int timeout_ms);

/* Process pending messages (non-blocking) */
int qd_netlink_process(qd_netlink_t *nl);

/*
 * Multicast Groups
 */

/* Subscribe to multicast group */
int qd_netlink_subscribe(qd_netlink_t *nl, uint32_t group);

/* Unsubscribe from multicast group */
int qd_netlink_unsubscribe(qd_netlink_t *nl, uint32_t group);

/*
 * Message Building
 */

/* Initialize message */
void qd_netlink_msg_init(qd_netlink_msg_t *msg, uint8_t cmd);

/* Clear message */
void qd_netlink_msg_clear(qd_netlink_msg_t *msg);

/* Add attribute */
int qd_netlink_msg_add_attr(qd_netlink_msg_t *msg, uint16_t type,
                             const void *data, uint16_t len);

/* Add string attribute */
int qd_netlink_msg_add_str(qd_netlink_msg_t *msg, uint16_t type, const char *str);

/* Add u32 attribute */
int qd_netlink_msg_add_u32(qd_netlink_msg_t *msg, uint16_t type, uint32_t val);

/* Add u64 attribute */
int qd_netlink_msg_add_u64(qd_netlink_msg_t *msg, uint16_t type, uint64_t val);

/* Add s32 attribute */
int qd_netlink_msg_add_s32(qd_netlink_msg_t *msg, uint16_t type, int32_t val);

/*
 * Message Parsing
 */

/* Get attribute by type */
qd_nl_attr_t *qd_netlink_msg_get_attr(qd_netlink_msg_t *msg, uint16_t type);

/* Get string attribute */
const char *qd_netlink_msg_get_str(qd_netlink_msg_t *msg, uint16_t type);

/* Get u32 attribute */
int qd_netlink_msg_get_u32(qd_netlink_msg_t *msg, uint16_t type, uint32_t *val);

/* Get u64 attribute */
int qd_netlink_msg_get_u64(qd_netlink_msg_t *msg, uint16_t type, uint64_t *val);

/* Get s32 attribute */
int qd_netlink_msg_get_s32(qd_netlink_msg_t *msg, uint16_t type, int32_t *val);

/* Get binary data attribute */
void *qd_netlink_msg_get_data(qd_netlink_msg_t *msg, uint16_t type, size_t *len);

/*
 * Convenience Functions
 */

/* Echo request (for testing) */
int qd_netlink_echo(qd_netlink_t *nl, const char *message, char *response, size_t len);

/* Get kernel module info */
int qd_netlink_get_info(qd_netlink_t *nl, char *info, size_t len);

/* Register with kernel module */
int qd_netlink_register(qd_netlink_t *nl, const char *name);

/* Unregister from kernel module */
int qd_netlink_unregister(qd_netlink_t *nl);

/*
 * Statistics
 */

typedef struct qd_netlink_stats {
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t errors;
    uint32_t subscribed_groups;
} qd_netlink_stats_t;

void qd_netlink_stats(qd_netlink_t *nl, qd_netlink_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* QD_NETLINK_H */
