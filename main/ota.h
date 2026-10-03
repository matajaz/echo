#pragma once

#include "esp_http_server.h"

// Starts the key-gated pull-OTA HTTP server (routes: /ota, /info) and
// returns the server handle so other modules (e.g. weblog) can register
// their own routes on the same server. Returns NULL on failure.
// Must be called after WiFi is connected.
//
// Auth: the shared key is compile-time configured (see Kconfig / NVS),
// not supplied by the caller. It must be presented as the "X-OTA-Key"
// request header; requests without a valid key are rejected on every
// route (both /ota and /info) before any handler runs.
httpd_handle_t ota_server_start(void);
