/*
 * QDaemon - Kernel Communication Example
 * Demonstrates netlink communication with kernel module
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qdaemon/qdaemon.h>
#include <qdaemon/qd_netlink.h>

int main(int argc, char **argv)
{
    const char *message = "Hello, Kernel!";
    
    if (argc > 1) {
        message = argv[1];
    }
    
    printf("QDaemon Kernel Communication Example\n");
    printf("=====================================\n\n");
    
    /* Create netlink connection */
    qd_netlink_config_t config = {
        .family_name = "QDAEMON",
        .recv_buf_size = 4096,
    };
    
    qd_netlink_t *nl = qd_netlink_create_ex(&config);
    if (!nl) {
        fprintf(stderr, "Failed to create netlink connection.\n");
        fprintf(stderr, "Make sure the kernel module is loaded:\n");
        fprintf(stderr, "  sudo insmod kernel/qd_kmod.ko\n");
        return 1;
    }
    
    printf("Connected to kernel module\n\n");
    
    /* Echo test */
    printf("Sending echo request: \"%s\"\n", message);
    
    char response[256] = {0};
    int ret = qd_netlink_echo(nl, message, response, sizeof(response));
    
    if (ret == QD_OK) {
        printf("Received echo response: \"%s\"\n\n", response);
    } else {
        printf("Echo request failed: %d\n\n", ret);
    }
    
    /* Get info */
    printf("Requesting kernel module info...\n");
    
    char info[256] = {0};
    ret = qd_netlink_get_info(nl, info, sizeof(info));
    
    if (ret == QD_OK) {
        printf("Module info: %s\n", info);
    } else {
        printf("Info request failed: %d\n", ret);
    }
    
    /* Cleanup */
    qd_netlink_destroy(nl);
    
    printf("\nDone!\n");
    return 0;
}
