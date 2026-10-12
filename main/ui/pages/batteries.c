#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
typedef struct { lv_obj_t *names[HA_MAX_BATT],*values[HA_MAX_BATT],*bars[HA_MAX_BATT],*summary; } batteries_t;
void *ui_batteries_create(lv_obj_t *root) {
    batteries_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;
    for (int i=0;i<HA_MAX_BATT;++i) {
        lv_obj_t *row=ui_row(root,LV_PCT(100),22,4);
        s->names[i]=ui_label(row,"Loading",ECHO_FONT_SMALL,ECHO_TEXT_DIM); lv_obj_set_width(s->names[i],80);
        s->bars[i]=lv_bar_create(row); lv_obj_set_size(s->bars[i],78,10); lv_bar_set_range(s->bars[i],0,100);
        lv_obj_set_style_bg_color(s->bars[i],lv_color_hex(ECHO_CARD),0);
        s->values[i]=ui_label(row,"--",ECHO_FONT_SMALL,ECHO_TEXT); lv_obj_set_width(s->values[i],52);
    }
    s->summary=ui_label(root,"Loading sensor batteries",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->summary,LV_PCT(100)); lv_label_set_long_mode(s->summary,LV_LABEL_LONG_WRAP);
    return s;
}
void ui_batteries_update(void *state,const ui_model_t *m) {
    batteries_t *s=state; int order[HA_MAX_BATT],n=m->ha.batt_count;
    for (int i=0;i<n;++i) order[i]=i;
    for (int i=0;i<n;++i) for (int j=i+1;j<n;++j) {
        const ha_sensor_t *a=&m->ha.batteries[order[i]],*b=&m->ha.batteries[order[j]];
        float av=ha_fresh(a,m->now_us)?a->value:101,bv=ha_fresh(b,m->now_us)?b->value:101;
        if (bv<av) { int t=order[i]; order[i]=order[j]; order[j]=t; }
    }
    int critical=0,low=0,healthy=0,unknown=0;
    for (int i=0;i<n;++i) {
        const ha_sensor_t *v=&m->ha.batteries[order[i]];
        ui_text(s->names[i],v->name);
        bool fresh=ha_fresh(v,m->now_us) && v->result==HA_VALID && v->value>=0 && v->value<=100;
        int pct=fresh?(int)(v->value+.5f):0;
        uint32_t col=!fresh?ECHO_TEXT_DIM:pct<=10?ECHO_RED:pct<=30?ECHO_AMBER:ECHO_GREEN;
        if (!fresh) ++unknown; else if (pct<=10) ++critical; else if (pct<=30) ++low; else ++healthy;
        lv_bar_set_value(s->bars[i],pct,LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s->bars[i],lv_color_hex(col),LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s->values[i],lv_color_hex(col),0);
        if (fresh) ui_number(s->values[i],pct,"%",0); else ui_text(s->values[i],"--");
    }
    char text[80]; snprintf(text,sizeof(text),"%d critical  %d low  %d healthy\n%d unknown/stale",critical,low,healthy,unknown);
    ui_text(s->summary,text);
    lv_obj_set_style_text_color(s->summary,lv_color_hex(critical?ECHO_RED:low?ECHO_AMBER:unknown?ECHO_TEXT_DIM:ECHO_GREEN),0);
}
