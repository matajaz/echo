#pragma once
#include "chart_history.h"
#include <string.h>
#include <stddef.h>
#define HISTORY_CACHE_METRICS 5
#define HISTORY_CACHE_MAGIC 0x48495331u
// Compact persisted records; epoch slots preserve outages across reboots.
typedef struct { uint32_t slot,count; float average,min,max; } history_record_t;
typedef struct { uint32_t magic,version; history_record_t records[HISTORY_CACHE_METRICS][CHART_HISTORY_POINTS]; uint32_t crc; } history_cache_t;
static inline uint32_t history_crc(const void *data,size_t size) {
    const uint8_t *p=data; uint32_t crc=~0u;
    while (size--) { crc^=*p++; for (int i=0;i<8;i++) crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return ~crc;
}
static inline bool history_cache_valid(const history_cache_t *b,uint64_t now) {
    if (b->magic!=HISTORY_CACHE_MAGIC || b->version!=1 || b->crc!=history_crc(b,offsetof(history_cache_t,crc))) return false;
    for (int m=0;m<HISTORY_CACHE_METRICS;m++) for (int i=0;i<CHART_HISTORY_POINTS;i++) {
        const history_record_t *r=&b->records[m][i];
        if (!r->count) continue;
        if (!r->slot || r->slot-1>now || (r->slot-1)%CHART_HISTORY_POINTS!=(unsigned)i || r->count>2000 ||
            !isfinite(r->average) || !isfinite(r->min) || !isfinite(r->max) || r->min>r->max || r->average<r->min || r->average>r->max) return false;
    }
    return true;
}
