#pragma once

// TFT_eSPI 2.5.43 uses SPI2_HOST (1) to index SPI register macros.
// Arduino ESP32 2.0.17 / ESP-IDF 4.4 expects the physical peripheral number (2).
// Without this translation SPI_USER_REG resolves to address 0x10 and init crashes.
// Keep this compatibility change in the TFT setup, so framework/library files
// remain unmodified and a fresh PlatformIO dependency install also works.
#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include <soc/soc.h>
#include <hal/spi_types.h>

#if REG_SPI_BASE(1) == 0 && REG_SPI_BASE(2) == DR_REG_SPI2_BASE
#undef REG_SPI_BASE
// Accept both TFT_eSPI's logical host ID and the SDK's physical SPI2 number.
#define REG_SPI_BASE(i) (((i) == 1 || (i) == 2) ? DR_REG_SPI2_BASE : 0)
#endif

static_assert(REG_SPI_BASE(SPI2_HOST) == DR_REG_SPI2_BASE,
              "Invalid ESP32-C3 SPI2 register base");
#endif
