#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"

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
#include "tusb.h"

#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

#ifndef CONTROL_GPIO
#define CONTROL_GPIO 10u
#endif

#ifndef USB_NET_GLOBAL_MAC
#define USB_NET_GLOBAL_MAC 0
#endif

#ifndef USB_NET_ADVERTISE_GATEWAY
#define USB_NET_ADVERTISE_GATEWAY 0
#endif

#if USB_NET_ADVERTISE_GATEWAY
#define DHCP_ROUTER_IP INIT_IP4(192, 168, 4, 1)
#else
#define DHCP_ROUTER_IP INIT_IP4(0, 0, 0, 0)
#endif

#if USB_NET_CROSS_PLATFORM
bool usb_net_adaptive_windows_probe_seen(void);
void usb_net_adaptive_use_windows_persona(void);
#endif

static struct netif netif_data;
static struct pbuf *received_frame;
static bool control_gpio_high;
static bool bootloader_requested;
static uint32_t bootloader_reset_at_ms;
static uint32_t http_request_count;
#if USB_NET_CROSS_PLATFORM
static bool adaptive_switched_to_windows;
#endif

uint8_t tud_network_mac_address[6] = {0x02, 0x52, 0x50, 0x32, 0x30, 0x40};

static const ip4_addr_t ipaddr = INIT_IP4(192, 168, 4, 1);
static const ip4_addr_t netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t dhcp_entries[] = {
    {{0}, INIT_IP4(192, 168, 4, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 4, 3), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 4, 4), 24 * 60 * 60},
};

static const dhcp_config_t dhcp_config = {
    .router = DHCP_ROUTER_IP,
    .port = 67,
    .dns = INIT_IP4(192, 168, 4, 1),
    .domain = "rp2040.local",
    .num_entry = LWIP_ARRAYSIZE(dhcp_entries),
    .entries = dhcp_entries,
};

static const char index_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>RP2040 Control</title><style>"
":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#17202a;background:#f6f7f9}"
"body{margin:0}.wrap{max-width:860px;margin:0 auto;padding:28px 18px}"
"header{display:flex;align-items:end;justify-content:space-between;gap:16px;border-bottom:1px solid #d9dee5;padding-bottom:18px}"
"h1{font-size:28px;margin:0}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;margin-top:18px}"
".panel{background:#fff;border:1px solid #d9dee5;border-radius:8px;padding:18px;box-shadow:0 1px 3px #0001}"
".label{color:#667085;font-size:13px;margin-bottom:5px}.value{font-size:28px;font-weight:650}"
"button{appearance:none;border:0;border-radius:6px;background:#0f766e;color:white;font-size:16px;font-weight:650;padding:12px 16px;cursor:pointer}"
"button.off{background:#374151}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
"</style></head><body><div class=\"wrap\"><header><div><h1>RP2040 Control</h1>"
"<div class=\"label mono\">192.168.4.1</div></div><button id=\"toggle\">Loading</button></header><main>"
"<section class=\"panel\"><div class=\"label\">Control GPIO</div><div id=\"gpio\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">Uptime</div><div id=\"uptime\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">HTTP requests</div><div id=\"requests\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">Device MAC</div><div id=\"mac\" class=\"value mono\">-</div></section>"
"</main></div><script>"
"const $=id=>document.getElementById(id);"
"async function state(){const r=await fetch('/api/state',{cache:'no-store'});return r.json()}"
"function draw(s){$('gpio').textContent=s.gpioHigh?'HIGH':'LOW';$('uptime').textContent=Math.floor(s.uptimeMs/1000)+' s';"
"$('requests').textContent=s.requests;$('mac').textContent=s.mac;const b=$('toggle');b.textContent=s.gpioHigh?'Set low':'Set high';b.className=s.gpioHigh?'off':''}"
"async function refresh(){try{draw(await state())}catch(e){}}"
"$('toggle').onclick=async()=>{const s=await state();await fetch('/api/gpio?value='+(s.gpioHigh?0:1),{cache:'no-store'});refresh()};"
"refresh();setInterval(refresh,1000);"
"</script></body></html>";

uint16_t board_usb_get_serial(uint16_t *desc_str, uint16_t max_chars) {
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

static void init_mac_address(void) {
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);

    tud_network_mac_address[0] = USB_NET_GLOBAL_MAC ? 0x00 : 0x02;
    for (uint8_t i = 1; i < sizeof(tud_network_mac_address); i++) {
        tud_network_mac_address[i] = board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - i];
    }
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

static void init_lwip(void) {
    struct netif *netif = &netif_data;

    lwip_init();

    netif->hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(netif->hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    netif->hwaddr[5] ^= 0x01;

    netif = netif_add(netif, &ipaddr, &netmask, &gateway, NULL, netif_init_cb, ip_input);
    netif_set_default(netif);
    netif_set_up(netif);
    netif_set_link_up(netif);
}

bool dns_query_proc(const char *name, ip4_addr_t *addr) {
    (void)name;
    *addr = ipaddr;
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

static err_t write_response(struct tcp_pcb *pcb, const char *data, uint16_t len) {
    return tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY);
}

static err_t send_response(struct tcp_pcb *pcb, const char *status, const char *content_type, const char *body) {
    char header[192];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n"
                              "Cache-Control: no-store\r\n\r\n",
                              status, content_type, (unsigned)strlen(body));

    if (header_len > 0) {
        err_t err = write_response(pcb, header, (uint16_t)header_len);
        if (err != ERR_OK) {
            return http_close_connection(pcb);
        }
    }
    err_t err = write_response(pcb, body, (uint16_t)strlen(body));
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    return finish_response(pcb);
}

static err_t send_redirect(struct tcp_pcb *pcb) {
    static const char body[] = "Redirecting to http://192.168.4.1/\n";
    static const char header[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: http://192.168.4.1/\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 35\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n\r\n";

    err_t err = write_response(pcb, header, sizeof(header) - 1);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    err = write_response(pcb, body, sizeof(body) - 1);
    if (err != ERR_OK) {
        return http_close_connection(pcb);
    }
    return finish_response(pcb);
}

static err_t send_state(struct tcp_pcb *pcb) {
    char json[192];
    snprintf(json, sizeof(json),
             "{\"gpio\":%u,\"gpioHigh\":%s,\"uptimeMs\":%lu,\"requests\":%lu,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\n",
             CONTROL_GPIO,
             control_gpio_high ? "true" : "false",
             (unsigned long)to_ms_since_boot(get_absolute_time()),
             (unsigned long)http_request_count,
             tud_network_mac_address[0], tud_network_mac_address[1],
             tud_network_mac_address[2], tud_network_mac_address[3],
             tud_network_mac_address[4], tud_network_mac_address[5]);
    return send_response(pcb, "200 OK", "application/json", json);
}

static err_t handle_http_request(struct tcp_pcb *pcb, const char *request) {
    http_request_count++;

    if (strncmp(request, "GET /api/state", 14) == 0) {
        return send_state(pcb);
    }

    if (strncmp(request, "GET /api/gpio?value=1", 21) == 0) {
        control_gpio_high = true;
        gpio_put(CONTROL_GPIO, 1);
        return send_state(pcb);
    }

    if (strncmp(request, "GET /api/gpio?value=0", 21) == 0) {
        control_gpio_high = false;
        gpio_put(CONTROL_GPIO, 0);
        return send_state(pcb);
    }

    if (strncmp(request, "GET /api/bootloader", 19) == 0) {
        bootloader_requested = true;
        bootloader_reset_at_ms = to_ms_since_boot(get_absolute_time()) + 250;
        return send_response(pcb, "200 OK", "text/plain", "Rebooting to BOOTSEL\n");
    }

    if (strncmp(request, "GET / ", 6) == 0 || strncmp(request, "GET /?", 6) == 0 ||
        strncmp(request, "GET /index.html", 15) == 0) {
        return send_response(pcb, "200 OK", "text/html; charset=utf-8", index_html);
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

static void http_server_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        return;
    }

    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_abort(pcb);
        return;
    }

    pcb = tcp_listen(pcb);
    if (!pcb) {
        return;
    }
    tcp_accept(pcb, http_accept_cb);
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

int main(void) {
    gpio_init(CONTROL_GPIO);
    gpio_set_dir(CONTROL_GPIO, GPIO_OUT);
    gpio_put(CONTROL_GPIO, 0);

    init_mac_address();
    stdio_init_all();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    init_lwip();
    while (dhserv_init(&dhcp_config) != ERR_OK) {
        tight_loop_contents();
    }
    while (dnserv_init(IP_ADDR_ANY, 53, dns_query_proc) != ERR_OK) {
        tight_loop_contents();
    }
    http_server_init();

    while (true) {
        tud_task();
#if USB_NET_CROSS_PLATFORM
        service_adaptive_usb_persona();
#endif
        service_traffic();

        if (bootloader_requested &&
            (int32_t)(to_ms_since_boot(get_absolute_time()) - bootloader_reset_at_ms) >= 0) {
            reset_usb_boot(0, 0);
        }
    }
}

sys_prot_t sys_arch_protect(void) {
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
}

uint32_t sys_now(void) {
    return to_ms_since_boot(get_absolute_time());
}
