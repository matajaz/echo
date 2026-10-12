#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
typedef struct { lv_obj_t *values[3],*ages[3],*status; } energy_now_t;
void *ui_energy_now_create(lv_obj_t *root) {
    energy_now_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;
    const char *names[]={"Grid meter","Solar generation","Electricity price"};
    const uint32_t accents[]={ECHO_COOL,ECHO_SOLAR,ECHO_SOLAR};
    for (int i=0;i<3;++i) {
        lv_obj_t *c=ui_card(root,62),*r=ui_row(c,LV_PCT(100),16,4);
        ui_card_accent(c,accents[i]);
        lv_obj_t *name=ui_label(r,names[i],ECHO_FONT_SMALL,accents[i]);
        lv_obj_set_width(name,132);
        s->ages[i]=ui_label(r,"Loading",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
        lv_obj_set_width(s->ages[i],72); lv_label_set_long_mode(s->ages[i],LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s->ages[i],LV_TEXT_ALIGN_RIGHT,0);
        s->values[i]=ui_label(c,"--",ECHO_FONT_TITLE,ECHO_TEXT);
    }
    s->status=ui_label(root,"Grid and solar are separate measurements",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(s->status,LV_PCT(100)); lv_label_set_long_mode(s->status,LV_LABEL_LONG_WRAP);
    return s;
}
void ui_energy_now_update(void *state,const ui_model_t *m) {
    energy_now_t *s=state;
    const ha_role_t roles[]={HA_ROLE_GRID_POWER,HA_ROLE_SOLAR_POWER,HA_ROLE_PRICE};
    for (int i=0;i<3;++i) {
        const ha_sensor_t *v=ha_find_role(&m->ha,roles[i]); ui_metric(s->values[i],v,m->now_us,i==2?2:0);
        char text[40];
        if (!v) snprintf(text,sizeof(text),"Not configured");
        else if (!ha_fresh(v,m->now_us)) snprintf(text,sizeof(text),"%s",v->ok?"Stale":ha_result_name(v->result));
        else if (v->result!=HA_VALID) snprintf(text,sizeof(text),"%s",ha_result_name(v->result));
        else snprintf(text,sizeof(text),"%lus",(unsigned long)((m->now_us-v->received_us)/1000000));
        ui_text(s->ages[i],text);
    }
    const ha_sensor_t *grid=ha_find_role(&m->ha,HA_ROLE_GRID_POWER), *solar=ha_find_role(&m->ha,HA_ROLE_SOLAR_POWER),
                      *exported=ha_find_role(&m->ha,HA_ROLE_GRID_EXPORT_POWER);
    // Do not fabricate household load: optional storage/export measurements
    // and a configured no-storage assumption are required to derive it.
#ifdef CONFIG_ECHO_NO_STORAGE
    if (ha_fresh(grid,m->now_us) && ha_fresh(solar,m->now_us) && ha_fresh(exported,m->now_us) &&
        grid->result==HA_VALID && solar->result==HA_VALID && exported->result==HA_VALID) {
        uint64_t oldest=grid->received_us<solar->received_us?grid->received_us:solar->received_us;
        uint64_t newest=grid->received_us>solar->received_us?grid->received_us:solar->received_us;
        if (exported->received_us<oldest) oldest=exported->received_us;
        if (exported->received_us>newest) newest=exported->received_us;
        float load=grid->value+solar->value-exported->value;
        if (newest-oldest<=5000000 && load>=0 && isfinite(load)) {
            char text[96]; snprintf(text,sizeof(text),"Solar + grid - export\nHouse load: %.0f W (derived)",load);
            ui_text(s->status,text); return;
        }
    }
#else
    (void)grid; (void)solar; (void)exported;
#endif
    ui_text(s->status,"Grid and solar measured separately.\nHouse load needs export/storage data.");
}
