#pragma once
#include "lvgl.h"
#include "ha_model.h"
#include "echo_theme.h"
void ui_theme_init(void);
void *ui_state_alloc(size_t size);
void ui_state_free(void *state);
typedef struct { lv_obj_t *title,*clock,*wifi,*battery; } ui_header_t;
ui_header_t ui_header(lv_obj_t *parent,const char *title);
void ui_page_style(lv_obj_t *root);
lv_obj_t *ui_column(lv_obj_t *parent,int width,int height,int gap);
lv_obj_t *ui_row(lv_obj_t *parent,int width,int height,int gap);
lv_obj_t *ui_card(lv_obj_t *parent,int height);
void ui_card_accent(lv_obj_t *card,uint32_t color);
lv_obj_t *ui_label(lv_obj_t *parent,const char *text,const lv_font_t *font,uint32_t color);
lv_obj_t *ui_button(lv_obj_t *parent,const char *text,int width,int height,lv_event_cb_t cb,void *data);
void ui_button_accent(lv_obj_t *button,uint32_t color);
void ui_text(lv_obj_t *label,const char *text);
void ui_metric(lv_obj_t *label,const ha_sensor_t *sensor,uint64_t now,int decimals);
void ui_metric_color(lv_obj_t *label,const ha_sensor_t *sensor,uint64_t now,int decimals,uint32_t color);
void ui_number(lv_obj_t *label,float value,const char *unit,int decimals);
lv_obj_t *ui_chart(lv_obj_t *parent,int height,lv_chart_type_t type);
lv_obj_t *ui_dropdown(lv_obj_t *parent,const char *options);
void ui_chart_scale(lv_event_t *event);
