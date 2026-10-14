#pragma once
#include "lvgl.h"
bool ui_shell_init(lv_indev_t *input);
void ui_shell_status(const char *text);
#include <stdint.h>
#include "ui_page.h"
typedef struct { bool ready,dimmed,pinned,manual_backlight; int page,backlight; uint32_t idle_ms; } ui_status_t;
void ui_shell_snapshot(ui_status_t *out);
const char *ui_shell_page_name(int page);
// UI owner lock required; false before initialization.
bool ui_shell_control(bool page,int value);
