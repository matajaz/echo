#pragma once
#include "ui/ui_page.h"
// Collection runs regardless of page visibility; presentation updates only
// while active. Call all lifecycle functions under the LVGL port lock.
void *ui_graphs_create(lv_obj_t *root);
void ui_graphs_update(void *state,const ui_model_t *model);
void ui_graphs_destroy(void *state);
