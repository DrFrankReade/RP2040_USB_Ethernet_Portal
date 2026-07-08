#pragma once

#include <stdint.h>

#define BOARD_TUD_RHPORT 0

uint16_t board_usb_get_serial(uint16_t *desc_str, uint16_t max_chars);
