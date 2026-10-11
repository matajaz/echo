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
//
// Line sanitization: the ring is a line-oriented store, but an ESP_LOGx
// message may itself contain '\r' or '\n' (e.g. a multi-line payload passed
// to a "%s" conversion). Before a record is stored, every embedded '\r' and
// '\n' is replaced with a single space. A stored record therefore never
// contains a newline and maps to exactly one line of /log.txt.
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the esp_log_set_vprintf hook so every ESP_LOGx line is also
// captured into the in-RAM ring (still goes to UART as well). Call early in
// app_main (before other subsystems log, so their startup lines are
// captured too).
//
// NOT reentrant / not thread-safe: the esp_log_set_vprintf swap and the
// lazily-created ring lock are unlocked, so a second call is only a no-op
// when it is serialized with the first. Two concurrent calls (or a call
// racing weblog_register()) can double-install the hook or create the lock
// twice (TOCTOU). Call exactly once, before other logging tasks start, or
// guard externally against concurrent weblog_init()/weblog_register().
void weblog_init(void);

// Register the /log (HTML) and /log.txt (plain + ?since=<id> cursor)
// routes onto an already-started httpd server. Call after
// ota_server_start() returns its handle.
//
// /log.txt wire format (fixed; the host collector depends on it): the body
// is exactly one cursor header line "id: <N>\n" followed by each stored
// ESP_LOGx line as "<msg>\n" (no id/level/tag prefix -- the id is carried
// only by the header, and lines are newline-terminated, unprefixed text).
// Because embedded '\r'/'\n' in a log message are replaced with a space
// before storing (see "Line sanitization" above), a stored record is always
// a single physical line: the number of "id:" headers is one and the count
// of body lines equals the number of records, so the host collector's
// line-counting contract cannot be desynchronized by log content.
//
// SECURITY: /log and /log.txt are served WITHOUT authentication -- like
// every other route on this httpd server unless a scheme is configured --
// and the ring captures *all* ESP_LOGx output (tags, WiFi/OTA URLs, config
// values, etc.). Do not log secrets, and only expose this server on a
// trusted network.
//
// Requires weblog_init() first (otherwise the routes serve an empty/
// uninitialized ring). NOT idempotent and NOT safe to retry: registering
// twice against the same handle can leave stale handlers installed (the
// second call's registrations may partially succeed, and only the /log
// route is rolled back on a partial failure, so /log.txt can leak). Call
// exactly once per server handle with preconditions satisfied; if it
// returns an error, do NOT call it again on the same handle while those
// routes may still be registered.
//
// Lifecycle: the module borrows the handle and never unregisters; the
// server must outlive the module and the routes stay live for the process.
esp_err_t weblog_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
