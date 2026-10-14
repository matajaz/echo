#include "ui_page.h"
#include "ui_widgets.h"
#include "chart_history.h"
#include <stdio.h>
typedef struct { lv_obj_t *chart,*status;lv_chart_series_t *series[2];chart_history_t history[2];uint64_t last[2],slot,rendered;unsigned generation,drawn; } power_history_ui_t;
static power_history_ui_t *history;
static void axis(lv_event_t *e) {
 lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);if(d&&d->text&&d->part==LV_PART_TICKS&&d->id==LV_CHART_AXIS_PRIMARY_X)lv_snprintf(d->text,d->text_length,"%s",d->value==0?"60m":d->value==1?"30m":"now");
}
void *ui_power_history_create(lv_obj_t *root) {
 power_history_ui_t *s=ui_state_alloc(sizeof(*s));if(!s)return NULL;history=s;s->rendered=UINT64_MAX;
 ui_label(root,"Solar / grid W - past hour",ECHO_FONT_READ,ECHO_TEXT);
 s->chart=ui_chart(root,172,LV_CHART_TYPE_LINE);lv_chart_set_point_count(s->chart,CHART_HISTORY_POINTS);
 s->series[0]=lv_chart_add_series(s->chart,lv_color_hex(ECHO_SOLAR),LV_CHART_AXIS_PRIMARY_Y);s->series[1]=lv_chart_add_series(s->chart,lv_color_hex(ECHO_COOL),LV_CHART_AXIS_PRIMARY_Y);
 lv_obj_add_event_cb(s->chart,axis,LV_EVENT_DRAW_PART_BEGIN,NULL);
 s->status=ui_label(root,"Yellow solar / blue grid",ECHO_FONT_SMALL,ECHO_TEXT_DIM);return s;
}
void ui_power_history_record(const ui_model_t *m) {
 if(!history)return;
 const ha_role_t roles[]={HA_ROLE_SOLAR_POWER,HA_ROLE_GRID_POWER};history->slot=m->now_us/60000000;
 for(int i=0;i<2;i++){const ha_sensor_t *v=ha_find_role(&m->ha,roles[i]);if(v&&v->result==HA_VALID&&ha_fresh(v,m->now_us)&&v->received_us!=history->last[i]){history->last[i]=v->received_us;chart_history_add(&history->history[i],v->received_us/60000000,v->value);history->generation++;}}
}
void ui_power_history_update(void *state,const ui_model_t *m) {
 (void)m;power_history_ui_t *s=state;if(s->rendered==s->slot&&s->drawn==s->generation)return;s->rendered=s->slot;s->drawn=s->generation;
 float low=0,high=100;for(int j=0;j<2;j++)for(int i=0;i<CHART_HISTORY_POINTS;i++){
  uint64_t age=CHART_HISTORY_POINTS-1-i;bool valid=s->slot>=age;uint64_t slot=valid?s->slot-age:0;
  chart_bucket_t *b=&s->history[j].buckets[slot%CHART_HISTORY_POINTS];valid=valid&&b->count&&b->slot==slot+1;
  float value=valid?(float)(b->sum/b->count):0;if(valid){if(value<low)low=value;if(value>high)high=value;}
  lv_chart_set_value_by_id(s->chart,s->series[j],i,valid?(int)lroundf(value*10):LV_CHART_POINT_NONE);
 }
 lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_Y,(int)floorf(low*11),(int)ceilf(high*11));lv_chart_refresh(s->chart);
 ui_text(s->status,"Yellow solar / blue grid\nMinute averages; gaps stay empty");
}
