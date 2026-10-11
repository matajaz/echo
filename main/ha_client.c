// echo -- Home Assistant REST client for temperature sensors.
// See ha_client.h for the contract.

#include "ha_client.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "secrets.h"

static const char *TAG = "ha_client";

// Poll cadence. HA states change slowly (a room temperature), so 30 s is
// plenty and keeps the shared single-core chip free for audio/sensors.
#define HA_POLL_PERIOD_MS 30000
// Per-request timeout. Short: a stuck request must not hold the poll task
// (and its snapshot write) for long. Matches the lightweight poll spirit of
// the sibling Vegena client (700 ms there); this box is a full ESP32 so a
// slightly larger budget is fine.
#define HA_HTTP_TIMEOUT_MS 3000
// Cap the response body we buffer. /api/states/<entity> for a single
// entity is well under 2 KB; a 4 KB cap bounds heap use even if HA (or a
// wrong URL) returns a big document.
#define HA_MAX_BODY 4096

// Configured entities. EDIT THIS LIST (or generate it from a build macro)
// to match your HA instance. entity_id is the HA object id; name is the
// short label shown on the panel. This keeps the "which temperatures"
// decision in one obvious place.
typedef struct {
    const char *entity_id;
    const char *name;
} ha_entity_cfg_t;

static const ha_entity_cfg_t k_entities[] = {
    {"sensor.ute_grund_temperatur",            "Ute-Grund"},
    {"sensor.ute_temp3_temperatur",            "Framsida"},
    {"sensor.oliver_sonoff_temperatur",        "Oliver"},
    {"sensor.kok_sonoff_temperatur_2",         "Kok"},
    {"sensor.badrum_temperatur",               "Badrum"},
    {"sensor.vind_temperatur",                 "Vind"},
    {"sensor.dusch_temperatur",                "Dusch"},
    {"sensor.vardagsrum_current_temperature",  "V-rum"},
};
#define HA_CFG_COUNT ((int)(sizeof(k_entities) / sizeof(k_entities[0])))

// Battery entities for the SAME physical sensors as k_entities, in the SAME
// order so the two pages line up. These are separate HA objects (the Sonoff/
// Zigbee battery_level diagnostic entities, device_class=battery). Verified
// live against the HA instance 2026-10-11.
//
// NOTE: k_entities has 8 rows but this list has 7 -- the V-rum temperature
// sensor ("Vardagsrum Aktuell temperatur") exposes NO battery entity in HA
// (confirmed: 0 of the 20 battery-class entities match it), so there is
// nothing to show for it. Do not invent an id -- polling a non-existent
// entity returns 404 and poisons last_http_code for the whole cycle.
static const ha_entity_cfg_t k_batt_entities[] = {
    {"sensor.ute_grund_batteri",       "Ute-Grund"},
    {"sensor.ute_temp3_batteri",       "Framsida"},
    {"sensor.oliver_sonoff_batteri",   "Oliver"},
    {"sensor.kok_sonoff_batteri_2",    "Kok"},
    {"sensor.badrum_batteri",          "Badrum"},
    {"sensor.vind_batteri",            "Vind"},
    {"sensor.dusch_batteri",           "Dusch"},
};
#define HA_BATT_CFG_COUNT ((int)(sizeof(k_batt_entities) / sizeof(k_batt_entities[0])))

static ha_data_t s_data;
static SemaphoreHandle_t s_mtx = NULL;
static bool s_started = false;

// OTA polling-quiescence handshake (V10, generation-tagged 2026-10-10).
// s_pause_req: OTA asks the poll task to park (sole writer: pause/resume).
// s_pause_gen: bumped on EVERY pause request. s_ack_gen: the poll task sets it
// to the generation it has actually parked for (sole writer: poll task).
// A waiter accepts the ack only when s_ack_gen == its OWN requested generation,
// so a STALE idle left over from a prior pause (resumed, task not yet un-idled)
// cannot acknowledge a brand-new pause — the check-then-act race the plain
// s_poll_idle boolean had. The poll task also RECHECKS s_pause_req immediately
// before poll_once(), so no HTTP can start after an ack for the current gen.
static volatile bool s_pause_req = false;
static volatile uint32_t s_pause_gen = 0;
static volatile uint32_t s_ack_gen = 0;

// Buffer passed to the HTTP event handler via user_data. Bounded by
// HA_MAX_BODY; overrun is flagged so the fetch is treated as failed rather
// than silently truncated into invalid JSON.
typedef struct {
    char  buf[HA_MAX_BODY];
    size_t len;
    bool  overflow;
} ha_http_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt) {
    ha_http_buf_t *b = (ha_http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b != NULL) {
        if (b->len + evt->data_len >= sizeof(b->buf)) {
            b->overflow = true; // keep draining; just refuse to store more
            return ESP_OK;
        }
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

// Parse a HA state string to a Celsius float. "unavailable"/"unknown"/
// ""/non-numeric -> NAN (never 0, so a missing sensor can't masquerade as
// a real 0 C reading). Mirrors the sibling Vegena/Vegena as_float() helper.
static float parse_state_temp(const char *s) {
    if (s == NULL) return NAN;
    while (*s == ' ' || *s == '\t') s++; // tolerate leading whitespace
    if (*s == '\0' || strcmp(s, "unavailable") == 0 || strcmp(s, "unknown") == 0 ||
        strcmp(s, "None") == 0 || strcmp(s, "null") == 0 || strcmp(s, "nan") == 0) {
        return NAN;
    }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s || *end != '\0') return NAN; // not fully numeric
    if (!isfinite(v)) return NAN;
    return v;
}

// Minimal, allocation-free "state" extractor for a HA state object. We only
// need the top-level "state" string. A full JSON parser is unnecessary for
// one field and would pull in a dependency; instead find the FIRST
// "state"\s*:\s*"..." occurrence. HA's object key order puts "entity_id",
// "state", "attributes"... but not guaranteed -- we scan for the key and
// handle simple escapes. Returns true and fills out[..] on success.
//
// DEPTH-AWARE (#2, 2026-10-10): a plain strstr("\"state\"") matches the FIRST
// "state" at ANY nesting depth, so an attribute literally named "state" inside
// the attributes{} object could be returned instead of the top-level entity
// state. This scans only brace-DEPTH-1 keys (the object's own fields), skipping
// over nested objects/arrays and string contents, so it returns the real
// top-level "state" value regardless of key order or nested look-alikes.
static bool extract_state(const char *json, char *out, size_t outsz) {
    if (json == NULL || out == NULL || outsz == 0) return false;
    const char *p = json;
    int depth = 0;
    while (*p) {
        char c = *p;
        if (c == '"') {
            // Start of a string token. Capture it (unescaped) to check if it's
            // a key, then decide based on depth + the following ':'.
            const char *start = p + 1;
            const char *q = start;
            while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; }
            // V04: if the scan stopped on NUL (not a closing quote) the body is
            // truncated/incomplete mid-token. Reject rather than dereference q+1
            // past the buffer end. A valid key always ends on a real '"'.
            if (*q != '"') return false;
            // Is this the top-level "state" KEY (depth 1, followed by ':')?
            if (depth == 1 && (size_t)(q - start) == 5 && strncmp(start, "state", 5) == 0) {
                const char *r = q + 1;
                while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
                if (*r == ':') {
                    r++;
                    while (*r == ' ' || *r == '\t' || *r == '\n' || *r == '\r') r++;
                    if (*r != '"') return false;   // state must be a string value
                    r++;
                    size_t n = 0;
                    while (*r && *r != '"' && n + 1 < outsz) {
                        if (*r == '\\' && r[1]) r++;
                        out[n++] = *r++;
                    }
                    out[n] = '\0';
                    return (*r == '"');
                }
            }
            p = (*q == '"') ? q + 1 : q;   // skip past the string token
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    return false;  // no top-level "state" found
}

// Fetch one entity's state into hb using a SHARED, kept-alive client.
// Reusing one connection across all entities (same host:port) avoids opening
// a fresh TCP socket per entity. This cuts per-poll connection churn (N GETs
// over 1 connection instead of N), which is a good idea regardless of root
// cause. (It was previously described as THE fix for a proven TIME_WAIT
// socket exhaustion; that cause is unverified — see sdkconfig.defaults — but
// the reuse is still worth keeping.) Returns HTTP status, or <0 on a
// transport error.
static int fetch_entity(esp_http_client_handle_t client, const char *entity_id,
                        ha_http_buf_t *hb) {
    char url[192];
    // Defensive: reject an entity id that could break the URL (a stray
    // quote/space). These are compile-time constants, but the guard keeps a
    // future config source honest and the URL well-formed.
    for (const char *q = entity_id; *q; q++) {
        if (*q == '"' || *q == ' ' || *q == '\r' || *q == '\n') {
            ESP_LOGW(TAG, "refusing malformed entity id: %s", entity_id);
            return -1;
        }
    }
    int url_n = snprintf(url, sizeof(url), "%s/api/states/%s", HA_BASE_URL, entity_id);
    if (url_n < 0 || (size_t)url_n >= sizeof(url)) {
        return -1; // truncated URL -> don't fetch
    }

    if (client == NULL || hb == NULL) return -1;
    hb->len = 0;
    hb->overflow = false;
    hb->buf[0] = '\0';   // #2: clear so an empty response can't reuse the
                         // previous entity's body as this entity's state.

    // Point the kept-alive client at this entity's URL. user_data was bound
    // to hb at init time; it stays valid across requests (same hb).
    if (esp_http_client_set_url(client, url) != ESP_OK) return -1;

    esp_err_t err = esp_http_client_perform(client);
    int code = -1;
    if (err == ESP_OK) {
        code = esp_http_client_get_status_code(client);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", entity_id, esp_err_to_name(err));
    }
    return code;
}

static void poll_once(void) {
    // Only poll when the link is up; otherwise leave last values (ok flags
    // are cleared below so stale readings don't look live).
    wifi_ap_record_t ap;
    bool wifi_up = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    ha_data_t d = {0};
    d.started = true;
    d.count = HA_CFG_COUNT;
    d.wifi_up = wifi_up;
    for (int i = 0; i < HA_CFG_COUNT && i < HA_MAX_SENSORS; i++) {
        ha_sensor_t *s = &d.sensors[i];
        strncpy(s->entity_id, k_entities[i].entity_id, sizeof(s->entity_id) - 1);
        strncpy(s->name, k_entities[i].name, sizeof(s->name) - 1);
        s->temp_c = NAN;
        s->ok = false;
    }
    d.batt_count = (HA_BATT_CFG_COUNT < HA_MAX_BATT) ? HA_BATT_CFG_COUNT : HA_MAX_BATT;
    for (int i = 0; i < d.batt_count; i++) {
        ha_sensor_t *s = &d.batteries[i];
        strncpy(s->entity_id, k_batt_entities[i].entity_id, sizeof(s->entity_id) - 1);
        strncpy(s->name, k_batt_entities[i].name, sizeof(s->name) - 1);
        s->temp_c = NAN;
        s->ok = false;
    }

    if (!wifi_up) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_data = d; // count/meta populated, all ok=0
        xSemaphoreGive(s_mtx);
        return;
    }

    int last_code = 0;
    static ha_http_buf_t hb; // file-static: only the single poll task uses it

    // One kept-alive client for the whole cycle (all entities share host:port).
    // keep_alive_enable keeps the TCP connection open between the per-entity
    // GETs, so a poll of N entities uses ONE connection instead of N — fewer
    // connects/closes per cycle (good hygiene; not a verified fix for any
    // specific OTA failure — see sdkconfig.defaults).
    const esp_http_client_config_t cfg = {
        .url = HA_BASE_URL "/api/states/_init_", // replaced per-request via set_url
        .timeout_ms = HA_HTTP_TIMEOUT_MS,
        .event_handler = http_event_cb,
        .user_data = &hb,
        .disable_auto_redirect = true,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != NULL) {
        if (HA_TOKEN[0] != '\0') {
            // HA long-lived tokens are JWTs ~180-250 chars. The old 160-byte
            // buffer SILENTLY TRUNCATED them -> a malformed "Bearer <partial>"
            // header -> HA rejects with the OAuth "resource_metadata" error
            // (the live auth failure seen on-device, 2026-10-10). Size the
            // header to the actual token length, and sanity-check it.
            size_t tlen = strlen(HA_TOKEN);
            if (tlen < 20) {
                ESP_LOGW(TAG, "HA_TOKEN looks too short (%u chars) — auth will likely fail",
                         (unsigned)tlen);
            }
            char auth[8 + 320 + 1];   // "Bearer " + up to 320 token chars + NUL
            int w = snprintf(auth, sizeof(auth), "Bearer %s", HA_TOKEN);
            if (w < 0 || (size_t)w >= sizeof(auth)) {
                ESP_LOGE(TAG, "HA_TOKEN too long (%u chars) — Authorization header "
                              "truncated; auth will fail", (unsigned)tlen);
            }
            esp_http_client_set_header(client, "Authorization", auth);
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");

        for (int i = 0; i < HA_CFG_COUNT && i < HA_MAX_SENSORS; i++) {
            ha_sensor_t *s = &d.sensors[i];
            int code = fetch_entity(client, s->entity_id, &hb);
            last_code = code;
            if (code == 200 && !hb.overflow) {
                char state[64];
                if (extract_state(hb.buf, state, sizeof(state))) {
                    float t = parse_state_temp(state);
                    if (!isnan(t)) {
                        s->temp_c = t;
                        s->ok = true;
                    }
                }
            } else if (hb.overflow) {
                ESP_LOGW(TAG, "%s: response exceeded %d bytes", s->entity_id, HA_MAX_BODY);
            }
        }

        // Battery entities: same sensors, separate HA objects (device_class=
        // battery). parse_state_temp is reused as a generic numeric-state
        // extractor (NAN for "unavailable"/"unknown"); the value is a percent,
        // stored in temp_c. Same kept-alive client, so still one connection.
        for (int i = 0; i < d.batt_count; i++) {
            ha_sensor_t *s = &d.batteries[i];
            int code = fetch_entity(client, s->entity_id, &hb);
            last_code = code;
            if (code == 200 && !hb.overflow) {
                char state[64];
                if (extract_state(hb.buf, state, sizeof(state))) {
                    float v = parse_state_temp(state);
                    if (!isnan(v)) {
                        s->temp_c = v;
                        s->ok = true;
                    }
                }
            }
        }
        esp_http_client_cleanup(client); // one close per cycle, not per entity
    } else {
        ESP_LOGW(TAG, "HA poll: client init failed");
        last_code = -1;
    }
    d.last_http_code = last_code;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "HA poll: %d entities + %d batteries, last_code=%d, wifi=%d",
             d.count, d.batt_count, last_code, wifi_up);
}

static void ha_poll_task(void *arg) {
    (void)arg;
    for (;;) {
        // Quiescence handshake: if OTA (or anyone) requested a pause, park HERE
        // — BEFORE any HTTP in poll_once() — and publish the ACK generation so
        // the requester knows no HA GET is in flight for THIS request.
        if (__atomic_load_n(&s_pause_req, __ATOMIC_ACQUIRE)) {
            // Ack the generation we are parking for; refresh it each loop so a
            // pause re-requested while we are already parked is acked too.
            while (__atomic_load_n(&s_pause_req, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&s_ack_gen,
                                 __atomic_load_n(&s_pause_gen, __ATOMIC_ACQUIRE),
                                 __ATOMIC_RELEASE);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        // V10: recheck immediately before HTTP. If a pause arrived in the window
        // between the park-loop exit and here, loop back and park rather than
        // starting a GET the requester thinks is quiesced.
        if (__atomic_load_n(&s_pause_req, __ATOMIC_ACQUIRE)) continue;
        poll_once();
        // Break the inter-poll sleep into short slices so a pause request mid-
        // sleep is honored promptly (not up to HA_POLL_PERIOD_MS later).
        for (int slept = 0; slept < HA_POLL_PERIOD_MS; slept += 200) {
            if (__atomic_load_n(&s_pause_req, __ATOMIC_ACQUIRE)) break;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

bool ha_poll_pause(unsigned timeout_ms) {
    // If the poll task never started, there's nothing to quiesce.
    if (!s_started) return true;
    // V10: bump the generation and remember OURS. The ack must match THIS
    // generation — a stale ack from a previous pause cannot satisfy us.
    uint32_t my_gen = __atomic_add_fetch(&s_pause_gen, 1, __ATOMIC_ACQ_REL);
    // The pause request is ALWAYS issued here. If the ack times out we return
    // false but LEAVE s_pause_req set — so the caller MUST call ha_poll_resume()
    // on every exit path regardless of this return value, or polling stays
    // parked forever (R7). The bool only reports whether the poll task
    // confirmed it parked for OUR generation within the timeout.
    __atomic_store_n(&s_pause_req, true, __ATOMIC_RELEASE);
    unsigned waited = 0;
    while (__atomic_load_n(&s_ack_gen, __ATOMIC_ACQUIRE) != my_gen) {
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "ha_poll_pause: gen %" PRIu32 " not acked within %u ms "
                          "(request stays set; caller must resume)", my_gen, timeout_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    ESP_LOGI(TAG, "HA polling paused (gen %" PRIu32 " acked in %u ms)", my_gen, waited);
    return true;
}

void ha_poll_resume(void) {
    __atomic_store_n(&s_pause_req, false, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "HA polling resumed");
}

esp_err_t ha_init(void) {
    if (s_started) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    // Publish the identity metadata immediately so the UI can render the
    // list (with "--" values) before the first successful fetch.
    ha_data_t d = {0};
    d.started = true;
    d.count = (HA_CFG_COUNT < HA_MAX_SENSORS) ? HA_CFG_COUNT : HA_MAX_SENSORS;
    for (int i = 0; i < d.count; i++) {
        strncpy(d.sensors[i].entity_id, k_entities[i].entity_id,
                sizeof(d.sensors[i].entity_id) - 1);
        strncpy(d.sensors[i].name, k_entities[i].name, sizeof(d.sensors[i].name) - 1);
        d.sensors[i].temp_c = NAN;
        d.sensors[i].ok = false;
    }
    d.batt_count = (HA_BATT_CFG_COUNT < HA_MAX_BATT) ? HA_BATT_CFG_COUNT : HA_MAX_BATT;
    for (int i = 0; i < d.batt_count; i++) {
        strncpy(d.batteries[i].entity_id, k_batt_entities[i].entity_id,
                sizeof(d.batteries[i].entity_id) - 1);
        strncpy(d.batteries[i].name, k_batt_entities[i].name,
                sizeof(d.batteries[i].name) - 1);
        d.batteries[i].temp_c = NAN;
        d.batteries[i].ok = false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_data = d;
    xSemaphoreGive(s_mtx);

    // #4 (2026-10-10): create the poll task BEFORE marking the module started.
    // The old order set s_started=true first, so a task-create failure left
    // ha_get() reporting "started" while the poll task never ran — stale/empty
    // data that looks alive. Now s_started reflects worker LIVENESS, not just
    // that init was attempted.
    if (xTaskCreate(ha_poll_task, "ha_poll", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start HA poll task");
        return ESP_ERR_NO_MEM;   // s_started stays false -> ha_get() returns false
    }
    s_started = true;
    ESP_LOGI(TAG, "HA client started (%d entities)", d.count);
    return ESP_OK;
}

bool ha_get(ha_data_t *out) {
    if (!s_started || out == NULL) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mtx);
    return true;
}

int ha_sensor_count(void) {
    return (HA_CFG_COUNT < HA_MAX_SENSORS) ? HA_CFG_COUNT : HA_MAX_SENSORS;
}

bool ha_sensor_meta(int index, ha_sensor_t *out) {
    if (out == NULL || index < 0 || index >= ha_sensor_count()) {
        return false;
    }
    ha_data_t d;
    if (!ha_get(&d)) {
        return false;
    }
    *out = d.sensors[index];
    return true;
}
