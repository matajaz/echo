#include "ha_parse.h"
#include "time_utils.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static const cJSON *field(const cJSON *o, const char *k) { return cJSON_GetObjectItemCaseSensitive(o,k); }
static bool parse_day(const cJSON *array, const char *unit, price_day_t *out) {
    *out = (price_day_t){0};
    if (!cJSON_IsArray(array)) return false;
    int count = cJSON_GetArraySize(array);
    if (count < 1 || count > ENERGY_MAX_PRICES) return false;
    const cJSON *point;
    cJSON_ArrayForEach(point, array) {
        const cJSON *start=field(point,"start"), *end=field(point,"end"), *v=field(point,"value");
        price_point_t p;
        if (!cJSON_IsString(start) || !cJSON_IsString(end) || !cJSON_IsNumber(v) ||
            !iso_epoch(start->valuestring,&p.start) || !iso_epoch(end->valuestring,&p.end) ||
            p.end <= p.start || !ha_normalize((float)v->valuedouble,unit,"SEK/kWh",&p.value)) return false;
        if (out->count && out->points[out->count-1].end != p.start) return false;
        out->points[out->count++] = p;
    }
    out->valid = true; return true;
}
ha_result_t ha_parse_state(const char *json, const char *want, float *value, energy_prices_t *prices) {
    if (prices) *prices = (energy_prices_t){0};
    cJSON *root = cJSON_Parse(json);
    ha_result_t result = HA_INVALID;
    if (!root || !cJSON_IsObject(root)) goto done;
    const cJSON *s = field(root,"state"), *attrs = field(root,"attributes");
    const cJSON *u = field(attrs,"unit_of_measurement");
    if (!cJSON_IsString(s) || !cJSON_IsString(u)) goto done;
    char *tail; float raw = strtof(s->valuestring,&tail);
    while (*tail == ' ' || *tail == '\t') ++tail;
    if (tail == s->valuestring || *tail || !isfinite(raw)) goto done;
    if (!ha_normalize(raw,u->valuestring,want,value)) { result = HA_UNIT_ERROR; goto done; }
    result = HA_VALID;
    if (prices) {
        parse_day(field(attrs,"raw_today"),u->valuestring,&prices->today);
        if (cJSON_IsTrue(field(attrs,"tomorrow_valid")))
            parse_day(field(attrs,"raw_tomorrow"),u->valuestring,&prices->tomorrow);
        prices->ok = prices->today.valid;
    }
done:
    cJSON_Delete(root); return result;
}

static bool statistics_series(const cJSON *result, const char *id, int kind, energy_history_t *h) {
    const cJSON *array=field(result,id), *row;
    if (!cJSON_IsArray(array)) return false;
    bool any=false;
    cJSON_ArrayForEach(row,array) {
        const cJSON *start=field(row,"start"), *change=field(row,"change");
        if (!cJSON_IsNumber(start) || !cJSON_IsNumber(change) || !isfinite(change->valuedouble) ||
            (kind<2 && change->valuedouble < 0)) continue;
        double raw = start->valuedouble;
        // HA recorder returns epoch milliseconds; tolerate seconds for tests/older servers.
        if (raw > 1e11) raw /= 1000;
        if (!isfinite(raw) || raw < h->day_start || raw >= h->day_end) continue;
        int64_t t=(int64_t)raw;
        if ((t-h->day_start)%3600) continue;
        int index=(int)((t-h->day_start)/3600);
        if (index < 0 || index >= h->count) continue;
        float v=(float)change->valuedouble;
        if (!isfinite(v)) continue;
        if (kind==0) { h->hours[index].imported=v; h->hours[index].import_ok=true; }
        else if (kind==1) { h->hours[index].exported=v; h->hours[index].export_ok=true; }
        else if (kind==2) { h->hours[index].import_cost=v; h->hours[index].import_cost_ok=true; }
        else { h->hours[index].export_income=v; h->hours[index].export_income_ok=true; }
        any=true;
    }
    return any;
}
bool ha_parse_statistics(const char *json, const char *import_id, const char *export_id, energy_history_t *h) {
    cJSON *root=cJSON_Parse(json);
    bool ok=false;
    if (!root || !cJSON_IsTrue(field(root,"success"))) goto done;
    const cJSON *result=field(root,"result");
    bool from=statistics_series(result,import_id,0,h);
    bool to=statistics_series(result,export_id,1,h);
    ok=from || to;
done:
    cJSON_Delete(root); return ok;
}

bool ha_parse_cost_statistics(const char *json,const char *cost_id,const char *income_id,energy_history_t *h) {
    cJSON *root=cJSON_Parse(json); bool ok=false;
    if (root && cJSON_IsTrue(field(root,"success"))) {
        const cJSON *result=field(root,"result");
        bool cost=cost_id && statistics_series(result,cost_id,2,h);
        bool income=income_id && statistics_series(result,income_id,3,h);
        ok=cost || income;
    }
    cJSON_Delete(root); return ok;
}
