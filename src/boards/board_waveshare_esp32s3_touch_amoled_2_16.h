// src/boards/board_waveshare_esp32s3_touch_amoled_2_16.h
#pragma once

// Display: 480×480 rounded-square AMOLED (CO5300). CO5300 panels can't
// tolerate per-row blits (QSPI state issue), so the LETTERBOX path is
// reused: PSRAM-backed full-frame buffer + one-shot draw16bitRGBBitmap.
// 184×224 canvas → DEST 368×448 (exact 2× scale, bilinear == nearest)
// centred via (LCD-DEST)/2 → (56, 16) on 480×480.
#define LCD_W_PHYS  480
#define LCD_H_PHYS  480

#define BOARD_HW_W        184
#define BOARD_HW_H        224
// Rounded corners fall in the 56 px L/R black border; canvas content is
// fully inside the visible rectangle. Same SAFE_INSET as 1.8.
#define BOARD_SAFE_INSET  8

// QSPI to CO5300 (LCD_RESET / TP_RESET pins differ from 1.75c)
#define PIN_LCD_SDIO0  4
#define PIN_LCD_SDIO1  5
#define PIN_LCD_SDIO2  6
#define PIN_LCD_SDIO3  7
#define PIN_LCD_SCLK   38
#define PIN_LCD_CS     12
#define PIN_LCD_RESET  39   // direct GPIO, not the GPIO 1 of 1.75c
#define PIN_TP_RESET   40   // direct GPIO, not the GPIO 2 of 1.75c

// I2C bus (shared: AXP2101, ES8311, ES7210, CST92xx, QMI8658, PCF85063)
#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   14

// FT3168/CST9217 family touch interrupt
#define PIN_TP_INT    11

// I2S to ES8311 codec — MCLK on GPIO42 (different from 1.75c's GPIO16)
#define PIN_I2S_MCLK  42
#define PIN_I2S_BCLK  9
#define PIN_I2S_WS    45
#define PIN_I2S_DI    10
#define PIN_I2S_DO    8
#define PIN_PA_CTRL   46

// Three physical keys
#define PIN_KEY1      16   // PWR silkscreen, middle. Active-HIGH via BSS138 inverter (PWRON gate).
#define PIN_KEY2      18   // IO18 silkscreen, left. Active-low.
#define PIN_KEY_BOOT  0    // BOOT silkscreen, right. Active-low. Synthesises BTN_A_LONG_PRESS.

#define BOARD_HAS_PSRAM            1

// Panel geometry / driver config
#define BOARD_CO5300_COL_OFFSET    0   // 480×480 panel uses the full window
#define BOARD_DISPLAY_ROTATION     0
// MADCTL override: Arduino_CO5300's setRotation only does X/Y mirror, not
// row/column swap (MV bit). Setting this to non-zero writes 0x36 directly
// after canvas->begin(). 0x60 = MV+MX (90° CW); 0xA0 = MV+MY (90° CCW); 0 = no override.
// It also seeds Settings.rotation's fresh-install default (see display.h).
#define BOARD_CO5300_MADCTL        0xC0   // MX+MY (180°; buttons on the right)
// Letterbox destination rect: the CO5300 needs a one-shot full-frame blit,
// so hwDisplayPush scales the canvas into a PSRAM frame buffer and centres
// it via (LCD - DEST) / 2.
#define BOARD_DISPLAY_DEST_W       368   // 184 × 2 (exact integer upscale; bilinear == nearest at 2×)
#define BOARD_DISPLAY_DEST_H       448   // 224 × 2

// Credits page
#define BOARD_MODEL_LINE1  "Waveshare ESP32-S3"
#define BOARD_MODEL_LINE2  "Touch AMOLED 2.16"
