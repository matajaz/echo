// Bounded, read-only HA recorder query; runs on the HA task, never on LVGL.
#include "ha_energy.h"
#include "ha_client.h"
#include "ha_parse.h"
#include "secrets.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WS_BODY_MAX 32768
typedef struct {
    char *body;
    size_t len, frame_base;
    bool failed;
    SemaphoreHandle_t complete;
} ws_reply_t;
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)base;
    ws_reply_t *r=arg;
    if (id==WEBSOCKET_EVENT_ERROR || id==WEBSOCKET_EVENT_DISCONNECTED) {
        r->failed=true; xSemaphoreGive(r->complete); return;
    }
    if (id!=WEBSOCKET_EVENT_DATA) return;
    esp_websocket_event_data_t *e=data;
    if (e->op_code!=1 && e->op_code!=0) return;
    if (e->payload_offset==0) {
        if (e->op_code==1) { r->len=0; r->failed=false; }
        r->frame_base=r->len;
    }
    if (e->data_len<0 || e->payload_offset<0 || e->payload_len<0 ||
        r->frame_base+(size_t)e->payload_offset!=r->len ||
        (size_t)e->data_len>=WS_BODY_MAX-r->len) r->failed=true;
    if (!r->failed) {
        memcpy(r->body+r->len,e->data_ptr,e->data_len); r->len+=e->data_len; r->body[r->len]=0;
    }
    if (e->fin && e->payload_offset+e->data_len==e->payload_len) xSemaphoreGive(r->complete);
}
static bool receive(ws_reply_t *r,bool (*cancel)(void)) {
    int64_t deadline=esp_timer_get_time()+8000000;
    while (!cancel() && esp_timer_get_time()<deadline)
        if (xSemaphoreTake(r->complete,pdMS_TO_TICKS(100))==pdTRUE) return !r->failed;
    return false;
}
static bool reply_type(const char *json,const char *type) {
    cJSON *r=cJSON_Parse(json), *t=cJSON_GetObjectItemCaseSensitive(r,"type");
    bool ok=cJSON_IsString(t) && !strcmp(t->valuestring,type); cJSON_Delete(r); return ok;
}
static void cache(energy_history_t *h,bool write) {
    static uint64_t last_write;
    nvs_handle_t n;
    if (nvs_open("echo_energy",write?NVS_READWRITE:NVS_READONLY,&n)!=ESP_OK) return;
    if (write) {
        uint64_t now=esp_timer_get_time();
        if (!last_write || now-last_write>=21600000000ULL) {
            if (nvs_set_blob(n,"history_v2",h,sizeof(*h))==ESP_OK && nvs_commit(n)==ESP_OK) last_write=now;
        }
    } else {
        energy_history_t saved; size_t size=sizeof(saved);
        if (nvs_get_blob(n,"history_v2",&saved,&size)==ESP_OK && size==sizeof(saved) &&
            saved.day_start==h->day_start && saved.day_end==h->day_end && saved.count==h->count && saved.ok) {
            *h=saved; h->cached=true; h->received_us=0;
            strlcpy(h->error,"Cached; HA unavailable",sizeof(h->error));
        }
    }
    nvs_close(n);
}
bool ha_energy_fetch(int day,energy_history_t *h,bool (*cancel)(void)) {
    *h=(energy_history_t){0};
    time_t now=time(NULL); struct tm start;
    localtime_r(&now,&start);
    start.tm_hour=start.tm_min=start.tm_sec=0; start.tm_mday+=day; start.tm_isdst=-1;
    time_t from=mktime(&start); start.tm_mday++; start.tm_isdst=-1;
    time_t until=mktime(&start);
    h->day_start=from; h->day_end=until; h->count=(until-from)/3600;
    if (h->count<1 || h->count>ENERGY_MAX_HOURS) { strlcpy(h->error,"Clock unavailable",sizeof(h->error)); return false; }
    for (int i=0;i<h->count;++i) h->hours[i].start=from+(int64_t)i*3600;
    const char *import_id=ha_role_entity(HA_ROLE_GRID_IMPORT_TODAY), *export_id=ha_role_entity(HA_ROLE_GRID_EXPORT_TODAY);
    const char *cost_id=ha_role_entity(HA_ROLE_GRID_IMPORT_COST_TODAY),*income_id=ha_role_entity(HA_ROLE_GRID_EXPORT_INCOME_TODAY);
    if (!import_id || !export_id) { strlcpy(h->error,"Configure grid energy roles",sizeof(h->error)); return false; }
    char uri[256];
    const char *base=HA_BASE_URL;
    if (strncmp(base,"http://",7) && strncmp(base,"https://",8)) return false;
    snprintf(uri,sizeof(uri),"%s%s/api/websocket",!strncmp(base,"https:",6)?"wss:":"ws:",strchr(base,':')+1);
    ws_reply_t reply={.body=heap_caps_malloc(WS_BODY_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),
                      .complete=xSemaphoreCreateBinary()};
    esp_websocket_client_handle_t client=NULL;
    const char *error="HA history unavailable";
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap)!=ESP_OK) { error="HA offline"; goto done; }
    if (!reply.body || !reply.complete) { error="History memory unavailable"; goto done; }
    esp_websocket_client_config_t cfg={.uri=uri,.disable_auto_reconnect=true,.network_timeout_ms=2000,
        .task_stack=4096,.task_prio=3,.buffer_size=1024,.crt_bundle_attach=esp_crt_bundle_attach};
    client=esp_websocket_client_init(&cfg);
    if (!client || esp_websocket_register_events(client,WEBSOCKET_EVENT_ANY,ws_event,&reply)!=ESP_OK ||
        esp_websocket_client_start(client)!=ESP_OK) goto done;
    if (!receive(&reply,cancel) || !reply_type(reply.body,"auth_required")) goto done;
    char auth[sizeof(HA_TOKEN)+64];
    int bytes=snprintf(auth,sizeof(auth),"{\"type\":\"auth\",\"access_token\":\"%s\"}",HA_TOKEN);
    if (bytes<0 || (size_t)bytes>=sizeof(auth) || esp_websocket_client_send_text(client,auth,bytes,pdMS_TO_TICKS(1000))!=bytes) goto done;
    if (!receive(&reply,cancel) || !reply_type(reply.body,"auth_ok")) { error="HA history authentication failed"; goto done; }
    struct tm utc; char a[32],b[32],query[1024];
    gmtime_r(&from,&utc); strftime(a,sizeof(a),"%Y-%m-%dT%H:%M:%SZ",&utc);
    gmtime_r(&until,&utc); strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&utc);
    bytes=snprintf(query,sizeof(query),"{\"id\":1,\"type\":\"recorder/statistics_during_period\","
        "\"start_time\":\"%s\",\"end_time\":\"%s\",\"statistic_ids\":[\"%s\",\"%s\"],"
        "\"period\":\"hour\",\"types\":[\"change\"],\"units\":{\"energy\":\"kWh\"}}",a,b,import_id,export_id);
    if (bytes<0 || (size_t)bytes>=sizeof(query) || esp_websocket_client_send_text(client,query,bytes,pdMS_TO_TICKS(1000))!=bytes) goto done;
    if (!receive(&reply,cancel)) { if (reply.failed) error="History response rejected"; goto done; }
    bool energy_ok=ha_parse_statistics(reply.body,import_id,export_id,h);
    bool cost_ok=false;
    if (cost_id && income_id && !cancel()) {
        bytes=snprintf(query,sizeof(query),"{\"id\":2,\"type\":\"recorder/statistics_during_period\","
            "\"start_time\":\"%s\",\"end_time\":\"%s\",\"statistic_ids\":[\"%s\",\"%s\"],"
            "\"period\":\"hour\",\"types\":[\"change\"]}",a,b,cost_id,income_id);
        if (bytes>0 && (size_t)bytes<sizeof(query) && esp_websocket_client_send_text(client,query,bytes,pdMS_TO_TICKS(1000))==bytes && receive(&reply,cancel))
            cost_ok=ha_parse_cost_statistics(reply.body,cost_id,income_id,h);
    }
    h->ok=energy_ok || cost_ok;
    if (!h->ok) error="No recorded energy for this day";
done:
    // The event context/buffer remain alive until the worker has fully stopped.
    if (client) { esp_websocket_client_stop(client); esp_websocket_client_destroy(client); }
    if (reply.complete) vSemaphoreDelete(reply.complete);
    free(reply.body);
    if (h->ok) { h->received_us=esp_timer_get_time(); cache(h,true); }
    else { strlcpy(h->error,error,sizeof(h->error)); cache(h,false); }
    return h->ok;
}
