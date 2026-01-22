/*
 * QDaemon - Userspace IPC API
 * Unix domain socket IPC with message protocol and shared memory
 */

#ifndef QD_IPC_H
#define QD_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/un.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_ipc_server qd_ipc_server_t;
typedef struct qd_ipc_client qd_ipc_client_t;
typedef struct qd_ipc_conn qd_ipc_conn_t;
typedef struct qd_ipc_msg qd_ipc_msg_t;
typedef struct qd_shm_ring qd_shm_ring_t;
typedef struct qd_event_loop qd_event_loop_t;

/* IPC protocol constants */
#define QD_IPC_MAGIC           0x51444950  /* "QDIP" */
#define QD_IPC_VERSION         1
#define QD_IPC_MAX_MSG_SIZE    (64 * 1024)
#define QD_IPC_MAX_NAME_LEN    64
#define QD_IPC_MAX_CLIENTS     256
#define QD_IPC_HEADER_SIZE     32

/* IPC message types */
typedef enum {
    QD_IPC_MSG_REGISTER = 1,     /* Register service */
    QD_IPC_MSG_UNREGISTER,       /* Unregister service */
    QD_IPC_MSG_DISCOVER,         /* Discover services */
    QD_IPC_MSG_REQUEST,          /* Request to service */
    QD_IPC_MSG_RESPONSE,         /* Response from service */
    QD_IPC_MSG_NOTIFY,           /* Notification (one-way) */
    QD_IPC_MSG_HEARTBEAT,        /* Heartbeat */
    QD_IPC_MSG_SUBSCRIBE,        /* Subscribe to events */
    QD_IPC_MSG_UNSUBSCRIBE,      /* Unsubscribe from events */
    QD_IPC_MSG_EVENT,            /* Event notification */
    QD_IPC_MSG_ERROR,            /* Error response */
} qd_ipc_msg_type_t;

/* IPC message flags */
typedef enum {
    QD_IPC_FLAG_NONE      = 0x00,
    QD_IPC_FLAG_REPLY_REQ = 0x01,  /* Reply required */
    QD_IPC_FLAG_BROADCAST = 0x02,  /* Broadcast message */
    QD_IPC_FLAG_URGENT    = 0x04,  /* Urgent message */
    QD_IPC_FLAG_COMPRESS  = 0x08,  /* Payload is compressed */
    QD_IPC_FLAG_ENCRYPTED = 0x10,  /* Payload is encrypted */
} qd_ipc_flags_t;

/* IPC message header */
typedef struct qd_ipc_header {
    uint32_t magic;              /* Protocol magic */
    uint16_t version;            /* Protocol version */
    uint16_t msg_type;           /* Message type */
    uint32_t msg_id;             /* Message ID for request/response matching */
    uint32_t payload_len;        /* Payload length */
    uint32_t flags;              /* Message flags */
    uint64_t timestamp;          /* Message timestamp */
} __attribute__((packed)) qd_ipc_header_t;

/* IPC message structure */
struct qd_ipc_msg {
    qd_ipc_header_t header;      /* Message header */
    char service[QD_IPC_MAX_NAME_LEN]; /* Target service name */
    void *payload;               /* Payload data */
    size_t payload_cap;          /* Payload capacity */
    int owned;                   /* Whether we own the payload buffer */
};

/* Connection state */
typedef enum {
    QD_IPC_CONN_DISCONNECTED = 0,
    QD_IPC_CONN_CONNECTING,
    QD_IPC_CONN_CONNECTED,
    QD_IPC_CONN_CLOSING,
} qd_ipc_conn_state_t;

/* IPC connection (server-side client representation) */
struct qd_ipc_conn {
    int fd;                      /* Socket fd */
    qd_ipc_conn_state_t state;   /* Connection state */
    uint32_t id;                 /* Connection ID */
    char name[QD_IPC_MAX_NAME_LEN]; /* Client name */
    qd_ipc_server_t *server;     /* Parent server */
    void *user_data;             /* User data */

    /* Receive buffer */
    uint8_t *recv_buf;
    size_t recv_len;
    size_t recv_cap;

    /* Send buffer */
    uint8_t *send_buf;
    size_t send_len;
    size_t send_cap;

    /* Statistics */
    uint64_t msgs_received;
    uint64_t msgs_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;

    /* List linkage */
    struct qd_ipc_conn *next;
    struct qd_ipc_conn *prev;
};

/* IPC handler callback */
typedef int (*qd_ipc_handler_t)(qd_ipc_conn_t *conn, qd_ipc_msg_t *msg, void *arg);

/* IPC connection callback */
typedef void (*qd_ipc_conn_cb_t)(qd_ipc_conn_t *conn, void *arg);

/* Service registration */
typedef struct qd_ipc_service {
    char name[QD_IPC_MAX_NAME_LEN];
    qd_ipc_handler_t handler;
    void *arg;
    struct qd_ipc_service *next;
} qd_ipc_service_t;

/* Server configuration */
typedef struct qd_ipc_server_config {
    const char *socket_path;     /* Unix socket path */
    int max_clients;             /* Maximum clients */
    int backlog;                 /* Listen backlog */
    size_t max_msg_size;         /* Maximum message size */
    int recv_buf_size;           /* Receive buffer size */
    int send_buf_size;           /* Send buffer size */
} qd_ipc_server_config_t;

#define QD_IPC_SERVER_CONFIG_DEFAULT { \
    .socket_path = "/tmp/qdaemon.sock", \
    .max_clients = 256, \
    .backlog = 128, \
    .max_msg_size = 64 * 1024, \
    .recv_buf_size = 8192, \
    .send_buf_size = 8192 \
}

/* IPC server structure */
struct qd_ipc_server {
    int listen_fd;               /* Listening socket */
    char socket_path[256];       /* Socket path */
    qd_event_loop_t *loop;       /* Event loop */

    qd_ipc_conn_t *connections;  /* Connected clients */
    int num_connections;
    int max_connections;

    qd_ipc_service_t *services;  /* Registered services */

    /* Handlers */
    qd_ipc_handler_t handlers[16]; /* Per-message-type handlers */
    void *handler_args[16];

    /* Callbacks */
    qd_ipc_conn_cb_t on_connect;
    void *on_connect_arg;
    qd_ipc_conn_cb_t on_disconnect;
    void *on_disconnect_arg;

    /* Configuration */
    size_t max_msg_size;
    size_t recv_buf_size;
    size_t send_buf_size;

    /* Statistics */
    uint64_t total_connections;
    uint64_t total_messages;

    pthread_mutex_t lock;
};

/*
 * IPC Server API
 */

/* Create server */
qd_ipc_server_t *qd_ipc_server_create(const char *socket_path);

/* Create server with configuration */
qd_ipc_server_t *qd_ipc_server_create_ex(const qd_ipc_server_config_t *config);

/* Destroy server */
void qd_ipc_server_destroy(qd_ipc_server_t *server);

/* Start server (attach to event loop) */
int qd_ipc_server_start(qd_ipc_server_t *server, qd_event_loop_t *loop);

/* Stop server */
void qd_ipc_server_stop(qd_ipc_server_t *server);

/* Set message handler */
void qd_ipc_server_set_handler(qd_ipc_server_t *server, qd_ipc_msg_type_t type,
                                qd_ipc_handler_t handler, void *arg);

/* Set connection callbacks */
void qd_ipc_server_set_connect_cb(qd_ipc_server_t *server,
                                   qd_ipc_conn_cb_t callback, void *arg);
void qd_ipc_server_set_disconnect_cb(qd_ipc_server_t *server,
                                      qd_ipc_conn_cb_t callback, void *arg);

/* Register service */
int qd_ipc_server_register_service(qd_ipc_server_t *server, const char *name,
                                    qd_ipc_handler_t handler, void *arg);

/* Unregister service */
int qd_ipc_server_unregister_service(qd_ipc_server_t *server, const char *name);

/* Send message to connection */
int qd_ipc_server_send(qd_ipc_conn_t *conn, qd_ipc_msg_t *msg);

/* Send reply to request */
int qd_ipc_server_reply(qd_ipc_conn_t *conn, qd_ipc_msg_t *request,
                         const void *data, size_t len);

/* Broadcast to all connections */
int qd_ipc_server_broadcast(qd_ipc_server_t *server, qd_ipc_msg_t *msg);

/* Disconnect client */
void qd_ipc_server_disconnect(qd_ipc_conn_t *conn);

/*
 * IPC Client API
 */

/* Client configuration */
typedef struct qd_ipc_client_config {
    const char *name;            /* Client name */
    int recv_buf_size;
    int send_buf_size;
    int connect_timeout_ms;
    int reconnect_interval_ms;
    int auto_reconnect;
} qd_ipc_client_config_t;

#define QD_IPC_CLIENT_CONFIG_DEFAULT { \
    .name = "qd_client", \
    .recv_buf_size = 8192, \
    .send_buf_size = 8192, \
    .connect_timeout_ms = 5000, \
    .reconnect_interval_ms = 1000, \
    .auto_reconnect = 0 \
}

/* Client message callback */
typedef void (*qd_ipc_msg_cb_t)(qd_ipc_client_t *client, qd_ipc_msg_t *msg, void *arg);

/* Client structure */
struct qd_ipc_client {
    int fd;
    qd_ipc_conn_state_t state;
    char socket_path[256];
    char name[QD_IPC_MAX_NAME_LEN];

    qd_event_loop_t *loop;

    /* Receive buffer */
    uint8_t *recv_buf;
    size_t recv_len;
    size_t recv_cap;

    /* Send buffer */
    uint8_t *send_buf;
    size_t send_len;
    size_t send_cap;

    /* Message ID counter */
    _Atomic uint32_t next_msg_id;

    /* Callbacks */
    qd_ipc_msg_cb_t on_message;
    void *on_message_arg;
    qd_ipc_conn_cb_t on_connect;
    void *on_connect_arg;
    qd_ipc_conn_cb_t on_disconnect;
    void *on_disconnect_arg;

    /* Configuration */
    int connect_timeout_ms;
    int reconnect_interval_ms;
    int auto_reconnect;

    /* Statistics */
    uint64_t msgs_received;
    uint64_t msgs_sent;

    pthread_mutex_t lock;
};

/* Create client */
qd_ipc_client_t *qd_ipc_client_create(void);

/* Create client with configuration */
qd_ipc_client_t *qd_ipc_client_create_ex(const qd_ipc_client_config_t *config);

/* Destroy client */
void qd_ipc_client_destroy(qd_ipc_client_t *client);

/* Connect to server */
int qd_ipc_client_connect(qd_ipc_client_t *client, const char *socket_path);

/* Connect with event loop (non-blocking) */
int qd_ipc_client_connect_async(qd_ipc_client_t *client, const char *socket_path,
                                 qd_event_loop_t *loop);

/* Disconnect */
void qd_ipc_client_disconnect(qd_ipc_client_t *client);

/* Check if connected */
int qd_ipc_client_is_connected(qd_ipc_client_t *client);

/* Send message */
int qd_ipc_client_send(qd_ipc_client_t *client, qd_ipc_msg_t *msg);

/* Send request and wait for response (blocking) */
int qd_ipc_client_request(qd_ipc_client_t *client, const char *service,
                           const void *data, size_t len,
                           void **response, size_t *response_len,
                           int timeout_ms);

/* Send notification (no response expected) */
int qd_ipc_client_notify(qd_ipc_client_t *client, const char *service,
                          const void *data, size_t len);

/* Receive message (blocking) */
int qd_ipc_client_recv(qd_ipc_client_t *client, qd_ipc_msg_t *msg, int timeout_ms);

/* Set callbacks */
void qd_ipc_client_set_message_cb(qd_ipc_client_t *client,
                                   qd_ipc_msg_cb_t callback, void *arg);
void qd_ipc_client_set_connect_cb(qd_ipc_client_t *client,
                                   qd_ipc_conn_cb_t callback, void *arg);
void qd_ipc_client_set_disconnect_cb(qd_ipc_client_t *client,
                                      qd_ipc_conn_cb_t callback, void *arg);

/*
 * IPC Message API
 */

/* Create message */
qd_ipc_msg_t *qd_ipc_msg_create(qd_ipc_msg_type_t type);

/* Create message with payload */
qd_ipc_msg_t *qd_ipc_msg_create_with_payload(qd_ipc_msg_type_t type,
                                               const void *payload, size_t len);

/* Destroy message */
void qd_ipc_msg_destroy(qd_ipc_msg_t *msg);

/* Initialize message (for stack-allocated messages) */
void qd_ipc_msg_init(qd_ipc_msg_t *msg, qd_ipc_msg_type_t type);

/* Set payload */
int qd_ipc_msg_set_payload(qd_ipc_msg_t *msg, const void *payload, size_t len);

/* Get payload */
void *qd_ipc_msg_get_payload(qd_ipc_msg_t *msg, size_t *len);

/* Set service name */
void qd_ipc_msg_set_service(qd_ipc_msg_t *msg, const char *service);

/* Serialize message to buffer (allocates buffer) */
int qd_ipc_msg_serialize(qd_ipc_msg_t *msg, void **buffer, size_t *len);

/* Deserialize message from buffer */
int qd_ipc_msg_deserialize(const void *buffer, size_t len, qd_ipc_msg_t *msg);

/* Validate message header in buffer */
int qd_ipc_validate_header(const void *buffer, size_t len);

/* Get total message size from header */
size_t qd_ipc_msg_total_size(const qd_ipc_header_t *header);

/*
 * Shared Memory Ring Buffer API
 * For high-throughput inter-process communication
 */

/* SHM ring buffer header (stored in shared memory) */
typedef struct qd_shm_ring_header {
    uint32_t magic;
    uint32_t version;
    size_t size;
    _Atomic size_t write_pos;
    _Atomic size_t read_pos;
    _Atomic int writer_waiting;
    _Atomic int reader_waiting;
} qd_shm_ring_header_t;

/* SHM ring buffer */
struct qd_shm_ring {
    char name[64];               /* Shared memory name */
    int fd;                      /* File descriptor */
    void *addr;                  /* Mapped address */
    size_t total_size;           /* Total mapped size */
    qd_shm_ring_header_t *header; /* Header pointer */
    uint8_t *data;               /* Data area pointer */
    size_t data_size;            /* Data area size */
    int is_creator;              /* Whether we created the ring */
};

/* Create shared memory ring buffer */
qd_shm_ring_t *qd_shm_ring_create(const char *name, size_t size);

/* Attach to existing ring buffer */
qd_shm_ring_t *qd_shm_ring_attach(const char *name);

/* Destroy ring buffer */
void qd_shm_ring_destroy(qd_shm_ring_t *ring);

/* Write to ring buffer */
int qd_shm_ring_write(qd_shm_ring_t *ring, const void *data, size_t len);

/* Write with timeout */
int qd_shm_ring_write_timeout(qd_shm_ring_t *ring, const void *data,
                               size_t len, int timeout_ms);

/* Read from ring buffer */
int qd_shm_ring_read(qd_shm_ring_t *ring, void *data, size_t len);

/* Read with timeout */
int qd_shm_ring_read_timeout(qd_shm_ring_t *ring, void *data,
                              size_t len, int timeout_ms);

/* Get available space for writing */
size_t qd_shm_ring_write_available(qd_shm_ring_t *ring);

/* Get available data for reading */
size_t qd_shm_ring_read_available(qd_shm_ring_t *ring);

/* Check if ring is empty */
int qd_shm_ring_is_empty(qd_shm_ring_t *ring);

/* Check if ring is full */
int qd_shm_ring_is_full(qd_shm_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* QD_IPC_H */
