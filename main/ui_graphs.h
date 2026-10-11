// echo -- consolidated single-graph page.
//
// Instead of five charts scattered across three pages, this page shows ONE
// large graph at a time, chosen from a dropdown:
//   0 Temperature   1 Humidity   2 Accel |a|   3 Gyro |w|   4 Battery SOC
// A tap on the value row cycles the displayed unit (C/F/K, %, ...).
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the graph page as a tileview tile and start its sample timer.
// Takes the LVGL port lock internally; safe to call from ui_init.
void ui_graphs_create(lv_obj_t *tile);

// Stop and release the sample timer + lock (called on teardown if ever needed).
void ui_graphs_destroy(void);

#ifdef __cplusplus
}
#endif
