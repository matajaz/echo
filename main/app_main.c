// echo -- minimal ESP32-C5 firmware bring-up.
// Scope: get WiFi + pull-OTA working on known-good infrastructure before
// building the real application on top. See /Volumes/repo/esp/echo/README.md
// for hardware details and why this project exists (stock xiaozhi-esp32
// firmware's WiFi-config fallback could not be triggered remotely).

#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "secrets.h"
#include "ota.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "sensors.h"
#include "audio.h"
#include "weblog.h"

static const char *TAG = "echo";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10
// Bounded connection budget. A single attempt can only ever wait this long
// for WIFI_CONNECTED_BIT/WIFI_FAIL_BIT: DHCP may never complete and some
// failure modes emit no terminal event at all, so an unbounded
// portMAX_DELAY wait would hang boot forever with no diagnosis.
#define WIFI_ATTEMPT_TIMEOUT_MS 30000
#define WIFI_MAX_ATTEMPTS 3

static int s_retry_count = 0;
// Set while we deliberately stop the STA, so the DISCONNECTED event that
// esp_wifi_stop() emits does not trigger a reconnect mid-restart.
static volatile bool s_wifi_stopping = false;

#define OTA_START_ATTEMPTS 3
#define OTA_START_RETRY_MS 1000
#define OTA_START_REBOOT_SECONDS 5

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() at start failed: 0x%x", err);
            // No DISCONNECTED event will follow a synchronous failure, so
            // burn a retry and surface FAIL ourselves if the budget is gone.
            s_retry_count++;
            if (s_retry_count >= WIFI_MAX_RETRY) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_stopping) {
            return;
        }
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_err_t err = esp_wifi_connect();
            s_retry_count++;
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
            } else {
                ESP_LOGW(TAG, "WiFi retry %d/%d rejected by driver: 0x%x",
                         s_retry_count, WIFI_MAX_RETRY, err);
            }
        } else {
            ESP_LOGE(TAG, "WiFi retry budget exhausted");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // IMPORTANT: do NOT call esp_wifi_set_ps(WIFI_PS_NONE) -- known to break
    // hosted/SDIO-NCP links on other boards in this workspace. ESP32-C5 has
    // its own native radio (not a hosted NCP) but there's no reason to
    // disable power-save here either; leave the default.
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", WIFI_SSID);

    // Bounded wait loop: a connect attempt that produces neither GOT_IP nor
    // FAIL (silent DHCP stall, driver error with no DISCONNECTED) is treated
    // as a failure after WIFI_ATTEMPT_TIMEOUT_MS instead of blocking forever.
    // xEventGroupClearBits() makes sure a late FAIL bit from a previous
    // attempt cannot short-circuit the next one.
    for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_ATTEMPT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected (attempt %d)", attempt);
            return true;
        }

        if (bits & WIFI_FAIL_BIT) {
            ESP_LOGE(TAG, "WiFi association failed (attempt %d/%d)",
                     attempt, WIFI_MAX_ATTEMPTS);
        } else {
            ESP_LOGE(TAG, "WiFi attempt %d/%d timed out after %d ms (no GOT_IP)",
                     attempt, WIFI_MAX_ATTEMPTS, WIFI_ATTEMPT_TIMEOUT_MS);
        }

        // Restart the driver for the next attempt. s_wifi_stopping keeps the
        // DISCONNECTED event this generates from recursing into the retry path.
        if (attempt < WIFI_MAX_ATTEMPTS) {
            s_wifi_stopping = true;
            esp_wifi_stop();
            s_wifi_stopping = false;
            esp_wifi_start();
        }
    }

    ESP_LOGE(TAG, "WiFi connection failed after %d attempts", WIFI_MAX_ATTEMPTS);
    return false;
}

// Touch poll task: logs each touch coordinate so touch can be verified
// remotely via /log (no USB needed). Only logs on the touch-down edge,
// not every idle poll, to keep the ring useful.
static void touch_poll_task(void *arg) {
    (void)arg;
    uint16_t x = 0, y = 0;
    bool was_touched = false;
    for (;;) {
        if (touch_get_point(&x, &y)) {
            if (!was_touched) {
                ESP_LOGI(TAG, "touch: x=%u y=%u", (unsigned)x, (unsigned)y);
                was_touched = true;
            }
        } else {
            was_touched = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Bring up the HTTP server and the routes that are safe to expose at this
// point. /metrics is deliberately NOT registered here: it must only exist
// once sensors_init() has published its state, otherwise the handler races
// the poll task's writes (see sensors_get()/s_mtx in sensors.c). It is
// registered at the end of sensors bring-up instead.
static httpd_handle_t start_http_stack(void) {
    for (int attempt = 1; attempt <= OTA_START_ATTEMPTS; attempt++) {
        httpd_handle_t server = ota_server_start();
        if (server == NULL) {
            ESP_LOGE(TAG, "ota_server_start() failed (attempt %d/%d)",
                     attempt, OTA_START_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(OTA_START_RETRY_MS));
            continue;
        }
        ESP_LOGI(TAG, "OTA HTTP server up (attempt %d)", attempt);

        if (weblog_register(server) == ESP_OK) {
            ESP_LOGI(TAG, "weblog routes registered (/log, /log.txt)");
        } else {
            ESP_LOGW(TAG, "weblog route registration failed");
        }
        return server;
    }
    return NULL;
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Observability FIRST (esp32-lessons.md baseline order: WiFi+HTTP ->
    // web log -> OTA -> API). Install the log ring hook before any other
    // subsystem logs, so their startup lines are captured too. The HTTP
    // routes for it are registered right after the server starts, below.
    weblog_init();

    ESP_LOGI(TAG, "echo firmware starting (ESP32-C5)");

    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "No WiFi -- cannot start OTA server. Halting network-dependent init.");
        return;
    }

    httpd_handle_t server = start_http_stack();
    if (server == NULL) {
        // The HTTP server is the ONLY remote-recovery path for this board
        // (no USB console, no serial fallback). Continuing with it down
        // would mean a firmware bug is now unfixable in the field, so make
        // the degraded state explicit and reboot to retry from a clean boot.
        ESP_LOGE(TAG, "HTTP server never came up after %d attempts -- no remote-recovery "
                      "path; rebooting to retry", OTA_START_ATTEMPTS);
        for (int i = OTA_START_REBOOT_SECONDS; i > 0; i--) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGE(TAG, "rebooting in %d s (OTA/web console unavailable)", i);
        }
        esp_restart();
    }

    // SHARED-I2C OWNERSHIP CONTRACT (read before adding another bus user).
    // display_init() creates ONE i2c_master bus (display_get_i2c_bus()) that
    // touch, sensors, audio and the LVGL touch poller all share, and the
    // CH32V003 expander sits on it too. From here on there are up to four
    // concurrent users:
    //   1. LVGL's own touch/indev task   (only when ui_init() succeeds)
    //   2. touch_poll_task               (only on the ui_init() fallback path)
    //   3. sensors' poll task
    //   4. audio init/playback (codec + PA enable through the expander)
    // The IDF I2C-master driver serializes individual transactions against
    // the bus, so no two tasks ever drive SDA/SCL at the same instant -- but
    // serialization is NOT the same as atomicity across a compound operation.
    // The CH32V003 GPIO expander is driven by read-modify-write, which is only
    // safe while nothing else interleaves a transaction, so any NEW bus user
    // must either reuse the existing per-device driver APIs (touch_*/sensors_*/
    // audio_*) or take the bus behind a mutex spanning its whole compound
    // operation. Adding a bare i2c_master_* transaction here is not enough.
    // Note also that exactly one touch reader exists by construction: the
    // standalone touch_poll_task is only started when ui_init() failed, i.e.
    // when LVGL is not polling touch itself.

    // Display bring-up (2026-10-02): minimal first step toward the
    // project's eventual LVGL/touch/audio/sensors goal -- panel on, solid
    // color, backlight on. Deliberately NON-FATAL: if the panel/expander
    // init fails, log it and keep running WiFi+OTA, since OTA is the
    // recovery path if a later display change bricks the UI -- the one
    // thing that must never be blocked by a display bug.
    esp_err_t disp_ret = display_init();
    if (disp_ret == ESP_OK) {
        display_set_backlight(100);
        ESP_LOGI(TAG, "display: panel on, backlight 100%%");

        // Touch (2026-10-02): reuses display's I2C bus + CH32V003 expander.
        // Non-fatal, same reasoning as the display init above.
        esp_err_t touch_ret = touch_init();
        if (touch_ret == ESP_OK) {
            ESP_LOGI(TAG, "touch: init ok");
        } else {
            ESP_LOGE(TAG, "touch: init failed (0x%x) -- continuing without it", touch_ret);
        }

        // LVGL (2026-10-02): attaches to the panel + touch handles above
        // and takes OVER drawing + touch input. Because LVGL now owns the
        // panel's draw path and polls the touch controller itself (via its
        // own task), we do NOT do the raw display_fill() test color or run
        // the standalone touch_poll_task here -- both would race LVGL for
        // the SPI panel / the I2C touch controller. If LVGL init FAILS we
        // fall back to the raw green fill + poll task so the lower layers
        // are still demonstrably alive. Non-fatal either way.
        esp_err_t ui_ret = ui_init();
        if (ui_ret == ESP_OK) {
            ESP_LOGI(TAG, "ui: LVGL up, owns panel + touch");
        } else {
            ESP_LOGE(TAG, "ui: LVGL init failed (0x%x) -- falling back to raw fill/poll", ui_ret);
            display_fill(0x07E0);
            if (touch_ret == ESP_OK) {
                xTaskCreate(touch_poll_task, "touch_poll", 3072, NULL, 4, NULL);
            }
        }

        // Sensors (2026-10-02): QMI8658 IMU + PCF85063A RTC + SHTC3
        // temp/humidity, all on the SAME shared I2C bus (reuses
        // display_get_i2c_bus()). Non-fatal and independent of the UI --
        // each sensor's own *_ok flag gates it, and the poll task logs
        // readings to /log for verification.
        esp_err_t sens_ret = sensors_init();
        if (sens_ret != ESP_OK) {
            ESP_LOGE(TAG, "sensors: init failed (0x%x) -- continuing without them", sens_ret);
        }

        // /metrics (Prometheus) is registered ONLY here, after sensors_init()
        // has created its mutex and published the first snapshot. Registering
        // it earlier would let a scraper call sensors_get() concurrently with
        // sensors_init() writing s_started/s_data.
        if (server != NULL && sensors_register_metrics(server) == ESP_OK) {
            ESP_LOGI(TAG, "metrics route registered (/metrics)");
        } else {
            ESP_LOGW(TAG, "metrics route registration failed");
        }

        // Audio (2026-10-02): ES8311 codec over I2S + PA enable via the
        // CH32 expander. Reuses the shared I2C bus + expander. Non-fatal.
        // On success, play a short startup chime so audio output is
        // audibly verifiable immediately (speaker on the SPK connector).
        esp_err_t aud_ret = audio_init();
        if (aud_ret == ESP_OK) {
            ESP_LOGI(TAG, "audio: ES8311 up, playing init beep");
            audio_play_tone(1000, 150);
        } else {
            ESP_LOGE(TAG, "audio: init failed (0x%x) -- continuing without it", aud_ret);
        }
    } else {
        ESP_LOGE(TAG, "display: init failed (0x%x) -- continuing without it", disp_ret);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive");
    }
}
