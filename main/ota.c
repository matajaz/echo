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
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

#include "secrets.h"
#include "ota.h"
#include "ha_client.h"

static const char *TAG = "echo_ota";

static volatile bool s_ota_in_progress = false;
// Monotonic start time (esp_timer_get_time) of the in-flight OTA, used by the
// supervisor to bound the whole transfer. 0 = no OTA running.
static volatile int64_t s_ota_start_us = 0;

static bool ota_busy_get(void);  // defined below; used by ota_debug_get

// --- OTA debug status (surfaced on the display info page) ----------------
// Written by the trigger handler + OTA task, read by the LVGL task. The
// snapshot is MULTI-FIELD and includes a 64-bit timestamp, which is NOT
// atomic to read on this 32-bit core — a reader could see a torn last_start_us
// or a mismatched (count, result) pair. Guard every access with a tiny
// portMUX critical section so the whole struct is copied atomically. The
// sections are a few field assignments long (no blocking, no I/O), so this is
// cheap and safe from any task/core. last_result points at a string literal
// (never freed), so storing the pointer under the lock is sufficient.
static portMUX_TYPE s_ota_dbg_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_debug_t s_ota_dbg = {
    .attempted = false,
    .in_progress = false,
    .attempt_count = 0,
    .last_start_us = 0,
    .last_result = "none",
};

// Record an accepted attempt (count++, start time, running). Done BEFORE the
// worker is launched so the worker can never finish-and-set-result before this
// writes "running" (the old ordering let a fast failure get stomped back to
// "running"). Returns under the lock.
static void ota_dbg_mark_running(int64_t start_us) {
    portENTER_CRITICAL(&s_ota_dbg_mux);
    s_ota_dbg.attempted = true;
    s_ota_dbg.attempt_count++;
    s_ota_dbg.last_start_us = start_us;
    s_ota_dbg.in_progress = true;
    s_ota_dbg.last_result = "running";
    portEXIT_CRITICAL(&s_ota_dbg_mux);
}

static void ota_dbg_set_result(const char *r) {
    portENTER_CRITICAL(&s_ota_dbg_mux);
    s_ota_dbg.last_result = r;
    s_ota_dbg.in_progress = false;
    portEXIT_CRITICAL(&s_ota_dbg_mux);
}

void ota_debug_get(ota_debug_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_ota_dbg_mux);
    *out = s_ota_dbg;
    portEXIT_CRITICAL(&s_ota_dbg_mux);
    // in_progress mirrors the authoritative busy flag (atomic bool).
    out->in_progress = ota_busy_get();
}

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
    bool won = __atomic_compare_exchange_n(&s_ota_in_progress, &expected, true,
                                           false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (won) {
        // Stamp the start so the supervisor can bound the whole transfer.
        __atomic_store_n(&s_ota_start_us, esp_timer_get_time(), __ATOMIC_RELEASE);
    }
    return won;
}

static void ota_busy_end(void) {
    __atomic_store_n(&s_ota_start_us, (int64_t)0, __ATOMIC_RELEASE);
    bool v = false;
    __atomic_store(&s_ota_in_progress, &v, __ATOMIC_RELEASE);
}

// --- Transfer supervisor (finding #1, 2026-10-10) ------------------------
// esp_https_ota()'s convenience helper retries a read-timeout (EAGAIN) in an
// UNBOUNDED loop, and the 15 s http timeout bounds a single read, not the
// whole operation. A firmware host that stalls mid-body could therefore keep
// the board busy forever (/info ota_in_progress:true stuck, new triggers 409).
// This periodic timer is an independent watchdog: if an OTA has been busy
// longer than OTA_TRANSFER_DEADLINE_US, it force-reboots. That is SAFE — the
// A/B boot partition is not switched until the new image fully verifies, so a
// mid-transfer reboot simply resumes the current (running) image and clears
// the stuck busy flag. The deadline is generous (a normal LAN pull is ~10 s),
// so it only ever fires on a genuine hang, never on a healthy transfer.
#define OTA_TRANSFER_DEADLINE_US (180LL * 1000 * 1000)   // 180 s
#define OTA_SUPERVISOR_PERIOD_US (10LL * 1000 * 1000)    // check every 10 s

static void ota_supervisor_cb(void *arg) {
    (void)arg;
    if (!ota_busy_get()) return;
    int64_t started = __atomic_load_n(&s_ota_start_us, __ATOMIC_ACQUIRE);
    if (started == 0) return;
    int64_t elapsed = esp_timer_get_time() - started;
    if (elapsed > OTA_TRANSFER_DEADLINE_US) {
        ESP_LOGE(TAG, "OTA transfer exceeded %llds deadline (%llds) -- force-rebooting; "
                      "A/B boot slot is untouched so the running image resumes",
                 OTA_TRANSFER_DEADLINE_US / 1000000, elapsed / 1000000);
        ota_dbg_set_result("transfer timeout (rebooted)");
        esp_restart();
    }
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
    if (strstr(rest, "..") != NULL) return 0;
    // Only allow a plain filename charset. Besides the literal '/', '?' and
    // '#', this also rejects '%': a URL-encoded traversal ("%2e%2e%2f",
    // "%2F") would pass the string tests above but is decoded downstream by
    // esp_http_client/nginx, escaping the firmware directory.
    for (const char *p = rest; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return 0;
        }
    }
    return matched;
}

static bool ota_url_allowed(const char *url) {
    return ota_url_prefix_len(url) > 0;
}

static bool ota_url_is_https(const char *url) {
    return url && strncmp(url, "https://", 8) == 0;
}

// The allowlist hands BOTH OTA_URL_PREFIX (http) and OTA_URL_PREFIX_HTTPS to
// esp_https_ota(). The security story only holds if the two really are the
// same host served on a different port. Nothing else enforces that, so a
// misconfigured OTA_URL_PREFIX_HTTPS (any scheme/host) would silently be
// allowlisted. Assert the invariant -- scheme-suffix + host identical --
// here, and refuse to start the server otherwise.
static bool ota_prefixes_share_host(void) {
    const char *http_scheme = "http://";
    const char *https_scheme = "https://";
    size_t nl = strlen(OTA_URL_PREFIX);
    size_t ns = strlen(OTA_URL_PREFIX_HTTPS);
    if (nl < strlen(http_scheme) || ns < strlen(https_scheme)) return false;
    // Scheme suffixes must be exactly "http://" / "https://".
    if (strncmp(OTA_URL_PREFIX, http_scheme, strlen(http_scheme)) != 0) return false;
    if (strncmp(OTA_URL_PREFIX_HTTPS, https_scheme, strlen(https_scheme)) != 0) return false;

    // Compare the host portions (everything after the scheme, up to the ':' of
    // the port or the '/' of the path).
    const char *host_l = OTA_URL_PREFIX + strlen(http_scheme);
    const char *host_s = OTA_URL_PREFIX_HTTPS + strlen(https_scheme);
    const char *end_l = strpbrk(host_l, ":/");
    const char *end_s = strpbrk(host_s, ":/");
    if (end_l == NULL || end_s == NULL) return false;
    size_t len_l = (size_t)(end_l - host_l);
    size_t len_s = (size_t)(end_s - host_s);
    if (len_l == 0 || len_l != len_s) return false;
    return strncmp(host_l, host_s, len_l) == 0;
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
    // Quiesce HA polling for the duration of the transfer so its periodic GETs
    // don't contend with the OTA download. R7: the pause is always issued, so
    // we ALWAYS ha_poll_resume() on every non-reboot exit below. V10: the
    // generation-tagged ack means a true return proves THIS pause quiesced the
    // poll task; log (don't silently ignore) a failure so a contended transfer
    // is diagnosable. We still proceed — the OTA is the recovery path and must
    // not be abandoned over a best-effort quiesce (the pause stays set and the
    // poll task parks as soon as it next checks).
    if (!ha_poll_pause(5000)) {
        ESP_LOGW(TAG, "HA polling did not quiesce within 5s before OTA — proceeding "
                      "(pause remains set; transfer may briefly contend)");
    }

    // STREAMING OTA WORKER — frees the HTTP transport BEFORE verify.
    // ROOT CAUSE CONFIRMED (2026-10-10, Sol): the stock one-shot esp_https_ota()
    // runs esp_ota_end()'s image VERIFY while the download buffer + HTTP client
    // are STILL RESIDENT. On this feature-full build that peak internal-RAM
    // footprint starves the verify (OOM/mapping failure masked as
    // ESP_ERR_OTA_VALIDATE_FAILED) on a BYTE-PERFECT image — proven because
    // re-verifying the same written slot AFTER transport was freed returned
    // ESP_OK. Fix: download via a plain esp_http_client, then CLOSE+CLEANUP the
    // client (release transport RAM) BEFORE esp_ota_end(). No LVGL/I2S teardown
    // needed. (This is why the earlier advanced-API attempt helped nothing — it
    // still verified with transport resident; the ordering is the whole point.)
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_err_t ret = client ? ESP_OK : ESP_ERR_NO_MEM;
    esp_ota_handle_t ota = 0;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    bool ota_begun = false;

    if (ret == ESP_OK) ret = esp_http_client_open(client, 0);
    int64_t clen = (ret == ESP_OK) ? esp_http_client_fetch_headers(client) : -1;
    int status = (ret == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    if (ret == ESP_OK && status != 200) {
        ESP_LOGE(TAG, "OTA http status %d", status);
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK && target == NULL) ret = ESP_ERR_NOT_FOUND;
    if (ret == ESP_OK) {
        ret = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &ota);
        ota_begun = (ret == ESP_OK);
    }

    // Download loop: read into a modest stack buffer and write to flash. The
    // buffer is small so the transfer footprint stays low.
    static char rxbuf[1536];
    int total = 0;
    while (ret == ESP_OK) {
        int n = esp_http_client_read(client, rxbuf, sizeof(rxbuf));
        if (n < 0) { ret = ESP_FAIL; break; }
        if (n == 0) {  // EOF (or no more data)
            if (esp_http_client_is_complete_data_received(client)) break;
            // not complete yet; a 0 can occur between chunks — keep reading
            if (esp_http_client_is_chunked_response(client)) continue;
            break;
        }
        ret = esp_ota_write(ota, rxbuf, n);
        total += n;
    }
    bool complete = (ret == ESP_OK) && esp_http_client_is_complete_data_received(client);
    ESP_LOGI(TAG, "OTA download: wrote=%d declared=%lld complete=%d", total, (long long)clen, complete);
    // V12: an early EOF on a non-chunked body leaves ret==ESP_OK but
    // complete==false. Promote it to a real transport/size error so the numeric
    // outcome logged below is not the misleading "OTA failed: ESP_OK", and the
    // incident is classified as an incomplete transfer (not a generic success).
    if (ret == ESP_OK && !complete) ret = ESP_ERR_INVALID_SIZE;

    // CLOSE + FREE THE TRANSPORT NOW — before verify. This is the fix.
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); client = NULL; }
    free(url);

    if (ret == ESP_OK && complete && ota_begun) {
        // esp_ota_end() verifies the written image — now with transport RAM
        // released, so the verify has the headroom it lacked before.
        ret = esp_ota_end(ota);        // consumes the handle
        ota_begun = false;
        if (ret == ESP_OK) ret = esp_ota_set_boot_partition(target);
        if (ret == ESP_OK) {
            ota_dbg_set_result("ok (rebooting)");
            ESP_LOGI(TAG, "OTA verified + set boot, rebooting");
            esp_restart();   // flag stays set across reboot (no 2nd-OTA window)
        }
    }

    // Any failure path. Abort the OTA handle if still open (does not switch boot
    // slot). Report a display-friendly reason.
    if (ota_begun) esp_ota_abort(ota);
    {
        const char *why;
        if (ret == ESP_OK && !complete)        why = "truncated download";
        else switch (ret) {
            case ESP_ERR_INVALID_SIZE:         why = "truncated download"; break;
            case ESP_ERR_OTA_VALIDATE_FAILED:  why = "image verify fail";  break;
            case ESP_ERR_HTTP_CONNECT:         why = "http connect fail";  break;
            case ESP_FAIL:                     why = "download/http fail"; break;
            case ESP_ERR_NO_MEM:               why = "out of memory";      break;
            default:                           why = esp_err_to_name(ret); break;
        }
        ota_dbg_set_result(why);
        ESP_LOGE(TAG, "OTA failed: %s (complete=%d wrote=%d)", esp_err_to_name(ret), complete, total);
        ha_poll_resume();   // R7: ALWAYS release the pause issued above
        ota_busy_end();
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

// Constant-time key comparison: strcmp early-exits on the first differing
// byte, leaking the key's length and matching prefix through timing. Compare
// over the full OTA_KEY length with no early exit.
static bool ota_key_matches(const char *key, size_t key_size) {
    const size_t n = sizeof(OTA_KEY) - 1;  // OTA_KEY is a string literal
    unsigned char diff = (unsigned char)(strlen(key) ^ n);
    for (size_t i = 0; i < n; i++) {
        unsigned char kc = (i < key_size) ? (unsigned char)key[i] : 0;
        diff |= (unsigned char)(kc ^ (unsigned char)OTA_KEY[i]);
    }
    return diff == 0;
}

static esp_err_t handle_ota(httpd_req_t *req) {
    char key[64] = {0};
    char url[300] = {0};

    if (ota_get_param(req, "key", key, sizeof(key)) != ESP_OK ||
        !ota_key_matches(key, sizeof(key))) {
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

    // Publish the accepted/"running" debug state BEFORE launching the worker.
    // Ordering matters: the worker (prio 5) can run and FAIL before this
    // handler would have written "running" if we did it after xTaskCreate —
    // the worker's real result would then be stomped back to "running" (Sol
    // #7b). Marking running first means the worker only ever moves it forward
    // (ok / failure reason). Done under the debug lock, atomically.
    ota_dbg_mark_running(esp_timer_get_time());

    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        ota_busy_end();
        ota_dbg_set_result("task start failed");  // overwrites "running" — correct
        return ota_send_error(req, "503 Service Unavailable", "ota task start failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OTA queued");
    return ESP_OK;
}

// GET /reboot?key=<OTA_KEY> — key-gated remote reboot. Useful when the board
// is reachable over WiFi but has no USB attached (today's situation: no clean
// remote reboot path existed, forcing a physical power-cycle). Also forces a
// fresh WiFi scan/association on the way back up. Rejects a wrong key with 401
// (same constant-time check as /ota) so it can't be used as a LAN DoS.
static esp_err_t handle_reboot(httpd_req_t *req) {
    char key[64] = {0};
    if (ota_get_param(req, "key", key, sizeof(key)) != ESP_OK ||
        !ota_key_matches(key, sizeof(key))) {
        return ota_send_error(req, "401 Unauthorized", "bad key");
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "rebooting\n");
    ESP_LOGW(TAG, "/reboot requested — restarting in 500 ms");
    // Small delay so the HTTP response is flushed to the client before reset.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

static esp_err_t handle_info(httpd_req_t *req) {
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // WiFi RSSI + free heap + uptime make the link-quality / resource state
    // diagnosable remotely (added 2026-10-10: a whole session was lost guessing
    // at a flaky RF link — "wifi:m f null" bursts + esp-tls select() timeouts —
    // with no RSSI visible). rssi=0 means "not associated / unknown".
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    unsigned heap = (unsigned)esp_get_free_heap_size();
    unsigned min_heap = (unsigned)esp_get_minimum_free_heap_size();
    unsigned long long up_s = (unsigned long long)(esp_timer_get_time() / 1000000);

    // Per-CLASS heap: aggregate free_heap can look huge (PSRAM) while the
    // SCARCE pools starve. dma_free/dma_largest = DMA-capable internal RAM
    // (I2S/SPI-LCD buffers live here); int_free/int_largest = all internal.
    // Added 2026-10-10 after a DMA-starvation bug (aggregate 8 MB, DMA 210 B)
    // cost hours — these make that class of fault visible in one GET.
    unsigned dma_free = (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA);
    unsigned dma_largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    unsigned int_free = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned int_largest = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // V09 exact-image identity: export the running image's ELF sha256 (first
    // 8 bytes hex), its A/B image state, and a per-boot id so the deploy client
    // can prove the EXACT image is running + confirmed (not just a version
    // string, and catching a reboot that happened entirely between polls).
    char sha_hex[17] = "";
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_st = ESP_OTA_IMG_UNDEFINED;
    const char *state_str = "unknown";
    if (running && esp_ota_get_state_partition(running, &img_st) == ESP_OK) {
        switch (img_st) {
            case ESP_OTA_IMG_NEW:            state_str = "new"; break;
            case ESP_OTA_IMG_PENDING_VERIFY: state_str = "pending_verify"; break;
            case ESP_OTA_IMG_VALID:          state_str = "valid"; break;
            case ESP_OTA_IMG_INVALID:        state_str = "invalid"; break;
            case ESP_OTA_IMG_ABORTED:        state_str = "aborted"; break;
            default:                         state_str = "undefined"; break;
        }
    }
    // app_desc->app_elf_sha256 is the 32-byte elf sha; format first 8 bytes.
    for (int i = 0; i < 8; i++) {
        snprintf(sha_hex + i * 2, 3, "%02x", app_desc->app_elf_sha256[i]);
    }
    // Stable per-boot id (random once at first /info; good enough to detect a
    // reboot across polls — a fresh boot re-randomizes it).
    static uint32_t s_boot_id = 0;
    if (s_boot_id == 0) s_boot_id = esp_random() | 1u;

    char buf[480];
    snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"idf\":\"%s\",\"ota_in_progress\":%s,"
        "\"rssi\":%d,\"heap_free\":%u,\"heap_min\":%u,\"uptime_s\":%llu,"
        "\"dma_free\":%u,\"dma_largest\":%u,\"int_free\":%u,\"int_largest\":%u,"
        "\"app_sha\":\"%s\",\"img_state\":\"%s\",\"boot_id\":%u}",
        app_desc->version, app_desc->idf_ver,
        ota_busy_get() ? "true" : "false",
        rssi, heap, min_heap, up_s,
        dma_free, dma_largest, int_free, int_largest,
        sha_hex, state_str, (unsigned)s_boot_id);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

httpd_handle_t ota_server_start(void) {
    // Fail closed if the HTTP and HTTPS allowlist prefixes don't share the
    // same host (see ota_prefixes_share_host). Without this, a misconfigured
    // OTA_URL_PREFIX_HTTPS would be silently allowlisted and handed to
    // esp_https_ota(), invalidating the safety story.
    if (!ota_prefixes_share_host()) {
        ESP_LOGE(TAG, "OTA_URL_PREFIX and OTA_URL_PREFIX_HTTPS must share host");
        return NULL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;  // /ota /info /reboot /log /log.txt /metrics + headroom

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

    httpd_uri_t reboot_uri = {
        .uri = "/reboot",
        .method = HTTP_GET,
        .handler = handle_reboot,
    };
    httpd_register_uri_handler(server, &reboot_uri);

    // Start the transfer supervisor: an independent watchdog that force-reboots
    // a wedged OTA past OTA_TRANSFER_DEADLINE_US (see ota_supervisor_cb). Failing
    // to create it is non-fatal — OTA still works, it just loses the hang guard.
    const esp_timer_create_args_t sup_args = {
        .callback = ota_supervisor_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_supervisor",
    };
    esp_timer_handle_t sup = NULL;
    if (esp_timer_create(&sup_args, &sup) == ESP_OK) {
        esp_timer_start_periodic(sup, OTA_SUPERVISOR_PERIOD_US);
    } else {
        ESP_LOGW(TAG, "OTA supervisor timer not created -- transfer hang guard disabled");
    }

    ESP_LOGI(TAG, "OTA HTTP server started on port %d", config.server_port);
    return server;
}
