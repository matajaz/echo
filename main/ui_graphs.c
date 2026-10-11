// echo -- consolidated single-graph page (see ui_graphs.h).
//
// One large graph shows ONE metric, chosen from a dropdown. The metric table
// below drives everything (title, colour, Y range, units, data source), so
// adding a metric is a one-line table entry.

#include "ui_graphs.h"

#include "sensors.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_graphs";

#define N_POINTS     60            // 60 x 2 s = 120 s = "last 2 min"
#define SAMPLE_MS    2000
#define VAL_X10      10            // chart stores value * 10 (keeps C/F/K/.. as ints)
#define TITLE_Y      4
#define DD_Y         28
#define CHART_Y      68
#define CARD_H       66
#define CHART_H      (320 - CHART_Y - CARD_H - 4)   // 182 px
#define CHART_W      220
#define CARD_W       232

// ---- unit tables (affine: display = base*mul + add) --------------------
static const char *const TEMP_U[] = {"C", "F", "K"};
static const float TEMP_M[] = {1.0f, 1.8f, 1.0f};
static const float TEMP_A[] = {0.0f, 32.0f, 273.15f};
static const char *const ONE_U[] = {"%"};
static const char *const SPD_U[] = {"m/s2"};
static const char *const GYR_U[] = {"dps"};
static const float ONE_M[] = {1.0f};
static const float ONE_A[] = {0.0f};

static bool base_temp(float *v) {
    sensors_data_t d;
    if (!sensors_get(&d)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!sensors_group_fresh_at(d.th_last_ms, now)) return false;
    if (v) *v = d.temp_c;
    return true;
}
static bool base_hum(float *v) {
    sensors_data_t d;
    if (!sensors_get(&d)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!sensors_group_fresh_at(d.th_last_ms, now)) return false;
    if (v) *v = d.humidity_rh;
    return true;
}
static bool base_acc(float *v) {
    sensors_data_t d;
    if (!sensors_get(&d)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!sensors_group_fresh_at(d.imu_last_ms, now)) return false;
    if (v) *v = sqrtf(d.accel_x * d.accel_x + d.accel_y * d.accel_y + d.accel_z * d.accel_z);
    return true;
}
static bool base_gyr(float *v) {
    sensors_data_t d;
    if (!sensors_get(&d)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!sensors_group_fresh_at(d.imu_last_ms, now)) return false;
    if (v) *v = sqrtf(d.gyro_x * d.gyro_x + d.gyro_y * d.gyro_y + d.gyro_z * d.gyro_z);
    return true;
}
static bool base_soc(float *v) {
    sensors_data_t d;
    if (!sensors_get(&d)) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!sensors_group_fresh_at(d.bat_last_ms, now)) return false;
    if (v) *v = (float)d.bat_soc_pct;
    return true;
}

typedef struct {
    const char *name;          // dropdown entry + chart title
    const char *const *units;  // unit labels
    const float *mul;          // affine mul per unit
    const float *add;          // affine add per unit
    int n_units;
    float base_min, base_max;  // Y range in BASE units (unit 0 = identity)
    uint32_t color_hex;
    bool (*get)(float *base_out);
} metric_t;

static const metric_t METRICS[] = {
    {"Temperature", TEMP_U, TEMP_M, TEMP_A, 3, 15.0f, 40.0f, 0xff8040, base_temp},
    {"Humidity",    ONE_U,  ONE_M,  ONE_A,  1,  0.0f, 100.0f, 0x40c0ff, base_hum},
    {"Accel |a|",   SPD_U,  ONE_M,  ONE_A,  1,  0.0f, 20.0f,  0x40ff80, base_acc},
    {"Gyro |w|",    GYR_U,  ONE_M,  ONE_A,  1,  0.0f, 500.0f, 0xffd040, base_gyr},
    {"Battery",     ONE_U,  ONE_M,  ONE_A,  1,  0.0f, 100.0f, 0x90c0ff, base_soc},
};
#define N_METRICS ((int)(sizeof(METRICS) / sizeof(METRICS[0])))

// ---- state -------------------------------------------------------------
static lv_obj_t  *s_dd = NULL;      // dropdown (metric picker)
static lv_obj_t  *s_chart = NULL;
static lv_chart_series_t *s_ser = NULL;
static lv_obj_t  *s_title = NULL;
static lv_obj_t  *s_card = NULL;    // value card (tap = cycle unit)
static lv_obj_t  *s_val = NULL;     // big value + unit
static lv_obj_t  *s_stats = NULL;   // min/max/avg line (small font)
static lv_timer_t *s_timer = NULL;

static int s_idx = 0;               // selected metric index
static int s_unit = 0;              // selected unit index for that metric

typedef struct { bool any; float lo, hi, last; double sum; int n; } sampler_t;
static sampler_t s_samp;

static inline float disp_of(const metric_t *m, int u, float base) {
    return base * m->mul[u] + m->add[u];
}
static void samp_reset(void) { memset(&s_samp, 0, sizeof(s_samp)); }
static void samp_add(float base) {
    if (!s_samp.any) { s_samp.lo = s_samp.hi = s_samp.last = base; s_samp.any = true; }
    else {
        if (base < s_samp.lo) s_samp.lo = base;
        if (base > s_samp.hi) s_samp.hi = base;
        s_samp.last = base;
    }
    s_samp.sum += base;
    s_samp.n++;
}

// Rewrite the X-axis ticks to relative time (mirrors the old chart page).
static void chart_x_labels_cb(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == NULL || dsc->text == NULL) return;
    if (!(dsc->part == LV_PART_TICKS && dsc->id == LV_CHART_AXIS_PRIMARY_X)) return;
    switch (dsc->value) {
        case 0:  lv_snprintf(dsc->text, dsc->text_length, "-2m"); break;
        case 1:  lv_snprintf(dsc->text, dsc->text_length, "-1m"); break;
        default: lv_snprintf(dsc->text, dsc->text_length, "now"); break;
    }
}

static void apply_range(void) {
    const metric_t *m = &METRICS[s_idx];
    int rmin = (int)lroundf(disp_of(m, s_unit, m->base_min) * VAL_X10);
    int rmax = (int)lroundf(disp_of(m, s_unit, m->base_max) * VAL_X10);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, rmin, rmax);
    lv_chart_set_all_value(s_chart, s_ser, LV_CHART_POINT_NONE);
    lv_chart_refresh(s_chart);
}

static void push_point(void) {
    const metric_t *m = &METRICS[s_idx];
    float base;
    int32_t pt = LV_CHART_POINT_NONE;
    if (m->get(&base)) {
        samp_add(base);
        pt = (int32_t)lroundf(disp_of(m, s_unit, base) * VAL_X10);
    }
    lv_chart_set_next_value(s_chart, s_ser, pt);
    lv_chart_refresh(s_chart);
}

static void update_readout(void) {
    const metric_t *m = &METRICS[s_idx];
    if (!s_samp.any) {
        lv_label_set_text(s_val, "--");
        lv_label_set_text(s_stats, "waiting for data...");
        return;
    }
    char v[32], st[56];
    const char *u = m->units[s_unit];
    snprintf(v, sizeof(v), "%.1f %s", disp_of(m, s_unit, s_samp.last), u);
    double avg = s_samp.sum / (s_samp.n ? s_samp.n : 1);
    snprintf(st, sizeof(st), "min %.1f  max %.1f  avg %.1f",
             disp_of(m, s_unit, s_samp.lo), disp_of(m, s_unit, s_samp.hi),
             disp_of(m, s_unit, (float)avg));
    lv_label_set_text(s_val, v);
    lv_label_set_text(s_stats, st);
}

// (Re)initialise the chart + readout for the current metric/unit.
static void rebuild(void) {
    const metric_t *m = &METRICS[s_idx];
    lv_label_set_text(s_title, m->name);
    lv_chart_set_series_color(s_chart, s_ser, lv_color_hex(m->color_hex));
    samp_reset();
    apply_range();
    update_readout();
    push_point();
}

static void card_cb(lv_event_t *e) {
    const metric_t *m = &METRICS[s_idx];
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (m->n_units <= 1) return;          // nothing to cycle
    s_unit = (s_unit + 1) % m->n_units;
    samp_reset();
    apply_range();
    update_readout();
    push_point();
}

static void dd_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t sel = lv_dropdown_get_selected(s_dd);
    if (sel >= (uint16_t)N_METRICS) sel = 0;
    s_idx = sel;
    s_unit = 0;
    rebuild();
}

static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (s_chart == NULL) return;
    push_point();
    update_readout();
}

void ui_graphs_create(lv_obj_t *tile) {
    if (!lvgl_port_lock(2000)) {
        ESP_LOGE(TAG, "LVGL lock timeout in create");
        return;
    }

    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x101418), LV_PART_MAIN);

    s_title = lv_label_create(tile);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x30d070), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(s_title, "graph");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, TITLE_Y);

    // Metric picker. LV_USE_DROPDOWN / LV_USE_DROPDOWN_LIST are enabled.
    s_dd = lv_dropdown_create(tile);
    lv_obj_set_style_text_font(s_dd, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_dropdown_set_options(s_dd,
        "Temperature\nHumidity\nAccel |a|\nGyro |w|\nBattery");
    lv_dropdown_set_selected(s_dd, 0);
    lv_obj_set_size(s_dd, 168, 32);
    lv_obj_align(s_dd, LV_ALIGN_TOP_LEFT, 6, DD_Y);
    lv_obj_add_flag(s_dd, LV_OBJ_FLAG_GESTURE_BUBBLE);   // let swipes page
    lv_obj_add_event_cb(s_dd, dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // The one graph.
    s_chart = lv_chart_create(tile);
    lv_obj_set_size(s_chart, CHART_W, CHART_H);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, CHART_Y);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, N_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_text_font(s_chart, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 3, 1, true, 40);
    lv_chart_set_axis_tick(s_chart, LV_CHART_AXIS_PRIMARY_X, 4, 2, 3, 1, true, 24);
    lv_obj_add_event_cb(s_chart, chart_x_labels_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    // Charts are clickable+scrollable by default and would eat the page swipe.
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
    s_ser = lv_chart_add_series(s_chart, lv_color_hex(METRICS[0].color_hex),
                                LV_CHART_AXIS_PRIMARY_Y);

    // Value card: big number (tap = cycle unit) + small min/max/avg line.
    s_card = lv_obj_create(tile);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_align(s_card, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x181c22), LV_PART_MAIN);
    lv_obj_set_style_radius(s_card, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x2a3140), LV_PART_MAIN);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_card, card_cb, LV_EVENT_CLICKED, NULL);

    s_val = lv_label_create(s_card);
    lv_obj_set_style_text_color(s_val, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_label_set_text(s_val, "--");
    lv_obj_align(s_val, LV_ALIGN_TOP_LEFT, 2, 0);

    s_stats = lv_label_create(s_card);
    lv_obj_set_style_text_color(s_stats, lv_color_hex(0x9aa4b2), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(s_stats, "");
    lv_obj_align(s_stats, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    rebuild();
    s_timer = lv_timer_create(tick_cb, SAMPLE_MS, NULL);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "graph page ready (%d metrics)", N_METRICS);
}

void ui_graphs_destroy(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    s_dd = s_chart = NULL;
    s_ser = NULL;
    s_title = s_card = s_val = s_stats = NULL;
}
