#include "ui_graphs.h"
#include "ui/ui_widgets.h"
#include "chart_history.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define METRIC_COUNT 5
#define RANGE_COUNT 4
static const uint64_t periods[RANGE_COUNT]={2000000,10000000,60000000,1440000000};
static const uint32_t colors[METRIC_COUNT]={0xff8040,0x40c0ff,0x40ff80,0xffd040,0x90c0ff};
static const char *const units[METRIC_COUNT]={"°C","%","m/s2","dps","% est."};
static const char *const labels[RANGE_COUNT]={"2 min","10 min","1 hour","24 hours"};
static const char *const left_labels[RANGE_COUNT]={"-2m","-10m","-1h","-24h"};
static const char *const middle_labels[RANGE_COUNT]={"-1m","-5m","-30m","-12h"};
typedef struct {
    lv_obj_t *root,*metric,*chart,*current,*stats,*history_note,*ranges[RANGE_COUNT];
    lv_chart_series_t *series;
    lv_timer_t *collector;
    chart_history_t history[METRIC_COUNT][RANGE_COUNT];
    int selected,range;
    uint64_t last_draw,started;
    bool dirty;
} graphs_t;
static bool values(const sensors_data_t *s,uint64_t now,float out[METRIC_COUNT],bool valid[METRIC_COUNT]) {
    bool th=sensors_group_fresh_at(s->th_last_us,now),imu=sensors_group_fresh_at(s->imu_last_us,now);
    valid[0]=valid[1]=th; valid[2]=valid[3]=imu; valid[4]=s->bat_ok&&sensors_group_fresh_at(s->bat_last_us,now);
    out[0]=s->temp_c; out[1]=s->humidity_rh;
    out[2]=sqrtf(s->accel_x*s->accel_x+s->accel_y*s->accel_y+s->accel_z*s->accel_z);
    out[3]=sqrtf(s->gyro_x*s->gyro_x+s->gyro_y*s->gyro_y+s->gyro_z*s->gyro_z);
    out[4]=s->bat_soc_pct; return true;
}
static void collect(lv_timer_t *timer) {
    graphs_t *s=timer->user_data; sensors_data_t data;
    if (!sensors_get(&data)) return;
    float v[METRIC_COUNT]; bool valid[METRIC_COUNT]; uint64_t now=esp_timer_get_time();
    values(&data,now,v,valid);
    for (int i=0;i<METRIC_COUNT;++i) if (valid[i]) for (int range=0;range<RANGE_COUNT;++range)
        chart_history_add(&s->history[i][range],now/periods[range],v[i]);
}
static void metric_changed(lv_event_t *e) {
    graphs_t *s=lv_event_get_user_data(e); s->selected=lv_dropdown_get_selected(s->metric); s->dirty=true;
}
static void range_changed(lv_event_t *e) {
    graphs_t *s=lv_event_get_user_data(e);
    for (int i=0;i<RANGE_COUNT;++i) if (lv_event_get_target(e)==s->ranges[i]) s->range=i;
    s->dirty=true;
}
static void ticks(lv_event_t *e) {
    graphs_t *s=lv_event_get_user_data(e); lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);
    if (!d || !d->text || d->part!=LV_PART_TICKS || d->id!=LV_CHART_AXIS_PRIMARY_X) return;
    lv_snprintf(d->text,d->text_length,"%s",d->value==0?left_labels[s->range]:d->value==2?"now":middle_labels[s->range]);
}
void *ui_graphs_create(lv_obj_t *root) {
    graphs_t *s=heap_caps_calloc(1,sizeof(*s),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); if (!s) return NULL;
    s->root=root; s->started=esp_timer_get_time(); s->dirty=true;
    s->metric=ui_dropdown(root,"Board temperature\nBoard humidity\nAccel magnitude\nGyro magnitude\nBattery estimate");
    lv_obj_set_size(s->metric,224,30); lv_obj_add_event_cb(s->metric,metric_changed,LV_EVENT_VALUE_CHANGED,s);
    s->chart=ui_chart(root,120,LV_CHART_TYPE_LINE); lv_chart_set_point_count(s->chart,CHART_HISTORY_POINTS);
    s->series=lv_chart_add_series(s->chart,lv_color_hex(ECHO_BLUE),LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(s->chart,s->series,LV_CHART_POINT_NONE);
    lv_obj_add_event_cb(s->chart,ticks,LV_EVENT_DRAW_PART_BEGIN,s);
    s->current=ui_label(root,"Current: --",ECHO_FONT_READ,ECHO_TEXT);
    s->stats=ui_label(root,"Min -- / Max -- / Avg --",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    s->history_note=ui_label(root,"History fills since boot",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_t *row=ui_row(root,224,28,4);
    for (int i=0;i<RANGE_COUNT;++i) s->ranges[i]=ui_button(row,labels[i],53,28,range_changed,s);
    s->collector=lv_timer_create(collect,2000,s); collect(s->collector); return s;
}
void ui_graphs_update(void *state,const ui_model_t *m) {
    graphs_t *s=state; float v[METRIC_COUNT]={0}; bool valid[METRIC_COUNT]={0};
    if (m->have_sensors) values(&m->sensors,m->now_us,v,valid);
    char text[128];
    if (valid[s->selected]) snprintf(text,sizeof(text),"Current: %.1f %s",v[s->selected],units[s->selected]);
    else snprintf(text,sizeof(text),"Current: -- (sensor stale)");
    ui_text(s->current,text);
    lv_obj_set_style_text_color(s->current,lv_color_hex(valid[s->selected]?colors[s->selected]:ECHO_TEXT_DIM),0);
    if (s->dirty || m->now_us-s->last_draw>=2000000) {
        uint64_t slot=m->now_us/periods[s->range]; chart_history_t *h=&s->history[s->selected][s->range];
        chart_stats_t stats=chart_history_stats(h,slot);
        float lo=stats.valid?stats.min:0,hi=stats.valid?stats.max:1;
        if (hi-lo<1) { lo-=.5f; hi+=.5f; }
        lv_chart_set_series_color(s->chart,s->series,lv_color_hex(colors[s->selected]));
        lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_Y,(int)floorf(lo*10),(int)ceilf(hi*10));
        for (int i=0;i<CHART_HISTORY_POINTS;++i) {
            int age=CHART_HISTORY_POINTS-1-i; int value=LV_CHART_POINT_NONE;
            if (slot>=(uint64_t)age) {
                uint64_t expected=slot-age; chart_bucket_t *b=&h->buckets[expected%CHART_HISTORY_POINTS];
                if (b->slot==expected+1 && b->count) value=(int)lround(b->sum/b->count*10);
            }
            lv_chart_set_value_by_id(s->chart,s->series,i,value);
        }
        lv_chart_refresh(s->chart);
        if (stats.valid) snprintf(text,sizeof(text),"Min %.1f / Max %.1f / Avg %.1f",stats.min,stats.max,stats.average);
        else snprintf(text,sizeof(text),"Min -- / Max -- / Avg --");
        ui_text(s->stats,text);
        uint64_t window=periods[s->range]*CHART_HISTORY_POINTS;
        ui_text(s->history_note,m->now_us-s->started<window?"Partial history since boot":"Selected window / missing = gaps");
        for (int i=0;i<RANGE_COUNT;++i) ui_button_accent(s->ranges[i],i==s->range?ECHO_ACTION:ECHO_BUTTON);
        s->last_draw=m->now_us; s->dirty=false;
    }
}
void ui_graphs_destroy(void *state) {
    graphs_t *s=state; if (!s) return; if (s->collector) lv_timer_del(s->collector);
    lv_obj_del(s->root); free(s);
}
