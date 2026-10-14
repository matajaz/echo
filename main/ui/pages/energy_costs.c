#include "ui_page.h"
#include "ui_widgets.h"
#include "cost_model.h"
#include <math.h>
#include <stdio.h>
#include <time.h>
typedef struct {
    int day; lv_obj_t *bought,*income,*net,*chart,*status,*today,*yesterday;
    lv_chart_series_t *cost,*sold;
    energy_history_t history; energy_prices_t prices;
    uint64_t history_stamp,price_stamp; int64_t rendered_day;
    bool cached,accumulated; int completed;
} costs_t;
static void day_changed(lv_event_t *e) {
    costs_t *s=lv_event_get_user_data(e); s->day=lv_event_get_target(e)==s->today?0:-1;
    s->history_stamp=UINT64_MAX; ha_history_request(s->day);
}
static void labels(lv_event_t *e) {
    costs_t *s=lv_event_get_user_data(e); lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);
    if (!d || !d->text || d->part!=LV_PART_TICKS) return;
    if(d->id==LV_CHART_AXIS_PRIMARY_Y){lv_snprintf(d->text,d->text_length,"%.1f",d->value/100.0);return;}
    if(d->id!=LV_CHART_AXIS_PRIMARY_X || !s->history.count)return;
    int i=d->value*(s->history.count-1)/2;
    if (i<0 || i>=s->history.count) return;
    time_t t=s->history.hours[i].start; struct tm tm; localtime_r(&t,&tm);
    lv_snprintf(d->text,d->text_length,"%02d",tm.tm_hour);
}
static void chart_mode(lv_event_t *e) {
    costs_t *s=lv_event_get_user_data(e);s->accumulated=!s->accumulated;s->history_stamp=UINT64_MAX;
    lv_chart_set_type(s->chart,s->accumulated?LV_CHART_TYPE_LINE:LV_CHART_TYPE_BAR);
}
void* ui_costs_create(lv_obj_t *root) {
    costs_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL; s->history_stamp=UINT64_MAX;
    lv_obj_t *row=ui_row(root,224,30,8);
    s->today=ui_button(row,"Today",108,30,day_changed,s); s->yesterday=ui_button(row,"Yesterday",108,30,day_changed,s);
    lv_obj_t *stats=ui_card(root,48); ui_card_accent(stats,ECHO_COOL);
    row=ui_row(stats,LV_PCT(100),40,4);
    lv_obj_t *col=ui_column(row,72,40,0); ui_label(col,"Bought kr",ECHO_FONT_SMALL,ECHO_COOL); s->bought=ui_label(col,"--",ECHO_FONT_READ,ECHO_COOL);
    col=ui_column(row,72,40,0); ui_label(col,"Sold kr",ECHO_FONT_SMALL,ECHO_GREEN); s->income=ui_label(col,"--",ECHO_FONT_READ,ECHO_GREEN);
    col=ui_column(row,72,40,0); ui_label(col,"Net kr",ECHO_FONT_SMALL,ECHO_TEXT); s->net=ui_label(col,"--",ECHO_FONT_READ,ECHO_TEXT);
    lv_obj_t *chartcard=ui_card(root,132); ui_card_accent(chartcard,ECHO_BLUE);
    s->chart=ui_chart(chartcard,116,LV_CHART_TYPE_BAR);
    lv_obj_add_flag(s->chart,LV_OBJ_FLAG_CLICKABLE);lv_obj_add_event_cb(s->chart,chart_mode,LV_EVENT_CLICKED,s);
    s->cost=lv_chart_add_series(s->chart,lv_color_hex(ECHO_BLUE),LV_CHART_AXIS_PRIMARY_Y);
    s->sold=lv_chart_add_series(s->chart,lv_color_hex(ECHO_GREEN),LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(s->chart,labels,LV_EVENT_DRAW_PART_BEGIN,s);
    lv_obj_t *statuscard=ui_card(root,40);
    s->status=ui_label(statuscard,"Loading hourly costs",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->status,LV_PCT(100)); lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);
    return s;
}
void ui_costs_enter(void *state) { costs_t *s=state; ha_history_request(s->day); }
void ui_costs_update(void *state,const ui_model_t *m) {
    costs_t *s=state; ha_history_get(&s->history); ha_prices_get(&s->prices);
    struct tm tm; localtime_r(&m->wall_time,&tm); tm.tm_hour=tm.tm_min=tm.tm_sec=0; tm.tm_mday+=s->day; tm.tm_isdst=-1;
    int64_t wanted=mktime(&tm);
    bool same=s->history.day_start==wanted && s->history.count>0;
    float bought=0,sold=0,low=0,high=1; int known=0,incomes=0,estimated=0,expected=0;
    for (int i=0;same && i<s->history.count;i++) {
        energy_hour_t *h=&s->history.hours[i]; float value;
        if (h->start+3600>m->wall_time) continue;
        if (h->start+3600<=m->wall_time) expected++;
        cost_source_t source=cost_hour(h,&s->prices.today,&value);
        if (source!=COST_MISSING) { bought+=value; known++; if (source==COST_ESTIMATED) estimated++; if(value<low)low=value; if(value>high)high=value; }
        if (h->export_income_ok && isfinite(h->export_income)) { sold+=h->export_income; incomes++; if(h->export_income<low)low=h->export_income; if(h->export_income>high)high=h->export_income; }
    }
    float b=known?bought:NAN,i=incomes?sold:NAN;
    if (!s->day) {
        const ha_sensor_t *v=ha_find_role(&m->ha,HA_ROLE_GRID_IMPORT_COST_TODAY),*w=ha_find_role(&m->ha,HA_ROLE_GRID_EXPORT_INCOME_TODAY);
        // Today cards use authoritative counters including the current hour.
        b=v && ha_fresh(v,m->now_us) && v->result==HA_VALID?v->value:NAN;
        i=w && ha_fresh(w,m->now_us) && w->result==HA_VALID?w->value:NAN;
    }
    ui_number(s->bought,b,"",2); ui_number(s->income,i,"",2); ui_number(s->net,isfinite(b)&&isfinite(i)?b-i:NAN,"",2);
    if (s->history_stamp!=s->history.received_us || s->price_stamp!=s->prices.received_us || s->rendered_day!=wanted || s->cached!=s->history.cached || s->completed!=expected) {
        s->history_stamp=s->history.received_us; s->price_stamp=s->prices.received_us; s->rendered_day=wanted; s->cached=s->history.cached;s->completed=expected;
        int n=same?s->history.count:24; lv_chart_set_point_count(s->chart,n);
        float cost_sum=0,income_sum=0;bool cost_complete=true,income_complete=true;
        if(s->accumulated){low=0;high=1;}
        for (int k=0;k<n;k++) {
            energy_hour_t *h=&s->history.hours[k]; float value;
            bool completed=same && h->start+3600<=m->wall_time;
            bool ok=completed && cost_hour(h,&s->prices.today,&value)!=COST_MISSING;
            bool income_ok=completed&&h->export_income_ok&&isfinite(h->export_income);
            float income=h->export_income;
            if(s->accumulated){
                if(!ok)cost_complete=false;
                if(!income_ok)income_complete=false;
                if(ok)cost_sum+=value;
                if(income_ok)income_sum+=income;
                ok=ok&&cost_complete;income_ok=income_ok&&income_complete;value=cost_sum;income=income_sum;
                if(ok){if(value<low)low=value;if(value>high)high=value;}
                if(income_ok){if(income<low)low=income;if(income>high)high=income;}
            }
            lv_chart_set_value_by_id(s->chart,s->cost,k,ok?(int)lroundf(value*100):LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(s->chart,s->sold,k,income_ok?(int)lroundf(income*100):LV_CHART_POINT_NONE);
        }
        lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_Y,(int)floorf(low*110),(int)ceilf(high*110)); lv_chart_refresh(s->chart);
    }
    char text[144];
    snprintf(text,sizeof(text),"%s kr (tap graph)\n%d/%d hours%s%s%s",s->accumulated?"Accumulated":"Hourly",known,expected,estimated?" / estimate":"",s->history.cached?" / cached":"",same?"":" / loading day");
    ui_text(s->status,text); ui_button_accent(s->today,!s->day?ECHO_ACTION:ECHO_BUTTON); ui_button_accent(s->yesterday,s->day?ECHO_ACTION:ECHO_BUTTON);
}
