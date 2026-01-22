/*
 * QDaemon Client Library
 * API for connecting to and communicating with QDaemon services
 */

#ifndef QDCLIENT_H
#define QDCLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes */
#define QDC_OK           0
#define QDC_ERR_NOMEM   -1
#define QDC_ERR_INVAL   -2
#define QDC_ERR_IO      -3
#define QDC_ERR_TIMEOUT -4
#define QDC_ERR_PROTO   -5
#define QDC_ERR_CONN    -6

/* Client handle */
typedef struct qdc_client qdc_client_t;

/* Response structure */
typedef struct qdc_response {
    int status;
    void *data;
    size_t len;
    uint32_t msg_id;
} qdc_response_t;

/* Callback types */
typedef void (*qdc_callback_t)(qdc_response_t *resp, void *arg);
typedef void (*qdc_event_cb_t)(const char *event, const void *data, size_t len, void *arg);
typedef void (*qdc_connect_cb_t)(qdc_client_t *client, int connected, void *arg);

/* Client configuration */
typedef struct qdc_client_config {
    const char *socket_path;
    int timeout_ms;
    int reconnect;
    int reconnect_interval_ms;
    size_t recv_buffer_size;
    size_t send_buffer_size;
} qdc_client_config_t;

#define QDC_CLIENT_CONFIG_DEFAULT { \
    .socket_path = "/var/run/qdaemon.sock", \
    .timeout_ms = 5000, \
    .reconnect = 1, \
    .reconnect_interval_ms = 1000, \
    .recv_buffer_size = 65536, \
    .send_buffer_size = 65536 \
}

/*
 * Connection Management
 */

/* Create client with default config */
qdc_client_t *qdc_connect(const char *socket_path);

/* Create client with custom config */
qdc_client_t *qdc_connect_ex(const qdc_client_config_t *config);

/* Disconnect and destroy client */
void qdc_disconnect(qdc_client_t *client);

/* Check if connected */
int qdc_is_connected(qdc_client_t *client);

/* Set connection callback */
void qdc_set_connect_callback(qdc_client_t *client, qdc_connect_cb_t cb, void *arg);

/* Get socket fd for external event loop integration */
int qdc_get_fd(qdc_client_t *client);

/*
 * Synchronous Requests
 */

/* Send request and wait for response */
qdc_response_t *qdc_request(qdc_client_t *client, const char *service,
                             const void *data, size_t len);

/* Send request with timeout */
qdc_response_t *qdc_request_timeout(qdc_client_t *client, const char *service,
                                     const void *data, size_t len, int timeout_ms);

/* Free response */
void qdc_response_free(qdc_response_t *resp);

/*
 * Asynchronous Requests
 */

/* Send async request */
int qdc_request_async(qdc_client_t *client, const char *service,
                       const void *data, size_t len,
                       qdc_callback_t cb, void *arg);

/* Process pending responses (call from event loop) */
int qdc_process(qdc_client_t *client);

/*
 * Notifications (one-way messages)
 */

/* Send notification (no response expected) */
int qdc_notify(qdc_client_t *client, const char *service,
                const void *data, size_t len);

/*
 * Subscriptions
 */

/* Subscribe to events */
int qdc_subscribe(qdc_client_t *client, const char *event,
                   qdc_event_cb_t cb, void *arg);

/* Unsubscribe from events */
int qdc_unsubscribe(qdc_client_t *client, const char *event);

/*
 * Message Building Helpers
 */

typedef struct qdc_message qdc_message_t;

/* Create message */
qdc_message_t *qdc_message_create(const char *service);

/* Add data to message */
int qdc_message_add_string(qdc_message_t *msg, const char *key, const char *value);
int qdc_message_add_int(qdc_message_t *msg, const char *key, int64_t value);
int qdc_message_add_float(qdc_message_t *msg, const char *key, double value);
int qdc_message_add_bool(qdc_message_t *msg, const char *key, int value);
int qdc_message_add_binary(qdc_message_t *msg, const char *key, const void *data, size_t len);

/* Send message */
qdc_response_t *qdc_message_send(qdc_client_t *client, qdc_message_t *msg);
int qdc_message_send_async(qdc_client_t *client, qdc_message_t *msg,
                            qdc_callback_t cb, void *arg);

/* Destroy message */
void qdc_message_destroy(qdc_message_t *msg);

/*
 * Response Parsing Helpers
 */

const char *qdc_response_get_string(qdc_response_t *resp, const char *key);
int64_t qdc_response_get_int(qdc_response_t *resp, const char *key, int64_t def);
double qdc_response_get_float(qdc_response_t *resp, const char *key, double def);
int qdc_response_get_bool(qdc_response_t *resp, const char *key, int def);
const void *qdc_response_get_binary(qdc_response_t *resp, const char *key, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* QDCLIENT_H */
