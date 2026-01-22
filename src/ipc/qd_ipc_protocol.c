/*
 * QDaemon - IPC Protocol Implementation
 * Message serialization and deserialization
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "qdaemon/qd_ipc.h"
#include "qdaemon/qd_memory.h"

qd_ipc_msg_t *qd_ipc_msg_create(qd_ipc_msg_type_t type)
{
    qd_ipc_msg_t *msg = calloc(1, sizeof(qd_ipc_msg_t));
    if (!msg)
        return NULL;

    qd_ipc_msg_init(msg, type);
    return msg;
}

qd_ipc_msg_t *qd_ipc_msg_create_with_payload(qd_ipc_msg_type_t type,
                                               const void *payload, size_t len)
{
    qd_ipc_msg_t *msg = qd_ipc_msg_create(type);
    if (!msg)
        return NULL;

    if (payload && len > 0) {
        if (qd_ipc_msg_set_payload(msg, payload, len) != 0) {
            qd_ipc_msg_destroy(msg);
            return NULL;
        }
    }

    return msg;
}

void qd_ipc_msg_destroy(qd_ipc_msg_t *msg)
{
    if (!msg)
        return;

    if (msg->owned && msg->payload)
        free(msg->payload);

    free(msg);
}

void qd_ipc_msg_init(qd_ipc_msg_t *msg, qd_ipc_msg_type_t type)
{
    if (!msg)
        return;

    memset(msg, 0, sizeof(*msg));

    msg->header.magic = QD_IPC_MAGIC;
    msg->header.version = QD_IPC_VERSION;
    msg->header.msg_type = type;
    msg->header.msg_id = 0;
    msg->header.payload_len = 0;
    msg->header.flags = 0;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    msg->header.timestamp = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int qd_ipc_msg_set_payload(qd_ipc_msg_t *msg, const void *payload, size_t len)
{
    if (!msg)
        return QD_ERR_INVAL;

    if (len > QD_IPC_MAX_MSG_SIZE)
        return QD_ERR_INVAL;

    /* Free existing payload if owned */
    if (msg->owned && msg->payload)
        free(msg->payload);

    if (len > 0) {
        msg->payload = malloc(len);
        if (!msg->payload)
            return QD_ERR_NOMEM;

        memcpy(msg->payload, payload, len);
        msg->payload_cap = len;
    } else {
        msg->payload = NULL;
        msg->payload_cap = 0;
    }

    msg->header.payload_len = len;
    msg->owned = 1;

    return QD_OK;
}

void *qd_ipc_msg_get_payload(qd_ipc_msg_t *msg, size_t *len)
{
    if (!msg)
        return NULL;

    if (len)
        *len = msg->header.payload_len;

    return msg->payload;
}

void qd_ipc_msg_set_service(qd_ipc_msg_t *msg, const char *service)
{
    if (!msg || !service)
        return;

    strncpy(msg->service, service, QD_IPC_MAX_NAME_LEN - 1);
    msg->service[QD_IPC_MAX_NAME_LEN - 1] = '\0';
}

/*
 * Message serialization
 */

/* Serialize message to buffer */
int qd_ipc_msg_serialize(qd_ipc_msg_t *msg, void **buffer, size_t *len)
{
    if (!msg || !buffer || !len)
        return QD_ERR_INVAL;

    size_t total_len = sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN + msg->header.payload_len;

    uint8_t *buf = malloc(total_len);
    if (!buf)
        return QD_ERR_NOMEM;

    /* Copy header */
    memcpy(buf, &msg->header, sizeof(qd_ipc_header_t));

    /* Copy service name */
    memcpy(buf + sizeof(qd_ipc_header_t), msg->service, QD_IPC_MAX_NAME_LEN);

    /* Copy payload */
    if (msg->payload && msg->header.payload_len > 0) {
        memcpy(buf + sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN,
               msg->payload, msg->header.payload_len);
    }

    *buffer = buf;
    *len = total_len;

    return QD_OK;
}

/* Deserialize message from buffer */
int qd_ipc_msg_deserialize(const void *buffer, size_t len, qd_ipc_msg_t *msg)
{
    if (!buffer || !msg)
        return QD_ERR_INVAL;

    size_t min_len = sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN;
    if (len < min_len)
        return QD_ERR_PROTO;

    const uint8_t *buf = buffer;

    /* Copy header */
    memcpy(&msg->header, buf, sizeof(qd_ipc_header_t));

    /* Validate header */
    if (msg->header.magic != QD_IPC_MAGIC)
        return QD_ERR_PROTO;

    if (msg->header.version != QD_IPC_VERSION)
        return QD_ERR_PROTO;

    if (msg->header.payload_len > QD_IPC_MAX_MSG_SIZE)
        return QD_ERR_PROTO;

    /* Check total length */
    if (len < min_len + msg->header.payload_len)
        return QD_ERR_PROTO;

    /* Copy service name */
    memcpy(msg->service, buf + sizeof(qd_ipc_header_t), QD_IPC_MAX_NAME_LEN);
    msg->service[QD_IPC_MAX_NAME_LEN - 1] = '\0';

    /* Copy payload */
    if (msg->header.payload_len > 0) {
        msg->payload = malloc(msg->header.payload_len);
        if (!msg->payload)
            return QD_ERR_NOMEM;

        memcpy(msg->payload, buf + min_len, msg->header.payload_len);
        msg->payload_cap = msg->header.payload_len;
        msg->owned = 1;
    }

    return QD_OK;
}

/* Validate message header in buffer */
int qd_ipc_validate_header(const void *buffer, size_t len)
{
    if (len < sizeof(qd_ipc_header_t))
        return QD_ERR_PROTO;

    const qd_ipc_header_t *header = buffer;

    if (header->magic != QD_IPC_MAGIC)
        return QD_ERR_PROTO;

    if (header->version != QD_IPC_VERSION)
        return QD_ERR_PROTO;

    if (header->payload_len > QD_IPC_MAX_MSG_SIZE)
        return QD_ERR_PROTO;

    return QD_OK;
}

/* Get total message size from header */
size_t qd_ipc_msg_total_size(const qd_ipc_header_t *header)
{
    return sizeof(qd_ipc_header_t) + QD_IPC_MAX_NAME_LEN + header->payload_len;
}
