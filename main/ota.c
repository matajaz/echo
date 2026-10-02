// Pull-OTA for echo (ESP32-C5).
//
// Pattern follows the workspace's proven lumen/Vegena approach, documented
// in steering (esp32-lessons.md "OTA (wireless firmware update) -- PULL,
// don't push"):
//   - Device PULLS the firmware (esp_https_ota), host never pushes/connects
//     back -- avoids host-firewall/NAT problems entirely.
//   - Trigger endpoint requires a key (OTA_KEY) -- an unauthenticated
//     "/ota?url=" is a LAN remote-flash / RCE hole.
//   - URL must match a host-allowlist PREFIX (OTA_URL_PREFIX) -- a key-gated
//     endpoint that fetches ANY url is still an SSRF / arbitrary-firmware
//     hole. Reject embedded credentials ("@") and over-long urls.
//   - Reject a second OTA while one is already in progress (409).
//   - A/B OTA partitions (set in partitions.csv) mean a failed/interrupted
//     flash leaves the running image intact.
//
// NOTE: this is plain HTTP (not HTTPS) to the OTA host, matching the LAN-only
// trust model already used for lumen/Vegena (own nginx on debian.local).
// esp_https_ota supports http:// targets when the IDF http client config
// doesn't force TLS verification; see ota_https_ota_config below.

#include <string.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "secrets.h"
#include "ota.h"

static const char *TAG = "echo_ota";

static volatile bool s_ota_in_progress = false;

// Reject anything that isn't exactly "<OTA_URL_PREFIX><filename>" with no
// embedded "@" (credential-in-URL) and a sane max length. Mirrors Vegena's
// util.cpp::ota_url_allowed() / lumen's hardened ota_url_check.cpp.
static bool ota_url_allowed(const char *url) {
    if (!url) return false;
    size_t len = strlen(url);
    if (len == 0 || len > 256) return false;
    if (strchr(url, '@') != NULL) return false;

    size_t prefix_len = strlen(OTA_URL_PREFIX);
    if (len <= prefix_len) return false;
    if (strncmp(url, OTA_URL_PREFIX, prefix_len) != 0) return false;

    // Reject path traversal / sub-path tricks after the prefix.
    const char *rest = url + prefix_len;
    if (strstr(rest, "..") != NULL || strchr(rest, '/') != NULL) return false;

    return true;
}

static void ota_task(void *pvParameter) {
    char *url = (char *)pvParameter;

    ESP_LOGI(TAG, "Starting OTA from %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    free(url);
    s_ota_in_progress = false;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    vTaskDelete(NULL);
}

static esp_err_t ota_get_param(httpd_req_t *req, const char *key, char *out, size_t out_len) {
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return ESP_FAIL;

    char *query = malloc(qlen);
    if (!query) return ESP_FAIL;

    esp_err_t ret = ESP_FAIL;
    if (httpd_req_get_url_query_str(req, query, qlen) == ESP_OK) {
        ret = httpd_query_key_value(query, key, out, out_len);
    }
    free(query);
    return ret;
}

static esp_err_t handle_ota(httpd_req_t *req) {
    char key[64] = {0};
    char url[300] = {0};

    if (ota_get_param(req, "key", key, sizeof(key)) != ESP_OK ||
        strcmp(key, OTA_KEY) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "bad key");
        return ESP_OK;
    }

    if (s_ota_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "OTA already in progress");
        return ESP_OK;
    }

    if (ota_get_param(req, "url", url, sizeof(url)) != ESP_OK ||
        !ota_url_allowed(url)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "missing/disallowed url");
        return ESP_OK;
    }

    char *url_copy = strdup(url);
    if (!url_copy) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "oom");
        return ESP_OK;
    }

    s_ota_in_progress = true;
    xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL);

    httpd_resp_sendstr(req, "OTA queued");
    return ESP_OK;
}

static esp_err_t handle_info(httpd_req_t *req) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"idf\":\"%s\",\"ota_in_progress\":%s}",
        app_desc->version, app_desc->idf_ver,
        s_ota_in_progress ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

void ota_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA HTTP server");
        return;
    }

    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = handle_ota,
    };
    httpd_register_uri_handler(server, &ota_uri);

    httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = handle_info,
    };
    httpd_register_uri_handler(server, &info_uri);

    ESP_LOGI(TAG, "OTA HTTP server started on port %d", config.server_port);
}
