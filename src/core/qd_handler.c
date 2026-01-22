/*
 * QDaemon - Table-Based Message Handler Implementation
 * Declarative message handler registration and dispatch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qdaemon/qd_handler.h"
#include "qdaemon/qd_memory.h"

/* Hash function for command IDs */
static inline uint32_t handler_hash(uint32_t cmd)
{
    /* Simple multiplicative hash */
    return (cmd * 2654435761U) % QD_HANDLER_HASH_SIZE;
}

/*
 * Handler Table Management
 */

qd_handler_table_t *qd_handler_table_create(const char *name)
{
    qd_handler_table_t *table = calloc(1, sizeof(qd_handler_table_t));
    if (!table)
        return NULL;

    if (name)
        snprintf(table->name, sizeof(table->name), "%s", name);
    else
        snprintf(table->name, sizeof(table->name), "handler_table");

    return table;
}

void qd_handler_table_destroy(qd_handler_table_t *table)
{
    if (!table)
        return;

    /* Free all handler nodes */
    for (int i = 0; i < QD_HANDLER_HASH_SIZE; i++) {
        qd_handler_node_t *node = table->buckets[i];
        while (node) {
            qd_handler_node_t *next = node->next;
            free(node);
            node = next;
        }
    }

    free(table);
}

int qd_handler_register(qd_handler_table_t *table, const qd_handler_entry_t *entry)
{
    if (!table || !entry || !entry->handler)
        return QD_ERR_INVAL;

    return qd_handler_register_fn(table, entry->cmd, entry->handler,
                                  entry->arg, entry->flags);
}

int qd_handler_register_fn(qd_handler_table_t *table, uint32_t cmd,
                           qd_handler_fn_t handler, void *arg, uint32_t flags)
{
    if (!table || !handler)
        return QD_ERR_INVAL;

    if (table->num_handlers >= QD_HANDLER_MAX_ENTRIES)
        return QD_ERR_NOMEM;

    /* Check for duplicate */
    uint32_t hash = handler_hash(cmd);
    qd_handler_node_t *node = table->buckets[hash];
    while (node) {
        if (node->cmd == cmd) {
            /* Update existing handler */
            node->handler = handler;
            node->arg = arg;
            node->flags = flags;
            return QD_OK;
        }
        node = node->next;
    }

    /* Create new node */
    node = calloc(1, sizeof(qd_handler_node_t));
    if (!node)
        return QD_ERR_NOMEM;

    node->cmd = cmd;
    node->handler = handler;
    node->arg = arg;
    node->flags = flags;

    /* Insert at head of bucket */
    node->next = table->buckets[hash];
    table->buckets[hash] = node;
    table->num_handlers++;

    return QD_OK;
}

int qd_handler_register_table(qd_handler_table_t *table, const qd_handler_entry_t *entries)
{
    if (!table || !entries)
        return QD_ERR_INVAL;

    int count = 0;
    const qd_handler_entry_t *entry = entries;

    while (entry->handler != NULL) {
        int ret = qd_handler_register(table, entry);
        if (ret != QD_OK)
            return ret;
        count++;
        entry++;
    }

    return count;
}

int qd_handler_unregister(qd_handler_table_t *table, uint32_t cmd)
{
    if (!table)
        return QD_ERR_INVAL;

    uint32_t hash = handler_hash(cmd);
    qd_handler_node_t **pp = &table->buckets[hash];

    while (*pp) {
        if ((*pp)->cmd == cmd) {
            qd_handler_node_t *node = *pp;
            *pp = node->next;
            free(node);
            table->num_handlers--;
            return QD_OK;
        }
        pp = &(*pp)->next;
    }

    return QD_ERR_NOENT;
}

void qd_handler_set_default(qd_handler_table_t *table, qd_handler_fn_t handler, void *arg)
{
    if (!table)
        return;
    table->default_handler = handler;
    table->default_handler_arg = arg;
}

void qd_handler_set_pre_hook(qd_handler_table_t *table, qd_pre_handler_fn_t hook, void *arg)
{
    if (!table)
        return;
    table->pre_hook = hook;
    table->pre_hook_arg = arg;
}

void qd_handler_set_post_hook(qd_handler_table_t *table, qd_post_handler_fn_t hook, void *arg)
{
    if (!table)
        return;
    table->post_hook = hook;
    table->post_hook_arg = arg;
}

void qd_handler_set_error_handler(qd_handler_table_t *table, qd_error_handler_fn_t handler, void *arg)
{
    if (!table)
        return;
    table->error_handler = handler;
    table->error_handler_arg = arg;
}

void qd_handler_set_user_data(qd_handler_table_t *table, void *user_data)
{
    if (table)
        table->user_data = user_data;
}

void *qd_handler_get_user_data(qd_handler_table_t *table)
{
    return table ? table->user_data : NULL;
}

/*
 * Handler Lookup
 */

static qd_handler_node_t *find_handler(qd_handler_table_t *table, uint32_t cmd)
{
    uint32_t hash = handler_hash(cmd);
    qd_handler_node_t *node = table->buckets[hash];

    while (node) {
        if (node->cmd == cmd)
            return node;
        node = node->next;
    }

    return NULL;
}

int qd_handler_exists(qd_handler_table_t *table, uint32_t cmd)
{
    if (!table)
        return 0;
    return find_handler(table, cmd) != NULL;
}

const qd_handler_entry_t *qd_handler_get(qd_handler_table_t *table, uint32_t cmd)
{
    if (!table)
        return NULL;

    qd_handler_node_t *node = find_handler(table, cmd);
    if (!node)
        return NULL;

    /* Return pointer to node (same layout as entry) */
    return (const qd_handler_entry_t *)node;
}

/*
 * Handler Dispatch
 */

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

qd_handler_result_t qd_handler_dispatch(qd_handler_table_t *table, qd_handler_ctx_t *ctx)
{
    if (!table || !ctx)
        return QD_HANDLER_INVALID;

    return qd_handler_dispatch_cmd(table, ctx->cmd, ctx);
}

qd_handler_result_t qd_handler_dispatch_cmd(qd_handler_table_t *table, uint32_t cmd,
                                             qd_handler_ctx_t *ctx)
{
    if (!table || !ctx)
        return QD_HANDLER_INVALID;

    ctx->cmd = cmd;
    ctx->user_data = table->user_data;
    table->total_calls++;

    /* Pre-hook */
    if (table->pre_hook) {
        int ret = table->pre_hook(ctx, table->pre_hook_arg);
        if (ret != 0) {
            return QD_HANDLER_SKIP;
        }
    }

    /* Find handler */
    qd_handler_node_t *node = find_handler(table, cmd);
    qd_handler_fn_t handler;
    void *handler_arg;

    if (node) {
        handler = node->handler;
        handler_arg = node->arg;
    } else if (table->default_handler) {
        handler = table->default_handler;
        handler_arg = table->default_handler_arg;
        table->unknown_commands++;
    } else {
        table->unknown_commands++;
        return QD_HANDLER_NOT_FOUND;
    }

    /* Execute handler */
    uint64_t start_time = 0;
    if (node && (node->flags & QD_HANDLER_FLAG_METRIC)) {
        start_time = get_time_ns();
    }

    qd_handler_result_t result = handler(ctx, handler_arg);

    /* Update statistics */
    if (node) {
        node->call_count++;
        if (result < 0) {
            node->error_count++;
            table->total_errors++;
        }
        if (start_time > 0) {
            node->total_time_ns += get_time_ns() - start_time;
        }
    }

    /* Error handler */
    if (result < 0 && table->error_handler) {
        table->error_handler(ctx, result, table->error_handler_arg);
    }

    /* Post-hook */
    if (table->post_hook) {
        table->post_hook(ctx, result, table->post_hook_arg);
    }

    return result;
}

/*
 * Handler Context Helpers
 */

void qd_handler_ctx_init(qd_handler_ctx_t *ctx)
{
    if (ctx)
        memset(ctx, 0, sizeof(*ctx));
}

int qd_handler_ctx_set_response(qd_handler_ctx_t *ctx, const void *data, size_t len)
{
    if (!ctx)
        return QD_ERR_INVAL;

    if (ctx->response && ctx->response_cap > 0) {
        /* Use existing buffer */
        if (len > ctx->response_cap)
            len = ctx->response_cap;
        memcpy(ctx->response, data, len);
        ctx->response_len = len;
    } else {
        /* Allocate new buffer */
        ctx->response = malloc(len);
        if (!ctx->response)
            return QD_ERR_NOMEM;
        memcpy(ctx->response, data, len);
        ctx->response_len = len;
        ctx->response_cap = len;
    }

    return QD_OK;
}

int qd_handler_ctx_append_response(qd_handler_ctx_t *ctx, const void *data, size_t len)
{
    if (!ctx)
        return QD_ERR_INVAL;

    size_t new_len = ctx->response_len + len;

    if (new_len > ctx->response_cap) {
        /* Grow buffer */
        size_t new_cap = ctx->response_cap == 0 ? 256 : ctx->response_cap * 2;
        while (new_cap < new_len)
            new_cap *= 2;

        void *new_buf = realloc(ctx->response, new_cap);
        if (!new_buf)
            return QD_ERR_NOMEM;

        ctx->response = new_buf;
        ctx->response_cap = new_cap;
    }

    memcpy((char *)ctx->response + ctx->response_len, data, len);
    ctx->response_len = new_len;

    return QD_OK;
}

void *qd_handler_ctx_alloc_response(qd_handler_ctx_t *ctx, size_t len)
{
    if (!ctx)
        return NULL;

    if (ctx->response_cap >= len) {
        ctx->response_len = len;
        return ctx->response;
    }

    void *buf = realloc(ctx->response, len);
    if (!buf)
        return NULL;

    ctx->response = buf;
    ctx->response_len = len;
    ctx->response_cap = len;

    return buf;
}

/*
 * Statistics
 */

void qd_handler_get_stats(qd_handler_table_t *table, qd_handler_stats_t *stats)
{
    if (!table || !stats)
        return;

    stats->total_calls = table->total_calls;
    stats->total_errors = table->total_errors;
    stats->unknown_commands = table->unknown_commands;
    stats->num_handlers = table->num_handlers;
}

int qd_handler_get_entry_stats(qd_handler_table_t *table, uint32_t cmd,
                               qd_handler_entry_stats_t *stats)
{
    if (!table || !stats)
        return QD_ERR_INVAL;

    qd_handler_node_t *node = find_handler(table, cmd);
    if (!node)
        return QD_ERR_NOENT;

    stats->cmd = node->cmd;
    stats->name = node->name;
    stats->call_count = node->call_count;
    stats->error_count = node->error_count;
    stats->avg_time_ns = node->call_count > 0 ? node->total_time_ns / node->call_count : 0;

    return QD_OK;
}

void qd_handler_iterate(qd_handler_table_t *table, qd_handler_iter_fn_t iter, void *arg)
{
    if (!table || !iter)
        return;

    for (int i = 0; i < QD_HANDLER_HASH_SIZE; i++) {
        qd_handler_node_t *node = table->buckets[i];
        while (node) {
            qd_handler_entry_stats_t stats = {
                .cmd = node->cmd,
                .name = node->name,
                .call_count = node->call_count,
                .error_count = node->error_count,
                .avg_time_ns = node->call_count > 0 ? node->total_time_ns / node->call_count : 0
            };

            if (iter(&stats, arg) != 0)
                return;

            node = node->next;
        }
    }
}
