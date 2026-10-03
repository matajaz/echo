// echo -- LVGL UI layer. See ui.h for the design contract.

#include "ui.h"

#include "display.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"
#include "touch.h"
#include "audio.h"
#include "sensors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdio.h>

static const char *TAG = "ui";

// Panel geometry -- must match display.c (ST7789, 240x320 portrait).
#define UI_H_RES 240
#define UI_V_RES 320

static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_vol_label = NULL;
static lv_obj_t *s_bat_label = NULL;
static lv_obj_t *s_sensor_label = NULL;
static lv_obj_t *s_status_label = NULL;

// Page 2 (temp/humidity) + page 3 (accel/gyro): one chart per quantity.
static lv_obj_t *s_chart_temp = NULL;
static lv_chart_series_t *s_ser_temp = NULL;
static lv_obj_t *s_chart_hum = NULL;
static lv_chart_series_t *s_ser_hum = NULL;
static lv_obj_t *s_chart_acc = NULL;
static lv_chart_series_t *s_ser_acc = NULL;
static lv_obj_t *s_chart_gyro = NULL;
static lv_chart_series_t *s_ser_gyro = NULL;
static lv_obj_t *s_chart_soc = NULL;
static lv_chart_series_t *s_ser_soc = NULL;

// A single long-lived player task consumes play-requests. A song-list
// press sets s_requested_index and (if something is playing) asks the
// current song to stop early -- so pressing a new song interrupts the
// current one and starts the new one. s_req_gen is the handoff counter:
// the player consumes a request by remembering the generation it last saw.
//
// ENQUEUE-ONLY FROM THE UI: audio_play_song() is blocking (it plays a whole
// tune, seconds), so it is called ONLY from this dedicated task -- never
// from the LVGL task. The LVGL callbacks (song_btn_cb) merely record the
// request and give s_req_sem, so the UI never stalls. Do NOT call
// audio_play_song() directly from an lv_event_cb().
static volatile int s_requested_index = -1;
// Monotonically increasing request generation. A plain bool "has_request"
// flag has a lost-wakeup race: song_btn_cb could set it just as player_task
// cleared it (after reading the index but before playing), delaying the new
// press for the whole previous song. The player remembers the last
// generation it consumed and re-checks; the counter only ever grows, so a
// request that lands during playback is never dropped.
static volatile uint32_t s_req_gen = 0;
static SemaphoreHandle_t s_req_sem = NULL;

static void player_task(void *arg) {
    (void)arg;
    uint32_t seen_gen = 0;
    while (1) {
        // Wait for a request.
        xSemaphoreTake(s_req_sem, portMAX_DELAY);
        for (;;) {
            uint32_t gen = s_req_gen;
            if (gen == seen_gen) break;
            int idx = s_requested_index;
            seen_gen = gen;
            // audio_play_song plays the whole tune, and returns early if
            // audio_request_stop() was called mid-song (generation-checked).
            audio_play_song(idx);
            // Loop again if a new request arrived while we were playing
            // (interrupt-and-replace); otherwise fall back to waiting.
        }
    }
}

// A song-list button carries its song index in its user-data. Pressing a
// song interrupts any currently-playing song and starts the new one.
static void song_btn_cb(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const audio_song_t *song = audio_get_song(index);
    ESP_LOGI(TAG, "song button pressed: %s", song ? song->name : "?");
    if (s_req_sem == NULL) {
        ESP_LOGW(TAG, "no player task -- dropping song request");
        return;
    }
    s_requested_index = index;
    s_req_gen++;               // publish the request (race-free handoff)
    audio_request_stop();      // interrupt current song (no-op if idle)
    xSemaphoreGive(s_req_sem); // wake the player task
}

// Refresh the volume label from the codec's current level.
static void update_vol_label(void) {
    if (s_vol_label == NULL) return;
    int v = audio_get_volume();
    if (v < 0) {
        lv_label_set_text(s_vol_label, "vol --");
    } else {
        lv_label_set_text_fmt(s_vol_label, "vol %d%%", v);
    }
}

// Narrow, thread-safe status-update entry point for other modules (WiFi
// status, OTA progress, ...). Reserving this API now keeps all LVGL object
// ownership inside ui.c -- callers must NOT grab lv_* objects directly.
// Safe from ANY task: it takes the LVGL port lock internally and no-ops if
// the UI has not been built yet.
void ui_set_status(const char *text) {
    if (s_status_label == NULL) return;
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_status_label, text ? text : "");
        lvgl_port_unlock();
    }
}

// Volume up/down -- audio_adjust_volume is fast (one I2C write), so it's
// safe to call directly on the LVGL event task (unlike the blocking song).
static void vol_up_cb(lv_event_t *e) {
    (void)e;
    audio_adjust_volume(+10);
    update_vol_label();
}

static void vol_down_cb(lv_event_t *e) {
    (void)e;
    audio_adjust_volume(-10);
    update_vol_label();
}

// Periodic battery readout refresh. Runs on the LVGL task (lv_timer), so
// it may touch LVGL objects directly. Reads the latest snapshot from the
// sensors module (non-blocking) and updates the small top-right label.
static void bat_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_bat_label == NULL) return;
    sensors_data_t d;
    if (sensors_get(&d) && d.bat_ok) {
        // Pick an icon that matches the actual level (was always FULL).
        const char *icon;
        int p = d.bat_soc_pct;
        if (p >= 85) icon = LV_SYMBOL_BATTERY_FULL;
        else if (p >= 60) icon = LV_SYMBOL_BATTERY_3;
        else if (p >= 35) icon = LV_SYMBOL_BATTERY_2;
        else if (p >= 10) icon = LV_SYMBOL_BATTERY_1;
        else icon = LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_bat_label, "%s %d%%", icon, p);
        if (s_chart_soc && s_ser_soc) {
            lv_chart_set_next_value(s_chart_soc, s_ser_soc, (int32_t)p);
        }
    } else {
        lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
    }

    // Sensor readout (same snapshot). NOTE: LVGL's lv_label_set_text_fmt
    // does NOT support %f (its printf is built without float), which
    // printed literal "%f"/"fff". Format floats with the full libc
    // snprintf first, then pass the finished string via %s.
    if (s_sensor_label != NULL) {
        sensors_data_t s;
        if (sensors_get(&s)) {
            char txt[160];
            snprintf(txt, sizeof(txt),
                     "temp %.1f C   hum %.0f%%\n"
                     "accel %.1f %.1f %.1f\n"
                     "gyro  %.0f %.0f %.0f",
                     s.temp_c, s.humidity_rh,
                     s.accel_x, s.accel_y, s.accel_z,
                     s.gyro_x, s.gyro_y, s.gyro_z);
            lv_label_set_text(s_sensor_label, txt);

            // Push points into the page-2 charts (scrolling lines).
            // Charts hold integers, so scale: temp x10 (0.1C res),
            // humidity x1, accel-magnitude x10 (0.1 m/s^2 res).
            if (s_chart_temp && s_ser_temp) {
                lv_chart_set_next_value(s_chart_temp, s_ser_temp, (int32_t)(s.temp_c * 10));
            }
            if (s_chart_hum && s_ser_hum) {
                lv_chart_set_next_value(s_chart_hum, s_ser_hum, (int32_t)(s.humidity_rh));
            }
            if (s_chart_acc && s_ser_acc) {
                float mag = sqrtf(s.accel_x * s.accel_x + s.accel_y * s.accel_y +
                                  s.accel_z * s.accel_z);
                lv_chart_set_next_value(s_chart_acc, s_ser_acc, (int32_t)(mag * 10));
            }
            if (s_chart_gyro && s_ser_gyro) {
                float gmag = sqrtf(s.gyro_x * s.gyro_x + s.gyro_y * s.gyro_y +
                                   s.gyro_z * s.gyro_z);
                lv_chart_set_next_value(s_chart_gyro, s_ser_gyro, (int32_t)gmag);
            }
        }
    }
}

// Create one labelled scrolling line-chart on a parent, return it and set
// Rewrite the chart's X-axis tick labels to relative time. lv_chart has no
// native time axis, so by default it just numbers the ticks (0, 1, 2...).
// 60 points at one sample / 2 s = a 120 s window; with 3 major ticks the
// labels become "-2m" (oldest) .. "-1m" .. "now" (newest).
static void chart_x_time_labels_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == NULL || dsc->text == NULL) return;
    if (!(dsc->part == LV_PART_TICKS && dsc->id == LV_CHART_AXIS_PRIMARY_X)) return;
    // dsc->value is the tick index (0..major_cnt-1); we set 3 majors.
    switch (dsc->value) {
        case 0: lv_snprintf(dsc->text, dsc->text_length, "-2m"); break;
        case 1: lv_snprintf(dsc->text, dsc->text_length, "-1m"); break;
        default: lv_snprintf(dsc->text, dsc->text_length, "now"); break;
    }
}

// *ser to its series. point_cnt points, auto-ranged Y.
typedef struct {
    const char *title;   // axis/legend label
    int y_off;           // vertical offset of the label (chart sits below)
    uint32_t color_hex;  // series line color (lv_color_hex is not constexpr)
    int ymin, ymax;      // Y-axis range; both axis-tick calls derive from it
} chart_cfg_t;

static lv_obj_t *make_chart(lv_obj_t *parent, const chart_cfg_t *cfg,
                            lv_chart_series_t **ser) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xc0c0c0), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(lbl, cfg->title);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 6, cfg->y_off);

    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_size(chart, 200, 95);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 8, cfg->y_off + 20);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 60); // ~2 min at 2 s/sample
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, cfg->ymin, cfg->ymax);
    // Axis tick marks + numeric labels. Y: 3 major ticks (min/mid/max) on
    // the left; X: time ticks along the bottom (older <- -> newer).
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 3, 1, true, 40);
    // X axis: 3 major ticks; a draw callback rewrites their labels to
    // relative time ("-2m".."now") since lv_chart has no native time axis.
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 4, 2, 3, 1, true, 24);
    lv_obj_add_event_cb(chart, chart_x_time_labels_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    // Don't let the chart capture the swipe: charts are clickable+scrollable
    // by default and "eat" the drag, so the tileview never sees the gesture
    // (symptom: stuck on the graph page, can't swipe back). Make the chart
    // transparent to input and bubble gestures up to the tileview.
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    *ser = lv_chart_add_series(chart, lv_color_hex(cfg->color_hex), LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

// Build the UI: a 2-tile tileview (swipe left/right). Tile 0 = controls +
// live readout, tile 1 = scrolling sensor graphs. Caller holds the LVGL
// port lock.
static void build_test_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(tv, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_t *t0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *t1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t2 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_t *t3 = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);
    // Tiles must not scroll internally or they swallow the swipe gesture.
    lv_obj_clear_flag(t0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(t3, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Tile 0: controls + readout ----
    lv_obj_t *title = lv_label_create(t0);
    lv_label_set_text(title, "echo");
    lv_obj_set_style_text_color(title, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_bat_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x90c0ff), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
    lv_obj_align(s_bat_label, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_timer_create(bat_timer_cb, 2000, NULL);
    bat_timer_cb(NULL);

    // Reserved status line: other modules report here (via ui_set_status())
    // instead of touching LVGL objects themselves.
    s_status_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 30);

    s_sensor_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_sensor_label, lv_color_hex(0xd0d0d0), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_sensor_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_sensor_label, "sensors: ...");
    lv_obj_align(s_sensor_label, LV_ALIGN_CENTER, 0, -60);
    lv_obj_set_style_text_align(s_sensor_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Scrollable song list -- one button per registered (public-domain)
    // song. Each button carries its song index as user-data; the callback
    // spawns the worker task to play it.
    lv_obj_t *list = lv_list_create(t0);
    lv_obj_set_size(list, 220, 120);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x181c22), LV_PART_MAIN);
    // Let vertical scroll work but still bubble horizontal swipes to the
    // tileview (so page-swipe keeps working from the list).
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    for (int i = 0; i < audio_song_count(); i++) {
        const audio_song_t *song = audio_get_song(i);
        lv_obj_t *b = lv_list_add_btn(list, LV_SYMBOL_AUDIO, song->name);
        lv_obj_add_event_cb(b, song_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Volume controls below the list.
    lv_obj_t *vol_dn = lv_btn_create(t0);
    lv_obj_set_size(vol_dn, 50, 34);
    lv_obj_align(vol_dn, LV_ALIGN_CENTER, -70, 88);
    lv_obj_add_event_cb(vol_dn, vol_down_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vd = lv_label_create(vol_dn);
    lv_label_set_text(vd, LV_SYMBOL_DOWN);
    lv_obj_center(vd);

    s_vol_label = lv_label_create(t0);
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(0xc0c0c0), LV_PART_MAIN);
    lv_obj_align(s_vol_label, LV_ALIGN_CENTER, 0, 88);
    update_vol_label();

    lv_obj_t *vol_up = lv_btn_create(t0);
    lv_obj_set_size(vol_up, 50, 34);
    lv_obj_align(vol_up, LV_ALIGN_CENTER, 70, 88);
    lv_obj_add_event_cb(vol_up, vol_up_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *vu = lv_label_create(vol_up);
    lv_label_set_text(vu, LV_SYMBOL_UP);
    lv_obj_center(vu);

    lv_obj_t *hint = lv_label_create(t0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x606060), LV_PART_MAIN);
    lv_label_set_text(hint, "swipe left for graphs " LV_SYMBOL_RIGHT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    // ---- Tile 1: temperature + humidity ----
    lv_obj_t *gt1 = lv_label_create(t1);
    lv_obj_set_style_text_color(gt1, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_label_set_text(gt1, LV_SYMBOL_LEFT " climate " LV_SYMBOL_RIGHT);
    lv_obj_align(gt1, LV_ALIGN_TOP_MID, 0, 4);
    static const chart_cfg_t temp_cfg = {"temp (0.1 C)", 30, 0xff8040, 150, 400};
    static const chart_cfg_t hum_cfg = {"humidity %", 160, 0x40c0ff, 0, 100};
    static const chart_cfg_t acc_cfg = {"accel |a| (0.1 m/s2)", 30, 0x40ff80, 0, 200};
    static const chart_cfg_t gyro_cfg = {"gyro |w| (dps)", 160, 0xffd040, 0, 500};
    static const chart_cfg_t soc_cfg = {"SOC %", 30, 0x90c0ff, 0, 100};
    s_chart_temp = make_chart(t1, &temp_cfg, &s_ser_temp);
    s_chart_hum = make_chart(t1, &hum_cfg, &s_ser_hum);

    // ---- Tile 2: acceleration + gyro ----
    lv_obj_t *gt2 = lv_label_create(t2);
    lv_obj_set_style_text_color(gt2, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_label_set_text(gt2, LV_SYMBOL_LEFT " motion");
    lv_obj_align(gt2, LV_ALIGN_TOP_MID, 0, 4);
    s_chart_acc = make_chart(t2, &acc_cfg, &s_ser_acc);
    s_chart_gyro = make_chart(t2, &gyro_cfg, &s_ser_gyro);

    // ---- Tile 3: battery state-of-charge ----
    lv_obj_t *gt3 = lv_label_create(t3);
    lv_obj_set_style_text_color(gt3, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_label_set_text(gt3, LV_SYMBOL_LEFT " battery");
    lv_obj_align(gt3, LV_ALIGN_TOP_MID, 0, 4);
    s_chart_soc = make_chart(t3, &soc_cfg, &s_ser_soc);
}

// NOTE: ui_init() returns only esp_err_t and keeps all UI state file-static
// in this file. If a second screen / UI instance is ever introduced, change
// this to `ui_init(ui_t **out)` so the caller owns the state rather than
// adding more file-static globals here.
esp_err_t ui_init(void) {
    esp_lcd_panel_handle_t panel = display_get_panel();
    esp_lcd_panel_io_handle_t panel_io = display_get_panel_io();
    esp_lcd_touch_handle_t touch = touch_get_handle();

    if (panel == NULL || panel_io == NULL) {
        ESP_LOGE(TAG, "display not initialized -- call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // 1. Init the LVGL port (creates its LVGL task + tick timer).
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    esp_err_t ret = lvgl_port_init(&port_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: 0x%x", ret);
        return ret;
    }

    // 2. Attach the existing ST7789 panel as an LVGL display.
    //    buffer_size = a partial-screen buffer (1/10 of the frame) in
    //    internal RAM, DMA-capable. NO swap_bytes: display.c already set
    //    the ST7789 data_endian to LITTLE to match the little-endian
    //    uint16_t pixel stores, so the bytes are already correct -- adding
    //    a swap here would re-break the green/red channel order.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .buffer_size = UI_H_RES * UI_V_RES / 10,
        .double_buffer = true,
        .hres = UI_H_RES,
        .vres = UI_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    // 3. Attach the CST3530 touch as an LVGL input device (if present --
    //    a missing touch must not block the display coming up).
    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "lvgl_port_add_touch failed -- UI will be display-only");
        }
    } else {
        ESP_LOGW(TAG, "no touch handle -- UI will be display-only");
    }

    // Create the play-request semaphore + start the persistent player task
    // (handles interrupt-and-replace: a new song press stops the current
    // song and starts the new one). Prio 20 -- high, so I2S DMA never
    // starves on this single-core chip.
    s_req_sem = xSemaphoreCreateBinary();
    if (s_req_sem != NULL) {
        if (xTaskCreate(player_task, "song_player", 4096, NULL, 20, NULL) != pdPASS) {
            // No consumer for requests: drop the semaphore so song_btn_cb
            // no-ops instead of posting to a queue nobody drains (and so we
            // never deref a stale handle on a later press).
            ESP_LOGE(TAG, "failed to create song player task");
            vSemaphoreDelete(s_req_sem);
            s_req_sem = NULL;
        }
    } else {
        ESP_LOGE(TAG, "failed to create song request semaphore");
    }

    // 4. Build the initial UI under the LVGL lock.
    if (lvgl_port_lock(0)) {
        build_test_ui();
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "could not take LVGL lock to build UI");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL UI init success (%dx%d)", UI_H_RES, UI_V_RES);
    return ESP_OK;
}
