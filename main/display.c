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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "custom_io_expander_ch32v003.h"
#include "esp_io_expander.h"
#include "board_services.h"

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
static bool s_spi_bus_ready = false;

// Signalled from the panel-IO "color transfer done" callback so display_fill()
// can WAIT for a queued DMA transfer to finish before freeing its source
// buffer. esp_lcd_panel_draw_bitmap() is ASYNC — freeing chunk_buf right after
// the call was a use-after-free while DMA was still reading it (R6, ASan
// heap-use-after-free, 2026-10-10).
static SemaphoreHandle_t s_trans_done = NULL;

static bool IRAM_ATTR display_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                            esp_lcd_panel_io_event_data_t *ed,
                                            void *user_ctx) {
    (void)io; (void)ed; (void)user_ctx;
    BaseType_t hp = pdFALSE;
    if (s_trans_done) xSemaphoreGiveFromISR(s_trans_done, &hp);
    return hp == pdTRUE;
}

// Undo any partial display bring-up so a later retry of display_init() is
// not permanently wedged by ESP_ERR_INVALID_STATE from spi_bus_initialize().
//
// PARTIAL-INIT UNWIND SCOPE (2026-10-10): this frees the DISPLAY-OWNED
// resources only — panel, panel-IO, SPI bus. It deliberately does NOT free the
// I2C bus (s_i2c_bus) or the CH32 IO expander (s_io_expander): although
// display_init() creates them, they are a SHARED board resource that
// sensors.c, touch.c and audio.c all consume via display_get_i2c_bus() /
// display_get_io_expander(). Those subsystems init even if the display fails,
// so the bus/expander must SURVIVE a display failure — freeing them here would
// pull the bus out from under the other users (a bug, not a leak fix). Making
// the shared I2C bus a first-class board service owned outside display is the
// proper long-term refactor (review item), not something this unwind should do.
static void display_release_partial(void) {
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_panel_io != NULL) {
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
    }
    if (s_spi_bus_ready) {
        spi_bus_free(LCD_SPI_HOST);
        s_spi_bus_ready = false;
    }
}

static esp_err_t io_expander_init(void) {
    if (s_io_expander != NULL) {
        return ESP_OK; // already brought up
    }
    // board_services owns the SHARED I2C bus + CH32 expander now (review order
    // 9). display just consumes the handles, so a display failure can't tear
    // the bus down under touch/sensors/audio. Values live in board_services.c.
    ESP_RETURN_ON_ERROR(board_services_init(), TAG, "board_services_init failed");
    s_i2c_bus = board_i2c_bus();
    s_io_expander = board_io_expander();
    if (s_i2c_bus == NULL || s_io_expander == NULL) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "CH32V003 IO expander init success");
    return ESP_OK;
}

static esp_err_t lcd_reset_pulse(void) {
    // Delegated to board_services (the shared-expander owner); keeps the proven
    // 200ms low / 200ms high vendor timing in one place.
    return board_lcd_reset();
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
    s_spi_bus_ready = true;

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_err_t ret = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_panel_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed: 0x%x", ret);
        display_release_partial();
        return ret;
    }

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
    ret = esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: 0x%x", ret);
        display_release_partial();
        return ret;
    }

    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel init failed: 0x%x", ret);
        display_release_partial();
        return ret;
    }
    // Confirmed from vendor source: this exact panel needs color inversion,
    // otherwise every drawn color (including this bring-up's fill) comes
    // out as its photographic negative.
    ret = esp_lcd_panel_invert_color(s_panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 color-invert failed: 0x%x", ret);
        display_release_partial();
        return ret;
    }
    ret = esp_lcd_panel_disp_on_off(s_panel, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 display-on failed: 0x%x", ret);
        display_release_partial();
        return ret;
    }

    s_display_ready = true;
    ESP_LOGI(TAG, "display init success (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

esp_err_t display_fill(uint16_t rgb565_color) {
    if (!s_display_ready) {
        ESP_LOGE(TAG, "display_fill called before display_init succeeded");
        return ESP_ERR_INVALID_STATE;
    }

    // Fill a chunk of rows per DMA transfer instead of one row at a time.
    // The SPI bus was initialized with max_transfer_sz = LCD_H_RES * 80 * 2,
    // so up to 80 rows fit one draw_bitmap. V02 uses a smaller 40-row chunk on
    // this degraded fallback path to keep the single DMA allocation modest.
    // V02 (2026-10-10): set up AND verify the completion mechanism BEFORE we
    // allocate pixels or enqueue any transfer. If we cannot prove when a DMA
    // transfer finishes, we must never enqueue-then-free (use-after-free on the
    // low-memory/degraded fallback path). Order: create semaphore -> register
    // callback (checked) -> only then allocate + draw.
    if (s_trans_done == NULL) {
        s_trans_done = xSemaphoreCreateBinary();
    }
    if (s_trans_done == NULL) {
        ESP_LOGE(TAG, "display_fill: no completion semaphore (OOM) -- refusing "
                      "to enqueue DMA without a way to know when it finishes");
        return ESP_ERR_NO_MEM;   // nothing allocated/enqueued yet: safe
    }
    // R6 (reworked 2026-10-10): register the completion callback HERE, not in
    // the panel-IO config. esp_lvgl_port registers its OWN on_color_trans_done
    // on this same IO; putting ours in the IO config fought LVGL for the single
    // callback slot and stalled the LVGL flush+touch loop (touch went dead).
    // display_fill only runs on the LVGL-init-FAILED fallback path, so LVGL is
    // not attached and this registration is safe/exclusive.
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = display_trans_done_cb };
    esp_err_t cbret = esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, NULL);
    if (cbret != ESP_OK) {
        // V02: without the callback the semaphore is never given, so we could
        // never prove completion -> never safe to free an enqueued buffer.
        // Refuse up front rather than enqueue blindly.
        ESP_LOGE(TAG, "display_fill: trans-done cb register failed: 0x%x", cbret);
        return cbret;   // nothing enqueued yet: safe
    }
    // Drain any stale completion token so the first wait reflects THIS fill.
    xSemaphoreTake(s_trans_done, 0);

    // Smaller bounded staging chunk for this degraded fallback path (V02): keep
    // the single DMA-capable allocation modest. 40 rows * 240 * 2B = ~19 KB.
    const int chunk_rows = 40;
    const size_t chunk_pixels = (size_t)LCD_H_RES * chunk_rows;
    uint16_t *chunk_buf = heap_caps_malloc(chunk_pixels * 2, MALLOC_CAP_DMA);
    if (chunk_buf == NULL) {
        ESP_LOGE(TAG, "display_fill: chunk buffer alloc failed (%u bytes)",
                 (unsigned)(chunk_pixels * 2));
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < chunk_pixels; i++) {
        chunk_buf[i] = rgb565_color;
    }

    esp_err_t ret = ESP_OK;
    bool dma_in_flight = false;   // true if a transfer was enqueued but not proven done
    for (int y = 0; y < LCD_V_RES && ret == ESP_OK; y += chunk_rows) {
        int y_end = y + chunk_rows;
        if (y_end > LCD_V_RES) {
            y_end = LCD_V_RES;
        }
        ret = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y_end, chunk_buf);
        if (ret == ESP_OK) {
            dma_in_flight = true;
            // R6: draw_bitmap is ASYNC — the DMA engine is still reading
            // chunk_buf after it returns. Wait for THIS transfer to complete
            // before reusing/freeing the buffer. Bounded (1 s) so a stuck
            // transfer on the degraded fallback path can't hang forever.
            if (xSemaphoreTake(s_trans_done, pdMS_TO_TICKS(1000)) == pdTRUE) {
                dma_in_flight = false;   // proven done: buffer is now free to reuse/free
            } else {
                ESP_LOGW(TAG, "display_fill: trans-done wait timed out");
                ret = ESP_ERR_TIMEOUT;
                // dma_in_flight stays true: a timeout does NOT prove the DMA
                // stopped. Falling through to free() would be a use-after-free.
            }
        }
    }

    // V02: only free the staging buffer when NO transfer is still in flight. A
    // timeout cannot cancel an async transfer, so freeing then would let the
    // DMA engine read freed memory. On the degraded fallback path we instead
    // deliberately leak this one bounded (~19 KB) chunk — a contained,
    // one-shot leak is strictly safer than a use-after-free, and this path only
    // runs when the display is already failing. The next full reinit/reboot
    // reclaims it.
    if (!dma_in_flight) {
        free(chunk_buf);
    } else {
        ESP_LOGE(TAG, "display_fill: DMA still in flight after timeout -- "
                      "leaking %u-byte staging buffer rather than freeing it "
                      "(use-after-free guard)", (unsigned)(chunk_pixels * 2));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "display_fill: draw_bitmap failed: 0x%x", ret);
    }
    return ret;
}

esp_err_t display_set_backlight(int percent) {
    esp_err_t ret = board_backlight_set(percent);   // owned by board_services
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
