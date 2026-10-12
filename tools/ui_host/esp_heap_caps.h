#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
static inline void *heap_caps_calloc(size_t n,size_t size,unsigned caps) { (void)caps; return calloc(n,size); }
static inline size_t heap_caps_get_free_size(unsigned caps) { return caps==1?8000000:40000; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 28000; }
