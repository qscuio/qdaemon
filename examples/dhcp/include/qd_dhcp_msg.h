/*
 * QDaemon DHCP Messaging API
 *
 * Transport-agnostic messaging API for communication with the DHCP kernel module.
 * This API abstracts all underlying transport details (netlink, char device, etc.)
 * and provides:
 *   - Type-safe data structures
 *   - Reliable message passing with acknowledgments and retries
 *   - Batch message processing with transactional semantics
 *   - Async notification support via callbacks
 *
 * The underlying transport can be changed without modifying user code.
 */

#ifndef QD_DHCP_MSG_H
#define QD_DHCP_MSG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <net/if.h>
#include <linux/if_ether.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward declarations
 */
typedef struct qd_dhcp_conn qd_dhcp_conn_t;
typedef struct qd_dhcp_batch qd_dhcp_batch_t;
typedef struct qd_dhcp_msg_ctx qd_dhcp_msg_ctx_t;
typedef struct qd_event_loop qd_event_loop_t;

/*
 * ==========================================================================
 * Result codes
 * ==========================================================================
 */
typedef enum {
    QD_DHCP_OK           =  0,   /* Success */
    QD_DHCP_ERR          = -1,   /* Generic error */
    QD_DHCP_ERR_NOMEM    = -2,   /* Out of memory */
    QD_DHCP_ERR_INVALID  = -3,   /* Invalid argument */
    QD_DHCP_ERR_TIMEOUT  = -4,   /* Operation timed out */
    QD_DHCP_ERR_NOT_FOUND = -5,  /* Resource not found */
    QD_DHCP_ERR_FULL     = -6,   /* Resource full */
    QD_DHCP_ERR_IO       = -7,   /* I/O error */
    QD_DHCP_ERR_PROTO    = -8,   /* Protocol error */
    QD_DHCP_ERR_BUSY     = -9,   /* Resource busy */
    QD_DHCP_ERR_NOCONN   = -10,  /* Not connected */
    QD_DHCP_ERR_RETRY    = -11,  /* Retry limit exceeded */
    QD_DHCP_ERR_ABORTED  = -12,  /* Operation aborted */
} qd_dhcp_result_t;

/*
 * ==========================================================================
 * Handler result codes (for notification callbacks)
 * ==========================================================================
 */
typedef enum {
    QD_DHCP_HANDLER_OK    = 0,   /* Handler processed successfully */
    QD_DHCP_HANDLER_ERROR = -1,  /* Handler encountered error */
    QD_DHCP_HANDLER_SKIP  = 1,   /* Skip to next handler */
    QD_DHCP_HANDLER_DONE  = 2,   /* Stop processing chain */
} qd_dhcp_handler_result_t;

/*
 * ==========================================================================
 * Command types (abstract - mapped to kernel commands internally)
 * ==========================================================================
 */
typedef enum {
    QD_DHCP_CMD_NONE = 0,
    QD_DHCP_CMD_REGISTER,
    QD_DHCP_CMD_UNREGISTER,
    QD_DHCP_CMD_ADD_INTERFACE,
    QD_DHCP_CMD_DEL_INTERFACE,
    QD_DHCP_CMD_SET_TRUSTED,
    QD_DHCP_CMD_ADD_BINDING,
    QD_DHCP_CMD_DEL_BINDING,
    QD_DHCP_CMD_CLEAR_BINDINGS,
    QD_DHCP_CMD_SET_METHOD,
    QD_DHCP_CMD_GET_STATS,
    QD_DHCP_CMD_NOTIFY_BINDING,
    QD_DHCP_CMD_NOTIFY_IFACE,
    __QD_DHCP_CMD_MAX,
} qd_dhcp_cmd_t;

/*
 * ==========================================================================
 * Event types for notifications
 * ==========================================================================
 */
typedef enum {
    QD_DHCP_EVENT_NONE = 0,
    QD_DHCP_EVENT_BINDING_ADD,
    QD_DHCP_EVENT_BINDING_DEL,
    QD_DHCP_EVENT_IFACE_ADD,
    QD_DHCP_EVENT_IFACE_DEL,
    QD_DHCP_EVENT_CONNECTED,      /* Connection established */
    QD_DHCP_EVENT_DISCONNECTED,   /* Connection lost */
} qd_dhcp_event_t;

/*
 * ==========================================================================
 * Handler flags
 * ==========================================================================
 */
#define QD_DHCP_FLAG_ASYNC     0x01  /* Handler runs asynchronously */
#define QD_DHCP_FLAG_LOGGED    0x02  /* Log handler invocations */
#define QD_DHCP_FLAG_ONESHOT   0x04  /* Remove handler after first call */

/*
 * ==========================================================================
 * Interception methods (from kernel module)
 * ==========================================================================
 */
#define QD_DHCP_METHOD_DEV_ADD_PACK  0x01
#define QD_DHCP_METHOD_TC_INGRESS    0x02
#define QD_DHCP_METHOD_BOTH          0x03

/*
 * ==========================================================================
 * User-facing data structures - Type-safe, transport-independent
 * ==========================================================================
 */

/*
 * Binding entry - MAC to IP mapping
 */
typedef struct qd_dhcp_binding {
    uint8_t  mac[ETH_ALEN];     /* MAC address */
    uint32_t ip;                /* IP address (network byte order) */
    int32_t  ifindex;           /* Interface index */
    uint32_t lease_time;        /* Lease time in seconds */
} qd_dhcp_binding_t;

/*
 * Interface configuration
 */
typedef struct qd_dhcp_iface {
    int32_t  ifindex;           /* Interface index */
    char     ifname[IFNAMSIZ];  /* Interface name */
    bool     trusted;           /* Trusted port flag */
    bool     active;            /* Active flag */
} qd_dhcp_iface_t;

/*
 * Statistics from kernel module
 */
typedef struct qd_dhcp_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t dhcp_discover;
    uint64_t dhcp_offer;
    uint64_t dhcp_request;
    uint64_t dhcp_ack;
    uint64_t dhcp_nak;
    uint64_t dhcp_release;
    uint64_t dhcp_decline;
    uint64_t dev_pack_intercepts;
    uint64_t tc_intercepts;
    uint64_t binding_count;
} qd_dhcp_stats_t;

/*
 * ==========================================================================
 * Transport types
 * ==========================================================================
 */

typedef enum {
    QD_DHCP_TRANSPORT_AUTO = 0,     /* Auto-detect best transport */
    QD_DHCP_TRANSPORT_NETLINK,      /* Generic Netlink */
    QD_DHCP_TRANSPORT_CHARDEV,      /* Character device (/dev/qdhcp_ctl) */
} qd_dhcp_transport_type_t;

/*
 * ==========================================================================
 * Connection configuration
 * ==========================================================================
 */

typedef struct qd_dhcp_conn_config {
    /* Transport selection */
    qd_dhcp_transport_type_t transport; /* Transport type (default: AUTO) */
    const char *chardev_path;           /* Char device path (NULL = default) */

    /* Reliability settings */
    uint32_t timeout_ms;        /* Message timeout (default: 5000) */
    uint32_t retry_count;       /* Number of retries (default: 3) */
    uint32_t retry_interval_ms; /* Retry interval (default: 1000) */

    /* Buffer settings */
    size_t   rx_buffer_size;    /* Receive buffer size (default: 8192) */
    size_t   tx_buffer_size;    /* Transmit buffer size (default: 8192) */

    /* Callback for connection events */
    void   (*on_connect)(qd_dhcp_conn_t *conn, void *arg);
    void   (*on_disconnect)(qd_dhcp_conn_t *conn, int reason, void *arg);
    void    *callback_arg;

    /* Reserved for future use */
    uint32_t flags;
    void    *reserved[4];
} qd_dhcp_conn_config_t;

/* Default configuration initializer */
#define QD_DHCP_CONN_CONFIG_DEFAULT { \
    .transport = QD_DHCP_TRANSPORT_AUTO, \
    .chardev_path = NULL,             \
    .timeout_ms = 5000,               \
    .retry_count = 3,                 \
    .retry_interval_ms = 1000,        \
    .rx_buffer_size = 8192,           \
    .tx_buffer_size = 8192,           \
    .on_connect = NULL,               \
    .on_disconnect = NULL,            \
    .callback_arg = NULL,             \
    .flags = 0,                       \
    .reserved = {NULL, NULL, NULL, NULL} \
}

/*
 * ==========================================================================
 * Message context - Parsed data passed to handlers
 * ==========================================================================
 */

struct qd_dhcp_msg_ctx {
    qd_dhcp_cmd_t   cmd;        /* Command that triggered this handler */
    qd_dhcp_event_t event;      /* Event type (for notifications) */
    void           *user_data;  /* User-provided context */
    int             status;     /* Response status */
    uint32_t        seq;        /* Message sequence number */

    /* Parsed data (set based on command type) */
    const qd_dhcp_binding_t *binding;  /* Binding data (if present) */
    const qd_dhcp_iface_t   *iface;    /* Interface data (if present) */
    const qd_dhcp_stats_t   *stats;    /* Statistics (if present) */
};

/*
 * ==========================================================================
 * Handler registration - Table-based callbacks for notifications
 * ==========================================================================
 */

/* Handler function type */
typedef qd_dhcp_handler_result_t (*qd_dhcp_handler_fn_t)(
    qd_dhcp_msg_ctx_t *ctx, void *arg);

/* Handler entry for table registration */
typedef struct qd_dhcp_handler_entry {
    qd_dhcp_cmd_t           cmd;        /* Command ID */
    qd_dhcp_handler_fn_t    handler;    /* Handler function */
    void                   *arg;        /* Handler argument */
    uint32_t                flags;      /* Handler flags */
    const char             *name;       /* Handler name (for debugging) */
} qd_dhcp_handler_entry_t;

/*
 * Macros for static handler table definition
 */

/* Define a handler entry */
#define QD_DHCP_HANDLER(cmd_id, handler_fn) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, \
      .flags = 0, .name = #handler_fn }

/* Define a handler entry with argument */
#define QD_DHCP_HANDLER_ARG(cmd_id, handler_fn, handler_arg) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = (handler_arg), \
      .flags = 0, .name = #handler_fn }

/* Define a handler entry with flags */
#define QD_DHCP_HANDLER_FLAGS(cmd_id, handler_fn, handler_flags) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, \
      .flags = (handler_flags), .name = #handler_fn }

/* End marker for handler table */
#define QD_DHCP_HANDLER_END() \
    { .cmd = 0, .handler = NULL, .arg = NULL, .flags = 0, .name = NULL }

/*
 * ==========================================================================
 * Connection Management API
 * ==========================================================================
 */

/*
 * Create connection to DHCP kernel module
 * Uses default configuration if config is NULL
 */
qd_dhcp_conn_t *qd_dhcp_connect(const qd_dhcp_conn_config_t *config);

/*
 * Disconnect and free resources
 */
void qd_dhcp_disconnect(qd_dhcp_conn_t *conn);

/*
 * Check if connected
 */
bool qd_dhcp_is_connected(qd_dhcp_conn_t *conn);

/*
 * Get file descriptor for event loop integration
 * Returns -1 if not applicable for current transport
 */
int qd_dhcp_get_fd(qd_dhcp_conn_t *conn);

/*
 * Set user data (passed to handlers)
 */
void qd_dhcp_set_user_data(qd_dhcp_conn_t *conn, void *user_data);

/*
 * Get user data
 */
void *qd_dhcp_get_user_data(qd_dhcp_conn_t *conn);

/*
 * Get connection statistics
 */
typedef struct qd_dhcp_conn_stats {
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t msgs_retried;
    uint64_t msgs_failed;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t acks_received;
    uint64_t naks_received;
    uint64_t timeouts;
} qd_dhcp_conn_stats_t;

void qd_dhcp_get_conn_stats(qd_dhcp_conn_t *conn, qd_dhcp_conn_stats_t *stats);

/*
 * ==========================================================================
 * Handler Registration API
 * ==========================================================================
 */

/*
 * Register handlers from static table
 * Table must be terminated with QD_DHCP_HANDLER_END()
 */
int qd_dhcp_register_handlers(qd_dhcp_conn_t *conn,
                               const qd_dhcp_handler_entry_t *handlers);

/*
 * Register a single handler programmatically
 */
int qd_dhcp_register_handler(qd_dhcp_conn_t *conn,
                              qd_dhcp_cmd_t cmd,
                              qd_dhcp_handler_fn_t handler,
                              void *arg,
                              uint32_t flags);

/*
 * Unregister handler for command
 */
int qd_dhcp_unregister_handler(qd_dhcp_conn_t *conn, qd_dhcp_cmd_t cmd);

/*
 * ==========================================================================
 * Event Loop Integration API
 * ==========================================================================
 */

/*
 * Attach to event loop for async notification processing
 */
int qd_dhcp_attach(qd_dhcp_conn_t *conn, qd_event_loop_t *loop);

/*
 * Detach from event loop
 */
int qd_dhcp_detach(qd_dhcp_conn_t *conn);

/*
 * Subscribe to kernel notifications
 */
int qd_dhcp_subscribe(qd_dhcp_conn_t *conn);

/*
 * Unsubscribe from kernel notifications
 */
int qd_dhcp_unsubscribe(qd_dhcp_conn_t *conn);

/*
 * Process pending messages (call from event loop callback or poll)
 * Returns number of messages processed, or negative on error
 */
int qd_dhcp_process(qd_dhcp_conn_t *conn);

/*
 * ==========================================================================
 * Synchronous Request API - Reliable message passing
 *
 * All operations are:
 *   - Acknowledged by the kernel
 *   - Automatically retried on timeout (configurable)
 *   - Thread-safe for concurrent use
 * ==========================================================================
 */

/*
 * Request options for fine-grained control
 */
typedef struct qd_dhcp_request_opts {
    uint32_t timeout_ms;        /* Override default timeout (0 = use default) */
    uint32_t retry_count;       /* Override retry count (0 = use default) */
    bool     no_ack;            /* Don't wait for acknowledgment */
    bool     async;             /* Non-blocking (use qd_dhcp_wait for result) */
} qd_dhcp_request_opts_t;

#define QD_DHCP_REQUEST_OPTS_DEFAULT { \
    .timeout_ms = 0,                   \
    .retry_count = 0,                  \
    .no_ack = false,                   \
    .async = false                     \
}

/*
 * Register daemon with kernel module
 */
int qd_dhcp_register(qd_dhcp_conn_t *conn);

/*
 * Unregister daemon from kernel module
 */
int qd_dhcp_unregister(qd_dhcp_conn_t *conn);

/*
 * Add interface to monitoring
 */
int qd_dhcp_add_interface(qd_dhcp_conn_t *conn, const qd_dhcp_iface_t *iface);

/*
 * Remove interface from monitoring
 */
int qd_dhcp_del_interface(qd_dhcp_conn_t *conn, int32_t ifindex);

/*
 * Set interface trusted flag
 */
int qd_dhcp_set_trusted(qd_dhcp_conn_t *conn, int32_t ifindex, bool trusted);

/*
 * Add or update binding entry
 */
int qd_dhcp_add_binding(qd_dhcp_conn_t *conn, const qd_dhcp_binding_t *binding);

/*
 * Delete binding entry by MAC address
 */
int qd_dhcp_del_binding(qd_dhcp_conn_t *conn, const uint8_t mac[ETH_ALEN]);

/*
 * Clear all bindings
 */
int qd_dhcp_clear_bindings(qd_dhcp_conn_t *conn);

/*
 * Set interception method
 */
int qd_dhcp_set_method(qd_dhcp_conn_t *conn, uint8_t method);

/*
 * Get statistics from kernel module
 */
int qd_dhcp_get_stats(qd_dhcp_conn_t *conn, qd_dhcp_stats_t *stats);

/*
 * Extended versions with options
 */
int qd_dhcp_add_interface_ex(qd_dhcp_conn_t *conn, const qd_dhcp_iface_t *iface,
                              const qd_dhcp_request_opts_t *opts);
int qd_dhcp_add_binding_ex(qd_dhcp_conn_t *conn, const qd_dhcp_binding_t *binding,
                            const qd_dhcp_request_opts_t *opts);

/*
 * ==========================================================================
 * Batch Operations API - Transactional message processing
 *
 * Batch operations allow multiple commands to be queued and executed together.
 * Features:
 *   - Atomic execution (all-or-nothing with QD_DHCP_BATCH_ATOMIC)
 *   - Ordered execution (commands processed in queue order)
 *   - Bulk acknowledgment (single ACK for entire batch)
 *   - Rollback support on failure
 * ==========================================================================
 */

/*
 * Batch flags
 */
#define QD_DHCP_BATCH_ATOMIC      0x01  /* Rollback all on any failure */
#define QD_DHCP_BATCH_ORDERED     0x02  /* Preserve queue order (default) */
#define QD_DHCP_BATCH_CONTINUE    0x04  /* Continue on error (report in results) */
#define QD_DHCP_BATCH_ASYNC       0x08  /* Non-blocking execution */

/*
 * Batch operation result
 */
typedef struct qd_dhcp_batch_result {
    int       total;            /* Total operations */
    int       succeeded;        /* Successful operations */
    int       failed;           /* Failed operations */
    int      *errors;           /* Error code for each operation (caller frees) */
    uint32_t  flags;            /* Result flags */
    bool      rolled_back;      /* True if batch was rolled back */
} qd_dhcp_batch_result_t;

/*
 * Create batch operation context
 */
qd_dhcp_batch_t *qd_dhcp_batch_create(qd_dhcp_conn_t *conn, uint32_t flags);

/*
 * Destroy batch operation context
 */
void qd_dhcp_batch_destroy(qd_dhcp_batch_t *batch);

/*
 * Queue add interface operation
 */
int qd_dhcp_batch_add_interface(qd_dhcp_batch_t *batch,
                                 const qd_dhcp_iface_t *iface);

/*
 * Queue delete interface operation
 */
int qd_dhcp_batch_del_interface(qd_dhcp_batch_t *batch, int32_t ifindex);

/*
 * Queue add binding operation
 */
int qd_dhcp_batch_add_binding(qd_dhcp_batch_t *batch,
                               const qd_dhcp_binding_t *binding);

/*
 * Queue delete binding operation
 */
int qd_dhcp_batch_del_binding(qd_dhcp_batch_t *batch,
                               const uint8_t mac[ETH_ALEN]);

/*
 * Queue set trusted operation
 */
int qd_dhcp_batch_set_trusted(qd_dhcp_batch_t *batch,
                               int32_t ifindex, bool trusted);

/*
 * Execute all queued operations
 * If result is non-NULL, caller must free result->errors
 */
int qd_dhcp_batch_execute(qd_dhcp_batch_t *batch,
                           qd_dhcp_batch_result_t *result);

/*
 * Get number of queued operations
 */
int qd_dhcp_batch_count(qd_dhcp_batch_t *batch);

/*
 * Clear queued operations without executing
 */
void qd_dhcp_batch_clear(qd_dhcp_batch_t *batch);

/*
 * Free batch result resources
 */
void qd_dhcp_batch_result_free(qd_dhcp_batch_result_t *result);

/*
 * ==========================================================================
 * Utility Functions
 * ==========================================================================
 */

/*
 * Convert error code to string
 */
const char *qd_dhcp_strerror(int error);

/*
 * Convert command to string
 */
const char *qd_dhcp_cmd_str(qd_dhcp_cmd_t cmd);

/*
 * Convert event to string
 */
const char *qd_dhcp_event_str(qd_dhcp_event_t event);

/*
 * Format MAC address to string (caller provides buffer, min 18 bytes)
 */
char *qd_dhcp_mac_str(const uint8_t mac[ETH_ALEN], char *buf, size_t len);

/*
 * Format IP address to string (caller provides buffer, min 16 bytes)
 */
char *qd_dhcp_ip_str(uint32_t ip, char *buf, size_t len);

/*
 * Parse MAC address from string
 */
int qd_dhcp_parse_mac(const char *str, uint8_t mac[ETH_ALEN]);

/*
 * Parse IP address from string
 */
int qd_dhcp_parse_ip(const char *str, uint32_t *ip);

#ifdef __cplusplus
}
#endif

#endif /* QD_DHCP_MSG_H */
