/*
 * QDaemon - Multi-Instance IPC Example
 * Demonstrates client-server communication between daemon instances
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <qdaemon/qdaemon.h>
#include <qdclient.h>

static void run_server(void)
{
    printf("Starting server...\n");
    
    qd_daemon_config_t config = QD_DAEMON_CONFIG_DEFAULT;
    config.name = "multi_server";
    config.foreground = 1;
    config.daemonize = 0;
    config.pid_file = "/tmp/multi_server.pid";
    config.socket_path = "/tmp/multi_server.sock";
    config.log_to_stderr = 1;
    config.log_level = QD_LOG_DEBUG;
    
    qd_daemon_t *daemon = qd_daemon_create(&config);
    if (!daemon) {
        fprintf(stderr, "Failed to create daemon\n");
        return;
    }
    
    if (qd_daemon_init(daemon) != QD_OK) {
        fprintf(stderr, "Failed to init daemon\n");
        qd_daemon_destroy(daemon);
        return;
    }
    
    printf("Server running, press Ctrl+C to stop\n");
    qd_daemon_run(daemon);
    
    qd_daemon_destroy(daemon);
}

static void run_client(const char *message)
{
    printf("Connecting to server...\n");
    
    qdc_client_t *client = qdc_connect("/tmp/multi_server.sock");
    if (!client) {
        fprintf(stderr, "Failed to connect to server\n");
        return;
    }
    
    printf("Connected! Sending message: %s\n", message);
    
    qdc_response_t *resp = qdc_request(client, "echo", message, strlen(message) + 1);
    if (resp) {
        printf("Response received: status=%d, len=%zu\n", resp->status, resp->len);
        if (resp->data && resp->len > 0) {
            printf("Data: %.*s\n", (int)resp->len, (char*)resp->data);
        }
        qdc_response_free(resp);
    } else {
        printf("No response received\n");
    }
    
    qdc_disconnect(client);
    printf("Disconnected\n");
}

static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s server          - Run as server\n", prog);
    printf("  %s client <msg>    - Run as client and send message\n", prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    if (strcmp(argv[1], "server") == 0) {
        run_server();
    } else if (strcmp(argv[1], "client") == 0) {
        const char *msg = argc > 2 ? argv[2] : "Hello, Server!";
        run_client(msg);
    } else {
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
