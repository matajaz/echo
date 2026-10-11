// echo -- Home Assistant REST client (temperature sensors).
//
// Polls a small, configured list of HA entities via
//   GET {HA_BASE_URL}/api/states/<entity_id>
// and exposes the parsed "state" values as degree-C temperatures. The
// design mirrors sensors.c: a dedicated poll task owns the data, and
// callers read a mutex-guarded snapshot via ha_get() -- no caller ever
// touches the network or the shared struct directly.
//
// Connection details (HA_BASE_URL, HA_TOKEN) live in secrets/secrets.h,
// the same file that (in the sibling Vegena/lumen projects) carries the
// LAN URL + long-lived access token.
//
// Non-fatal by contract: a failed HA fetch must never block the UI, WiFi
// or OTA. A missing/unreachable HA simply leaves the last values in place
// with ok=0 (and the UI shows "--").
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of temperature entities polled/displayed.
#define HA_MAX_SENSORS 8
// Maximum number of sensor-battery entities (the tile-4 sensors' battery
// counterparts, device_class=battery). Displayed on the page AFTER the temps.
#define HA_MAX_BATT 8
// Bounded entity id / friendly-name length (kept short: they live in a
// snapshot struct copied under a lock and shown on a small panel).
#define HA_ENTITY_LEN 64
#define HA_NAME_LEN 24

// One HA temperature entity's last known reading.
typedef struct {
    char entity_id[HA_ENTITY_LEN]; // e.g. "sensor.outdoor_temperature"
    char name[HA_NAME_LEN];        // short display label
    float temp_c;                  // degrees Celsius; NAN when invalid
    bool ok;                       // true when the last fetch was a valid number
} ha_sensor_t;

// Thread-safe snapshot of all configured entities.
typedef struct {
    ha_sensor_t sensors[HA_MAX_SENSORS];
    int count;              // number of configured entities (<= HA_MAX_SENSORS)
    // Sensor-battery entities for the SAME physical sensors as sensors[]
    // (separate HA objects). temp_c carries a PERCENT here, not a temperature.
    ha_sensor_t batteries[HA_MAX_BATT];
    int batt_count;         // number of configured battery entities
    bool started;           // poll task running
    bool wifi_up;          // last poll saw a live link (best-effort)
    int last_http_code;     // last HTTP status (or negative transport err)
} ha_data_t;

// Start the HA poll task (idempotent). Reads the entity list compiled into
// ha_client.c and HA_BASE_URL/HA_TOKEN from secrets.h. Safe to call once
// from app_main after WiFi is up; returns ESP_OK even if HA is unreachable
// (the task retries).
esp_err_t ha_init(void);

// Copy the latest snapshot into *out. Returns false if not started or out
// is NULL. Non-blocking (mutex held only for the struct copy).
bool ha_get(ha_data_t *out);

// Number of configured entities (valid after ha_init; 0 before).
int ha_sensor_count(void);

// Copy the i-th configured entity's identity (id + name) into *out without
// waiting for a network fetch. Returns false on out-of-range / NULL.
bool ha_sensor_meta(int index, ha_sensor_t *out);

// --- Polling quiescence for OTA (confirmed-idle handshake) ----------------
// ha_poll_pause() asks the poll task to STOP issuing HTTP and BLOCKS until it
// has confirmed it is parked with no request in flight (or until timeout_ms
// elapses). This is a real handshake, NOT a bare flag: it guarantees no HA GET
// contends with the OTA transfer. Returns true if the poll task acknowledged
// idle within the timeout, false if it timed out (caller may still proceed,
// but should log it). ha_poll_resume() releases the pause. Safe to call even
// if the poll task never started (returns true immediately). Idempotent.
bool ha_poll_pause(unsigned timeout_ms);
void ha_poll_resume(void);

#ifdef __cplusplus
}
#endif
