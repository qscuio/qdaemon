/*
 * QDaemon Client - Connection Management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <pthread.h>

#include "qdclient.h"

/* Internal client structure */
struct qdc_client {
    int fd;
    char *socket_path;
    int connected;
    int timeout_ms;
    int reconnect;
    int reconnect_interval_ms;
    
    uint8_t *recv_buf;
    size_t recv_buf_size;
    size_t recv_len;
    
    uint8_t *send_buf;
    size_t send_buf_size;
    
    uint32_t next_msg_id;
    
    qdc_connect_cb_t connect_cb;
    void *connect_cb_arg;
    
    pthread_mutex_t lock;
};

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int do_connect(qdc_client_t *c)
{
    if (c->fd >= 0) close(c->fd);
    
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) return QDC_ERR_IO;
    
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, c->socket_path, sizeof(addr.sun_path) - 1);
    
    if (connect(c->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(c->fd);
        c->fd = -1;
        return QDC_ERR_CONN;
    }
    
    set_nonblocking(c->fd);
    c->connected = 1;
    
    if (c->connect_cb)
        c->connect_cb(c, 1, c->connect_cb_arg);
    
    return QDC_OK;
}

qdc_client_t *qdc_connect(const char *socket_path)
{
    qdc_client_config_t cfg = QDC_CLIENT_CONFIG_DEFAULT;
    cfg.socket_path = socket_path;
    return qdc_connect_ex(&cfg);
}

qdc_client_t *qdc_connect_ex(const qdc_client_config_t *config)
{
    if (!config || !config->socket_path) return NULL;
    
    qdc_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    
    c->fd = -1;
    c->socket_path = strdup(config->socket_path);
    c->timeout_ms = config->timeout_ms;
    c->reconnect = config->reconnect;
    c->reconnect_interval_ms = config->reconnect_interval_ms;
    c->recv_buf_size = config->recv_buffer_size > 0 ? config->recv_buffer_size : 65536;
    c->send_buf_size = config->send_buffer_size > 0 ? config->send_buffer_size : 65536;
    
    c->recv_buf = malloc(c->recv_buf_size);
    c->send_buf = malloc(c->send_buf_size);
    if (!c->recv_buf || !c->send_buf) {
        free(c->recv_buf);
        free(c->send_buf);
        free(c->socket_path);
        free(c);
        return NULL;
    }
    
    pthread_mutex_init(&c->lock, NULL);
    
    if (do_connect(c) != QDC_OK) {
        free(c->recv_buf);
        free(c->send_buf);
        free(c->socket_path);
        pthread_mutex_destroy(&c->lock);
        free(c);
        return NULL;
    }
    
    return c;
}

void qdc_disconnect(qdc_client_t *c)
{
    if (!c) return;
    
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->connected = 0;
    
    if (c->connect_cb)
        c->connect_cb(c, 0, c->connect_cb_arg);
    
    free(c->recv_buf);
    free(c->send_buf);
    free(c->socket_path);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

int qdc_is_connected(qdc_client_t *c) { return c && c->connected; }

void qdc_set_connect_callback(qdc_client_t *c, qdc_connect_cb_t cb, void *arg)
{
    if (c) { c->connect_cb = cb; c->connect_cb_arg = arg; }
}

int qdc_get_fd(qdc_client_t *c) { return c ? c->fd : -1; }

/* IPC protocol header - must match server */
#define QDC_MAGIC   0x51444950
#define QDC_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t payload_len;
    uint32_t flags;
    uint64_t timestamp;
} __attribute__((packed)) qdc_header_t;

#define QDC_MAX_NAME_LEN 64
#define QDC_MSG_REQUEST  4
#define QDC_MSG_RESPONSE 5
#define QDC_MSG_NOTIFY   6

static int send_full(int fd, const void *buf, size_t len, int timeout_ms)
{
    const uint8_t *p = buf;
    size_t sent = 0;
    
    while (sent < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return QDC_ERR_TIMEOUT;
        
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN) continue;
            return QDC_ERR_IO;
        }
        sent += n;
    }
    return QDC_OK;
}

static int recv_full(int fd, void *buf, size_t len, int timeout_ms)
{
    uint8_t *p = buf;
    size_t recvd = 0;
    
    while (recvd < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return QDC_ERR_TIMEOUT;
        
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n <= 0) {
            if (n < 0 && errno == EAGAIN) continue;
            return n == 0 ? QDC_ERR_CONN : QDC_ERR_IO;
        }
        recvd += n;
    }
    return QDC_OK;
}

qdc_response_t *qdc_request(qdc_client_t *c, const char *service, const void *data, size_t len)
{
    return qdc_request_timeout(c, service, data, len, c ? c->timeout_ms : 5000);
}

qdc_response_t *qdc_request_timeout(qdc_client_t *c, const char *service,
                                     const void *data, size_t len, int timeout_ms)
{
    if (!c || !c->connected || !service) return NULL;
    
    pthread_mutex_lock(&c->lock);
    
    /* Build message */
    qdc_header_t hdr = {
        .magic = QDC_MAGIC,
        .version = QDC_VERSION,
        .msg_type = QDC_MSG_REQUEST,
        .msg_id = ++c->next_msg_id,
        .payload_len = len,
        .flags = 0
    };
    
    char svc_buf[QDC_MAX_NAME_LEN] = {0};
    strncpy(svc_buf, service, QDC_MAX_NAME_LEN - 1);
    
    /* Send header + service name + payload */
    if (send_full(c->fd, &hdr, sizeof(hdr), timeout_ms) != QDC_OK ||
        send_full(c->fd, svc_buf, QDC_MAX_NAME_LEN, timeout_ms) != QDC_OK ||
        (len > 0 && send_full(c->fd, data, len, timeout_ms) != QDC_OK)) {
        pthread_mutex_unlock(&c->lock);
        c->connected = 0;
        return NULL;
    }
    
    /* Receive response header */
    qdc_header_t resp_hdr;
    if (recv_full(c->fd, &resp_hdr, sizeof(resp_hdr), timeout_ms) != QDC_OK) {
        pthread_mutex_unlock(&c->lock);
        c->connected = 0;
        return NULL;
    }
    
    if (resp_hdr.magic != QDC_MAGIC || resp_hdr.msg_type != QDC_MSG_RESPONSE) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }
    
    /* Skip service name in response */
    char resp_svc[QDC_MAX_NAME_LEN];
    recv_full(c->fd, resp_svc, QDC_MAX_NAME_LEN, timeout_ms);
    
    /* Receive payload */
    qdc_response_t *resp = calloc(1, sizeof(*resp));
    if (!resp) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }
    
    resp->status = 0;
    resp->msg_id = resp_hdr.msg_id;
    resp->len = resp_hdr.payload_len;
    
    if (resp->len > 0) {
        resp->data = malloc(resp->len);
        if (!resp->data || recv_full(c->fd, resp->data, resp->len, timeout_ms) != QDC_OK) {
            free(resp->data);
            free(resp);
            pthread_mutex_unlock(&c->lock);
            return NULL;
        }
    }
    
    pthread_mutex_unlock(&c->lock);
    return resp;
}

void qdc_response_free(qdc_response_t *resp)
{
    if (!resp) return;
    free(resp->data);
    free(resp);
}

int qdc_notify(qdc_client_t *c, const char *service, const void *data, size_t len)
{
    if (!c || !c->connected || !service) return QDC_ERR_INVAL;
    
    pthread_mutex_lock(&c->lock);
    
    qdc_header_t hdr = {
        .magic = QDC_MAGIC,
        .version = QDC_VERSION,
        .msg_type = QDC_MSG_NOTIFY,
        .msg_id = ++c->next_msg_id,
        .payload_len = len,
        .flags = 0
    };
    
    char svc_buf[QDC_MAX_NAME_LEN] = {0};
    strncpy(svc_buf, service, QDC_MAX_NAME_LEN - 1);
    
    int ret = QDC_OK;
    if (send_full(c->fd, &hdr, sizeof(hdr), c->timeout_ms) != QDC_OK ||
        send_full(c->fd, svc_buf, QDC_MAX_NAME_LEN, c->timeout_ms) != QDC_OK ||
        (len > 0 && send_full(c->fd, data, len, c->timeout_ms) != QDC_OK)) {
        c->connected = 0;
        ret = QDC_ERR_IO;
    }
    
    pthread_mutex_unlock(&c->lock);
    return ret;
}

int qdc_process(qdc_client_t *c)
{
    (void)c;
    return QDC_OK; /* TODO: Process async responses */
}

int qdc_subscribe(qdc_client_t *c, const char *event, qdc_event_cb_t cb, void *arg)
{
    (void)c; (void)event; (void)cb; (void)arg;
    return QDC_OK; /* TODO */
}

int qdc_unsubscribe(qdc_client_t *c, const char *event)
{
    (void)c; (void)event;
    return QDC_OK;
}
