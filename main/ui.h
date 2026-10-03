// echo -- LVGL UI layer (esp_lvgl_port).
//
// Attaches LVGL to the ST7789 panel + CST3530 touch that display.c and
// touch.c already brought up (reusing their handles via the accessors --
// no SPI/panel/touch re-init here), then draws a minimal test UI to prove
// the LVGL rendering + touch-input path end to end.
//
// Design notes:
//   - Build order: display_init() -> touch_init() -> ui_init(). ui_init
//     requires BOTH to have succeeded (it reads display_get_panel() etc).
//   - esp_lvgl_port runs its own LVGL task + tick timer; all LVGL object
//     calls from OUTSIDE that task MUST be wrapped in lvgl_port_lock() /
//     lvgl_port_unlock(). ui_init does its initial widget creation under
//     that lock.
//   - LVGL 8.x API (pinned in idf_component.yml) -- lv_scr_act(),
//     lv_label_create(), lv_btn_create(), LV_EVENT_CLICKED, etc.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize LVGL, attach the existing display + touch, and draw the
// initial test UI. Must be called AFTER display_init() and touch_init()
// have both succeeded. Returns ESP_OK on success, or an error (in which
// case the caller should continue without a UI -- a UI failure must never
// block WiFi/OTA/log, same non-fatal policy as display/touch).
//
// Init-once: ui_init() must be called exactly ONCE, from a single task,
// after display_init() + touch_init() have both succeeded. It is NOT
// idempotent and is NOT internally serialized against a second caller;
// concurrent or repeated calls are undefined.
//
// Locking: ui_init() takes lvgl_port_lock()/lvgl_port_unlock() internally
// for its own widget creation. It exposes NO lock API to callers; any
// subsequent LVGL object calls from outside the esp_lvgl_port task are the
// caller's responsibility to wrap in lvgl_port_lock()/lvgl_port_unlock().
esp_err_t ui_init(void);

#ifdef __cplusplus
}
#endif
