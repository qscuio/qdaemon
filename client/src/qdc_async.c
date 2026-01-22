/*
 * QDaemon Client - Async Request Handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "qdclient.h"

#define MAX_PENDING 256

typedef struct {
    uint32_t msg_id;
    qdc_callback_t callback;
    void *arg;
    int active;
} pending_request_t;

static struct {
    pending_request_t requests[MAX_PENDING];
    int count;
    pthread_mutex_t lock;
} g_pending = {
    .count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

int qdc_request_async(qdc_client_t *client, const char *service,
                       const void *data, size_t len,
                       qdc_callback_t cb, void *arg)
{
    if (!client || !service) return QDC_ERR_INVAL;
    
    pthread_mutex_lock(&g_pending.lock);
    
    if (g_pending.count >= MAX_PENDING) {
        pthread_mutex_unlock(&g_pending.lock);
        return QDC_ERR_NOMEM;
    }
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g_pending.requests[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        pthread_mutex_unlock(&g_pending.lock);
        return QDC_ERR_NOMEM;
    }
    
    /* Send request (sync for now, just stores callback) */
    qdc_response_t *resp = qdc_request(client, service, data, len);
    
    if (resp) {
        /* Immediate response - call callback directly */
        pthread_mutex_unlock(&g_pending.lock);
        if (cb) cb(resp, arg);
        qdc_response_free(resp);
        return QDC_OK;
    }
    
    /* Store pending request for later */
    g_pending.requests[slot].callback = cb;
    g_pending.requests[slot].arg = arg;
    g_pending.requests[slot].active = 1;
    g_pending.count++;
    
    pthread_mutex_unlock(&g_pending.lock);
    return QDC_OK;
}

void qdc_async_complete(uint32_t msg_id, qdc_response_t *resp)
{
    pthread_mutex_lock(&g_pending.lock);
    
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending.requests[i].active &&
            g_pending.requests[i].msg_id == msg_id) {
            
            qdc_callback_t cb = g_pending.requests[i].callback;
            void *arg = g_pending.requests[i].arg;
            
            g_pending.requests[i].active = 0;
            g_pending.count--;
            
            pthread_mutex_unlock(&g_pending.lock);
            
            if (cb) cb(resp, arg);
            return;
        }
    }
    
    pthread_mutex_unlock(&g_pending.lock);
}

int qdc_async_pending_count(void)
{
    return g_pending.count;
}

void qdc_async_cancel_all(void)
{
    pthread_mutex_lock(&g_pending.lock);
    
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending.requests[i].active) {
            qdc_callback_t cb = g_pending.requests[i].callback;
            void *arg = g_pending.requests[i].arg;
            g_pending.requests[i].active = 0;
            
            if (cb) {
                qdc_response_t resp = { .status = QDC_ERR_CONN };
                cb(&resp, arg);
            }
        }
    }
    
    g_pending.count = 0;
    pthread_mutex_unlock(&g_pending.lock);
}
