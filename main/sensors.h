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
} sensors_data_t;

// Bring up all three sensors on the shared I2C bus and start the poll
// task. Requires display_init() to have succeeded (reuses its bus).
// Returns ESP_OK if the subsystem started (even if some individual
// sensors failed -- check the per-sensor *_ok flags via sensors_get()).
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
void sensors_set_paused(bool paused);

// Register a GET /metrics endpoint (Prometheus text format) on the given
// HTTP server, exposing all sensor + battery values for scraping.
esp_err_t sensors_register_metrics(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
