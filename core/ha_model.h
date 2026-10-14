#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include "reading_units.h"
#include <string.h>

#define HA_MAX_SENSORS 9
#define HA_MAX_BATT 8
#define HA_MAX_POWER 8
#define HA_ENTITY_LEN 80
#define HA_NAME_LEN 24
typedef enum {
    HA_ROLE_NONE, HA_ROLE_OUTDOOR, HA_ROLE_INDOOR,
    HA_ROLE_SOLAR_POWER, HA_ROLE_GRID_POWER, HA_ROLE_GRID_EXPORT_POWER,
    HA_ROLE_GRID_EXPORT_TODAY, HA_ROLE_GRID_IMPORT_TODAY, HA_ROLE_PRICE,
    HA_ROLE_SOLAR_TODAY, HA_ROLE_GRID_IMPORT_COST_TODAY, HA_ROLE_GRID_EXPORT_INCOME_TODAY
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
