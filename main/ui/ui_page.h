#pragma once
#include "lvgl.h"
#include <time.h>
#include "ha_client.h"
#include "sensors.h"
#include "audio.h"
typedef enum { UI_OVERVIEW, UI_ENERGY_NOW, UI_ENERGY_TODAY, UI_PRICES,
    UI_TEMPERATURES, UI_BATTERIES, UI_MELODIES, UI_GRAPHS, UI_INFO, UI_COSTS, UI_STATUS, UI_WEATHER, UI_WEATHER_DAY, UI_SUN, UI_NETWORK, UI_MARKETS, UI_AIR, UI_SERVICES, UI_NEWS, UI_POLICE, UI_TRANSIT, UI_LIGHTS, UI_HUMIDITY, UI_POWER_HISTORY, UI_PAGE_COUNT } ui_page_id_t;
typedef struct {
    uint64_t now_us;
    time_t wall_time;
    ha_data_t ha;
    sensors_data_t sensors;
    audio_status_t audio;
    bool have_ha, have_sensors;
    int rssi;
    const char *clock_source;
    const char *advisory;
} ui_model_t;
typedef struct {
    const char *title;
    void *(*create)(lv_obj_t *root);
    void (*update)(void *state,const ui_model_t *model);
    void (*enter)(void *state);
    void (*leave)(void *state);
    void (*destroy)(void *state);
} ui_page_desc_t;
void ui_navigate(ui_page_id_t page);
void ui_clean_start(void);
void ui_countdown_toggle(void);
int ui_countdown_seconds(void);
bool ui_countdown_done(void);
void ui_feedback(const char *message);
void *ui_overview_create(lv_obj_t *root);
void ui_overview_update(void *state,const ui_model_t *m);
void *ui_energy_now_create(lv_obj_t *root);
void ui_energy_now_update(void *state,const ui_model_t *m);
void *ui_energy_today_create(lv_obj_t *root);
void ui_energy_today_update(void *state,const ui_model_t *m);
void ui_energy_today_enter(void *state);
void *ui_prices_create(lv_obj_t *root);
void ui_prices_update(void *state,const ui_model_t *m);
void *ui_temperatures_create(lv_obj_t *root);
void ui_temperatures_update(void *state,const ui_model_t *m);
void *ui_batteries_create(lv_obj_t *root);
void ui_batteries_update(void *state,const ui_model_t *m);
void *ui_melodies_create(lv_obj_t *root);
void ui_melodies_update(void *state,const ui_model_t *m);
void *ui_info_create(lv_obj_t *root);
void ui_info_update(void *state,const ui_model_t *m);

void *ui_costs_create(lv_obj_t *root);
void ui_costs_update(void *state,const ui_model_t *m);
void ui_costs_enter(void *state);

void ui_dashboard_update(void *state,const ui_model_t *m);
void *ui_dash_status_create(lv_obj_t *root);
void *ui_dash_weather_create(lv_obj_t *root);
void *ui_dash_weather_day_create(lv_obj_t *root);
void *ui_dash_sun_create(lv_obj_t *root);
void *ui_dash_network_create(lv_obj_t *root);
void *ui_dash_markets_create(lv_obj_t *root);
void *ui_dash_air_create(lv_obj_t *root);
void *ui_dash_services_create(lv_obj_t *root);
void *ui_dash_news_create(lv_obj_t *root);
void *ui_dash_police_create(lv_obj_t *root);
void *ui_dash_transit_create(lv_obj_t *root);
void *ui_dash_lights_create(lv_obj_t *root);
void *ui_dash_humidity_create(lv_obj_t *root);

void *ui_power_history_create(lv_obj_t *root);
void ui_power_history_update(void *state,const ui_model_t *model);
void ui_power_history_record(const ui_model_t *model);
