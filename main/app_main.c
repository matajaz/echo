// echo -- minimal ESP32-C5 firmware bring-up.
// Scope: get WiFi + pull-OTA working on known-good infrastructure before
// building the real application on top. See /Volumes/repo/esp/echo/README.md
// for hardware details and why this project exists (stock xiaozhi-esp32
// firmware's WiFi-config fallback could not be triggered remotely).

#include <inttypes.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "secrets.h"
#include "ota.h"
#include "display.h"
#include "board_services.h"
#include "touch.h"
#include "ui.h"
#include "sensors.h"
#include "ha_client.h"
#include "audio.h"
#include "weblog.h"
#include "readiness.h"   // host-tested readiness/probation/deadline policy

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

// Dual-SSID fallback (2026-10-11): profile 0 = home (WIFI_SSID), profile 1 =
// phone hotspot (WIFI_SSID_2). When the active profile fails to connect
// WIFI_PROFILE_SWITCH_AT times in a row, switch to the other profile (if a
// fallback SSID is configured). This keeps the board reachable (OTA/HA) when
// the home AP is down by hopping onto the phone, and hops back when home
// returns. Compile-time creds (secrets.h) — no NVS provisioning needed.
#define WIFI_PROFILE_SWITCH_AT 5
static int s_wifi_profile = 0;   // 0 = home, 1 = phone; written by the WiFi handler task only

// True if a usable fallback SSID is configured.
static inline bool wifi_have_fallback(void) {
    return WIFI_SSID_2[0] != '\0';
}

// Load profile `idx` into the STA config and apply it. Returns the SSID in use,
// or NULL if the config could not be applied. N08: a 32-byte SSID is LEGAL and
// the esp_wifi ssid[32] field does NOT require a terminator at full width, so a
// strlcpy (which keeps only 31 bytes) would truncate it. Copy into a ZEROED
// config with memcpy bounded by the real length (<=32). The profile index is
// updated ONLY after esp_wifi_set_config() succeeds.
static const char *wifi_apply_profile(int idx) {
    const char *ssid = (idx == 1) ? WIFI_SSID_2 : WIFI_SSID;
    const char *pass = (idx == 1) ? WIFI_PASS_2 : WIFI_PASS;
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi_5g_adjustment = 10,
        },
    };
    // An open hotspot (empty password) must advertise OPEN authmode, else the
    // join is rejected on threshold. A phone hotspot normally has a password.
    if (pass[0] == '\0') cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // SSID: full-width copy (cfg is zero-initialized above, so a <32-byte SSID
    // is naturally NUL-padded; a 32-byte SSID fills the field with no loss).
    // SSID comes from a compile-time literal (NUL-terminated); cap at the field.
    size_t slen = strlen(ssid);
    if (slen > sizeof(cfg.sta.ssid)) slen = sizeof(cfg.sta.ssid);
    memcpy(cfg.sta.ssid, ssid, slen);
    // Password field is 64 bytes and the driver treats it as a C string, so a
    // NUL-terminated copy is correct here.
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    esp_err_t cr = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (cr != ESP_OK) {
        ESP_LOGE(TAG, "wifi_apply_profile(%d) set_config failed: 0x%x", idx, cr);
        return NULL;                 // leave s_wifi_profile unchanged
    }
    s_wifi_profile = idx;            // only after a successful apply
    return ssid;
}

static int s_retry_count = 0;
// Guards s_retry_count, which is written by the event-loop task (the WiFi
// handler) and by app_main (wifi_init_sta). Always access it via the helpers
// below to avoid a data race between the two tasks.
static SemaphoreHandle_t s_retry_mtx;

static int retry_count_inc(void) {
    int v = s_retry_count;
    if (s_retry_mtx) {
        xSemaphoreTake(s_retry_mtx, portMAX_DELAY);
        v = ++s_retry_count;
        xSemaphoreGive(s_retry_mtx);
    }
    return v;
}

static void retry_count_reset(void) {
    if (s_retry_mtx) {
        xSemaphoreTake(s_retry_mtx, portMAX_DELAY);
        s_retry_count = 0;
        xSemaphoreGive(s_retry_mtx);
    } else {
        s_retry_count = 0;
    }
}

#define OTA_START_ATTEMPTS 3
#define OTA_START_RETRY_MS 1000
#define OTA_START_REBOOT_SECONDS 5
// Cap on consecutive reboots triggered by a failed HTTP bring-up. Kept in RTC
// no-init memory so it survives the reboot itself; without a cap a persistent
// failure (port conflict, netif issue) becomes an unconditional boot loop.
#define OTA_MAX_REBOOTS 3
static RTC_NOINIT_ATTR uint32_t s_ota_boot_count;
// R11: set true when app_main finds the running image PENDING_VERIFY; the main
// loop confirms it after the first stable heartbeat tick. Single-task (app_main).
static bool s_ota_verify_pending = false;

// V01 (2026-10-10): first-boot readiness + bounded probation + an independent
// deadline so a freshly-OTA'd image that HANGS before (or during) the service
// loop still rolls back. Task-WDT panic is disabled on this board, so without
// this a pre-loop init hang would stay PENDING_VERIFY forever (never confirmed,
// never rolled back) until an external reset.
//
// Essential readiness for a wall display = the HTTP maintenance server (the
// only remote-recovery path; already required to reach the confirm code) PLUS a
// usable UI. Optional hardware (sensors/HA/audio) is DEGRADED, never fatal.
// Probation + loop deadline live in the host-tested policy
// (READINESS_PROBATION_TICKS / READINESS_DEADLINE_MS). The independent esp_timer
// below is derived from the SAME deadline so a pre-loop hang and a loop-stall
// share one bound.
#define OTA_CONFIRM_DEADLINE_US ((int64_t)READINESS_DEADLINE_MS * 1000LL)
static bool s_ui_ready = false;                  // LVGL up, OR raw-fallback render succeeded
static bool s_input_ready = false;               // touch attached (readiness input)
static uint32_t s_pending_start_ms = 0;          // monotonic ms when PENDING_VERIFY detected
static volatile bool s_ota_confirmed = false;    // set once the image is marked valid
static esp_timer_handle_t s_ota_deadline_timer = NULL;

// Independent deadline: if we have not confirmed the pending image within
// OTA_CONFIRM_DEADLINE_US, assume init/loop wedged and restart. A still-pending
// image restarting => the bootloader rolls it back to the previous good slot.
static void ota_confirm_deadline_cb(void *arg) {
    (void)arg;
    if (s_ota_confirmed) return;   // already confirmed in time: nothing to do
    ESP_LOGE(TAG, "OTA confirm deadline (%llds) elapsed without a stable, ready "
                  "heartbeat -- restarting so the bootloader rolls back the "
                  "unconfirmed image", (long long)(OTA_CONFIRM_DEADLINE_US / 1000000LL));
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect() at start failed: 0x%x", err);
            // No DISCONNECTED event will follow a synchronous failure, so
            // burn a retry and surface FAIL ourselves if the budget is gone.
            if (retry_count_inc() >= WIFI_MAX_RETRY) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // Log the actual disconnect reason (was ignored): distinguishes RF
        // loss (e.g. BEACON_TIMEOUT/NO_AP_FOUND), auth failure (handshake/
        // password) and AP-side de-auth — key for diagnosing the flaky link.
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t dreason = dis ? dis->reason : 0;
        // PERPETUAL RECONNECT (finding #4, 2026-10-10). Previously this stopped
        // calling esp_wifi_connect() once retry_count hit WIFI_MAX_RETRY and only
        // set WIFI_FAIL_BIT — so on a flaky link (observed: bursts of
        // "wifi:m f null" missed-beacon warnings) the board burned its budget
        // and then stayed OFFLINE forever, leaving OTA + HA unreachable until a
        // manual reboot. Now: we ALWAYS try to reconnect. The retry COUNT is
        // still used to end the INITIAL boot wait (set FAIL_BIT once so
        // wifi_init_sta() stops blocking), but reconnection itself never stops.
        int r = retry_count_inc();
        if (r == WIFI_MAX_RETRY) {
            // Unblock the initial boot wait exactly once; services can start
            // and the board stays reachable for recovery once the link returns.
            ESP_LOGW(TAG, "WiFi still down after %d tries (reason=%u) — proceeding; "
                          "will keep retrying", WIFI_MAX_RETRY, dreason);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        // Past the initial burst, pace the reconnect attempts so we don't spin
        // the event-loop task on a hard-down AP. A short delay here is safe (the
        // driver already paces DISCONNECTED events); cap it so recovery stays
        // snappy when the AP returns.
        if (r > WIFI_MAX_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        // Dual-SSID fallback: after WIFI_PROFILE_SWITCH_AT consecutive failures
        // on the current profile, hop to the other one (home <-> phone) if a
        // fallback SSID is configured. r counts consecutive failures (reset on
        // GOT_IP), so this flips roughly every WIFI_PROFILE_SWITCH_AT tries
        // until one associates — keeping OTA/HA reachable via whichever is up.
        if (wifi_have_fallback() && r > 0 && (r % WIFI_PROFILE_SWITCH_AT) == 0) {
            int next = s_wifi_profile ? 0 : 1;
            const char *ss = wifi_apply_profile(next);
            if (ss != NULL) {
                ESP_LOGW(TAG, "WiFi: %d failures on previous SSID — switching to \"%s\" (profile %d)",
                         r, ss, next);
            } else {
                ESP_LOGE(TAG, "WiFi: profile switch to %d failed to apply; staying on current", next);
            }
        }
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && (r <= WIFI_MAX_RETRY || (r % 15) == 0)) {
            ESP_LOGW(TAG, "WiFi reconnect attempt %d rejected by driver: 0x%x", r, err);
        } else if (r <= WIFI_MAX_RETRY) {
            ESP_LOGW(TAG, "WiFi disconnected (reason=%u), retry %d/%d", dreason, r, WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        // Log which BAND/channel we landed on (channel 1-14 = 2.4 GHz,
        // 36+ = 5 GHz) + RSSI, so band selection is verifiable from the log.
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "Got IP: " IPSTR " | ch=%u (%s) rssi=%d",
                     IP2STR(&event->ip_info.ip), ap.primary,
                     (ap.primary >= 36) ? "5GHz" : "2.4GHz", ap.rssi);
        } else {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        retry_count_reset();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate() failed");
        return false;
    }
    s_retry_mtx = xSemaphoreCreateMutex();
    if (s_retry_mtx == NULL) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex() failed");
        // Free the event group already created above so a failed early
        // bring-up does not leak it (callers may retry init).
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return false;
    }

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
            // AP SELECTION (2026-10-10): the default WIFI_FAST_SCAN stops at
            // the FIRST matching AP, so with LilleSkutt on both bands the C5
            // kept landing on 2.4 GHz (ch 6) and never compared the 5 GHz
            // candidate. Scan ALL channels, sort by signal, and bias toward
            // 5 GHz: rssi_5g_adjustment adds 10 dB to the 5 GHz AP's score so
            // it wins when within 10 dB of the 2.4 GHz AP. Both must appear in
            // the scan (all-channel scan ensures that).
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.rssi_5g_adjustment = 10,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // Explicit country (Sweden) so the 5 GHz channel set is enabled correctly.
    esp_wifi_set_country_code("SE", true);
    // Dual-band AUTO so the station considers both 2.4 and 5 GHz candidates.
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    // NOTE: do NOT force WIFI_PS_NONE here. On the C5 it is link-safe (native
    // radio, unlike lumen's hosted NCP), BUT it makes WiFi hold more RX buffers
    // in scarce INTERNAL RAM — measured via /info to collapse int_free to ~5 KB
    // during an OTA, starving the image write/verify and causing
    // ESP_ERR_OTA_VALIDATE_FAILED (2026-10-10). The 5 GHz band selection above
    // already fixed the link quality that PS-NONE was meant to help, so the
    // default modem-sleep (which frees that internal RAM) is the right choice.
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", WIFI_SSID);

    // Bounded wait loop: a connect attempt that produces neither GOT_IP nor
    // FAIL (silent DHCP stall, driver error with no DISCONNECTED) is treated
    // as a failure after WIFI_ATTEMPT_TIMEOUT_MS instead of blocking forever.
    // xEventGroupClearBits() makes sure a late FAIL bit from a previous
    // attempt cannot short-circuit the next one.
    for (int attempt = 1; attempt <= WIFI_MAX_ATTEMPTS; attempt++) {
        // Each outer attempt gets its own full retry budget; otherwise a
        // previous attempt can leave s_retry_count already at WIFI_MAX_RETRY.
        retry_count_reset();
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

        // Restart the driver for the next attempt, temporarily dropping the
        // WIFI_EVENT handler so the DISCONNECTED event esp_wifi_stop() emits
        // asynchronously cannot be dequeued after we resume and trigger a
        // spurious reconnect (a time-based suppression flag races that event).
        if (attempt < WIFI_MAX_ATTEMPTS) {
            ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
            esp_wifi_stop();
            ESP_ERROR_CHECK(esp_event_handler_instance_register(
                WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
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
// Runs an optional subsystem initializer with a consistent, non-fatal failure
// policy and log format. Returns the init result so callers can gate dependent
// work on it.
static esp_err_t run_optional(const char *name, esp_err_t (*init_fn)(void)) {
    esp_err_t err = init_fn();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: init ok", name);
    } else {
        ESP_LOGE(TAG, "%s: init failed (0x%x) -- continuing without it", name, err);
    }
    return err;
}

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

static void clock_synced(struct timeval *timeval) { (void)timeval; sensors_clock_synced(); }

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

    // R8 (2026-10-10): do NOT abort app_main if the INITIAL connection wait
    // times out. wifi_init_sta() has already started the STA and registered
    // the perpetual-reconnect handler (R4), so WiFi keeps trying in the
    // background. Returning here used to leave the board with NO HTTP server,
    // display, sensors or UI — and because nothing consumed a later GOT_IP,
    // the OTA-recovery path never came up even after WiFi eventually
    // associated. Instead we press on and bring up every service; the HTTP
    // server (OTA/weblog) becomes reachable the moment the background
    // reconnect succeeds, and the local UI/sensors are useful offline.
    bool wifi_up = wifi_init_sta();
    if (!wifi_up) {
        ESP_LOGW(TAG, "No WiFi yet -- starting all services anyway; the perpetual "
                      "reconnect will bring OTA/web online once the AP returns.");
    }

    // SNTP: get a trustworthy wall clock for the Overview time (the RTC is not
    // NTP-synced; uptime is not a civil time). Swedish tz with DST. Non-fatal —
    // the UI shows "--:--" until the first sync lands. Safe to call before the
    // link is up; it syncs when connectivity arrives.
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb=clock_synced;
    esp_netif_sntp_init(&sntp_cfg);
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // Europe/Stockholm
    tzset();

    httpd_handle_t server = start_http_stack();
    if (server == NULL) {
        // The HTTP server is the ONLY remote-recovery path for this board
        // (no USB console, no serial fallback). Reboot once or twice from a
        // clean boot in case the failure was transient, but cap the attempts:
        // restarting forever on a persistent failure would brick the board
        // with no way to observe why. Once the cap is hit, fall through to the
        // passive alive loop below so the log ring stays visible.
        if (s_ota_boot_count < OTA_MAX_REBOOTS) {
            s_ota_boot_count++;
            ESP_LOGE(TAG, "HTTP server never came up after %d attempts -- no remote-recovery "
                          "path; rebooting to retry (%" PRIu32 "/%d)",
                     OTA_START_ATTEMPTS, s_ota_boot_count, OTA_MAX_REBOOTS);
            for (int i = OTA_START_REBOOT_SECONDS; i > 0; i--) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGE(TAG, "rebooting in %d s (OTA/web console unavailable)", i);
            }
            esp_restart();
        }
        ESP_LOGE(TAG, "HTTP server still down after %d reboot attempts -- falling back to "
                      "passive alive loop", OTA_MAX_REBOOTS);
    } else {
        s_ota_boot_count = 0;

        // OTA SELF-CONFIRM — DEFERRED to a stable heartbeat (R11, 2026-10-10).
        // sdkconfig selects CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP, so a
        // freshly-OTA'd image boots ESP_OTA_IMG_PENDING_VERIFY and STAYS pending
        // until this app confirms it. The HTTP server being up is necessary but
        // NOT sufficient: an image that then wedges in display/sensors/audio
        // init or in the main loop should still roll back. So we do NOT confirm
        // here — we only RECORD that a verify is pending, bring up every
        // service below, and confirm after the main loop has ticked a few times
        // (a brief stable heartbeat). Optional-hardware failures (display,
        // sensors, audio) are DEGRADED, not fatal: they must not block
        // confirmation, or a board with e.g. a dead sensor would roll back
        // forever and lose remote recovery. The gate is "did we reach a stable
        // service loop", not "is every peripheral healthy".
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
            ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_ota_verify_pending = true;
            s_pending_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGI(TAG, "OTA image PENDING_VERIFY — will confirm after a stable heartbeat");
            // V01: arm the independent deadline NOW, before the rest of init.
            // If init hangs before the service loop (or the loop never reaches
            // a ready+probation state), this fires and restarts -> rollback.
            const esp_timer_create_args_t targs = {
                .callback = ota_confirm_deadline_cb,
                .name = "ota_confirm_deadline",
            };
            if (esp_timer_create(&targs, &s_ota_deadline_timer) == ESP_OK) {
                esp_timer_start_once(s_ota_deadline_timer, OTA_CONFIRM_DEADLINE_US);
            } else {
                ESP_LOGE(TAG, "V01: failed to create OTA confirm deadline timer");
            }
        }
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
    // board_services owns the SHARED I2C bus + CH32 expander (review order 9).
    // Bring it up BEFORE the display so the bus exists independent of the panel
    // — a display failure must not deny touch/sensors/audio the bus.
    if (board_services_init() != ESP_OK) {
        ESP_LOGE(TAG, "board_services_init failed — I2C-dependent subsystems degraded");
    }
    esp_err_t disp_ret = display_init();
    if (disp_ret == ESP_OK) {
        display_set_backlight(100);
        ESP_LOGI(TAG, "display: panel on, backlight 100%%");

        // Touch (2026-10-02): reuses display's I2C bus + CH32V003 expander.
        // Non-fatal, same reasoning as the display init above.
        esp_err_t touch_ret = run_optional("touch", touch_init);
        if (touch_ret == ESP_OK) s_input_ready = true;   // readiness input

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
            s_ui_ready = true;   // V01: usable UI
        } else {
            ESP_LOGE(TAG, "ui: LVGL init failed (0x%x) -- falling back to raw fill/poll", ui_ret);
            if (display_fill(0x07E0) == ESP_OK) {
                s_ui_ready = true;   // V01: raw fallback demonstrably rendered
            }
            if (touch_ret == ESP_OK) {
                xTaskCreate(touch_poll_task, "touch_poll", 3072, NULL, 4, NULL);
            }
        }
    } else {
        ESP_LOGE(TAG, "display: init failed (0x%x) -- continuing without it", disp_ret);
    }

    // Sensors, /metrics and audio are deliberately brought up OUTSIDE the
    // display branch above: a display/expander bug must not silently disable
    // unrelated subsystems -- in particular it must not take out /metrics, the
    // only observability path. Each is gated on its own init result only.

    // Sensors (2026-10-02): QMI8658 IMU + PCF85063A RTC + SHTC3 temp/humidity,
    // all on the SAME shared I2C bus (reuses display_get_i2c_bus()). Non-fatal
    // and independent of the UI; the poll task logs readings to /log for
    // verification.
    run_optional("sensors", sensors_init);

    // Home Assistant (temperature page): starts a dedicated REST poll task
    // that fetches the configured entities from HA_BASE_URL. Non-fatal -- if
    // HA is unreachable the page just shows "--" and the task retries. Must
    // run AFTER WiFi is up (this point in app_main is past the WiFi wait).
    run_optional("ha_client", ha_init);

    // /metrics (Prometheus) is registered ONLY here, after sensors_init()
    // has created its mutex and published the first snapshot. Registering it
    // earlier would let a scraper call sensors_get() concurrently with
    // sensors_init() writing s_started/s_data. Registered regardless of the
    // display result, since /metrics is the only observability path.
    if (server != NULL && sensors_register_metrics(server) == ESP_OK) {
        ESP_LOGI(TAG, "metrics route registered (/metrics)");
    } else {
        ESP_LOGW(TAG, "metrics route registration failed");
    }

    // Audio (2026-10-02): ES8311 codec over I2S + PA enable via the CH32
    // expander. Reuses the shared I2C bus + expander. Non-fatal. On success,
    // play a short startup chime so audio output is audibly verifiable
    // immediately (speaker on the SPK connector).
    if (run_optional("audio", audio_init) == ESP_OK) {
        ESP_LOGI(TAG, "audio: playing init beep");
        audio_post_tone(1000, 150);   // enqueue onto the audio service (non-blocking)
    }

    // R11 + V01: the main service loop, now driven by the host-tested
    // readiness policy (core/readiness.h) instead of inline booleans. Each tick
    // feeds current readiness + heartbeat + probation count to readiness_eval,
    // which returns WAIT / CONFIRM_FULL / CONFIRM_DEGRADED / REJECT. The
    // esp_timer deadline armed above remains the independent net for a hang that
    // never reaches this loop; readiness_eval's own deadline covers a loop that
    // runs but never gets ready.
    uint32_t stable_ticks = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive");
        if (!s_ota_verify_pending) continue;

        stable_ticks++;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        readiness_input_t rin = {
            .maintenance_ready = (server != NULL),
            .ui_ready = s_ui_ready,
            // N07: pass the ACTUAL touch readiness — do NOT OR it with ui_ready.
            // A working LCD/LVGL does not imply a working touch input device; a
            // display-only startup with failed touch must classify DEGRADED, not
            // FULL, so the readiness verdict is an honest usability assessment.
            .input_ready = s_input_ready,
            .last_heartbeat_ms = now_ms,                // this tick IS the heartbeat
            .stable_ticks = stable_ticks,
        };
        readiness_verdict_t v = readiness_eval(&rin, now_ms, s_pending_start_ms);
        if (v == READINESS_WAIT) continue;

        if (v == READINESS_REJECT) {
            ESP_LOGE(TAG, "V01: readiness REJECT (deadline) — restarting so the "
                          "bootloader rolls back the unconfirmed image");
            esp_restart();
        }
        // CONFIRM_FULL or CONFIRM_DEGRADED
        if (v == READINESS_CONFIRM_DEGRADED) {
            ESP_LOGW(TAG, "V01: confirming OTA image in DEGRADED mode — UI not "
                          "ready but HTTP recovery is up (rolling back would lose "
                          "remote recovery for a display fault)");
        }
        s_ota_verify_pending = false;
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            s_ota_confirmed = true;
            if (s_ota_deadline_timer) esp_timer_stop(s_ota_deadline_timer);
            ESP_LOGI(TAG, "OTA image confirmed valid after %" PRIu32
                          " stable ticks%s (rollback cancelled)",
                     stable_ticks, (v == READINESS_CONFIRM_FULL) ? "" : " [degraded]");
        } else {
            ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed");
        }
    }
}
