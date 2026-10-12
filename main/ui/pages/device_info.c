#include "ui_page.h"
#include "ui_widgets.h"
#include "ota.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <stdlib.h>
#include <stdio.h>
typedef struct { lv_obj_t *text; } info_t;
void *ui_info_create(lv_obj_t *root) {
    info_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;
    s->text=ui_label(root,"Loading diagnostics",ECHO_FONT_SMALL,ECHO_TEXT);
    lv_obj_set_width(s->text,224); lv_label_set_long_mode(s->text,LV_LABEL_LONG_WRAP); return s;
}
void ui_info_update(void *state,const ui_model_t *m) {
    info_t *s=state; const esp_app_desc_t *app=esp_app_get_description(); ota_debug_t ota; ota_debug_get(&ota);
    char text[640];
    snprintf(text,sizeof(text),"Echo %s / ESP32-C5\n240 x 320 / LVGL 8.4\n\nWi-Fi %d dBm / HA %s\nClock: %s\nAudio: %s / %d%%\n\nInternal free: %u B\nDMA largest: %u B\nPSRAM free: %u B\n\nBattery estimate: %d%% / ADC %u\nBoard: %.1f°C / %.0f%% RH\n\nOTA: %s\nUptime: %lus",
        app->version,m->rssi,m->ha.connected?"connected":"offline",m->clock_source,
        m->audio.ready?"ready":"unavailable",(int)m->audio.volume,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),m->sensors.bat_soc_pct,m->sensors.bat_adc,
        m->sensors.temp_c,m->sensors.humidity_rh,ota.in_progress?"running":ota.last_result,
        (unsigned long)(m->now_us/1000000));
    ui_text(s->text,text);
}
