/*
 * QDaemon Client - Message Building
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qdclient.h"

#define MAX_MSG_ENTRIES 64

typedef struct {
    char key[64];
    enum { T_STR, T_INT, T_FLOAT, T_BOOL, T_BIN } type;
    union {
        char *str;
        int64_t i64;
        double f64;
        int b;
        struct { void *data; size_t len; } bin;
    } val;
} msg_entry_t;

struct qdc_message {
    char service[64];
    msg_entry_t entries[MAX_MSG_ENTRIES];
    int count;
};

qdc_message_t *qdc_message_create(const char *service)
{
    qdc_message_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (service) strncpy(m->service, service, sizeof(m->service) - 1);
    return m;
}

void qdc_message_destroy(qdc_message_t *m)
{
    if (!m) return;
    for (int i = 0; i < m->count; i++) {
        if (m->entries[i].type == T_STR) free(m->entries[i].val.str);
        if (m->entries[i].type == T_BIN) free(m->entries[i].val.bin.data);
    }
    free(m);
}

int qdc_message_add_string(qdc_message_t *m, const char *key, const char *value)
{
    if (!m || m->count >= MAX_MSG_ENTRIES) return QDC_ERR_NOMEM;
    msg_entry_t *e = &m->entries[m->count++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->type = T_STR;
    e->val.str = value ? strdup(value) : NULL;
    return QDC_OK;
}

int qdc_message_add_int(qdc_message_t *m, const char *key, int64_t value)
{
    if (!m || m->count >= MAX_MSG_ENTRIES) return QDC_ERR_NOMEM;
    msg_entry_t *e = &m->entries[m->count++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->type = T_INT;
    e->val.i64 = value;
    return QDC_OK;
}

int qdc_message_add_float(qdc_message_t *m, const char *key, double value)
{
    if (!m || m->count >= MAX_MSG_ENTRIES) return QDC_ERR_NOMEM;
    msg_entry_t *e = &m->entries[m->count++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->type = T_FLOAT;
    e->val.f64 = value;
    return QDC_OK;
}

int qdc_message_add_bool(qdc_message_t *m, const char *key, int value)
{
    if (!m || m->count >= MAX_MSG_ENTRIES) return QDC_ERR_NOMEM;
    msg_entry_t *e = &m->entries[m->count++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->type = T_BOOL;
    e->val.b = value ? 1 : 0;
    return QDC_OK;
}

int qdc_message_add_binary(qdc_message_t *m, const char *key, const void *data, size_t len)
{
    if (!m || m->count >= MAX_MSG_ENTRIES) return QDC_ERR_NOMEM;
    msg_entry_t *e = &m->entries[m->count++];
    strncpy(e->key, key, sizeof(e->key) - 1);
    e->type = T_BIN;
    e->val.bin.data = malloc(len);
    if (!e->val.bin.data) { m->count--; return QDC_ERR_NOMEM; }
    memcpy(e->val.bin.data, data, len);
    e->val.bin.len = len;
    return QDC_OK;
}

/* Serialize to simple binary format */
static size_t serialize_message(qdc_message_t *m, void **buf)
{
    /* Simple format: count + entries */
    size_t size = 4096;
    uint8_t *b = malloc(size);
    if (!b) { *buf = NULL; return 0; }
    
    uint8_t *p = b;
    *(int32_t*)p = m->count; p += 4;
    
    for (int i = 0; i < m->count; i++) {
        msg_entry_t *e = &m->entries[i];
        size_t keylen = strlen(e->key);
        *p++ = (uint8_t)keylen;
        memcpy(p, e->key, keylen); p += keylen;
        *p++ = (uint8_t)e->type;
        
        switch (e->type) {
            case T_STR: {
                size_t len = e->val.str ? strlen(e->val.str) : 0;
                *(uint32_t*)p = len; p += 4;
                if (len) { memcpy(p, e->val.str, len); p += len; }
                break;
            }
            case T_INT:
                *(int64_t*)p = e->val.i64; p += 8;
                break;
            case T_FLOAT:
                *(double*)p = e->val.f64; p += 8;
                break;
            case T_BOOL:
                *p++ = e->val.b;
                break;
            case T_BIN:
                *(uint32_t*)p = e->val.bin.len; p += 4;
                memcpy(p, e->val.bin.data, e->val.bin.len);
                p += e->val.bin.len;
                break;
        }
    }
    
    *buf = b;
    return p - b;
}

qdc_response_t *qdc_message_send(qdc_client_t *client, qdc_message_t *msg)
{
    if (!client || !msg) return NULL;
    
    void *buf;
    size_t len = serialize_message(msg, &buf);
    if (!buf) return NULL;
    
    qdc_response_t *resp = qdc_request(client, msg->service, buf, len);
    free(buf);
    return resp;
}

int qdc_message_send_async(qdc_client_t *client, qdc_message_t *msg,
                            qdc_callback_t cb, void *arg)
{
    if (!client || !msg) return QDC_ERR_INVAL;
    
    void *buf;
    size_t len = serialize_message(msg, &buf);
    if (!buf) return QDC_ERR_NOMEM;
    
    int ret = qdc_request_async(client, msg->service, buf, len, cb, arg);
    free(buf);
    return ret;
}

/* Response parsing - simple implementation */
const char *qdc_response_get_string(qdc_response_t *resp, const char *key)
{
    (void)resp; (void)key;
    return NULL; /* TODO: Parse response data */
}

int64_t qdc_response_get_int(qdc_response_t *resp, const char *key, int64_t def)
{
    (void)resp; (void)key;
    return def;
}

double qdc_response_get_float(qdc_response_t *resp, const char *key, double def)
{
    (void)resp; (void)key;
    return def;
}

int qdc_response_get_bool(qdc_response_t *resp, const char *key, int def)
{
    (void)resp; (void)key;
    return def;
}

const void *qdc_response_get_binary(qdc_response_t *resp, const char *key, size_t *len)
{
    (void)resp; (void)key;
    if (len) *len = 0;
    return NULL;
}
