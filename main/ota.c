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
// NOTE: the OTA host is a LAN-only nginx served over plain HTTP (same trust
// model as the Vegena/lumen boards). Authenticity rests on the OTA_URL_PREFIX
// host allowlist + the key-gated trigger + redirects disabled on the fetch,
// not on TLS -- adequate for a trusted home LAN. Redirects are disabled so an
// allowlisted URL cannot hand the transfer to an attacker-controlled,
// loopback or link-local host.

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

// Reads of s_ota_in_progress happen from the HTTP worker task(s) as well as
// from the OTA task, so make every access atomic. That removes the
// check-then-set race in handle_ota() and the data race on the flag itself.
static bool ota_busy_get(void) {
    bool v;
    __atomic_load(&s_ota_in_progress, &v, __ATOMIC_ACQUIRE);
    return v;
}

// Returns true only for the caller that flipped false -> true, so exactly one
// request can win the race to start an OTA.
static bool ota_try_begin(void) {
    bool expected = false;
    return __atomic_compare_exchange_n(&s_ota_in_progress, &expected, true,
                                      false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void ota_busy_end(void) {
    bool v = false;
    __atomic_store(&s_ota_in_progress, &v, __ATOMIC_RELEASE);
}

// --- Firmware provenance -------------------------------------------------
// esp_https_ota() validates the *structure* of an image (magic/version/size)
// and A/B partitioning means a bad/interrupted pull can't brick the device
// (the running slot is untouched until the new image verifies). The trust
// model here is the project's standard LAN-only one: the url must match the
// OTA_URL_PREFIX host allowlist (below), reject embedded credentials, and be
// HTTPS where used. (A per-image SHA-256 digest-pinning scheme was prototyped
// but removed -- the esp_https_ota per-chunk callback it needed isn't exposed
// in this IDF version, and no digest files are published on the OTA host.)

// Reject anything that isn't exactly "<prefix><filename>" (for either the
// HTTP OTA_URL_PREFIX or its HTTPS equivalent -- the deploy server serves
// both), with no embedded "@" (credential-in-URL) and a sane max length.
// Mirrors Vegena's util.cpp::ota_url_allowed() / lumen's ota_url_check.cpp.
// Returns the matched prefix length, or 0 if disallowed.
static size_t ota_url_prefix_len(const char *url) {
    if (!url) return 0;
    size_t len = strlen(url);
    if (len == 0 || len > 256) return 0;
    if (strchr(url, '@') != NULL) return 0;

    // Accept the configured HTTP prefix OR the explicit HTTPS prefix (same
    // host, different port -- both served by the deploy nginx). https pins
    // the embedded self-signed cert (below).
    size_t matched = 0;
    const char *prefixes[2] = {OTA_URL_PREFIX, OTA_URL_PREFIX_HTTPS};
    for (int i = 0; i < 2; i++) {
        size_t plen = strlen(prefixes[i]);
        // Invariant: a prefix must end in '/' so it can only ever authorise
        // "<prefix><filename>". Reject a misconfigured prefix rather than
        // silently treating it as a bare host-name substring match.
        if (plen == 0 || prefixes[i][plen - 1] != '/') {
            ESP_LOGE(TAG, "OTA URL prefix %d must end in '/'", i);
            return 0;
        }
        if (len > plen && strncmp(url, prefixes[i], plen) == 0) {
            matched = plen;
            break;
        }
    }
    if (matched == 0) return 0;

    // Reject path traversal / sub-path tricks and query/fragment smuggling
    // after the prefix: the remainder must be exactly "<filename>".
    const char *rest = url + matched;
    if (strstr(rest, "..") != NULL || strchr(rest, '/') != NULL ||
        strchr(rest, '?') != NULL || strchr(rest, '#') != NULL) {
        return 0;
    }
    return matched;
}

static bool ota_url_allowed(const char *url) {
    return ota_url_prefix_len(url) > 0;
}

static bool ota_url_is_https(const char *url) {
    return url && strncmp(url, "https://", 8) == 0;
}

// Embedded self-signed CA cert for the HTTPS OTA host (pinned). CMake
// EMBED_TXTFILES provides these symbols.
extern const char ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");

static void ota_task(void *pvParameter) {
    char *url = (char *)pvParameter;

    ESP_LOGI(TAG, "Starting OTA from %s", url);

    // Set up the HTTP(S) client. For https:// the embedded self-signed
    // cert is pinned via cert_pem; http:// is allowed too (LAN trust model,
    // CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP) and leaves cert_pem NULL.
    bool use_tls = ota_url_is_https(url);
    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .cert_pem = use_tls ? ota_ca_pem_start : NULL,
        // The allowlist authorises THIS url only. esp_https_ota() follows
        // redirects by default, which would let a permitted URL hand the
        // fetch to an attacker-controlled, loopback or link-local host, so
        // a 3xx is treated as a failure instead.
        .disable_auto_redirect = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    free(url);
    ota_busy_end();

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

// Single code path for every error reply: sets the status, tags the body as
// text/plain, and sends the message.
static esp_err_t ota_send_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t *req) {
    char key[64] = {0};
    char url[300] = {0};

    if (ota_get_param(req, "key", key, sizeof(key)) != ESP_OK ||
        strcmp(key, OTA_KEY) != 0) {
        return ota_send_error(req, "401 Unauthorized", "bad key");
    }

    // Claim the OTA slot atomically. A plain "if (busy) reject; busy = true;"
    // lets two HTTP workers pass the check before either sets the flag, and
    // s_ota_in_progress is read concurrently by handle_info().
    if (!ota_try_begin()) {
        return ota_send_error(req, "409 Conflict", "OTA already in progress");
    }

    if (ota_get_param(req, "url", url, sizeof(url)) != ESP_OK ||
        !ota_url_allowed(url)) {
        ota_busy_end();
        return ota_send_error(req, "400 Bad Request", "missing/disallowed url");
    }

    char *url_copy = strdup(url);
    if (!url_copy) {
        ota_busy_end();
        return ota_send_error(req, "500 Internal Server Error", "oom");
    }

    // Check the result: on failure url_copy leaks, the flag stays stuck at
    // true forever, and the client is told "OTA queued" for nothing.
    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        ota_busy_end();
        return ota_send_error(req, "503 Service Unavailable", "ota task start failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OTA queued");
    return ESP_OK;
}

static esp_err_t handle_info(httpd_req_t *req) {
    const esp_app_desc_t *app_desc = esp_app_get_description();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"idf\":\"%s\",\"ota_in_progress\":%s}",
        app_desc->version, app_desc->idf_ver,
        ota_busy_get() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

httpd_handle_t ota_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;   // /ota /info /log /log.txt + headroom

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA HTTP server");
        return NULL;
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
    return server;
}
