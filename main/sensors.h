// echo -- onboard sensors on the shared I2C bus (SDA=GPIO0, SCL=GPIO1).
//
// Three sensors, all reusing the ONE I2C master bus display.c already
// created (display_get_i2c_bus()) -- no second bus:
//   - QMI8658  6-axis IMU   (I2C 0x6B high / 0x6A low)  waveshare/qmi8658
//   - PCF85063A RTC          (I2C 0x51)                 waveshare/pcf85063a
//   - SHTC3   temp/humidity  (I2C 0x70)                 pedrominatel/shtc3
//
// All API signatures verified against the real component headers (github
// waveshareteam/Waveshare-ESP32-components + pedrominatel/esp-components),
// not guessed.
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Latest sensor readings, updated by the internal poll task. The *_ok
// flags say whether that sensor initialized and is producing readings --
// a missing/failed sensor leaves its values at 0 and *_ok=false rather
// than blocking the sensors.
//
// THREAD SAFETY: this struct is never handed out or mutated field-by-field.
// The poll task fills a private staging struct and then PUBLISHES the whole
// struct in one shot under a critical section; sensors_get() takes the same
// critical section and COPIES the entire struct into *out before releasing it.
// A caller therefore always observes a single coherent snapshot -- it can
// never see, for example, an updated temp_c alongside a stale humidity_rh
// or a stale rtc_ok. No external locking is required on the caller's side;
// call sensors_get() directly from any task, callback, or HTTP handler.
typedef struct {
    bool imu_ok;
    float accel_x, accel_y, accel_z; // m/s^2
    float gyro_x, gyro_y, gyro_z;    // deg/s
    float imu_temp_c;                // IMU die temperature

    bool th_ok;
    float temp_c;      // SHTC3 ambient temperature
    float humidity_rh; // SHTC3 relative humidity %

    bool rtc_ok;
    uint16_t year;
    uint8_t month, day, hour, min, sec;

    // Battery, read via the CH32 expander's ADC (0..1023 raw). soc_pct is
    // an ESTIMATE (see sensors.c -- the exact divider ratio isn't vendor-
    // documented, so the voltage scaling needs calibration). bat_ok is
    // true once a reading has been taken.
    bool bat_ok;
    uint16_t bat_adc;  // raw 0..1023
    int bat_soc_pct;   // estimated 0..100

    // FRESHNESS/QUALITY (R10, 2026-10-10). The *_ok flags above mean the
    // sensor was DETECTED at init; they do NOT mean the current value is
    // fresh. These are the monotonic timestamp (ms since boot, from
    // esp_timer) of the LAST SUCCESSFUL read of each group. 0 = never read
    // successfully. A consumer treats a value as STALE when
    // (now_ms - *_last_ms) exceeds a threshold, and should render "--"
    // rather than a stale/zero value. now_ms is published alongside so a
    // single coherent snapshot can compute age without racing the clock.
    uint32_t now_ms;        // esp_timer ms at publish time
    uint32_t imu_last_ms;   // last good accel/gyro read
    uint32_t th_last_ms;    // last good temp/humidity read
    uint32_t rtc_last_ms;   // last good RTC read
    uint32_t bat_last_ms;   // last good battery read
} sensors_data_t;

// Freshness threshold: a group whose last good read is older than this is
// STALE. The poll runs every ~5 s, so 15 s = ~3 missed reads.
#define SENSORS_STALE_MS 15000

// V05a: freshness MUST be measured against the READER's current clock, not the
// producer's publish-time now_ms. If the poll worker stops/parks after a good
// publish, the stored now_ms freezes and (now_ms - last_ms) stays small forever
// -> data looks fresh indefinitely. Callers pass esp_timer_get_time()/1000 read
// at reader time. (Unsigned subtraction handles the ~49-day ms wrap.)
static inline bool sensors_group_fresh_at(uint32_t last_ms, uint32_t reader_now_ms) {
    return last_ms != 0 && (uint32_t)(reader_now_ms - last_ms) <= SENSORS_STALE_MS;
}

// DEPRECATED (V05a): compares against the producer's frozen publish-time, so a
// stopped worker reads fresh forever. Prefer sensors_group_fresh_at() with a
// reader-time clock. Retained only so existing callers compile.
static inline bool sensors_group_fresh(uint32_t last_ms, uint32_t now_ms) {
    return last_ms != 0 && (uint32_t)(now_ms - last_ms) <= SENSORS_STALE_MS;
}

// Bring up all three sensors on the shared I2C bus and start the poll
// task. Requires display_init() to have succeeded (reuses its bus).
//
// IDEMPOTENT: calling sensors_init() again after a successful call is a
// no-op that returns ESP_OK -- it does NOT spawn a second poll task, does
// NOT re-initialize the bus, and does NOT leak resources. Re-entry while an
// earlier call is still in progress (concurrent call from another task) is
// NOT supported and returns ESP_ERR_INVALID_STATE; serialize init on one task.
//
// Returns ESP_OK if the subsystem (already) started, even if some individual
// sensors failed -- check the per-sensor *_ok flags via sensors_get().
esp_err_t sensors_init(void);

// Copy the latest readings into *out. Returns false if sensors_init()
// hasn't run yet.
//
// Thread safety: the entire struct is copied out under the same critical
// section the poll task uses to publish it, so *out is always a single
// coherent snapshot (never a mix of old and new fields) and needs no locking
// on the caller's side.
bool sensors_get(sensors_data_t *out);

// Pause/resume the internal poll task (stops all I2C sensor reads while
// paused). Used to quiesce the single core during audio playback.
//
// SYNCHRONIZATION: this only stores a flag; it does NOT block and provides
// no completion acknowledgement. The poll task observes the flag on its
// next tick, after any I2C transaction already in flight has finished. The
// pause is therefore ASYNCHRONOUS: on return, the shared I2C bus may still
// be busy with one final sensor read, so it is NOT safe to start audio
// traffic on the same bus immediately. Callers that need a hard quiesce
// must wait at least one poll interval (or wait until the bus is idle)
// after calling this before using the shared bus.
void sensors_set_paused(bool paused);

// Register a GET /metrics endpoint (Prometheus text format) on the given
// HTTP server, exposing all sensor + battery values for scraping.
//
// `server` MUST be a live handle obtained from httpd_start() that the caller
// guarantees will outlive this call; passing NULL or a handle being torn down
// by httpd_stop() is undefined and the HTTP registration can fail silently.
// Returns ESP_OK on success, or a non-OK esp_err_t (e.g. ESP_ERR_INVALID_ARG
// for a NULL server, ESP_ERR_HTTPD_HANDLERS_FULL, or ESP_ERR_NO_MEM) if the
// handler could not be registered. Callers MUST check the return value and
// treat non-OK as "metrics endpoint unavailable": log it and continue -- the
// rest of the sensor subsystem keeps working, the /metrics route is simply
// absent. Never assume the route exists just because this was called.
esp_err_t sensors_register_metrics(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
