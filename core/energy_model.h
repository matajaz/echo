#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// Supports quarter-hour prices on a 25-hour DST day without assuming 24 bars.
#define ENERGY_MAX_PRICES 100
#define ENERGY_MAX_HOURS 25
typedef struct { int64_t start, end; float value; } price_point_t;
typedef struct {
    price_point_t points[ENERGY_MAX_PRICES];
    int count;
    bool valid;
} price_day_t;
typedef struct {
    price_day_t today, tomorrow;
    uint64_t received_us;
    bool ok;
} energy_prices_t;
typedef struct {
    int64_t start;
    float imported, exported;
    bool import_ok, export_ok;
    float import_cost, export_income;
    bool import_cost_ok, export_income_ok;
} energy_hour_t;
typedef struct {
    energy_hour_t hours[ENERGY_MAX_HOURS];
    int count;
    int64_t day_start, day_end;
    uint64_t received_us;
    bool ok, cached;
    char error[48];
} energy_history_t;
// Duration-weighted cheapest contiguous interval. Gaps invalidate a window;
// negative prices are legitimate. Does not assume hourly spacing or 24h days.
static inline bool price_cheapest_after(const price_day_t *d, int64_t seconds,
                                        int64_t earliest, int64_t *start, float *average) {
    if (!d || !d->valid || d->count<1 || d->count>ENERGY_MAX_PRICES || seconds <= 0) return false;
    if (earliest<d->points[0].start) earliest=d->points[0].start;
    bool found = false; double best = 0; int64_t chosen=0;
    for (int candidate = 0; candidate <= d->count * 2; ++candidate) {
        // On irregular grids a minimum can also occur when the window END
        // meets an interval boundary, with its start inside another interval.
        int64_t from=candidate==d->count*2?earliest:candidate<d->count?d->points[candidate].start:
            d->points[candidate-d->count].end-seconds;
        if (from<earliest) continue;
        int64_t end = from + seconds, cursor = from;
        double sum = 0;
        for (int j = 0; j < d->count && cursor < end; ++j) {
            const price_point_t *p = &d->points[j];
            if (p->end<=cursor) continue;
            if (p->start>cursor || !isfinite(p->value)) break;
            int64_t until = p->end < end ? p->end : end;
            sum += (double)p->value * (until - cursor); cursor = until;
        }
        if (cursor == end && (!found || sum < best || (sum==best && from<chosen))) {
            found = true; best = sum; chosen=from;
            if (start) *start = from;
            if (average) *average = (float)(sum / seconds);
        }
    }
    return found;
}
static inline bool price_cheapest(const price_day_t *d,int64_t seconds,int64_t *start,float *average) {
    return price_cheapest_after(d,seconds,INT64_MIN,start,average);
}
