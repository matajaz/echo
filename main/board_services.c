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

// N06: track completion STAGES, not just object existence. The idempotence
// guard must mean "fully initialized", and a retry after a mid-stage failure
// must RESUME the incomplete stage — never recreate an already-created bus
// (that returns ESP_ERR_INVALID_STATE on an occupied port) and never report
// ready before the safe-pin config actually ran.
static bool s_bus_created = false;
static bool s_exp_created = false;
static bool s_pins_ready = false;

esp_err_t board_services_init(void) {
    if (s_pins_ready) return ESP_OK;   // fully up (not merely "handle exists")

    // Stage 1: I2C bus — only if not already created (reuse a valid bus).
    if (!s_bus_created) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = BRD_I2C_PORT,
            .sda_io_num = BRD_PIN_SDA,
            .scl_io_num = BRD_PIN_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init failed");
        s_bus_created = true;
    }

    // Stage 2: CH32 expander — only if not already created.
    if (!s_exp_created) {
        ESP_RETURN_ON_ERROR(
            custom_io_expander_new_i2c_ch32v003(
                s_bus, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &s_exp),
            TAG, "CH32V003 IO expander init failed");
        s_exp_created = true;
    }

    // Stage 3: safe pin config (LCD_RST high, PA off). Only marks ready when ALL
    // of it succeeds; a failure here leaves s_pins_ready false so the next call
    // retries THIS stage (bus + expander are reused, not recreated). PA errors
    // are now checked (were silently ignored).
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_exp, BRD_LCD_RST_PIN, IO_EXPANDER_OUTPUT),
                        TAG, "LCD_RST dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_LCD_RST_PIN, 1),
                        TAG, "LCD_RST high");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_exp, BRD_PA_PIN, IO_EXPANDER_OUTPUT),
                        TAG, "PA dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_PA_PIN, 0),
                        TAG, "PA low");
    s_pins_ready = true;

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
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_exp, BRD_PA_PIN, IO_EXPANDER_OUTPUT), TAG, "PA direction");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_exp, BRD_PA_PIN, on ? 1 : 0), TAG, "PA latch");
    vTaskDelay(pdMS_TO_TICKS(30));
    uint32_t level=0;
    ESP_RETURN_ON_ERROR(esp_io_expander_get_level(s_exp, BRD_PA_PIN, &level), TAG, "PA readback");
    if (!!(level & BRD_PA_PIN) != on) {
        // On this board an acknowledged latch write can leave EXIO3 low.
        // Hardware-qualified recovery (v149): reconfigure only PA as input,
        // then output, preserving every other direction/output bit. The PA
        // pull-down keeps the amplifier disabled during the transition.
        // Force callbacks: set_dir() otherwise skips the cached output mode.
        uint32_t direction=0;
        ESP_RETURN_ON_ERROR(s_exp->read_direction_reg(s_exp, &direction), TAG, "PA direction shadow");
        esp_err_t transition=s_exp->write_direction_reg(s_exp, direction & ~BRD_PA_PIN);
        vTaskDelay(pdMS_TO_TICKS(30));
        esp_err_t restore=s_exp->write_direction_reg(s_exp, direction | BRD_PA_PIN);
        if (restore!=ESP_OK) return restore;
        if (transition!=ESP_OK) return transition;
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_RETURN_ON_ERROR(esp_io_expander_get_level(s_exp, BRD_PA_PIN, &level), TAG, "PA recovery readback");
    }
    ESP_LOGI(TAG,"PA %s: actual=%d",on?"on":"off",!!(level & BRD_PA_PIN));
    return !!(level & BRD_PA_PIN)==on?ESP_OK:ESP_ERR_INVALID_STATE;
}
