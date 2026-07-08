# RP2040 USB Ethernet Portal

Reusable proof-of-concept firmware for an RP2040 board that enumerates as a USB network adapter, leases the host an IPv4 address by DHCP, answers DNS queries, and serves a captive-style local portal at:

```text
http://192.168.4.1/
```

The active example page exposes a toggle only for pins explicitly registered by the example application. On a Raspberry Pi Pico, the selected Pico SDK board definition provides `PICO_DEFAULT_LED_PIN`, so the example controls the onboard LED on GPIO 25. Boards that do not define a simple default LED expose no demo output unless the project opts into one.

## Current Status

The default firmware profile, `pico-portal-auto`, has been tested as working on:

- Two Windows hosts.
- A Pixel 6 running Android 16, using a USB OTG adapter.
- Raspberry Pi Pico upload through a Raspberry Pi Debug Probe.

On Android, the most reliable user path is the captive-network notification, for example `Sign into network 00:24:53:59:93:25`. Opening that notification loads the portal.

This is a proof of concept. It is not a production USB product identity, security model, or provisioning system.

## What It Does

- Enumerates as a USB network device.
- Hosts the device at `192.168.4.1/24`.
- Runs a DHCP server with leases at `192.168.4.2` through `192.168.4.4`.
- Advertises DNS and router options through DHCP for captive-portal behavior.
- Answers all DNS queries with `192.168.4.1`.
- Provides a small reusable C API around the USB network portal.
- Serves a minimal HTTP control page and JSON API through an application callback.
- Supports multiple USB network personalities selected at compile time.

## Hardware

Development hardware used:

- Raspberry Pi Pico.
- Raspberry Pi Debug Probe connected to `GND`, `SWDIO`, and `SWCLK`.
- USB cable from the Pico USB port to the host computer or Android OTG adapter.

Target hardware:

- Adafruit KB2040, or another RP2040 design with a USB device connection.

The Debug Probe UART pins are optional for this firmware. `GND`, `SWDIO`, and `SWCLK` are enough for SWD upload and debug.

## Library Shape

The reusable layer lives in:

- `include/rp2040_usb_ethernet_portal.h`
- `include/rp2040_usb_ethernet_portal_config.h`
- `src/rp2040_usb_ethernet_portal.c`

The application calls `rp2040_usb_portal_config_init()`, optionally changes the returned config, assigns an HTTP handler, calls `rp2040_usb_portal_init()`, then calls `rp2040_usb_portal_task()` from the main loop.

The portal owns the USB network device, LwIP netif, DHCP server, DNS catch-all, basic HTTP server, `/api/bootloader`, and adaptive Windows/Android USB persona switching. Application code owns routes, page content, and any hardware-specific control policy.

The active example is `src/example_portal_app.c`.

## Build Profiles

The recommended default is `pico-portal-auto`.

| Environment | Board | USB behavior | Intended host |
| --- | --- | --- | --- |
| `pico-portal-auto` | Raspberry Pi Pico | Adaptive: Android-friendly CDC-ECM first, Windows RNDIS after host probe detection | Windows and Android |
| `pico-portal-android` | Raspberry Pi Pico | CDC-ECM only | Android |
| `pico-portal-windows` | Raspberry Pi Pico | CDC-NCM only | Windows |
| `pico-portal-windows-rndis` | Raspberry Pi Pico | RNDIS | Windows |
| `kb2040-portal-auto` | Adafruit KB2040 | Adaptive: Android-friendly CDC-ECM first, Windows RNDIS after host probe detection | Windows and Android |
| `kb2040-portal-android` | Adafruit KB2040 | CDC-ECM only | Android |
| `kb2040-portal-windows` | Adafruit KB2040 | CDC-NCM only | Windows |
| `kb2040-portal-windows-rndis` | Adafruit KB2040 | RNDIS | Windows |
| `kb2040-bootsel` | Adafruit KB2040 | BOOTSEL upload profile using `picotool` | Manual USB mass-storage style flashing |

The board selection is intentionally left to PlatformIO's `board = ...` setting. The example does not repeat per-board GPIO declarations. For the demo LED, it uses `PICO_DEFAULT_LED_PIN` when the selected Pico SDK board header provides one. For a board without that macro, either register project-specific pins in the application or add a deliberate build flag such as `-DPORTAL_EXAMPLE_LED_GPIO=10`.

Flash size also comes from the selected PlatformIO board metadata and Pico SDK board header. If a custom board definition is wrong, override both the PlatformIO size check and the SDK macro in that board environment, for example:

```ini
board_upload.maximum_size = 8384512
build_flags =
    ${env.build_flags}
    -DPICO_FLASH_SIZE_BYTES=8388608
```

### Why The Auto Profile Uses RNDIS On Windows

Windows has a built-in RNDIS driver, but Device Manager names it with Microsoft's driver description, typically something like:

```text
Remote NDIS based Internet Sharing Device
```

That label comes from the Windows in-box driver INF, not from this firmware's USB product strings. A driver-free firmware cannot reliably replace that adapter description.

CDC-NCM gives a cleaner Windows adapter identity and is available as `pico-portal-windows` / `kb2040-portal-windows`, but the tested Pixel 6 did not enumerate the NCM firmware as a usable network device. The adaptive profile exists to keep one firmware image usable on both tested host classes.

## Build And Upload

Install PlatformIO Core or use the PlatformIO extension for VS Code.

This project sets PlatformIO's build directory under the user's `.platformio` directory. That avoids a Pico SDK build failure seen on Windows when the project path contains spaces.

Build the default Pico firmware:

```powershell
pio run -e pico-portal-auto
```

Upload to a Raspberry Pi Pico through a Raspberry Pi Debug Probe:

```powershell
pio run -e pico-portal-auto -t upload
```

Build and upload the KB2040 adaptive target:

```powershell
pio run -e kb2040-portal-auto
pio run -e kb2040-portal-auto -t upload
```

If `pio` is not on `PATH`, run the same environments from the PlatformIO extension, or invoke the PlatformIO executable from the local PlatformIO virtual environment.

## Using The Portal

1. Flash the firmware.
2. Plug the RP2040 USB device port into the host.
3. Let the host obtain an address by DHCP.
4. Open `http://192.168.4.1/`.

On Windows with the adaptive profile, the final adapter is expected to be RNDIS. On Android, use the captive-network sign-in notification when it appears.

## Network Configuration

Network defaults are centralized in `include/rp2040_usb_ethernet_portal_config.h`.

Default values:

- Portal IP: `192.168.4.1`
- Netmask: `255.255.255.0`
- DHCP leases: `192.168.4.2` through `192.168.4.4`
- DHCP domain: `rp2040.local`
- DNS: catch-all to the portal IP
- DHCP router option: portal IP

Projects can override the `RP2040_USB_PORTAL_*` macros in build flags or modify the `rp2040_usb_portal_config_t` before calling `rp2040_usb_portal_init()`. Lower-level LwIP pool and buffer sizes also have `RP2040_USB_PORTAL_LWIP_*` defaults in the same config header.

## HTTP API

```text
GET /api/state
```

Returns portal state, uptime, request count, MAC address, and the example application's registered pin list.

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

## Size

Representative PlatformIO build sizes:

| Environment | Flash bytes | RAM bytes |
| --- | ---: | ---: |
| `pico-portal-auto` | 68,424 | 70,308 |
| `pico-portal-android` | 67,880 | 70,300 |
| `pico-portal-windows` | 68,200 | 73,060 |
| `pico-portal-windows-rndis` | 67,972 | 70,300 |
| `kb2040-portal-auto` | 67,572 | 70,296 |
| `kb2040-portal-android` | 67,028 | 70,288 |
| `kb2040-portal-windows` | 67,348 | 73,048 |
| `kb2040-portal-windows-rndis` | 67,120 | 70,288 |

The adaptive firmware is about 544 bytes larger than the Android-only CDC-ECM profile in these builds. It is about 452 bytes larger than the Windows RNDIS profile on Pico. The CDC-NCM profile uses about 2.7 KiB more RAM than the ECM/RNDIS profiles.

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
| `platformio.ini` | PlatformIO environments for Pico and KB2040 targets |
| `include/rp2040_usb_ethernet_portal.h` | Reusable portal API |
| `include/rp2040_usb_ethernet_portal_config.h` | Network and LwIP defaults |
| `src/rp2040_usb_ethernet_portal.c` | TinyUSB, LwIP, DHCP, DNS, HTTP, and BOOTSEL portal implementation |
| `src/example_portal_app.c` | Example application with explicit safe pin registry |
| `src/tinyusb_descriptors/usb_descriptors.c` | USB descriptors and adaptive Windows probe detection |
| `src/tinyusb_ncm/` | Local CDC-NCM device implementation used by the NCM profiles |
| `include/tusb_config.h` | TinyUSB mode selection |
| `include/lwipopts.h` | LwIP configuration |
| `scripts/extra_picosdk_sources.py` | PlatformIO source injection for TinyUSB networking and LwIP helpers |
