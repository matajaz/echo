#include "ui_page.h"
#include "ui_widgets.h"
#include "dashboard_model.h"
#include "dashboard_lights.h"
#include <stdio.h>
LV_FONT_DECLARE(font_montserrat_14_sv);
typedef struct { int id;lv_obj_t *body,*age,*wan_body;dashboard_section_t snapshot;uint64_t rendered;bool fresh; } dashboard_ui_t;
static void light_pressed(lv_event_t *e) {
 int command=(int)(intptr_t)lv_event_get_user_data(e);
 ui_feedback(ha_light_request(command/2,command%2)?"Light command queued":"Unavailable / command pending");
}
static void *create(lv_obj_t *root,int id) {
 dashboard_ui_t *s=ui_state_alloc(sizeof(*s));if(!s)return NULL;s->id=id;
 lv_obj_t *header=ui_card(root,34);ui_card_accent(header,ECHO_BLUE);
 s->age=ui_label(header,"Waiting for data",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
 lv_obj_t *card=ui_card(root,210);
 lv_obj_t *scroll=ui_column(card,206,192,4);lv_obj_add_flag(scroll,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_scroll_dir(scroll,LV_DIR_VER);
 s->body=ui_label(scroll,"Waiting for Home Assistant / collector",&font_montserrat_14_sv,ECHO_TEXT);lv_obj_set_width(s->body,196);lv_label_set_long_mode(s->body,LV_LABEL_LONG_WRAP);
 if(id==DASH_NETWORK){s->wan_body=ui_label(scroll,"WAN health unavailable",&font_montserrat_14_sv,ECHO_TEXT_DIM);lv_obj_set_width(s->wan_body,196);lv_label_set_long_mode(s->wan_body,LV_LABEL_LONG_WRAP);}
 if(id==DASH_LIGHTS)for(int i=0;i<9;i++) {
    ui_label(scroll,dashboard_light_names[i],ECHO_FONT_SMALL,ECHO_TEXT);
    lv_obj_t *row=ui_row(scroll,196,32,6);
    ui_button(row,"On",90,30,light_pressed,(void*)(intptr_t)(i*2+1));ui_button(row,"Off",90,30,light_pressed,(void*)(intptr_t)(i*2));
 }
 return s;
}
void ui_dashboard_update(void *state,const ui_model_t *m) {
 dashboard_ui_t *s=state;if(!ha_dashboard_get(s->id,&s->snapshot))return;
 bool fresh=dashboard_fresh(&s->snapshot,m->now_us,m->wall_time);
 if(s->id==DASH_TRANSIT && ((m->now_us-s->snapshot.received_us)>120000000ULL || (s->snapshot.observed_at && m->wall_time-s->snapshot.observed_at>120)))fresh=false;
 if(s->rendered!=s->snapshot.received_us||s->fresh!=fresh){s->rendered=s->snapshot.received_us;s->fresh=fresh;ui_text(s->body,*s->snapshot.text?s->snapshot.text:"Data unavailable");lv_obj_set_style_text_color(s->body,lv_color_hex(fresh?ECHO_TEXT:ECHO_TEXT_DIM),0);}
 char age[64];if(!s->snapshot.received_us)snprintf(age,sizeof(age),"Waiting for data");else snprintf(age,sizeof(age),"%s / received %llu min ago",fresh?"Current":"STALE / unavailable",(unsigned long long)((m->now_us-s->snapshot.received_us)/60000000));ui_text(s->age,age);
 if(s->wan_body&&ha_dashboard_get(DASH_WAN,&s->snapshot)){bool wan_fresh=dashboard_fresh(&s->snapshot,m->now_us,m->wall_time);lv_obj_set_style_text_color(s->wan_body,lv_color_hex(wan_fresh?ECHO_TEXT:ECHO_TEXT_DIM),0);static char text[DASH_TEXT_CAP+32];snprintf(text,sizeof(text),"WAN %s\n%s",wan_fresh?"current":"STALE / unavailable",s->snapshot.text);ui_text(s->wan_body,text);}
}
void *ui_dash_status_create(lv_obj_t *root){return create(root,DASH_STATUS);}
void *ui_dash_weather_create(lv_obj_t *root){return create(root,DASH_WEATHER);}
void *ui_dash_weather_day_create(lv_obj_t *root){return create(root,DASH_WEATHER_DAY);}
void *ui_dash_sun_create(lv_obj_t *root){return create(root,DASH_SUN);}
void *ui_dash_network_create(lv_obj_t *root){return create(root,DASH_NETWORK);}
void *ui_dash_markets_create(lv_obj_t *root){return create(root,DASH_MARKETS);}
void *ui_dash_air_create(lv_obj_t *root){return create(root,DASH_AIR);}
void *ui_dash_services_create(lv_obj_t *root){return create(root,DASH_SERVICES);}
void *ui_dash_news_create(lv_obj_t *root){return create(root,DASH_NEWS);}
void *ui_dash_police_create(lv_obj_t *root){return create(root,DASH_POLICE);}
void *ui_dash_transit_create(lv_obj_t *root){return create(root,DASH_TRANSIT);}
void *ui_dash_lights_create(lv_obj_t *root){return create(root,DASH_LIGHTS);}
void *ui_dash_humidity_create(lv_obj_t *root){return create(root,DASH_HUMIDITY);}
