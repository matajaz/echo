#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#define CHART_HISTORY_POINTS 60
typedef struct { uint64_t slot; double sum; float min,max; uint32_t count; } chart_bucket_t;
typedef struct { chart_bucket_t buckets[CHART_HISTORY_POINTS]; } chart_history_t;
typedef struct { bool valid; float min,max,average; uint64_t count; } chart_stats_t;
static inline void chart_history_add(chart_history_t *h,uint64_t slot,float value) {
    if (!isfinite(value)) return;
    chart_bucket_t *b=&h->buckets[slot%CHART_HISTORY_POINTS];
    if (b->slot!=slot+1 || !b->count) *b=(chart_bucket_t){.slot=slot+1,.min=value,.max=value};
    b->sum+=value; ++b->count;
    if (value<b->min) b->min=value;
    if (value>b->max) b->max=value;
}
static inline chart_stats_t chart_history_stats(const chart_history_t *h,uint64_t newest) {
    chart_stats_t s={0}; double sum=0;
    for (int i=0;i<CHART_HISTORY_POINTS;++i) {
        const chart_bucket_t *b=&h->buckets[i];
        if (!b->count || !b->slot || b->slot-1>newest || newest-(b->slot-1)>=CHART_HISTORY_POINTS) continue;
        if (!s.valid) { s.valid=true; s.min=b->min; s.max=b->max; }
        if (b->min<s.min) s.min=b->min;
        if (b->max>s.max) s.max=b->max;
        sum+=b->sum; s.count+=b->count;
    }
    if (s.count) s.average=(float)(sum/s.count);
    return s;
}
