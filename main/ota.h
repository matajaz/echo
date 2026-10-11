#pragma once

#include "esp_http_server.h"

// Starts the key-gated pull-OTA HTTP server (routes: /ota, /info) and
// returns the server handle so other modules (e.g. weblog) can register
// their own routes on the same server.
//
// Must be called after WiFi is connected.
//
// Lifetime/ownership: the returned handle is owned by ota.c and remains
// valid for the process lifetime (it survives WiFi disconnect/reconnect).
// Callers must not free it and must not assume it becomes invalid on
// network changes. There is intentionally no stop function — the server
// runs for the lifetime of the process.
//
// Failure: returns NULL on any failure (e.g. WiFi not ready, port bind
// failure, missing auth config). NULL is terminal: ota.c has already
// logged the underlying esp_err_t via ESP_LOGE, so callers should log a
// generic failure and disable OTA rather than retry blindly.
//
// Auth: the shared key is loaded from NVS at boot, seeded from the
// Kconfig default. It can therefore be rotated at runtime via NVS and is
// not a compile-time-only constant. It must be presented as the
// "X-OTA-Key" request header.
//
// SECURITY: "X-OTA-Key" is a secret bearer credential. Access-logging
// middlewares (including the weblog module that shares this handle) must
// redact this header and must never echo its value into logs, responses,
// or persistent storage. If a logger cannot redact it, prefer carrying
// the key as "Authorization: Bearer <key>", for which masking
// conventions already exist.
//
// Key rotation: NVS rotation takes effect at the next request boundary;
// the in-RAM key buffer is replaced atomically, so a request in flight
// compares against a single consistent key (never a torn value). Writers
// rotating the key must also go through NVS so the atomic swap happens
// on the next request.
//
// _WARN_: Auth scope — the key gate applies ONLY to the routes
// registered by ota.c (/ota and /info); requests to those routes without
// a valid key are rejected before any handler runs. Any module that
// registers its own routes on this handle (e.g. weblog) is responsible
// for its own auth — this key gate does NOT protect them. Do NOT add an
// unauthenticated route to this handle: because the handle is shared,
// every new route must carry its own auth or it is silently exposed.
// Prefer routing new protected endpoints through ota.c rather than
// registering raw httpd_uri_t entries directly.
httpd_handle_t ota_server_start(void);

// --- OTA debug status (for on-screen display) ----------------------------
// Snapshot of the most recent OTA attempt, so the UI info page can show
// whether an OTA was triggered, when, and how it ended. All fields are
// updated by the OTA task / trigger handler and read by the LVGL task;
// ota_debug_get() copies them out under no lock (plain word reads are
// atomic enough for display purposes — worst case the UI shows a 1-frame
// stale value). result is a short static string, never freed.
typedef struct {
    bool attempted;          // has any OTA been triggered since boot?
    bool in_progress;        // an OTA is running right now
    unsigned attempt_count;  // how many triggers accepted since boot
    int64_t last_start_us;   // esp_timer_get_time() when the last attempt began (0 = never)
    const char *last_result; // "running" / "ok (rebooting)" / "http connect" / "read fail" / ...
} ota_debug_t;

// Copy the current OTA debug snapshot into *out. Safe from any task.
void ota_debug_get(ota_debug_t *out);
