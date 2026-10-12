#pragma once
#include "energy_model.h"
bool ha_energy_fetch(int day_offset, energy_history_t *out, bool (*cancel)(void));
