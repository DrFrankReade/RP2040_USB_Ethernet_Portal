#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/adc.h"
#include "pico/stdlib.h"
#include "rp2040_usb_ethernet_portal.h"

/*
 * Demo LED selection:
 * - platformio.ini chooses the board.
 * - The Pico SDK board header supplies PICO_DEFAULT_LED_PIN when it knows one.
 * - A project can override both values with build flags if needed.
 */
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

#ifndef PORTAL_EXAMPLE_SERIAL_DEBUG
#define PORTAL_EXAMPLE_SERIAL_DEBUG 1
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

/*
 * Hardware exposure list for this example.
 *
 * The HTTP API only reads or writes pins in this table. Extend this table
 * deliberately with a mode and polarity instead of accepting arbitrary GPIO
 * numbers from the browser.
 */
#if EXAMPLE_HAS_LED_GPIO
static example_pin_t example_pins[] = {
    {
        .gpio = EXAMPLE_LED_GPIO,
        .mode = EXAMPLE_PIN_OUTPUT,
        .inverted = EXAMPLE_LED_INVERTED,
        .name = "board_led",
    },
};
#else
static example_pin_t example_pins[1];
#endif

static const size_t example_pin_count = EXAMPLE_HAS_LED_GPIO ? 1u : 0u;
static char json_response_buffer[1024];
static char serial_command_buffer[64];
static size_t serial_command_len;
static bool serial_console_was_connected;
static bool serial_debug_enabled = PORTAL_EXAMPLE_SERIAL_DEBUG != 0;

/*
 * A self-contained page keeps the demo easy to flash and inspect. Production
 * projects can serve different routes from the same HTTP callback.
 */
static const char portal_page_html[] =
    "<!doctype html>\n"
    "<html lang='en'>\n"
    "<head>\n"
    "  <meta charset='utf-8'>\n"
    "  <meta name='viewport' content='width=device-width,initial-scale=1'>\n"
    "  <title>RP2040 Control</title>\n"
    "  <style>\n"
    "    :root {\n"
    "      font-family: system-ui, -apple-system, Segoe UI, sans-serif;\n"
    "      color: #17202a;\n"
    "      background: #f6f7f9;\n"
    "    }\n"
    "    body { margin: 0; }\n"
    "    .wrap { max-width: 860px; margin: 0 auto; padding: 28px 18px; }\n"
    "    header {\n"
    "      display: flex;\n"
    "      align-items: end;\n"
    "      justify-content: space-between;\n"
    "      gap: 16px;\n"
    "      border-bottom: 1px solid #d9dee5;\n"
    "      padding-bottom: 18px;\n"
    "    }\n"
    "    h1 { font-size: 28px; margin: 0; }\n"
    "    main {\n"
    "      display: grid;\n"
    "      grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));\n"
    "      gap: 14px;\n"
    "      margin-top: 18px;\n"
    "    }\n"
    "    .panel {\n"
    "      background: #fff;\n"
    "      border: 1px solid #d9dee5;\n"
    "      border-radius: 8px;\n"
    "      padding: 18px;\n"
    "      box-shadow: 0 1px 3px #0001;\n"
    "    }\n"
    "    .label { color: #667085; font-size: 13px; margin-bottom: 5px; }\n"
    "    .value { font-size: 28px; font-weight: 650; }\n"
    "    button {\n"
    "      appearance: none;\n"
    "      border: 0;\n"
    "      border-radius: 6px;\n"
    "      background: #0f766e;\n"
    "      color: white;\n"
    "      cursor: pointer;\n"
    "      font-size: 16px;\n"
    "      font-weight: 650;\n"
    "      padding: 12px 16px;\n"
    "    }\n"
    "    button.off { background: #374151; }\n"
    "    button:disabled { background: #9ca3af; cursor: not-allowed; }\n"
    "    .mono { font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class='wrap'>\n"
    "    <header>\n"
    "      <div>\n"
    "        <h1>RP2040 Control</h1>\n"
    "        <div class='label mono' id='ip'>192.168.4.1</div>\n"
    "      </div>\n"
    "      <button id='toggle' disabled>Loading</button>\n"
    "    </header>\n"
    "    <main>\n"
    "      <section class='panel'>\n"
    "        <div class='label'>Configured output</div>\n"
    "        <div id='gpio' class='value'>-</div>\n"
    "      </section>\n"
    "      <section class='panel'>\n"
    "        <div class='label'>Output state</div>\n"
    "        <div id='state' class='value'>-</div>\n"
    "      </section>\n"
    "      <section class='panel'>\n"
    "        <div class='label'>Uptime</div>\n"
    "        <div id='uptime' class='value'>-</div>\n"
    "      </section>\n"
    "      <section class='panel'>\n"
    "        <div class='label'>HTTP requests</div>\n"
    "        <div id='requests' class='value'>-</div>\n"
    "      </section>\n"
    "      <section class='panel'>\n"
    "        <div class='label'>Device MAC</div>\n"
    "        <div id='mac' class='value mono'>-</div>\n"
    "      </section>\n"
    "    </main>\n"
    "  </div>\n"
    "  <script>\n"
    "    const byId = (id) => document.getElementById(id);\n"
    "    let outputPin = null;\n"
    "\n"
    "    async function fetchState() {\n"
    "      const response = await fetch('/api/state', { cache: 'no-store' });\n"
    "      return response.json();\n"
    "    }\n"
    "\n"
    "    function renderState(state) {\n"
    "      byId('ip').textContent = state.portal.ip;\n"
    "\n"
    "      const outputs = state.pins.filter((pin) => pin.mode === 'out');\n"
    "      outputPin = outputs[0] || null;\n"
    "\n"
    "      byId('gpio').textContent = outputPin ? `${outputPin.name} GPIO ${outputPin.gpio}` : 'none';\n"
    "      byId('state').textContent = outputPin ? (outputPin.value ? 'HIGH' : 'LOW') : '-';\n"
    "      byId('uptime').textContent = `${Math.floor(state.uptimeMs / 1000)} s`;\n"
    "      byId('requests').textContent = state.portal.requests;\n"
    "      byId('mac').textContent = state.portal.mac;\n"
    "\n"
    "      const button = byId('toggle');\n"
    "      button.disabled = !outputPin;\n"
    "      button.textContent = outputPin ? (outputPin.value ? 'Set low' : 'Set high') : 'No output';\n"
    "      button.className = outputPin && outputPin.value ? 'off' : '';\n"
    "    }\n"
    "\n"
    "    async function refresh() {\n"
    "      try {\n"
    "        renderState(await fetchState());\n"
    "      } catch (error) {\n"
    "      }\n"
    "    }\n"
    "\n"
    "    byId('toggle').onclick = async () => {\n"
    "      if (!outputPin) {\n"
    "        return;\n"
    "      }\n"
    "\n"
    "      const nextValue = outputPin.value ? 0 : 1;\n"
    "      await fetch(`/api/gpio?pin=${outputPin.gpio}&value=${nextValue}`, { cache: 'no-store' });\n"
    "      refresh();\n"
    "    };\n"
    "\n"
    "    refresh();\n"
    "    setInterval(refresh, 1000);\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

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

static void write_registered_pin(const example_pin_t *pin, bool logical_value) {
    if (!pin || pin->mode != EXAMPLE_PIN_OUTPUT) {
        return;
    }

    bool electrical_value = pin->inverted ? !logical_value : logical_value;
    gpio_put(pin->gpio, electrical_value);
}

static uint16_t read_registered_pin(const example_pin_t *pin) {
    if (!pin) {
        return 0;
    }

    if (pin->mode == EXAMPLE_PIN_ANALOG) {
        /* RP2040 ADC inputs are GPIO26 through GPIO29. */
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

static void init_registered_pins(void) {
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
            write_registered_pin(pin, false);
        }
    }

    if (adc_needed) {
        adc_init();
    }
}

static example_pin_t *find_registered_pin(uint gpio) {
    for (size_t i = 0; i < example_pin_count; i++) {
        if (example_pins[i].gpio == gpio) {
            return &example_pins[i];
        }
    }
    return NULL;
}

static example_pin_t *first_registered_output_pin(void) {
    for (size_t i = 0; i < example_pin_count; i++) {
        if (example_pins[i].mode == EXAMPLE_PIN_OUTPUT) {
            return &example_pins[i];
        }
    }
    return NULL;
}

static bool query_param_uint(const char *query, const char *key, uint *value) {
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

static bool send_error_response(rp2040_usb_portal_http_response_t *response,
                                const char *status,
                                const char *message) {
    snprintf(json_response_buffer, sizeof(json_response_buffer), "{\"error\":\"%s\"}\n", message);
    return set_json_response(response, status, json_response_buffer);
}

static void append_pin_json(char *body, size_t body_len, size_t *used, const example_pin_t *pin) {
    uint16_t value = read_registered_pin(pin);
    const char *separator = (*used > 0 && body[*used - 1] != '[') ? "," : "";

    int count = snprintf(body + *used, body_len - *used,
                         "%s{\"gpio\":%u,\"name\":\"%s\",\"mode\":\"%s\",\"value\":%u}",
                         separator,
                         pin->gpio,
                         pin->name,
                         pin_mode_name(pin->mode),
                         value);
    if (count > 0) {
        size_t written = (size_t)count;
        if (written >= body_len - *used) {
            *used = body_len - 1;
        } else {
            *used += written;
        }
    }
}

static bool send_state_response(rp2040_usb_portal_http_response_t *response) {
    rp2040_usb_portal_config_t config;
    char ip[16];
    const uint8_t *mac = rp2040_usb_portal_mac_address();

    rp2040_usb_portal_config_init(&config);
    rp2040_usb_portal_ipv4_to_string(config.address, ip, sizeof(ip));

    size_t used = 0;
    int count = snprintf(json_response_buffer, sizeof(json_response_buffer),
                         "{\"portal\":{\"ip\":\"%s\",\"requests\":%lu,"
                         "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"},"
                         "\"uptimeMs\":%lu,\"pins\":[",
                         ip,
                         (unsigned long)rp2040_usb_portal_http_request_count(),
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                         (unsigned long)to_ms_since_boot(get_absolute_time()));
    if (count > 0) {
        used = (size_t)count < sizeof(json_response_buffer) ? (size_t)count : sizeof(json_response_buffer) - 1;
    }

    for (size_t i = 0; i < example_pin_count && used < sizeof(json_response_buffer) - 4; i++) {
        append_pin_json(json_response_buffer, sizeof(json_response_buffer), &used, &example_pins[i]);
    }
    snprintf(json_response_buffer + used, sizeof(json_response_buffer) - used, "]}\n");

    return set_json_response(response, "200 OK", json_response_buffer);
}

static void serial_write_text(const char *text) {
    if (text) {
        rp2040_usb_portal_serial_write(text, strlen(text));
    }
}

static void serial_debugf(const char *format, ...) {
    if (!serial_debug_enabled || !format || !rp2040_usb_portal_serial_connected()) {
        return;
    }

    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[208];
    snprintf(line, sizeof(line), "[%lu ms] %s\r\n",
             (unsigned long)to_ms_since_boot(get_absolute_time()),
             message);
    serial_write_text(line);
}

static void serial_prompt(void) {
    serial_write_text("> ");
}

static void serial_print_help(void) {
    serial_write_text(
        "Commands:\r\n"
        "  help       show this list\r\n"
        "  state      show portal and pin state\r\n"
        "  debug on   enable serial debug events\r\n"
        "  debug off  disable serial debug events\r\n"
        "  led on     set the registered output high\r\n"
        "  led off    set the registered output low\r\n"
        "  bootloader reboot to BOOTSEL\r\n");
}

static void serial_print_state(void) {
    rp2040_usb_portal_config_t config;
    char ip[16];
    char line[192];
    const uint8_t *mac = rp2040_usb_portal_mac_address();

    rp2040_usb_portal_config_init(&config);
    rp2040_usb_portal_ipv4_to_string(config.address, ip, sizeof(ip));

    snprintf(line, sizeof(line),
             "Portal: http://%s/  requests=%lu  uptime=%lu ms\r\n"
             "MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n"
             "Serial debug: %s\r\n",
             ip,
             (unsigned long)rp2040_usb_portal_http_request_count(),
             (unsigned long)to_ms_since_boot(get_absolute_time()),
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             serial_debug_enabled ? "on" : "off");
    serial_write_text(line);

    for (size_t i = 0; i < example_pin_count; i++) {
        const example_pin_t *pin = &example_pins[i];
        snprintf(line, sizeof(line), "%s: GPIO %u %s value=%u\r\n",
                 pin->name,
                 pin->gpio,
                 pin_mode_name(pin->mode),
                 read_registered_pin(pin));
        serial_write_text(line);
    }

    if (example_pin_count == 0) {
        serial_write_text("No registered example pins.\r\n");
    }
}

static char *trim_serial_command(char *command) {
    while (*command == ' ' || *command == '\t') {
        command++;
    }

    char *end = command + strlen(command);
    while (end > command && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }

    return command;
}

static void serial_set_first_output(bool value) {
    example_pin_t *pin = first_registered_output_pin();
    if (!pin) {
        serial_write_text("No registered output pin.\r\n");
        return;
    }

    write_registered_pin(pin, value);

    char line[96];
    snprintf(line, sizeof(line), "%s GPIO %u = %s\r\n",
             pin->name,
             pin->gpio,
             value ? "HIGH" : "LOW");
    serial_write_text(line);
    serial_debugf("serial set %s GPIO %u %s",
                  pin->name,
                  pin->gpio,
                  value ? "HIGH" : "LOW");
}

static void handle_serial_command(char *raw_command) {
    char *command = trim_serial_command(raw_command);

    if (strcmp(command, "help") == 0) {
        serial_print_help();
    } else if (strcmp(command, "state") == 0) {
        serial_print_state();
    } else if (strcmp(command, "debug on") == 0) {
        serial_debug_enabled = true;
        serial_write_text("Serial debug output enabled.\r\n");
    } else if (strcmp(command, "debug off") == 0) {
        serial_debug_enabled = false;
        serial_write_text("Serial debug output disabled.\r\n");
    } else if (strcmp(command, "led on") == 0) {
        serial_set_first_output(true);
    } else if (strcmp(command, "led off") == 0) {
        serial_set_first_output(false);
    } else if (strcmp(command, "bootloader") == 0) {
        serial_debugf("serial requested BOOTSEL reboot");
        serial_write_text("Rebooting to BOOTSEL.\r\n");
        rp2040_usb_portal_reboot_to_bootsel(250);
    } else {
        serial_write_text("Unknown command. Type help.\r\n");
    }
}

static void handle_example_serial_rx(const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;

    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];

        if (ch == '\r' || ch == '\n') {
            if (serial_command_len > 0) {
                serial_command_buffer[serial_command_len] = '\0';
                handle_serial_command(serial_command_buffer);
                serial_command_len = 0;
                serial_prompt();
            }
            continue;
        }

        if (ch == '\b' || ch == 0x7f) {
            if (serial_command_len > 0) {
                serial_command_len--;
            }
            continue;
        }

        if (ch < 32 || ch > 126) {
            continue;
        }

        if (serial_command_len + 1 < sizeof(serial_command_buffer)) {
            serial_command_buffer[serial_command_len++] = ch;
        } else {
            serial_command_len = 0;
            serial_write_text("Command too long.\r\n");
            serial_prompt();
        }
    }
}

static void service_serial_console_banner(void) {
    bool connected = rp2040_usb_portal_serial_connected();

    if (connected && !serial_console_was_connected) {
        serial_command_len = 0;
        serial_write_text("\r\nRP2040 USB Portal serial console\r\n");
        serial_print_help();
        serial_print_state();
        serial_debugf("serial console connected");
        serial_prompt();
    } else if (!connected) {
        serial_command_len = 0;
    }

    serial_console_was_connected = connected;
}

static bool handle_gpio_api_request(const rp2040_usb_portal_http_request_t *request,
                                    rp2040_usb_portal_http_response_t *response) {
    uint pin_number = 0;
    uint value = 0;
    bool has_pin = query_param_uint(request->query, "pin", &pin_number);
    bool has_value = query_param_uint(request->query, "value", &value);
    example_pin_t *pin = has_pin ? find_registered_pin(pin_number) : first_registered_output_pin();

    if (!pin) {
        if (has_pin) {
            serial_debugf("HTTP GPIO rejected: GPIO %u is not registered", pin_number);
        } else {
            serial_debugf("HTTP GPIO rejected: no registered output pin");
        }
        return send_error_response(response, "404 Not Found", "pin is not registered");
    }

    if (has_value) {
        if (pin->mode != EXAMPLE_PIN_OUTPUT) {
            serial_debugf("HTTP GPIO rejected: %s GPIO %u is %s, not out",
                          pin->name,
                          pin->gpio,
                          pin_mode_name(pin->mode));
            return send_error_response(response, "403 Forbidden", "pin is not configured as an output");
        }
        if (value > 1) {
            serial_debugf("HTTP GPIO rejected: %s GPIO %u invalid value %u",
                          pin->name,
                          pin->gpio,
                          value);
            return send_error_response(response, "400 Bad Request", "output value must be 0 or 1");
        }
        write_registered_pin(pin, value != 0);
        serial_debugf("HTTP set %s GPIO %u %s",
                      pin->name,
                      pin->gpio,
                      value ? "HIGH" : "LOW");
    }

    uint16_t read_value = read_registered_pin(pin);
    snprintf(json_response_buffer, sizeof(json_response_buffer),
             "{\"gpio\":%u,\"name\":\"%s\",\"mode\":\"%s\",\"value\":%u}\n",
             pin->gpio, pin->name, pin_mode_name(pin->mode), read_value);
    return set_json_response(response, "200 OK", json_response_buffer);
}

static bool handle_example_http_request(const rp2040_usb_portal_http_request_t *request,
                                        rp2040_usb_portal_http_response_t *response,
                                        void *user_data) {
    (void)user_data;

    if (strcmp(request->method, "GET") != 0) {
        serial_debugf("HTTP rejected method %s for %s", request->method, request->path);
        return send_error_response(response, "405 Method Not Allowed", "method not allowed");
    }

    if (strcmp(request->path, "/") == 0 ||
        strcmp(request->path, "/index.html") == 0) {
        serial_debugf("HTTP served portal page");
        response->status = "200 OK";
        response->content_type = "text/html; charset=utf-8";
        response->body = portal_page_html;
        return true;
    }

    if (strcmp(request->path, "/api/state") == 0 ||
        strcmp(request->path, "/api/pins") == 0) {
        return send_state_response(response);
    }

    if (strcmp(request->path, "/api/gpio") == 0) {
        return handle_gpio_api_request(request, response);
    }

    serial_debugf("HTTP route miss: %s", request->path);
    return false;
}

int main(void) {
    stdio_init_all();
    init_registered_pins();

    rp2040_usb_portal_config_t config;
    rp2040_usb_portal_config_init(&config);
    config.http_handler = handle_example_http_request;
    config.serial_rx_handler = handle_example_serial_rx;

    if (!rp2040_usb_portal_init(&config)) {
        while (true) {
            tight_loop_contents();
        }
    }

    while (true) {
        rp2040_usb_portal_task();
        service_serial_console_banner();
    }
}
