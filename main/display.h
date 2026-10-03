// echo -- display bring-up (CH32V003 IO expander + ST7789 panel).
//
// Minimal first step toward the project's eventual LVGL/touch/audio/sensors
// goal: get the panel powered on and showing a solid color, using the SAME
// real managed components and pin wiring the vendor's own full BSP uses
// (github.com/waveshareteam/ESP32-C5-Touch-LCD-2.8, example/ESP-IDF-V554/
// 08_bookesia's components/esp32_c5_touch_lcd_2_8), not a hand-rolled
// driver or a port from a different board -- so later layers (LVGL, touch)
// slot in on top of this instead of requiring a rewrite.
//
// Hardware facts (verified against docs.waveshare.com/ESP32-C5-Touch-LCD-2.8
// and the vendor's actual board source, NOT guessed):
//   - LCD SPI: SCK=GPIO6, MOSI=GPIO7, DC=GPIO9, CS=GPIO10, 80MHz, mode 0.
//   - LCD_RST and LCD_BL are NOT direct ESP32 GPIOs -- both are driven
//     through the onboard CH32V003 IO expander over I2C (GPIO0=SDA,
//     GPIO1=SCL, shared bus with touch/IMU/RTC/audio). LCD_RST is
//     IO_EXPANDER_PIN_NUM_1 on the expander; backlight is set via the
//     expander's own PWM command (custom_io_expander_set_pwm), not an
//     ESP32 LEDC channel.
//   - Panel needs esp_lcd_panel_invert_color(..., true) -- confirmed from
//     vendor source; without it the fill color (and anything else drawn)
//     is a photographic negative of what was requested.
//   - Panel data byte order must be set to LCD_RGB_DATA_ENDIAN_LITTLE in
//     esp_lcd_panel_dev_config_t. This field writes the ST7789's own RAMCTL
//     register (it is NOT a software byte-swap), and leaving it at the
//     zero-init default (BIG) makes every 16-bit color arrive byte-swapped
//     -- e.g. a requested pure green fill (0x07E0) renders as red.
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bring up I2C (if not already up), the CH32V003 IO expander, the LCD SPI
// bus, and the ST7789 panel itself (reset pulse, init, color-inversion
// quirk, display on). Backlight stays OFF until display_set_backlight() is
// called -- callers decide when it's safe to actually show something.
// Idempotent: a second call returns ESP_OK without re-initializing.
esp_err_t display_init(void);

// Fill the whole screen with one RGB565 color. display_init() must have
// already succeeded. Primarily a bring-up/smoke-test helper for now --
// real content rendering is out of scope until the LVGL layer lands.
esp_err_t display_fill(uint16_t rgb565_color);

// Set backlight brightness via the CH32V003 expander's PWM, 0-100 (%).
// Values outside that range are clamped, not rejected.
esp_err_t display_set_backlight(int percent);

// Accessors for the shared I2C bus + CH32V003 expander handle, so other
// modules on the same bus (touch, sensors, audio codec control) reuse
// the ONE bus/expander display_init() already brought up instead of
// each creating their own -- the I2C bus and the CH32V003 chip can only
// have one real owner each. Both return NULL/invalid until display_init()
// has succeeded at least once.
i2c_master_bus_handle_t display_get_i2c_bus(void);
esp_io_expander_handle_t display_get_io_expander(void);

// Accessors for the ST7789 panel + its SPI panel-IO handle, so the LVGL
// layer (esp_lvgl_port) can attach to the SAME panel display_init()
// already created, rather than re-initializing the SPI bus / panel. Both
// return NULL until display_init() has succeeded.
esp_lcd_panel_handle_t display_get_panel(void);
esp_lcd_panel_io_handle_t display_get_panel_io(void);

#ifdef __cplusplus
}
#endif
