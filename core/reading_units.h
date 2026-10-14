#pragma once
#include <stdbool.h>
#include <math.h>
#include <string.h>
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
