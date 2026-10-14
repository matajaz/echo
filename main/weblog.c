// echo -- in-RAM ring-buffer web log implementation. See weblog.h.
//
// Follows the esp32-lessons.md guidance point-by-point:
//   - fixed RAM ring (no unbounded growth), guarded by a mutex;
//   - a monotonic per-line id so /log.txt?since=<id> returns only newer
//     lines + an "id: <N>" cursor header line (O(1) dedup for a host
//     collector, bounded responses, short mutex hold);
//   - on reboot the id resets below a client's cursor, so the board
//     returns a tail and the collector detects the gap;
//   - HTML content is escaped line-by-line (not a second full escaped
//     copy -> avoids heap fragmentation);
//   - the log response is built under the lock but send() happens OUTSIDE
//     it (don't hold the ring mutex across a slow HTTP write).
#include "weblog.h"
#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RING_LINES 200       // fixed line count (wraps); ~200 * ~160B ~= 32KB
#define WEBLOG_LINE_MAX 160         // per-line cap incl. NUL; longer lines truncated

typedef struct {
    uint32_t id;             // monotonic line id (0 = empty slot)
    char text[WEBLOG_LINE_MAX];
} log_line_t;

static log_line_t s_ring[RING_LINES];
static uint32_t s_dropped,s_truncated;
static uint32_t s_next_id = 1;   // id to assign to the NEXT stored line
static int s_head = 0;           // index of the next slot to write
static SemaphoreHandle_t s_lock = NULL;
static vprintf_like_t s_prev_vprintf = NULL;  // chain to the original (UART)

// Store one already-formatted line into the ring (called under s_lock).
static void ring_put(const char *line) {
    s_ring[s_head].id = s_next_id;
    // 0 is the "empty slot" sentinel, so wrap from UINT32_MAX to 1, not 0.
    // Otherwise after 2^32 lines every slot would look empty.
    if (++s_next_id == 0) {
        s_next_id = 1;
    }
    strlcpy(s_ring[s_head].text, line, sizeof(s_ring[s_head].text));
    s_head = (s_head + 1) % RING_LINES;
}

// esp_log_set_vprintf hook: format the line, tee into the ring, AND pass
// through to the previous handler (UART) so serial still works when present.
static int weblog_vprintf(const char *fmt, va_list args) {
    // Pass through to UART first using a COPY of the va_list (a va_list can
    // only be traversed once).
    int ret;
    va_list args_copy;
    va_copy(args_copy, args);
    if (s_prev_vprintf) {
        ret = s_prev_vprintf(fmt, args);
    } else {
        ret = vprintf(fmt, args);
    }
    va_end(args);

    char buf[WEBLOG_LINE_MAX];
    int n = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);
    if (n >= (int)sizeof(buf)) __atomic_fetch_add(&s_truncated,1,__ATOMIC_RELAXED);
    if (n > 0 && s_lock != NULL) {
        // Strip a single trailing newline so each ring slot is one line.
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }
        if (xSemaphoreTake(s_lock, 0) == pdTRUE) {   // never block the logger
            ring_put(buf);
            xSemaphoreGive(s_lock);
        } else __atomic_fetch_add(&s_dropped,1,__ATOMIC_RELAXED);
    }
    return ret;
}

void weblog_init(void) {
    if (s_lock != NULL) {
        return; // already initialized
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return; // out of memory -- leave logging on UART only
    }
    s_prev_vprintf = esp_log_set_vprintf(weblog_vprintf);
}

// --- HTTP: /log.txt (plain, incremental cursor) ----------------------
static esp_err_t handle_log_txt(httpd_req_t *req) {
    // Parse optional ?since=<id>. Absent -> a bounded tail (last ~60 lines).
    uint32_t since = 0;
    bool have_since = false;
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1) {
        char *query = malloc(qlen);
        if (query && httpd_req_get_url_query_str(req, query, qlen) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
                since = (uint32_t)strtoul(val, NULL, 10);
                have_since = true;
            }
        }
        free(query);
    }

    httpd_resp_set_type(req, "text/plain");

    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_sendstr(req, "id: 0\n(log unavailable)\n");
        return ESP_OK;
    }

    // Newest id currently in the ring: an upper bound for the cursor.
    uint32_t newest = (s_next_id > 1) ? (s_next_id - 1) : 0;

    // Walk the ring oldest->newest; include a line if its id qualifies.
    // Without ?since: a tail of the last ~60. With ?since: only id > since.
    // A cursor ahead of the newest id means the board rebooted and ids reset
    // below the client's position; fall back to a tail so the collector can
    // observe the gap instead of stalling forever.
    bool stale_since = have_since && since > newest;
    const uint32_t tail_n = 60;
    uint32_t tail_floor = ((!have_since || stale_since) && newest > tail_n) ? (newest - tail_n) : 0;

    // Build body under the lock into a bounded heap buffer, then release the
    // lock and send -- never send() while holding the ring mutex.
    size_t cap = 8192;
    char *body = malloc(cap);
    size_t used = 0;
    // Initialize immediately: an empty incremental read (e.g. ?since=<newest>
    // with no new line yet) appends nothing, and later code must see a valid
    // empty string, not uninitialized heap. Reading strlen() on an un-init'd
    // malloc was a heap-buffer-overread (R3, ASan, 2026-10-10).
    if (body) body[0] = '\0';
    uint32_t last_included = 0;   // id of the last line actually written out
    bool have_included = false;
    bool truncated = false;
    // The advertised cursor must only ever name a line the collector really
    // received. Until we know the body was sent in full, stay where the
    // client already was so nothing is skipped.
    uint32_t cursor = (have_since && !stale_since) ? since : 0;
    if (body) {
        for (int i = 0; i < RING_LINES; i++) {
            int idx = (s_head + i) % RING_LINES;   // oldest first
            uint32_t id = s_ring[idx].id;
            if (id == 0) continue;                  // empty slot
            if ((!have_since || stale_since) ? (id <= tail_floor) : (id <= since)) continue;
            size_t need = strlen(s_ring[idx].text) + 1;
            if (used + need + 1 >= cap) {           // bounded; stop appending
                truncated = true;
                break;
            }
            used += strlcpy(body + used, s_ring[idx].text, cap - used);
            body[used++] = '\n';
            body[used] = '\0';
            last_included = id;
            have_included = true;
        }
        if (!truncated) {
            // Every qualifying line fit, so the client may resume from the
            // newest id (a stale cursor is advanced here so the collector
            // stops pinning to an id that will never reappear).
            cursor = newest;
        } else {
            // Acknowledge only the prefix actually sent; the remainder stays
            // pending and is re-requested via ?since=<cursor>.
            cursor = have_included ? last_included : ((have_since && !stale_since) ? since : 0);
        }
    }
    xSemaphoreGive(s_lock);

    // Emit the cursor header and the body as one allocation in a single
    // send. Sending them separately would let a partial write deliver the
    // cursor while dropping the body, and the client would then resume past
    // lines it never received.
    char header[64];
    int hlen = snprintf(header, sizeof(header), "boot: %u\nid: %u\n", (unsigned)ota_boot_id(), (unsigned)cursor);
    if (hlen < 0) {
        hlen = 0;
    }
    // Use the tracked 'used' length — authoritative and never rescans the
    // buffer (R3). body is '\0'-initialized above, so used==0 is a valid empty.
    size_t blen = body ? used : 0;
    size_t total = (size_t)hlen + blen;
    char *resp = malloc(total + 1);
    if (resp == NULL) {
        // Could not assemble header+body as one send. Sending the header alone
        // would advance the collector's cursor past body lines it never got
        // (R3). Send a SAFE cursor that doesn't skip anything: the client's
        // own prior position (resume from there and retry), not the advanced
        // one. With no prior ?since, send id 0 so it re-pulls a tail.
        free(body);
        uint32_t safe_cursor = (have_since && !stale_since) ? since : 0;
        char safe_header[64];
        int sl = snprintf(safe_header, sizeof(safe_header), "boot: %u\nid: %u\n", (unsigned)ota_boot_id(),
                          (unsigned)safe_cursor);
        httpd_resp_send(req, safe_header, (sl > 0) ? sl : 0);
        return ESP_OK;
    }
    memcpy(resp, header, (size_t)hlen);
    if (blen > 0) {
        memcpy(resp + hlen, body, blen);
    }
    free(body);
    resp[total] = '\0';
    esp_err_t err = httpd_resp_send(req, resp, (ssize_t)total);
    free(resp);
    return err;
}

// --- HTTP: /log (auto-refresh HTML, escaped) -------------------------
static const char *HTML_HEAD =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta http-equiv=refresh content=3>"
    "<title>echo log</title>"
    "<style>body{background:#111;color:#ddd;font:12px monospace;margin:8px}"
    "pre{white-space:pre-wrap;word-break:break-word}</style></head>"
    "<body><pre>";
static const char *HTML_TAIL = "</pre></body></html>";

// Snapshot staging for the HTML view: the ring lock is held only long enough
// to copy one batch of lines here, never across httpd_resp_send_chunk(), so a
// slow client cannot stall the ring (weblog_vprintf drops lines when the lock
// is contended). 16 * 160B of .bss is far cheaper than a 32KB heap copy.
//
// NOTE: this buffer is shared between requests and is only safe because
// esp_http_server services a single server on one task by default. If the
// server is ever configured so handlers can run concurrently, move this to
// per-request heap (or it will clobber a concurrent /log response).
#define HTML_BATCH_LINES 16
static char s_html_batch[HTML_BATCH_LINES][WEBLOG_LINE_MAX];

// Append *s* to the chunked response with HTML-escaping, one char at a time
// (line-by-line / char-by-char avoids a second full escaped copy of the
// whole ring -> less heap fragmentation, per the lesson).
static esp_err_t send_escaped(httpd_req_t *req, const char *s) {
    char esc[64];
    size_t e = 0;
    for (const char *p = s; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '&': rep = "&amp;"; break;
            default: break;
        }
        esp_err_t err;
        if (rep) {
            if (e) {
                err = httpd_resp_send_chunk(req, esc, e);
                if (err != ESP_OK) return err;
                e = 0;
            }
            err = httpd_resp_send_chunk(req, rep, strlen(rep));
            if (err != ESP_OK) return err;
        } else {
            if (e >= sizeof(esc)) {
                err = httpd_resp_send_chunk(req, esc, e);
                if (err != ESP_OK) return err;
                e = 0;
            }
            esc[e++] = *p;
        }
    }
    if (e) {
        esp_err_t err = httpd_resp_send_chunk(req, esc, e);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t handle_log_html(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send_chunk(req, HTML_HEAD, strlen(HTML_HEAD));
    if (err != ESP_OK) {
        return err;
    }

    if (s_lock != NULL) {
        // Snapshot the head (and the newest id) once, under the lock, so lines
        // written during this chunked, possibly slow response cannot shift the
        // index mid-stream and duplicate or skip lines. Anything newer than the
        // snapshot waits for the next auto-refresh.
        int start;
        uint32_t newest;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
            start = 0;
            newest = 0;
        } else {
            start = s_head;
            newest = (s_next_id > 1) ? (s_next_id - 1) : 0;
            xSemaphoreGive(s_lock);
        }
        int i = 0;
        for (;;) {
            int n = 0;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
                break;                             // ring busy -- send what we have
            }
            // Copy one batch, then drop the lock before any HTTP write.
            while (n < HTML_BATCH_LINES && i < RING_LINES) {
                int idx = (start + i) % RING_LINES;    // oldest first
                i++;
                uint32_t id = s_ring[idx].id;
                if (id == 0) continue;                 // empty slot
                if (newest != 0 && id > newest) continue;  // newer than snapshot
                strlcpy(s_html_batch[n], s_ring[idx].text, WEBLOG_LINE_MAX);
                n++;
            }
            xSemaphoreGive(s_lock);

            for (int k = 0; k < n; k++) {
                err = send_escaped(req, s_html_batch[k]);
                if (err != ESP_OK) {
                    return err;                    // client gone -- stop escaping
                }
                err = httpd_resp_send_chunk(req, "\n", 1);
                if (err != ESP_OK) {
                    return err;
                }
            }
            if (i >= RING_LINES) {
                break;
            }
        }
    }

    err = httpd_resp_send_chunk(req, HTML_TAIL, strlen(HTML_TAIL));
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);   // end chunked response
}

esp_err_t weblog_register(httpd_handle_t server) {
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    httpd_uri_t log_html = {.uri = "/log", .method = HTTP_GET, .handler = handle_log_html};
    httpd_uri_t log_txt = {.uri = "/log.txt", .method = HTTP_GET, .handler = handle_log_txt};
    esp_err_t r1 = httpd_register_uri_handler(server, &log_html);
    esp_err_t r2 = httpd_register_uri_handler(server, &log_txt);
    if (r1 != ESP_OK || r2 != ESP_OK) {
        // Don't leave a partially-registered pair behind; a retry would then
        // fail with ESP_ERR_HTTPD_HANDLER_EXISTS for whichever succeeded.
        if (r1 == ESP_OK) {
            httpd_unregister_uri_handler(server, log_html.uri, log_html.method);
        }
        if (r2 == ESP_OK) {
            httpd_unregister_uri_handler(server, log_txt.uri, log_txt.method);
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

void weblog_stats(uint32_t *dropped,uint32_t *truncated) {
    *dropped=__atomic_load_n(&s_dropped,__ATOMIC_RELAXED);
    *truncated=__atomic_load_n(&s_truncated,__ATOMIC_RELAXED);
}
