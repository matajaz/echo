// Shared, bounded dashboard projection. Kept identical in Echo and Lumen.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DASH_STATUS, DASH_WEATHER, DASH_WEATHER_DAY, DASH_SUN, DASH_NETWORK,
 DASH_MARKETS, DASH_AIR, DASH_SERVICES, DASH_NEWS, DASH_POLICE, DASH_TRANSIT,
 DASH_LIGHTS, DASH_HUMIDITY, DASH_TEMPERATURES, DASH_BATTERIES, DASH_ELECTRICITY, DASH_WAN, DASH_COUNT };
#define DASH_TEXT_CAP 1536
typedef struct { char text[DASH_TEXT_CAP]; uint32_t observed_at; uint64_t received_us; uint16_t available_lights; bool valid; } dashboard_section_t;
extern const char *const dashboard_names[DASH_COUNT];
// source: 0=collector extra.json, 1=HA panel state, 2=HA ambient state.
// Invalid documents leave the previous snapshot unchanged.
bool dashboard_parse(const char *json,int source,uint64_t received_us,dashboard_section_t sections[DASH_COUNT]);
int dashboard_moon_percent(int64_t epoch,bool *waxing);
bool dashboard_fresh(const dashboard_section_t *section,uint64_t now_us,int64_t wall_time);
#ifdef __cplusplus
}
#endif
