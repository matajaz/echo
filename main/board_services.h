// board_services.h — single owner of the SHARED board resources.
//
// Review order 9 (03-ARCHITECTURE "board_services.c"): the I2C bus and the
// CH32V003 IO-expander are a BOARD resource shared by display, touch, sensors
// and audio — not a display-private one. Previously display_init() created
// them, so other subsystems depended on display's hidden partial state and a
// display failure/retry could invalidate bus users. board_services owns their
// creation (once, idempotent) + the named compound operations on the expander
// (LCD reset, backlight PWM, speaker-PA enable), giving ONE safe recovery
// point. Existing driver APIs are preserved: display_get_i2c_bus() /
// display_get_io_expander() now delegate here.
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the shared I2C master bus + CH32V003 expander (idempotent: a second
// call is a no-op returning ESP_OK). Safe to call before display_init(); each
// consumer (display/touch/sensors/audio) may also call it defensively. This is
// deliberately independent of the display panel, so a panel failure never tears
// the bus down under the other users.
esp_err_t board_services_init(void);

// Shared handles (NULL until board_services_init() succeeded).
i2c_master_bus_handle_t board_i2c_bus(void);
esp_io_expander_handle_t board_io_expander(void);

// Named compound expander operations (serialized on the shared bus by the IDF
// I2C driver; these are the ONLY sanctioned read-modify-write ops on the
// expander so no bare transaction interleaves one).
esp_err_t board_lcd_reset(void);          // pulse LCD_RST low->high
esp_err_t board_backlight_set(int percent); // 0..100 via expander PWM
esp_err_t board_pa_enable(bool on);        // speaker power amplifier

#ifdef __cplusplus
}
#endif
