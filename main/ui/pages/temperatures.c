#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
typedef struct { lv_obj_t *names[HA_MAX_SENSORS],*values[HA_MAX_SENSORS],*status,*local; } temperatures_t;
void *ui_temperatures_create(lv_obj_t *root) {
    temperatures_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;
    for (int i=0;i<HA_MAX_SENSORS;++i) {
        lv_obj_t *row=ui_row(root,LV_PCT(100),19,2);
        s->names[i]=ui_label(row,"Loading",ECHO_FONT_READ,ECHO_TEXT); lv_obj_set_width(s->names[i],100);
        s->values[i]=ui_label(row,"--",ECHO_FONT_READ,ECHO_TEXT);
    }
    s->local=ui_label(root,"Board sensor: --",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    s->status=ui_label(root,"HA: loading",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    return s;
}
void ui_temperatures_update(void *state,const ui_model_t *m) {
    temperatures_t *s=state; int stale=0;
    for (int i=0;i<m->ha.count;++i) {
        const ha_sensor_t *v=&m->ha.sensors[i]; ui_text(s->names[i],v->name); ui_metric(s->values[i],v,m->now_us,1);
        if (!ha_fresh(v,m->now_us) || v->result!=HA_VALID) ++stale;
    }
    char text[64];
    bool fresh=m->have_sensors && sensors_group_fresh_at(m->sensors.th_last_us,m->now_us);
    if (fresh) snprintf(text,sizeof(text),"Board: %.1f°C  %.0f%% RH",m->sensors.temp_c,m->sensors.humidity_rh);
    else snprintf(text,sizeof(text),"Board temperature / humidity: stale");
    ui_text(s->local,text);
    lv_obj_set_style_text_color(s->local,lv_color_hex(fresh?ECHO_COOL:ECHO_TEXT_DIM),0);
    snprintf(text,sizeof(text),"%s / %d unavailable or stale",m->ha.connected?"HA connected":"HA offline",stale);
    ui_text(s->status,text);
    lv_obj_set_style_text_color(s->status,lv_color_hex(!m->ha.connected?ECHO_TEXT_DIM:stale?ECHO_AMBER:ECHO_GREEN),0);
}
