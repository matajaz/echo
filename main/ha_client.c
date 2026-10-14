#include "ha_client.h"
#include "ha_entities.generated.h"
#include "ha_parse.h"
#include "ha_energy.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "secrets.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define HA_MAX_BODY 32768
#define COUNT(a) ((int)(sizeof(a)/sizeof((a)[0])))
static const char *TAG="ha_client";
static ha_data_t s_data;
static energy_prices_t *s_prices, *s_price_stage;
static energy_history_t s_history;
static SemaphoreHandle_t s_mtx;
static bool s_started;
static bool s_pause_req;
static uint32_t s_pause_gen, s_ack_gen;
static int s_day_offset;
static uint32_t s_history_request;
static char *s_body;
typedef struct { char *buf; size_t len; bool overflow; } http_body_t;
static void *json_alloc(size_t n) { return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
void ha_json_init(void) {
    // Startup calls this before registering HTTP handlers; allocation hooks must
    // not change while requests or HA parsing are using cJSON.
    static bool initialized;
    if (!initialized) { cJSON_Hooks hooks={.malloc_fn=json_alloc,.free_fn=free}; cJSON_InitHooks(&hooks); initialized=true; }
}
static bool paused(void) { return __atomic_load_n(&s_pause_req,__ATOMIC_ACQUIRE); }
static esp_err_t http_event(esp_http_client_event_t *e) {
    http_body_t *b=e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b && e->data_len > 0) {
        if (b->overflow || (size_t)e->data_len >= HA_MAX_BODY-b->len) b->overflow=true;
        else { memcpy(b->buf+b->len,e->data,e->data_len); b->len+=e->data_len; b->buf[b->len]=0; }
    }
    return ESP_OK;
}
static void init_entries(ha_sensor_t *dest,const ha_entity_def_t *defs,int count,uint32_t stale) {
    for (int i=0;i<count;++i) {
        strlcpy(dest[i].entity_id,defs[i].entity_id,sizeof(dest[i].entity_id));
        strlcpy(dest[i].name,defs[i].name,sizeof(dest[i].name));
        strlcpy(dest[i].unit,defs[i].unit,sizeof(dest[i].unit));
        dest[i].role=defs[i].role; dest[i].value=NAN; dest[i].stale_ms=stale;
        dest[i].result=HA_LOADING;
    }
}
static unsigned cadence(const ha_sensor_t *s,int group) {
    if (s->role==HA_ROLE_PRICE) return 300000;
    return group==0 ? 10000 : group==1 ? 30000 : 300000;
}
static void fetch(esp_http_client_handle_t client,http_body_t *b,ha_sensor_t *entry) {
    char url[256];
    int n=snprintf(url,sizeof(url),"%s/api/states/%s",HA_BASE_URL,entry->entity_id);
    if (n<0 || (size_t)n>=sizeof(url)) return;
    b->len=0; b->overflow=false; b->buf[0]=0;
    esp_err_t err=esp_http_client_set_url(client,url);
    if (err==ESP_OK) err=esp_http_client_perform(client);
    int code=err==ESP_OK ? esp_http_client_get_status_code(client) : -1;
    float value=NAN;
    bool price=entry->role==HA_ROLE_PRICE;
    ha_result_t result=err!=ESP_OK ? HA_OFFLINE : code!=200 ? HA_HTTP_ERROR :
        b->overflow ? HA_OVERFLOW : ha_parse_state(b->buf,entry->unit,&value,price?s_price_stage:NULL);
    uint64_t now=esp_timer_get_time();
    xSemaphoreTake(s_mtx,portMAX_DELAY);
    entry->checked_us=now; entry->http_code=code; entry->result=result;
    s_data.last_http_code=code;
    if (code==200) s_data.last_response_us=now;
    if (result==HA_VALID) {
        entry->value=value; entry->ok=true; entry->received_us=now;
        if (price) { s_price_stage->received_us=now; *s_prices=*s_price_stage; }
    }
    // Keep last-good data on failure, with explicit result/age for the UI.
    xSemaphoreGive(s_mtx);
    if (result!=HA_VALID) ESP_LOGW(TAG,"%s: %s (http=%d bytes=%u)",entry->entity_id,
        ha_result_name(result),code,(unsigned)b->len);
}
static void poll_task(void *arg) {
    (void)arg;
    http_body_t body={.buf=s_body};
    uint64_t last_history=0; uint32_t request_seen=UINT32_MAX;
    for (;;) {
        if (paused()) {
            while (paused()) {
                __atomic_store_n(&s_ack_gen,__atomic_load_n(&s_pause_gen,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        wifi_ap_record_t ap;
        bool wifi=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
        xSemaphoreTake(s_mtx,portMAX_DELAY); s_data.wifi_up=wifi; xSemaphoreGive(s_mtx);
        bool due=false; uint64_t tick_now=esp_timer_get_time();
        for (int group=0;group<3;++group) {
            const ha_sensor_t *entries=group==0?s_data.power:group==1?s_data.sensors:s_data.batteries;
            int count=group==0?s_data.power_count:group==1?s_data.count:s_data.batt_count;
            for (int i=0;i<count;++i) if (!entries[i].checked_us || tick_now-entries[i].checked_us >= (uint64_t)cadence(&entries[i],group)*1000) due=true;
        }
        if (wifi && due && !paused()) {
            esp_http_client_config_t cfg={.url=HA_BASE_URL,.timeout_ms=1500,.event_handler=http_event,
                .user_data=&body,.disable_auto_redirect=true,.keep_alive_enable=true,
                .crt_bundle_attach=esp_crt_bundle_attach};
            esp_http_client_handle_t client=esp_http_client_init(&cfg);
            if (client) {
                char auth[sizeof(HA_TOKEN)+8];
                snprintf(auth,sizeof(auth),"Bearer %s",HA_TOKEN);
                esp_http_client_set_header(client,"Authorization",auth);
                // Live power first; publish each entity immediately. Temperature
                // and battery polling cannot delay visibility until a whole cycle ends.
                for (int group=0;group<3 && !paused();++group) {
                    ha_sensor_t *entries=group==0?s_data.power:group==1?s_data.sensors:s_data.batteries;
                    int count=group==0?s_data.power_count:group==1?s_data.count:s_data.batt_count;
                    for (int i=0;i<count && !paused();++i) {
                        uint64_t now=esp_timer_get_time();
                        if (!entries[i].checked_us || now-entries[i].checked_us >= (uint64_t)cadence(&entries[i],group)*1000)
                            fetch(client,&body,&entries[i]);
                    }
                }
                esp_http_client_cleanup(client);
            }
        }
        uint64_t now=esp_timer_get_time();
        uint32_t request=__atomic_load_n(&s_history_request,__ATOMIC_ACQUIRE);
        if (!paused() && time(NULL)>1609459200 && (!last_history || request!=request_seen || now-last_history>=600000000ULL)) {
            static energy_history_t history;
            int day=__atomic_load_n(&s_day_offset,__ATOMIC_ACQUIRE);
            ha_energy_fetch(day,&history,paused);
            if (!paused()) {
                xSemaphoreTake(s_mtx,portMAX_DELAY);
                // Preserve cached data only for the requested calendar day.
                if (history.ok || s_history.day_start!=history.day_start) s_history=history;
                else strlcpy(s_history.error,history.error,sizeof(s_history.error));
                xSemaphoreGive(s_mtx);
                last_history=now; request_seen=request;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
esp_err_t ha_init(void) {
    if (__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) return ESP_OK;
    s_mtx=xSemaphoreCreateMutex();
    s_prices=json_alloc(sizeof(*s_prices)); s_price_stage=json_alloc(sizeof(*s_price_stage));
    s_body=json_alloc(HA_MAX_BODY);
    if (!s_mtx || !s_prices || !s_price_stage || !s_body) {
        free(s_prices); free(s_price_stage); free(s_body);
        if (s_mtx) vSemaphoreDelete(s_mtx);
        s_mtx=NULL; s_prices=s_price_stage=NULL; s_body=NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_prices,0,sizeof(*s_prices));
    ha_json_init();
    s_data.count=COUNT(k_entities); s_data.batt_count=COUNT(k_batt_entities); s_data.power_count=COUNT(k_power_entities);
    init_entries(s_data.sensors,k_entities,s_data.count,90000);
    init_entries(s_data.batteries,k_batt_entities,s_data.batt_count,900000);
    init_entries(s_data.power,k_power_entities,s_data.power_count,45000);
    for (int i=0;i<s_data.power_count;++i) if (s_data.power[i].role==HA_ROLE_PRICE) s_data.power[i].stale_ms=900000;
    if (xTaskCreate(poll_task,"ha_poll",6144,NULL,3,NULL)!=pdPASS) {
        free(s_prices); free(s_price_stage); free(s_body); vSemaphoreDelete(s_mtx);
        s_mtx=NULL; s_prices=s_price_stage=NULL; s_body=NULL;
        return ESP_ERR_NO_MEM;
    }
    s_data.started=true; __atomic_store_n(&s_started,true,__ATOMIC_RELEASE);
    return ESP_OK;
}
bool ha_get(ha_data_t *out) {
    if (!out) return false;
    if (!__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) {
        // Metadata is compile-time data, so pages remain labelled even before
        // the service starts or if its allocation fails. No global snapshot is
        // read while startup is publishing it.
        memset(out,0,sizeof(*out));
        out->count=COUNT(k_entities); out->batt_count=COUNT(k_batt_entities); out->power_count=COUNT(k_power_entities);
        init_entries(out->sensors,k_entities,out->count,90000);
        init_entries(out->batteries,k_batt_entities,out->batt_count,900000);
        init_entries(out->power,k_power_entities,out->power_count,45000);
        return false;
    }
    xSemaphoreTake(s_mtx,portMAX_DELAY); *out=s_data; xSemaphoreGive(s_mtx);
    uint64_t now=esp_timer_get_time();
    out->connected=out->wifi_up && out->last_response_us && now-out->last_response_us<=90000000;
    return true;
}
bool ha_prices_get(energy_prices_t *out) {
    if (!out || !__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) return false;
    xSemaphoreTake(s_mtx,portMAX_DELAY); *out=*s_prices; xSemaphoreGive(s_mtx); return true;
}
bool ha_history_get(energy_history_t *out) {
    if (!out || !__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) return false;
    xSemaphoreTake(s_mtx,portMAX_DELAY); *out=s_history; xSemaphoreGive(s_mtx); return true;
}
void ha_history_request(int day) {
    if (day!=0 && day!=-1) return;
    __atomic_store_n(&s_day_offset,day,__ATOMIC_RELEASE);
    __atomic_add_fetch(&s_history_request,1,__ATOMIC_ACQ_REL);
}
const ha_sensor_t *ha_find_role(const ha_data_t *d,ha_role_t role) {
    if (!d || role==HA_ROLE_NONE) return NULL;
    for (int i=0;i<d->power_count;++i) if (d->power[i].role==role) return &d->power[i];
    for (int i=0;i<d->count;++i) if (d->sensors[i].role==role) return &d->sensors[i];
    return NULL;
}
const char *ha_role_entity(ha_role_t role) {
    for (int i=0;i<COUNT(k_power_entities);++i) if (k_power_entities[i].role==role) return k_power_entities[i].entity_id;
    return NULL;
}
int ha_sensor_count(void) { return COUNT(k_entities); }
int ha_batt_count(void) { return COUNT(k_batt_entities); }
bool ha_sensor_meta(int i,ha_sensor_t *out) {
    if (!out || i<0 || i>=COUNT(k_entities) || !__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) return false;
    xSemaphoreTake(s_mtx,portMAX_DELAY); *out=s_data.sensors[i]; xSemaphoreGive(s_mtx); return true;
}
bool ha_poll_pause(unsigned timeout) {
    if (!__atomic_load_n(&s_started,__ATOMIC_ACQUIRE)) return true;
    uint32_t gen=__atomic_add_fetch(&s_pause_gen,1,__ATOMIC_ACQ_REL);
    __atomic_store_n(&s_pause_req,true,__ATOMIC_RELEASE);
    for (unsigned elapsed=0;elapsed<timeout;elapsed+=20) {
        if (__atomic_load_n(&s_ack_gen,__ATOMIC_ACQUIRE)==gen) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false; // request remains issued; every caller must resume on exit
}
void ha_poll_resume(void) { __atomic_store_n(&s_pause_req,false,__ATOMIC_RELEASE); }
