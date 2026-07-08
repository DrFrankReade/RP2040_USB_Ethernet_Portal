# Third-Party Notices

This repository builds against the Raspberry Pi Pico SDK, TinyUSB, and LwIP through PlatformIO.

The files in `src/tinyusb_ncm/` are derived from TinyUSB CDC-NCM device code and retain their upstream SPDX license headers.

The PlatformIO extra script compiles networking support sources from the installed Pico SDK package at build time, including TinyUSB network device support, DHCP/DNS helper code, and LwIP sources.
