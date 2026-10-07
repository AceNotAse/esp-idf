/*
 * board.h - everything that touches the CrowPanel hardware directly:
 * power pins, the round LCD, the touch chip, the backlight and the LED ring.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 240
#define BOARD_LCD_V_RES 240

/* Bring up all hardware and start the LVGL task. Call once from app_main(). */
esp_err_t board_init(void);

/* LVGL is not thread-safe: wrap any LVGL call made outside the LVGL task in lock/unlock. */
void board_lvgl_lock(void);
void board_lvgl_unlock(void);

/* Backlight brightness, 0 (off) .. 100 (full). */
void board_backlight_set(uint8_t percent);

/* Paint every LED in the ring the same colour (0..255 per channel). */
void board_leds_set(uint8_t r, uint8_t g, uint8_t b);
