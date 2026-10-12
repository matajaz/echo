#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
typedef struct {
    bool tomorrow,drawn_valid; lv_obj_t *current,*chart,*stats,*today,*next;
    lv_chart_series_t *series; energy_prices_t prices; uint64_t stamp; int64_t chart_start;
    uint64_t suggestion_stamp;
    int64_t suggestion_minute;
    char suggestion[128];
} prices_t;
static void choose(lv_event_t *e) {
    prices_t *s=lv_event_get_user_data(e); s->tomorrow=lv_event_get_target(e)==s->next; s->stamp=UINT64_MAX;
    s->suggestion_minute=INT64_MIN;
}
static void ticks(lv_event_t *e) {
    prices_t *s=lv_event_get_user_data(e); lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);
    if (!d || !d->text || d->part!=LV_PART_TICKS || d->id!=LV_CHART_AXIS_PRIMARY_X) return;
    time_t t=s->chart_start+d->value*60; struct tm tm; localtime_r(&t,&tm);
    lv_snprintf(d->text,d->text_length,"%02d:%02d",tm.tm_hour,tm.tm_min);
}
void *ui_prices_create(lv_obj_t *root) {
    prices_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL; s->stamp=UINT64_MAX;
    s->suggestion_minute=INT64_MIN;
    lv_obj_t *card=ui_card(root,56); ui_label(card,"Current electricity price",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    ui_card_accent(card,ECHO_SOLAR);
    s->current=ui_label(card,"-- kr/kWh",ECHO_FONT_TITLE,ECHO_TEXT);
    lv_obj_t *row=ui_row(root,LV_PCT(100),32,8);
    s->today=ui_button(row,"Today",108,32,choose,s); s->next=ui_button(row,"Tomorrow",108,32,choose,s);
    s->chart=ui_chart(root,114,LV_CHART_TYPE_SCATTER);
    lv_obj_set_style_size(s->chart,2,LV_PART_INDICATOR);
    lv_chart_set_update_mode(s->chart,LV_CHART_UPDATE_MODE_SHIFT);
    s->series=lv_chart_add_series(s->chart,lv_color_hex(ECHO_SOLAR),LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_add_event_cb(s->chart,ticks,LV_EVENT_DRAW_PART_BEGIN,s);
    s->stats=ui_label(root,"Loading price intervals",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->stats,224); lv_label_set_long_mode(s->stats,LV_LABEL_LONG_WRAP); return s;
}
void ui_prices_update(void *state,const ui_model_t *m) {
    prices_t *s=state;
    ui_metric(s->current,ha_find_role(&m->ha,HA_ROLE_PRICE),m->now_us,2);
    ha_prices_get(&s->prices);
    const price_day_t *d=s->tomorrow?&s->prices.tomorrow:&s->prices.today;
    bool valid=d->valid && d->count && m->wall_time>=1609459200;
    // Reject old "today" curves after midnight even if a network outage
    // leaves the last successful snapshot cached in RAM.
    if (valid && !s->tomorrow) valid=m->wall_time>=d->points[0].start && m->wall_time<d->points[d->count-1].end;
    if (valid && s->tomorrow) valid=d->points[0].start>m->wall_time;
    if (s->stamp!=s->prices.received_us || s->stamp==UINT64_MAX || s->drawn_valid!=valid) {
        s->stamp=s->prices.received_us;
        s->drawn_valid=valid;
        lv_chart_set_point_count(s->chart,valid?d->count:2);
        lv_chart_set_all_value(s->chart,s->series,LV_CHART_POINT_NONE);
        if (valid) {
            s->chart_start=d->points[0].start;
            float min=d->points[0].value,max=min;
            for (int i=0;i<d->count;++i) { if (d->points[i].value<min) min=d->points[i].value; if (d->points[i].value>max) max=d->points[i].value; }
            lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_X,0,(d->points[d->count-1].end-s->chart_start)/60);
            lv_chart_set_range(s->chart,LV_CHART_AXIS_PRIMARY_Y,(int)floorf(min*10)-1,(int)ceilf(max*10)+1);
            for (int i=0;i<d->count;++i) lv_chart_set_next_value2(s->chart,s->series,
                (d->points[i].start-s->chart_start)/60,(int)lroundf(d->points[i].value*10));
        }
        lv_chart_refresh(s->chart);
    }
    char text[128];
    if (!valid) snprintf(text,sizeof(text),"%s",s->tomorrow?"Tomorrow prices not available yet":"Today's price curve unavailable");
    else {
        // Window optimization is quadratic in interval count. Recompute only
        // when the forecast/day changes or time advances a minute, rather than
        // on every 250ms UI refresh on this single-core board.
        if (s->suggestion_stamp!=s->prices.received_us || s->suggestion_minute!=m->wall_time/60) {
            float min=d->points[0].value,max=min;
            for (int i=1;i<d->count;++i) { if (d->points[i].value<min) min=d->points[i].value; if (d->points[i].value>max) max=d->points[i].value; }
            int64_t cheap; float average;
            if (price_cheapest_after(d,7200,m->wall_time,&cheap,&average)) {
                time_t t=cheap; struct tm tm; localtime_r(&t,&tm);
                snprintf(text,sizeof(text),"Min %.2f / max %.2f kr/kWh\nNext cheap 2h: %02d:%02d / %.2f",min,max,tm.tm_hour,tm.tm_min,average);
            } else snprintf(text,sizeof(text),"Min %.2f / max %.2f kr/kWh\nNo 2h window remaining",min,max);
            strlcpy(s->suggestion,text,sizeof(s->suggestion));
            s->suggestion_stamp=s->prices.received_us;
            s->suggestion_minute=m->wall_time/60;
        }
        strlcpy(text,s->suggestion,sizeof(text));
        if (!s->prices.received_us || m->now_us-s->prices.received_us>900000000ULL) strlcat(text," (stale)",sizeof(text));
    }
    ui_text(s->stats,text);
    ui_button_accent(s->today,!s->tomorrow?ECHO_ACTION:ECHO_BUTTON);
    ui_button_accent(s->next,s->tomorrow?ECHO_ACTION:ECHO_BUTTON);
}
