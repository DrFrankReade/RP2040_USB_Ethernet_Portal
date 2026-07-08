#include "rp2040_usb_ethernet_portal.h"

#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "dhserver.h"
#include "dnserver.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#if RP2040_USB_PORTAL_DHCP_LEASE_COUNT < 1
#error RP2040_USB_PORTAL_DHCP_LEASE_COUNT must be at least 1
#endif

#if USB_NET_CROSS_PLATFORM
bool usb_net_adaptive_windows_probe_seen(void);
void usb_net_adaptive_use_windows_persona(void);
#endif

static rp2040_usb_portal_config_t active_config;
static struct netif netif_data;
static struct pbuf *received_frame;
static dhcp_entry_t dhcp_entries[RP2040_USB_PORTAL_DHCP_LEASE_COUNT];
static dhcp_config_t dhcp_config;
static ip4_addr_t active_ipaddr;
static ip4_addr_t active_netmask;
static ip4_addr_t active_gateway;
static bool portal_initialized;
static bool bootloader_requested;
static uint32_t bootloader_reset_at_ms;
static uint32_t http_request_count;
#if USB_NET_CROSS_PLATFORM
static bool adaptive_switched_to_windows;
#endif
#if CFG_TUD_CDC
#define SERIAL_RX_BUFFER_SIZE 256u
static uint8_t serial_rx_buffer[SERIAL_RX_BUFFER_SIZE];
static uint16_t serial_rx_head;
static uint16_t serial_rx_tail;
#endif

uint8_t tud_network_mac_address[6] = {0x02, 0x52, 0x50, 0x32, 0x30, 0x40};

static ip4_addr_t ip4_from_portal(rp2040_usb_portal_ipv4_t ip) {
    ip4_addr_t out;
    IP4_ADDR(&out, ip.a, ip.b, ip.c, ip.d);
    return out;
}

static rp2040_usb_portal_ipv4_t ipv4_add_host(rp2040_usb_portal_ipv4_t ip, uint8_t offset) {
    uint16_t host = (uint16_t)ip.d + offset;
    if (host > 254) {
        host = 254;
    }
    ip.d = (uint8_t)host;
    return ip;
}

void rp2040_usb_portal_ipv4_to_string(rp2040_usb_portal_ipv4_t ip, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return;
    }
    snprintf(buffer, buffer_len, "%u.%u.%u.%u", ip.a, ip.b, ip.c, ip.d);
}

void rp2040_usb_portal_config_init(rp2040_usb_portal_config_t *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->address = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_IP_A,
                                             RP2040_USB_PORTAL_IP_B,
                                             RP2040_USB_PORTAL_IP_C,
                                             RP2040_USB_PORTAL_IP_D);
    config->netmask = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_NETMASK_A,
                                             RP2040_USB_PORTAL_NETMASK_B,
                                             RP2040_USB_PORTAL_NETMASK_C,
                                             RP2040_USB_PORTAL_NETMASK_D);
    config->gateway = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_GATEWAY_A,
                                             RP2040_USB_PORTAL_GATEWAY_B,
                                             RP2040_USB_PORTAL_GATEWAY_C,
                                             RP2040_USB_PORTAL_GATEWAY_D);
    config->dhcp_lease_start = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_DHCP_START_A,
                                                      RP2040_USB_PORTAL_DHCP_START_B,
                                                      RP2040_USB_PORTAL_DHCP_START_C,
                                                      RP2040_USB_PORTAL_DHCP_START_D);
    config->dhcp_router = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_ROUTER_A,
                                                 RP2040_USB_PORTAL_ROUTER_B,
                                                 RP2040_USB_PORTAL_ROUTER_C,
                                                 RP2040_USB_PORTAL_ROUTER_D);
    config->dns_server = RP2040_USB_PORTAL_IPV4(RP2040_USB_PORTAL_DNS_A,
                                                RP2040_USB_PORTAL_DNS_B,
                                                RP2040_USB_PORTAL_DNS_C,
                                                RP2040_USB_PORTAL_DNS_D);
    config->dhcp_lease_count = RP2040_USB_PORTAL_DHCP_LEASE_COUNT;
    config->dhcp_lease_seconds = RP2040_USB_PORTAL_DHCP_LEASE_SECONDS;
    config->dhcp_port = RP2040_USB_PORTAL_DHCP_PORT;
    config->dns_port = RP2040_USB_PORTAL_DNS_PORT;
    config->dhcp_domain = RP2040_USB_PORTAL_DHCP_DOMAIN;
#ifdef RP2040_USB_PORTAL_DEFAULT_REDIRECT_URL
    config->default_redirect_url = RP2040_USB_PORTAL_DEFAULT_REDIRECT_URL;
#else
    config->default_redirect_url = NULL;
#endif
    config->advertise_gateway = RP2040_USB_PORTAL_ADVERTISE_GATEWAY != 0;
    config->enable_dhcp_server = RP2040_USB_PORTAL_ENABLE_DHCP != 0;
    config->enable_dns_catchall = true;
    config->enable_http_server = true;
    config->enable_serial_bootloader_touch = true;
    config->initialize_lwip = true;
    config->initialize_tinyusb = true;
}

#if CFG_TUD_CDC
static void serial_rx_push(uint8_t value) {
    uint16_t next_head = (uint16_t)((serial_rx_head + 1u) % SERIAL_RX_BUFFER_SIZE);
    if (next_head == serial_rx_tail) {
        serial_rx_tail = (uint16_t)((serial_rx_tail + 1u) % SERIAL_RX_BUFFER_SIZE);
    }
    serial_rx_buffer[serial_rx_head] = value;
    serial_rx_head = next_head;
}

static bool serial_rx_pop(uint8_t *value) {
    if (serial_rx_head == serial_rx_tail) {
        return false;
    }
    *value = serial_rx_buffer[serial_rx_tail];
    serial_rx_tail = (uint16_t)((serial_rx_tail + 1u) % SERIAL_RX_BUFFER_SIZE);
    return true;
}
#endif

static void init_mac_address(void) {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    tud_network_mac_address[0] = RP2040_USB_PORTAL_GLOBAL_MAC ? 0x00 : 0x02;
    for (uint8_t i = 1; i < sizeof(tud_network_mac_address); i++) {
        tud_network_mac_address[i] = board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - i];
    }
}

__attribute__((weak)) uint16_t board_usb_get_serial(uint16_t *desc_str, uint16_t max_chars) {
    static const char hex[] = "0123456789ABCDEF";
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    uint16_t count = 0;
    for (uint8_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES && count + 2 <= max_chars; i++) {
        desc_str[count++] = hex[(board_id.id[i] >> 4) & 0x0f];
        desc_str[count++] = hex[board_id.id[i] & 0x0f];
    }
    return count;
}

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p) {
    (void)netif;

    while (true) {
        if (!tud_ready()) {
            return ERR_USE;
        }

        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }

        tud_task();
    }
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr) {
    return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif) {
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->name[0] = 'U';
    netif->name[1] = 'S';
    netif->linkoutput = linkoutput_fn;
    netif->output = ip4_output_fn;
    return ERR_OK;
}

static void init_netif(void) {
    struct netif *netif = &netif_data;

    memset(netif, 0, sizeof(*netif));
    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    netif->hwaddr[5] ^= 0x01;

    netif = netif_add(netif, &active_ipaddr, &active_netmask, &active_gateway, NULL, netif_init_cb, ip_input);
    netif_set_default(netif);
    netif_set_up(netif);
    netif_set_link_up(netif);
}

static bool dns_query_proc(const char *name, ip4_addr_t *addr) {
    (void)name;
    *addr = active_ipaddr;
    return true;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (received_frame) {
        return false;
    }

    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            memcpy(p->payload, src, size);
            received_frame = p;
        }
    }

    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void) {
    if (received_frame) {
        pbuf_free(received_frame);
        received_frame = NULL;
    }
}

static void service_traffic(void) {
    if (received_frame) {
        if (ethernet_input(received_frame, &netif_data) != ERR_OK) {
            pbuf_free(received_frame);
        }
        received_frame = NULL;
        tud_network_recv_renew();
    }

    sys_check_timeouts();
}

static void service_serial(void) {
#if CFG_TUD_CDC
    uint8_t buffer[64];

    while (tud_cdc_available()) {
        uint32_t count = tud_cdc_read(buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }

        if (active_config.serial_rx_handler) {
            active_config.serial_rx_handler(buffer, count, active_config.serial_user_data);
        } else {
            for (uint32_t i = 0; i < count; i++) {
                serial_rx_push(buffer[i]);
            }
        }
    }

    tud_cdc_write_flush();
#endif
}

static err_t http_close_connection(struct tcp_pcb *pcb) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);

    err_t err = tcp_close(pcb);
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len) {
    (void)arg;
    (void)len;
    return http_close_connection(pcb);
}

static err_t http_poll_cb(void *arg, struct tcp_pcb *pcb) {
    (void)arg;
    return http_close_connection(pcb);
}

static err_t finish_response(struct tcp_pcb *pcb) {
    tcp_sent(pcb, http_sent_cb);
    tcp_poll(pcb, http_poll_cb, 2);
    err_t err = tcp_output(pcb);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    return ERR_OK;
}

static err_t write_response(struct tcp_pcb *pcb, const char *data, uint32_t len) {
    uint32_t written = 0;
    while (written < len) {
        uint32_t remaining = len - written;
        uint16_t chunk_len = remaining > 0xffffu ? 0xffffu : (uint16_t)remaining;
        err_t err = tcp_write(pcb, data + written, chunk_len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            return err;
        }
        written += chunk_len;
    }
    return ERR_OK;
}

static err_t send_response(struct tcp_pcb *pcb, const rp2040_usb_portal_http_response_t *response) {
    const char *status = response && response->status ? response->status : "200 OK";
    const char *content_type = response && response->content_type ? response->content_type : "text/plain";
    const char *body = response && response->body ? response->body : "";
    uint32_t body_len = response && response->body_len ? response->body_len : (uint32_t)strlen(body);

    char header[192];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %lu\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n\r\n",
                              status, content_type, (unsigned long)body_len);

    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return http_close_connection(pcb);
    }

    err_t err = write_response(pcb, header, (uint32_t)header_len);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    err = write_response(pcb, body, body_len);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    return finish_response(pcb);
}

static err_t send_redirect(struct tcp_pcb *pcb) {
    char generated_url[40];
    const char *url = active_config.default_redirect_url;
    if (!url) {
        char ip[16];
        rp2040_usb_portal_ipv4_to_string(active_config.address, ip, sizeof(ip));
        snprintf(generated_url, sizeof(generated_url), "http://%s/", ip);
        url = generated_url;
    }
    char body[128];
    char header[256];
    int body_len = snprintf(body, sizeof(body), "Redirecting to %s\n", url);
    if (body_len < 0) {
        body_len = 0;
    }
    if ((size_t)body_len >= sizeof(body)) {
        body_len = (int)sizeof(body) - 1;
        body[body_len] = '\0';
    }

    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 302 Found\r\n"
                              "Location: %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n\r\n",
                              url, (unsigned)body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        return http_close_connection(pcb);
    }

    err_t err = write_response(pcb, header, (uint32_t)header_len);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    err = write_response(pcb, body, (uint32_t)body_len);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    return finish_response(pcb);
}

static err_t send_portal_state(struct tcp_pcb *pcb) {
    char ip[16];
    char json[224];
    rp2040_usb_portal_ipv4_to_string(active_config.address, ip, sizeof(ip));
    snprintf(json, sizeof(json),
             "{\"ip\":\"%s\",\"requests\":%lu,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\n",
             ip,
             (unsigned long)http_request_count,
             tud_network_mac_address[0], tud_network_mac_address[1],
             tud_network_mac_address[2], tud_network_mac_address[3],
             tud_network_mac_address[4], tud_network_mac_address[5]);

    rp2040_usb_portal_http_response_t response = {
        .status = "200 OK",
        .content_type = "application/json",
        .body = json,
    };
    return send_response(pcb, &response);
}

static bool parse_http_request(const char *raw,
                               rp2040_usb_portal_http_request_t *request,
                               char *method,
                               size_t method_len,
                               char *target,
                               size_t target_len) {
    if (!raw || !request || !method || !target || method_len == 0 || target_len == 0) {
        return false;
    }

    const char *method_end = strchr(raw, ' ');
    if (!method_end) {
        return false;
    }
    size_t method_count = (size_t)(method_end - raw);
    if (method_count >= method_len) {
        method_count = method_len - 1;
    }
    memcpy(method, raw, method_count);
    method[method_count] = '\0';

    const char *target_start = method_end + 1;
    const char *target_end = strchr(target_start, ' ');
    if (!target_end) {
        return false;
    }
    size_t target_count = (size_t)(target_end - target_start);
    if (target_count >= target_len) {
        target_count = target_len - 1;
    }
    memcpy(target, target_start, target_count);
    target[target_count] = '\0';

    char *query = strchr(target, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    request->method = method;
    request->path = target;
    request->query = query ? query : "";
    request->raw = raw;
    return true;
}

static err_t handle_http_request(struct tcp_pcb *pcb, const char *raw_request) {
    http_request_count++;

    char method[8];
    char target[192];
    rp2040_usb_portal_http_request_t request;
    if (!parse_http_request(raw_request, &request, method, sizeof(method), target, sizeof(target))) {
        rp2040_usb_portal_http_response_t response = {
            .status = "400 Bad Request",
            .content_type = "text/plain",
            .body = "Bad request\n",
        };
        return send_response(pcb, &response);
    }

    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/api/bootloader") == 0) {
        rp2040_usb_portal_reboot_to_bootsel(250);
        rp2040_usb_portal_http_response_t response = {
            .status = "200 OK",
            .content_type = "text/plain",
            .body = "Rebooting to BOOTSEL\n",
        };
        return send_response(pcb, &response);
    }

    if (strcmp(request.method, "GET") == 0 && strcmp(request.path, "/api/portal/state") == 0) {
        return send_portal_state(pcb);
    }

    if (active_config.http_handler) {
        rp2040_usb_portal_http_response_t response = {
            .status = "200 OK",
            .content_type = "text/plain",
            .body = "",
        };
        if (active_config.http_handler(&request, &response, active_config.http_user_data)) {
            return send_response(pcb, &response);
        }
    }

    return send_redirect(pcb);
}

static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;

    if (!p || err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        return http_close_connection(pcb);
    }

    char request[384];
    uint16_t copy_len = p->tot_len < sizeof(request) - 1 ? p->tot_len : sizeof(request) - 1;
    pbuf_copy_partial(p, request, copy_len, 0);
    request[copy_len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    return handle_http_request(pcb, request);
}

static err_t http_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    tcp_recv(newpcb, http_recv_cb);
    tcp_nagle_disable(newpcb);
    return ERR_OK;
}

static bool http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        return false;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_abort(pcb);
        return false;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        return false;
    }
    tcp_accept(pcb, http_accept_cb);
    return true;
}

#if USB_NET_CROSS_PLATFORM
static void service_adaptive_usb_persona(void) {
    if (adaptive_switched_to_windows || !usb_net_adaptive_windows_probe_seen()) {
        return;
    }

    adaptive_switched_to_windows = true;
    tud_disconnect();
    sleep_ms(250);
    usb_net_adaptive_use_windows_persona();
    tud_connect();
}
#endif

bool rp2040_usb_portal_init(const rp2040_usb_portal_config_t *config) {
    if (portal_initialized) {
        return true;
    }

    if (config) {
        active_config = *config;
    } else {
        rp2040_usb_portal_config_init(&active_config);
    }

    init_mac_address();

    if (active_config.initialize_tinyusb) {
        tusb_rhport_init_t dev_init = {
            .role = TUSB_ROLE_DEVICE,
            .speed = TUSB_SPEED_AUTO,
        };
        tusb_init(BOARD_TUD_RHPORT, &dev_init);
    }

    if (active_config.initialize_lwip) {
        lwip_init();
    }

    active_ipaddr = ip4_from_portal(active_config.address);
    active_netmask = ip4_from_portal(active_config.netmask);
    active_gateway = ip4_from_portal(active_config.gateway);
    init_netif();

    if (active_config.enable_dhcp_server) {
        memset(dhcp_entries, 0, sizeof(dhcp_entries));
        uint8_t lease_count = active_config.dhcp_lease_count;
        if (lease_count > RP2040_USB_PORTAL_DHCP_LEASE_COUNT) {
            lease_count = RP2040_USB_PORTAL_DHCP_LEASE_COUNT;
        }
        for (uint8_t i = 0; i < lease_count; i++) {
            dhcp_entries[i].addr = ip4_from_portal(ipv4_add_host(active_config.dhcp_lease_start, i));
            dhcp_entries[i].lease = active_config.dhcp_lease_seconds;
        }

        ip4_addr_t router = active_config.advertise_gateway ?
                            ip4_from_portal(active_config.dhcp_router) :
                            ip4_from_portal(RP2040_USB_PORTAL_IPV4(0, 0, 0, 0));
        dhcp_config.router = router;
        dhcp_config.port = active_config.dhcp_port;
        dhcp_config.dns = ip4_from_portal(active_config.dns_server);
        dhcp_config.domain = active_config.dhcp_domain;
        dhcp_config.num_entry = lease_count;
        dhcp_config.entries = dhcp_entries;

        if (dhserv_init(&dhcp_config) != ERR_OK) {
            return false;
        }
    }
    if (active_config.enable_dns_catchall &&
        dnserv_init(IP_ADDR_ANY, active_config.dns_port, dns_query_proc) != ERR_OK) {
        return false;
    }
    if (active_config.enable_http_server && !http_server_init()) {
        return false;
    }

    portal_initialized = true;
    return true;
}

void rp2040_usb_portal_task(void) {
    if (!portal_initialized) {
        return;
    }

    tud_task();
#if USB_NET_CROSS_PLATFORM
    service_adaptive_usb_persona();
#endif
    service_serial();
    service_traffic();

    if (bootloader_requested &&
        (int32_t)(to_ms_since_boot(get_absolute_time()) - bootloader_reset_at_ms) >= 0) {
        reset_usb_boot(0, 0);
    }
}

void rp2040_usb_portal_reboot_to_bootsel(uint32_t delay_ms) {
    bootloader_requested = true;
    bootloader_reset_at_ms = to_ms_since_boot(get_absolute_time()) + delay_ms;
}

struct netif *rp2040_usb_portal_netif(void) {
    return &netif_data;
}

uint32_t rp2040_usb_portal_http_request_count(void) {
    return http_request_count;
}

const uint8_t *rp2040_usb_portal_mac_address(void) {
    return tud_network_mac_address;
}

bool rp2040_usb_portal_serial_connected(void) {
#if CFG_TUD_CDC
    return tud_ready() && tud_cdc_connected();
#else
    return false;
#endif
}

size_t rp2040_usb_portal_serial_read(void *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        return 0;
    }

#if CFG_TUD_CDC
    uint8_t *out = (uint8_t *)buffer;
    size_t count = 0;
    while (count < buffer_len && serial_rx_pop(&out[count])) {
        count++;
    }
    return count;
#else
    return 0;
#endif
}

size_t rp2040_usb_portal_serial_write(const void *data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }

#if CFG_TUD_CDC
    if (!tud_ready() || !tud_cdc_connected()) {
        return 0;
    }

    uint32_t written = tud_cdc_write(data, (uint32_t)len);
    tud_cdc_write_flush();
    return written;
#else
    return 0;
#endif
}

#if CFG_TUD_CDC
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;

    if (itf != 0 || dtr || !active_config.enable_serial_bootloader_touch) {
        return;
    }

    cdc_line_coding_t coding;
    tud_cdc_n_get_line_coding(itf, &coding);
    if (coding.bit_rate == 1200) {
        rp2040_usb_portal_reboot_to_bootsel(50);
    }
}
#endif

__attribute__((weak)) sys_prot_t sys_arch_protect(void) {
    return 0;
}

__attribute__((weak)) void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

__attribute__((weak)) uint32_t sys_now(void) {
    return to_ms_since_boot(get_absolute_time());
}
