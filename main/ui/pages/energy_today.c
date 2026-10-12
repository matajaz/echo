#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
typedef struct {
    int day; lv_obj_t *imported,*exported,*chart,*status,*today,*yesterday;
    lv_chart_series_t *in,*out; energy_history_t history; uint64_t stamp;
} energy_today_t;
static void select_day(lv_event_t *e) {
    energy_today_t *s=lv_event_get_user_data(e);
    s->day=lv_event_get_target(e)==s->today?0:-1; s->stamp=UINT64_MAX;
    ha_history_request(s->day);
}
static void x_labels(lv_event_t *e) {
    energy_today_t *s=lv_event_get_user_data(e); lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);
    if (!d || !d->text || d->part!=LV_PART_TICKS || d->id!=LV_CHART_AXIS_PRIMARY_X) return;
    // For a bar/line chart LVGL emits ordinal major ticks (0,1,2), not
    // the underlying series index. Scatter charts emit the numeric X value.
    int i=(d->value*(s->history.count-1)+1)/2;
    if (i<0 || i>=s->history.count) return;
    time_t t=s->history.hours[i].start; struct tm tm; localtime_r(&t,&tm);
    lv_snprintf(d->text,d->text_length,"%02d",tm.tm_hour);
}
void *ui_energy_today_create(lv_obj_t *root) {
    energy_today_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL; s->stamp=UINT64_MAX;
    lv_obj_t *row=ui_row(root,LV_PCT(100),32,8);
    s->today=ui_button(row,"Today",108,32,select_day,s); s->yesterday=ui_button(row,"Yesterday",108,32,select_day,s);
    row=ui_row(root,LV_PCT(100),38,8);
    lv_obj_t *col=ui_column(row,108,38,2); ui_label(col,"Imported",ECHO_FONT_SMALL,ECHO_BLUE);
    s->imported=ui_label(col,"-- kWh",ECHO_FONT_READ,ECHO_COOL);
    col=ui_column(row,108,38,2); ui_label(col,"Exported",ECHO_FONT_SMALL,ECHO_GREEN);
    s->exported=ui_label(col,"-- kWh",ECHO_FONT_READ,ECHO_GREEN);
    s->chart=ui_chart(root,130,LV_CHART_TYPE_BAR); lv_chart_set_point_count(s->chart,24);
    s->in=lv_chart_add_series(s->chart,lv_color_hex(ECHO_BLUE),LV_CHART_AXIS_PRIMARY_Y);
    s->out=lv_chart_add_series(s->chart,lv_color_hex(ECHO_GREEN),LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(s->chart,x_labels,LV_EVENT_DRAW_PART_BEGIN,s);
    s->status=ui_label(root,"Loading hourly HA statistics",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->status,224); lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);
    return s;
}
void ui_energy_today_enter(void *state) { energy_today_t *s=state; ha_history_request(s->day); }
void ui_energy_today_update(void *state,const ui_model_t *m) {
    energy_today_t *s=state; ha_history_get(&s->history);
    struct tm tm; localtime_r(&m->wall_time,&tm); tm.tm_hour=tm.tm_min=tm.tm_sec=0; tm.tm_mday+=s->day; tm.tm_isdst=-1;
    time_t wanted=mktime(&tm);
    bool same=s->history.day_start==wanted && s->history.count>0;
    float in=0,out=0,max=1; int ni=0,no=0;
    if (same) for (int i=0;i<s->history.count;++i) {
        energy_hour_t *h=&s->history.hours[i];
        if (h->import_ok) { in+=h->imported; ++ni; if (h->imported>max) max=h->imported; }
        if (h->export_ok) { out+=h->exported; ++no; if (h->exported>max) max=h->exported; }
    }
    // Current-day counters include the live incomplete hour; the bars are HA
    // recorder's completed-hour changes. Yesterday totals come from those bars.
    if (!s->day) {
        ui_metric(s->imported,ha_find_role(&m->ha,HA_ROLE_GRID_IMPORT_TODAY),m->now_us,2);
        ui_metric(s->exported,ha_find_role(&m->ha,HA_ROLE_GRID_EXPORT_TODAY),m->now_us,2);
    } else {
        ui_number(s->imported,ni?in:NAN,"kWh",2); ui_number(s->exported,no?out:NAN,"kWh",2);
        lv_obj_set_style_text_color(s->imported,lv_color_hex(ni?ECHO_COOL:ECHO_TEXT_DIM),0);
        lv_obj_set_style_text_color(s->exported,lv_color_hex(no?ECHO_GREEN:ECHO_TEXT_DIM),0);
    }
    uint64_t stamp=same?s->history.received_us:0;
    // Cached history has received_us=0, so day/count also participate in redraw.
    if (s->stamp!=stamp || s->stamp==UINT64_MAX) {
        s->stamp=stamp; int n=same?s->history.count:24;
        lv_chart_set_point_count(s->chart,n);
        lv_chart_set_all_value(s->chart,s->in,LV_CHART_POINT_NONE); lv_chart_set_all_value(s->chart,s->out,LV_CHART_POINT_NONE);
        for (int i=0;i<n;++i) {
            energy_hour_t *h=&s->history.hours[i];
            lv_chart_set_value_by_id(s->chart,s->in,i,same&&h->import_ok?(int)lroundf(h->imported*10):LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s->chart,s->out,i,same&&h->export_ok?(int)lroundf(h->exported*10):LV_CHART_POINT_NONE);
        }
        lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_Y,0,(int)ceilf(max*12)); lv_chart_refresh(s->chart);
    }
    char text[96];
    if (!same) snprintf(text,sizeof(text),"Loading requested day...");
    else if (!s->history.ok) snprintf(text,sizeof(text),"%s",s->history.error[0]?s->history.error:"No recorded data");
    else {
        localtime_r(&wanted,&tm); char date[24]; strftime(date,sizeof(date),"%d %b",&tm);
        snprintf(text,sizeof(text),"%s / %dh / %s\n%.30s",date,s->history.count,
            s->history.cached?"cached":s->history.error[0]?"last HA data":"HA hourly kWh",
            s->history.error[0]?s->history.error:"Missing hours are gaps");
    }
    ui_text(s->status,text);
    ui_button_accent(s->today,!s->day?ECHO_ACTION:ECHO_BUTTON);
    ui_button_accent(s->yesterday,s->day?ECHO_ACTION:ECHO_BUTTON);
}
