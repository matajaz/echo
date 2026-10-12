#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

#define HA_MAX_SENSORS 9
#define HA_MAX_BATT 8
#define HA_MAX_POWER 6
#define HA_ENTITY_LEN 80
#define HA_NAME_LEN 24
typedef enum {
    HA_ROLE_NONE, HA_ROLE_OUTDOOR, HA_ROLE_INDOOR,
    HA_ROLE_SOLAR_POWER, HA_ROLE_GRID_POWER, HA_ROLE_GRID_EXPORT_POWER,
    HA_ROLE_GRID_EXPORT_TODAY, HA_ROLE_GRID_IMPORT_TODAY, HA_ROLE_PRICE,
    HA_ROLE_SOLAR_TODAY
} ha_role_t;
typedef enum { HA_LOADING, HA_VALID, HA_OFFLINE, HA_HTTP_ERROR, HA_OVERFLOW,
               HA_INVALID, HA_UNIT_ERROR } ha_result_t;
typedef struct {
    char entity_id[HA_ENTITY_LEN], name[HA_NAME_LEN], unit[16];
    ha_role_t role;
    float value;
    bool ok;
    uint64_t received_us;
    uint64_t checked_us;
    uint32_t stale_ms;
    int http_code;
    ha_result_t result;
} ha_sensor_t;
static inline bool ha_fresh(const ha_sensor_t *s, uint64_t now) {
    return s && s->ok && s->received_us && now >= s->received_us &&
           now - s->received_us <= (uint64_t)s->stale_ms * 1000u;
}
static inline const char *ha_result_name(ha_result_t r) {
    switch (r) {
        case HA_LOADING: return "Loading";
        case HA_VALID: return "Live";
        case HA_OFFLINE: return "Offline";
        case HA_HTTP_ERROR: return "HA error";
        case HA_OVERFLOW: return "Too much data";
        case HA_UNIT_ERROR: return "Wrong unit";
        default: return "Unavailable";
    }
}
// Return normalized Celsius / percent / W / kWh / SEK per kWh. Unknown units
// are rejected, never relabelled into a different physical quantity.
static inline bool ha_normalize(float input, const char *unit, const char *want, float *out) {
    if (!unit || !want || !out || !isfinite(input)) return false;
    float v = input;
    if (!strcmp(want, "C")) {
        if (!strcmp(unit, "°F") || !strcmp(unit, "F")) v = (v - 32) / 1.8f;
        else if (!strcmp(unit, "K")) v -= 273.15f;
        else if (strcmp(unit, "°C") && strcmp(unit, "C")) return false;
    } else if (!strcmp(want, "W")) {
        if (!strcmp(unit, "kW")) v *= 1000;
        else if (strcmp(unit, "W")) return false;
    } else if (!strcmp(want, "kWh")) {
        if (!strcmp(unit, "Wh")) v /= 1000;
        else if (strcmp(unit, "kWh")) return false;
    } else if (!strcmp(want, "SEK/kWh")) {
        if (!strcmp(unit, "SEK/MWh")) v /= 1000;
        else if (!strcmp(unit, "öre/kWh")) v /= 100;
        else if (strcmp(unit, "SEK/kWh") && strcmp(unit, "kr/kWh")) return false;
    } else if (strcmp(unit, want)) return false;
    if (!isfinite(v)) return false;
    *out = v; return true;
}
