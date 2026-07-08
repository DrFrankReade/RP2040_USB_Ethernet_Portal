# RP2040 USB Ethernet Portal

Proof-of-concept firmware for an RP2040 board that enumerates as a USB network adapter, leases the host an IPv4 address by DHCP, answers DNS queries, and serves a small control page at:

```text
http://192.168.4.1/
```

The demo page exposes a toggle for a control GPIO. On a Raspberry Pi Pico this controls the onboard LED on GPIO 25. On an Adafruit KB2040 this uses GPIO 10.

## Current Status

The default firmware profile, `pico-portal-auto`, has been tested as working on:

- Two Windows hosts.
- A Pixel 6 running Android 16, using a USB OTG adapter.

On Android, the most reliable user path is the captive-network notification, for example `Sign into network 00:24:53:59:93:25`. Opening that notification loads the portal.

This is a proof of concept. It is not a production USB product identity, security model, or provisioning system.

## What It Does

- Enumerates as a USB network device.
- Hosts the device at `192.168.4.1/24`.
- Runs a DHCP server with leases at `192.168.4.2` through `192.168.4.4`.
- Advertises DNS and router options through DHCP for captive-portal behavior.
- Answers all DNS queries with `192.168.4.1`.
- Serves a minimal HTTP control page and JSON API.
- Supports multiple USB network personalities selected at compile time.

## Hardware

Development hardware used:

- Raspberry Pi Pico.
- Raspberry Pi Debug Probe connected to `GND`, `SWDIO`, and `SWCLK`.
- USB cable from the Pico USB port to the host computer or Android OTG adapter.

Target hardware:

- Adafruit KB2040, or another RP2040 design with a USB device connection.

The Debug Probe UART pins are optional for this firmware. `GND`, `SWDIO`, and `SWCLK` are enough for SWD upload and debug.

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

## HTTP API

```text
GET /api/state
```

Returns current GPIO state, uptime, request count, and device MAC address.

```text
GET /api/gpio?value=1
GET /api/gpio?value=0
```

Sets the control GPIO high or low and returns the updated state.

```text
GET /api/bootloader
```

Returns a response, then reboots the RP2040 into BOOTSEL after a short delay.

Unknown paths redirect to `http://192.168.4.1/`.

## Size

Representative PlatformIO build sizes:

| Environment | Flash bytes | RAM bytes |
| --- | ---: | ---: |
| `pico-portal-auto` | 65,724 | 69,176 |
| `pico-portal-android` | 65,180 | 69,168 |
| `pico-portal-windows` | 65,504 | 71,928 |
| `pico-portal-windows-rndis` | 65,264 | 69,168 |
| `kb2040-portal-auto` | 65,724 | 69,176 |
| `kb2040-portal-android` | 65,180 | 69,168 |
| `kb2040-portal-windows` | 65,504 | 71,928 |
| `kb2040-portal-windows-rndis` | 65,264 | 69,168 |

The adaptive firmware is only about 544 bytes larger than the Android-only CDC-ECM profile in these builds. It is about 460 bytes larger than the Windows RNDIS profile. The CDC-NCM profile uses about 2.7 KiB more RAM than the ECM/RNDIS profiles.

## Implementation Notes

The adaptive profile starts as an Android-friendly CDC-ECM USB device using a host-compatible unicast MAC format. If the firmware sees the Windows descriptor probe pattern, it disconnects, changes descriptors, and reconnects as RNDIS under a different PID. This keeps Android from seeing the RNDIS personality first while still giving Windows a driver-free network adapter.

CDC-NCM is kept as a Windows-specific build profile because TinyUSB's ECM/RNDIS and NCM device paths both provide the same `tud_network_*` API surface. Combining NCM with adaptive ECM/RNDIS in one firmware would require deeper TinyUSB driver work rather than only descriptor switching.

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
| `src/main.c` | LwIP setup, DHCP/DNS/HTTP servers, GPIO control, adaptive reconnect trigger |
| `src/tinyusb_descriptors/usb_descriptors.c` | USB descriptors and adaptive Windows probe detection |
| `src/tinyusb_ncm/` | Local CDC-NCM device implementation used by the NCM profiles |
| `include/tusb_config.h` | TinyUSB mode selection |
| `include/lwipopts.h` | LwIP configuration |
| `scripts/extra_picosdk_sources.py` | PlatformIO source injection for TinyUSB networking and LwIP helpers |
