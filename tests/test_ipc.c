/*
 * QDaemon IPC Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include <qdaemon/qd_ipc.h>
#include <qdclient.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

#define TEST_SOCKET "/tmp/qd_test_ipc.sock"

/* Test Unix socket server creation */
TEST(unix_server_create)
{
    unlink(TEST_SOCKET);
    
    qd_ipc_unix_t *server = qd_ipc_unix_create(TEST_SOCKET, 0);
    assert(server != NULL);
    
    qd_ipc_unix_destroy(server);
    unlink(TEST_SOCKET);
}

/* Test client connection */
TEST(client_connect)
{
    unlink(TEST_SOCKET);
    
    qd_ipc_unix_t *server = qd_ipc_unix_create(TEST_SOCKET, 0);
    assert(server != NULL);
    
    int listen_fd = qd_ipc_unix_get_fd(server);
    assert(listen_fd >= 0);
    
    /* Fork child to connect */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child - client */
        usleep(10000); /* Wait for server */
        
        qdc_client_t *client = qdc_connect(TEST_SOCKET);
        if (!client) {
            _exit(1);
        }
        qdc_disconnect(client);
        _exit(0);
    }
    
    /* Parent - accept connection */
    int client_fd = qd_ipc_unix_accept(server, 1000);
    assert(client_fd >= 0);
    close(client_fd);
    
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    
    qd_ipc_unix_destroy(server);
    unlink(TEST_SOCKET);
}

/* Test message exchange */
TEST(message_exchange)
{
    unlink(TEST_SOCKET);
    
    qd_ipc_unix_t *server = qd_ipc_unix_create(TEST_SOCKET, 0);
    assert(server != NULL);
    
    pid_t pid = fork();
    if (pid == 0) {
        /* Child - client */
        usleep(10000);
        
        qdc_client_t *client = qdc_connect(TEST_SOCKET);
        if (!client) _exit(1);
        
        /* Build and send request */
        qdc_message_t *msg = qdc_message_create();
        qdc_message_add_string(msg, "key", "test_value");
        
        const void *data;
        size_t len;
        qdc_message_serialize(msg, &data, &len);
        
        qdc_response_t *resp = qdc_request(client, "echo", data, len);
        
        qdc_message_destroy(msg);
        qdc_response_free(resp);
        qdc_disconnect(client);
        _exit(0);
    }
    
    /* Parent - handle connection */
    int client_fd = qd_ipc_unix_accept(server, 1000);
    assert(client_fd >= 0);
    
    /* Read some data */
    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    assert(n > 0);
    
    /* Write response */
    write(client_fd, buf, n);
    
    close(client_fd);
    
    int status;
    waitpid(pid, &status, 0);
    
    qd_ipc_unix_destroy(server);
    unlink(TEST_SOCKET);
}

/* Test shared memory */
TEST(shm_basic)
{
    const char *shm_name = "/qd_test_shm";
    
    qd_ipc_shm_t *shm = qd_ipc_shm_create(shm_name, 4096, 0);
    assert(shm != NULL);
    
    void *ptr = qd_ipc_shm_get_ptr(shm);
    assert(ptr != NULL);
    
    /* Write to shared memory */
    memset(ptr, 0xAB, 100);
    
    qd_ipc_shm_destroy(shm);
    shm_unlink(shm_name);
}

/* Test shared memory ring buffer */
TEST(shm_ring)
{
    const char *shm_name = "/qd_test_ring";
    
    qd_ipc_shm_t *shm = qd_ipc_shm_create(shm_name, 8192, QD_SHM_RING);
    assert(shm != NULL);
    
    /* Write to ring */
    const char *msg = "Hello, Ring!";
    int ret = qd_ipc_shm_write(shm, msg, strlen(msg) + 1);
    assert(ret == QD_OK);
    
    /* Read from ring */
    char buf[64];
    size_t len = sizeof(buf);
    ret = qd_ipc_shm_read(shm, buf, &len);
    assert(ret == QD_OK);
    assert(strcmp(buf, msg) == 0);
    
    qd_ipc_shm_destroy(shm);
    shm_unlink(shm_name);
}

/* Test message building */
TEST(message_builder)
{
    qdc_message_t *msg = qdc_message_create();
    assert(msg != NULL);
    
    qdc_message_add_string(msg, "name", "test");
    qdc_message_add_int(msg, "count", 42);
    qdc_message_add_float(msg, "value", 3.14);
    qdc_message_add_bool(msg, "flag", 1);
    
    const void *data;
    size_t len;
    int ret = qdc_message_serialize(msg, &data, &len);
    assert(ret == QDC_OK);
    assert(len > 0);
    
    qdc_message_destroy(msg);
}

/* Test protocol framing */
TEST(protocol_frame)
{
    qd_ipc_msg_t msg = {
        .type = QD_IPC_MSG_REQUEST,
        .id = 123,
        .len = 5,
    };
    
    char frame[64];
    size_t frame_len = qd_ipc_protocol_encode(&msg, "hello", frame, sizeof(frame));
    assert(frame_len > 0);
    
    qd_ipc_msg_t decoded;
    char payload[32];
    int ret = qd_ipc_protocol_decode(frame, frame_len, &decoded, payload, sizeof(payload));
    assert(ret == QD_OK);
    assert(decoded.type == QD_IPC_MSG_REQUEST);
    assert(decoded.id == 123);
    assert(decoded.len == 5);
    assert(memcmp(payload, "hello", 5) == 0);
}

int main(void)
{
    printf("\n=== IPC Tests ===\n\n");
    
    RUN_TEST(unix_server_create);
    RUN_TEST(client_connect);
    RUN_TEST(message_exchange);
    RUN_TEST(shm_basic);
    RUN_TEST(shm_ring);
    RUN_TEST(message_builder);
    RUN_TEST(protocol_frame);
    
    printf("\nAll IPC tests passed!\n\n");
    return 0;
}
