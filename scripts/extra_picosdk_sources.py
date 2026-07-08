from os.path import join

from SCons.Script import DefaultEnvironment


env = DefaultEnvironment()
platform = env.PioPlatform()
framework_dir = platform.get_package_dir("framework-picosdk")

if framework_dir:
    env.Append(
        CPPPATH=[
            "$PROJECT_INCLUDE_DIR",
            join(framework_dir, "lib", "tinyusb", "src"),
            join(framework_dir, "lib", "tinyusb", "src", "class", "net"),
            join(framework_dir, "lib", "tinyusb", "lib", "networking"),
            join(framework_dir, "lib", "lwip", "src", "include"),
        ],
        CPPDEFINES=[
            ("CFG_TUSB_DEBUG", 0),
            ("CFG_TUSB_MCU", "OPT_MCU_RP2040"),
            ("CFG_TUSB_OS", "OPT_OS_PICO"),
            ("PICO_RP2040_USB_DEVICE_UFRAME_FIX", 1),
            ("PICO_RP2040_USB_DEVICE_ENUMERATION_FIX", 1),
        ],
    )

    env.BuildSources(
        join("$BUILD_DIR", "TinyUSB"),
        join(framework_dir, "lib", "tinyusb", "src"),
        "-<*> +<tusb.c> +<common/tusb_fifo.c> +<device/*.c> "
        "+<class/cdc/cdc_device.c> "
        "+<class/net/ecm_rndis_device.c> "
        "+<portable/raspberrypi/rp2040/dcd_rp2040.c> "
        "+<portable/raspberrypi/rp2040/rp2040_usb.c>",
    )
    env.BuildSources(
        join("$BUILD_DIR", "TinyUSBNetworking"),
        join(framework_dir, "lib", "tinyusb", "lib", "networking"),
        "+<dhserver.c> +<dnserver.c> +<rndis_reports.c>",
    )
    env.BuildSources(
        join("$BUILD_DIR", "LwIP"),
        join(framework_dir, "lib", "lwip", "src"),
        "-<*> +<core/*.c> +<core/ipv4/*.c> +<netif/ethernet.c>",
    )
    env.BuildSources(
        join("$BUILD_DIR", "PicoUSBFix"),
        join(framework_dir, "src", "rp2_common", "pico_fix"),
    )
