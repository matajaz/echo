#include "ui_shell.h"
#include "ui_page.h"
#include "ui_widgets.h"
#include "ui_graphs.h"
#include "input_gate.h"
#include "comfort_rules.h"
#include "display.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "ota.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static const ui_page_desc_t pages[UI_PAGE_COUNT]={
    [UI_OVERVIEW]={"Overview",ui_overview_create,ui_overview_update,NULL,NULL,NULL},
    [UI_ENERGY_NOW]={"Energy now",ui_energy_now_create,ui_energy_now_update,NULL,NULL,NULL},
    [UI_ENERGY_TODAY]={"Energy today",ui_energy_today_create,ui_energy_today_update,ui_energy_today_enter,NULL,NULL},
    [UI_PRICES]={"Prices",ui_prices_create,ui_prices_update,NULL,NULL,NULL},
    [UI_TEMPERATURES]={"Temperatures",ui_temperatures_create,ui_temperatures_update,NULL,NULL,NULL},
    [UI_BATTERIES]={"Batteries",ui_batteries_create,ui_batteries_update,NULL,NULL,NULL},
    [UI_MELODIES]={"Melodies",ui_melodies_create,ui_melodies_update,NULL,NULL,NULL},
    [UI_GRAPHS]={"Sensor graphs",ui_graphs_create,ui_graphs_update,NULL,NULL,ui_graphs_destroy},
    [UI_INFO]={"Device info",ui_info_create,ui_info_update,NULL,NULL,NULL},
    [UI_COSTS]={"Energy costs",ui_costs_create,ui_costs_update,ui_costs_enter,NULL,NULL},
    [UI_STATUS]={"Right now",ui_dash_status_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_WEATHER]={"Weather",ui_dash_weather_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_WEATHER_DAY]={"Weather today",ui_dash_weather_day_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_SUN]={"Sun & Moon",ui_dash_sun_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_NETWORK]={"Network",ui_dash_network_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_MARKETS]={"World / Markets",ui_dash_markets_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_AIR]={"Air quality",ui_dash_air_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_SERVICES]={"Network clients",ui_dash_services_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_NEWS]={"News",ui_dash_news_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_POLICE]={"Police events",ui_dash_police_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_TRANSIT]={"SL Transit",ui_dash_transit_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_LIGHTS]={"Lights",ui_dash_lights_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_HUMIDITY]={"Humidity",ui_dash_humidity_create,ui_dashboard_update,NULL,NULL,NULL},
    [UI_POWER_HISTORY]={"Solar / grid history",ui_power_history_create,ui_power_history_update,NULL,NULL,NULL}
};
typedef struct {
    lv_obj_t *tileview,*tiles[UI_PAGE_COUNT],*title,*clock,*wifi,*battery,*index,*pin,*menu,*clean,*clean_text,*toast,*motion_button;
    void *states[UI_PAGE_COUNT];
    lv_timer_t *timer;
    void (*raw_read)(lv_indev_drv_t *,lv_indev_data_t *);
    input_gate_t input_gate;
    ui_model_t model;
    ui_page_id_t active;
    uint64_t activity,clean_until,countdown_until,toast_until;
    bool dimmed,pinned,countdown_fired,pickup_wake;
    int backlight_override,saved_page;
    uint64_t page_changed;
    uint32_t motion_seq;
    comfort_rt_t comfort[2];
} shell_t;
static const comfort_rule_t rules[]={
    {"Board humidity high",COMFORT_ABOVE,70,65,600000},
    {"Board temperature low",COMFORT_BELOW,15,17,600000}
};
static shell_t *s;
static portMUX_TYPE status_mux=portMUX_INITIALIZER_UNLOCKED;
static ui_status_t status;
void ui_shell_snapshot(ui_status_t *out) {
    portENTER_CRITICAL(&status_mux); *out=status; portEXIT_CRITICAL(&status_mux);
}
const char *ui_shell_page_name(int page) { return page>=0 && page<UI_PAGE_COUNT?pages[page].title:"Unavailable"; }
static void publish_status(uint64_t now) {
    ui_status_t next={.ready=true,.dimmed=s->dimmed,.pinned=s->pinned,.page=s->active,
        .manual_backlight=s->backlight_override>=0,.backlight=s->backlight_override>=0?s->backlight_override:s->dimmed?10:100,
        .idle_ms=(now-s->activity)/1000};
    portENTER_CRITICAL(&status_mux); status=next; portEXIT_CRITICAL(&status_mux);
}

static uint64_t now_us(void) { return esp_timer_get_time(); }
static void dim(bool on) { if (s->dimmed==on) return; if (display_set_backlight(s->backlight_override>=0?s->backlight_override:on?10:100)==ESP_OK) s->dimmed=on; }
static void touch_read(lv_indev_drv_t *driver,lv_indev_data_t *data) {
    s->raw_read(driver,data);
    bool pressed=data->state==LV_INDEV_STATE_PR;
    bool blocked=s->dimmed || s->clean_until!=0;
    if (pressed) {
        s->activity=now_us();
        if (s->dimmed) dim(false);
    }
    if (input_gate_filter(&s->input_gate,pressed,blocked)) data->state=LV_INDEV_STATE_REL;
}
static void navigation_changed(lv_event_t *event) {
    (void)event;
    lv_obj_t *tile=lv_tileview_get_tile_act(s->tileview);
    for (int i=0;i<UI_PAGE_COUNT;++i) if (tile==s->tiles[i]) {
        ui_page_id_t previous=s->active;
        if (previous!=i && pages[previous].leave && s->states[previous]) pages[previous].leave(s->states[previous]);
        if (previous!=i) s->page_changed=now_us();
        s->active=i; ui_text(s->title,pages[i].title);
        char text[16]; snprintf(text,sizeof(text),"%d/%d",i+1,UI_PAGE_COUNT); ui_text(s->index,text);
        if (previous!=i && pages[i].enter && s->states[i]) pages[i].enter(s->states[i]);
        break;
    }
}
void ui_navigate(ui_page_id_t page) {
    if (!s || page<0 || page>=UI_PAGE_COUNT) return;
    s->activity=now_us(); dim(false); lv_obj_set_tile(s->tileview,s->tiles[page],LV_ANIM_OFF);
    lv_obj_add_flag(s->menu,LV_OBJ_FLAG_HIDDEN); navigation_changed(NULL);
}
static void nav_button(lv_event_t *event) {
    intptr_t action=(intptr_t)lv_event_get_user_data(event);
    if (action==-1) ui_navigate(s->active? s->active-1:0);
    else if (action==-2) ui_navigate(s->active+1<UI_PAGE_COUNT?s->active+1:s->active);
    else if (action==-3) { lv_obj_clear_flag(s->menu,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s->menu); }
    else if (action==-4) {
        s->pinned=!s->pinned;
        ui_button_accent(s->pin,s->pinned?ECHO_ACTION:ECHO_BUTTON);
        ui_feedback(s->pinned?"Keep this page":"Automatic home return");
    } else ui_navigate((ui_page_id_t)action);
}
static void pickup(lv_event_t *e) {
    (void)e; s->pickup_wake=!s->pickup_wake;
    ui_text(lv_obj_get_child(s->motion_button,0),s->pickup_wake?"Pickup wake: on":"Pickup wake: off");
}
void ui_clean_start(void) {
    if (!s) return;
    s->clean_until=now_us()+30000000; dim(false);
    lv_obj_clear_flag(s->clean,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s->clean);
}
void ui_countdown_toggle(void) {
    if (!s) return;
    s->countdown_until=s->countdown_until?0:now_us()+300000000;
    s->countdown_fired=false;
}
int ui_countdown_seconds(void) {
    if (!s || !s->countdown_until || now_us()>=s->countdown_until) return 0;
    return (s->countdown_until-now_us()+999999)/1000000;
}
bool ui_countdown_done(void) { return s && s->countdown_fired; }
void ui_feedback(const char *message) {
    if (!s) return;
    ui_text(s->toast,message); s->toast_until=now_us()+3000000;
    lv_obj_clear_flag(s->toast,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(s->toast);
}
void ui_shell_status(const char *text) { ui_feedback(text); }
static void update(lv_timer_t *timer) {
    (void)timer; uint64_t now=now_us(); s->model.now_us=now; s->model.wall_time=time(NULL);
    s->model.have_ha=ha_get(&s->model.ha); s->model.have_sensors=sensors_get(&s->model.sensors);
    ui_power_history_record(&s->model);
    audio_get_status(&s->model.audio); s->model.clock_source=sensors_clock_source();
    bool th=s->model.have_sensors && sensors_group_fresh_at(s->model.sensors.th_last_us,now);
    comfort_rule_update(&rules[0],&s->comfort[0],th,s->model.sensors.humidity_rh,now/1000);
    comfort_rule_update(&rules[1],&s->comfort[1],th,s->model.sensors.temp_c,now/1000);
    s->model.advisory=s->comfort[0].state==COMFORT_ACTIVE?rules[0].label:
        s->comfort[1].state==COMFORT_ACTIVE?rules[1].label:NULL;
    wifi_ap_record_t ap; s->model.rssi=esp_wifi_sta_get_ap_info(&ap)==ESP_OK?ap.rssi:0;
    char text[32]; struct tm wall; localtime_r(&s->model.wall_time,&wall);
    if (wall.tm_year>=121) snprintf(text,sizeof(text),"%02d:%02d",wall.tm_hour,wall.tm_min);
    else snprintf(text,sizeof(text),"--:--");
    ui_text(s->clock,text);
    lv_obj_set_style_text_color(s->wifi,lv_color_hex(s->model.rssi?ECHO_GREEN:ECHO_TEXT_DIM),0);
    bool battery=s->model.have_sensors && s->model.sensors.bat_ok &&
        sensors_group_fresh_at(s->model.sensors.bat_last_us,now);
    int percent=s->model.sensors.bat_soc_pct;
    ui_text(s->battery,!battery?LV_SYMBOL_BATTERY_EMPTY:percent>=85?LV_SYMBOL_BATTERY_FULL:
        percent>=60?LV_SYMBOL_BATTERY_3:percent>=35?LV_SYMBOL_BATTERY_2:percent>=10?LV_SYMBOL_BATTERY_1:LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(s->battery,lv_color_hex(!battery?ECHO_TEXT_DIM:percent<=10?ECHO_RED:ECHO_TEXT),0);
    if (s->clean_until) {
        if (now>=s->clean_until) { s->clean_until=0; s->activity=now; lv_obj_add_flag(s->clean,LV_OBJ_FLAG_HIDDEN); }
        else { snprintf(text,sizeof(text),"Cleaning\nTouch locked\n%lus",(unsigned long)((s->clean_until-now+999999)/1000000)); ui_text(s->clean_text,text); }
    }
    if (s->countdown_until && now>=s->countdown_until) {
        s->countdown_until=0; s->countdown_fired=true; audio_post_tone(880,400); dim(false);
        s->activity=now; ui_feedback("Timer finished");
    }
    if (s->model.have_sensors && s->model.sensors.motion_seq!=s->motion_seq) {
        s->motion_seq=s->model.sensors.motion_seq;
        if (s->pickup_wake && !s->clean_until) { dim(false); s->activity=now; }
    }
    if (!s->clean_until) {
        if (!s->dimmed && now-s->activity>=60000000) dim(true);
        if (!s->pinned && !audio_is_active(&s->model.audio) && now-s->activity>=120000000 && s->active!=UI_OVERVIEW) {
            // Auto-return is not user activity: keep an already dimmed display dim.
            ui_navigate(UI_OVERVIEW); s->activity=now-60000000;
        }
    }
    if (s->toast_until && now>=s->toast_until) { s->toast_until=0; lv_obj_add_flag(s->toast,LV_OBJ_FLAG_HIDDEN); }
    if (s->states[s->active]) pages[s->active].update(s->states[s->active],&s->model);
    ota_debug_t ota; ota_debug_get(&ota);
    if (s->active!=s->saved_page && now-s->page_changed>=5000000 && !ota.in_progress) {
        nvs_handle_t n;
        if (nvs_open("echo_ui",NVS_READWRITE,&n)==ESP_OK) {
            if (nvs_set_i32(n,"page",s->active)==ESP_OK && nvs_commit(n)==ESP_OK) s->saved_page=s->active;
            nvs_close(n);
        }
        s->page_changed=now;
    }
    publish_status(now);
}
bool ui_shell_init(lv_indev_t *input) {
    s=lv_mem_alloc(sizeof(*s)); if (!s) return false; memset(s,0,sizeof(*s));
    s->backlight_override=-1; s->saved_page=-1;
    s->activity=now_us(); ui_theme_init(); lv_obj_t *screen=lv_scr_act();
    lv_obj_set_style_bg_color(screen,lv_color_hex(ECHO_BG),0); lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    ui_header_t header=ui_header(screen,"Overview");
    s->title=header.title; s->clock=header.clock; s->wifi=header.wifi; s->battery=header.battery;
    s->tileview=lv_tileview_create(screen); lv_obj_set_size(s->tileview,240,256); lv_obj_set_pos(s->tileview,0,28);
    lv_obj_add_event_cb(s->tileview,navigation_changed,LV_EVENT_VALUE_CHANGED,NULL);
    for (int i=0;i<UI_PAGE_COUNT;++i) {
        s->tiles[i]=lv_tileview_add_tile(s->tileview,i,0,(i?LV_DIR_LEFT:0)|(i+1<UI_PAGE_COUNT?LV_DIR_RIGHT:0));
        ui_page_style(s->tiles[i]); s->states[i]=pages[i].create(s->tiles[i]);
        if (!s->states[i]) ui_label(s->tiles[i],"Page unavailable: memory",ECHO_FONT_READ,ECHO_RED);
    }
    lv_obj_t *nav=ui_row(screen,224,32,4); lv_obj_set_pos(nav,8,286);
    ui_button(nav,LV_SYMBOL_HOME,32,32,nav_button,(void *)UI_OVERVIEW);
    ui_button(nav,LV_SYMBOL_LEFT,28,32,nav_button,(void *)-1);
    s->index=ui_label(nav,"1/24",ECHO_FONT_SMALL,ECHO_TEXT_DIM); lv_obj_set_size(s->index,36,32); lv_obj_set_style_text_align(s->index,LV_TEXT_ALIGN_CENTER,0);
    ui_button(nav,LV_SYMBOL_RIGHT,28,32,nav_button,(void *)-2);
    ui_button(nav,"Menu",48,32,nav_button,(void *)-3);
    s->pin=ui_button(nav,"Pin",32,32,nav_button,(void *)-4);
    s->menu=ui_column(screen,240,256,4); lv_obj_set_pos(s->menu,0,28);
    lv_obj_set_style_bg_color(s->menu,lv_color_hex(ECHO_BG),0); lv_obj_set_style_bg_opa(s->menu,LV_OPA_COVER,0);
    lv_obj_set_style_pad_all(s->menu,8,0); lv_obj_set_flex_flow(s->menu,LV_FLEX_FLOW_ROW_WRAP);
    for (int i=0;i<UI_PAGE_COUNT;++i) ui_button(s->menu,pages[i].title,108,40,nav_button,(void *)(intptr_t)i);
    s->motion_button=ui_button(s->menu,"Pickup wake: off",108,40,pickup,NULL);
    lv_obj_add_flag(s->menu,LV_OBJ_FLAG_HIDDEN);
    s->clean=lv_obj_create(screen); lv_obj_set_size(s->clean,240,320);
    lv_obj_set_style_bg_color(s->clean,lv_color_hex(ECHO_BG),0); lv_obj_set_style_bg_opa(s->clean,LV_OPA_COVER,0);
    s->clean_text=ui_label(s->clean,"Cleaning\nTouch locked",ECHO_FONT_TITLE,ECHO_GREEN); lv_obj_center(s->clean_text);
    lv_obj_add_flag(s->clean,LV_OBJ_FLAG_HIDDEN);
    s->toast=ui_label(screen,"",ECHO_FONT_SMALL,ECHO_TEXT); lv_obj_set_width(s->toast,224);
    lv_obj_set_style_bg_color(s->toast,lv_color_hex(ECHO_CARD),0); lv_obj_set_style_bg_opa(s->toast,LV_OPA_COVER,0);
    lv_obj_align(s->toast,LV_ALIGN_BOTTOM_MID,0,-36); lv_obj_add_flag(s->toast,LV_OBJ_FLAG_HIDDEN);
    if (input) { s->raw_read=input->driver->read_cb; input->driver->read_cb=touch_read; }
    nvs_handle_t n; int32_t page=UI_OVERVIEW;
    if (nvs_open("echo_ui",NVS_READONLY,&n)==ESP_OK) { nvs_get_i32(n,"page",&page); nvs_close(n); }
    if (page>=0 && page<UI_PAGE_COUNT) { ui_navigate(page); s->saved_page=page; }
    s->timer=lv_timer_create(update,250,NULL); update(NULL);
    return true;
}

bool ui_shell_control(bool page,int value) {
    if (!s || !s->timer) return false;
    if (page) { if (value<0 || value>=UI_PAGE_COUNT) return false; ui_navigate(value); }
    else {
        if (value < -1 || value>100) return false;
        int level=value<0?(s->dimmed?10:100):value;
        if (display_set_backlight(level)!=ESP_OK) return false;
        s->backlight_override=value;
    }
    publish_status(now_us()); return true;
}
