// lv_psram_alloc.h / .c pair — PSRAM-backed allocator for LVGL.
//
// WHY: the fixed internal LVGL pool (40 KB) chronically ran full with the
// redesigned card/list pages — under swipe churn + playback, lv_mem_buf_get
// spun (LVGL task watchdog hang). Growing the internal pool starves WiFi
// (internal RAM is shared); LV_MEM_CUSTOM with plain malloc did the same.
// Routing LVGL allocations to the 8 MB PSRAM removes the ceiling AND leaves
// internal RAM for WiFi/tasks. LVGL objects are not DMA'd, so PSRAM is fine.
//
// Wired via: CONFIG_LV_MEM_CUSTOM=y, LV_MEM_CUSTOM_INCLUDE="lv_psram_alloc.h",
// and compile-defs LV_MEM_CUSTOM_ALLOC/FREE/REALLOC (main/CMakeLists.txt).
#pragma once
#include <stddef.h>

void *echo_lv_malloc(size_t size);
void  echo_lv_free(void *p);
void *echo_lv_realloc(void *p, size_t size);
