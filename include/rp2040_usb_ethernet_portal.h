#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/netif.h"
#include "rp2040_usb_ethernet_portal_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
} rp2040_usb_portal_ipv4_t;

#define RP2040_USB_PORTAL_IPV4(a_, b_, c_, d_) \
    ((rp2040_usb_portal_ipv4_t){(uint8_t)(a_), (uint8_t)(b_), (uint8_t)(c_), (uint8_t)(d_)})

typedef struct {
    const char *method;
    const char *path;
    const char *query;
    const char *raw;
} rp2040_usb_portal_http_request_t;

typedef struct {
    const char *status;
    const char *content_type;
    const char *body;
    uint16_t body_len;
} rp2040_usb_portal_http_response_t;

typedef bool (*rp2040_usb_portal_http_handler_t)(
    const rp2040_usb_portal_http_request_t *request,
    rp2040_usb_portal_http_response_t *response,
    void *user_data);

typedef struct {
    rp2040_usb_portal_ipv4_t address;
    rp2040_usb_portal_ipv4_t netmask;
    rp2040_usb_portal_ipv4_t gateway;
    rp2040_usb_portal_ipv4_t dhcp_lease_start;
    rp2040_usb_portal_ipv4_t dhcp_router;
    rp2040_usb_portal_ipv4_t dns_server;
    uint8_t dhcp_lease_count;
    uint32_t dhcp_lease_seconds;
    uint16_t dhcp_port;
    uint16_t dns_port;
    const char *dhcp_domain;
    const char *default_redirect_url;
    bool advertise_gateway;
    bool enable_dns_catchall;
    bool enable_http_server;
    bool initialize_lwip;
    bool initialize_tinyusb;
    rp2040_usb_portal_http_handler_t http_handler;
    void *http_user_data;
} rp2040_usb_portal_config_t;

void rp2040_usb_portal_config_init(rp2040_usb_portal_config_t *config);
bool rp2040_usb_portal_init(const rp2040_usb_portal_config_t *config);
void rp2040_usb_portal_task(void);
void rp2040_usb_portal_reboot_to_bootsel(uint32_t delay_ms);

struct netif *rp2040_usb_portal_netif(void);
uint32_t rp2040_usb_portal_http_request_count(void);
const uint8_t *rp2040_usb_portal_mac_address(void);
void rp2040_usb_portal_ipv4_to_string(rp2040_usb_portal_ipv4_t ip, char *buffer, size_t buffer_len);

#ifdef __cplusplus
}
#endif
