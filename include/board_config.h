#pragma once
#include <stdint.h>
#include "display_setup.h"

// GPIO numbers, NOT the J1..J32 numbers in the LCD expansion schematic.
// Pin mapping supplied by the user for the actual board; see docs/hardware.md.
// LCD GPIOs are in display_setup.h and match imshixin/esp32c3_lvgl_observer.
// That reference has no direction-key mapping. Do not copy its LED output setup:
// GPIO13 is currently assigned to UP and must not be driven as an LED output.
namespace Board {
constexpr uint8_t keyUp = 13;    // User's current physical direction mapping.
constexpr uint8_t keyCenter = 4;
constexpr uint8_t keyDown = 8;   // Strapping GPIO; release during power-up/reset.
constexpr uint8_t keyLeft = 9;   // Shared with BOOT; release during power-up/reset.
constexpr uint8_t keyRight = 5;
constexpr uint8_t backlight = 11;
constexpr uint8_t rotation = 1;  // 160 x 80 landscape; try 3 for a 180-degree flip.
constexpr bool invertColors = LCD_PANEL_LEGACY != 0;
constexpr uint32_t debounceMs = 30;
constexpr uint32_t eventMaxAgeMs = 1500;
constexpr uint32_t heartbeatMs = 2500;
constexpr uint32_t statusPollMs = 1000;
constexpr uint32_t repeatDelayMs = 450;
constexpr uint32_t repeatIntervalMs = 150;
}
