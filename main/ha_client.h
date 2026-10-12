// HA task owns network work; consumers read short mutex-protected snapshots.
#pragma once
#include "esp_err.h"
#include "ha_model.h"
#include "energy_model.h"
typedef struct {
    ha_sensor_t sensors[HA_MAX_SENSORS], batteries[HA_MAX_BATT], power[HA_MAX_POWER];
    int count, batt_count, power_count;
    bool started, wifi_up, connected;
    int last_http_code;
    uint64_t last_response_us;
} ha_data_t;
esp_err_t ha_init(void);
bool ha_get(ha_data_t *out);
bool ha_prices_get(energy_prices_t *out);
bool ha_history_get(energy_history_t *out);
// 0=today, -1=yesterday; applied asynchronously by the HA owner.
void ha_history_request(int day_offset);
const ha_sensor_t *ha_find_role(const ha_data_t *data, ha_role_t role);
const char *ha_role_entity(ha_role_t role);
int ha_sensor_count(void);
int ha_batt_count(void);
bool ha_sensor_meta(int index, ha_sensor_t *out);
bool ha_poll_pause(unsigned timeout_ms);
void ha_poll_resume(void);
