/*
 * DHCP Option Builder and Parser API
 * Handles encoding/decoding of DHCP options including relay agent info (RFC 3046)
 */

#ifndef DHCP_OPTION_H
#define DHCP_OPTION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "dhcp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum options in a list */
#define DHCP_OPTION_LIST_MAX    64

/*
 * Parsed Option Entry
 */
typedef struct dhcp_option {
    uint8_t        code;        /* Option code */
    uint8_t        len;         /* Option data length */
    const uint8_t *data;        /* Pointer to option data */
} dhcp_option_t;

/*
 * Option List Builder - For constructing options
 */
typedef struct dhcp_option_list {
    uint8_t  buffer[DHCP_MAX_OPTIONS_SIZE];
    size_t   offset;            /* Current write offset */
    int      count;             /* Number of options added */
    bool     finalized;         /* END option added */
} dhcp_option_list_t;

/*
 * Option List Lifecycle
 */
dhcp_option_list_t *dhcp_option_list_create(void);
void dhcp_option_list_destroy(dhcp_option_list_t *list);
void dhcp_option_list_init(dhcp_option_list_t *list);
void dhcp_option_list_reset(dhcp_option_list_t *list);

/*
 * Generic Option Add
 */
int dhcp_option_add(dhcp_option_list_t *list, uint8_t code,
                    const void *data, uint8_t len);
int dhcp_option_add_byte(dhcp_option_list_t *list, uint8_t code, uint8_t value);
int dhcp_option_add_u16(dhcp_option_list_t *list, uint8_t code, uint16_t value);
int dhcp_option_add_u32(dhcp_option_list_t *list, uint8_t code, uint32_t value);
int dhcp_option_add_ip(dhcp_option_list_t *list, uint8_t code, uint32_t ip);
int dhcp_option_add_ips(dhcp_option_list_t *list, uint8_t code,
                        const uint32_t *ips, int count);
int dhcp_option_add_string(dhcp_option_list_t *list, uint8_t code,
                           const char *str);

/*
 * Standard Option Helpers
 */
int dhcp_option_add_msg_type(dhcp_option_list_t *list, uint8_t type);
int dhcp_option_add_server_id(dhcp_option_list_t *list, uint32_t ip);
int dhcp_option_add_lease_time(dhcp_option_list_t *list, uint32_t seconds);
int dhcp_option_add_subnet_mask(dhcp_option_list_t *list, uint32_t mask);
int dhcp_option_add_router(dhcp_option_list_t *list, uint32_t ip);
int dhcp_option_add_routers(dhcp_option_list_t *list, const uint32_t *ips, int count);
int dhcp_option_add_dns(dhcp_option_list_t *list, const uint32_t *servers, int count);
int dhcp_option_add_domain(dhcp_option_list_t *list, const char *domain);
int dhcp_option_add_hostname(dhcp_option_list_t *list, const char *hostname);
int dhcp_option_add_broadcast(dhcp_option_list_t *list, uint32_t addr);
int dhcp_option_add_renewal_time(dhcp_option_list_t *list, uint32_t seconds);
int dhcp_option_add_rebind_time(dhcp_option_list_t *list, uint32_t seconds);
int dhcp_option_add_client_id(dhcp_option_list_t *list,
                              const uint8_t *id, size_t len);
int dhcp_option_add_requested_ip(dhcp_option_list_t *list, uint32_t ip);
int dhcp_option_add_param_request(dhcp_option_list_t *list,
                                   const uint8_t *params, int count);
int dhcp_option_add_message(dhcp_option_list_t *list, const char *msg);

/*
 * Relay Agent Information Option (Option 82 - RFC 3046)
 */
int dhcp_option_add_relay_info(dhcp_option_list_t *list,
                               const uint8_t *circuit_id, size_t circuit_len,
                               const uint8_t *remote_id, size_t remote_len);
int dhcp_option_add_relay_info_ex(dhcp_option_list_t *list,
                                   const uint8_t *circuit_id, size_t circuit_len,
                                   const uint8_t *remote_id, size_t remote_len,
                                   const uint8_t *link_sel, size_t link_sel_len);

/*
 * Finalize and Serialize
 */
int dhcp_option_list_finalize(dhcp_option_list_t *list);
int dhcp_option_list_serialize(dhcp_option_list_t *list, uint8_t *buf, size_t max_len);
size_t dhcp_option_list_size(const dhcp_option_list_t *list);

/*
 * Option Parsing
 */
int dhcp_options_parse(const uint8_t *options, size_t len,
                       dhcp_option_t *parsed, int max_options);
int dhcp_packet_parse_options(const dhcp_packet_t *pkt,
                              dhcp_option_t *parsed, int max_options);

/*
 * Option Query
 */
const dhcp_option_t *dhcp_option_find(const dhcp_option_t *opts, int count,
                                       uint8_t code);
int dhcp_option_find_in_packet(const dhcp_packet_t *pkt, uint8_t code,
                                void *buf, size_t buflen);
uint32_t dhcp_option_get_u32_from_packet(const dhcp_packet_t *pkt, uint8_t code,
                                          uint32_t default_val);
int dhcp_option_get_string_from_packet(const dhcp_packet_t *pkt, uint8_t code,
                                        char *buf, size_t buflen);

/*
 * Relay Agent Info Sub-option Parsing
 */
typedef struct dhcp_relay_info {
    uint8_t  circuit_id[255];
    size_t   circuit_id_len;
    uint8_t  remote_id[255];
    size_t   remote_id_len;
    uint8_t  link_sel[4];
    bool     has_link_sel;
} dhcp_relay_info_t;

int dhcp_option_parse_relay_info(const dhcp_packet_t *pkt, dhcp_relay_info_t *info);
int dhcp_option_strip_relay_info(dhcp_packet_t *pkt);

/*
 * Option Utility Functions
 */
const char *dhcp_option_name(uint8_t code);
void dhcp_option_dump(const dhcp_option_t *opt, char *buf, size_t buflen);
void dhcp_options_dump_all(const dhcp_packet_t *pkt,
                           void (*print_fn)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif

#endif /* DHCP_OPTION_H */
