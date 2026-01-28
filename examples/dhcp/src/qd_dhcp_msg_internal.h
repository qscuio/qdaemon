/*
 * QDaemon DHCP Messaging - Internal Header
 *
 * Internal structures and transport abstraction layer.
 * This file should only be included by implementation files.
 */

#ifndef QD_DHCP_MSG_INTERNAL_H
#define QD_DHCP_MSG_INTERNAL_H

#include "qd_dhcp_msg.h"
#include <pthread.h>

/*
 * ==========================================================================
 * Constants
 * ==========================================================================
 */
#define QD_DHCP_HANDLER_HASH_SIZE   32
#define QD_DHCP_MSG_MAX_SIZE        4096
#define QD_DHCP_BATCH_MAX_OPS       256
#define QD_DHCP_PENDING_MAX         64
#define QD_DHCP_SEQ_WINDOW          256

/*
 * ==========================================================================
 * Message Header - Transport-independent wire format
 * ==========================================================================
 */

#define QD_DHCP_MSG_MAGIC       0x51444843  /* "QDHC" */
#define QD_DHCP_MSG_VERSION     1

/* Message flags */
#define QD_DHCP_MSG_FLAG_ACK        0x01  /* Request acknowledgment */
#define QD_DHCP_MSG_FLAG_RESPONSE   0x02  /* This is a response */
#define QD_DHCP_MSG_FLAG_ERROR      0x04  /* Error response */
#define QD_DHCP_MSG_FLAG_BATCH      0x08  /* Part of a batch */
#define QD_DHCP_MSG_FLAG_BATCH_END  0x10  /* Last message in batch */
#define QD_DHCP_MSG_FLAG_NOTIFY     0x20  /* Notification (no ACK expected) */

typedef struct qd_dhcp_msg_hdr {
    uint32_t magic;             /* Magic number for validation */
    uint8_t  version;           /* Protocol version */
    uint8_t  cmd;               /* Command ID */
    uint8_t  flags;             /* Message flags */
    uint8_t  reserved;          /* Reserved for future use */
    uint32_t seq;               /* Sequence number */
    uint32_t len;               /* Payload length */
    int32_t  status;            /* Status code (for responses) */
} __attribute__((packed)) qd_dhcp_msg_hdr_t;

#define QD_DHCP_MSG_HDR_SIZE    sizeof(qd_dhcp_msg_hdr_t)

/*
 * ==========================================================================
 * Attribute TLV format - Transport-independent
 * ==========================================================================
 */

typedef struct qd_dhcp_attr_hdr {
    uint16_t type;              /* Attribute type */
    uint16_t len;               /* Attribute length (including header) */
} __attribute__((packed)) qd_dhcp_attr_hdr_t;

#define QD_DHCP_ATTR_HDR_SIZE   sizeof(qd_dhcp_attr_hdr_t)
#define QD_DHCP_ATTR_ALIGN(len) (((len) + 3) & ~3)

/* Attribute types */
enum {
    QD_DHCP_ATTR_NONE = 0,
    QD_DHCP_ATTR_IFINDEX,
    QD_DHCP_ATTR_IFNAME,
    QD_DHCP_ATTR_MAC,
    QD_DHCP_ATTR_IP,
    QD_DHCP_ATTR_TRUSTED,
    QD_DHCP_ATTR_LEASE_TIME,
    QD_DHCP_ATTR_METHOD,
    QD_DHCP_ATTR_STATS,
    QD_DHCP_ATTR_EVENT_TYPE,
    QD_DHCP_ATTR_BATCH_ID,
    QD_DHCP_ATTR_BATCH_INDEX,
    QD_DHCP_ATTR_BATCH_TOTAL,
    __QD_DHCP_ATTR_MAX,
};

/*
 * ==========================================================================
 * Parsed attribute structure
 * ==========================================================================
 */

typedef struct qd_dhcp_attr {
    uint16_t type;
    uint16_t len;
    void    *data;
} qd_dhcp_attr_t;

/*
 * ==========================================================================
 * Transport Operations - Abstract interface for different transports
 * ==========================================================================
 */

typedef struct qd_dhcp_transport qd_dhcp_transport_t;

typedef struct qd_dhcp_transport_ops {
    /* Transport name for debugging */
    const char *name;

    /* Open transport connection */
    int (*open)(qd_dhcp_transport_t *transport);

    /* Close transport connection */
    void (*close)(qd_dhcp_transport_t *transport);

    /* Get file descriptor for polling (-1 if not applicable) */
    int (*get_fd)(qd_dhcp_transport_t *transport);

    /* Send message (blocking) */
    int (*send)(qd_dhcp_transport_t *transport,
                const void *data, size_t len);

    /* Receive message (blocking or non-blocking based on flags) */
    int (*recv)(qd_dhcp_transport_t *transport,
                void *buf, size_t buf_size, int flags);

    /* Subscribe to notifications/multicast */
    int (*subscribe)(qd_dhcp_transport_t *transport);

    /* Unsubscribe from notifications */
    int (*unsubscribe)(qd_dhcp_transport_t *transport);

    /* Check if connected */
    bool (*is_connected)(qd_dhcp_transport_t *transport);

    /* Get transport-specific error string */
    const char *(*strerror)(qd_dhcp_transport_t *transport, int error);

} qd_dhcp_transport_ops_t;

/*
 * Transport instance
 */
struct qd_dhcp_transport {
    const qd_dhcp_transport_ops_t *ops;
    void                          *priv;        /* Transport-specific data */
    int                            last_error;
};

/* Receive flags */
#define QD_DHCP_RECV_NONBLOCK   0x01
#define QD_DHCP_RECV_PEEK       0x02

/*
 * ==========================================================================
 * Pending message tracking (for reliable delivery)
 * ==========================================================================
 */

typedef struct qd_dhcp_pending_msg {
    uint32_t                seq;            /* Sequence number */
    uint64_t                send_time_ms;   /* Time message was sent */
    uint64_t                timeout_ms;     /* Timeout for this message */
    int                     retry_count;    /* Retries remaining */
    int                     retry_interval; /* Retry interval in ms */
    uint8_t                *msg_data;       /* Message data for retransmit */
    size_t                  msg_len;        /* Message length */
    qd_dhcp_cmd_t           cmd;            /* Command type */
    bool                    in_use;         /* Slot is in use */

    /* Completion callback (for async) */
    void                  (*callback)(int result, void *arg);
    void                   *callback_arg;

    struct qd_dhcp_pending_msg *next;
} qd_dhcp_pending_msg_t;

/*
 * ==========================================================================
 * Handler node in hash table
 * ==========================================================================
 */

typedef struct qd_dhcp_handler_node {
    qd_dhcp_cmd_t               cmd;
    qd_dhcp_handler_fn_t        handler;
    void                       *arg;
    uint32_t                    flags;
    const char                 *name;
    struct qd_dhcp_handler_node *next;

    /* Statistics */
    uint64_t call_count;
    uint64_t error_count;
} qd_dhcp_handler_node_t;

/*
 * ==========================================================================
 * Batch operation entry
 * ==========================================================================
 */

typedef enum {
    QD_DHCP_BATCH_OP_ADD_IFACE,
    QD_DHCP_BATCH_OP_DEL_IFACE,
    QD_DHCP_BATCH_OP_SET_TRUSTED,
    QD_DHCP_BATCH_OP_ADD_BINDING,
    QD_DHCP_BATCH_OP_DEL_BINDING,
} qd_dhcp_batch_op_type_t;

typedef struct qd_dhcp_batch_op {
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
    struct qd_dhcp_batch_op *next;
} qd_dhcp_batch_op_t;

/*
 * ==========================================================================
 * Batch context structure
 * ==========================================================================
 */

struct qd_dhcp_batch {
    qd_dhcp_conn_t     *conn;
    qd_dhcp_batch_op_t *head;
    qd_dhcp_batch_op_t *tail;
    int                 count;
    uint32_t            flags;
    uint32_t            batch_id;
};

/*
 * ==========================================================================
 * Main connection structure
 * ==========================================================================
 */

struct qd_dhcp_conn {
    /* Transport layer */
    qd_dhcp_transport_t        *transport;

    /* Configuration */
    qd_dhcp_conn_config_t       config;

    /* Sequence number management */
    uint32_t                    seq_next;
    pthread_mutex_t             seq_lock;

    /* Pending messages for reliable delivery */
    qd_dhcp_pending_msg_t       pending[QD_DHCP_PENDING_MAX];
    int                         pending_count;
    pthread_mutex_t             pending_lock;

    /* Handler hash table */
    qd_dhcp_handler_node_t     *handlers[QD_DHCP_HANDLER_HASH_SIZE];
    int                         num_handlers;
    pthread_mutex_t             handler_lock;

    /* Event loop integration */
    qd_event_loop_t            *loop;
    bool                        attached;
    bool                        subscribed;

    /* User data */
    void                       *user_data;

    /* Receive buffer */
    uint8_t                    *rx_buf;
    size_t                      rx_buf_size;

    /* Transmit buffer */
    uint8_t                    *tx_buf;
    size_t                      tx_buf_size;

    /* Connection statistics */
    qd_dhcp_conn_stats_t        stats;
    pthread_mutex_t             stats_lock;

    /* Temporary parsed data for handler context */
    qd_dhcp_binding_t           tmp_binding;
    qd_dhcp_iface_t             tmp_iface;
    qd_dhcp_stats_t             tmp_stats;

    /* Batch ID counter */
    uint32_t                    batch_id_next;
};

/*
 * ==========================================================================
 * Message builder
 * ==========================================================================
 */

typedef struct qd_dhcp_msg_builder {
    uint8_t                *buf;
    size_t                  buf_size;
    size_t                  len;
    qd_dhcp_msg_hdr_t      *hdr;
} qd_dhcp_msg_builder_t;

/*
 * ==========================================================================
 * Internal functions
 * ==========================================================================
 */

/* Transport creation (factory functions) */
qd_dhcp_transport_t *qd_dhcp_transport_netlink_create(void);
qd_dhcp_transport_t *qd_dhcp_transport_chardev_create(const char *path);
qd_dhcp_transport_t *qd_dhcp_transport_bde_create(void);
void qd_dhcp_transport_destroy(qd_dhcp_transport_t *transport);

/* Message builder functions */
void qd_dhcp_msg_builder_init(qd_dhcp_msg_builder_t *builder,
                               uint8_t *buf, size_t buf_size);
void qd_dhcp_msg_builder_reset(qd_dhcp_msg_builder_t *builder,
                                qd_dhcp_cmd_t cmd, uint32_t seq, uint8_t flags);
int qd_dhcp_msg_builder_add_attr(qd_dhcp_msg_builder_t *builder,
                                  uint16_t type, const void *data, size_t len);
int qd_dhcp_msg_builder_add_u8(qd_dhcp_msg_builder_t *builder,
                                uint16_t type, uint8_t val);
int qd_dhcp_msg_builder_add_u32(qd_dhcp_msg_builder_t *builder,
                                 uint16_t type, uint32_t val);
int qd_dhcp_msg_builder_add_s32(qd_dhcp_msg_builder_t *builder,
                                 uint16_t type, int32_t val);
int qd_dhcp_msg_builder_add_string(qd_dhcp_msg_builder_t *builder,
                                    uint16_t type, const char *str);
size_t qd_dhcp_msg_builder_finish(qd_dhcp_msg_builder_t *builder);

/* Message parsing */
int qd_dhcp_msg_validate(const void *data, size_t len);
int qd_dhcp_msg_parse_attrs(const void *data, size_t len,
                             qd_dhcp_attr_t *attrs, int max_attrs);
void *qd_dhcp_msg_find_attr(const qd_dhcp_attr_t *attrs, int num_attrs,
                             uint16_t type, size_t *len);

/* Reliable messaging */
int qd_dhcp_send_reliable(qd_dhcp_conn_t *conn,
                           const void *data, size_t len,
                           const qd_dhcp_request_opts_t *opts);
int qd_dhcp_wait_ack(qd_dhcp_conn_t *conn, uint32_t seq,
                      uint32_t timeout_ms);
void qd_dhcp_process_ack(qd_dhcp_conn_t *conn, uint32_t seq, int status);
void qd_dhcp_check_timeouts(qd_dhcp_conn_t *conn);

/* Handler dispatch */
int qd_dhcp_dispatch(qd_dhcp_conn_t *conn, const qd_dhcp_msg_hdr_t *hdr,
                      const qd_dhcp_attr_t *attrs, int num_attrs);

/* Hash function for handler table */
static inline uint32_t qd_dhcp_hash_cmd(qd_dhcp_cmd_t cmd)
{
    return (uint32_t)cmd % QD_DHCP_HANDLER_HASH_SIZE;
}

/* Time utilities */
uint64_t qd_dhcp_time_ms(void);

#endif /* QD_DHCP_MSG_INTERNAL_H */
