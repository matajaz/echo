#pragma once
#include "esp_err.h"
// Initialize once from startup after display_init. Touch is optional.
// Owns LVGL hardware attachment; UI pages are created under the port lock.
esp_err_t ui_init(void);
// Bounded status notification from any task; drops updates when UI is busy.
void ui_set_status(const char *text);
