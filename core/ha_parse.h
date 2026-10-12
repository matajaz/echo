#pragma once
#include "ha_model.h"
#include "energy_model.h"
ha_result_t ha_parse_state(const char *json, const char *unit, float *value,
                          energy_prices_t *prices);
bool ha_parse_statistics(const char *json, const char *import_id, const char *export_id,
                         energy_history_t *history);
