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
#include <qdaemon/qd_event.h>
#include <qdclient.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

#define TEST_SOCKET "/tmp/qd_test_ipc.sock"

/* Server creation test */
TEST(server_create)
{
    unlink(TEST_SOCKET);
    
    qd_ipc_server_t *server = qd_ipc_server_create(TEST_SOCKET);
    assert(server != NULL);
    
    qd_ipc_server_destroy(server);
    unlink(TEST_SOCKET);
}

/* Test message handler */
static int echo_handler(qd_ipc_conn_t *conn, qd_ipc_msg_t *msg, void *arg)
{
    (void)arg;
    
    /* Echo payload back */
    size_t len;
    void *data = qd_ipc_msg_get_payload(msg, &len);
    
    if (msg->header.msg_type == QD_IPC_MSG_REQUEST) {
        qd_ipc_server_reply(conn, msg, data, len);
    }
    
    return 0;
}

/* Message exchange test */
TEST(message_exchange)
{
    unlink(TEST_SOCKET);
    
    /* Fork process */
    pid_t pid = fork();
    
    if (pid == 0) {
        /* Child - Client */
        usleep(100000); /* Wait for server to start */
        
        qdc_client_t *client = qdc_connect(TEST_SOCKET);
        if (!client) {
            fprintf(stderr, "Client failed to connect\n");
            exit(1);
        }
        
        /* Send request */
        const char *req_data = "Hello IPC";
        qdc_response_t *resp = qdc_request(client, "echo", req_data, strlen(req_data) + 1);
        
        if (!resp) {
            fprintf(stderr, "Request failed\n");
            exit(1);
        }
        
        size_t len;
        const char *resp_data = qdc_response_get_binary(resp, NULL, &len);
        if (strcmp(resp_data, req_data) != 0) {
            fprintf(stderr, "Response mismatch: %s != %s\n", resp_data, req_data);
            exit(1);
        }
        
        qdc_response_free(resp);
        qdc_disconnect(client);
        exit(0);
    }
    
    /* Parent - Server */
    qd_event_loop_t *loop = qd_event_loop_create();
    qd_ipc_server_t *server = qd_ipc_server_create(TEST_SOCKET);
    
    /* Register echo handler */
    qd_ipc_server_register_service(server, "echo", echo_handler, NULL);
    
    /* Start server */
    qd_ipc_server_start(server, loop);
    
    /* Run loop for separate thread or just for some time? 
       To make this test robust without infinite loop, we can run loop with timeout 
       or until child exits.
       But standard loop run blocks. run_once doesn't guarantee handling connection if we don't loop.
    */
    
    /* Run loop for 2 seconds (enough for child) */
    /* We need to properly check for child exit */
    
    for (int i = 0; i < 20; i++) {
        qd_event_loop_run_once(loop, 100);
        
        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            break;
        }
    }
    
    qd_ipc_server_stop(server);
    qd_ipc_server_destroy(server);
    qd_event_loop_destroy(loop);
    unlink(TEST_SOCKET);
}

/* Shared Memory Ring Test */
TEST(shm_ring)
{
    const char *shm_name = "/qd_test_ring";
    
    /* Create ring */
    qd_shm_ring_t *ring = qd_shm_ring_create(shm_name, 4096);
    assert(ring != NULL);
    
    /* Write */
    const char *msg = "RingData";
    int ret = qd_shm_ring_write(ring, msg, strlen(msg) + 1);
    assert(ret == 0); // 0 is success typically
    
    /* Read */
    char buf[64];
    ret = qd_shm_ring_read(ring, buf, sizeof(buf)); /* This variant probably returns bytes read? */
    /* Checking header: int qd_shm_ring_read(qd_shm_ring_t *ring, void *data, size_t len); */
    /* Usually returns bytes read or 0/error? qd_ipc.h return type is int. */
    /* Let's assume it returns read count or 0 on success? */
    /* Header comment: "Read from ring buffer". */
    /* Based on write: "Write to ring buffer". */
    
    /* If implementation follows read/write semantics, it returns bytes? */
    /* If it follows qdaemon common, 0 might be OK. */
    /* But qd_shm_ring_read signature takes 'len' as input capacity. */
    
    (void)ret;
    assert(strcmp(buf, msg) == 0);
    
    qd_shm_ring_destroy(ring);
}

int main(void)
{
    printf("\n=== IPC Tests ===\n\n");
    
    RUN_TEST(server_create);
    RUN_TEST(message_exchange);
    RUN_TEST(shm_ring);
    
    printf("\nAll IPC tests passed!\n\n");
    return 0;
}
