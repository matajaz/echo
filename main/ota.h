#pragma once

// Starts the key-gated pull-OTA HTTP server (routes: /ota, /info).
// Must be called after WiFi is connected.
void ota_server_start(void);
