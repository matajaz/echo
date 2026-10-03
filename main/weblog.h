// echo -- in-RAM ring-buffer log served over HTTP.
//
// Baseline observability (per esp32-lessons.md: "the single highest-leverage
// lesson ... Add an in-RAM ring-buffer log served over HTTP ... Build this
// BEFORE debugging anything nontrivial"). A wall-mounted/battery board is
// nearly unobservable over serial (USB often absent); this tees every
// ESP_LOGx line into a fixed RAM ring and serves it at /log (auto-refresh
// HTML) and /log.txt (plain, with an incremental ?since=<id> cursor so a
// host collector fetches only new lines with bounded responses).
//
// Design mirrors Vegena's weblog.{h,cpp} / lumen's weblog_core: the ring +
// formatting live here; the HTTP glue registers onto the EXISTING httpd
// server (the one ota_server_start() already runs) rather than standing up
// a second server on another port.
//
// Ring policy: fixed capacity of 200 lines, each truncated to 160 bytes
// (including NUL) -- longer lines are silently cut. When full, the oldest
// line is silently evicted (the ring wraps); /log.txt never blocks on the
// ring, so lines logged while the lock is contended are dropped.
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the esp_log_set_vprintf hook so every ESP_LOGx line is also
// captured into the in-RAM ring (still goes to UART as well). Safe to call
// once, early in app_main (before other subsystems log, so their startup
// lines are captured too). Idempotent.
//
// NOT reentrant / not thread-safe: the esp_log_set_vprintf swap and the
// lazily-created ring lock are unlocked. Call once before other logging
// tasks start, or guard externally against a concurrent weblog_init()/
// weblog_register().
void weblog_init(void);

// Register the /log (HTML) and /log.txt (plain + ?since=<id> cursor)
// routes onto an already-started httpd server. Call after
// ota_server_start() returns its handle.
//
// /log.txt wire format (fixed; the host collector depends on it): the body
// is exactly one cursor header line "id: <N>\n" followed by each stored
// ESP_LOGx line as "<msg>\n" (no id/level/tag prefix -- the id is carried
// only by the header, and lines are newline-terminated, unprefixed text).
//
// Requires weblog_init() first (otherwise the routes serve an empty/
// uninitialized ring). NOT idempotent: registering twice against the same
// handle fails (double register -> ESP_ERR_HTTPD_HANDLERS_FULL or
// overwrite), so call once per server handle and return the underlying
// error.
//
// Lifecycle: the module borrows the handle and never unregisters; the
// server must outlive the module and the routes stay live for the process.
esp_err_t weblog_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
