/*
 * QDaemon DHCP Netlink Wrapper API
 *
 * Clean, user-friendly wrapper for Generic Netlink communication with
 * the DHCP kernel module. Hides all netlink internals from users.
 *
 * Features:
 * - Type-safe data structures
 * - Table-based handler registration (following qd_handler patterns)
 * - Batch message processing for SET and GET operations
 * - Async notifications from kernel via multicast groups
 */

#ifndef QD_DHCP_NL_H
#define QD_DHCP_NL_H

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
typedef struct qd_dhcp_nl qd_dhcp_nl_t;
typedef struct qd_dhcp_nl_batch qd_dhcp_nl_batch_t;
typedef struct qd_dhcp_nl_ctx qd_dhcp_nl_ctx_t;
typedef struct qd_event_loop qd_event_loop_t;

/*
 * Result codes
 */
#define QD_DHCP_NL_OK           0
#define QD_DHCP_NL_ERROR       -1
#define QD_DHCP_NL_NOMEM       -2
#define QD_DHCP_NL_INVALID     -3
#define QD_DHCP_NL_TIMEOUT     -4
#define QD_DHCP_NL_NOT_FOUND   -5
#define QD_DHCP_NL_FULL        -6

/*
 * Handler result codes
 */
typedef enum {
    QD_DHCP_NL_HANDLER_OK = 0,
    QD_DHCP_NL_HANDLER_ERROR = -1,
    QD_DHCP_NL_HANDLER_SKIP = 1,
    QD_DHCP_NL_HANDLER_DONE = 2,
} qd_dhcp_nl_handler_result_t;

/*
 * Command types (from kernel module)
 */
typedef enum {
    QD_DHCP_CMD_UNSPEC = 0,
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
 * Event types for notifications
 */
typedef enum {
    QD_DHCP_EVENT_NONE = 0,
    QD_DHCP_EVENT_BINDING_ADD,
    QD_DHCP_EVENT_BINDING_DEL,
    QD_DHCP_EVENT_IFACE_ADD,
    QD_DHCP_EVENT_IFACE_DEL,
} qd_dhcp_event_t;

/*
 * Handler flags
 */
#define QD_DHCP_NL_FLAG_ASYNC     0x01  /* Handler runs asynchronously */
#define QD_DHCP_NL_FLAG_LOGGED    0x02  /* Log handler invocations */

/*
 * Interception methods (from kernel module)
 */
#define QD_DHCP_METHOD_DEV_ADD_PACK  0x01
#define QD_DHCP_METHOD_TC_INGRESS    0x02
#define QD_DHCP_METHOD_BOTH          0x03

/*
 * ==========================================================================
 * User-facing data structures - Type-safe, no netlink internals exposed
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
 * Handler context - Parsed attributes passed to handlers
 * ==========================================================================
 */

struct qd_dhcp_nl_ctx {
    qd_dhcp_cmd_t   cmd;        /* Command that triggered this handler */
    qd_dhcp_event_t event;      /* Event type (for notifications) */
    void           *user_data;  /* User-provided context */
    int             status;     /* Response status */

    /* Parsed data (set based on command type) */
    const qd_dhcp_binding_t *binding;  /* Binding data (if present) */
    const qd_dhcp_iface_t   *iface;    /* Interface data (if present) */
    const qd_dhcp_stats_t   *stats;    /* Statistics (if present) */
};

/*
 * ==========================================================================
 * Handler registration - Table-based, following qd_handler patterns
 * ==========================================================================
 */

/* Handler function type */
typedef qd_dhcp_nl_handler_result_t (*qd_dhcp_nl_handler_fn_t)(
    qd_dhcp_nl_ctx_t *ctx, void *arg);

/* Handler entry for table registration */
typedef struct qd_dhcp_nl_handler_entry {
    qd_dhcp_cmd_t           cmd;        /* Command ID */
    qd_dhcp_nl_handler_fn_t handler;    /* Handler function */
    void                   *arg;        /* Handler argument */
    uint32_t                flags;      /* Handler flags */
    const char             *name;       /* Handler name (for debugging) */
} qd_dhcp_nl_handler_entry_t;

/*
 * Macros for static handler table definition
 */

/* Define a handler entry */
#define QD_DHCP_NL_HANDLER(cmd_id, handler_fn) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, \
      .flags = 0, .name = #handler_fn }

/* Define a handler entry with argument */
#define QD_DHCP_NL_HANDLER_ARG(cmd_id, handler_fn, handler_arg) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = (handler_arg), \
      .flags = 0, .name = #handler_fn }

/* Define a handler entry with flags */
#define QD_DHCP_NL_HANDLER_FLAGS(cmd_id, handler_fn, handler_flags) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, \
      .flags = (handler_flags), .name = #handler_fn }

/* End marker for handler table */
#define QD_DHCP_NL_HANDLER_END() \
    { .cmd = 0, .handler = NULL, .arg = NULL, .flags = 0, .name = NULL }

/*
 * ==========================================================================
 * Connection Management API
 * ==========================================================================
 */

/*
 * Create netlink connection to DHCP kernel module
 * Returns NULL on failure
 */
qd_dhcp_nl_t *qd_dhcp_nl_create(void);

/*
 * Destroy netlink connection
 */
void qd_dhcp_nl_destroy(qd_dhcp_nl_t *nl);

/*
 * Get file descriptor for event loop integration
 * Returns -1 if not connected
 */
int qd_dhcp_nl_get_fd(qd_dhcp_nl_t *nl);

/*
 * Check if connected to kernel module
 */
bool qd_dhcp_nl_is_connected(qd_dhcp_nl_t *nl);

/*
 * Set user data (passed to handlers)
 */
void qd_dhcp_nl_set_user_data(qd_dhcp_nl_t *nl, void *user_data);

/*
 * Get user data
 */
void *qd_dhcp_nl_get_user_data(qd_dhcp_nl_t *nl);

/*
 * ==========================================================================
 * Handler Registration API
 * ==========================================================================
 */

/*
 * Register handlers from static table
 * Table must be terminated with QD_DHCP_NL_HANDLER_END()
 */
int qd_dhcp_nl_register_handlers(qd_dhcp_nl_t *nl,
                                  const qd_dhcp_nl_handler_entry_t *handlers);

/*
 * Register a single handler programmatically
 */
int qd_dhcp_nl_register_handler(qd_dhcp_nl_t *nl,
                                 qd_dhcp_cmd_t cmd,
                                 qd_dhcp_nl_handler_fn_t handler,
                                 void *arg,
                                 uint32_t flags);

/*
 * Unregister handler for command
 */
int qd_dhcp_nl_unregister_handler(qd_dhcp_nl_t *nl, qd_dhcp_cmd_t cmd);

/*
 * ==========================================================================
 * Event Loop Integration API
 * ==========================================================================
 */

/*
 * Attach to event loop for async notification processing
 */
int qd_dhcp_nl_attach(qd_dhcp_nl_t *nl, qd_event_loop_t *loop);

/*
 * Detach from event loop
 */
int qd_dhcp_nl_detach(qd_dhcp_nl_t *nl);

/*
 * Subscribe to kernel notifications (join multicast group)
 */
int qd_dhcp_nl_subscribe(qd_dhcp_nl_t *nl);

/*
 * Unsubscribe from kernel notifications
 */
int qd_dhcp_nl_unsubscribe(qd_dhcp_nl_t *nl);

/*
 * Process pending notifications (call from event loop callback)
 * Returns number of messages processed, or negative on error
 */
int qd_dhcp_nl_process(qd_dhcp_nl_t *nl);

/*
 * ==========================================================================
 * Synchronous Request API - Type-safe, no netlink internals exposed
 * ==========================================================================
 */

/*
 * Register daemon with kernel module
 */
int qd_dhcp_nl_register(qd_dhcp_nl_t *nl);

/*
 * Unregister daemon from kernel module
 */
int qd_dhcp_nl_unregister(qd_dhcp_nl_t *nl);

/*
 * Add interface to monitoring
 */
int qd_dhcp_nl_add_interface(qd_dhcp_nl_t *nl, const qd_dhcp_iface_t *iface);

/*
 * Remove interface from monitoring
 */
int qd_dhcp_nl_del_interface(qd_dhcp_nl_t *nl, int32_t ifindex);

/*
 * Set interface trusted flag
 */
int qd_dhcp_nl_set_trusted(qd_dhcp_nl_t *nl, int32_t ifindex, bool trusted);

/*
 * Add or update binding entry
 */
int qd_dhcp_nl_add_binding(qd_dhcp_nl_t *nl, const qd_dhcp_binding_t *binding);

/*
 * Delete binding entry by MAC address
 */
int qd_dhcp_nl_del_binding(qd_dhcp_nl_t *nl, const uint8_t mac[ETH_ALEN]);

/*
 * Clear all bindings
 */
int qd_dhcp_nl_clear_bindings(qd_dhcp_nl_t *nl);

/*
 * Set interception method
 */
int qd_dhcp_nl_set_method(qd_dhcp_nl_t *nl, uint8_t method);

/*
 * Get statistics from kernel module
 */
int qd_dhcp_nl_get_stats(qd_dhcp_nl_t *nl, qd_dhcp_stats_t *stats);

/*
 * ==========================================================================
 * Batch Operations API
 * ==========================================================================
 */

/*
 * Batch operation result
 */
typedef struct qd_dhcp_nl_batch_result {
    int total;      /* Total operations */
    int succeeded;  /* Successful operations */
    int failed;     /* Failed operations */
    int *errors;    /* Error codes for each operation (caller must free) */
} qd_dhcp_nl_batch_result_t;

/*
 * Create batch operation context
 */
qd_dhcp_nl_batch_t *qd_dhcp_nl_batch_create(qd_dhcp_nl_t *nl);

/*
 * Destroy batch operation context
 */
void qd_dhcp_nl_batch_destroy(qd_dhcp_nl_batch_t *batch);

/*
 * Queue add interface operation
 */
int qd_dhcp_nl_batch_add_interface(qd_dhcp_nl_batch_t *batch,
                                    const qd_dhcp_iface_t *iface);

/*
 * Queue delete interface operation
 */
int qd_dhcp_nl_batch_del_interface(qd_dhcp_nl_batch_t *batch,
                                    int32_t ifindex);

/*
 * Queue add binding operation
 */
int qd_dhcp_nl_batch_add_binding(qd_dhcp_nl_batch_t *batch,
                                  const qd_dhcp_binding_t *binding);

/*
 * Queue delete binding operation
 */
int qd_dhcp_nl_batch_del_binding(qd_dhcp_nl_batch_t *batch,
                                  const uint8_t mac[ETH_ALEN]);

/*
 * Queue set trusted operation
 */
int qd_dhcp_nl_batch_set_trusted(qd_dhcp_nl_batch_t *batch,
                                  int32_t ifindex, bool trusted);

/*
 * Execute all queued operations
 * If result is non-NULL, caller must free result->errors
 */
int qd_dhcp_nl_batch_execute(qd_dhcp_nl_batch_t *batch,
                              qd_dhcp_nl_batch_result_t *result);

/*
 * Get number of queued operations
 */
int qd_dhcp_nl_batch_count(qd_dhcp_nl_batch_t *batch);

/*
 * Clear queued operations without executing
 */
void qd_dhcp_nl_batch_clear(qd_dhcp_nl_batch_t *batch);

/*
 * ==========================================================================
 * Utility Functions
 * ==========================================================================
 */

/*
 * Convert error code to string
 */
const char *qd_dhcp_nl_strerror(int error);

/*
 * Convert command to string
 */
const char *qd_dhcp_nl_cmd_str(qd_dhcp_cmd_t cmd);

/*
 * Convert event to string
 */
const char *qd_dhcp_nl_event_str(qd_dhcp_event_t event);

/*
 * Format MAC address to string (caller provides buffer, min 18 bytes)
 */
char *qd_dhcp_nl_mac_str(const uint8_t mac[ETH_ALEN], char *buf, size_t len);

/*
 * Format IP address to string (caller provides buffer, min 16 bytes)
 */
char *qd_dhcp_nl_ip_str(uint32_t ip, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* QD_DHCP_NL_H */
