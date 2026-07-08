# RP2040 USB Ethernet Portal

Proof-of-concept firmware for RP2040 boards that enumerate as a USB network adapter and host a local control page at:

```text
http://192.168.4.1/
```

In other words, this gives a microcontroller a wired local network link over USB. The host gets an address by DHCP, the RP2040 stays at `192.168.4.1`, and application code can serve HTTP or run its own TCP/UDP protocols without Wi-Fi, passwords, AP mode, or radio setup.

The example page exposes only explicitly registered pins. On a Raspberry Pi Pico, the board definition supplies `PICO_DEFAULT_LED_PIN`, so the example controls the onboard LED. Other boards expose no demo output unless the project opts into a pin.

Tested so far: Windows, a Pixel 6 running Android 16 with USB OTG, Raspberry Pi Pico, and Adafruit KB2040 builds. This is a proof of concept, not a production USB identity, security model, or provisioning system.

## What It Does

- Enumerates as a USB network device.
- Hosts the device at `192.168.4.1/24`.
- Runs a DHCP server with leases at `192.168.4.2` through `192.168.4.4` by default.
- Advertises DNS and router options through DHCP by default for captive-portal behavior.
- Can answer DNS queries with `192.168.4.1`.
- Provides a reusable C API around the USB network portal.
- Can serve a minimal HTTP control page and JSON API through an application callback.
- Exposes a CDC-ACM virtual serial port by default for diagnostics and upload convenience.
- Supports multiple USB network personalities selected at compile time.

## Library Shape

The reusable layer lives in:

- `include/rp2040_usb_ethernet_portal.h`
- `include/rp2040_usb_ethernet_portal_config.h`
- `src/rp2040_usb_ethernet_portal.c`

The application calls `rp2040_usb_portal_config_init()`, optionally changes the returned config, assigns HTTP or serial handlers, calls `rp2040_usb_portal_init()`, then calls `rp2040_usb_portal_task()` from the main loop.

The portal owns the USB network device, CDC-ACM serial function, LwIP netif, optional DHCP server, optional DNS catch-all, optional basic HTTP server, `/api/bootloader`, 1200-baud BOOTSEL touch, and adaptive Windows/Android USB persona switching. Application code owns routes, page content, serial commands, and any hardware-specific control policy.

The active example is `src/example_portal_app.c`.

## Build Profiles

The recommended default is `both`.

The board is selected once in `platformio.ini`:

```ini
[rp2040_board]
id = pico
```

Set `id = adafruit_kb2040` for the KB2040, or use another PlatformIO RP2040 board ID for a different board. USB behavior is selected by the environment name.

| Environment | USB behavior | Intended host |
| --- | --- | --- |
| `both` | Composite CDC-ACM serial plus adaptive network: Android-friendly CDC-ECM first, Windows RNDIS after host probe detection | Windows, Linux, Android |
| `both-network-only` | Adaptive network only, no CDC-ACM serial | Phone compatibility testing |
| `android` | Composite CDC-ACM serial plus CDC-ECM network | Android/Linux |
| `windows-ncm` | Composite CDC-ACM serial plus CDC-NCM network | Windows |
| `windows-rndis` | Composite CDC-ACM serial plus RNDIS network | Windows |
| `bootsel` | Same firmware behavior as `both`, uploaded with `picotool` | Manual USB mass-storage style flashing |

`network-only` means the firmware does not expose the CDC-ACM serial function. It still uses the same network behavior.

The Pico and KB2040 build the same portal firmware behavior. The selected PlatformIO board ID controls board metadata such as flash size, SDK board headers, default LED definitions, and upload details.

### Why The Auto Profile Uses RNDIS On Windows

Windows has a built-in RNDIS driver, but Device Manager names it with Microsoft's driver description, typically something like:

```text
Remote NDIS based Internet Sharing Device
```

That label comes from the Windows in-box driver INF, not from this firmware's USB product strings. A driver-free firmware cannot reliably replace that adapter description.

CDC-NCM gives a cleaner Windows adapter identity and is available as `windows-ncm`, but the tested Pixel 6 did not enumerate the NCM firmware as a usable network device. The adaptive profile exists to keep one firmware image usable on both tested host classes.

## Build And Upload

Install PlatformIO Core or use the PlatformIO extension for VS Code.

This project sets PlatformIO's build directory under the user's `.platformio` directory. That avoids a Pico SDK build failure seen on Windows when the project path contains spaces.

Build the default firmware for the selected board:

```powershell
pio run -e both
```

Build the network-only firmware when testing phone hosts that reject composite USB devices:

```powershell
pio run -e both-network-only
```

Upload using the configured PlatformIO upload protocol:

```powershell
pio run -e both -t upload
```

Build for a KB2040 by changing the board selector:

```ini
[rp2040_board]
id = adafruit_kb2040
```

Then run the same environment:

```powershell
pio run -e both
pio run -e both -t upload
```

If `pio` is not on `PATH`, run the same environments from the PlatformIO extension, or invoke the PlatformIO executable from the local PlatformIO virtual environment.

## Using The Portal

1. Flash the firmware.
2. Plug the RP2040 USB device port into the host.
3. Let the host obtain an address by DHCP.
4. Open `http://192.168.4.1/`.

On Windows with the adaptive profile, the final adapter is expected to be RNDIS and the same USB device should also expose a COM port. On Linux, the default composite build should appear as a USB Ethernet interface plus a CDC ACM serial device. On Android, use the captive-network sign-in notification when it appears. If a phone does not accept the composite device, try the `both-network-only` environment or build with `-DUSB_PORTAL_ENABLE_CDC_SERIAL=0`.

## Portal Launch Behavior

The default behavior is intentionally captive-style. The device advertises itself as router and DNS, runs DNS catch-all, and redirects unknown HTTP paths to the portal. This is very convenient when the host has no other internet connection: Windows and Android often open the portal automatically.

On Windows with another active internet connection, that same OS detection path may open the default browser to a Microsoft/MSN connectivity-test page instead of the RP2040. That is host routing and captive-portal detection behavior, not a device failure.

In other words:

- Offline host: the default mode is usually the nicest path because the portal can open itself.
- Online Windows host: the default mode may occasionally open a browser page you did not ask for.
- Link-only mode: no captive launch attempt; open `http://192.168.4.1/` yourself or from host software.

The library helper for link-only mode is:

```c
rp2040_usb_portal_config_use_link_only(&config);
```

The example program keeps this as a source-level application choice, not a PlatformIO build profile:

```c
static const bool example_link_only = false;
```

Set it to `true` in [src/example_portal_app.c](src/example_portal_app.c) to keep DHCP address assignment but stop advertising the RP2040 as a router or DNS server.

## Network Configuration

Network defaults are centralized in `include/rp2040_usb_ethernet_portal_config.h`.

Default values:

- Portal IP: `192.168.4.1`
- Netmask: `255.255.255.0`
- DHCP server: enabled
- DHCP leases: `192.168.4.2` through `192.168.4.4`
- DHCP domain: `rp2040.local`
- DNS: catch-all to the portal IP
- DHCP router option: portal IP

Projects can override the `RP2040_USB_PORTAL_*` macros in build flags or modify the `rp2040_usb_portal_config_t` before calling `rp2040_usb_portal_init()`. Lower-level LwIP pool and buffer sizes also have `RP2040_USB_PORTAL_LWIP_*` defaults in the same config header.

Example runtime override:

```c
rp2040_usb_portal_config_t config;
rp2040_usb_portal_config_init(&config);

config.address = RP2040_USB_PORTAL_IPV4(10, 55, 0, 1);
config.dhcp_lease_start = RP2040_USB_PORTAL_IPV4(10, 55, 0, 2);
config.dhcp_router = config.address;
config.dns_server = config.address;
```

## Integration Modes

The library does not require every project to behave like a captive portal.

For a full captive portal, keep the defaults, assign `config.http_handler`, and call `rp2040_usb_portal_task()` from the main loop. This gives the host an address by DHCP, catches DNS, and serves the page at `http://192.168.4.1/`.

For a local web UI without captive-portal behavior, use link-only mode:

```c
rp2040_usb_portal_config_t config;
rp2040_usb_portal_config_init(&config);

rp2040_usb_portal_config_use_link_only(&config);

rp2040_usb_portal_init(&config);
```

The HTTP server still runs. The host still gets a DHCP lease. The RP2040 does not advertise itself as the host's router or DNS server.

For a plain USB network interface with DHCP, disable the captive pieces and keep DHCP enabled:

```c
rp2040_usb_portal_config_t config;
rp2040_usb_portal_config_init(&config);

rp2040_usb_portal_config_use_link_only(&config);
config.enable_http_server = false;

rp2040_usb_portal_init(&config);
```

The RP2040 still comes up at the configured device IP, the host still gets a lease, and application code can use LwIP directly through `rp2040_usb_portal_netif()` plus its own TCP or UDP code.

For a static-only USB network interface, disable DHCP as well:

```c
rp2040_usb_portal_config_t config;
rp2040_usb_portal_config_init(&config);

config.enable_dhcp_server = false;
config.enable_dns_catchall = false;
config.enable_http_server = false;

rp2040_usb_portal_init(&config);
```

Do not disable DHCP for the plug-and-go portal flow. Without DHCP, the host needs some other way to know the subnet and its own address. If a product already controls host network configuration, the option is there.

## Serial Console

CDC-ACM serial is enabled by default. The library services the serial endpoint from `rp2040_usb_portal_task()` and provides:

- `rp2040_usb_portal_serial_connected()`
- `rp2040_usb_portal_serial_read()`
- `rp2040_usb_portal_serial_write()`
- `rp2040_usb_portal_serial_rx_handler_t`

The example application registers a serial RX handler with these commands:

```text
help
state
debug on
debug off
led on
led off
bootloader
```

The example also prints serial debug events for useful actions such as portal page opens, GPIO writes, rejected GPIO requests, and unknown routes. Opening and closing the serial port at 1200 baud schedules a reboot into BOOTSEL. This mirrors the common Arduino-style upload touch behavior while keeping the serial port independent from Pico SDK `stdio_usb`.

## Migrating From A Wireless AP Portal

This project is meant to replace the network side of a classic embedded WiFi AP portal without forcing the application to become USB-specific.

1. Remove the WiFi AP bring-up code: SSID, password, channel, country, AP mode, and wireless station tracking.
2. Add this library's headers, source file, TinyUSB descriptor support, LwIP options, and PlatformIO `extra_scripts` support to the project.
3. Replace the AP IP/subnet setup with `rp2040_usb_portal_config_t`. Keep `192.168.4.1` if it already fits, or set a different address once in the config.
4. Move existing HTTP routes into an `rp2040_usb_portal_http_handler_t` callback. Return `false` for paths the library should handle or redirect.
5. Keep DHCP enabled unless the host-side software is prepared to configure its own address.
6. If the project only needs a link for UDP, TCP, telemetry, RPC, or a custom protocol, disable DNS and HTTP and use the LwIP netif directly.
7. Keep hardware access explicit. The example registers safe pins by mode and rejects arbitrary GPIO requests; product code should do the same for relays, motors, analog inputs, and configuration pins.
8. Call `rp2040_usb_portal_task()` regularly from the main loop.

## HTTP API

```text
GET /api/state
```

Returns portal state, uptime, request count, MAC address, link-only status, and the example application's registered pin list.

```text
GET /api/gpio?pin=25&value=1
GET /api/gpio?pin=25&value=0
```

Sets a registered output pin high or low and returns the updated state. The example rejects unregistered pins and rejects writes to pins not registered as outputs.

```text
GET /api/pins
```

Returns the same registered pin list included in `/api/state`.

```text
GET /api/bootloader
```

Returns a response, then reboots the RP2040 into BOOTSEL after a short delay.

Unknown paths redirect to the configured portal IP.

## Implementation Notes

The adaptive profile starts as an Android-friendly CDC-ECM USB device using a host-compatible unicast MAC format. If the firmware sees the Windows descriptor probe pattern, it disconnects, changes descriptors, and reconnects as RNDIS under a different PID. This keeps Android from seeing the RNDIS personality first while still giving Windows a driver-free network adapter.

CDC-NCM is kept as a Windows-specific build profile because TinyUSB's ECM/RNDIS and NCM device paths both provide the same `tud_network_*` API surface. Combining NCM with adaptive ECM/RNDIS in one firmware would require deeper TinyUSB driver work rather than only descriptor switching.

The example deliberately separates portal plumbing from hardware policy. GPIOs are readable or writable through the example API only if they are present in the example's explicit pin table with a declared mode (`in`, `out`, or `analog`). This is intentional; arbitrary web-controlled GPIO writes are not safe on a general embedded board.

## Production Gaps

- The USB VID/PID values in this repository are proof-of-concept values.
- The generated MAC address is based on the RP2040 unique ID, but the first byte is forced to `00` for Android compatibility in the tested configuration. A production product needs a deliberate MAC allocation strategy.
- The web server is plain HTTP with no authentication.
- The fixed subnet `192.168.4.0/24` can conflict with another active network.
- Host behavior is OS- and policy-dependent. The tested set is not a compatibility guarantee.

## Repository Layout

| Path | Purpose |
| --- | --- |
| `platformio.ini` | PlatformIO board selector and USB behavior environments |
| `include/rp2040_usb_ethernet_portal.h` | Reusable portal API |
| `include/rp2040_usb_ethernet_portal_config.h` | Network and LwIP defaults |
| `src/rp2040_usb_ethernet_portal.c` | TinyUSB, LwIP, DHCP, DNS, HTTP, and BOOTSEL portal implementation |
| `src/example_portal_app.c` | Example application with explicit safe pin registry |
| `src/tinyusb_descriptors/usb_descriptors.c` | USB descriptors and adaptive Windows probe detection |
| `src/tinyusb_ncm/` | Local CDC-NCM device implementation used by the NCM profiles |
| `include/tusb_config.h` | TinyUSB mode selection |
| `include/lwipopts.h` | LwIP configuration |
| `scripts/extra_picosdk_sources.py` | PlatformIO source injection for TinyUSB networking and LwIP helpers |
