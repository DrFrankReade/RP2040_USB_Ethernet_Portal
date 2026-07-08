#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "rp2040_usb_ethernet_portal.h"

#ifndef PICO_DEFAULT_LED_PIN_INVERTED
#define PICO_DEFAULT_LED_PIN_INVERTED 0
#endif

#if defined(PORTAL_EXAMPLE_LED_GPIO)
#define EXAMPLE_HAS_LED_GPIO 1
#define EXAMPLE_LED_GPIO ((uint)PORTAL_EXAMPLE_LED_GPIO)
#ifndef PORTAL_EXAMPLE_LED_INVERTED
#define PORTAL_EXAMPLE_LED_INVERTED 0
#endif
#define EXAMPLE_LED_INVERTED PORTAL_EXAMPLE_LED_INVERTED
#elif defined(PICO_DEFAULT_LED_PIN)
#define EXAMPLE_HAS_LED_GPIO 1
#define EXAMPLE_LED_GPIO ((uint)PICO_DEFAULT_LED_PIN)
#define EXAMPLE_LED_INVERTED PICO_DEFAULT_LED_PIN_INVERTED
#else
#define EXAMPLE_HAS_LED_GPIO 0
#endif

typedef enum {
    EXAMPLE_PIN_INPUT,
    EXAMPLE_PIN_OUTPUT,
    EXAMPLE_PIN_ANALOG,
} example_pin_mode_t;

typedef struct {
    uint gpio;
    example_pin_mode_t mode;
    bool inverted;
    const char *name;
} example_pin_t;

#if EXAMPLE_HAS_LED_GPIO
static example_pin_t example_pins[] = {
    {.gpio = EXAMPLE_LED_GPIO, .mode = EXAMPLE_PIN_OUTPUT, .inverted = EXAMPLE_LED_INVERTED, .name = "board_led"},
};
#else
static example_pin_t example_pins[1];
#endif

static const size_t example_pin_count = EXAMPLE_HAS_LED_GPIO ? 1u : 0u;
static char api_body[1024];

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
"button.off{background:#374151}button:disabled{background:#9ca3af;cursor:not-allowed}.mono{font-family:ui-monospace,SFMono-Regular,Consolas,monospace}"
"</style></head><body><div class=\"wrap\"><header><div><h1>RP2040 Control</h1>"
"<div class=\"label mono\" id=\"ip\">192.168.4.1</div></div><button id=\"toggle\" disabled>Loading</button></header><main>"
"<section class=\"panel\"><div class=\"label\">Configured output</div><div id=\"gpio\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">Output state</div><div id=\"state\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">Uptime</div><div id=\"uptime\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">HTTP requests</div><div id=\"requests\" class=\"value\">-</div></section>"
"<section class=\"panel\"><div class=\"label\">Device MAC</div><div id=\"mac\" class=\"value mono\">-</div></section>"
"</main></div><script>"
"const $=id=>document.getElementById(id);let outputPin=null;"
"async function state(){const r=await fetch('/api/state',{cache:'no-store'});return r.json()}"
"function draw(s){$('ip').textContent=s.portal.ip;const outputs=s.pins.filter(p=>p.mode==='out');outputPin=outputs[0]||null;"
"$('gpio').textContent=outputPin?outputPin.name+' GPIO '+outputPin.gpio:'none';$('state').textContent=outputPin?(outputPin.value?'HIGH':'LOW'):'-';"
"$('uptime').textContent=Math.floor(s.uptimeMs/1000)+' s';$('requests').textContent=s.portal.requests;$('mac').textContent=s.portal.mac;"
"const b=$('toggle');b.disabled=!outputPin;b.textContent=outputPin?(outputPin.value?'Set low':'Set high'):'No output';b.className=outputPin&&outputPin.value?'off':''}"
"async function refresh(){try{draw(await state())}catch(e){}}"
"$('toggle').onclick=async()=>{if(!outputPin)return;await fetch('/api/gpio?pin='+outputPin.gpio+'&value='+(outputPin.value?0:1),{cache:'no-store'});refresh()};"
"refresh();setInterval(refresh,1000);"
"</script></body></html>";

static const char *pin_mode_name(example_pin_mode_t mode) {
    switch (mode) {
        case EXAMPLE_PIN_INPUT:
            return "in";
        case EXAMPLE_PIN_OUTPUT:
            return "out";
        case EXAMPLE_PIN_ANALOG:
            return "analog";
        default:
            return "unknown";
    }
}

static void example_pin_write(const example_pin_t *pin, bool value) {
    if (!pin || pin->mode != EXAMPLE_PIN_OUTPUT) {
        return;
    }
    gpio_put(pin->gpio, pin->inverted ? !value : value);
}

static uint16_t example_pin_read(const example_pin_t *pin) {
    if (!pin) {
        return 0;
    }

    if (pin->mode == EXAMPLE_PIN_ANALOG) {
        if (pin->gpio < 26 || pin->gpio > 29) {
            return 0;
        }
        adc_select_input(pin->gpio - 26);
        return adc_read();
    }

    bool value = gpio_get(pin->gpio);
    if (pin->mode == EXAMPLE_PIN_OUTPUT || pin->mode == EXAMPLE_PIN_INPUT) {
        value = pin->inverted ? !value : value;
    }
    return value ? 1u : 0u;
}

static void init_example_pins(void) {
    bool adc_needed = false;
    for (size_t i = 0; i < example_pin_count; i++) {
        example_pin_t *pin = &example_pins[i];
        if (pin->mode == EXAMPLE_PIN_ANALOG) {
            if (pin->gpio >= 26 && pin->gpio <= 29) {
                adc_needed = true;
                adc_gpio_init(pin->gpio);
            }
            continue;
        }

        gpio_init(pin->gpio);
        gpio_set_dir(pin->gpio, pin->mode == EXAMPLE_PIN_OUTPUT);
        if (pin->mode == EXAMPLE_PIN_OUTPUT) {
            example_pin_write(pin, false);
        }
    }

    if (adc_needed) {
        adc_init();
    }
}

static example_pin_t *find_pin(uint gpio) {
    for (size_t i = 0; i < example_pin_count; i++) {
        if (example_pins[i].gpio == gpio) {
            return &example_pins[i];
        }
    }
    return NULL;
}

static example_pin_t *first_output_pin(void) {
    for (size_t i = 0; i < example_pin_count; i++) {
        if (example_pins[i].mode == EXAMPLE_PIN_OUTPUT) {
            return &example_pins[i];
        }
    }
    return NULL;
}

static bool query_uint(const char *query, const char *key, uint *value) {
    size_t key_len = strlen(key);
    const char *cursor = query ? query : "";

    while (*cursor) {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            cursor += key_len + 1;
            uint parsed = 0;
            bool saw_digit = false;
            while (*cursor >= '0' && *cursor <= '9') {
                saw_digit = true;
                parsed = parsed * 10u + (uint)(*cursor - '0');
                cursor++;
            }
            if (saw_digit) {
                *value = parsed;
                return true;
            }
            return false;
        }

        cursor = strchr(cursor, '&');
        if (!cursor) {
            return false;
        }
        cursor++;
    }

    return false;
}

static bool set_json_response(rp2040_usb_portal_http_response_t *response,
                              const char *status,
                              const char *json) {
    response->status = status;
    response->content_type = "application/json";
    response->body = json;
    return true;
}

static bool send_error(rp2040_usb_portal_http_response_t *response, const char *status, const char *message) {
    snprintf(api_body, sizeof(api_body), "{\"error\":\"%s\"}\n", message);
    return set_json_response(response, status, api_body);
}

static void append_pin_json(char *body, size_t body_len, size_t *used, const example_pin_t *pin) {
    uint16_t value = example_pin_read(pin);
    int count = snprintf(body + *used, body_len - *used,
                         "%s{\"gpio\":%u,\"name\":\"%s\",\"mode\":\"%s\",\"value\":%u}",
                         *used && body[*used - 1] != '[' ? "," : "",
                         pin->gpio, pin->name, pin_mode_name(pin->mode), value);
    if (count > 0) {
        size_t written = (size_t)count;
        if (written >= body_len - *used) {
            *used = body_len - 1;
        } else {
            *used += written;
        }
    }
}

static bool send_state(rp2040_usb_portal_http_response_t *response) {
    rp2040_usb_portal_config_t config;
    char ip[16];
    const uint8_t *mac = rp2040_usb_portal_mac_address();

    rp2040_usb_portal_config_init(&config);
    rp2040_usb_portal_ipv4_to_string(config.address, ip, sizeof(ip));

    size_t used = 0;
    int count = snprintf(api_body, sizeof(api_body),
                         "{\"portal\":{\"ip\":\"%s\",\"requests\":%lu,"
                         "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"},"
                         "\"uptimeMs\":%lu,\"pins\":[",
                         ip,
                         (unsigned long)rp2040_usb_portal_http_request_count(),
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                         (unsigned long)to_ms_since_boot(get_absolute_time()));
    if (count > 0) {
        used = (size_t)count < sizeof(api_body) ? (size_t)count : sizeof(api_body) - 1;
    }

    for (size_t i = 0; i < example_pin_count && used < sizeof(api_body) - 4; i++) {
        append_pin_json(api_body, sizeof(api_body), &used, &example_pins[i]);
    }
    snprintf(api_body + used, sizeof(api_body) - used, "]}\n");

    return set_json_response(response, "200 OK", api_body);
}

static bool handle_gpio_request(const rp2040_usb_portal_http_request_t *request,
                                rp2040_usb_portal_http_response_t *response) {
    uint pin_number = 0;
    uint value = 0;
    bool has_pin = query_uint(request->query, "pin", &pin_number);
    bool has_value = query_uint(request->query, "value", &value);
    example_pin_t *pin = has_pin ? find_pin(pin_number) : first_output_pin();

    if (!pin) {
        return send_error(response, "404 Not Found", "pin is not registered");
    }

    if (has_value) {
        if (pin->mode != EXAMPLE_PIN_OUTPUT) {
            return send_error(response, "403 Forbidden", "pin is not configured as an output");
        }
        if (value > 1) {
            return send_error(response, "400 Bad Request", "output value must be 0 or 1");
        }
        example_pin_write(pin, value != 0);
    }

    uint16_t read_value = example_pin_read(pin);
    snprintf(api_body, sizeof(api_body),
             "{\"gpio\":%u,\"name\":\"%s\",\"mode\":\"%s\",\"value\":%u}\n",
             pin->gpio, pin->name, pin_mode_name(pin->mode), read_value);
    return set_json_response(response, "200 OK", api_body);
}

static bool example_http_handler(const rp2040_usb_portal_http_request_t *request,
                                 rp2040_usb_portal_http_response_t *response,
                                 void *user_data) {
    (void)user_data;

    if (strcmp(request->method, "GET") != 0) {
        return send_error(response, "405 Method Not Allowed", "method not allowed");
    }

    if (strcmp(request->path, "/") == 0 ||
        strcmp(request->path, "/index.html") == 0) {
        response->status = "200 OK";
        response->content_type = "text/html; charset=utf-8";
        response->body = index_html;
        return true;
    }

    if (strcmp(request->path, "/api/state") == 0 ||
        strcmp(request->path, "/api/pins") == 0) {
        return send_state(response);
    }

    if (strcmp(request->path, "/api/gpio") == 0) {
        return handle_gpio_request(request, response);
    }

    return false;
}

int main(void) {
    stdio_init_all();
    init_example_pins();

    rp2040_usb_portal_config_t config;
    rp2040_usb_portal_config_init(&config);
    config.http_handler = example_http_handler;

    if (!rp2040_usb_portal_init(&config)) {
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        rp2040_usb_portal_task();
    }
}
