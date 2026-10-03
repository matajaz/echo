// echo -- touch bring-up implementation. See touch.h for hardware facts.
#include "touch.h"

#include "display.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst3530.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define PIN_TOUCH_INT GPIO_NUM_5
#define LCD_H_RES 240
#define LCD_V_RES 320

// IO_EXPANDER_PIN_NUM_0 on the CH32V003 == TP_RST (confirmed via the
// vendor board header's `#define IO_LCD_TOUCH_RST (IO_EXPANDER_PIN_NUM_0)`),
// same sourcing discipline as display.c's IO_LCD_RST.
#define IO_TOUCH_RST IO_EXPANDER_PIN_NUM_0

static esp_lcd_touch_handle_t s_touch = NULL;

// touch_init() is callable from more than one task path, so the
// check-then-set on s_touch is a TOCTOU race: two callers could both see
// NULL and both run the CST3530 bring-up. A plain FreeRTOS mutex with
// static storage is used instead of portMUX so the (slow) I2C bring-up
// does not run with interrupts disabled.
static SemaphoreHandle_t s_touch_mutex = NULL;

static esp_err_t touch_init_locked(void);

static esp_err_t touch_reset_pulse(void) {
    esp_io_expander_handle_t expander = display_get_io_expander();
    // 10ms low / 10ms high -- matches the vendor source's own timing for
    // the touch reset pulse specifically (shorter than the LCD's 200ms;
    // the two chips have different real reset requirements, not a typo).
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(expander, IO_TOUCH_RST, IO_EXPANDER_OUTPUT),
        TAG, "set IO_TOUCH_RST direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, IO_TOUCH_RST, 0),
                         TAG, "touch reset (low) failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, IO_TOUCH_RST, 1),
                         TAG, "touch reset (high) failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t touch_init(void) {
    // Serialise concurrent callers across the whole check-and-set so the
    // CST3530 is only ever brought up once.
    if (s_touch_mutex == NULL) {
        s_touch_mutex = xSemaphoreCreateMutex();
        if (s_touch_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_touch_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t init_ret = touch_init_locked();

    xSemaphoreGive(s_touch_mutex);
    return init_ret;
}

static esp_err_t touch_init_locked(void) {
    if (s_touch != NULL) {
        return ESP_OK; // already brought up
    }

    i2c_master_bus_handle_t i2c_bus = display_get_i2c_bus();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "touch_init called before display_init succeeded");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(touch_reset_pulse(), TAG, "touch reset pulse failed");

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG();
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &tp_io_handle), TAG,
        "touch panel IO init failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, // reset already done above via the expander
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
        // This specific CST3530 driver (waveshare v0.0.2) reads the I2C
        // MASTER BUS handle from config.driver_data -- NOT from the panel
        // IO handle like most esp_lcd_touch drivers. Confirmed from its
        // source (esp_lcd_touch_cst3530.c:122: `bus = (i2c_master_bus_
        // handle_t)tp->config.driver_data`). Leaving it NULL made init
        // fail with "No I2C bus handle in driver_data" -> "I2C ctx not
        // ready" (caught live via the new /log endpoint).
        .driver_data = (void *)i2c_bus,
    };
    esp_err_t ret = esp_lcd_touch_new_i2c_cst3530(tp_io_handle, &tp_cfg, &s_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CST3530 touch create failed: 0x%x", ret);
        // The panel IO was created successfully and is owned by us until the
        // touch driver takes it, so release it here or it leaks on every
        // failed (e.g. retried) init.
        esp_lcd_panel_io_del(tp_io_handle);
        return ret;
    }

    ESP_LOGI(TAG, "touch init success");
    return ESP_OK;
}

esp_lcd_touch_handle_t touch_get_handle(void) {
    return s_touch;
}

bool touch_get_point(uint16_t *x, uint16_t *y) {
    if (s_touch == NULL) {
        return false;
    }

    esp_err_t ret = esp_lcd_touch_read_data(s_touch);
    if (ret != ESP_OK) {
        return false;
    }

    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_count = 0;
    ret = esp_lcd_touch_get_data(s_touch, &point, &point_count, 1);
    if (ret != ESP_OK || point_count == 0) {
        return false;
    }

    *x = point.x;
    *y = point.y;
    return true;
}
