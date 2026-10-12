#include "ui_page.h"
#include "ui_widgets.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
typedef struct { lv_obj_t *indoor,*outdoor,*grid,*solar,*price,*imported,*exported,*timer,*status; } overview_t;
static void nav(lv_event_t *e) { ui_navigate((ui_page_id_t)(intptr_t)lv_event_get_user_data(e)); }
static void clean(lv_event_t *e) { (void)e; ui_clean_start(); }
static void timer(lv_event_t *e) { (void)e; ui_countdown_toggle(); }
static lv_obj_t *pair(lv_obj_t *card,const char *caption,lv_obj_t **a,const char *first,lv_obj_t **b,const char *second) {
    (void)caption;
    lv_obj_t *row=ui_row(card,LV_PCT(100),LV_SIZE_CONTENT,10);
    lv_obj_t *left=ui_column(row,98,LV_SIZE_CONTENT,0),*right=ui_column(row,98,LV_SIZE_CONTENT,0);
    const lv_font_t *font=!strcmp(caption,"HOME TEMPERATURE")?ECHO_FONT_TITLE:ECHO_FONT_READ;
    ui_label(left,first,ECHO_FONT_SMALL,ECHO_TEXT_DIM); *a=ui_label(left,"--",font,ECHO_TEXT);
    ui_label(right,second,ECHO_FONT_SMALL,ECHO_TEXT_DIM); *b=ui_label(right,"--",font,ECHO_TEXT);
    return row;
}
void *ui_overview_create(lv_obj_t *root) {
    overview_t *s=ui_state_alloc(sizeof(*s)); if (!s) return NULL;
    lv_obj_t *c=ui_card(root,56); pair(c,"HOME TEMPERATURE",&s->indoor,"Indoor",&s->outdoor,"Outdoor");
    ui_card_accent(c,ECHO_COOL);
    lv_obj_add_flag(c,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(c,nav,LV_EVENT_CLICKED,(void *)UI_TEMPERATURES);
    c=ui_card(root,68); pair(c,"POWER NOW",&s->grid,"Grid",&s->solar,"Solar");
    ui_card_accent(c,ECHO_SOLAR);
    lv_obj_t *sun=ui_label(c,ECHO_SUN_GLYPH,&echo_sun_20,ECHO_SOLAR);
    lv_obj_add_flag(sun,LV_OBJ_FLAG_FLOATING);
    lv_obj_align(sun,LV_ALIGN_TOP_RIGHT,-4,1);
    s->price=ui_label(c,"-- kr/kWh",ECHO_FONT_READ,ECHO_AMBER);
    lv_obj_add_flag(c,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(c,nav,LV_EVENT_CLICKED,(void *)UI_ENERGY_NOW);
    c=ui_card(root,48); pair(c,"GRID ENERGY TODAY",&s->imported,"Import today",&s->exported,"Export today");
    ui_card_accent(c,ECHO_GREEN);
    lv_obj_add_flag(c,LV_OBJ_FLAG_CLICKABLE); lv_obj_add_event_cb(c,nav,LV_EVENT_CLICKED,(void *)UI_ENERGY_TODAY);
    lv_obj_t *row=ui_row(root,LV_PCT(100),32,6);
    ui_button_accent(ui_button(row,"Clean",60,32,clean,NULL),ECHO_ACTION);
    s->timer=ui_button(row,"5m timer",84,32,timer,NULL);
    ui_button_accent(ui_button(row,"Prices",68,32,nav,(void *)UI_PRICES),ECHO_ACTION);
    s->status=ui_label(root,"Connecting to HA",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    return s;
}
void ui_overview_update(void *state,const ui_model_t *m) {
    overview_t *s=state;
    ui_metric(s->indoor,ha_find_role(&m->ha,HA_ROLE_INDOOR),m->now_us,1);
    ui_metric(s->outdoor,ha_find_role(&m->ha,HA_ROLE_OUTDOOR),m->now_us,1);
    ui_metric(s->grid,ha_find_role(&m->ha,HA_ROLE_GRID_POWER),m->now_us,0);
    ui_metric(s->solar,ha_find_role(&m->ha,HA_ROLE_SOLAR_POWER),m->now_us,0);
    ui_metric(s->price,ha_find_role(&m->ha,HA_ROLE_PRICE),m->now_us,2);
    ui_metric(s->imported,ha_find_role(&m->ha,HA_ROLE_GRID_IMPORT_TODAY),m->now_us,2);
    ui_metric(s->exported,ha_find_role(&m->ha,HA_ROLE_GRID_EXPORT_TODAY),m->now_us,2);
    char text[48]; int secs=ui_countdown_seconds();
    if (secs>0) snprintf(text,sizeof(text),"%d:%02d Stop",secs/60,secs%60);
    else snprintf(text,sizeof(text),"%s",ui_countdown_done()?"Done / 5m":"5m timer");
    ui_text(lv_obj_get_child(s->timer,0),text);
    ui_button_accent(s->timer,secs>0?ECHO_ACTION:ECHO_BUTTON);
    int stale=0;
    for (int i=0;i<m->ha.power_count;++i) if (!ha_fresh(&m->ha.power[i],m->now_us) || m->ha.power[i].result!=HA_VALID) ++stale;
    if (m->advisory) ui_text(s->status,m->advisory);
    else if (!m->ha.connected) ui_text(s->status,"HA offline / last values shown");
    else if (stale) { snprintf(text,sizeof(text),"%d energy values unavailable/stale",stale); ui_text(s->status,text); }
    else ui_text(s->status,m->advisory?m->advisory:"HA connected");
    lv_obj_set_style_text_color(s->status,lv_color_hex(m->advisory?ECHO_AMBER:
        !m->ha.connected?ECHO_TEXT_DIM:stale?ECHO_AMBER:ECHO_GREEN),0);
}
