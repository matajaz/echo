// Render the production page modules using the actual LVGL 8 renderer.
#include "ui_page.h"
#include "ui_widgets.h"
#include "ui_graphs.h"
#include "ha_parse.h"
#include "ota.h"
#include "esp_app_desc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
static lv_color_t frame[240*320];
static ui_model_t model;
static energy_prices_t prices;
static energy_history_t history;
static int day_offset;
static int song_posts,last_song=-1,stop_posts,last_volume=-1;
int64_t esp_timer_get_time(void) { return model.now_us; }
bool sensors_get(sensors_data_t *out) { *out=model.sensors; return true; }
const char *sensors_clock_source(void) { return "NTP"; }
void audio_get_status(audio_status_t *out) { *out=model.audio; }
bool audio_post_song(int index) { ++song_posts; last_song=index; return true; }
bool audio_post_stop(void) { ++stop_posts; return true; }
bool audio_post_mute(bool mute) { (void)mute; return true; }
bool audio_post_volume_absolute(int volume) { last_volume=volume; return true; }
static const char *names[]={"Happy Birthday","Twinkle Twinkle","Ode to Joy","Jingle Bells","Fur Elise"};
int audio_song_count(void) { return 5; }
const audio_song_t *audio_get_song(int index) { static audio_song_t song; if(index<0 || index>=5) return NULL; song.name=names[index]; return &song; }
bool ha_prices_get(energy_prices_t *out) { *out=prices; return true; }
bool ha_history_get(energy_history_t *out) { *out=history; return true; }
void ha_history_request(int day) { day_offset=day; }
const ha_sensor_t *ha_find_role(const ha_data_t *d,ha_role_t role) {
    for(int i=0;i<d->power_count;++i) if(d->power[i].role==role) return &d->power[i];
    for(int i=0;i<d->count;++i) if(d->sensors[i].role==role) return &d->sensors[i]; return NULL;
}
void ui_navigate(ui_page_id_t page) { (void)page; }
void ui_clean_start(void) {}
void ui_countdown_toggle(void) {}
int ui_countdown_seconds(void) { return 0; }
bool ui_countdown_done(void) { return false; }
void ui_feedback(const char *text) { (void)text; }
const esp_app_desc_t *esp_app_get_description(void) { static esp_app_desc_t app={.version=ECHO_PREVIEW_VERSION}; return &app; }
void ota_debug_get(ota_debug_t *out) { *out=(ota_debug_t){.last_result="idle"}; }
static void flush(lv_disp_drv_t *drv,const lv_area_t *area,lv_color_t *colors) {
    for(int y=area->y1;y<=area->y2;++y) for(int x=area->x1;x<=area->x2;++x) {
        if(x>=0 && x<240 && y>=0 && y<320) frame[y*240+x]=*colors;
        ++colors;
    }
    lv_disp_flush_ready(drv);
}
static int clipping(lv_obj_t *object,const lv_area_t *bounds) {
    if(lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)) return 0;
    lv_area_t a; lv_obj_get_coords(object,&a); int bad=0;
    if(a.x1<bounds->x1 || a.y1<bounds->y1 || a.x2>bounds->x2 || a.y2>bounds->y2) {
        printf("Outside page: %s (%d,%d)-(%d,%d)\n",lv_obj_check_type(object,&lv_label_class)?lv_label_get_text(object):"container",a.x1,a.y1,a.x2,a.y2);
        ++bad;
    }
    for(unsigned i=0;i<lv_obj_get_child_cnt(object);++i) bad+=clipping(lv_obj_get_child(object,i),bounds);
    return bad;
}
static void save(const char *name) {
    FILE *f=fopen(name,"wb"); assert(f); fputs("P6\n240 320\n255\n",f);
    for(int i=0;i<240*320;++i) {
        lv_color32_t c; c.full=lv_color_to32(frame[i]);
        unsigned char rgb[]={c.ch.red,c.ch.green,c.ch.blue}; fwrite(rgb,1,3,f);
    }
    fclose(f);
}
static char *readfile(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f); fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char *p=malloc(n+1); assert(p); assert(fread(p,1,n,f)==(size_t)n); p[n]=0; fclose(f); return p;
}
int main(int argc,char **argv) {
    assert(argc==3); setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();
    char *json=readfile(argv[1]); float current;
    assert(ha_parse_state(json,"SEK/kWh",&current,&prices)==HA_VALID); free(json);
    model.now_us=1000000000; model.wall_time=prices.today.points[0].start+12*3600;
    prices.received_us=model.now_us;
    model.have_ha=model.have_sensors=true; model.clock_source="NTP"; model.rssi=-62;
    model.sensors=(sensors_data_t){.th_last_us=model.now_us,.imu_last_us=model.now_us,.bat_last_us=model.now_us,
        .temp_c=30.7,.humidity_rh=36,.bat_soc_pct=97,.bat_adc=407,.bat_ok=true,.accel_z=9.81,.gyro_x=0.1};
    audio_status_init(&model.audio,70); model.audio.ready=true;
    model.ha.count=9; model.ha.batt_count=8; model.ha.power_count=5; model.ha.connected=true;
    const char *sensor_names[]={"Ute-Aquara","Ute-Grund","Framsida","Oliver","Kok","Badrum","Vind","Dusch","V-rum"};
    for(int i=0;i<9;++i) {
        model.ha.sensors[i]=(ha_sensor_t){.ok=true,.received_us=model.now_us,.stale_ms=90000,.result=HA_VALID,.value=8.7+i*2,.unit="C"};
        snprintf(model.ha.sensors[i].name,HA_NAME_LEN,"%s",sensor_names[i]);
    }
    model.ha.sensors[0].role=HA_ROLE_OUTDOOR; model.ha.sensors[4].role=HA_ROLE_INDOOR; model.ha.sensors[4].value=21.7;
    for(int i=0;i<8;++i) { model.ha.batteries[i]=model.ha.sensors[i]; model.ha.batteries[i].value=i?35+i*5:1; strcpy(model.ha.batteries[i].unit,"%"); }
    const ha_role_t roles[]={HA_ROLE_SOLAR_POWER,HA_ROLE_GRID_POWER,HA_ROLE_GRID_EXPORT_TODAY,HA_ROLE_GRID_IMPORT_TODAY,HA_ROLE_PRICE};
    const char *units[]={"W","W","kWh","kWh","SEK/kWh"}; float values[]={930,2679,.19,26.948,current};
    for(int i=0;i<5;++i) { model.ha.power[i]=(ha_sensor_t){.role=roles[i],.value=values[i],.ok=true,.received_us=model.now_us,.stale_ms=900000,.result=HA_VALID}; strcpy(model.ha.power[i].unit,units[i]); }
    history.day_start=prices.today.points[0].start; history.day_end=prices.today.points[prices.today.count-1].end;
    history.count=(history.day_end-history.day_start)/3600; history.ok=true; history.received_us=model.now_us;
    for(int i=0;i<history.count;++i) history.hours[i]=(energy_hour_t){.start=history.day_start+i*3600,.imported=.7f+.2f*(i%7),.exported=i>=9&&i<15?.2f:0,.import_ok=true,.export_ok=true};
    lv_init(); static lv_color_t buffer[240*32]; static lv_disp_draw_buf_t draw;
    lv_disp_draw_buf_init(&draw,buffer,NULL,240*32); lv_disp_drv_t drv; lv_disp_drv_init(&drv);
    drv.hor_res=240; drv.ver_res=320; drv.flush_cb=flush; drv.draw_buf=&draw; lv_disp_drv_register(&drv); ui_theme_init();
    void *(*create[])(lv_obj_t *)={ui_overview_create,ui_energy_now_create,ui_energy_today_create,ui_prices_create,ui_temperatures_create,ui_batteries_create,ui_melodies_create,ui_graphs_create,ui_info_create};
    void (*update[])(void *,const ui_model_t *)={ui_overview_update,ui_energy_now_update,ui_energy_today_update,ui_prices_update,ui_temperatures_update,ui_batteries_update,ui_melodies_update,ui_graphs_update,ui_info_update};
    const char *titles[]={"Overview","Energy now","Energy today","Prices","Temperatures","Batteries","Melodies","Sensor graphs","Device info"};
    int errors=0;
    for(int i=0;i<9;++i) {
        lv_obj_t *screen=lv_scr_act(); lv_obj_clean(screen); lv_obj_set_style_bg_color(screen,lv_color_hex(ECHO_BG),0);
        ui_header_t header=ui_header(screen,titles[i]);
        ui_text(header.clock,"12:00"); ui_text(header.battery,LV_SYMBOL_BATTERY_FULL);
        lv_obj_set_style_text_color(header.wifi,lv_color_hex(ECHO_GREEN),0);
        lv_obj_t *root=lv_obj_create(screen); lv_obj_set_pos(root,0,28); lv_obj_set_size(root,240,256); ui_page_style(root);
        void *state=create[i](root); assert(state); update[i](state,&model);
        if(i==6) {
            lv_obj_t *volume=lv_obj_get_child(root,1);
            model.audio.ready=false; update[i](state,&model);
            assert(lv_obj_has_state(volume,LV_STATE_DISABLED));
            model.audio.ready=true; update[i](state,&model);
            assert(!lv_obj_has_state(volume,LV_STATE_DISABLED) && lv_slider_get_value(volume)==70);
            lv_slider_set_value(volume,40,LV_ANIM_OFF); lv_event_send(volume,LV_EVENT_VALUE_CHANGED,NULL);
            lv_slider_set_value(volume,60,LV_ANIM_OFF); lv_event_send(volume,LV_EVENT_VALUE_CHANGED,NULL);
            assert(last_volume==60);
            int posted=song_posts;
            lv_event_send(lv_obj_get_child(root,4),LV_EVENT_CLICKED,NULL); // select third song
            assert(song_posts==posted); // browsing/selection never starts playback
            lv_obj_t *play=lv_obj_get_child(lv_obj_get_child(root,0),0);
            lv_event_send(play,LV_EVENT_CLICKED,NULL); assert(last_song==2);
            model.audio.state=AUDIO_PLAYING; update[i](state,&model);
            lv_event_send(play,LV_EVENT_CLICKED,NULL); assert(stop_posts==1);
            model.audio.state=AUDIO_IDLE; update[i](state,&model);
            printf("Melody readiness/selection/absolute volume/Stop interactions passed\n");
        }
        lv_obj_t *nav=ui_row(screen,224,32,4); lv_obj_set_pos(nav,8,286);
        ui_button(nav,LV_SYMBOL_HOME,32,32,NULL,NULL); ui_button(nav,LV_SYMBOL_LEFT,28,32,NULL,NULL);
        char pos[12]; snprintf(pos,sizeof(pos),"%d/9",i+1); lv_obj_t *index=ui_label(nav,pos,ECHO_FONT_SMALL,ECHO_TEXT_DIM); lv_obj_set_size(index,36,32);
        ui_button(nav,LV_SYMBOL_RIGHT,28,32,NULL,NULL); ui_button(nav,"Menu",48,32,NULL,NULL); ui_button(nav,"Pin",32,32,NULL,NULL);
        lv_obj_update_layout(screen); lv_refr_now(NULL);
        lv_area_t bounds={0,28,239,283}; printf("Page %s:\n",titles[i]); errors+=clipping(root,&bounds);
        char path[512]; snprintf(path,sizeof(path),"%s/page-%d.ppm",argv[2],i); save(path);
        if(i==7) ui_graphs_destroy(state); else ui_state_free(state);
    }
    printf("Page clipping errors: %d\n",errors); return errors?1:0;
}
