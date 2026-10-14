#include "maintenance_api.h"
#include "ha_client.h"
#include "ota.h"
#include "weblog.h"
#include "ui/ui_shell.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "secrets.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static esp_err_t send_json(httpd_req_t *req,cJSON *body,int status) {
    char *text=cJSON_PrintUnformatted(body); cJSON_Delete(body);
    httpd_resp_set_type(req,"application/json"); httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Access-Control-Allow-Origin","*");
    const char *label=status==503?"503 Service Unavailable":status==400?"400 Bad Request":status==403?"403 Forbidden":status==202?"202 Accepted":"200 OK";
    httpd_resp_set_status(req,label);
    esp_err_t result=text?httpd_resp_sendstr(req,text):httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"memory");
    free(text); return result;
}
static esp_err_t observation(httpd_req_t *req) {
    cJSON *j=cJSON_CreateObject();
    char path[80]; size_t length=strcspn(req->uri,"?");
    if (length>=sizeof(path)) { cJSON_AddStringToObject(j,"error","invalid_path"); return send_json(req,j,400); }
    memcpy(path,req->uri,length); path[length]=0;
    if (!strcmp(path,"/api/v1/health")) {
        ha_data_t *ha=heap_caps_malloc(sizeof(*ha),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!ha) { cJSON_AddStringToObject(j,"error","memory"); return send_json(req,j,503); }
        bool have=ha_get(ha); ui_status_t ui; ui_shell_snapshot(&ui);
        wifi_ap_record_t ap; bool wifi=esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
        uint64_t now=esp_timer_get_time(); int fresh=0,total=ha->power_count+ha->count+ha->batt_count;
        for (int group=0;group<3;group++) {
            const ha_sensor_t *items=group==0?ha->power:group==1?ha->sensors:ha->batteries;
            int count=group==0?ha->power_count:group==1?ha->count:ha->batt_count;
            for (int i=0;i<count;i++) if (ha_fresh(items+i,now)&&items[i].result==HA_VALID) fresh++;
        }
        ota_debug_t ota; ota_debug_get(&ota); cJSON *reasons=cJSON_AddArrayToObject(j,"reasons");
        if (!wifi) cJSON_AddItemToArray(reasons,cJSON_CreateString("wifi_offline"));
        if (!have || !ha->connected) cJSON_AddItemToArray(reasons,cJSON_CreateString("ha_unavailable"));
        if (fresh<total) cJSON_AddItemToArray(reasons,cJSON_CreateString("data_stale_or_unavailable"));
        if (!ui.ready) cJSON_AddItemToArray(reasons,cJSON_CreateString("ui_unavailable"));
        if (ota.in_progress) cJSON_AddItemToArray(reasons,cJSON_CreateString("updating"));
        cJSON_AddStringToObject(j,"device_status",ota.in_progress?"updating":!wifi?"wifi_offline":!fresh?"waiting_for_data":cJSON_GetArraySize(reasons)?"degraded":"healthy");
        cJSON_AddStringToObject(j,"status",cJSON_GetArraySize(reasons)?"degraded":"ok");
        cJSON_AddBoolToObject(j,"maintenance_ready",true); cJSON_AddBoolToObject(j,"ui_ready",ui.ready);
        cJSON_AddNumberToObject(j,"fresh_readings",fresh); cJSON_AddNumberToObject(j,"total_readings",total);
        free(ha);
    } else if (!strcmp(path,"/api/v1/metrics")) {
        uint32_t dropped,truncated; weblog_stats(&dropped,&truncated);
        cJSON_AddNumberToObject(j,"log_dropped",dropped); cJSON_AddNumberToObject(j,"log_truncated",truncated);
        cJSON_AddNumberToObject(j,"uptime_s",esp_timer_get_time()/1000000);
        cJSON_AddNumberToObject(j,"heap_free",esp_get_free_heap_size());
        cJSON_AddNumberToObject(j,"heap_min",esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(j,"heap_maxblk",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    } else if (!strcmp(path,"/api/v1/ota")) {
        ota_debug_t s; ota_debug_get(&s); cJSON_AddBoolToObject(j,"in_progress",s.in_progress);
        cJSON_AddStringToObject(j,"phase",s.phase?s.phase:"idle");
        cJSON_AddStringToObject(j,"result",s.last_result); cJSON_AddNumberToObject(j,"attempt",s.attempt_count);
        cJSON_AddNumberToObject(j,"bytes",s.bytes); cJSON_AddNumberToObject(j,"total",s.total);
        cJSON_AddNumberToObject(j,"elapsed_s",s.last_start_us?((s.last_end_us?s.last_end_us:esp_timer_get_time())-s.last_start_us)/1000000:0);
    } else if (!strcmp(path,"/api/v1/ui")) {
        ui_status_t s; ui_shell_snapshot(&s); cJSON_AddBoolToObject(j,"ready",s.ready);
        cJSON_AddNumberToObject(j,"page_id",s.page); cJSON_AddStringToObject(j,"page_name",ui_shell_page_name(s.page));
        cJSON_AddBoolToObject(j,"screensaver",s.dimmed); cJSON_AddBoolToObject(j,"pinned",s.pinned);
        cJSON_AddNumberToObject(j,"backlight",s.backlight); cJSON_AddNumberToObject(j,"idle_ms",s.idle_ms);
        cJSON_AddStringToObject(j,"backlight_mode",s.manual_backlight?"manual":"auto");
        return send_json(req,j,s.ready?200:503);
    } else if (!strcmp(path,"/pages")) {
        cJSON *a=cJSON_AddArrayToObject(j,"pages");
        for (int i=0;i<UI_PAGE_COUNT;i++) { cJSON *p=cJSON_CreateObject(); cJSON_AddNumberToObject(p,"id",i); cJSON_AddStringToObject(p,"name",ui_shell_page_name(i)); cJSON_AddItemToArray(a,p); }
    } else {
        cJSON_AddNumberToObject(j,"api_version",1); cJSON_AddNumberToObject(j,"boot_id",ota_boot_id());
        cJSON *limits=cJSON_AddObjectToObject(j,"limits"); cJSON_AddNumberToObject(limits,"backlight_max",100); cJSON_AddNumberToObject(limits,"pages",UI_PAGE_COUNT);
        cJSON *read=cJSON_AddArrayToObject(j,"read"),*control=cJSON_AddArrayToObject(j,"control");
        const char *paths[]={"/info","/api/v1/info","/api/v1/health","/api/v1/metrics","/api/v1/ui","/api/v1/ota","/api/v1/capabilities","/pages","/log","/log.txt","/metrics"};
        for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) cJSON_AddItemToArray(read,cJSON_CreateString(paths[i]));
        cJSON_AddItemToArray(control,cJSON_CreateString("POST /api/v1/ui/page?id=N"));
        cJSON_AddItemToArray(control,cJSON_CreateString("POST /api/v1/screen/backlight?level=0..100|auto"));
        cJSON_AddStringToObject(j,"control_auth","X-OTA-Key header; legacy /ota uses key query");
    }
    return send_json(req,j,200);
}
static esp_err_t control(httpd_req_t *req) {
    char key[64]={0}; cJSON *j=cJSON_CreateObject();
    bool auth=httpd_req_get_hdr_value_str(req,"X-OTA-Key",key,sizeof(key))==ESP_OK;
    size_t expected=strlen(OTA_KEY); unsigned diff=(unsigned)(strlen(key)^expected);
    if (expected<sizeof(key)) for (size_t i=0;i<expected;i++) diff|=(unsigned char)key[i]^(unsigned char)OTA_KEY[i];
    if (!auth || !expected || expected>=sizeof(key) || diff) { cJSON_AddStringToObject(j,"error","forbidden"); return send_json(req,j,403); }
    char query[128],value[16]; bool page=!strncmp(req->uri,"/api/v1/ui/page",15);
    if (httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK || httpd_query_key_value(query,page?"id":"level",value,sizeof(value))!=ESP_OK) {
        cJSON_AddStringToObject(j,"error","missing_argument"); return send_json(req,j,400);
    }
    int v=-1;
    if (page || strcmp(value,"auto")) {
        errno=0; char *end; long n=strtol(value,&end,10);
        if (!value[0] || value[0]<'0' || value[0]>'9' || *end || errno || n<0 || n>(page?UI_PAGE_COUNT-1:100)) {
            cJSON_AddStringToObject(j,"error","invalid_argument"); return send_json(req,j,400);
        }
        v=n;
    }
    ui_status_t state; ui_shell_snapshot(&state);
    if (!state.ready || !lvgl_port_lock(100)) { cJSON_AddStringToObject(j,"error","ui_unavailable"); return send_json(req,j,503); }
    bool ok=ui_shell_control(page,v); lvgl_port_unlock(); cJSON_AddBoolToObject(j,"accepted",ok);
    return send_json(req,j,ok?202:503);
}
esp_err_t maintenance_api_register(httpd_handle_t server) {
    const char *paths[]={"/api/v1/health","/api/v1/metrics","/api/v1/ui","/api/v1/ota","/api/v1/capabilities","/pages","/api/v1/ui/page","/api/v1/screen/backlight"};
    for (unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        httpd_uri_t route={.uri=paths[i],.method=i<6?HTTP_GET:HTTP_POST,.handler=i<6?observation:control};
        esp_err_t e=httpd_register_uri_handler(server,&route); if (e!=ESP_OK) return e;
    }
    return ESP_OK;
}
