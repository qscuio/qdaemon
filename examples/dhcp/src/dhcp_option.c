/*
 * DHCP Option Builder and Parser Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "dhcp_option.h"

/*
 * Option List Lifecycle
 */

dhcp_option_list_t *dhcp_option_list_create(void)
{
    dhcp_option_list_t *list = calloc(1, sizeof(dhcp_option_list_t));
    if (list) {
        dhcp_option_list_init(list);
    }
    return list;
}

void dhcp_option_list_destroy(dhcp_option_list_t *list)
{
    free(list);
}

void dhcp_option_list_init(dhcp_option_list_t *list)
{
    memset(list, 0, sizeof(*list));
}

void dhcp_option_list_reset(dhcp_option_list_t *list)
{
    list->offset = 0;
    list->count = 0;
    list->finalized = false;
}

/*
 * Generic Option Add
 */

int dhcp_option_add(dhcp_option_list_t *list, uint8_t code,
                    const void *data, uint8_t len)
{
    if (!list || list->finalized)
        return -1;

    /* Check space: code(1) + len(1) + data(len) */
    if (list->offset + 2 + len > DHCP_MAX_OPTIONS_SIZE - 1)
        return -1;

    list->buffer[list->offset++] = code;
    list->buffer[list->offset++] = len;

    if (len > 0 && data) {
        memcpy(&list->buffer[list->offset], data, len);
        list->offset += len;
    }

    list->count++;
    return 0;
}

int dhcp_option_add_byte(dhcp_option_list_t *list, uint8_t code, uint8_t value)
{
    return dhcp_option_add(list, code, &value, 1);
}

int dhcp_option_add_u16(dhcp_option_list_t *list, uint8_t code, uint16_t value)
{
    uint16_t net = htons(value);
    return dhcp_option_add(list, code, &net, 2);
}

int dhcp_option_add_u32(dhcp_option_list_t *list, uint8_t code, uint32_t value)
{
    uint32_t net = htonl(value);
    return dhcp_option_add(list, code, &net, 4);
}

int dhcp_option_add_ip(dhcp_option_list_t *list, uint8_t code, uint32_t ip)
{
    /* IP is already in network byte order */
    return dhcp_option_add(list, code, &ip, 4);
}

int dhcp_option_add_ips(dhcp_option_list_t *list, uint8_t code,
                        const uint32_t *ips, int count)
{
    if (count <= 0 || count > 63)
        return -1;
    return dhcp_option_add(list, code, ips, count * 4);
}

int dhcp_option_add_string(dhcp_option_list_t *list, uint8_t code,
                           const char *str)
{
    if (!str)
        return -1;
    size_t len = strlen(str);
    if (len > 255)
        len = 255;
    return dhcp_option_add(list, code, str, (uint8_t)len);
}

/*
 * Standard Option Helpers
 */

int dhcp_option_add_msg_type(dhcp_option_list_t *list, uint8_t type)
{
    return dhcp_option_add_byte(list, DHCP_OPT_MSG_TYPE, type);
}

int dhcp_option_add_server_id(dhcp_option_list_t *list, uint32_t ip)
{
    return dhcp_option_add_ip(list, DHCP_OPT_SERVER_ID, ip);
}

int dhcp_option_add_lease_time(dhcp_option_list_t *list, uint32_t seconds)
{
    return dhcp_option_add_u32(list, DHCP_OPT_LEASE_TIME, seconds);
}

int dhcp_option_add_subnet_mask(dhcp_option_list_t *list, uint32_t mask)
{
    return dhcp_option_add_ip(list, DHCP_OPT_SUBNET_MASK, mask);
}

int dhcp_option_add_router(dhcp_option_list_t *list, uint32_t ip)
{
    return dhcp_option_add_ip(list, DHCP_OPT_ROUTER, ip);
}

int dhcp_option_add_routers(dhcp_option_list_t *list, const uint32_t *ips, int count)
{
    return dhcp_option_add_ips(list, DHCP_OPT_ROUTER, ips, count);
}

int dhcp_option_add_dns(dhcp_option_list_t *list, const uint32_t *servers, int count)
{
    return dhcp_option_add_ips(list, DHCP_OPT_DNS, servers, count);
}

int dhcp_option_add_domain(dhcp_option_list_t *list, const char *domain)
{
    return dhcp_option_add_string(list, DHCP_OPT_DOMAIN, domain);
}

int dhcp_option_add_hostname(dhcp_option_list_t *list, const char *hostname)
{
    return dhcp_option_add_string(list, DHCP_OPT_HOSTNAME, hostname);
}

int dhcp_option_add_broadcast(dhcp_option_list_t *list, uint32_t addr)
{
    return dhcp_option_add_ip(list, DHCP_OPT_BROADCAST, addr);
}

int dhcp_option_add_renewal_time(dhcp_option_list_t *list, uint32_t seconds)
{
    return dhcp_option_add_u32(list, DHCP_OPT_RENEWAL_TIME, seconds);
}

int dhcp_option_add_rebind_time(dhcp_option_list_t *list, uint32_t seconds)
{
    return dhcp_option_add_u32(list, DHCP_OPT_REBIND_TIME, seconds);
}

int dhcp_option_add_client_id(dhcp_option_list_t *list,
                              const uint8_t *id, size_t len)
{
    if (len > 255)
        return -1;
    return dhcp_option_add(list, DHCP_OPT_CLIENT_ID, id, (uint8_t)len);
}

int dhcp_option_add_requested_ip(dhcp_option_list_t *list, uint32_t ip)
{
    return dhcp_option_add_ip(list, DHCP_OPT_REQUESTED_IP, ip);
}

int dhcp_option_add_param_request(dhcp_option_list_t *list,
                                   const uint8_t *params, int count)
{
    if (count > 255)
        return -1;
    return dhcp_option_add(list, DHCP_OPT_PARAM_LIST, params, (uint8_t)count);
}

int dhcp_option_add_message(dhcp_option_list_t *list, const char *msg)
{
    return dhcp_option_add_string(list, DHCP_OPT_MESSAGE, msg);
}

/*
 * Relay Agent Information Option (Option 82 - RFC 3046)
 */

int dhcp_option_add_relay_info(dhcp_option_list_t *list,
                               const uint8_t *circuit_id, size_t circuit_len,
                               const uint8_t *remote_id, size_t remote_len)
{
    return dhcp_option_add_relay_info_ex(list, circuit_id, circuit_len,
                                          remote_id, remote_len, NULL, 0);
}

int dhcp_option_add_relay_info_ex(dhcp_option_list_t *list,
                                   const uint8_t *circuit_id, size_t circuit_len,
                                   const uint8_t *remote_id, size_t remote_len,
                                   const uint8_t *link_sel, size_t link_sel_len)
{
    if (!list || list->finalized)
        return -1;

    /* Calculate total sub-option length */
    size_t total_len = 0;
    if (circuit_id && circuit_len > 0)
        total_len += 2 + circuit_len;  /* sub-opt code + len + data */
    if (remote_id && remote_len > 0)
        total_len += 2 + remote_len;
    if (link_sel && link_sel_len > 0)
        total_len += 2 + link_sel_len;

    if (total_len == 0 || total_len > 255)
        return -1;

    /* Check space */
    if (list->offset + 2 + total_len > DHCP_MAX_OPTIONS_SIZE - 1)
        return -1;

    /* Write option header */
    list->buffer[list->offset++] = DHCP_OPT_RELAY_AGENT;
    list->buffer[list->offset++] = (uint8_t)total_len;

    /* Write Circuit ID sub-option */
    if (circuit_id && circuit_len > 0) {
        list->buffer[list->offset++] = DHCP_RAI_CIRCUIT_ID;
        list->buffer[list->offset++] = (uint8_t)circuit_len;
        memcpy(&list->buffer[list->offset], circuit_id, circuit_len);
        list->offset += circuit_len;
    }

    /* Write Remote ID sub-option */
    if (remote_id && remote_len > 0) {
        list->buffer[list->offset++] = DHCP_RAI_REMOTE_ID;
        list->buffer[list->offset++] = (uint8_t)remote_len;
        memcpy(&list->buffer[list->offset], remote_id, remote_len);
        list->offset += remote_len;
    }

    /* Write Link Selection sub-option */
    if (link_sel && link_sel_len > 0) {
        list->buffer[list->offset++] = DHCP_RAI_LINK_SELECTION;
        list->buffer[list->offset++] = (uint8_t)link_sel_len;
        memcpy(&list->buffer[list->offset], link_sel, link_sel_len);
        list->offset += link_sel_len;
    }

    list->count++;
    return 0;
}

/*
 * Finalize and Serialize
 */

int dhcp_option_list_finalize(dhcp_option_list_t *list)
{
    if (!list || list->finalized)
        return -1;

    if (list->offset >= DHCP_MAX_OPTIONS_SIZE)
        return -1;

    list->buffer[list->offset++] = DHCP_OPT_END;
    list->finalized = true;
    return 0;
}

int dhcp_option_list_serialize(dhcp_option_list_t *list, uint8_t *buf, size_t max_len)
{
    if (!list || !buf)
        return -1;

    /* Auto-finalize if not done */
    if (!list->finalized) {
        if (dhcp_option_list_finalize(list) < 0)
            return -1;
    }

    if (list->offset > max_len)
        return -1;

    memcpy(buf, list->buffer, list->offset);
    return (int)list->offset;
}

size_t dhcp_option_list_size(const dhcp_option_list_t *list)
{
    if (!list)
        return 0;
    return list->offset;
}

/*
 * Option Parsing
 */

int dhcp_options_parse(const uint8_t *options, size_t len,
                       dhcp_option_t *parsed, int max_options)
{
    if (!options || !parsed || max_options <= 0)
        return -1;

    int count = 0;
    size_t i = 0;

    while (i < len && count < max_options) {
        uint8_t code = options[i];

        if (code == DHCP_OPT_PAD) {
            i++;
            continue;
        }

        if (code == DHCP_OPT_END)
            break;

        if (i + 1 >= len)
            break;

        uint8_t opt_len = options[i + 1];
        if (i + 2 + opt_len > len)
            break;

        parsed[count].code = code;
        parsed[count].len = opt_len;
        parsed[count].data = &options[i + 2];
        count++;

        i += 2 + opt_len;
    }

    return count;
}

int dhcp_packet_parse_options(const dhcp_packet_t *pkt,
                              dhcp_option_t *parsed, int max_options)
{
    if (!pkt)
        return -1;
    return dhcp_options_parse(pkt->options, DHCP_MAX_OPTIONS_SIZE, parsed, max_options);
}

/*
 * Option Query
 */

const dhcp_option_t *dhcp_option_find(const dhcp_option_t *opts, int count,
                                       uint8_t code)
{
    for (int i = 0; i < count; i++) {
        if (opts[i].code == code)
            return &opts[i];
    }
    return NULL;
}

int dhcp_option_find_in_packet(const dhcp_packet_t *pkt, uint8_t code,
                                void *buf, size_t buflen)
{
    if (!pkt)
        return -1;

    const uint8_t *opt = pkt->options;
    size_t i = 0;

    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }

        if (i + 1 >= DHCP_MAX_OPTIONS_SIZE)
            break;

        uint8_t opt_len = opt[i + 1];

        if (opt[i] == code) {
            if (buf && opt_len <= buflen) {
                memcpy(buf, &opt[i + 2], opt_len);
            }
            return opt_len;
        }

        i += 2 + opt_len;
    }

    return -1;
}

uint32_t dhcp_option_get_u32_from_packet(const dhcp_packet_t *pkt, uint8_t code,
                                          uint32_t default_val)
{
    uint32_t val;
    if (dhcp_option_find_in_packet(pkt, code, &val, 4) == 4) {
        return ntohl(val);
    }
    return default_val;
}

int dhcp_option_get_string_from_packet(const dhcp_packet_t *pkt, uint8_t code,
                                        char *buf, size_t buflen)
{
    if (!buf || buflen == 0)
        return -1;

    int len = dhcp_option_find_in_packet(pkt, code, buf, buflen - 1);
    if (len > 0) {
        buf[len] = '\0';
        return len;
    }
    buf[0] = '\0';
    return -1;
}

/*
 * Relay Agent Info Parsing
 */

int dhcp_option_parse_relay_info(const dhcp_packet_t *pkt, dhcp_relay_info_t *info)
{
    if (!pkt || !info)
        return -1;

    memset(info, 0, sizeof(*info));

    /* Find Option 82 */
    uint8_t opt82_buf[255];
    int opt82_len = dhcp_option_find_in_packet(pkt, DHCP_OPT_RELAY_AGENT,
                                                opt82_buf, sizeof(opt82_buf));
    if (opt82_len <= 0)
        return -1;

    /* Parse sub-options */
    size_t i = 0;
    while (i + 2 <= (size_t)opt82_len) {
        uint8_t sub_code = opt82_buf[i];
        uint8_t sub_len = opt82_buf[i + 1];

        if (i + 2 + sub_len > (size_t)opt82_len)
            break;

        switch (sub_code) {
        case DHCP_RAI_CIRCUIT_ID:
            /* sub_len is uint8_t (max 255), circuit_id is 255 bytes, always fits */
            memcpy(info->circuit_id, &opt82_buf[i + 2], sub_len);
            info->circuit_id_len = sub_len;
            break;
        case DHCP_RAI_REMOTE_ID:
            /* sub_len is uint8_t (max 255), remote_id is 255 bytes, always fits */
            memcpy(info->remote_id, &opt82_buf[i + 2], sub_len);
            info->remote_id_len = sub_len;
            break;
        case DHCP_RAI_LINK_SELECTION:
            if (sub_len == 4) {
                memcpy(info->link_sel, &opt82_buf[i + 2], 4);
                info->has_link_sel = true;
            }
            break;
        }

        i += 2 + sub_len;
    }

    return 0;
}

int dhcp_option_strip_relay_info(dhcp_packet_t *pkt)
{
    if (!pkt)
        return -1;

    uint8_t *opt = pkt->options;
    size_t i = 0;
    size_t write_pos = 0;

    /* Copy options, skipping Option 82 */
    while (i < DHCP_MAX_OPTIONS_SIZE && opt[i] != DHCP_OPT_END) {
        if (opt[i] == DHCP_OPT_PAD) {
            if (write_pos < DHCP_MAX_OPTIONS_SIZE) {
                pkt->options[write_pos++] = DHCP_OPT_PAD;
            }
            i++;
            continue;
        }

        if (i + 1 >= DHCP_MAX_OPTIONS_SIZE)
            break;

        uint8_t code = opt[i];
        uint8_t len = opt[i + 1];

        if (code != DHCP_OPT_RELAY_AGENT) {
            /* Copy this option */
            size_t opt_size = 2 + len;
            if (write_pos + opt_size < DHCP_MAX_OPTIONS_SIZE) {
                memmove(&pkt->options[write_pos], &opt[i], opt_size);
                write_pos += opt_size;
            }
        }

        i += 2 + len;
    }

    /* Add END option */
    if (write_pos < DHCP_MAX_OPTIONS_SIZE) {
        pkt->options[write_pos++] = DHCP_OPT_END;
    }

    /* Zero remaining space */
    if (write_pos < DHCP_MAX_OPTIONS_SIZE) {
        memset(&pkt->options[write_pos], 0, DHCP_MAX_OPTIONS_SIZE - write_pos);
    }

    return 0;
}

/*
 * Option Names for Debugging
 */

const char *dhcp_option_name(uint8_t code)
{
    switch (code) {
    case DHCP_OPT_PAD:           return "Pad";
    case DHCP_OPT_SUBNET_MASK:   return "Subnet Mask";
    case DHCP_OPT_ROUTER:        return "Router";
    case DHCP_OPT_DNS:           return "DNS Server";
    case DHCP_OPT_HOSTNAME:      return "Hostname";
    case DHCP_OPT_DOMAIN:        return "Domain Name";
    case DHCP_OPT_BROADCAST:     return "Broadcast Address";
    case DHCP_OPT_REQUESTED_IP:  return "Requested IP";
    case DHCP_OPT_LEASE_TIME:    return "Lease Time";
    case DHCP_OPT_MSG_TYPE:      return "Message Type";
    case DHCP_OPT_SERVER_ID:     return "Server Identifier";
    case DHCP_OPT_PARAM_LIST:    return "Parameter Request List";
    case DHCP_OPT_MESSAGE:       return "Message";
    case DHCP_OPT_RENEWAL_TIME:  return "Renewal Time (T1)";
    case DHCP_OPT_REBIND_TIME:   return "Rebind Time (T2)";
    case DHCP_OPT_CLIENT_ID:     return "Client Identifier";
    case DHCP_OPT_RELAY_AGENT:   return "Relay Agent Info";
    case DHCP_OPT_END:           return "End";
    default:                     return "Unknown";
    }
}

void dhcp_option_dump(const dhcp_option_t *opt, char *buf, size_t buflen)
{
    if (!opt || !buf || buflen == 0)
        return;

    int pos = snprintf(buf, buflen, "Option %d (%s) len=%d: ",
                       opt->code, dhcp_option_name(opt->code), opt->len);

    /* Format data based on option type */
    if (opt->len == 4 && (opt->code == DHCP_OPT_SUBNET_MASK ||
                          opt->code == DHCP_OPT_ROUTER ||
                          opt->code == DHCP_OPT_DNS ||
                          opt->code == DHCP_OPT_BROADCAST ||
                          opt->code == DHCP_OPT_REQUESTED_IP ||
                          opt->code == DHCP_OPT_SERVER_ID)) {
        /* IP address */
        struct in_addr addr;
        memcpy(&addr.s_addr, opt->data, 4);
        if ((size_t)pos < buflen) {
            snprintf(buf + pos, buflen - pos, "%s", inet_ntoa(addr));
        }
    } else if (opt->code == DHCP_OPT_MSG_TYPE && opt->len == 1) {
        /* Message type */
        if ((size_t)pos < buflen) {
            snprintf(buf + pos, buflen - pos, "%s",
                     dhcp_msg_type_name(opt->data[0]));
        }
    } else if (opt->code == DHCP_OPT_LEASE_TIME && opt->len == 4) {
        /* Lease time */
        uint32_t time;
        memcpy(&time, opt->data, 4);
        if ((size_t)pos < buflen) {
            snprintf(buf + pos, buflen - pos, "%u seconds", ntohl(time));
        }
    } else {
        /* Hex dump */
        for (int i = 0; i < opt->len && (size_t)(pos + 3) < buflen; i++) {
            pos += snprintf(buf + pos, buflen - pos, "%02x ", opt->data[i]);
        }
    }
}

void dhcp_options_dump_all(const dhcp_packet_t *pkt,
                           void (*print_fn)(const char *fmt, ...))
{
    if (!pkt || !print_fn)
        return;

    dhcp_option_t opts[DHCP_OPTION_LIST_MAX];
    int count = dhcp_packet_parse_options(pkt, opts, DHCP_OPTION_LIST_MAX);

    print_fn("DHCP Options (%d):\n", count);

    char buf[256];
    for (int i = 0; i < count; i++) {
        dhcp_option_dump(&opts[i], buf, sizeof(buf));
        print_fn("  %s\n", buf);
    }
}
