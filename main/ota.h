#pragma once

#include "esp_http_server.h"

// Starts the maintenance HTTP server. /ota, /reboot, /beep and /play require
// the compile-time OTA_KEY as a key query parameter. /info is a read-only
// public diagnostic. Wi-Fi reconnection does not destroy this server.
// Secrets live in secrets/secrets.h; never print keys/tokens in logs.
// Registered routes from other modules have their own access policy.
httpd_handle_t ota_server_start(void);

// Short, mutex/critical-section-protected snapshot of the last OTA attempt.
typedef struct {
    bool attempted;          // has any OTA been triggered since boot?
    bool in_progress;        // an OTA is running right now
    unsigned attempt_count;  // how many triggers accepted since boot
    int64_t last_start_us;   // esp_timer_get_time() when the last attempt began (0 = never)
    uint32_t bytes, total;
    int64_t last_end_us;
    const char *phase;
    const char *last_result; // "running" / "ok (rebooting)" / "http connect" / "read fail" / ...
} ota_debug_t;

// Copy the current OTA debug snapshot into *out. Safe from any task.
void ota_debug_get(ota_debug_t *out);

uint32_t ota_boot_id(void);
