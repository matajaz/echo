#include "ui_page.h"
#include "ui_widgets.h"
#include "ota.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdlib.h>
#include <stdio.h>

// Device/Diagnostics: three grouped cards (Connectivity / System / Firmware)
// so the page has visible boxes + accent colours again (not one flat label).
typedef struct { lv_obj_t *conn, *sys, *fw; } info_t;

void *ui_info_create(lv_obj_t *root) {
    info_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;

    lv_obj_t *c1=ui_card(root,76); ui_card_accent(c1,ECHO_BLUE);
    ui_label(c1,"CONNECTIVITY",ECHO_FONT_SMALL,ECHO_BLUE);
    s->conn=ui_label(c1,"...",ECHO_FONT_SMALL,ECHO_TEXT);
    lv_obj_set_width(s->conn,208); lv_label_set_long_mode(s->conn,LV_LABEL_LONG_WRAP);

    lv_obj_t *c2=ui_card(root,76); ui_card_accent(c2,ECHO_BLUE);
    ui_label(c2,"SYSTEM",ECHO_FONT_SMALL,ECHO_BLUE);
    s->sys=ui_label(c2,"...",ECHO_FONT_SMALL,ECHO_TEXT);
    lv_obj_set_width(s->sys,208); lv_label_set_long_mode(s->sys,LV_LABEL_LONG_WRAP);

    lv_obj_t *c3=ui_card(root,80); ui_card_accent(c3,ECHO_BLUE);
    ui_label(c3,"FIRMWARE",ECHO_FONT_SMALL,ECHO_BLUE);
    s->fw=ui_label(c3,"...",ECHO_FONT_SMALL,ECHO_TEXT);
    lv_obj_set_width(s->fw,208); lv_label_set_long_mode(s->fw,LV_LABEL_LONG_WRAP);
    return s;
}

void ui_info_update(void *state,const ui_model_t *m) {
    info_t *s=state;
    const esp_app_desc_t *app=esp_app_get_description();
    ota_debug_t ota; ota_debug_get(&ota);
    char b[256];

    // Connectivity
    snprintf(b,sizeof(b),"Wi-Fi %d dBm\nHA %s\nClock: %s",
        m->rssi, m->ha.connected?"connected":"offline", m->clock_source);
    ui_text(s->conn,b);

    // System
    snprintf(b,sizeof(b),
        "Uptime %lus\nInternal free %u B\nDMA largest %u B\nPSRAM free %u B\n"
        "Battery %d%% / ADC %u\nBoard %.1f\xC2\xB0""C / %.0f%% RH",
        (unsigned long)(m->now_us/1000000),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        m->sensors.bat_soc_pct, m->sensors.bat_adc,
        m->sensors.temp_c, m->sensors.humidity_rh);
    ui_text(s->sys,b);

    // Firmware
    snprintf(b,sizeof(b),"Build %s\nESP32-C5 / LVGL 8.4\nAudio %s / %d%%\nOTA: %s",
        app->version, m->audio.ready?"ready":"unavailable", (int)m->audio.volume,
        ota.in_progress?"running":ota.last_result);
    ui_text(s->fw,b);
}
