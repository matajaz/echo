// echo -- display bring-up implementation. See display.h for the hardware
// facts and why this follows the vendor's own BSP wiring rather than a
// hand-rolled driver.
#include "display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "custom_io_expander_ch32v003.h"
#include "esp_io_expander.h"

static const char *TAG = "display";

// --- Pins (confirmed via docs.waveshare.com/ESP32-C5-Touch-LCD-2.8 and the
// vendor board source -- see display.h) --------------------------------
#define PIN_I2C_SDA GPIO_NUM_0
#define PIN_I2C_SCL GPIO_NUM_1
#define PIN_LCD_SCK GPIO_NUM_6
#define PIN_LCD_MOSI GPIO_NUM_7
#define PIN_LCD_DC GPIO_NUM_9
#define PIN_LCD_CS GPIO_NUM_10

#define I2C_PORT I2C_NUM_0
#define LCD_SPI_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_SPI_CLOCK_HZ (80 * 1000 * 1000)

// IO_EXPANDER_PIN_NUM_1 on the CH32V003 == LCD_RST (confirmed via the
// vendor board header's `#define IO_LCD_RST (IO_EXPANDER_PIN_NUM_1)`).
// Using the real enum from esp_io_expander.h directly -- it's already a
// transitive dependency of custom_io_expander_ch32v003 (registry entry
// lists `esp_io_expander ^1.0.1` as a public dependency) -- rather than a
// hand-copied bit value, so this can't silently drift from the real
// component if a future version renumbers anything.
#define IO_LCD_RST IO_EXPANDER_PIN_NUM_1

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static esp_io_expander_handle_t s_io_expander = NULL;
static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_display_ready = false;

static esp_err_t io_expander_init(void) {
    if (s_io_expander != NULL) {
        return ESP_OK; // already brought up
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                         "I2C bus init failed");

    ESP_RETURN_ON_ERROR(
        custom_io_expander_new_i2c_ch32v003(
            s_i2c_bus, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &s_io_expander),
        TAG, "CH32V003 IO expander init failed");

    // Default LCD_RST high (not held in reset) until display_init() does
    // its own explicit reset pulse -- matches the vendor's own default
    // level set right after expander creation.
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(s_io_expander, IO_LCD_RST, IO_EXPANDER_OUTPUT),
        TAG, "set IO_LCD_RST direction failed");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_level(s_io_expander, IO_LCD_RST, 1), TAG,
        "set IO_LCD_RST level failed");

    ESP_LOGI(TAG, "CH32V003 IO expander init success");
    return ESP_OK;
}

static esp_err_t lcd_reset_pulse(void) {
    // 200ms low / 200ms high, matching the vendor source's own timing --
    // ST7789 datasheets generally want on the order of microseconds to a
    // few ms, but there's no cost to matching the proven-working timing
    // exactly rather than tightening it without a reason to.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_io_expander, IO_LCD_RST, 0),
                         TAG, "LCD reset (low) failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_io_expander, IO_LCD_RST, 1),
                         TAG, "LCD reset (high) failed");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_err_t display_init(void) {
    if (s_display_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "IO expander bring-up failed");
    ESP_RETURN_ON_ERROR(lcd_reset_pulse(), TAG, "LCD reset pulse failed");

    const spi_bus_config_t spi_cfg = {
        .sclk_io_num = PIN_LCD_SCK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * 80 * 2, // a handful of rows per DMA chunk
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &spi_cfg, SPI_DMA_CH_AUTO),
                         TAG, "LCD SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg,
                                 &s_panel_io),
        TAG, "LCD panel IO init failed");

    // reset_gpio_num = -1 (GPIO_NUM_NC): the panel driver's own reset is
    // disabled because reset already happened above through the CH32V003
    // expander, not through a direct ESP32 GPIO -- matches the vendor
    // board file's own BSP_LCD_RST = GPIO_NUM_NC.
    //
    // data_endian = LITTLE (bug fix, 2026-10-02, confirmed from the real
    // ST7789 driver source: esp_lcd_panel_st7789.c's create function
    // writes panel_dev_config->data_endian straight into the ST7789's own
    // RAMCTL register -- it tells the PANEL CHIP which byte order to
    // expect, it is not a software byte-swap applied to the buffer.
    // Leaving this at its zero-initialized default (LCD_RGB_DATA_ENDIAN_
    // BIG) told the panel to expect MSB-first bytes, but `row_buf[x] =
    // rgb565_color` (a plain uint16_t store) produces LSB-first (little-
    // endian) byte pairs in memory on this RISC-V chip -- every 16-bit
    // color therefore arrived at the panel with its high/low byte
    // swapped. Confirmed live: a requested pure green fill (0x07E0)
    // rendered as red, which is exactly what byte-swapping 0x07E0 ->
    // 0xE007 produces (R=11100, G=000000, B=00111 -- mostly red).
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    esp_err_t ret = esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: 0x%x", ret);
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "ST7789 panel init failed");
    // Confirmed from vendor source: this exact panel needs color inversion,
    // otherwise every drawn color (including this bring-up's fill) comes
    // out as its photographic negative.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG,
                         "ST7789 color-invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG,
                         "ST7789 display-on failed");

    s_display_ready = true;
    ESP_LOGI(TAG, "display init success (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_err_t display_fill(uint16_t rgb565_color) {
    if (!s_display_ready) {
        ESP_LOGE(TAG, "display_fill called before display_init succeeded");
        return ESP_ERR_INVALID_STATE;
    }

    // One row at a time rather than the whole 240x320 frame in one DMA
    // buffer -- 240*320*2 = 153600 bytes is a meaningful chunk of this
    // chip's internal RAM to hold just for a bring-up smoke test; a
    // single-row buffer (480 bytes) reused per row keeps this cheap
    // regardless of panel size.
    const size_t row_bytes = (size_t)LCD_H_RES * 2;
    uint16_t *row_buf = heap_caps_malloc(row_bytes, MALLOC_CAP_DMA);
    if (row_buf == NULL) {
        ESP_LOGE(TAG, "display_fill: row buffer alloc failed (%u bytes)",
                 (unsigned)row_bytes);
        return ESP_ERR_NO_MEM;
    }
    for (int x = 0; x < LCD_H_RES; x++) {
        row_buf[x] = rgb565_color;
    }

    esp_err_t ret = ESP_OK;
    for (int y = 0; y < LCD_V_RES && ret == ESP_OK; y++) {
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + 1, row_buf);
    }

    free(row_buf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display_fill: draw_bitmap failed: 0x%x", ret);
    }
    return ret;
}

esp_err_t display_set_backlight(int percent) {
    if (s_io_expander == NULL) {
        ESP_LOGE(TAG, "display_set_backlight called before display_init");
        return ESP_ERR_INVALID_STATE;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    uint8_t pwm_value = (uint8_t)((percent * 255) / 100);
    esp_err_t ret = custom_io_expander_set_pwm(s_io_expander, pwm_value);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "backlight set to %d%%", percent);
    } else {
        ESP_LOGE(TAG, "backlight set failed: 0x%x", ret);
    }
    return ret;
}

i2c_master_bus_handle_t display_get_i2c_bus(void) {
    return s_i2c_bus;
}

esp_io_expander_handle_t display_get_io_expander(void) {
    return s_io_expander;
}

esp_lcd_panel_handle_t display_get_panel(void) {
    return s_panel;
}

esp_lcd_panel_io_handle_t display_get_panel_io(void) {
    return s_panel_io;
}
