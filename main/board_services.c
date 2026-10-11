// board_services.c — single owner of the shared I2C bus + CH32V003 expander.
// See board_services.h. Values mirror the vendor BSP exactly (confirmed, not
// guessed): I2C0 SDA=GPIO0 SCL=GPIO1; CH32V003 expander @ its fixed address;
// LCD_RST = expander pin 1; speaker PA = expander pin 3 (active HIGH).
#include "board_services.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "custom_io_expander_ch32v003.h"

static const char *TAG = "board_svc";

#define BRD_I2C_PORT      I2C_NUM_0
#define BRD_PIN_SDA       GPIO_NUM_0
#define BRD_PIN_SCL       GPIO_NUM_1
#define BRD_LCD_RST_PIN   IO_EXPANDER_PIN_NUM_1
#define BRD_PA_PIN        IO_EXPANDER_PIN_NUM_3

static i2c_master_bus_handle_t s_bus = NULL;
static esp_io_expander_handle_t s_exp = NULL;

esp_err_t board_services_init(void) {
    if (s_exp != NULL) return ESP_OK;   // idempotent; bus+expander already up

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BRD_I2C_PORT,
        .sda_io_num = BRD_PIN_SDA,
        .scl_io_num = BRD_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG,
                         "I2C bus init failed");
    ESP_RETURN_ON_ERROR(
        custom_io_expander_new_i2c_ch32v003(
            s_bus, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &s_exp),
        TAG, "CH32V003 IO expander init failed");

    // Default LCD_RST high (not held in reset) + PA off until explicitly asked.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_exp, BRD_LCD_RST_PIN, IO_EXPANDER_OUTPUT),
                        TAG, "LCD_RST dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_LCD_RST_PIN, 1),
                        TAG, "LCD_RST high");
    esp_io_expander_set_dir(s_exp, BRD_PA_PIN, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_exp, BRD_PA_PIN, 0);

    ESP_LOGI(TAG, "board services up (shared I2C bus + CH32 expander)");
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)   { return s_bus; }
esp_io_expander_handle_t board_io_expander(void) { return s_exp; }

esp_err_t board_lcd_reset(void) {
    if (s_exp == NULL) return ESP_ERR_INVALID_STATE;
    // 200ms low / 200ms high — matches the vendor's proven timing.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_LCD_RST_PIN, 0), TAG, "rst low");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_LCD_RST_PIN, 1), TAG, "rst high");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_err_t board_backlight_set(int percent) {
    if (s_exp == NULL) return ESP_ERR_INVALID_STATE;
    if (percent < 0) percent = 0; else if (percent > 100) percent = 100;
    uint8_t pwm = (uint8_t)((percent * 255) / 100);
    return custom_io_expander_set_pwm(s_exp, pwm);
}

esp_err_t board_pa_enable(bool on) {
    if (s_exp == NULL) return ESP_ERR_INVALID_STATE;
    return esp_io_expander_set_level(s_exp, BRD_PA_PIN, on ? 1 : 0);
}
