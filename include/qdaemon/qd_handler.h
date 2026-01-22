/*
 * QDaemon - Table-Based Message Handler API
 * Declarative message handler registration and dispatch
 */

#ifndef QD_HANDLER_H
#define QD_HANDLER_H

#include <stddef.h>
#include <stdint.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_handler_table qd_handler_table_t;
typedef struct qd_handler_ctx qd_handler_ctx_t;

/* Maximum handlers per table */
#define QD_HANDLER_MAX_ENTRIES    256
#define QD_HANDLER_HASH_SIZE      64

/* Handler flags */
#define QD_HANDLER_FLAG_ASYNC     0x01  /* Handler runs asynchronously */
#define QD_HANDLER_FLAG_LOGGED    0x02  /* Log handler invocations */
#define QD_HANDLER_FLAG_METRIC    0x04  /* Collect metrics */
#define QD_HANDLER_FLAG_REQUIRED  0x08  /* Must be present */

/* Handler result codes */
typedef enum {
    QD_HANDLER_OK = 0,
    QD_HANDLER_ERROR = -1,
    QD_HANDLER_NOT_FOUND = -2,
    QD_HANDLER_INVALID = -3,
    QD_HANDLER_SKIP = 1,        /* Skip to next handler */
    QD_HANDLER_DONE = 2,        /* Stop processing */
} qd_handler_result_t;

/*
 * Handler context - passed to every handler
 */
struct qd_handler_ctx {
    uint32_t cmd;               /* Command/message ID */
    uint32_t flags;             /* Message flags */
    void *data;                 /* Message data */
    size_t len;                 /* Data length */
    void *user_data;            /* User-provided context */
    void *conn;                 /* Connection (if applicable) */
    
    /* Response building */
    void *response;             /* Response buffer */
    size_t response_len;        /* Response length */
    size_t response_cap;        /* Response capacity */
    int status;                 /* Response status code */
    
    /* Metadata */
    uint64_t timestamp;         /* Message timestamp */
    uint32_t seq;               /* Sequence number */
};

/* Handler function type */
typedef qd_handler_result_t (*qd_handler_fn_t)(qd_handler_ctx_t *ctx, void *arg);

/* Pre/post hook function types */
typedef int (*qd_pre_handler_fn_t)(qd_handler_ctx_t *ctx, void *arg);
typedef void (*qd_post_handler_fn_t)(qd_handler_ctx_t *ctx, qd_handler_result_t result, void *arg);

/* Error handler function type */
typedef void (*qd_error_handler_fn_t)(qd_handler_ctx_t *ctx, int error, void *arg);

/*
 * Handler entry - used for static table definition
 */
typedef struct qd_handler_entry {
    uint32_t cmd;               /* Command ID */
    qd_handler_fn_t handler;    /* Handler function */
    void *arg;                  /* Handler argument */
    uint32_t flags;             /* Handler flags */
    const char *name;           /* Handler name (for debugging) */
} qd_handler_entry_t;

/*
 * Macros for static handler table definition
 */

/* Define a handler entry */
#define QD_HANDLER(cmd_id, handler_fn) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, .flags = 0, .name = #handler_fn }

/* Define a handler entry with argument */
#define QD_HANDLER_ARG(cmd_id, handler_fn, handler_arg) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = (handler_arg), .flags = 0, .name = #handler_fn }

/* Define a handler entry with flags */
#define QD_HANDLER_FLAGS(cmd_id, handler_fn, handler_flags) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = NULL, .flags = (handler_flags), .name = #handler_fn }

/* Define a full handler entry */
#define QD_HANDLER_FULL(cmd_id, handler_fn, handler_arg, handler_flags, handler_name) \
    { .cmd = (cmd_id), .handler = (handler_fn), .arg = (handler_arg), .flags = (handler_flags), .name = (handler_name) }

/* End marker for handler table */
#define QD_HANDLER_END() \
    { .cmd = 0, .handler = NULL, .arg = NULL, .flags = 0, .name = NULL }

/*
 * Handler table structure (internal)
 */
typedef struct qd_handler_node {
    uint32_t cmd;
    qd_handler_fn_t handler;
    void *arg;
    uint32_t flags;
    const char *name;
    struct qd_handler_node *next;
    
    /* Statistics */
    uint64_t call_count;
    uint64_t error_count;
    uint64_t total_time_ns;
} qd_handler_node_t;

struct qd_handler_table {
    char name[32];
    
    /* Hash table for O(1) lookup */
    qd_handler_node_t *buckets[QD_HANDLER_HASH_SIZE];
    int num_handlers;
    
    /* Default handler for unknown commands */
    qd_handler_fn_t default_handler;
    void *default_handler_arg;
    
    /* Pre/post hooks */
    qd_pre_handler_fn_t pre_hook;
    void *pre_hook_arg;
    qd_post_handler_fn_t post_hook;
    void *post_hook_arg;
    
    /* Error handler */
    qd_error_handler_fn_t error_handler;
    void *error_handler_arg;
    
    /* User data */
    void *user_data;
    
    /* Statistics */
    uint64_t total_calls;
    uint64_t total_errors;
    uint64_t unknown_commands;
};

/*
 * Handler Table API
 */

/* Create handler table */
qd_handler_table_t *qd_handler_table_create(const char *name);

/* Destroy handler table */
void qd_handler_table_destroy(qd_handler_table_t *table);

/* Register handler from static entry */
int qd_handler_register(qd_handler_table_t *table, const qd_handler_entry_t *entry);

/* Register handler programmatically */
int qd_handler_register_fn(qd_handler_table_t *table, uint32_t cmd,
                           qd_handler_fn_t handler, void *arg, uint32_t flags);

/* Register multiple handlers from static table */
int qd_handler_register_table(qd_handler_table_t *table, const qd_handler_entry_t *entries);

/* Unregister handler */
int qd_handler_unregister(qd_handler_table_t *table, uint32_t cmd);

/* Set default handler for unknown commands */
void qd_handler_set_default(qd_handler_table_t *table, qd_handler_fn_t handler, void *arg);

/* Set pre-dispatch hook */
void qd_handler_set_pre_hook(qd_handler_table_t *table, qd_pre_handler_fn_t hook, void *arg);

/* Set post-dispatch hook */
void qd_handler_set_post_hook(qd_handler_table_t *table, qd_post_handler_fn_t hook, void *arg);

/* Set error handler */
void qd_handler_set_error_handler(qd_handler_table_t *table, qd_error_handler_fn_t handler, void *arg);

/* Set user data */
void qd_handler_set_user_data(qd_handler_table_t *table, void *user_data);

/* Get user data */
void *qd_handler_get_user_data(qd_handler_table_t *table);

/*
 * Handler Dispatch
 */

/* Dispatch message to appropriate handler */
qd_handler_result_t qd_handler_dispatch(qd_handler_table_t *table, qd_handler_ctx_t *ctx);

/* Dispatch with command ID override */
qd_handler_result_t qd_handler_dispatch_cmd(qd_handler_table_t *table, uint32_t cmd, qd_handler_ctx_t *ctx);

/* Check if handler exists for command */
int qd_handler_exists(qd_handler_table_t *table, uint32_t cmd);

/* Get handler info */
const qd_handler_entry_t *qd_handler_get(qd_handler_table_t *table, uint32_t cmd);

/*
 * Handler Context Helpers
 */

/* Initialize context */
void qd_handler_ctx_init(qd_handler_ctx_t *ctx);

/* Set response data */
int qd_handler_ctx_set_response(qd_handler_ctx_t *ctx, const void *data, size_t len);

/* Append to response */
int qd_handler_ctx_append_response(qd_handler_ctx_t *ctx, const void *data, size_t len);

/* Allocate response buffer */
void *qd_handler_ctx_alloc_response(qd_handler_ctx_t *ctx, size_t len);

/*
 * Statistics
 */

typedef struct qd_handler_stats {
    uint64_t total_calls;
    uint64_t total_errors;
    uint64_t unknown_commands;
    int num_handlers;
} qd_handler_stats_t;

void qd_handler_get_stats(qd_handler_table_t *table, qd_handler_stats_t *stats);

/* Get per-handler stats */
typedef struct qd_handler_entry_stats {
    uint32_t cmd;
    const char *name;
    uint64_t call_count;
    uint64_t error_count;
    uint64_t avg_time_ns;
} qd_handler_entry_stats_t;

int qd_handler_get_entry_stats(qd_handler_table_t *table, uint32_t cmd,
                               qd_handler_entry_stats_t *stats);

/* Iterate over all handlers */
typedef int (*qd_handler_iter_fn_t)(const qd_handler_entry_stats_t *stats, void *arg);
void qd_handler_iterate(qd_handler_table_t *table, qd_handler_iter_fn_t iter, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* QD_HANDLER_H */
