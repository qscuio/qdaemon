/*
 * Test Raw Socket Communication with DHCP Kernel Module
 *
 * This test program demonstrates how to:
 * 1. Create a raw socket bound to qdhcp0
 * 2. Receive packets with metadata headers from the kernel
 * 3. Send packets with metadata headers back to kernel
 *
 * Usage:
 *   1. Load kernel module: sudo insmod kernel/qd_dhcp_kmod.ko
 *   2. Add interface to monitor: (via netlink or ip link)
 *   3. Run this test: sudo ./test_rawsock
 *   4. Send DHCP packet to monitored interface
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#include "include/dhcp_kmod.h"
#include "include/dhcp_protocol.h"

static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void dump_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && i < 64; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0)
            printf("\n    ");
    }
    if (len > 64)
        printf("... (%zu more bytes)", len - 64);
    printf("\n");
}

static const char *dhcp_type_str(uint8_t type)
{
    switch (type) {
        case DHCP_TYPE_DISCOVER: return "DISCOVER";
        case DHCP_TYPE_OFFER:    return "OFFER";
        case DHCP_TYPE_REQUEST:  return "REQUEST";
        case DHCP_TYPE_DECLINE:  return "DECLINE";
        case DHCP_TYPE_ACK:      return "ACK";
        case DHCP_TYPE_NAK:      return "NAK";
        case DHCP_TYPE_RELEASE:  return "RELEASE";
        case DHCP_TYPE_INFORM:   return "INFORM";
        default:                 return "UNKNOWN";
    }
}

static const char *method_str(uint8_t method)
{
    switch (method) {
        case QDHCP_METHOD_DEV_ADD_PACK: return "dev_add_pack";
        case QDHCP_METHOD_TC_INGRESS:   return "tc_ingress";
        case QDHCP_METHOD_BOTH:         return "both";
        default:                        return "unknown";
    }
}

int main(void)
{
    int sock;
    struct ifreq ifr;
    struct sockaddr_ll sll;
    uint8_t buf[QDHCP_MAX_PKT_SIZE];

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("=== QDHCP Raw Socket Test ===\n\n");

    /* Create raw socket */
    sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    /* Get interface index for qdhcp0 */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, QDHCP_DEV_NAME, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl SIOCGIFINDEX");
        printf("\nMake sure to load the kernel module first:\n");
        printf("  sudo insmod kernel/qd_dhcp_kmod.ko\n");
        close(sock);
        return 1;
    }

    printf("Interface: %s (ifindex=%d)\n", QDHCP_DEV_NAME, ifr.ifr_ifindex);

    /* Bind to the qdhcp0 interface */
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifr.ifr_ifindex;
    sll.sll_protocol = htons(ETH_P_ALL);

    if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    /* Bring interface up */
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(sock, SIOCSIFFLAGS, &ifr);
    }

    printf("Bound to %s, waiting for DHCP packets...\n", QDHCP_DEV_NAME);
    printf("Press Ctrl+C to exit\n\n");

    while (running) {
        ssize_t len = recv(sock, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR)
                continue;
            perror("recv");
            break;
        }

        if ((size_t)len < sizeof(struct qdhcp_meta_hdr)) {
            printf("Packet too short: %zd bytes\n", len);
            continue;
        }

        struct qdhcp_meta_hdr *meta = (struct qdhcp_meta_hdr *)buf;

        /* Validate metadata */
        if (!QDHCP_META_VALID(meta)) {
            printf("Invalid metadata (magic=0x%08x, version=%d)\n",
                   meta->magic, meta->version);
            continue;
        }

        /* Only show RX direction (kernel -> userspace) */
        if (meta->direction != QDHCP_DIR_RX) {
            continue;
        }

        printf("----------------------------------------\n");
        printf("RX Packet: %zd bytes\n", len);
        printf("  Direction:  %s\n", meta->direction == QDHCP_DIR_RX ? "RX" : "TX");
        printf("  DHCP Type:  %s (%d)\n", dhcp_type_str(meta->msg_type), meta->msg_type);
        printf("  Interface:  %d\n", meta->ifindex);
        printf("  Method:     %s (%d)\n", method_str(meta->method), meta->method);
        printf("  Trusted:    %s\n", meta->trusted ? "yes" : "no");
        printf("  VLAN ID:    %d\n", meta->vlan_id);
        printf("  Src MAC:    %02x:%02x:%02x:%02x:%02x:%02x\n",
               meta->src_mac[0], meta->src_mac[1], meta->src_mac[2],
               meta->src_mac[3], meta->src_mac[4], meta->src_mac[5]);
        printf("  Dst MAC:    %02x:%02x:%02x:%02x:%02x:%02x\n",
               meta->dst_mac[0], meta->dst_mac[1], meta->dst_mac[2],
               meta->dst_mac[3], meta->dst_mac[4], meta->dst_mac[5]);
        printf("  Pkt Len:    %d\n", meta->pkt_len);
        printf("  Packet Data:\n    ");
        dump_hex(buf + sizeof(struct qdhcp_meta_hdr), meta->pkt_len);
        printf("\n");
    }

    printf("\nShutting down...\n");
    close(sock);
    return 0;
}
