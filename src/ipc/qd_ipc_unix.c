/*
 * QDaemon - Unix Domain Socket IPC Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include "qdaemon/qd_ipc.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

/* Forward declarations */
static void server_accept_callback(int fd, uint32_t events, void *arg);
static void conn_read_callback(int fd, uint32_t events, void *arg);

/*
 * Connection management
 */

static qd_ipc_conn_t *conn_create(qd_ipc_server_t *server, int fd)
{
    qd_ipc_conn_t *conn = calloc(1, sizeof(qd_ipc_conn_t));
    if (!conn)
        return NULL;

    conn->fd = fd;
    conn->state = QD_IPC_CONN_CONNECTED;
    conn->server = server;

    /* Allocate buffers */
    conn->recv_cap = server->recv_buf_size;
    conn->recv_buf = malloc(conn->recv_cap);
    if (!conn->recv_buf) {
        free(conn);
        return NULL;
    }
    conn->recv_len = 0;

    conn->send_cap = server->send_buf_size;
    conn->send_buf = malloc(conn->send_cap);
    if (!conn->send_buf) {
        free(conn->recv_buf);
        free(conn);
        return NULL;
    }
    conn->send_len = 0;

    return conn;
}

static void conn_destroy(qd_ipc_conn_t *conn)
{
    if (!conn)
        return;

    if (conn->fd >= 0)
        close(conn->fd);

    free(conn->recv_buf);
    free(conn->send_buf);
    free(conn);
}

static void conn_list_add(qd_ipc_server_t *server, qd_ipc_conn_t *conn)
{
    conn->next = server->connections;
    conn->prev = NULL;
    if (server->connections)
        server->connections->prev = conn;
    server->connections = conn;
    server->num_connections++;
}

static void conn_list_remove(qd_ipc_server_t *server, qd_ipc_conn_t *conn)
{
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        server->connections = conn->next;
    if (conn->next)
        conn->next->prev = conn->prev;
    server->num_connections--;
}

/*
 * Server implementation
 */

qd_ipc_server_t *qd_ipc_server_create(const char *socket_path)
{
    qd_ipc_server_config_t config = QD_IPC_SERVER_CONFIG_DEFAULT;
    config.socket_path = socket_path;
    return qd_ipc_server_create_ex(&config);
}

qd_ipc_server_t *qd_ipc_server_create_ex(const qd_ipc_server_config_t *config)
{
    if (!config || !config->socket_path)
        return NULL;

    qd_ipc_server_t *server = calloc(1, sizeof(qd_ipc_server_t));
    if (!server)
        return NULL;

    server->listen_fd = -1;
    strncpy(server->socket_path, config->socket_path, sizeof(server->socket_path) - 1);
    server->max_connections = config->max_clients;
    server->max_msg_size = config->max_msg_size;
    server->recv_buf_size = config->recv_buf_size;
    server->send_buf_size = config->send_buf_size;

    pthread_mutex_init(&server->lock, NULL);

    /* Create socket */
    server->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server->listen_fd == -1) {
        free(server);
        return NULL;
    }

    /* Bind to path */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->socket_path, sizeof(addr.sun_path) - 1);

    /* Remove existing socket file */
    unlink(config->socket_path);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    /* Listen */
    if (listen(server->listen_fd, config->backlog) == -1) {
        close(server->listen_fd);
        unlink(config->socket_path);
        free(server);
        return NULL;
    }

    return server;
}

void qd_ipc_server_destroy(qd_ipc_server_t *server)
{
    if (!server)
        return;

    qd_ipc_server_stop(server);

    /* Close all connections */
    pthread_mutex_lock(&server->lock);
    qd_ipc_conn_t *conn = server->connections;
    while (conn) {
        qd_ipc_conn_t *next = conn->next;
        conn_destroy(conn);
        conn = next;
    }
    server->connections = NULL;
    pthread_mutex_unlock(&server->lock);

    /* Free services */
    qd_ipc_service_t *svc = server->services;
    while (svc) {
        qd_ipc_service_t *next = svc->next;
        free(svc);
        svc = next;
    }

    /* Close listen socket */
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        unlink(server->socket_path);
    }

    pthread_mutex_destroy(&server->lock);
    free(server);
}

static void server_accept_callback(int fd, uint32_t events, void *arg)
{
    QD_UNUSED(events);
    qd_ipc_server_t *server = arg;

    while (1) {
        struct sockaddr_un addr;
        socklen_t addrlen = sizeof(addr);

        int client_fd = accept4(fd, (struct sockaddr *)&addr, &addrlen,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return;
        }

        pthread_mutex_lock(&server->lock);

        if (server->num_connections >= server->max_connections) {
            close(client_fd);
            pthread_mutex_unlock(&server->lock);
            continue;
        }

        qd_ipc_conn_t *conn = conn_create(server, client_fd);
        if (!conn) {
            close(client_fd);
            pthread_mutex_unlock(&server->lock);
            continue;
        }

        conn_list_add(server, conn);
        server->total_connections++;

        pthread_mutex_unlock(&server->lock);

        /* Add to event loop */
        if (server->loop) {
            qd_event_add(server->loop, client_fd, QD_EVENT_READ | QD_EVENT_EDGE,
                         conn_read_callback, conn);
        }

        /* Call connect callback */
        if (server->on_connect)
            server->on_connect(conn, server->on_connect_arg);
    }
}

static int process_message(qd_ipc_conn_t *conn, qd_ipc_msg_t *msg)
{
    qd_ipc_server_t *server = conn->server;

    /* Call type-specific handler */
    int msg_type = msg->header.msg_type;
    if (msg_type > 0 && msg_type < 16) {
        qd_ipc_handler_t handler = server->handlers[msg_type];
        if (handler) {
            return handler(conn, msg, server->handler_args[msg_type]);
        }
    }

    /* Check for service handler */
    if (msg->service[0] != '\0') {
        pthread_mutex_lock(&server->lock);
        qd_ipc_service_t *svc = server->services;
        while (svc) {
            if (strcmp(svc->name, msg->service) == 0) {
                pthread_mutex_unlock(&server->lock);
                return svc->handler(conn, msg, svc->arg);
            }
            svc = svc->next;
        }
        pthread_mutex_unlock(&server->lock);
    }

    return QD_OK;
}

static void conn_read_callback(int fd, uint32_t events, void *arg)
{
    qd_ipc_conn_t *conn = arg;
    qd_ipc_server_t *server = conn->server;

    if (events & (QD_EVENT_ERROR | QD_EVENT_HANGUP)) {
        qd_ipc_server_disconnect(conn);
        return;
    }

    /* Read data */
    while (1) {
        size_t space = conn->recv_cap - conn->recv_len;
        if (space == 0) {
            /* Buffer full, need to process */
            break;
        }

        ssize_t n = read(fd, conn->recv_buf + conn->recv_len, space);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            qd_ipc_server_disconnect(conn);
            return;
        }

        if (n == 0) {
            /* EOF */
            qd_ipc_server_disconnect(conn);
            return;
        }

        conn->recv_len += n;
        conn->bytes_received += n;
    }

    /* Process messages in buffer */
    while (conn->recv_len >= sizeof(qd_ipc_header_t)) {
        /* Check header */
        qd_ipc_header_t *header = (qd_ipc_header_t *)conn->recv_buf;

        if (header->magic != QD_IPC_MAGIC) {
            /* Protocol error */
            qd_ipc_server_disconnect(conn);
            return;
        }

        size_t msg_size = sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN + header->payload_len;

        if (conn->recv_len < msg_size) {
            /* Incomplete message */
            break;
        }

        /* Deserialize message */
        qd_ipc_msg_t msg;
        memset(&msg, 0, sizeof(msg));

        if (qd_ipc_msg_deserialize(conn->recv_buf, msg_size, &msg) != QD_OK) {
            qd_ipc_server_disconnect(conn);
            return;
        }

        conn->msgs_received++;
        server->total_messages++;

        /* Process message */
        process_message(conn, &msg);

        /* Free message payload */
        if (msg.owned && msg.payload)
            free(msg.payload);

        /* Move remaining data to front */
        if (conn->recv_len > msg_size) {
            memmove(conn->recv_buf, conn->recv_buf + msg_size, conn->recv_len - msg_size);
        }
        conn->recv_len -= msg_size;
    }
}

int qd_ipc_server_start(qd_ipc_server_t *server, qd_event_loop_t *loop)
{
    if (!server || !loop)
        return QD_ERR_INVAL;

    server->loop = loop;

    /* Add listen socket to event loop */
    if (qd_event_add(loop, server->listen_fd, QD_EVENT_READ,
                     server_accept_callback, server) == NULL) {
        return QD_ERR_IO;
    }

    return QD_OK;
}

void qd_ipc_server_stop(qd_ipc_server_t *server)
{
    if (!server)
        return;

    if (server->loop) {
        qd_event_del_fd(server->loop, server->listen_fd);
        server->loop = NULL;
    }
}

void qd_ipc_server_set_handler(qd_ipc_server_t *server, qd_ipc_msg_type_t type,
                                qd_ipc_handler_t handler, void *arg)
{
    if (!server || type <= 0 || type >= 16)
        return;

    server->handlers[type] = handler;
    server->handler_args[type] = arg;
}

void qd_ipc_server_set_connect_cb(qd_ipc_server_t *server,
                                   qd_ipc_conn_cb_t callback, void *arg)
{
    if (!server)
        return;
    server->on_connect = callback;
    server->on_connect_arg = arg;
}

void qd_ipc_server_set_disconnect_cb(qd_ipc_server_t *server,
                                      qd_ipc_conn_cb_t callback, void *arg)
{
    if (!server)
        return;
    server->on_disconnect = callback;
    server->on_disconnect_arg = arg;
}

int qd_ipc_server_register_service(qd_ipc_server_t *server, const char *name,
                                    qd_ipc_handler_t handler, void *arg)
{
    if (!server || !name || !handler)
        return QD_ERR_INVAL;

    qd_ipc_service_t *svc = calloc(1, sizeof(qd_ipc_service_t));
    if (!svc)
        return QD_ERR_NOMEM;

    strncpy(svc->name, name, QD_IPC_MAX_NAME_LEN - 1);
    svc->handler = handler;
    svc->arg = arg;

    pthread_mutex_lock(&server->lock);
    svc->next = server->services;
    server->services = svc;
    pthread_mutex_unlock(&server->lock);

    return QD_OK;
}

int qd_ipc_server_unregister_service(qd_ipc_server_t *server, const char *name)
{
    if (!server || !name)
        return QD_ERR_INVAL;

    pthread_mutex_lock(&server->lock);
    qd_ipc_service_t **pp = &server->services;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            qd_ipc_service_t *svc = *pp;
            *pp = svc->next;
            free(svc);
            pthread_mutex_unlock(&server->lock);
            return QD_OK;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&server->lock);

    return QD_ERR_NOENT;
}

int qd_ipc_server_send(qd_ipc_conn_t *conn, qd_ipc_msg_t *msg)
{
    if (!conn || !msg)
        return QD_ERR_INVAL;

    void *buffer;
    size_t len;

    if (qd_ipc_msg_serialize(msg, &buffer, &len) != QD_OK)
        return QD_ERR_NOMEM;

    /* Send data */
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(conn->fd, (uint8_t *)buffer + sent, len - sent);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Would block - could buffer for later */
                break;
            }
            free(buffer);
            return QD_ERR_IO;
        }
        sent += n;
    }

    conn->msgs_sent++;
    conn->bytes_sent += sent;

    free(buffer);
    return QD_OK;
}

int qd_ipc_server_reply(qd_ipc_conn_t *conn, qd_ipc_msg_t *request,
                         const void *data, size_t len)
{
    if (!conn || !request)
        return QD_ERR_INVAL;

    qd_ipc_msg_t response;
    qd_ipc_msg_init(&response, QD_IPC_MSG_RESPONSE);
    response.header.msg_id = request->header.msg_id;

    if (data && len > 0) {
        if (qd_ipc_msg_set_payload(&response, data, len) != QD_OK)
            return QD_ERR_NOMEM;
    }

    int ret = qd_ipc_server_send(conn, &response);

    if (response.owned && response.payload)
        free(response.payload);

    return ret;
}

int qd_ipc_server_broadcast(qd_ipc_server_t *server, qd_ipc_msg_t *msg)
{
    if (!server || !msg)
        return QD_ERR_INVAL;

    pthread_mutex_lock(&server->lock);
    qd_ipc_conn_t *conn = server->connections;
    while (conn) {
        qd_ipc_server_send(conn, msg);
        conn = conn->next;
    }
    pthread_mutex_unlock(&server->lock);

    return QD_OK;
}

void qd_ipc_server_disconnect(qd_ipc_conn_t *conn)
{
    if (!conn)
        return;

    qd_ipc_server_t *server = conn->server;

    /* Call disconnect callback */
    if (server->on_disconnect)
        server->on_disconnect(conn, server->on_disconnect_arg);

    /* Remove from event loop */
    if (server->loop)
        qd_event_del_fd(server->loop, conn->fd);

    /* Remove from list and destroy */
    pthread_mutex_lock(&server->lock);
    conn_list_remove(server, conn);
    pthread_mutex_unlock(&server->lock);

    conn_destroy(conn);
}

/*
 * Client implementation
 */

qd_ipc_client_t *qd_ipc_client_create(void)
{
    qd_ipc_client_config_t config = QD_IPC_CLIENT_CONFIG_DEFAULT;
    return qd_ipc_client_create_ex(&config);
}

qd_ipc_client_t *qd_ipc_client_create_ex(const qd_ipc_client_config_t *config)
{
    if (!config)
        return NULL;

    qd_ipc_client_t *client = calloc(1, sizeof(qd_ipc_client_t));
    if (!client)
        return NULL;

    client->fd = -1;
    client->state = QD_IPC_CONN_DISCONNECTED;

    if (config->name)
        strncpy(client->name, config->name, QD_IPC_MAX_NAME_LEN - 1);

    client->recv_cap = config->recv_buf_size;
    client->recv_buf = malloc(client->recv_cap);
    if (!client->recv_buf) {
        free(client);
        return NULL;
    }

    client->send_cap = config->send_buf_size;
    client->send_buf = malloc(client->send_cap);
    if (!client->send_buf) {
        free(client->recv_buf);
        free(client);
        return NULL;
    }

    client->connect_timeout_ms = config->connect_timeout_ms;
    client->reconnect_interval_ms = config->reconnect_interval_ms;
    client->auto_reconnect = config->auto_reconnect;

    qd_atomic_init(&client->next_msg_id, 1);
    pthread_mutex_init(&client->lock, NULL);

    return client;
}

void qd_ipc_client_destroy(qd_ipc_client_t *client)
{
    if (!client)
        return;

    qd_ipc_client_disconnect(client);

    free(client->recv_buf);
    free(client->send_buf);
    pthread_mutex_destroy(&client->lock);
    free(client);
}

int qd_ipc_client_connect(qd_ipc_client_t *client, const char *socket_path)
{
    if (!client || !socket_path)
        return QD_ERR_INVAL;

    if (client->state == QD_IPC_CONN_CONNECTED)
        return QD_OK;

    /* Create socket */
    client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->fd == -1)
        return QD_ERR_IO;

    qd_set_cloexec(client->fd);

    /* Connect */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(client->fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(client->fd);
        client->fd = -1;
        return QD_ERR_CONN;
    }

    strncpy(client->socket_path, socket_path, sizeof(client->socket_path) - 1);
    client->state = QD_IPC_CONN_CONNECTED;

    /* Call connect callback */
    if (client->on_connect)
        client->on_connect((qd_ipc_conn_t *)client, client->on_connect_arg);

    return QD_OK;
}

int qd_ipc_client_connect_async(qd_ipc_client_t *client, const char *socket_path,
                                 qd_event_loop_t *loop)
{
    if (!client || !socket_path || !loop)
        return QD_ERR_INVAL;

    /* Create non-blocking socket */
    client->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (client->fd == -1)
        return QD_ERR_IO;

    qd_set_cloexec(client->fd);

    /* Connect */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    int ret = connect(client->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1 && errno != EINPROGRESS) {
        close(client->fd);
        client->fd = -1;
        return QD_ERR_CONN;
    }

    strncpy(client->socket_path, socket_path, sizeof(client->socket_path) - 1);
    client->state = QD_IPC_CONN_CONNECTING;
    client->loop = loop;

    return QD_OK;
}

void qd_ipc_client_disconnect(qd_ipc_client_t *client)
{
    if (!client)
        return;

    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }

    client->state = QD_IPC_CONN_DISCONNECTED;

    if (client->on_disconnect)
        client->on_disconnect((qd_ipc_conn_t *)client, client->on_disconnect_arg);
}

int qd_ipc_client_is_connected(qd_ipc_client_t *client)
{
    return client && client->state == QD_IPC_CONN_CONNECTED;
}

int qd_ipc_client_send(qd_ipc_client_t *client, qd_ipc_msg_t *msg)
{
    if (!client || !msg)
        return QD_ERR_INVAL;

    if (client->state != QD_IPC_CONN_CONNECTED)
        return QD_ERR_CONN;

    void *buffer;
    size_t len;

    if (qd_ipc_msg_serialize(msg, &buffer, &len) != QD_OK)
        return QD_ERR_NOMEM;

    pthread_mutex_lock(&client->lock);

    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(client->fd, (uint8_t *)buffer + sent, len - sent);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Poll for write ready */
                struct pollfd pfd = { .fd = client->fd, .events = POLLOUT };
                poll(&pfd, 1, 1000);
                continue;
            }
            pthread_mutex_unlock(&client->lock);
            free(buffer);
            return QD_ERR_IO;
        }
        sent += n;
    }

    client->msgs_sent++;
    pthread_mutex_unlock(&client->lock);

    free(buffer);
    return QD_OK;
}

int qd_ipc_client_request(qd_ipc_client_t *client, const char *service,
                           const void *data, size_t len,
                           void **response, size_t *response_len,
                           int timeout_ms)
{
    if (!client || !service)
        return QD_ERR_INVAL;

    /* Build request */
    qd_ipc_msg_t msg;
    qd_ipc_msg_init(&msg, QD_IPC_MSG_REQUEST);
    msg.header.msg_id = qd_atomic_fetch_add(&client->next_msg_id, 1);
    msg.header.flags = QD_IPC_FLAG_REPLY_REQ;
    qd_ipc_msg_set_service(&msg, service);

    if (data && len > 0) {
        if (qd_ipc_msg_set_payload(&msg, data, len) != QD_OK)
            return QD_ERR_NOMEM;
    }

    /* Send request */
    int ret = qd_ipc_client_send(client, &msg);
    uint32_t msg_id = msg.header.msg_id;

    if (msg.owned && msg.payload)
        free(msg.payload);

    if (ret != QD_OK)
        return ret;

    /* Wait for response */
    qd_ipc_msg_t resp;
    ret = qd_ipc_client_recv(client, &resp, timeout_ms);
    if (ret != QD_OK)
        return ret;

    /* Check message ID matches */
    if (resp.header.msg_id != msg_id) {
        if (resp.owned && resp.payload)
            free(resp.payload);
        return QD_ERR_PROTO;
    }

    /* Return response payload */
    if (response && response_len) {
        *response = resp.payload;
        *response_len = resp.header.payload_len;
        resp.owned = 0;  /* Transfer ownership */
    } else if (resp.owned && resp.payload) {
        free(resp.payload);
    }

    return QD_OK;
}

int qd_ipc_client_notify(qd_ipc_client_t *client, const char *service,
                          const void *data, size_t len)
{
    if (!client || !service)
        return QD_ERR_INVAL;

    qd_ipc_msg_t msg;
    qd_ipc_msg_init(&msg, QD_IPC_MSG_NOTIFY);
    qd_ipc_msg_set_service(&msg, service);

    if (data && len > 0) {
        if (qd_ipc_msg_set_payload(&msg, data, len) != QD_OK)
            return QD_ERR_NOMEM;
    }

    int ret = qd_ipc_client_send(client, &msg);

    if (msg.owned && msg.payload)
        free(msg.payload);

    return ret;
}

int qd_ipc_client_recv(qd_ipc_client_t *client, qd_ipc_msg_t *msg, int timeout_ms)
{
    if (!client || !msg)
        return QD_ERR_INVAL;

    if (client->state != QD_IPC_CONN_CONNECTED)
        return QD_ERR_CONN;

    memset(msg, 0, sizeof(*msg));

    qd_time_t start = qd_time_now();

    /* Read until we have a complete message */
    while (1) {
        /* Check for timeout */
        if (timeout_ms >= 0) {
            qd_time_t elapsed = qd_time_now() - start;
            if (elapsed >= (qd_time_t)timeout_ms)
                return QD_ERR_TIMEOUT;

            /* Poll for data */
            struct pollfd pfd = { .fd = client->fd, .events = POLLIN };
            int ret = poll(&pfd, 1, timeout_ms - (int)elapsed);
            if (ret == 0)
                return QD_ERR_TIMEOUT;
            if (ret < 0)
                return QD_ERR_IO;
        }

        /* Read data */
        size_t space = client->recv_cap - client->recv_len;
        if (space > 0) {
            ssize_t n = read(client->fd, client->recv_buf + client->recv_len, space);
            if (n == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                return QD_ERR_IO;
            }
            if (n == 0)
                return QD_ERR_CONN;

            client->recv_len += n;
        }

        /* Check if we have a complete message */
        if (client->recv_len >= sizeof(qd_ipc_header_t)) {
            qd_ipc_header_t *header = (qd_ipc_header_t *)client->recv_buf;

            if (header->magic != QD_IPC_MAGIC)
                return QD_ERR_PROTO;

            size_t msg_size = sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN + header->payload_len;

            if (client->recv_len >= msg_size) {
                /* Deserialize message */
                int ret = qd_ipc_msg_deserialize(client->recv_buf, msg_size, msg);
                if (ret != QD_OK)
                    return ret;

                client->msgs_received++;

                /* Move remaining data */
                if (client->recv_len > msg_size) {
                    memmove(client->recv_buf, client->recv_buf + msg_size,
                            client->recv_len - msg_size);
                }
                client->recv_len -= msg_size;

                return QD_OK;
            }
        }
    }
}

void qd_ipc_client_set_message_cb(qd_ipc_client_t *client,
                                   qd_ipc_msg_cb_t callback, void *arg)
{
    if (!client)
        return;
    client->on_message = callback;
    client->on_message_arg = arg;
}

void qd_ipc_client_set_connect_cb(qd_ipc_client_t *client,
                                   qd_ipc_conn_cb_t callback, void *arg)
{
    if (!client)
        return;
    client->on_connect = callback;
    client->on_connect_arg = arg;
}

void qd_ipc_client_set_disconnect_cb(qd_ipc_client_t *client,
                                      qd_ipc_conn_cb_t callback, void *arg)
{
    if (!client)
        return;
    client->on_disconnect = callback;
    client->on_disconnect_arg = arg;
}

/* Message serialization helpers - declared in header, implemented in protocol.c */
extern int qd_ipc_msg_serialize(qd_ipc_msg_t *msg, void **buffer, size_t *len);
extern int qd_ipc_msg_deserialize(const void *buffer, size_t len, qd_ipc_msg_t *msg);
