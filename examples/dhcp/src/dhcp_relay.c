/*
 * DHCP Relay Agent Implementation (RFC 2131, RFC 3046)
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>

#ifdef STANDALONE
#include "qdaemon_stub.h"
#else
#include <qdaemon/qd_log.h>
#endif

#include "dhcp_relay.h"

/*
 * Relay Context
 */
struct dhcp_relay {
    dhcp_relay_config_t config;

    /* Interfaces */
    dhcp_relay_iface_t ifaces[DHCP_RELAY_MAX_IFACES];
    int                num_ifaces;

    /* Send callback */
    dhcp_relay_send_fn send_fn;
    void              *send_arg;

    /* Event loop */
    qd_event_loop_t   *loop;

    /* Relay MAC address */
    uint8_t            relay_mac[6];
    char               hostname[64];

    /* Statistics */
    dhcp_relay_stats_t stats;

    /* State */
    bool               enabled;
};

/*
 * Lifecycle
 */

dhcp_relay_t *dhcp_relay_create(const dhcp_relay_config_t *config)
{
    dhcp_relay_t *relay = calloc(1, sizeof(dhcp_relay_t));
    if (!relay)
        return NULL;

    if (config) {
        memcpy(&relay->config, config, sizeof(dhcp_relay_config_t));
    } else {
        dhcp_relay_config_t defaults = DHCP_RELAY_CONFIG_DEFAULT;
        memcpy(&relay->config, &defaults, sizeof(dhcp_relay_config_t));
    }

    /* Get hostname */
    gethostname(relay->hostname, sizeof(relay->hostname) - 1);

    /* Generate relay MAC (locally administered) */
    relay->relay_mac[0] = 0x02;
    for (int i = 1; i < 6; i++) {
        relay->relay_mac[i] = rand() & 0xff;
    }

    return relay;
}

void dhcp_relay_destroy(dhcp_relay_t *relay)
{
    free(relay);
}

int dhcp_relay_start(dhcp_relay_t *relay, qd_event_loop_t *loop)
{
    if (!relay)
        return -1;

    relay->loop = loop;
    relay->enabled = true;

    qd_log_info("DHCP relay started with %d servers, %d interfaces",
                relay->config.num_servers, relay->num_ifaces);

    return 0;
}

void dhcp_relay_stop(dhcp_relay_t *relay)
{
    if (!relay)
        return;

    relay->enabled = false;
    relay->loop = NULL;

    qd_log_info("DHCP relay stopped");
}

/*
 * Server Management
 */

int dhcp_relay_add_server(dhcp_relay_t *relay, uint32_t server_ip)
{
    if (!relay)
        return -1;

    if (relay->config.num_servers >= DHCP_RELAY_MAX_SERVERS)
        return -1;

    /* Check for duplicate */
    for (int i = 0; i < relay->config.num_servers; i++) {
        if (relay->config.server_addrs[i] == server_ip)
            return 0;  /* Already exists */
    }

    relay->config.server_addrs[relay->config.num_servers++] = server_ip;

    struct in_addr addr = { .s_addr = server_ip };
    qd_log_info("Added relay server: %s", inet_ntoa(addr));

    return 0;
}

int dhcp_relay_remove_server(dhcp_relay_t *relay, uint32_t server_ip)
{
    if (!relay)
        return -1;

    for (int i = 0; i < relay->config.num_servers; i++) {
        if (relay->config.server_addrs[i] == server_ip) {
            /* Shift remaining servers */
            for (int j = i; j < relay->config.num_servers - 1; j++) {
                relay->config.server_addrs[j] = relay->config.server_addrs[j + 1];
            }
            relay->config.num_servers--;
            return 0;
        }
    }

    return -1;
}

int dhcp_relay_clear_servers(dhcp_relay_t *relay)
{
    if (!relay)
        return -1;
    relay->config.num_servers = 0;
    return 0;
}

int dhcp_relay_get_servers(dhcp_relay_t *relay, uint32_t *servers, int max_count)
{
    if (!relay || !servers)
        return -1;

    int count = relay->config.num_servers < max_count ?
                relay->config.num_servers : max_count;
    memcpy(servers, relay->config.server_addrs, count * sizeof(uint32_t));
    return count;
}

/*
 * Interface Management
 */

int dhcp_relay_add_interface(dhcp_relay_t *relay, const char *ifname, uint32_t giaddr)
{
    if (!relay || !ifname)
        return -1;

    if (relay->num_ifaces >= DHCP_RELAY_MAX_IFACES)
        return -1;

    /* Check for duplicate */
    for (int i = 0; i < relay->num_ifaces; i++) {
        if (strcmp(relay->ifaces[i].name, ifname) == 0)
            return -1;
    }

    dhcp_relay_iface_t *iface = &relay->ifaces[relay->num_ifaces];
    strncpy(iface->name, ifname, sizeof(iface->name) - 1);
    iface->ifindex = if_nametoindex(ifname);
    iface->giaddr = giaddr;
    iface->enabled = true;

    relay->num_ifaces++;

    struct in_addr addr = { .s_addr = giaddr };
    qd_log_info("Added relay interface: %s (giaddr=%s)", ifname, inet_ntoa(addr));

    return 0;
}

int dhcp_relay_remove_interface(dhcp_relay_t *relay, const char *ifname)
{
    if (!relay || !ifname)
        return -1;

    for (int i = 0; i < relay->num_ifaces; i++) {
        if (strcmp(relay->ifaces[i].name, ifname) == 0) {
            for (int j = i; j < relay->num_ifaces - 1; j++) {
                relay->ifaces[j] = relay->ifaces[j + 1];
            }
            relay->num_ifaces--;
            return 0;
        }
    }

    return -1;
}

const dhcp_relay_iface_t *dhcp_relay_get_interface(dhcp_relay_t *relay, const char *ifname)
{
    if (!relay || !ifname)
        return NULL;

    for (int i = 0; i < relay->num_ifaces; i++) {
        if (strcmp(relay->ifaces[i].name, ifname) == 0)
            return &relay->ifaces[i];
    }

    return NULL;
}

int dhcp_relay_get_interfaces(dhcp_relay_t *relay, dhcp_relay_iface_t *ifaces, int max_count)
{
    if (!relay || !ifaces)
        return -1;

    int count = relay->num_ifaces < max_count ? relay->num_ifaces : max_count;
    memcpy(ifaces, relay->ifaces, count * sizeof(dhcp_relay_iface_t));
    return count;
}

/*
 * Configuration
 */

int dhcp_relay_set_send_callback(dhcp_relay_t *relay, dhcp_relay_send_fn fn, void *arg)
{
    if (!relay)
        return -1;
    relay->send_fn = fn;
    relay->send_arg = arg;
    return 0;
}

void dhcp_relay_set_option82(dhcp_relay_t *relay, bool insert, bool replace)
{
    if (!relay)
        return;
    relay->config.insert_option82 = insert;
    relay->config.replace_option82 = replace;
}

void dhcp_relay_set_max_hops(dhcp_relay_t *relay, int max_hops)
{
    if (!relay)
        return;
    relay->config.max_hops = max_hops;
}

/*
 * Build Circuit ID based on configuration
 */
static int build_circuit_id(dhcp_relay_t *relay, int ifindex,
                            uint8_t *buf, size_t buflen)
{
    switch (relay->config.circuit_id_type) {
    case DHCP_CIRCUIT_ID_IFNAME: {
        char ifname[32];
        if (if_indextoname(ifindex, ifname)) {
            size_t len = strlen(ifname);
            if (len > buflen) len = buflen;
            memcpy(buf, ifname, len);
            return len;
        }
        return -1;
    }
    case DHCP_CIRCUIT_ID_IFINDEX: {
        if (buflen < 4) return -1;
        uint32_t idx = htonl(ifindex);
        memcpy(buf, &idx, 4);
        return 4;
    }
    case DHCP_CIRCUIT_ID_CUSTOM:
        if (relay->config.custom_circuit_id_len > buflen)
            return -1;
        memcpy(buf, relay->config.custom_circuit_id,
               relay->config.custom_circuit_id_len);
        return relay->config.custom_circuit_id_len;
    default:
        return -1;
    }
}

/*
 * Build Remote ID based on configuration
 */
static int build_remote_id(dhcp_relay_t *relay, uint8_t *buf, size_t buflen)
{
    switch (relay->config.remote_id_type) {
    case DHCP_REMOTE_ID_MAC:
        if (buflen < 6) return -1;
        memcpy(buf, relay->relay_mac, 6);
        return 6;
    case DHCP_REMOTE_ID_HOSTNAME: {
        size_t len = strlen(relay->hostname);
        if (len > buflen) len = buflen;
        memcpy(buf, relay->hostname, len);
        return len;
    }
    case DHCP_REMOTE_ID_CUSTOM:
        if (relay->config.custom_remote_id_len > buflen)
            return -1;
        memcpy(buf, relay->config.custom_remote_id,
               relay->config.custom_remote_id_len);
        return relay->config.custom_remote_id_len;
    default:
        return -1;
    }
}

/*
 * Option 82 Handling
 */

int dhcp_relay_add_option82(dhcp_relay_t *relay, dhcp_packet_t *pkt, int ifindex)
{
    if (!relay || !pkt)
        return -1;

    /* Check if option 82 already exists */
    if (dhcp_has_option82(pkt)) {
        if (relay->config.replace_option82) {
            dhcp_option_strip_relay_info(pkt);
        } else {
            /* Keep existing option 82 */
            return 0;
        }
    }

    /* Build circuit-id */
    uint8_t circuit_id[64];
    int circuit_len = build_circuit_id(relay, ifindex, circuit_id, sizeof(circuit_id));
    if (circuit_len < 0) {
        qd_log_warn("Failed to build circuit-id");
        return -1;
    }

    /* Build remote-id */
    uint8_t remote_id[64];
    int remote_len = build_remote_id(relay, remote_id, sizeof(remote_id));
    if (remote_len < 0) {
        qd_log_warn("Failed to build remote-id");
        return -1;
    }

    /* Find end of options and insert option 82 before END */
    uint8_t *opt = pkt->options;
    size_t i = 0;

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        i += 2 + opt[i + 1];
    }

    if (i >= DHCP_MAX_OPTIONS_SIZE)
        return -1;

    /* Calculate option 82 length */
    size_t opt82_len = 2 + circuit_len + 2 + remote_len;  /* sub-options with code/len */
    size_t total_len = 2 + opt82_len;  /* option code + len + data */

    if (i + total_len + 1 > DHCP_MAX_OPTIONS_SIZE)
        return -1;

    /* Insert option 82 */
    opt[i++] = DHCP_OPT_RELAY_AGENT;
    opt[i++] = opt82_len;

    /* Circuit ID sub-option */
    opt[i++] = DHCP_RAI_CIRCUIT_ID;
    opt[i++] = circuit_len;
    memcpy(&opt[i], circuit_id, circuit_len);
    i += circuit_len;

    /* Remote ID sub-option */
    opt[i++] = DHCP_RAI_REMOTE_ID;
    opt[i++] = remote_len;
    memcpy(&opt[i], remote_id, remote_len);
    i += remote_len;

    /* END option */
    opt[i] = DHCP_OPT_END;

    relay->stats.option82_added++;

    return 0;
}

int dhcp_relay_strip_option82(dhcp_relay_t *relay, dhcp_packet_t *pkt)
{
    if (!relay || !pkt)
        return -1;

    int ret = dhcp_option_strip_relay_info(pkt);
    if (ret == 0)
        relay->stats.option82_stripped++;

    return ret;
}

/*
 * Find interface by index
 */
static dhcp_relay_iface_t *find_interface_by_index(dhcp_relay_t *relay, int ifindex)
{
    for (int i = 0; i < relay->num_ifaces; i++) {
        if (relay->ifaces[i].ifindex == ifindex)
            return &relay->ifaces[i];
    }
    return NULL;
}

/*
 * Process packet from client
 */
int dhcp_relay_process_client_packet(dhcp_relay_t *relay,
                                      dhcp_packet_t *pkt,
                                      size_t pkt_len,
                                      int ifindex)
{
    if (!relay || !pkt || !relay->enabled)
        return -1;

    if (relay->config.num_servers == 0) {
        qd_log_warn("No relay servers configured");
        relay->stats.drops_no_server++;
        return -1;
    }

    /* Check hop count */
    if (pkt->hops >= relay->config.max_hops) {
        qd_log_warn("Max hops exceeded (%d >= %d)", pkt->hops, relay->config.max_hops);
        relay->stats.drops_max_hops++;
        return -1;
    }

    /* Find interface configuration */
    dhcp_relay_iface_t *iface = find_interface_by_index(relay, ifindex);
    if (!iface || !iface->enabled) {
        qd_log_debug("Relay not enabled on interface %d", ifindex);
        return -1;
    }

    /* Increment hop count */
    pkt->hops++;

    /* Set giaddr if not already set */
    if (pkt->giaddr == 0) {
        pkt->giaddr = iface->giaddr;
    }

    /* Add Option 82 if configured */
    if (relay->config.insert_option82) {
        dhcp_relay_add_option82(relay, pkt, ifindex);
    }

    /* Forward to all configured servers */
    if (!relay->send_fn) {
        qd_log_error("No send callback configured");
        return -1;
    }

    int sent = 0;
    for (int i = 0; i < relay->config.num_servers; i++) {
        if (relay->send_fn((uint8_t *)pkt, pkt_len,
                           relay->config.server_addrs[i], DHCP_SERVER_PORT,
                           0, relay->send_arg) == 0) {
            sent++;
        }
    }

    if (sent > 0) {
        relay->stats.requests_forwarded++;
        iface->tx_packets++;

        int msg_type = dhcp_get_msg_type(pkt);
        qd_log_debug("Relayed %s to %d servers",
                     dhcp_msg_type_name(msg_type), sent);
    }

    return sent > 0 ? 0 : -1;
}

/*
 * Process packet from server (reply)
 */
int dhcp_relay_process_server_packet(dhcp_relay_t *relay,
                                      dhcp_packet_t *pkt,
                                      size_t pkt_len)
{
    if (!relay || !pkt || !relay->enabled)
        return -1;

    /* Verify giaddr is one of ours */
    if (pkt->giaddr == 0) {
        qd_log_debug("Server reply has no giaddr");
        relay->stats.drops_bad_giaddr++;
        return -1;
    }

    /* Find interface by giaddr */
    dhcp_relay_iface_t *iface = NULL;
    for (int i = 0; i < relay->num_ifaces; i++) {
        if (relay->ifaces[i].giaddr == pkt->giaddr) {
            iface = &relay->ifaces[i];
            break;
        }
    }

    if (!iface) {
        struct in_addr addr = { .s_addr = pkt->giaddr };
        qd_log_warn("Reply giaddr %s doesn't match any interface", inet_ntoa(addr));
        relay->stats.drops_bad_giaddr++;
        return -1;
    }

    /* Strip Option 82 before forwarding to client */
    if (dhcp_has_option82(pkt)) {
        dhcp_relay_strip_option82(relay, pkt);
    }

    /* Clear giaddr before sending to client */
    pkt->giaddr = 0;

    /* Forward to client */
    if (!relay->send_fn) {
        qd_log_error("No send callback configured");
        return -1;
    }

    /* Determine destination:
     * - If broadcast flag set, broadcast
     * - Otherwise, unicast to yiaddr (offered IP)
     */
    uint32_t dst_ip;
    uint16_t dst_port = DHCP_CLIENT_PORT;

    if (ntohs(pkt->flags) & DHCP_FLAG_BROADCAST || pkt->yiaddr == 0) {
        dst_ip = INADDR_BROADCAST;
    } else {
        dst_ip = pkt->yiaddr;
    }

    if (relay->send_fn((uint8_t *)pkt, pkt_len,
                       dst_ip, dst_port,
                       iface->ifindex, relay->send_arg) == 0) {
        relay->stats.replies_relayed++;
        iface->tx_packets++;

        int msg_type = dhcp_get_msg_type(pkt);
        qd_log_debug("Relayed %s reply to client", dhcp_msg_type_name(msg_type));
        return 0;
    }

    return -1;
}

/*
 * Statistics
 */

void dhcp_relay_get_stats(dhcp_relay_t *relay, dhcp_relay_stats_t *stats)
{
    if (!relay || !stats)
        return;
    memcpy(stats, &relay->stats, sizeof(*stats));
}

void dhcp_relay_reset_stats(dhcp_relay_t *relay)
{
    if (!relay)
        return;
    memset(&relay->stats, 0, sizeof(relay->stats));
}

/*
 * Utility
 */

const char *dhcp_relay_state_name(dhcp_relay_state_t state)
{
    switch (state) {
    case DHCP_RELAY_IDLE:       return "IDLE";
    case DHCP_RELAY_FORWARDING: return "FORWARDING";
    case DHCP_RELAY_RELAYING:   return "RELAYING";
    default:                    return "UNKNOWN";
    }
}

bool dhcp_relay_is_enabled(dhcp_relay_t *relay)
{
    return relay && relay->enabled;
}
