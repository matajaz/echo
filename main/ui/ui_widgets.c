#include "ui_widgets.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static lv_style_t page,card,text,button;
void *ui_state_alloc(size_t size) {
    void *state=lv_mem_alloc(size);
    if (state) memset(state,0,size);
    return state;
}
void ui_state_free(void *state) { lv_mem_free(state); }
void ui_theme_init(void) {
    lv_style_init(&page); lv_style_set_bg_color(&page,lv_color_hex(ECHO_BG));
    lv_style_set_bg_opa(&page,LV_OPA_COVER); lv_style_set_border_width(&page,0);
    lv_style_set_pad_all(&page,8); lv_style_set_pad_row(&page,4);
    lv_style_init(&card); lv_style_set_bg_color(&card,lv_color_hex(ECHO_CARD_TOP));
    lv_style_set_bg_grad_color(&card,lv_color_hex(ECHO_CARD)); lv_style_set_bg_grad_dir(&card,LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&card,LV_OPA_COVER); lv_style_set_border_width(&card,1);
    lv_style_set_border_color(&card,lv_color_hex(ECHO_EDGE));
    // Pronounced raised 3D look: a wider, softer drop shadow offset down so the
    // card reads as lifted off the dark background (was width 6 / ofs 2 / opa 20).
    lv_style_set_shadow_color(&card,lv_color_black()); lv_style_set_shadow_width(&card,16);
    lv_style_set_shadow_ofs_y(&card,5); lv_style_set_shadow_spread(&card,1);
    lv_style_set_shadow_opa(&card,LV_OPA_50);
    lv_style_set_radius(&card,8); lv_style_set_pad_all(&card,8); lv_style_set_pad_row(&card,4);
    lv_style_init(&text); lv_style_set_text_color(&text,lv_color_hex(ECHO_TEXT));
    lv_style_set_text_font(&text,ECHO_FONT_READ);
    lv_style_init(&button); lv_style_set_bg_color(&button,lv_color_hex(ECHO_BUTTON));
    lv_style_set_bg_grad_color(&button,lv_color_hex(ECHO_CARD)); lv_style_set_bg_grad_dir(&button,LV_GRAD_DIR_VER);
    lv_style_set_border_width(&button,1); lv_style_set_border_color(&button,lv_color_hex(ECHO_EDGE));
    lv_style_set_radius(&button,6);
    // Subtle raised shadow so buttons read as 3D/clickable (matches the cards).
    lv_style_set_shadow_color(&button,lv_color_black()); lv_style_set_shadow_width(&button,8);
    lv_style_set_shadow_ofs_y(&button,3); lv_style_set_shadow_opa(&button,LV_OPA_40);
    lv_style_set_pad_all(&button,0); lv_style_set_text_font(&button,ECHO_FONT_SMALL);
}
void ui_page_style(lv_obj_t *root) {
    // Tile geometry is itself stored in local styles. Removing every style
    // here would erase the tileview's size/position and stack all pages at 0,0.
    lv_obj_add_style(root,&page,0);
    lv_obj_set_flex_flow(root,LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root,LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *container(lv_obj_t *p,int w,int h,int gap,lv_flex_flow_t flow) {
    lv_obj_t *o=lv_obj_create(p); lv_obj_remove_style_all(o); lv_obj_set_size(o,w,h);
    lv_obj_set_flex_flow(o,flow); lv_obj_set_style_pad_row(o,gap,0); lv_obj_set_style_pad_column(o,gap,0);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE); return o;
}
lv_obj_t *ui_column(lv_obj_t *p,int w,int h,int gap) { return container(p,w,h,gap,LV_FLEX_FLOW_COLUMN); }
lv_obj_t *ui_row(lv_obj_t *p,int w,int h,int gap) { return container(p,w,h,gap,LV_FLEX_FLOW_ROW); }
lv_obj_t *ui_card(lv_obj_t *p,int h) {
    lv_obj_t *o=lv_obj_create(p); lv_obj_remove_style_all(o); lv_obj_add_style(o,&card,0);
    lv_obj_set_size(o,LV_PCT(100),h); lv_obj_set_flex_flow(o,LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE); return o;
}
void ui_card_accent(lv_obj_t *o,uint32_t color) {
    lv_obj_set_style_border_color(o,lv_color_darken(lv_color_hex(color),LV_OPA_40),0);
}
lv_obj_t *ui_label(lv_obj_t *p,const char *s,const lv_font_t *f,uint32_t col) {
    lv_obj_t *o=lv_label_create(p); lv_obj_add_style(o,&text,0);
    lv_obj_set_style_text_font(o,f,0); lv_obj_set_style_text_color(o,lv_color_hex(col),0);
    lv_label_set_text(o,s); return o;
}
lv_obj_t *ui_button(lv_obj_t *p,const char *s,int w,int h,lv_event_cb_t cb,void *data) {
    lv_obj_t *o=lv_btn_create(p); lv_obj_add_style(o,&button,0); lv_obj_set_size(o,w,h);
    if (cb) lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);
    lv_obj_t *l=ui_label(o,s,ECHO_FONT_SMALL,ECHO_TEXT); lv_obj_center(l); return o;
}
void ui_button_accent(lv_obj_t *o,uint32_t color) {
    lv_color_t c=lv_color_hex(color);
    lv_obj_set_style_bg_color(o,c,0);
    lv_obj_set_style_bg_grad_color(o,lv_color_darken(c,LV_OPA_30),0);
    lv_obj_set_style_border_color(o,lv_color_lighten(c,LV_OPA_20),0);
    lv_obj_set_style_bg_color(o,lv_color_darken(c,LV_OPA_20),LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(o,lv_color_darken(c,LV_OPA_40),LV_STATE_PRESSED);
}
ui_header_t ui_header(lv_obj_t *parent,const char *title) {
    ui_header_t h;
    h.title=ui_label(parent,title,ECHO_FONT_READ,ECHO_COOL);
    lv_obj_align(h.title,LV_ALIGN_TOP_LEFT,8,6);
    h.clock=ui_label(parent,"--:--",ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(h.clock,42); lv_obj_set_style_text_align(h.clock,LV_TEXT_ALIGN_RIGHT,0);
    lv_obj_align(h.clock,LV_ALIGN_TOP_RIGHT,-56,6);
    h.wifi=ui_label(parent,LV_SYMBOL_WIFI,ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(h.wifi,18); lv_obj_set_style_text_align(h.wifi,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(h.wifi,LV_ALIGN_TOP_RIGHT,-34,6);
    h.battery=ui_label(parent,LV_SYMBOL_BATTERY_EMPTY,ECHO_FONT_SMALL,ECHO_TEXT_DIM);
    lv_obj_set_width(h.battery,22); lv_obj_set_style_text_align(h.battery,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(h.battery,LV_ALIGN_TOP_RIGHT,-8,6);
    return h;
}
void ui_text(lv_obj_t *l,const char *s) {
    if (l && strcmp(lv_label_get_text(l),s)) lv_label_set_text(l,s);
}
void ui_number(lv_obj_t *l,float v,const char *u,int decimals) {
    char buf[48]; if (isfinite(v)) snprintf(buf,sizeof(buf),"%.*f%s%s",decimals,v,*u?" ":"",u);
    else snprintf(buf,sizeof(buf),"--%s%s",*u?" ":"",u);
    ui_text(l,buf);
}
void ui_metric(lv_obj_t *l,const ha_sensor_t *s,uint64_t now,int decimals) {
    uint32_t color=ECHO_TEXT;
    if (s) switch (s->role) {
        case HA_ROLE_OUTDOOR:
        case HA_ROLE_GRID_IMPORT_TODAY: color=ECHO_COOL; break;
        case HA_ROLE_SOLAR_POWER:
        case HA_ROLE_SOLAR_TODAY:
        case HA_ROLE_PRICE: color=ECHO_SOLAR; break;
        case HA_ROLE_GRID_EXPORT_POWER:
        case HA_ROLE_GRID_EXPORT_TODAY: color=ECHO_GREEN; break;
        default: break;
    }
    ui_metric_color(l,s,now,decimals,color);
}
void ui_metric_color(lv_obj_t *l,const ha_sensor_t *s,uint64_t now,int decimals,uint32_t color) {
    if (!s) { ui_text(l,"--"); lv_obj_set_style_text_color(l,lv_color_hex(ECHO_TEXT_DIM),0); return; }
    const char *u=!strcmp(s->unit,"SEK/kWh")?"kr/kWh":!strcmp(s->unit,"C")?"°C":s->unit;
    ui_number(l,s->ok?s->value:NAN,u,decimals);
    bool live=ha_fresh(s,now) && s->result==HA_VALID;
    lv_obj_set_style_text_color(l,lv_color_hex(live?color:ECHO_TEXT_DIM),0);
}
void ui_chart_scale(lv_event_t *e) {
    lv_obj_draw_part_dsc_t *d=lv_event_get_draw_part_dsc(e);
    if (!d || !d->text || d->part!=LV_PART_TICKS || d->id!=LV_CHART_AXIS_PRIMARY_Y) return;
    // All compact charts use tenths; format back into the physical unit.
    int v=d->value, mag=v<0?-v:v;
    lv_snprintf(d->text,d->text_length,"%s%d.%d",v<0?"-":"",mag/10,mag%10);
}
lv_obj_t *ui_dropdown(lv_obj_t *parent,const char *options) {
    lv_obj_t *dropdown=lv_dropdown_create(parent);
    lv_dropdown_set_options(dropdown,options);
    lv_obj_set_size(dropdown,224,30);
    lv_obj_set_style_bg_color(dropdown,lv_color_hex(ECHO_BUTTON),0);
    lv_obj_set_style_bg_grad_color(dropdown,lv_color_hex(ECHO_CARD),0);
    lv_obj_set_style_bg_grad_dir(dropdown,LV_GRAD_DIR_VER,0);
    lv_obj_set_style_text_color(dropdown,lv_color_hex(ECHO_TEXT),0);
    lv_obj_set_style_text_font(dropdown,ECHO_FONT_READ,0);
    lv_obj_set_style_border_width(dropdown,1,0);
    lv_obj_set_style_border_color(dropdown,lv_color_hex(ECHO_EDGE),0);
    lv_obj_t *list=lv_dropdown_get_list(dropdown);
    if (list) {
        lv_obj_set_style_bg_color(list,lv_color_hex(ECHO_CARD),0);
        lv_obj_set_style_text_color(list,lv_color_hex(ECHO_TEXT),0);
        lv_obj_set_style_text_font(list,ECHO_FONT_READ,0);
    }
    return dropdown;
}
lv_obj_t *ui_chart(lv_obj_t *p,int h,lv_chart_type_t type) {
    lv_obj_t *wrap=ui_row(p,LV_PCT(100),h,0);
    lv_obj_set_style_pad_left(wrap,30,0); lv_obj_set_style_pad_bottom(wrap,16,0);
    lv_obj_t *c=lv_chart_create(wrap); lv_obj_set_size(c,LV_PCT(100),LV_PCT(100));
    lv_obj_set_style_bg_color(c,lv_color_hex(0x101A2E),0);
    lv_obj_set_style_border_width(c,1,0); lv_obj_set_style_border_color(c,lv_color_hex(ECHO_EDGE),0);
    lv_obj_set_style_line_color(c,lv_color_hex(0x2C405E),0);
    lv_obj_set_style_line_width(c,2,LV_PART_ITEMS);
    if (type==LV_CHART_TYPE_LINE) {
        lv_obj_set_style_bg_opa(c,LV_OPA_20,LV_PART_ITEMS);
        lv_obj_set_style_bg_grad_dir(c,LV_GRAD_DIR_VER,LV_PART_ITEMS);
        lv_obj_set_style_bg_grad_color(c,lv_color_hex(ECHO_BG),LV_PART_ITEMS);
    }
    lv_obj_set_style_text_font(c,ECHO_FONT_SMALL,0); lv_obj_set_style_text_color(c,lv_color_hex(ECHO_TEXT_DIM),0);
    lv_chart_set_type(c,type); lv_chart_set_div_line_count(c,4,4);
    lv_chart_set_axis_tick(c,LV_CHART_AXIS_PRIMARY_Y,4,2,4,1,true,30);
    lv_chart_set_axis_tick(c,LV_CHART_AXIS_PRIMARY_X,0,0,3,1,true,16);
    lv_obj_add_event_cb(c,ui_chart_scale,LV_EVENT_DRAW_PART_BEGIN,NULL);
    lv_obj_clear_flag(c,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE); return c;
}
