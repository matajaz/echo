#pragma once
#include "energy_model.h"
// Use measured monetary changes first. Energy * interval-average tariff is an
// estimate (uniform consumption within the hour), not metered quarter-hour cost.
typedef enum { COST_MISSING, COST_RECORDED, COST_ESTIMATED } cost_source_t;
static inline bool cost_interval_price(const price_day_t *day,int64_t from,int64_t until,float *out) {
    if (!day || !day->valid || until<=from || day->count<1 || day->count>ENERGY_MAX_PRICES) return false;
    int64_t cursor=from; double sum=0;
    for (int i=0;i<day->count && cursor<until;i++) {
        const price_point_t *p=&day->points[i];
        if (p->end<=cursor) continue;
        if (p->start>cursor || !isfinite(p->value) || p->end<=p->start) return false;
        int64_t end=p->end<until?p->end:until;
        sum+=(double)p->value*(end-cursor); cursor=end;
    }
    if (cursor!=until) return false;
    *out=sum/(until-from); return isfinite(*out);
}
static inline cost_source_t cost_hour(const energy_hour_t *h,const price_day_t *day,float *out) {
    if (h->import_cost_ok && isfinite(h->import_cost)) { *out=h->import_cost; return COST_RECORDED; }
    float price;
    if (!h->import_ok || !isfinite(h->imported) || h->imported<0 || !cost_interval_price(day,h->start,h->start+3600,&price)) return COST_MISSING;
    *out=h->imported*price; return isfinite(*out)?COST_ESTIMATED:COST_MISSING;
}
