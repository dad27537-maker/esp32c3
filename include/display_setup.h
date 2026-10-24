#pragma once

// Air101 0.96-inch ST7735S, portrait memory is 80 x 160.
// Reference: imshixin/esp32c3_lvgl_observer, pio.zip -> TFT_eSPI/User_Setup.h.
// 0: newer panel (landscape offset 0,24); 1: older panel (offset 1,26).
// Official reference: docs/hardware.md. Change this ONE switch if shifted.
#define LCD_PANEL_LEGACY 0
#define ST7735_DRIVER
#if LCD_PANEL_LEGACY
#define ST7735_GREENTAB160x80
#else
#define ST7735_REDTAB160x80
#endif
#define TFT_WIDTH 80
#define TFT_HEIGHT 160
// Matches the reference project's TFT_RGB_ORDER TFT_BGR (TFT_BGR == 0).
#define TFT_RGB_ORDER 0
// User wiring: SCK=IO02, SDA=IO3, RES=IO10, DC=IO6, CS=IO7.
// Backlight BL=IO11 is controlled by Board::backlight in board_config.h.
#define TFT_MISO -1
#define TFT_MOSI 3
#define TFT_SCLK 2
#define TFT_CS 7
#define TFT_DC 6
#define TFT_RST 10
#define SPI_FREQUENCY 27000000
#define SPI_READ_FREQUENCY 20000000
#define LOAD_GLCD
#define LOAD_FONT2
