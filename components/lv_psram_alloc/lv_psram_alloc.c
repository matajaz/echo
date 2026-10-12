// PSRAM-backed allocator for LVGL (see lv_psram_alloc.h).
#include "lv_psram_alloc.h"
#include "esp_heap_caps.h"

// Prefer PSRAM; fall back to default (internal) only if PSRAM is exhausted so
// a transient PSRAM pressure never hard-fails an allocation.
void *echo_lv_malloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    return p;
}

void echo_lv_free(void *p) {
    heap_caps_free(p);
}

void *echo_lv_realloc(void *p, size_t size) {
    void *n = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n && size) n = heap_caps_realloc(p, size, MALLOC_CAP_DEFAULT);
    return n;
}
