// echo -- touch bring-up (CST3530 capacitive touch controller).
//
// Hardware facts (verified against docs.waveshare.com/ESP32-C5-Touch-LCD-2.8
// and the vendor's actual board source -- see display.h for the same
// sourcing discipline):
//   - Touch I2C: SDA=GPIO0, SCL=GPIO1 -- the SAME I2C bus display.c already
//     brings up for the CH32V003 expander (shared bus with IMU/RTC/audio).
//   - Touch INT: GPIO5 (a direct ESP32 GPIO, unlike LCD/touch RESET).
//   - Touch RESET is NOT a direct ESP32 GPIO -- like the LCD, it's driven
//     through the CH32V003 expander (IO_EXPANDER_PIN_NUM_0 == TP_RST).
#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the CST3530 touch controller on the shared I2C bus. Requires
// display_init() to have already succeeded (reuses its I2C bus + CH32V003
// expander handle rather than creating a second one). Idempotent.
esp_err_t touch_init(void);

// The underlying esp_lcd_touch handle, so the LVGL layer can register it
// as an input device via lvgl_port_add_touch(). NULL until touch_init()
// has succeeded.
esp_lcd_touch_handle_t touch_get_handle(void);

// Poll the controller for a current touch point. Returns true if the
// panel is currently being touched, with *x/*y filled in panel pixel
// coordinates (0..239, 0..319). Returns false (x/y untouched) if not
// touched or if touch_init() hasn't succeeded yet.
bool touch_get_point(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
